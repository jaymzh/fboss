/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/agent/types.h"
#include "fboss/agent/ndp/IPv6RouteAdvertiser.h"
#include "fboss/agent/StateObserver.h"

#include <memory>
#include <boost/container/flat_map.hpp>
#include <folly/IPAddressV6.h>
#include <folly/MacAddress.h>
namespace folly { namespace io {
class Cursor;
}}

namespace facebook { namespace fboss {

class IPv6Hdr;
class Interface;
class RxPacket;
class StateDelta;
class SwitchState;
class Vlan;

class IPv6Handler : public AutoRegisterStateObserver {
 public:
  enum : uint16_t { ETHERTYPE_IPV6 = 0x86dd };
  enum : uint32_t { IPV6_MIN_MTU = 1280 };

  explicit IPv6Handler(SwSwitch* sw);

  void stateUpdated(const StateDelta& delta) override;

  void handlePacket(std::unique_ptr<RxPacket> pkt,
                    folly::MacAddress dst,
                    folly::MacAddress src,
                    folly::io::Cursor cursor);

  uint32_t flushNdpEntryBlocking(folly::IPAddressV6, VlanID vlan);
  void floodNeighborAdvertisements();
  void sendNeighborSolicitation(const folly::IPAddressV6& targetIP,
                                const std::shared_ptr<Vlan> vlan);

 private:
  struct ICMPHeaders;
  typedef boost::container::flat_map<InterfaceID, IPv6RouteAdvertiser> RAMap;

  // Forbidden copy constructor and assignment operator
  IPv6Handler(IPv6Handler const &) = delete;
  IPv6Handler& operator=(IPv6Handler const &) = delete;

  bool raEnabled(const Interface* intf) const;
  void intfAdded(const SwitchState* state, const Interface* intf);
  void intfDeleted(const Interface* intf);

  void sendICMPv6TimeExceeded(VlanID srcVlan,
                              folly::MacAddress dst,
                              folly::MacAddress src,
                              IPv6Hdr& v6Hdr,
                              folly::io::Cursor cursor);
  /**
   * Function to handle ICMPv6
   *
   * For now, the controller just handle ND related ICMPv6 packets. For
   * other types, the packet is returned back to the caller through the return
   * value.
   *
   * @return nullptr The packet has been handled by this function
   *         others The function does not handle the packet. It is up to the
   *                caller to decide what to do with the packet (i.e. forward
   *                the packet to host)
   */
  std::unique_ptr<RxPacket> handleICMPv6Packet(std::unique_ptr<RxPacket> pkt,
                                               folly::MacAddress dst,
                                               folly::MacAddress src,
                                               const IPv6Hdr& ipv6,
                                               folly::io::Cursor cursor);
  void handleRouterSolicitation(std::unique_ptr<RxPacket> pkt,
                                const ICMPHeaders& hdr,
                                folly::io::Cursor cursor);
  void handleRouterAdvertisement(std::unique_ptr<RxPacket> pkt,
                                 const ICMPHeaders& hdr,
                                 folly::io::Cursor cursor);
  void handleNeighborSolicitation(std::unique_ptr<RxPacket> pkt,
                                  const ICMPHeaders& hdr,
                                  folly::io::Cursor cursor);
  void handleNeighborAdvertisement(std::unique_ptr<RxPacket> pkt,
                                   const ICMPHeaders& hdr,
                                   folly::io::Cursor cursor);

  bool checkNdpPacket(const ICMPHeaders& hdr,
                      const RxPacket* pkt) const;

  void sendNeighborSolicitations(const folly::IPAddressV6& targetIP);
  void sendNeighborAdvertisement(VlanID vlan,
                                 folly::MacAddress srcMac,
                                 folly::IPAddressV6 srcIP,
                                 folly::MacAddress dstMac,
                                 folly::IPAddressV6 dstIP);
  void updateNeighborEntry(const RxPacket* pkt,
                           folly::IPAddressV6 ip,
                           folly::MacAddress mac,
                           uint32_t flags);
  void setPendingNdpEntry(std::shared_ptr<Vlan> vlan,
                          const folly::IPAddressV6 &ip);

  std::shared_ptr<SwitchState> performNeighborFlush(
      const std::shared_ptr<SwitchState>& state,
      VlanID vlan,
      folly::IPAddressV6 ip,
      uint32_t* countFlushed);
  bool performNeighborFlush(std::shared_ptr<SwitchState>* state,
                            Vlan* vlan,
                            folly::IPAddressV6 ip);

  SwSwitch* sw_{nullptr};
  RAMap routeAdvertisers_;
};

}} // facebook::fboss
