/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/FibService.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/messaging/Queue.h>

namespace openr {

/**
 * Proxy agent to program computed routes using platform dependent agent (e.g.
 * FBOSS in case of Wedge Platform).
 *
 * The functionality is very simple. We just react to RouteDatabase updates
 * from Decision module and forward best paths to switch agent to program.
 * There is no state keeping being done apart from handling interface events.
 *
 * On interface event down event we find affected routes and either withdraw
 * them or reprogram with new nexthops.
 *
 * This RouteDatabase contains all Loop Free Alternate (LFAs) paths along with
 * best paths. So Fib module derives best paths (path with minimum cost) and
 * programs them.
 *
 * Note: If for a prefix there are multiple paths with the smallest cost then
 * we program all of them which simulates ECMP behaviours across programmed
 * nexthops.
 *
 */
class Fib final : public OpenrEventBase {
 public:
  Fib(std::string myNodeName,
      int32_t thriftPort,
      bool dryrun,
      bool enableSegmentRouting,
      bool enableOrderedFib,
      std::chrono::seconds coldStartDuration,
      bool waitOnDecision,
      messaging::RQueue<thrift::RouteDatabaseDelta> routeUpdatesQueue,
      messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue,
      const MonitorSubmitUrl& monitorSubmitUrl,
      const KvStoreLocalCmdUrl& storeCmdUrl,
      const KvStoreLocalPubUrl& storePubUrl,
      fbzmq::Context& zmqContext);

  /**
   * Utility function to create thrift client connection to SwitchAgent. Can
   * throw exception if it fails to open transport to client on specified port.
   * It will return immediately if healthy client connection already exists.
   */
  static void createFibClient(
      folly::EventBase& evb,
      std::shared_ptr<apache::thrift::async::TAsyncSocket>& socket,
      std::unique_ptr<thrift::FibServiceAsyncClient>& client,
      int32_t port);

  /**
   * Perform longest prefix match among all prefixes in route database.
   * @param inputPrefix - a prefix that need to be matched
   * @param unicastRoutes - current unicast routes in RouteDatabase
   *
   * @return the matched IpPrefix if prefix matching succeed.
   */
  static std::optional<thrift::IpPrefix> longestPrefixMatch(
      const folly::CIDRNetwork& inputPrefix,
      const std::unordered_map<thrift::IpPrefix, thrift::UnicastRoute>&
          unicastRoutes);

  /**
   * NOTE: DEPRECATED! Use getUnicastRoutes or getMplsRoutes.
   */
  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>> getRouteDb();

  /**
   * Retrieve unicast routes for specified prefixes or IP. Returns all if
   * no prefix is specified in filter list.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
  getUnicastRoutes(std::vector<std::string> prefixes);

  /**
   * Retrieve mpls routes for specified labels. Returns all if no label is
   * specified in filter list.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
  getMplsRoutes(std::vector<int32_t> labels);

  /**
   * Retrieve performance related information from FIB module
   */
  folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>> getPerfDb();

 private:
  // No-copy
  Fib(const Fib&) = delete;
  Fib& operator=(const Fib&) = delete;

  /**
   * Process new route updates received from Decision module
   */
  void processRouteUpdates(thrift::RouteDatabaseDelta&& routeDelta);

  /**
   * Process interface status information from LinkMonitor. We remove all
   * routes associated with interface if we detect that it just went down.
   */
  void processInterfaceDb(thrift::InterfaceDatabase&& interfaceDb);

  /**
   * Convert local perfDb_ into PerfDataBase
   */
  thrift::PerfDatabase dumpPerfDb() const;

  /**
   * Retrieve unicast routes with specified filters
   */
  std::vector<thrift::UnicastRoute> getUnicastRoutesFiltered(
      std::vector<std::string> prefixes);

  /**
   * Retrieve mpls routes with specified filters
   */
  std::vector<thrift::MplsRoute> getMplsRoutesFiltered(
      std::vector<int32_t> labels);

