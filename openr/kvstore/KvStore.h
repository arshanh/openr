/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include <boost/heap/priority_queue.hpp>
#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Optional.h>
#include <folly/TokenBucket.h>
#include <folly/futures/Future.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncTimeout.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/dual/Dual.h>
#include <openr/if/gen-cpp2/Dual_types.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

struct TtlCountdownQueueEntry {
  std::chrono::steady_clock::time_point expiryTime;
  std::string key;
  int64_t version{0};
  int64_t ttlVersion{0};
  std::string originatorId;
  bool
  operator>(TtlCountdownQueueEntry other) const {
    return expiryTime > other.expiryTime;
  }
};

using TtlCountdownQueue = boost::heap::priority_queue<
    TtlCountdownQueueEntry,
    // Always returns smallest first
    boost::heap::compare<std::greater<TtlCountdownQueueEntry>>,
    boost::heap::stable<true>>;

// Kvstore flooding rate <messages/sec, burst size>
using KvStoreFloodRate = std::optional<std::pair<const size_t, const size_t>>;

class KvStoreFilters {
 public:
  // takes the list of comma separated key prefixes to match,
  // and the list of originator IDs to match in the value
  explicit KvStoreFilters(
      std::vector<std::string> const& keyPrefix,
      std::set<std::string> const& originatorIds);

  // Check if key matches the filters
  bool keyMatch(std::string const& key, thrift::Value const& value) const;

  // return comma separeated string prefix
  std::vector<std::string> getKeyPrefixes() const;

  // return set of origninator IDs
  std::set<std::string> getOrigniatorIdList() const;

  // print filters
  std::string str() const;

 private:
  // list of string prefixes, empty list matches all keys
  std::vector<std::string> keyPrefixList_{};

  // set of node IDs to match, empty set matches all nodes
  std::set<std::string> originatorIds_{};

  // keyPrefix class to create RE2 set and to match keys
  KeyPrefix keyPrefixObjList_;
};

// structure for common params across all instances of KvStoreDb
struct KvStoreParams {
  // the name of this node (unique in domain)
  std::string nodeId;

  // Queue for publishing KvStore updates to other modules within a process
  messaging::ReplicateQueue<thrift::Publication>& kvStoreUpdatesQueue;

  // the socket to publish changes to kv-store
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> localPubSock;
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> globalPubSock;
  // socket for remote & local commands
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> globalCmdSock;
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> inprocCmdSock;

  // ZMQ high water
  int zmqHwm;
  // IP ToS
  folly::Optional<int> maybeIpTos;
  // how often to request full db sync from peers
  std::chrono::seconds dbSyncInterval;
  // KvStore key filters
  std::optional<KvStoreFilters> filters;
  // Kvstore flooding rate
  KvStoreFloodRate floodRate = std::nullopt;
  // TTL decrement factor
  std::chrono::milliseconds ttlDecr{Constants::kTtlDecrement};
  bool enableFloodOptimization{false};
  bool isFloodRoot{false};
  bool useFloodOptimization{false};
  std::shared_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient{nullptr};

  KvStoreParams(
      std::string nodeid,
      messaging::ReplicateQueue<thrift::Publication>& kvStoreUpdatesQueue,
      fbzmq::Context& zmqContext,
      fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> globalPubSock,
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> globalCmdSock,
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> inprocCmdSock,
      // ZMQ high water mark
      int zmqhwm,
      // IP QoS
      folly::Optional<int> maybeipTos,
      // how often to request full db sync from peers
      std::chrono::seconds dbsyncInterval,
      std::optional<KvStoreFilters> filter,
      // Kvstore flooding rate
      KvStoreFloodRate floodrate,
      // TTL decrement factor
      std::chrono::milliseconds ttldecr,
      bool enablefloodOptimization,
      bool isfloodRoot,
      bool usefloodOptimization)
      : nodeId(nodeid),
        kvStoreUpdatesQueue(kvStoreUpdatesQueue),
        localPubSock(zmqContext),
        globalPubSock(std::move(globalPubSock)),
        globalCmdSock(std::move(globalCmdSock)),
        inprocCmdSock(std::move(inprocCmdSock)),
        zmqHwm(zmqhwm),
        maybeIpTos(std::move(maybeipTos)),
        dbSyncInterval(dbsyncInterval),
        filters(std::move(filter)),
        floodRate(std::move(floodrate)),
        ttlDecr(ttldecr),
        enableFloodOptimization(enablefloodOptimization),
        isFloodRoot(isfloodRoot),
        useFloodOptimization(usefloodOptimization) {}
};

// The class represents a KV Store DB and stores KV pairs in internal map.
// KV store DB instance is created for each area.
// This class processes messages received from KvStore server. The configuration
// is passed via constructor arguments.

