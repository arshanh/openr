/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqThrottle.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncTimeout.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Decision_types.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {
struct ProcessPublicationResult {
  bool adjChanged{false};
  bool prefixesChanged{false};
};

struct BestPathCalResult {
  bool success{false};
  std::string bestNode{""};
  std::set<std::string> nodes;
  folly::Optional<int64_t> bestIgpMetric{folly::none};
  std::string const* bestData{nullptr};
  folly::Optional<thrift::MetricVector> bestVector{folly::none};
};

namespace detail {
/**
 * Keep track of hash for pending SPF calculation because of certain
 * updates in graph.
 * Out of all buffered applications we try to keep the perf events for the
 * oldest appearing event.
 */
struct DecisionPendingUpdates {
  void
  clear() {
    count_ = 0;
    minTs_ = folly::none;
    perfEvents_ = folly::none;
  }

  void
  addUpdate(
      const std::string& nodeName,
      const folly::Optional<thrift::PerfEvents>& perfEvents) {
    ++count_;

    // Skip if perf information is missing
    if (not perfEvents.hasValue()) {
      if (not perfEvents_) {
        perfEvents_ = thrift::PerfEvents{};
        addPerfEvent(*perfEvents_, nodeName, "DECISION_RECEIVED");
        minTs_ = perfEvents_->events.front().unixTs;
      }
      return;
    }

    // Update local copy of perf evens if it is newer than the one to be added
    // We do debounce (batch updates) for recomputing routes and in order to
    // measure convergence performance, it is better to use event which is
    // oldest.
    if (!minTs_ or minTs_.value() > perfEvents->events.front().unixTs) {
      minTs_ = perfEvents->events.front().unixTs;
      perfEvents_ = perfEvents;
      addPerfEvent(*perfEvents_, nodeName, "DECISION_RECEIVED");
    }
  }

  uint32_t
  getCount() const {
    return count_;
  }

  folly::Optional<thrift::PerfEvents>
  getPerfEvents() const {
    return perfEvents_;
  }

 private:
  uint32_t count_{0};
  folly::Optional<int64_t> minTs_;
  folly::Optional<thrift::PerfEvents> perfEvents_;
};
} // namespace detail

// The class to compute shortest-paths using Dijkstra algorithm
class SpfSolver {
 public:
  // these need to be defined in the .cpp so they can refer
  // to the actual implementation of SpfSolverImpl
  SpfSolver(
      const std::string& myNodeName,
      bool enableV4,
      bool computeLfaPaths,
      bool enableOrderedFib = false,
      bool bgpDryRun = false,
      bool bgpUseIgpMetric = false);
  ~SpfSolver();

  //
  // The following methods talk to implementation so need to
  // be defined in the .cpp
  //

  // update adjacencies for the given router
  std::pair<
      bool /* topology has changed */,
      bool /* route attributes has changed (nexthop addr, node/adj label */>
  updateAdjacencyDatabase(thrift::AdjacencyDatabase const& adjacencyDb);

  bool hasHolds() const;

  // delete a node's adjacency database
  // return true if this has caused any change in graph
  bool deleteAdjacencyDatabase(const std::string& nodeName);

  // get adjacency databases
  std::unordered_map<
      std::string /* nodeName */,
      thrift::AdjacencyDatabase> const&
  getAdjacencyDatabases();

  // update prefixes for a given router. Returns true if this has caused any
  // routeDb change
  bool updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb);

  // get prefix databases
  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
  getPrefixDatabases();

  // Compute all routes from perspective of a given router.
  // Returns folly::none if myNodeName doesn't have any prefix database
  folly::Optional<thrift::RouteDatabase> buildPaths(
      const std::string& myNodeName);

  // Build route database using global prefix database and cached SPF
  // computation from perspective of a given router.
  // Returns folly::none if myNodeName doesn't have any prefix database
  folly::Optional<thrift::RouteDatabase> buildRouteDb(
      const std::string& myNodeName);

  bool decrementHolds();

  std::unordered_map<std::string, int64_t> getCounters();

 private:
  // no-copy
  SpfSolver(SpfSolver const&) = delete;
  SpfSolver& operator=(SpfSolver const&) = delete;

  // pointer to implementation class
  class SpfSolverImpl;
  std::unique_ptr<SpfSolverImpl> impl_;
};

//
// The decision thread announces FIB updates for myNodeName every time
// there is a change in LSDB. The announcements are made on a PUB socket. At
// the same time, it listens on a REP socket to respond with the recent
// FIB state if requested by clients.
//
// On the "client" side of things, it uses REQ socket to request a full dump
// of link-state information from KvStore, and before that it subscribes to
// the PUB address of the KvStore to receive ongoing LSDB updates from KvStore.
//
// The prefix/adjacency Db markers are used to find the keys in KvStore that
// correspond to the prefix information or link state information. This way
// we do not need to try and parse the values to tell that. For example,
// the key name could be "adj:router1" or "prefix:router2" to tell of
// the AdjacencyDatabase of router1 and PrefixDatabase of router2
//

