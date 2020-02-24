/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <fbzmq/async/ZmqThrottle.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <folly/IPAddress.h>
#include <folly/String.h>

#include <openr/common/ExponentialBackoff.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace openr {

/**
 * Holds interface attributes along with complex information like backoffs. All
 * updates made into this object is reflected asynchronously via throttled
 * callback or timers provided in constructor.
 *
 * - Any change will always trigger throttled callback
 * - Interface transition from Active to Inactive schedules immediate timeout
 *   for fast reactions to down events.
 */
class InterfaceEntry final {
 public:
  InterfaceEntry(
      std::string const& ifName,
      std::chrono::milliseconds const& initBackoff,
      std::chrono::milliseconds const& maxBackoff,
      fbzmq::ZmqThrottle& updateCallback,
      fbzmq::ZmqTimeout& updateTimeout);

  // Update attributes
  bool updateAttrs(int ifIndex, bool isUp, uint64_t weight);

  // Update addresses
  bool updateAddr(folly::CIDRNetwork const& ipNetwork, bool isValid);

  // Is interface active. Interface is active only when it is in UP state and
  // it's not backed off
  bool isActive();

  // Get backoff time
  std::chrono::milliseconds getBackoffDuration() const;

  // Used to check for updates if doing a re-sync
  bool
  operator==(const InterfaceEntry& interfaceEntry) {
    return (
        (ifName_ == interfaceEntry.getIfName()) &&
        (ifIndex_ == interfaceEntry.getIfIndex()) &&
        (isUp_ == interfaceEntry.isUp()) &&
        (networks_ == interfaceEntry.getNetworks()) &&
        (weight_ == interfaceEntry.getWeight()));
  }

  friend std::ostream&
  operator<<(std::ostream& out, const InterfaceEntry& interfaceEntry) {
    out << "Interface data: " << (interfaceEntry.isUp() ? "UP" : "DOWN")
        << " ifIndex: " << interfaceEntry.getIfIndex()
        << " weight: " << interfaceEntry.getWeight() << " IPv6ll: "
        << folly::join(", ", interfaceEntry.getV6LinkLocalAddrs())
        << " IPv4: " << folly::join(", ", interfaceEntry.getV4Addrs());
    return out;
  }

  std::string
  getIfName() const {
    return ifName_;
  }

  int
  getIfIndex() const {
    return ifIndex_;
  }

  bool
  isUp() const {
    return isUp_;
  }

  uint64_t
  getWeight() const {
    return weight_;
  }

  // returns const references for optimization
  const std::unordered_set<folly::CIDRNetwork>&
  getNetworks() const {
    return networks_;
  }

  // Utility function to retrieve v4 addresses
  std::unordered_set<folly::IPAddress> getV4Addrs() const;

  // Utility function to retrieve v6 link local addresses
  std::unordered_set<folly::IPAddress> getV6LinkLocalAddrs() const;

  // Utility function to retrieve re-distribute addresses
  std::vector<thrift::PrefixEntry> getGlobalUnicastNetworks(
      bool enableV4) const;

  // Create the Interface info for Interface request
  thrift::InterfaceInfo getInterfaceInfo() const;

 private:
  // Attributes
  std::string const ifName_;
  int ifIndex_{0};
  bool isUp_{false};
  uint64_t weight_{1};
  std::unordered_set<folly::CIDRNetwork> networks_;

  // Backoff variables
  ExponentialBackoff<std::chrono::milliseconds> backoff_;

  // Update callback
  fbzmq::ZmqThrottle& updateCallback_;
  fbzmq::ZmqTimeout& updateTimeout_;
};

} // namespace openr
