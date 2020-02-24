/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <vector>

#include <fbzmq/zmq/Zmq.h>
#include <folly/SocketAddress.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace openr {

class KvStorePoller {
 public:
  /* implicit */ KvStorePoller(std::vector<folly::SocketAddress>& sockAddrs);

  ~KvStorePoller() {}

  std::pair<
      folly::Optional<
          std::unordered_map<std::string, thrift::AdjacencyDatabase>>,
      std::vector<fbzmq::SocketUrl> /* unreached url */>
  getAdjacencyDatabases(std::chrono::milliseconds pollTimeout);

  std::pair<
      folly::Optional<std::unordered_map<std::string, thrift::PrefixDatabase>>,
      std::vector<fbzmq::SocketUrl> /* unreached url */>
  getPrefixDatabases(std::chrono::milliseconds pollTimeout);

 private:
  std::vector<folly::SocketAddress> sockAddrs_;

}; // class KvStorePoller
} // namespace openr