class KvStoreDb : public DualNode {
 public:
  KvStoreDb(
      OpenrEventBase* evb,
      KvStoreParams& kvParams,
      const std::string& area,
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> peersyncSock,
      bool isFloodRoot,
      const std::string& nodeId,
      std::unordered_map<std::string, thrift::PeerSpec> peers);

  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsgHelper(
      thrift::KvStoreRequest& thriftReq);

  // Extracts the counters and submit them to monitor
  std::unordered_map<std::string, int64_t> getCounters();

  // get multiple keys at once
  thrift::Publication getKeyVals(std::vector<std::string> const& keys);

  // dump the entries of my KV store whose keys match the given prefix
  // if prefix is the empty sting, the full KV store is dumped
  thrift::Publication dumpAllWithFilters(KvStoreFilters const& kvFilters) const;

  // dump the hashes of my KV store whose keys match the given prefix
  // if prefix is the empty sting, the full hash store is dumped
  thrift::Publication dumpHashWithFilters(
      KvStoreFilters const& kvFilters) const;

  // dump the keys on which hashes differ from given keyVals
  thrift::Publication dumpDifference(
      std::unordered_map<std::string, thrift::Value> const& myKeyVal,
      std::unordered_map<std::string, thrift::Value> const& reqKeyVal) const;

  // Merge received publication with local store and publish out the delta.
  // If senderId is set, will build <key:value> map from kvStore_ and
  // rcvdPublication.tobeUpdatedKeys and send back to senderId to update it
  // @return: Number of KV updates applied
  size_t mergePublication(
      thrift::Publication const& rcvdPublication,
      std::optional<std::string> senderId = std::nullopt);

  // update Time to expire filed in Publication
  // removeAboutToExpire: knob to remove keys which are about to expire
  // and hence do not want to include them. Constants::kTtlThreshold
  void updatePublicationTtl(
      thrift::Publication& thriftPub, bool removeAboutToExpire = false);

  // add new peers to sync with
  void addPeers(std::unordered_map<std::string, thrift::PeerSpec> const& peers);

  // delete some peers we are subscribed to
  void delPeers(std::vector<std::string> const& peers);

  // dump all peers we are subscribed to
  thrift::PeerCmdReply dumpPeers();

  // process spanning-tree-set command to set/unset a child for a given root
  void processFloodTopoSet(
      const thrift::FloodTopoSetParams& setParams) noexcept;

  // get current snapshot of SPT(s) information
  thrift::SptInfos processFloodTopoGet() noexcept;

 private:
  // disable copying
  KvStoreDb(KvStoreDb const&) = delete;
  KvStoreDb& operator=(KvStoreDb const&) = delete;

  // Kv store parameters
  KvStoreParams& kvParams_;

  //
  // Private methods
  //

  // send dual messages over syncSock
  bool sendDualMessages(
      const std::string& neighbor,
      const thrift::DualMessages& msgs) noexcept override;

  // send topology-set command to peer, peer will set/unset me as child
  // rootId: action will applied on given rootId
  // peerName: peer name
  // setChild: true if set, false if unset
  // allRoots: if true, rootId will be ignored, action will be applied to all
  //           roots. (currently used for initial unsetChildAll() cmd)
  void sendTopoSetCmd(
      const std::string& rootId,
      const std::string& peerName,
      bool setChild,
      bool allRoots) noexcept;

  // set child on given rootId
  void setChild(
      const std::string& rootId, const std::string& peerName) noexcept;

  // unset child on given rootId
  void unsetChild(
      const std::string& rootId, const std::string& peerName) noexcept;

  // unset child on all rootIds
  void unsetChildAll(const std::string& peerName) noexcept;

  // callbacks when nexthop changed for a given root-id
  void processNexthopChange(
      const std::string& rootId,
      const std::optional<std::string>& oldNh,
      const std::optional<std::string>& newNh) noexcept override;

  // get flooding peers for a given spt-root-id
  // if rootId is none => flood to all physical peers
  // else only flood to formed SPT-peers for rootId
  std::unordered_set<std::string> getFloodPeers(
      const std::optional<std::string>& rootId);

  // collect router-client send failure statistics in following form
  // "kvstore.send_failure.dst-peer-id.error-code"
  // error: fbzmq-Error
  // dstSockId: destination socket identity
  void collectSendFailureStats(
      const fbzmq::Error& error, const std::string& dstSockId);

  // request full-sync (KEY_DUMP) with peersToSyncWith_
  void requestFullSyncFromPeers();

  // add new query entries into ttlCountdownQueue from publication
  // and Reschedule ttl expiry timer if needed
  void updateTtlCountdownQueue(const thrift::Publication& publication);

  // periodically count down and purge expired keys from CountdownQueue
  void cleanupTtlCountdownQueue();