class Decision : public OpenrEventBase {
 public:
  Decision(
      std::string myNodeName,
      bool enableV4,
      bool computeLfaPaths,
      bool enableOrderedFib,
      bool bgpDryRun,
      bool bgpUseIgpMetric,
      const AdjacencyDbMarker& adjacencyDbMarker,
      const PrefixDbMarker& prefixDbMarker,
      std::chrono::milliseconds debounceMinDur,
      std::chrono::milliseconds debounceMaxDur,
      folly::Optional<std::chrono::seconds> gracefulRestartDuration,
      messaging::RQueue<thrift::Publication> kvStoreUpdatesQueue,
      messaging::ReplicateQueue<thrift::RouteDatabaseDelta>& routeUpdatesQueue,
      const MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  virtual ~Decision() = default;

  std::unordered_map<std::string, int64_t> getCounters();

  /*
   * Retrieve routeDb from specified node.
   * If empty nodename specified, will return routeDb of its own
   */
  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>> getDecisionRouteDb(
      std::string nodeName);

  /*
   * Retrieve AdjacencyDatabase as map.
   */
  folly::SemiFuture<std::unique_ptr<thrift::AdjDbs>> getDecisionAdjacencyDbs();

  /*
   * Retrieve PrefixDatabase as a map.
   */
  folly::SemiFuture<std::unique_ptr<thrift::PrefixDbs>> getDecisionPrefixDbs();

 private:
  Decision(Decision const&) = delete;
  Decision& operator=(Decision const&) = delete;

  // process publication from KvStore
  ProcessPublicationResult processPublication(
      thrift::Publication const& thriftPub);

  /**
   * Process received publication and populate the pendingAdjUpdates_
   * attributes which can be applied later on after a debounce timeout.
   */
  detail::DecisionPendingUpdates pendingAdjUpdates_;

  /**
   * Process received publication and populate the pendingPrefixUpdates_
   * attributes upon receiving prefix update publication
   */
  detail::DecisionPendingUpdates pendingPrefixUpdates_;

  // callback timer used on startup to publish routes after
  // gracefulRestartDuration
  std::unique_ptr<fbzmq::ZmqTimeout> coldStartTimer_{nullptr};

  /**
   * Timer to schedule pending update processing
   * Refer to processUpdatesStatus_ to decide whether spf recalculation or
   * just route rebuilding is needed.
   * Apply exponential backoff timeout to avoid churn
   */
  std::unique_ptr<fbzmq::ZmqTimeout> processUpdatesTimer_;
  ExponentialBackoff<std::chrono::milliseconds> processUpdatesBackoff_;

  // store update to-do status
  ProcessPublicationResult processUpdatesStatus_;

  /**
   * Caller function of processPendingAdjUpdates and processPendingPrefixUpdates
   * Check current processUpdatesStatus_ to decide which sub function to call
   * to further process pending updates
   * Reset timer and status afterwards.
   */
  void processPendingUpdates();

  /**
   * Function to process pending adjacency publications.
   */
  void processPendingAdjUpdates();

  /**
   * Function to process prefix updates.
   */
  void processPendingPrefixUpdates();

  void decrementOrderedFibHolds();

  void coldStartUpdate();

  void sendRouteUpdate(
      thrift::RouteDatabase& db, std::string const& eventDescription);

  std::chrono::milliseconds getMaxFib();

  // periodically submit counters to monitor thread
  void submitCounters();

  // node to prefix entries database for nodes advertising per prefix keys
  thrift::PrefixDatabase updateNodePrefixDatabase(
      const std::string& key, const thrift::PrefixDatabase& prefixDb);

  // this node's name and the key markers
  const std::string myNodeName_;
  // the prefix we use to find the adjacency database announcements
  const std::string adjacencyDbMarker_;
  // the prefix we use to find the prefix db key announcements
  const std::string prefixDbMarker_;

  thrift::RouteDatabase routeDb_;

  // Queue to publish route changes
  messaging::ReplicateQueue<thrift::RouteDatabaseDelta>& routeUpdatesQueue_;

  // the pointer to the SPF path calculator
  std::unique_ptr<SpfSolver> spfSolver_;

  // For orderedFib prgramming, we keep track of the fib programming times
  // across the network
  std::unordered_map<std::string, std::chrono::milliseconds> fibTimes_;

  apache::thrift::CompactSerializer serializer_;

  // base interval to submit to monitor with (jitter will be added)
  std::chrono::seconds monitorSyncInterval_{0};

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // Timer for decrementing link holds for ordered fib programming
  std::unique_ptr<fbzmq::ZmqTimeout> orderedFibTimer_{nullptr};

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // need to store all this for backward compatibility, otherwise a key update
  // can lead to mistakenly withdrawing some prefixes
  std::unordered_map<
      std::string,
      std::unordered_map<thrift::IpPrefix, thrift::PrefixEntry>>
      perPrefixPrefixEntries_, fullDbPrefixEntries_;
};

} // namespace openr
