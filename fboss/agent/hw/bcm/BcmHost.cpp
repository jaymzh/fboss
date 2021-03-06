/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "BcmHost.h"

#include "fboss/agent/Constants.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/BcmEgress.h"
#include "fboss/agent/hw/bcm/BcmIntf.h"
#include "fboss/agent/hw/bcm/BcmPort.h"
#include "fboss/agent/hw/bcm/BcmWarmBootCache.h"

namespace {
constexpr auto kVrf = "vrf";
constexpr auto kIp = "ip";
constexpr auto kPort = "port";
constexpr auto kNextHops = "nexthops";
constexpr auto kEgress = "egress";
constexpr auto kEgressId = "egressId";
constexpr auto kHosts = "hosts";
}

namespace facebook { namespace fboss {

using std::unique_ptr;
using std::shared_ptr;
using folly::MacAddress;
using folly::IPAddress;

BcmHost::BcmHost(const BcmSwitch* hw, opennsl_vrf_t vrf, const IPAddress& addr,
    opennsl_if_t referenced_egress)
      : hw_(hw), vrf_(vrf), addr_(addr),
      egressId_(referenced_egress) {
  hw_->writableHostTable()->incEgressReference(egressId_);
}

void BcmHost::initHostCommon(opennsl_l3_host_t *host) const {
  opennsl_l3_host_t_init(host);
  if (addr_.isV4()) {
    host->l3a_ip_addr = addr_.asV4().toLongHBO();
  } else {
    memcpy(&host->l3a_ip6_addr, addr_.asV6().toByteArray().data(),
           sizeof(host->l3a_ip6_addr));
    host->l3a_flags |= OPENNSL_L3_IP6;
  }
  host->l3a_vrf = vrf_;
  host->l3a_intf = getEgressId();
}

void BcmHost::addBcmHost(bool isMultipath) {
  if (added_) {
    return;
  }
  opennsl_l3_host_t host;
  initHostCommon(&host);
  if (isMultipath) {
    host.l3a_flags |= OPENNSL_L3_MULTIPATH;
  }
  const auto warmBootCache = hw_->getWarmBootCache();
  auto vrfIp2HostCitr = warmBootCache->findHost(vrf_, addr_);
  if (vrfIp2HostCitr != warmBootCache->vrfAndIP2Host_end()) {
    // Lambda to compare if hosts are equivalent
    auto equivalent =
      [=] (const opennsl_l3_host_t& newHost,
          const opennsl_l3_host_t& existingHost) {
      // Compare the flags we care about, I have seen garbage
      // values set on actual non flag bits when reading entries
      // back on warm boot.
      bool flagsEqual = ((existingHost.l3a_flags & OPENNSL_L3_IP6) ==
          (newHost.l3a_flags & OPENNSL_L3_IP6) &&
          (existingHost.l3a_flags & OPENNSL_L3_MULTIPATH) ==
          (newHost.l3a_flags & OPENNSL_L3_MULTIPATH));
      return flagsEqual && existingHost.l3a_vrf == newHost.l3a_vrf &&
        existingHost.l3a_intf == newHost.l3a_intf;
    };
    if (!equivalent(host, vrfIp2HostCitr->second)) {
      LOG (FATAL) << "Host entries should never change ";
    } else {
      VLOG(1) << "Host entry for : " << addr_ << " already exists";
    }
    warmBootCache->programmed(vrfIp2HostCitr);
  } else {
    VLOG(3) << "Adding host entry for : " << addr_;
    auto rc = opennsl_l3_host_add(hw_->getUnit(), &host);
    bcmCheckError(rc, "failed to program L3 host object for ", addr_.str(),
      " @egress ", getEgressId());
    VLOG(3) << "created L3 host object for " << addr_.str()
    << " @egress " << getEgressId();

  }
  added_ = true;
}

void BcmHost::program(opennsl_if_t intf, const MacAddress* mac,
                      opennsl_port_t port, RouteForwardAction action) {
  unique_ptr<BcmEgress> createdEgress{nullptr};
  BcmEgress* egress{nullptr};
  // get the egress object and then update it with the new MAC
  if (egressId_ == BcmEgressBase::INVALID) {
    createdEgress = folly::make_unique<BcmEgress>(hw_);
    egress = createdEgress.get();
  } else {
    egress = dynamic_cast<BcmEgress*>(
        hw_->writableHostTable()->getEgressObjectIf(egressId_));
  }
  CHECK(egress);
  if (mac) {
    egress->program(intf, vrf_, addr_, *mac, port);
  } else {
    if (action == DROP) {
      egress->programToDrop(intf, vrf_, addr_);
    } else {
      egress->programToCPU(intf, vrf_, addr_);
    }
  }
  if (createdEgress) {
    egressId_ = createdEgress->getID();
    hw_->writableHostTable()->insertBcmEgress(std::move(createdEgress));
  }

  // if no host was added already, add one pointing to the egress object
  if (!added_) {
    addBcmHost();
  }
  auto oldPort = port_;
  port_ = port;
  VLOG(1) << "Updated port for : " << egress->getID() << " from " << oldPort
    << " to " << port;
  // Update port mapping, for entries marked to DROP or to CPU port gets
  // set to 0, which implies no ports are associated with this entry now.
  hw_->writableHostTable()->updatePortEgressMapping(egress->getID(),
      oldPort, port_);
}


BcmHost::~BcmHost() {
  if (!added_) {
    return;
  }
  opennsl_l3_host_t host;
  initHostCommon(&host);
  auto rc = opennsl_l3_host_delete(hw_->getUnit(), &host);
  bcmLogFatal(rc, hw_, "failed to delete L3 host object for ", addr_.str());
  VLOG(3) << "deleted L3 host object for " << addr_.str();
  // This host mapping just went away, update the port -> egress id mapping
  hw_->writableHostTable()->updatePortEgressMapping(egressId_, port_, 0);
  hw_->writableHostTable()->derefEgress(egressId_);
}

BcmEcmpHost::BcmEcmpHost(const BcmSwitch *hw, opennsl_vrf_t vrf,
                         const RouteForwardNexthops& fwd)
    : hw_(hw), vrf_(vrf) {
  CHECK_GT(fwd.size(), 0);
  BcmHostTable *table = hw_->writableHostTable();
  BcmEcmpEgress::Paths paths;
  RouteForwardNexthops prog;
  prog.reserve(fwd.size());
  SCOPE_FAIL {
    for (const auto& nhop : prog) {
      table->derefBcmHost(vrf, nhop.nexthop);
    }
  };
  // allocate a BcmHost object for each path in this ECMP
  int total = 0;
  for (const auto& nhop : fwd) {
    auto host = table->incRefOrCreateBcmHost(vrf, nhop.nexthop);
    auto ret = prog.emplace(nhop.intf, nhop.nexthop);
    CHECK(ret.second);
    // TODO:
    // Ideally, we should have the nexthop resolved already and programmed in
    // HW. If not, SW can preemptively trigger neighbor discovery and then
    // do the HW programming. For now, we program the egress object to punt
    // to CPU. Any traffic going to CPU will trigger the neighbor discovery.
    if (!host->isProgrammed()) {
      const auto intf = hw->getIntfTable()->getBcmIntf(nhop.intf);
      host->programToCPU(intf->getBcmIfId());
    }
    paths.insert(host->getEgressId());
  }
  if (paths.size() == 1) {
    // just one path. No BcmEcmpEgress object this case.
    egressId_ = *paths.begin();
  } else {
    auto ecmp = folly::make_unique<BcmEcmpEgress>(hw, std::move(paths));
    //ecmp->program(paths, fwd.size());
    egressId_ = ecmp->getID();
    ecmpEgressId_ = egressId_;
    hw_->writableHostTable()->insertBcmEgress(std::move(ecmp));
  }
  fwd_ = std::move(prog);
}

BcmEcmpHost::~BcmEcmpHost() {
  // Deref ECMP egress first since the ECMP egress entry holds references
  // to egress entries.
  VLOG(3) << "Decremented reference for egress object for " << fwd_;
  hw_->writableHostTable()->derefEgress(ecmpEgressId_);
  BcmHostTable *table = hw_->writableHostTable();
  for (const auto& nhop : fwd_) {
    table->derefBcmHost(vrf_, nhop.nexthop);
  }
}

BcmHostTable::BcmHostTable(const BcmSwitch *hw) : hw_(hw) {
  auto port2EgressIds = std::make_shared<PortAndEgressIdsMap>();
  port2EgressIds->publish();
  setPort2EgressIdsInternal(port2EgressIds);
}

BcmHostTable::~BcmHostTable() {
}

template<typename KeyT, typename HostT, typename... Args>
HostT* BcmHostTable::incRefOrCreateBcmHost(
    HostMap<KeyT, HostT>* map, const KeyT& key, Args... args) {
  auto ret = map->emplace(key, std::make_pair(nullptr, 1));
  auto& iter = ret.first;
  if (!ret.second) {
    // there was an entry already there
    iter->second.second++;  // increase the reference counter
    return iter->second.first.get();
  }
  SCOPE_FAIL {
    map->erase(iter);
  };
  auto newHost = folly::make_unique<HostT>(hw_, key.first, key.second, args...);
  auto hostPtr = newHost.get();
  iter->second.first = std::move(newHost);
  return hostPtr;
}

BcmHost* BcmHostTable::incRefOrCreateBcmHost(
    opennsl_vrf_t vrf, const IPAddress& addr) {
  return incRefOrCreateBcmHost(&hosts_, std::make_pair(vrf, addr));
}

BcmHost* BcmHostTable::incRefOrCreateBcmHost(
    opennsl_vrf_t vrf, const IPAddress& addr, opennsl_if_t egressId) {
  return incRefOrCreateBcmHost(&hosts_, std::make_pair(vrf, addr), egressId);
}

BcmEcmpHost* BcmHostTable::incRefOrCreateBcmEcmpHost(
    opennsl_vrf_t vrf, const RouteForwardNexthops& fwd) {
  return incRefOrCreateBcmHost(&ecmpHosts_, std::make_pair(vrf, fwd));
}

template<typename KeyT, typename HostT, typename... Args>
HostT* BcmHostTable::getBcmHostIf(const HostMap<KeyT, HostT>* map,
                                  Args... args) const {
  KeyT key{args...};
  auto iter = map->find(key);
  if (iter == map->end()) {
    return nullptr;
  }
  return iter->second.first.get();
}

BcmHost* BcmHostTable::getBcmHostIf(opennsl_vrf_t vrf,
                                    const IPAddress& addr) const {
  return getBcmHostIf(&hosts_, vrf, addr);
}

BcmHost* BcmHostTable::getBcmHost(
    opennsl_vrf_t vrf, const IPAddress& addr) const {
  auto host = getBcmHostIf(vrf, addr);
  if (!host) {
    throw FbossError("Cannot find BcmHost vrf=", vrf, " addr=", addr);
  }
  return host;
}

BcmEcmpHost* BcmHostTable::getBcmEcmpHostIf(
    opennsl_vrf_t vrf, const RouteForwardNexthops& fwd) const {
  return getBcmHostIf(&ecmpHosts_, vrf, fwd);
}

BcmEcmpHost* BcmHostTable::getBcmEcmpHost(
    opennsl_vrf_t vrf, const RouteForwardNexthops& fwd) const {
  auto host = getBcmEcmpHostIf(vrf, fwd);
  if (!host) {
    throw FbossError("Cannot find BcmEcmpHost vrf=", vrf, " fwd=", fwd);
  }
  return host;
}

template<typename KeyT, typename HostT, typename... Args>
HostT* BcmHostTable::derefBcmHost(HostMap<KeyT, HostT>* map,
                                  Args... args) noexcept {
  KeyT key{args...};
  auto iter = map->find(key);
  if (iter == map->end()) {
    return nullptr;
  }
  auto& entry = iter->second;
  CHECK_GT(entry.second, 0);
  if (--entry.second == 0) {
    map->erase(iter);
    return nullptr;
  }
  return entry.first.get();
}

BcmHost* BcmHostTable::derefBcmHost(
    opennsl_vrf_t vrf, const IPAddress& addr) noexcept {
  return derefBcmHost(&hosts_, vrf, addr);
}

BcmEcmpHost* BcmHostTable::derefBcmEcmpHost(
    opennsl_vrf_t vrf, const RouteForwardNexthops& fwd) noexcept {
  return derefBcmHost(&ecmpHosts_, vrf, fwd);
}

BcmEgressBase* BcmHostTable::incEgressReference(opennsl_if_t egressId) {
  if (egressId == BcmEcmpEgress::INVALID ||
      egressId == hw_->getDropEgressId()) {
    return nullptr;
  }
  auto it = egressMap_.find(egressId);
  CHECK(it != egressMap_.end());
  it->second.second++;
  return it->second.first.get();
}

BcmEgressBase* BcmHostTable::derefEgress(opennsl_if_t egressId) {
  if (egressId == BcmEcmpEgress::INVALID ||
      egressId == hw_->getDropEgressId()) {
    return nullptr;
  }
  auto it = egressMap_.find(egressId);
  CHECK(it != egressMap_.end());
  CHECK_GT(it->second.second, 0);
  if (--it->second.second == 0) {
    egressMap_.erase(egressId);
    return nullptr;
  }
  return it->second.first.get();
}

opennsl_port_t BcmHostTable::egressIdPort(opennsl_if_t egressId) const {
  auto itr = egressId2Port_.find(egressId);
  return itr == egressId2Port_.end() ? 0 : itr->second;
}

void BcmHostTable::updatePortEgressMapping(opennsl_if_t egressId,
    opennsl_port_t oldPort, opennsl_port_t newPort) {
  auto newMapping = getPortAndEgressIdsMap()->clone();

  if (oldPort) {
    egressId2Port_.erase(egressId);
    auto old = newMapping->getPortAndEgressIdsIf(oldPort);
    CHECK(old);
    auto oldCloned = old->clone();
    oldCloned->removeEgressId(egressId);
    if (oldCloned->empty()) {
      newMapping->removePort(oldPort);
    } else {
      newMapping->updatePortAndEgressIds(oldCloned);
    }
  }
  if (newPort) {
    egressId2Port_[egressId] = newPort;
    auto existing = newMapping->getPortAndEgressIdsIf(newPort);
    if (existing) {
      auto existingCloned = existing->clone();
      existingCloned->addEgressId(egressId);
      newMapping->updatePortAndEgressIds(existingCloned);
    } else {
      PortAndEgressIdsFields::EgressIds egressIds;
      egressIds.insert(egressId);
      auto toAdd = std::make_shared<PortAndEgressIds>(newPort, egressIds);
      newMapping->addPortAndEgressIds(toAdd);
    }
  }
  // Publish and replace with the updated mapping
  newMapping->publish();
  setPort2EgressIdsInternal(newMapping);
  bool cameUp = !oldPort && newPort;
  bool wentDown = oldPort && !newPort;
  if (cameUp || wentDown) {
    // If ARP/NDP just resolved for this host, we need to inform
    // ecmp egress objects about this egress Id becoming reachable.
    // Consider the case where a port went down, neighbor entry expires
    // and then the port came back up. When the neighbor entry expired,
    // we would have taken it out of the port->egressId mapping. Now even
    // when the port comes back up, we won't have that egress Id mapping
    // there and won't signal ecmp objects to add this back. So when
    // a egress object gets resolved, for all the ecmp objects that
    // have this egress Id, ask them to add it back if they don't
    // already have this egress Id. We do a checked add because if
    // the neighbor entry just expired w/o the port going down we
    // would have never removed it from ecmp egress object.
    BcmEcmpEgress::Paths affectedPaths;
    affectedPaths.insert(egressId);
    egressResolutionChangedMaybeLocked(affectedPaths, cameUp ? true /*up*/ :
        false /*went away*/, true /* hw locked*/);
  }
}

void BcmHostTable::setPort2EgressIdsInternal(
    std::shared_ptr<PortAndEgressIdsMap> newMap) {
  // This is one of the only two places that should ever directly access
  // portAndEgressIdsDontUseDirectly_.  (getPortAndEgressIdsMap() being the
  // other one.)
  CHECK(newMap->isPublished());
  folly::SpinLockGuard guard(portAndEgressIdsLock_);
  portAndEgressIdsDontUseDirectly_.swap(newMap);
}

const BcmEgressBase* BcmHostTable::getEgressObjectIf(opennsl_if_t egress) const {
  auto it = egressMap_.find(egress);
  return it == egressMap_.end() ? nullptr : it->second.first.get();
}

BcmEgressBase* BcmHostTable::getEgressObjectIf(opennsl_if_t egress) {
  return const_cast<BcmEgressBase*>(
      const_cast<const BcmHostTable*>(this)->getEgressObjectIf(egress));
}

void BcmHostTable::insertBcmEgress(
    std::unique_ptr<BcmEgressBase> egress) {
  auto id = egress->getID();
  auto ret = egressMap_.emplace(id, std::make_pair(std::move(egress), 1));
  CHECK(ret.second);
}

void BcmHostTable::warmBootHostEntriesSynced() {
  opennsl_port_config_t pcfg;
  auto rv = opennsl_port_config_get(hw_->getUnit(), &pcfg);
  bcmCheckError(rv, "failed to get port configuration");
  // Ideally we should call this only for ports which were
  // down when we went down, but since we don't record that
  // signal up for all up ports.
  VLOG(1) << "Warm boot host entries synced, signalling link "
    "up for all up ports";
  opennsl_port_t idx;
  OPENNSL_PBMP_ITER(pcfg.port, idx) {
    // Some ports might have come up or gone down during
    // the time controller was down. So call linkUp/DownHwLocked
    // for these. We could track this better by just calling
    // linkUp/DownHwLocked only for ports that actually changed
    // state, but thats a minor optimization.
    if (hw_->isPortUp(PortID(idx))) {
      linkUpHwLocked(idx);
    } else {
      linkDownHwLocked(idx);
    }
  }
}

folly::dynamic BcmHost::toFollyDynamic() const {
  folly::dynamic host = folly::dynamic::object;
  host[kVrf] = vrf_;
  host[kIp] = addr_.str();
  host[kPort] = port_;
  host[kEgressId] = egressId_;
  if (egressId_ != BcmEgressBase::INVALID &&
      egressId_ != hw_->getDropEgressId()) {
    host[kEgress] = hw_->getHostTable()
      ->getEgressObjectIf(egressId_)->toFollyDynamic();
  }
  return host;
}

folly::dynamic BcmEcmpHost::toFollyDynamic() const {
  folly::dynamic ecmpHost = folly::dynamic::object;
  ecmpHost[kVrf] = vrf_;
  std::vector<folly::dynamic> nhops;
  for (auto& nhop: fwd_) {
    nhops.emplace_back(nhop.toFollyDynamic());
  }
  ecmpHost[kNextHops] = std::move(nhops);
  ecmpHost[kEgressId] = egressId_;
  ecmpHost[kEcmpEgressId] = ecmpEgressId_;
  if (ecmpEgressId_ != BcmEgressBase::INVALID) {
    ecmpHost[kEcmpEgress] = hw_->getHostTable()
      ->getEgressObjectIf(ecmpEgressId_)->toFollyDynamic();
  }
  return ecmpHost;
}

folly::dynamic BcmHostTable::toFollyDynamic() const {
  std::vector<folly::dynamic> hostsJson;
  for (const auto& vrfIpAndHost: hosts_) {
    hostsJson.emplace_back(vrfIpAndHost.second.first->toFollyDynamic());
  }
  std::vector<folly::dynamic> ecmpHostsJson;
  for (const auto& vrfNhopsAndHost: ecmpHosts_) {
    ecmpHostsJson.emplace_back(vrfNhopsAndHost.second.first->toFollyDynamic());
  }
  folly::dynamic hostTable = folly::dynamic::object;
  hostTable[kHosts] = std::move(hostsJson);
  hostTable[kEcmpHosts] = std::move(ecmpHostsJson);
  return hostTable;
}

void BcmHostTable::linkStateChangedMaybeLocked(opennsl_port_t port, bool up,
    bool locked) {
  auto portAndEgressIdMapping = getPortAndEgressIdsMap();
  const auto portAndEgressIds = portAndEgressIdMapping->getPortAndEgressIdsIf(
      port);
  if (!portAndEgressIds) {
    return;
  }
  egressResolutionChangedMaybeLocked(portAndEgressIds->getEgressIds(), up,
      locked);
}

void BcmHostTable::egressResolutionChangedMaybeLocked(
    const Paths& affectedPaths, bool up, bool locked) {
  for (const auto& nextHopsAndEcmpHostInfo : ecmpHosts_) {
    auto ecmpId = nextHopsAndEcmpHostInfo.second.first->getEcmpEgressId();
    if (ecmpId == BcmEgressBase::INVALID) {
      continue;
    }
    auto ecmpEgress = static_cast<BcmEcmpEgress*>(getEgressObjectIf(ecmpId));
    // Must find the egress object, we could have done a slower
    // dynamic cast check to ensure that this is the right type
    // our map should be pointing to valid Ecmp egress object for
    // a ecmp egress Id anyways
    CHECK(ecmpEgress);
    for (auto path: affectedPaths) {
      if (up) {
        CHECK(locked);
        ecmpEgress->pathReachableHwLocked(path);
      } else {
        auto updated = locked ? ecmpEgress->pathUnreachableHwLocked(path) :
          ecmpEgress->pathUnreachableHwNotLocked(path);
      }
    }
  }
  /*
   * We may not have done a FIB sync before ports start coming
   * up or ARP/NDP start getting resolved/unresolved. In this case
   * we won't have BcmEcmpHost entries, so we
   * look through the warm boot cache for ecmp egress entries.
   * Conversely post a FIB sync we won't have any ecmp egress IDs
   * in the warm boot cache
   */
  for (const auto& ecmpAndEgressIds:
      hw_->getWarmBootCache()->ecmp2EgressIds()) {
    for (auto path: affectedPaths) {
      if (up) {
        CHECK(locked);
        BcmEcmpEgress::addEgressIdHwLocked(hw_->getUnit(),
            ecmpAndEgressIds.first, ecmpAndEgressIds.second, path);
      } else {
         if (locked) {
           BcmEcmpEgress::removeEgressIdHwLocked(hw_->getUnit(),
               ecmpAndEgressIds.first, ecmpAndEgressIds.second, path);
         } else {
           BcmEcmpEgress::removeEgressIdHwNotLocked(hw_->getUnit(),
               ecmpAndEgressIds.first, ecmpAndEgressIds.second, path);
         }
      }
    }
  }
}

}}