  /**
   * Trigger add/del routes thrift calls
   * on success no action needed
   * on failure invokes syncRouteDbDebounced
   */
  void updateRoutes(const thrift::RouteDatabaseDelta& routeDbDelta);

  /**
   * Sync the current routeDb_ with the switch agent.
   * on success no action needed
   * on failure invokes syncRouteDbDebounced
   */
  bool syncRouteDb();

  /**
   * Asynchrounsly schedules the syncRouteDb call and returns immediately. All
   * APIs should call this function to sync-routes.
   */
  void syncRouteDbDebounced();

  /**
   * Get aliveSince from FibService, and check if Fib restarts
   * If so, push syncFib to FibService
   */
  void keepAliveCheck();

  // Submit internal state counters to monitor
  void submitCounters();

  // log perf events
  void logPerfEvents(folly::Optional<thrift::PerfEvents> perfEvents);

  // Prefix to available nexthop information. Also store perf information of
  // received route-db if provided.
  struct RouteState {
    // Non modified copy of Unicast and MPLS routes received from Decision
    std::unordered_map<thrift::IpPrefix, thrift::UnicastRoute> unicastRoutes;
    std::unordered_map<uint32_t, thrift::MplsRoute> mplsRoutes;

    // indicates we've received a decision route publication and therefore have
    // routes to sync. will not synce routes with system until this is set
    bool hasRoutesFromDecision{false};

    // Set of routes whose nexthops are auto-resized on link failure.
    // Populated
    // - when nexthop shrink happens on interface down
    // Cleared on
    // - receiving new route for prefix or label
    // - full route sync happens
    // - interface up event happens for disabled nexthop
    std::unordered_set<thrift::IpPrefix> dirtyPrefixes;
    std::unordered_set<uint32_t> dirtyLabels;

    // Flag to indicate the result of previous route programming attempt.
    // If set, it means what currently cached in local routes has not been 100%
    // successfully synced with agent, we have to trigger an enforced full fib
    // sync with agent again
    bool dirtyRouteDb{false};
  };
  RouteState routeState_;

  // Events to capture and indicate performance of protocol convergence.
  std::deque<thrift::PerfEvents> perfDb_;

  // Create timestamp of recently logged perf event
  int64_t recentPerfEventCreateTs_{0};

  // Interface status map
  std::unordered_map<std::string /* ifName*/, bool /* isUp */>
      interfaceStatusDb_;

  // Name of node on which OpenR is running
  const std::string myNodeName_;

  // Switch agent thrift server port
  const int32_t thriftPort_{0};

  // In dry run we do not make actual thrift call to manipulate routes
  bool dryrun_{true};

  // Enable segment routing
  const bool enableSegmentRouting_{false};

  // indicates that we should publish fib programming time to kvstore
  bool enableOrderedFib_{false};

  // amount of time to wait before send routes to agent either when this module
  // starts or the agent we are talking with restarts
  const std::chrono::seconds coldStartDuration_;

  apache::thrift::CompactSerializer serializer_;

  // Thrift client connection to switch FIB Agent using which we actually
  // manipulate routes.
  folly::EventBase evb_;
  std::shared_ptr<apache::thrift::async::TAsyncSocket> socket_{nullptr};
  std::unique_ptr<thrift::FibServiceAsyncClient> client_{nullptr};

  // Callback timer to sync routes to switch agent and scheduled on route-sync
  // failure. ExponentialBackoff timer to ease up things if they go wrong
  std::unique_ptr<fbzmq::ZmqTimeout> syncRoutesTimer_{nullptr};
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // periodically send alive msg to switch agent
  std::unique_ptr<fbzmq::ZmqTimeout> keepAliveTimer_{nullptr};

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  std::unique_ptr<KvStoreClient> kvStoreClient_;

  // Latest aliveSince heard from FibService. If the next one is different then
  // it means that FibAgent has restarted and we need to perform sync.
  int64_t latestAliveSince_{0};

  // moves to true after initial sync
  bool hasSyncedFib_{false};

  const int16_t kFibId_{static_cast<int16_t>(thrift::FibClient::OPENR)};
};

} // namespace openr
