/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "BcmRoute.h"

extern "C" {
#include <opennsl/l3.h>
}

#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include "fboss/agent/state/Route.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/BcmHost.h"
#include "fboss/agent/hw/bcm/BcmIntf.h"
#include "fboss/agent/hw/bcm/BcmPlatform.h"
#include "fboss/agent/hw/bcm/BcmWarmBootCache.h"

namespace facebook { namespace fboss {

BcmRoute::BcmRoute(const BcmSwitch* hw, opennsl_vrf_t vrf,
                   const folly::IPAddress& addr, uint8_t len)
    : hw_(hw), vrf_(vrf), prefix_(addr), len_(len) {
}

void BcmRoute::initL3RouteT(opennsl_l3_route_t* rt) const {
  opennsl_l3_route_t_init(rt);
  rt->l3a_vrf = vrf_;
  if (prefix_.isV4()) {
    // both l3a_subnet and l3a_ip_mask for IPv4 are in host order
    rt->l3a_subnet = prefix_.asV4().toLongHBO();
    rt->l3a_ip_mask = folly::IPAddressV4(
        folly::IPAddressV4::fetchMask(len_)).toLongHBO();
  } else {
    memcpy(&rt->l3a_ip6_net, prefix_.asV6().toByteArray().data(),
           sizeof(rt->l3a_ip6_net));
    memcpy(&rt->l3a_ip6_mask, folly::IPAddressV6::fetchMask(len_).data(),
           sizeof(rt->l3a_ip6_mask));
    rt->l3a_flags |= OPENNSL_L3_IP6;
  }
}

bool BcmRoute::isHostRoute() const {
  return prefix_.isV6() ? len_ == 128 : len_ == 32;
}

bool BcmRoute::canUseHostTable() const {
  return isHostRoute() && hw_->getPlatform()->canUseHostTableForHostRoutes();
}

void BcmRoute::program(const RouteForwardInfo& fwd) {

  // if the route has been programmed to the HW, check if the forward info is
  // changed or not. If not, nothing to do.
  if (added_ && fwd == fwd_) {
    return;
  }

  // function to clean up the host reference
  auto cleanupHost = [&] (const RouteForwardNexthops& nhopsClean) noexcept {
    if (nhopsClean.size()) {
      hw_->writableHostTable()->derefBcmEcmpHost(vrf_, nhopsClean);
    }
  };

  auto action = fwd.getAction();
  // find out the egress object ID
  opennsl_if_t egressId;
  if (action == RouteForwardAction::DROP) {
    egressId = hw_->getDropEgressId();
  } else if (action == RouteForwardAction::TO_CPU) {
    egressId = hw_->getToCPUEgressId();
  } else {
    CHECK(action == RouteForwardAction::NEXTHOPS);
    // need to get an entry from the host table for the forward info
    const RouteForwardNexthops& nhops = fwd.getNexthops();
    CHECK_GT(nhops.size(), 0);
    auto host = hw_->writableHostTable()->incRefOrCreateBcmEcmpHost(
        vrf_, nhops);
    egressId = host->getEgressId();
  }

  // At this point host and egress objects for next hops have been
  // created, what remains to be done is to program route into the
  // route table or host table (if this is a host route and use of
  // host table for host routes is allowed by the chip).
  SCOPE_FAIL {
    cleanupHost(fwd.getNexthops());
  };
  if (canUseHostTable()) {
    if (added_) {
      auto host = hw_->getHostTable()->getBcmHostIf(vrf_, prefix_);
      CHECK(host);
      VLOG(3) <<" Derefrencing host prefix for : " << prefix_
        <<" /" << len_ <<" host egress Id : " << host->getEgressId();
      hw_->writableHostTable()->derefBcmHost(vrf_, prefix_);
    }
    programHostRoute(egressId, fwd);
  } else {
    programLpmRoute(egressId, fwd);
  }
  if (added_) {
    // the route was added before, need to free the old nexthop(s)
    cleanupHost(fwd_.getNexthops());
  }
  fwd_ = fwd;
  // new nexthop has been stored in fwd_. From now on, it is up to
  // ~BcmRoute() to clean up such nexthop.
  added_ = true;
}

void BcmRoute::programHostRoute(opennsl_if_t egressId,
    const RouteForwardInfo& fwd) {
  auto hostRouteHost = hw_->writableHostTable()->incRefOrCreateBcmHost(
      vrf_, prefix_, egressId);
  auto cleanupHostRoute = [=]() noexcept {
    hw_->writableHostTable()->derefBcmHost(vrf_, prefix_);
  };
  SCOPE_FAIL {
    cleanupHostRoute();
  };
  hostRouteHost->addBcmHost(fwd.getNexthops().size() > 1);
}

void BcmRoute::programLpmRoute(opennsl_if_t egressId,
    const RouteForwardInfo& fwd) {
  opennsl_l3_route_t rt;
  initL3RouteT(&rt);
  rt.l3a_intf = egressId;
  if (fwd.getNexthops().size() > 1) {         // multipath
    rt.l3a_flags |= OPENNSL_L3_MULTIPATH;
  }

  bool addRoute = false;
  const auto warmBootCache = hw_->getWarmBootCache();
  auto vrfAndPfx2RouteCitr = warmBootCache->findRoute(vrf_, prefix_, len_);
  if (vrfAndPfx2RouteCitr != warmBootCache->vrfAndPrefix2Route_end()) {
    // Lambda to compare if the routes are equivalent and thus we need to
    // do nothing
    auto equivalent =
      [=] (const opennsl_l3_route_t& newRoute,
           const opennsl_l3_route_t& existingRoute) {
      // Compare flags (primarily MULTIPATH vs non MULTIPATH
      // and egress id.
      return existingRoute.l3a_flags == newRoute.l3a_flags &&
      existingRoute.l3a_intf == newRoute.l3a_intf;
    };
    if (!equivalent(rt, vrfAndPfx2RouteCitr->second)) {
      VLOG (3) << "Updating route for : " << prefix_ << "/"
        << static_cast<int>(len_) << " in vrf : " << vrf_;
      // This is a change
      rt.l3a_flags |= OPENNSL_L3_REPLACE;
      addRoute = true;
    } else {
      VLOG(3) << " Route for : " << prefix_ << "/" << static_cast<int>(len_)
        << " in vrf : " << vrf_ << " already exists";
    }
  } else {
    addRoute = true;
  }
  if (addRoute) {
    if (vrfAndPfx2RouteCitr == warmBootCache->vrfAndPrefix2Route_end()) {
      VLOG (3) << "Adding route for : " << prefix_ << "/"
        << static_cast<int>(len_) << " in vrf : " << vrf_;
    }
    if (added_) {
      rt.l3a_flags |= OPENNSL_L3_REPLACE;
    }
    auto rc = opennsl_l3_route_add(hw_->getUnit(), &rt);
    bcmCheckError(rc, "failed to create a route entry for ", prefix_, "/",
        static_cast<int>(len_), " @ ", fwd, " @egress ", egressId);
    VLOG(3) << "created a route entry for " << prefix_.str() << "/"
      << static_cast<int>(len_) << " @egress " << egressId
      << " with " << fwd;
  }
  if (vrfAndPfx2RouteCitr != warmBootCache->vrfAndPrefix2Route_end()) {
    warmBootCache->programmed(vrfAndPfx2RouteCitr);
  }
}

BcmRoute::~BcmRoute() {
  if (!added_) {
    return;
  }
  if (canUseHostTable()) {
    auto host = hw_->getHostTable()->getBcmHostIf(vrf_, prefix_);
    CHECK(host);
    VLOG(3) <<" Derefrencing host prefix for : " << prefix_
      <<" /" << len_ << " host: " << host;
    hw_->writableHostTable()->derefBcmHost(vrf_, prefix_);
  } else {
    opennsl_l3_route_t rt;
    opennsl_l3_route_t_init(&rt);
    initL3RouteT(&rt);
    auto rc = opennsl_l3_route_delete(hw_->getUnit(), &rt);
    if (OPENNSL_FAILURE(rc)) {
      LOG(ERROR) << "Failed to delete a route entry for " << prefix_ << "/"
                 << static_cast<int>(len_) << " Error: " << opennsl_errmsg(rc);
    } else {
      VLOG(3) << "deleted a route entry for " << prefix_.str() << "/"
              << static_cast<int>(len_);
    }
  }
  // decrease reference counter of the host entry for next hops
  const auto& nhops = fwd_.getNexthops();
  if (nhops.size()) {
    hw_->writableHostTable()->derefBcmEcmpHost(vrf_, nhops);
  }
}

bool BcmRouteTable::Key::operator<(const Key& k2) const {
  if (vrf < k2.vrf) {
    return true;
  } else if (vrf > k2.vrf) {
    return false;
  }
  if (mask < k2.mask) {
    return true;
  } else if (mask > k2.mask) {
    return false;
  }
  return network < k2.network;
}

BcmRouteTable::BcmRouteTable(const BcmSwitch* hw) : hw_(hw) {
}

BcmRouteTable::~BcmRouteTable() {
}

BcmRoute* BcmRouteTable::getBcmRouteIf(
    opennsl_vrf_t vrf, const folly::IPAddress& network, uint8_t mask) const {
  Key key{network, mask, vrf};
  auto iter = fib_.find(key);
  if (iter == fib_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

BcmRoute* BcmRouteTable::getBcmRoute(
    opennsl_vrf_t vrf, const folly::IPAddress& network, uint8_t mask) const {
  auto rt = getBcmRouteIf(vrf, network, mask);
  if (!rt) {
    throw FbossError("Cannot find route for ", network,
                     "/", static_cast<uint32_t>(mask), " @ vrf ", vrf);
  }
  return rt;
}

template<typename RouteT>
void BcmRouteTable::addRoute(opennsl_vrf_t vrf, const RouteT *route) {
  const auto& prefix = route->prefix();
  Key key{folly::IPAddress(prefix.network), prefix.mask, vrf};
  auto ret = fib_.emplace(key, nullptr);
  if (ret.second) {
    SCOPE_FAIL {
      fib_.erase(ret.first);
    };
    ret.first->second.reset(new BcmRoute(hw_, vrf,
                                        folly::IPAddress(prefix.network),
                                        prefix.mask));
  }
  ret.first->second->program(route->getForwardInfo());
}

template<typename RouteT>
void BcmRouteTable::deleteRoute(opennsl_vrf_t vrf, const RouteT *route) {
  const auto& prefix = route->prefix();
  Key key{folly::IPAddress(prefix.network), prefix.mask, vrf};
  auto iter = fib_.find(key);
  if (iter == fib_.end()) {
    throw FbossError("Failed to delete a non-existing route ", route->str());
  }
  fib_.erase(iter);
}

template void BcmRouteTable::addRoute(opennsl_vrf_t, const RouteV4 *);
template void BcmRouteTable::addRoute(opennsl_vrf_t, const RouteV6 *);
template void BcmRouteTable::deleteRoute(opennsl_vrf_t, const RouteV4 *);
template void BcmRouteTable::deleteRoute(opennsl_vrf_t, const RouteV6 *);

}}
