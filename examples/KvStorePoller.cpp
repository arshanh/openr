/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStorePoller.h"

#include <openr/common/Constants.h>
#include <openr/kvstore/KvStoreClient.h>

namespace openr {

KvStorePoller::KvStorePoller(std::vector<folly::SocketAddress>& sockAddrs)
    : sockAddrs_(sockAddrs) {}

std::pair<
    folly::Optional<std::unordered_map<std::string, thrift::AdjacencyDatabase>>,
    std::vector<fbzmq::SocketUrl> /* unreached url */>
KvStorePoller::getAdjacencyDatabases(std::chrono::milliseconds pollTimeout) {
  return openr::KvStoreClient::dumpAllWithPrefixMultipleAndParse<
      thrift::AdjacencyDatabase>(
      sockAddrs_,
      Constants::kAdjDbMarker.toString(),
      Constants::kServiceConnTimeout,
      pollTimeout);
}

std::pair<
    folly::Optional<std::unordered_map<std::string, thrift::PrefixDatabase>>,
    std::vector<fbzmq::SocketUrl> /* unreached url */>
KvStorePoller::getPrefixDatabases(std::chrono::milliseconds pollTimeout) {
  return openr::KvStoreClient::dumpAllWithPrefixMultipleAndParse<
      thrift::PrefixDatabase>(
      sockAddrs_,
      Constants::kPrefixDbMarker.toString(),
      Constants::kServiceConnTimeout,
      pollTimeout);
}

} // namespace openr