  // Function to flood publication to neighbors
  // publication => data element to flood
  // rateLimit => if 'false', publication will not be rate limited
  // setFloodRoot => if 'false', floodRootId will not be set
  void floodPublication(
      thrift::Publication&& publication,
      bool rateLimit = true,
      bool setFloodRoot = true);

  // perform last step as a 3-way full-sync request
  // full-sync initiator sends back key-val to senderId (where we made
  // full-sync request to) who need to update those keys
  void finalizeFullSync(
      const std::vector<std::string>& keys, const std::string& senderId);

  // process received KV_DUMP from one of our neighbor
  void processSyncResponse() noexcept;

  // randomly request sync from one connected neighbor
  void requestSync();

  // this will poll the sockets listening to the requests
  void attachCallbacks();

  // Submit full-sync event to monitor
  void logSyncEvent(
      const std::string& peerNodeName,
      const std::chrono::milliseconds syncDuration);

  // Submit events to monitor
  void logKvEvent(const std::string& event, const std::string& key);

  // buffer publications blocked by the rate limiter
  void bufferPublication(thrift::Publication&& publication);

  // flood pending update blocked by rate limiter
  void floodBufferedUpdates(void);

  // Send message via socket
  folly::Expected<size_t, fbzmq::Error> sendMessageToPeer(
      const std::string& peerSocketId, const thrift::KvStoreRequest& request);

  //
  // Private variables
  //
  // area identified of this KvStoreDb instance
  const std::string area_{};

  // zmq ROUTER socket for requesting full dumps from peers
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> peerSyncSock_;

  //
  // Mutable state
  //

  // The peers we will be talking to: both PUB and CMD URLs for each. We use
  // peerAddCounter_ to uniquely identify a peering session's socket-id.
  uint64_t peerAddCounter_{0};
  std::unordered_map<
      std::string /* node-name */,
      std::pair<thrift::PeerSpec, std::string /* socket-id */>>
      peers_;

  // set of peers to perform full sync from. We use exponential backoff to try
  // repetitively untill we succeeed (without overwhelming anyone with too
  // many requests).
  std::unordered_map<std::string, ExponentialBackoff<std::chrono::milliseconds>>
      peersToSyncWith_{};

  // Callback timer to get full KEY_DUMP from peersToSyncWith_
  std::unique_ptr<fbzmq::ZmqTimeout> fullSyncTimer_;

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // store keys mapped to (version, originatoId, value)
  std::unordered_map<std::string, thrift::Value> kvStore_;

  // TTL count down queue
  TtlCountdownQueue ttlCountdownQueue_;

  // TTL count down timer
  std::unique_ptr<fbzmq::ZmqTimeout> ttlCountdownTimer_;

  // Data-struct for maintaining stats/counters
  fbzmq::ThreadData tData_;

  // Map of latest peer sync up request send to each peer
  // this is used to measure full-dump sync time between this node and each of
  // its peers
  std::unordered_map<
      std::string,
      std::chrono::time_point<std::chrono::steady_clock>>
      latestSentPeerSync_;

  // Kvstore rate limiter
  std::unique_ptr<folly::BasicTokenBucket<>> floodLimiter_{nullptr};

  // timer to send pending kvstore publication
  std::unique_ptr<fbzmq::ZmqTimeout> pendingPublicationTimer_{nullptr};

  // timer for requesting full-sync
  std::unique_ptr<folly::AsyncTimeout> requestSyncTimer_{nullptr};

  // pending keys to flood publication
  // map<flood-root-id: set<keys>>
  std::
      unordered_map<std::optional<std::string>, std::unordered_set<std::string>>
          publicationBuffer_{};

  // max parallel syncs allowed. It's initialized with '2' and doubles
  // up to a max value of kMaxFullSyncPendingCountThresholdfor each full sync
  // response received
  int32_t fullSycnReqInProgress_{2};

  // event loop
  OpenrEventBase* evb_{nullptr};
};

// The class represent a server on either the thrift server port or the
// it listens for submission on REP socket. The configuration
// is passed via constructor arguments. This class instantiates KV Store DB
// for each area. The list of areas is passed in the constructor.
// The messages received are either sent to a specific instance of
// KvStore DB or to all instances.

