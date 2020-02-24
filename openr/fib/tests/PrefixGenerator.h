/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <syslog.h>
#include <sstream>
#include <string>
#include <utility>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>

#pragma once

namespace openr {
class PrefixGenerator {
 public:
  // Generate random IpPrefixes
  static std::vector<thrift::IpPrefix> ipv6PrefixGenerator(
      const uint32_t numOfPrefixes, const uint8_t bitMaskLen);

  // Generate random nextHops for one prefix
  static std::vector<thrift::NextHopThrift> getRandomNextHopsUnicast(
      const uint8_t numOfNextHops, const std::string& ifname);

 private:
  // Generate random Ipv6
  static folly::IPAddressV6 randIpv6();

  // Generate all nextHops.
  static std::vector<thrift::NextHopThrift> getRandomNextHops(
      const uint8_t numOfNextHops, const std::string& ifname);
};
} // namespace openr
