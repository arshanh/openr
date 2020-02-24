/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqThrottle.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/futures/Future.h>

#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/PrefixManager_types.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/messaging/Queue.h>

namespace openr {

class PrefixManager final : public OpenrEventBase {
 public:
  PrefixManager(
      const std::string& nodeId,
      messaging::RQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue,
      PersistentStore* configStore,
      const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
      const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
      const MonitorSubmitUrl& monitorSubmitUrl,
      const PrefixDbMarker& prefixDbMarker,
      bool createIpPrefix,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      const std::chrono::seconds prefixHoldTime,
      const std::chrono::milliseconds ttlKeyInKvStore,
      fbzmq::Context& zmqContext,
      const std::unordered_set<std::string>& area = {
          openr::thrift::KvStore_constants::kDefaultArea()});

  // disable copying
  PrefixManager(PrefixManager const&) = delete;
  PrefixManager& operator=(PrefixManager const&) = delete;

  void stop() override;

  /*
   * Public API for PrefixManager operations, including:
   *  - add prefixes
   *  - withdraw prefixes
   *  - withdraw prefixes by type
   *  - sync prefixes by type
   *  - dump all prefixes
   *
   * Returns true if there are changes else false
   */
  folly::SemiFuture<bool> advertisePrefixes(
      std::vector<thrift::PrefixEntry> prefixes);

  folly::SemiFuture<bool> withdrawPrefixes(
      std::vector<thrift::PrefixEntry> prefixes);

  folly::SemiFuture<bool> withdrawPrefixesByType(thrift::PrefixType prefixType);

  folly::SemiFuture<bool> syncPrefixesByType(
      thrift::PrefixType prefixType, std::vector<thrift::PrefixEntry> prefixes);

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
  getPrefixes();

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
  getPrefixesByType(thrift::PrefixType prefixType);

 private:
  void outputState();
  // Update persistent store with non-ephemeral prefix entries
  void persistPrefixDb();

  // Update kvstore with both ephemeral and non-ephemeral prefixes
  void updateKvStore();

  // update all IP keys in KvStore
  void updateKvStorePrefixKeys();

  // helpers to modify prefix db, returns true if the db is modified
  bool addOrUpdatePrefixes(const std::vector<thrift::PrefixEntry>& prefixes);
  bool removePrefixes(const std::vector<thrift::PrefixEntry>& prefixes);
  bool removePrefixesByType(thrift::PrefixType type);
  // replace all prefixes of @type w/ @prefixes
  bool syncPrefixes(
      thrift::PrefixType type,
      const std::vector<thrift::PrefixEntry>& prefixes);

  // Submit internal state counters to monitor
  void submitCounters();

  // add prefix entry in kvstore, return per prefix key name
  std::string advertisePrefix(thrift::PrefixEntry& prefixEntry);

  // add event named updateEvent to perfEvents if it has value and the last
  // element is not already updateEvent
  void maybeAddEvent(
      thrift::PerfEvents& perfEvents, std::string const& updateEvent);

  // this node name
  const std::string nodeId_;

  // client to interact with ConfigStore
  PersistentStore* configStore_{nullptr};

  // keep track of prefixDB on disk
  thrift::PrefixDatabase diskState_;

  const PrefixDbMarker prefixDbMarker_;

  // create IP keys
  bool perPrefixKeys_{false};

  // enable convergence performance measurement for Adjacencies update
  const bool enablePerfMeasurement_{false};

  // Throttled version of updateKvStore. It batches up multiple calls and
  // send them in one go!
  std::unique_ptr<fbzmq::ZmqThrottle> outputStateThrottled_;

  std::unique_ptr<fbzmq::ZmqTimeout> initialOutputStateTimer_;

  // TTL for a key in the key value store
  const std::chrono::milliseconds ttlKeyInKvStore_;

  // kvStoreClient for persisting our prefix db
  KvStoreClient kvStoreClient_;

  // The current prefix db this node is advertising. In-case if multiple entries
  // exists for a given prefix, lowest prefix-type is preferred. This is to
  // bring deterministic behavior for advertising routes.
  // IMP: Ordered
  std::map<
      thrift::PrefixType,
      std::unordered_map<thrift::IpPrefix, thrift::PrefixEntry>>
      prefixMap_;

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // track any prefix keys for this node that we see to make sure we withdraw
  // anything we no longer wish to advertise
  std::unordered_set<std::string> keysToClear_;

  // perfEvents related to a given prefisEntry
  std::unordered_map<
      thrift::PrefixType,
      std::unordered_map<thrift::IpPrefix, thrift::PerfEvents>>
      addingEvents_;

  // area Id
  const std::unordered_set<std::string> areas_{};

  // Future for fiber
  folly::Future<folly::Unit> prefixUpdatesTaskFuture_;
}; // PrefixManager

} // namespace openr