class KvStore final : public OpenrEventBase {
 public:
  KvStore(
      // the zmq context to use for IO
      fbzmq::Context& zmqContext,
      // the name of this node (unique in domain)
      std::string nodeId,
      // Queue for publishing kvstore updates
      messaging::ReplicateQueue<thrift::Publication>& kvStoreUpdatesQueue,
      // the url we use to publish our updates to
      // local subscribers
      KvStoreLocalPubUrl localPubUrl,
      // the url we use to publish our updates to
      // any subscriber (often encrypted)
      KvStoreGlobalPubUrl globalPubUrl,
      // the url to receive command from local and
      // non local clients (often encrypted channel)
      KvStoreGlobalCmdUrl globalCmdUrl,
      // the url to submit to monitor
      MonitorSubmitUrl monitorSubmitUrl,
      // IP TOS value to set on sockets using TCP
      folly::Optional<int> ipTos,
      // how often to request full db sync from peers
      std::chrono::seconds dbSyncInterval,
      // how often to submit to monitor
      std::chrono::seconds monitorSubmitInterval,
      // initial list of peers to connect to
      std::unordered_map<std::string, thrift::PeerSpec> peers,
      // KvStore key filters
      std::optional<KvStoreFilters> filters = std::nullopt,
      // ZMQ high water mark
      int zmqHwm = Constants::kHighWaterMark,
      // Kvstore flooding rate
      KvStoreFloodRate floodRate = std::nullopt,
      // TTL decrement factor
      std::chrono::milliseconds ttlDecr = Constants::kTtlDecrement,
      bool enableFloodOptimization = false,
      bool isFloodRoot = false,
      bool useFloodOptimization = false,
      const std::unordered_set<std::string>& areas = {
          openr::thrift::KvStore_constants::kDefaultArea()});

  // process the key-values publication, and attempt to
  // merge it in existing map (first argument)
  // Return a publication made out of the updated values
  static std::unordered_map<std::string, thrift::Value> mergeKeyValues(
      std::unordered_map<std::string, thrift::Value>& kvStore,
      std::unordered_map<std::string, thrift::Value> const& update,
      std::optional<KvStoreFilters> const& filters = std::nullopt);

  // compare two thrift::Values to figure out which value is better to
  // use, it will compare following attributes in order
  // <version>, <orginatorId>, <value>, <ttl-version>
  // return 1 if v1 is better,
  //       -1 if v2 is better,
  //        0 if equal,
  //       -2 if unknown
  // unknown can happen if value is missing (only hash is provided)
  static int compareValues(const thrift::Value& v1, const thrift::Value& v2);

  // Public APIs
  fbzmq::thrift::CounterMap getCounters();

  folly::SemiFuture<std::unique_ptr<thrift::AreasConfig>> getAreasConfig();

  folly::SemiFuture<std::unique_ptr<thrift::Publication>> getKvStoreKeyVals(
      thrift::KeyGetParams keyGetParams,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<folly::Unit> setKvStoreKeyVals(
      thrift::KeySetParams keySetParams,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<std::unique_ptr<thrift::Publication>> dumpKvStoreKeys(
      thrift::KeyDumpParams keyDumpParams,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<std::unique_ptr<thrift::Publication>> dumpKvStoreHashes(
      thrift::KeyDumpParams keyDumpParams,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>> getKvStorePeers(
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<folly::Unit> addUpdateKvStorePeers(
      thrift::PeerAddParams peerAddParams,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<folly::Unit> deleteKvStorePeers(
      thrift::PeerDelParams peerDelParams,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<std::unique_ptr<thrift::SptInfos>> getSpanningTreeInfos(
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<folly::Unit> updateFloodTopologyChild(
      thrift::FloodTopoSetParams floodTopoSetParams,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  folly::SemiFuture<folly::Unit> processKvStoreDualMessage(
      thrift::DualMessages dualMessages,
      std::string area = openr::thrift::KvStore_constants::kDefaultArea());

  // Inproc URL for REQ/REP to KvStore
  const std::string inprocCmdUrl;

 private:
  // disable copying
  KvStore(KvStore const&) = delete;
  KvStore& operator=(KvStore const&) = delete;

  //
  // Private methods
  //

  void prepareSocket(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& socket,
      std::string const& url,
      folly::Optional<int> maybeIpTos = folly::none);

  void processCmdSocketRequest(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock) noexcept;

  // This function wraps `processRequestMsgHelper` and updates send/received
  // bytes counters.
  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      fbzmq::Message&& msg);

  void submitCounters();

  //
  // Private variables
  //

  //
  // Non mutable state
  //

  // The ZMQ URL we'll be using for publications
  const std::string localPubUrl_;
  const std::string globalPubUrl_;

  // Interval to submit to monitor. Default value is high
  // to avoid submission of counters in testing.
  const std::chrono::seconds monitorSubmitInterval_;

  std::optional<KvStoreFilters> filters_ = std::nullopt;

  //
  // Mutable state
  //

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_;

  // client to interact with monitor
  std::shared_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // kvstore parameters common to all kvstoreDB
  KvStoreParams kvParams_;

  // map of area IDs and instance of KvStoreDb
  std::unordered_map<std::string /* area ID */, KvStoreDb> kvStoreDb_{};

  // Data-struct for maintaining stats/counters
  fbzmq::ThreadData tData_;

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // list of areas
  std::unordered_set<std::string> areas_{};
};
} // namespace openr
