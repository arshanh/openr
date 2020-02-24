/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LinkMonitor.h"

#include <functional>

#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/spark/Spark.h>

using apache::thrift::FRAGILE;

namespace {

const std::string kConfigKey{"link-monitor-config"};

/**
 * Transformation function to convert measured rtt (in us) to a metric value
 * to be used. Metric can never be zero.
 */
int32_t
getRttMetric(int64_t rttUs) {
  return std::max((int)(rttUs / 100), (int)1);
}

void
printLinkMonitorConfig(openr::thrift::LinkMonitorConfig const& config) {
  VLOG(1) << "LinkMonitor config .... ";
  VLOG(1) << "\tnodeLabel: " << config.nodeLabel;
  VLOG(1) << "\tisOverloaded: " << (config.isOverloaded ? "true" : "false");
  if (not config.overloadedLinks.empty()) {
    VLOG(1) << "\toverloadedLinks: "
            << folly::join(",", config.overloadedLinks);
  }
  if (not config.linkMetricOverrides.empty()) {
    VLOG(1) << "\tlinkMetricOverrides: ";
    for (auto const& kv : config.linkMetricOverrides) {
      VLOG(1) << "\t\t" << kv.first << ": " << kv.second;
    }
  }
}

} // anonymous namespace

namespace openr {

//
// LinkMonitor code
//
LinkMonitor::LinkMonitor(
    //
    // Immutable state initializers
    //
    fbzmq::Context& zmqContext,
    std::string nodeId,
    int32_t platformThriftPort,
    KvStoreLocalCmdUrl kvStoreLocalCmdUrl,
    KvStoreLocalPubUrl kvStoreLocalPubUrl,
    std::unique_ptr<re2::RE2::Set> includeRegexList,
    std::unique_ptr<re2::RE2::Set> excludeRegexList,
    std::unique_ptr<re2::RE2::Set> redistRegexList,
    std::vector<thrift::IpPrefix> const& staticPrefixes,
    bool useRttMetric,
    bool enablePerfMeasurement,
    bool enableV4,
    bool enableSegmentRouting,
    bool forwardingTypeMpls,
    bool forwardingAlgoKsp2Ed,
    AdjacencyDbMarker adjacencyDbMarker,
    messaging::ReplicateQueue<thrift::InterfaceDatabase>& intfUpdatesQueue,
    messaging::RQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue,
    MonitorSubmitUrl const& monitorSubmitUrl,
    PersistentStore* configStore,
    bool assumeDrained,
    messaging::ReplicateQueue<thrift::PrefixUpdateRequest>& prefixUpdatesQueue,
    PlatformPublisherUrl const& platformPubUrl,
    std::chrono::seconds adjHoldTime,
    std::chrono::milliseconds flapInitialBackoff,
    std::chrono::milliseconds flapMaxBackoff,
    std::chrono::milliseconds ttlKeyInKvStore,
    const std::unordered_set<std::string>& areas)
    : nodeId_(nodeId),
      platformThriftPort_(platformThriftPort),
      kvStoreLocalCmdUrl_(kvStoreLocalCmdUrl),
      kvStoreLocalPubUrl_(kvStoreLocalPubUrl),
      includeRegexList_(std::move(includeRegexList)),
      excludeRegexList_(std::move(excludeRegexList)),
      redistRegexList_(std::move(redistRegexList)),
      staticPrefixes_(staticPrefixes),
      useRttMetric_(useRttMetric),
      enablePerfMeasurement_(enablePerfMeasurement),
      enableV4_(enableV4),
      enableSegmentRouting_(enableSegmentRouting),
      forwardingTypeMpls_(forwardingTypeMpls),
      forwardingAlgoKsp2Ed_(forwardingAlgoKsp2Ed),
      adjacencyDbMarker_(adjacencyDbMarker),
      platformPubUrl_(platformPubUrl),
      flapInitialBackoff_(flapInitialBackoff),
      flapMaxBackoff_(flapMaxBackoff),
      ttlKeyInKvStore_(ttlKeyInKvStore),
      adjHoldUntilTimePoint_(std::chrono::steady_clock::now() + adjHoldTime),
      // mutable states
      interfaceUpdatesQueue_(intfUpdatesQueue),
      prefixUpdatesQueue_(prefixUpdatesQueue),
      nlEventSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      expBackoff_(Constants::kInitialBackoff, Constants::kMaxBackoff),
      configStore_(configStore),
      areas_(areas) {
  CHECK(configStore_);
  // Create throttled adjacency advertiser
  advertiseAdjacenciesThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        // will advertise to all areas but will not trigger a adj key update
        // if nothing changed. For peers no action is taken if nothing changed
        advertiseKvStorePeers();
        advertiseAdjacencies();
      });

  // Create throttled interfaces and addresses advertiser
  advertiseIfaceAddrThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        advertiseIfaceAddr();
      });
  // Create timer. Timer is used for immediate or delayed executions.
  advertiseIfaceAddrTimer_ = fbzmq::ZmqTimeout::make(
      getEvb(), [this]() noexcept { advertiseIfaceAddr(); });

  LOG(INFO) << "Loading link-monitor config";
  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Create config-store client
  auto config =
      configStore_->loadThriftObj<thrift::LinkMonitorConfig>(kConfigKey).get();
  if (config.hasValue()) {
    LOG(INFO) << "Loaded link-monitor config from disk.";
    config_ = config.value();
    printLinkMonitorConfig(config_);
  } else {
    config_.isOverloaded = assumeDrained;
    LOG(WARNING) << folly::sformat(
        "Failed to load link-monitor config. "
        "Setting node as {}",
        assumeDrained ? "DRAINED" : "UNDRAINED");
  }

  //  Create KvStore client
  kvStoreClient_ = std::make_unique<KvStoreClient>(
      zmqContext,
      this,
      nodeId_,
      kvStoreLocalCmdUrl_,
      kvStoreLocalPubUrl_,
      folly::none, /* persist key timer */
      folly::none /* recv timeout */);

  if (enableSegmentRouting) {
    // create range allocator to get unique node labels
    for (const auto& area : areas_) {
      rangeAllocator_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(area),
          std::forward_as_tuple(
              nodeId_,
              Constants::kNodeLabelRangePrefix.toString(),
              kvStoreClient_.get(),
              [&](folly::Optional<int32_t> newVal) noexcept {
                config_.nodeLabel = newVal ? newVal.value() : 0;
                advertiseAdjacencies();
              },
              std::chrono::milliseconds(100),
              std::chrono::seconds(2),
              false /* override owner */,
              nullptr,
              Constants::kRangeAllocTtl,
              area));

      // Delay range allocation until we have formed all of our adjcencies
      auto startAllocTimer =
          folly::AsyncTimeout::make(*getEvb(), [this, area]() noexcept {
            folly::Optional<int32_t> initValue;
            if (config_.nodeLabel != 0) {
              initValue = config_.nodeLabel;
            }
            rangeAllocator_.at(area).startAllocator(
                Constants::kSrGlobalRange, initValue);
          });
      startAllocTimer->scheduleTimeout(adjHoldTime);
      startAllocationTimers_.emplace_back(std::move(startAllocTimer));
    }
  }

  // Schedule callback to advertise the initial set of adjacencies and prefixes
  adjHoldTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    LOG(INFO) << "Hold time expired. Advertising adjacencies and addresses";
    // Advertise adjacencies and addresses after hold-timeout
    advertiseAdjacencies();
    advertiseRedistAddrs();

    // Cancel throttle as we are publishing latest state
    if (advertiseAdjacenciesThrottled_->isActive()) {
      advertiseAdjacenciesThrottled_->cancel();
    }
  });
  adjHoldTimer_->scheduleTimeout(adjHoldTime);

  // Add fiber to process the neighbor events
  addFiberTask([q = std::move(neighborUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeEvent = q.get();
      VLOG(1) << "Received neighbor update";
      if (maybeEvent.hasError()) {
        LOG(INFO) << "Terminating neighbor update processing fiber";
        break;
      }
      processNeighborEvent(std::move(maybeEvent).value());
    }
  });

  // Initialize ZMQ sockets
  prepare();

  // Initialize stats keys
  tData_.addStatExportType("link_monitor.neighbor_up", fbzmq::SUM);
  tData_.addStatExportType("link_monitor.neighbor_down", fbzmq::SUM);
  tData_.addStatExportType("link_monitor.advertise_adjacencies", fbzmq::SUM);
  tData_.addStatExportType("link_monitor.advertise_links", fbzmq::SUM);
}

void
LinkMonitor::prepare() noexcept {
  //
  // Prepare all sockets
  //

  // Subscribe to link/addr events published by NetlinkAgent
  VLOG(2) << "Connect to PlatformPublisher to subscribe NetlinkEvent on "
          << platformPubUrl_;
  const auto linkEventType =
      static_cast<uint16_t>(thrift::PlatformEventType::LINK_EVENT);
  const auto addrEventType =
      static_cast<uint16_t>(thrift::PlatformEventType::ADDRESS_EVENT);
  auto nlLinkSubOpt =
      nlEventSub_.setSockOpt(ZMQ_SUBSCRIBE, &linkEventType, sizeof(uint16_t));
  if (nlLinkSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to " << linkEventType << " "
               << nlLinkSubOpt.error();
  }
  auto nlAddrSubOpt =
      nlEventSub_.setSockOpt(ZMQ_SUBSCRIBE, &addrEventType, sizeof(uint16_t));
  if (nlAddrSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to " << addrEventType << " "
               << nlAddrSubOpt.error();
  }
  const auto nlSub = nlEventSub_.connect(fbzmq::SocketUrl{platformPubUrl_});
  if (nlSub.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << platformPubUrl_ << "' "
               << nlSub.error();
  }

  syncInterfaces();

  // Listen for messages from spark
  addSocket(
      fbzmq::RawZmqSocketPtr{*sparkReportSock_},
      ZMQ_POLLIN,
      [this](int) noexcept {
        VLOG(1) << "LinkMonitor: Spark message received...";

        auto maybeEvent =
            sparkReportSock_.recvThriftObj<thrift::SparkNeighborEvent>(
                serializer_, Constants::kReadTimeout);
        if (maybeEvent.hasError()) {
          LOG(ERROR) << "Error processing Spark event object: "
                     << maybeEvent.error();
          return;
        }
        auto event = maybeEvent.value();

        auto neighborAddrV4 = toIPAddress(event.neighbor.transportAddressV4);
        auto neighborAddrV6 = toIPAddress(event.neighbor.transportAddressV6);

        VLOG(2) << "Received neighbor event for " << event.neighbor.nodeName
                << " from " << event.neighbor.ifName << " at " << event.ifName
                << " with addrs " << neighborAddrV6.str() << " and "
                << neighborAddrV4.str();

        switch (event.eventType) {
        case thrift::SparkNeighborEventType::NEIGHBOR_UP:
          logEvent(
              "NB_UP",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);

          VLOG(2) << "Calling addNlNeighbor!!!!";
          try {
              client_->sync_addNlNeighbor(event.ifName, 
                                          neighborAddrV6.str());
          } catch (std::exception const& ex) {
              LOG(ERROR) << "Error in syncing neighbor to kernel";
          }

          neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
          break;

        case thrift::SparkNeighborEventType::NEIGHBOR_RESTART:
          logEvent(
              "NB_RESTART",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          try {
              client_->sync_addNlNeighbor(event.ifName,
                                          neighborAddrV6.str());
          } catch (std::exception const& ex) {
              LOG(ERROR) << "Error in syncing neighbor to kernel";
          }

          neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
          break;

        case thrift::SparkNeighborEventType::NEIGHBOR_DOWN:
          logEvent(
              "NB_DOWN",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);

          try {
              client_->sync_delNlNeighbor(event.ifName,
                                          neighborAddrV6.str());
          } catch (std::exception const& ex) {
              LOG(ERROR) << "Error in syncing neighbor to kernel";
          }
 
          neighborDownEvent(event.neighbor.nodeName, event.ifName);
          break;

        case thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE: {
          if (!useRttMetric_) {
            break;
          }

          logEvent(
              "NB_RTT_CHANGE",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);

          int32_t newRttMetric = getRttMetric(event.rttUs);
          VLOG(1) << "Metric value changed for neighbor "
                  << event.neighbor.nodeName << " to " << newRttMetric;
          const auto adjId =
              std::make_pair(event.neighbor.nodeName, event.ifName);
          auto it = adjacencies_.find(adjId);
          if (it != adjacencies_.end()) {
            auto& adj = it->second.second;
            adj.metric = newRttMetric;
            adj.rtt = event.rttUs;
          } else {
            // this occurs when a neighbor reports NEIGHBOR_UP but has not been
            // added into adjacencies bcoz of throttling
            auto _it = peerAddRequests_.find(adjId);
            DCHECK(_it != peerAddRequests_.end());
            auto& adj = _it->second.second;
            adj.metric = newRttMetric;
            adj.rtt = event.rttUs;
          }
          advertiseMyAdjacenciesThrottled_->operator()();
          break;
        }

        default:
          LOG(ERROR) << "Unknown event type " << (int32_t)event.eventType;
        }
      }); // sparkReportSock_ callback

  addSocket(
      fbzmq::RawZmqSocketPtr{*nlEventSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(2) << "LinkMonitor: Netlink Platform message received....";
        fbzmq::Message eventHeader, eventData;
        const auto ret = nlEventSub_.recvMultiple(eventHeader, eventData);
        if (ret.hasError()) {
          LOG(ERROR) << "Error processing PlatformPublisher event "
                     << "publication for node: " << nodeId_
                     << ", exception: " << ret.error();
          return;
        }

        auto eventMsg =
            eventData.readThriftObj<thrift::PlatformEvent>(serializer_);
        if (eventMsg.hasError()) {
          LOG(ERROR) << "Error in reading publication eventData";
          return;
        }

        const auto eventType = eventMsg.value().eventType;
        CHECK_EQ(
            static_cast<uint16_t>(eventType),
            eventHeader.read<uint16_t>().value());

        switch (eventType) {
        case thrift::PlatformEventType::LINK_EVENT: {
          VLOG(3) << "Received Link Event from Platform....";
          try {
            const auto linkEvt =
                fbzmq::util::readThriftObjStr<thrift::LinkEntry>(
                    eventMsg.value().eventData, serializer_);
            auto interfaceEntry = getOrCreateInterfaceEntry(linkEvt.ifName);
            if (interfaceEntry) {
              const bool wasUp = interfaceEntry->isUp();
              interfaceEntry->updateAttrs(
                  linkEvt.ifIndex, linkEvt.isUp, linkEvt.weight);
              logLinkEvent(
                  interfaceEntry->getIfName(),
                  wasUp,
                  interfaceEntry->isUp(),
                  interfaceEntry->getBackoffDuration());
            }
          } catch (std::exception const& e) {
            LOG(ERROR) << "Error parsing linkEvt. Reason: "
                       << folly::exceptionStr(e);
          }
        } break;

        case thrift::PlatformEventType::ADDRESS_EVENT: {
          VLOG(3) << "Received Address Event from Platform....";
          try {
            const auto addrEvt =
                fbzmq::util::readThriftObjStr<thrift::AddrEntry>(
                    eventMsg.value().eventData, serializer_);
            auto interfaceEntry = getOrCreateInterfaceEntry(addrEvt.ifName);
            if (interfaceEntry) {
              interfaceEntry->updateAddr(
                  toIPNetwork(addrEvt.ipPrefix, false /* no masking */),
                  addrEvt.isValid);
            }
          } catch (std::exception const& e) {
            LOG(ERROR) << "Error parsing addrEvt. Reason: "
                       << folly::exceptionStr(e);
          }
        } break;

        default:
          LOG(ERROR) << "Wrong eventType received on " << nodeId_
                     << ", eventType: " << static_cast<uint16_t>(eventType);
        }
      });

  // Schedule periodic timer for monitor submission
  const bool isPeriodic = true;
  monitorTimer_ = fbzmq::ZmqTimeout::make(
      getEvb(), [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // Schedule periodic timer for InterfaceDb re-sync from Netlink Platform
  interfaceDbSyncTimer_ = fbzmq::ZmqTimeout::make(getEvb(), [this]() noexcept {
    auto success = syncInterfaces();
    if (success) {
      VLOG(2) << "InterfaceDb Sync is successful";
      expBackoff_.reportSuccess();
      interfaceDbSyncTimer_->scheduleTimeout(
          Constants::kPlatformSyncInterval, isPeriodic);
    } else {
      tData_.addStatValue(
          "link_monitor.thrift.failure.getAllLinks", 1, fbzmq::SUM);
      // Apply exponential backoff and schedule next run
      expBackoff_.reportError();
      interfaceDbSyncTimer_->scheduleTimeout(
          expBackoff_.getTimeRemainingUntilRetry());
      LOG(ERROR) << "InterfaceDb Sync failed, apply exponential "
                 << "backoff and retry in "
                 << expBackoff_.getTimeRemainingUntilRetry().count() << " ms";
    }
  });
  // schedule immediate with small timeout
  interfaceDbSyncTimer_->scheduleTimeout(std::chrono::milliseconds(100));
}

void
LinkMonitor::neighborUpEvent(
    const thrift::BinaryAddress& neighborAddrV4,
    const thrift::BinaryAddress& neighborAddrV6,
    const thrift::SparkNeighborEvent& event) {
  const std::string& ifName = event.ifName;
  const std::string& remoteNodeName = event.neighbor.nodeName;
  const std::string& remoteIfName = event.neighbor.ifName;
  const std::string& area = event.area;
  const auto adjId = std::make_pair(remoteNodeName, ifName);
  const int32_t neighborKvStorePubPort = event.neighbor.kvStorePubPort;
  const int32_t neighborKvStoreCmdPort = event.neighbor.kvStoreCmdPort;
  auto rttMetric = getRttMetric(event.rttUs);
  auto now = std::chrono::system_clock::now();
  // current unixtime in s
  int64_t timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  VLOG(2) << "LinkMonitor::neighborUpEvent called for '" << neighborAddrV6.str()
          << "', nodeName: '" << remoteNodeName << "'"
          << ", nodeIfName: '" << remoteIfName << "'";
  syslog(
      LOG_NOTICE,
      "%s",
      folly::sformat(
          "Neighbor {} is up on interface {}.", remoteNodeName, ifName)
          .c_str());


  int64_t weight = 1;
  if (interfaces_.count(ifName)) {
    weight = interfaces_.at(ifName).getWeight();
  }

  thrift::Adjacency newAdj = createThriftAdjacency(
      remoteNodeName /* otherNodeName */,
      ifName,
      toString(neighborAddrV6) /* nextHopV6 */,
      toString(neighborAddrV4) /* nextHopV4 */,
      useRttMetric_ ? rttMetric : 1 /* metric */,
      enableSegmentRouting_ ? event.label : 0 /* adjacency-label */,
      false /* overload bit */,
      useRttMetric_ ? event.rttUs : 0,
      timestamp,
      weight,
      remoteIfName /* otherIfName */);

  SYSLOG(INFO)
      << "Neighbor " << remoteNodeName << " is up on interface " << ifName
      << ". Remote Interface: " << remoteIfName << ", metric: " << newAdj.metric
      << ", rttUs: " << event.rttUs << ", addrV4: " << toString(neighborAddrV4)
      << ", addrV6: " << toString(neighborAddrV6) << ", area: " << area;
  tData_.addStatValue("link_monitor.neighbor_up", 1, fbzmq::SUM);

  std::string pubUrl, repUrl;
  if (!mockMode_) {
    // use link local address
    pubUrl = folly::sformat(
        "tcp://[{}%{}]:{}",
        toString(neighborAddrV6),
        ifName,
        neighborKvStorePubPort);
    repUrl = folly::sformat(
        "tcp://[{}%{}]:{}",
        toString(neighborAddrV6),
        ifName,
        neighborKvStoreCmdPort);
  } else {
    // use inproc address
    pubUrl = folly::sformat("inproc://{}-kvstore-pub-global", remoteNodeName);
    repUrl = folly::sformat("inproc://{}-kvstore-cmd-global", remoteNodeName);
  }

  // two cases upon this event:
  // 1) the min interface changes: the previous min interface's connection will
  // be overridden by KvStoreClient, thus no need to explicitly remove it
  // 2) does not change: the existing connection to a neighbor is retained
  const auto& peerSpec =
      thrift::PeerSpec(FRAGILE, pubUrl, repUrl, event.supportFloodOptimization);
  adjacencies_[adjId] =
      AdjacencyValue(peerSpec, std::move(newAdj), false, area);

  // Advertise KvStore peers immediately
  advertiseKvStorePeers(area, {{remoteNodeName, peerSpec}});

  // Advertise new adjancies in a throttled fashion
  advertiseAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::neighborDownEvent(
    const std::string& remoteNodeName,
    const std::string& ifName,
    const std::string& area) {
  const auto adjId = std::make_pair(remoteNodeName, ifName);

  SYSLOG(INFO) << "Neighbor " << remoteNodeName << " is down on interface "
               << ifName;
  tData_.addStatValue("link_monitor.neighbor_down", 1, fbzmq::SUM);

  auto adjValueIt = adjacencies_.find(adjId);
  if (adjValueIt != adjacencies_.end()) {
    // remove such adjacencies
    adjacencies_.erase(adjValueIt);
  }
  // advertise both peers and adjacencies
  advertiseKvStorePeers(area);
  advertiseAdjacencies(area);
}

void
LinkMonitor::neighborRestartingEvent(
    const std::string& remoteNodeName,
    const std::string& ifName,
    const std::string& area) {
  const auto adjId = std::make_pair(remoteNodeName, ifName);
  SYSLOG(INFO) << "Neighbor " << remoteNodeName
               << " is restarting on interface " << ifName;

  // update adjacencies_ restarting-bit and advertise peers
  auto adjValueIt = adjacencies_.find(adjId);
  if (adjValueIt != adjacencies_.end()) {
    adjValueIt->second.isRestarting = true;
  }
  advertiseKvStorePeers(area);
}

std::unordered_map<std::string, thrift::PeerSpec>
LinkMonitor::getPeersFromAdjacencies(
    const std::unordered_map<AdjacencyKey, AdjacencyValue>& adjacencies,
    const std::string& area) {
  std::unordered_map<std::string, std::string> neighborToIface;
  for (const auto& adjKv : adjacencies) {
    if (adjKv.second.area != area || adjKv.second.isRestarting) {
      continue;
    }
    const auto& nodeName = adjKv.first.first;
    const auto& iface = adjKv.first.second;

    // Look up for node
    auto it = neighborToIface.find(nodeName);
    if (it == neighborToIface.end()) {
      // Add nbr-iface if not found
      neighborToIface.emplace(nodeName, iface);
    } else if (it->second > iface) {
      // Update iface if it is smaller (minimum interface)
      it->second = iface;
    }
  }

  std::unordered_map<std::string, thrift::PeerSpec> peers;
  for (const auto& kv : neighborToIface) {
    peers.emplace(kv.first, adjacencies.at(kv).peerSpec);
  }
  return peers;
}

void
LinkMonitor::advertiseKvStorePeers(
    const std::string& area,
    const std::unordered_map<std::string, thrift::PeerSpec>& upPeers) {
  // Get old and new peer list. Also update local state
  const auto oldPeers = std::move(peers_[area]);
  peers_[area] = getPeersFromAdjacencies(adjacencies_, area);
  const auto& newPeers = peers_[area];

  // Get list of peers to delete
  std::vector<std::string> toDelPeers;
  for (const auto& oldKv : oldPeers) {
    const auto& nodeName = oldKv.first;
    if (newPeers.count(nodeName) == 0) {
      toDelPeers.emplace_back(nodeName);
      logPeerEvent("DEL_PEER", oldKv.first, oldKv.second);
    }
  }

  // Delete old peers
  if (toDelPeers.size() > 0) {
    const auto ret = kvStoreClient_->delPeers(toDelPeers, area);
    CHECK(ret) << ret.error();
  }

  // Get list of peers to add
  std::unordered_map<std::string, thrift::PeerSpec> toAddPeers;
  for (const auto& newKv : newPeers) {
    const auto& nodeName = newKv.first;
    // send out peer-add to kvstore if
    // 1. it's a new peer (not exist in old-peers)
    // 2. old-peer but peer-spec changed (e.g parallel link case)
    if (oldPeers.find(nodeName) == oldPeers.end() or
        oldPeers.at(nodeName) != newKv.second) {
      toAddPeers.emplace(nodeName, newKv.second);
      logPeerEvent("ADD_PEER", newKv.first, newKv.second);
    }
  }

  for (const auto& upPeer : upPeers) {
    const auto& name = upPeer.first;
    const auto& spec = upPeer.second;
    // upPeer MUST already be in current state peers_
    CHECK(peers_.at(area).count(name));

    if (toAddPeers.count(name)) {
      // already added, skip it
      continue;
    }
    if (spec != peers_.at(area).at(name)) {
      // spec does not match, skip it
      continue;
    }
    toAddPeers.emplace(name, spec);
  }

  // Add new peers
  if (toAddPeers.size() > 0) {
    const auto ret = kvStoreClient_->addPeers(std::move(toAddPeers), area);
    CHECK(ret) << ret.error();
  }
}

void
LinkMonitor::advertiseKvStorePeers(
    const std::unordered_map<std::string, thrift::PeerSpec>& upPeers) {
  // Get old and new peer list. Also update local state
  for (const auto& area : areas_) {
    advertiseKvStorePeers(area, upPeers);
  }
}

void
LinkMonitor::advertiseAdjacencies(const std::string& area) {
  if (std::chrono::steady_clock::now() < adjHoldUntilTimePoint_) {
    // Too early for advertising my own adjacencies. Let timeout advertise it
    // and skip here.
    return;
  }

  auto adjDb = thrift::AdjacencyDatabase();
  adjDb.thisNodeName = nodeId_;
  adjDb.isOverloaded = config_.isOverloaded;
  adjDb.nodeLabel = config_.nodeLabel;
  adjDb.area = area;
  for (const auto& adjKv : adjacencies_) {
    // 'second.second' is the adj object for this peer
    // must match the area
    if (adjKv.second.area != area) {
      continue;
    }
    // NOTE: copy on purpose
    auto adj = folly::copy(adjKv.second.adjacency);

    // Set link overload bit
    adj.isOverloaded = config_.overloadedLinks.count(adj.ifName) > 0;

    // Override metric with link metric if it exists
    adj.metric =
        folly::get_default(config_.linkMetricOverrides, adj.ifName, adj.metric);

    // Override metric with adj metric if it exists
    thrift::AdjKey adjKey;
    adjKey.nodeName = adj.otherNodeName;
    adjKey.ifName = adj.ifName;
    adj.metric =
        folly::get_default(config_.adjMetricOverrides, adjKey, adj.metric);

    adjDb.adjacencies.emplace_back(std::move(adj));
  }

  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "ADJ_DB_UPDATED");
    adjDb.perfEvents = perfEvents;
  } else {
    DCHECK(!adjDb.perfEvents.hasValue());
  }

  LOG(INFO) << "Updating adjacency database in KvStore with "
            << adjDb.adjacencies.size() << " entries in area: " << area;
  const auto keyName = adjacencyDbMarker_ + nodeId_;
  std::string adjDbStr = fbzmq::util::writeThriftObjStr(adjDb, serializer_);
  kvStoreClient_->persistKey(keyName, adjDbStr, ttlKeyInKvStore_, area);
  tData_.addStatValue("link_monitor.advertise_adjacencies", 1, fbzmq::SUM);

  // Config is most likely to have changed. Update it in `ConfigStore`
  configStore_->storeThriftObj(kConfigKey, config_); // not awaiting on result

  // Cancel throttle timeout if scheduled
  if (advertiseAdjacenciesThrottled_->isActive()) {
    advertiseAdjacenciesThrottled_->cancel();
  }
}
void
LinkMonitor::advertiseAdjacencies() {
  // advertise to all areas. Once area configuration per link is implemented
  // then adjacencies can be advertised to a specific area
  for (const auto& area : areas_) {
    // Update KvStore
    advertiseAdjacencies(area);
  }
}

void
LinkMonitor::advertiseIfaceAddr() {
  auto retryTime = getRetryTimeOnUnstableInterfaces();

  advertiseInterfaces();
  advertiseRedistAddrs();

  // Cancel throttle timeout if scheduled
  if (advertiseIfaceAddrThrottled_->isActive()) {
    advertiseIfaceAddrThrottled_->cancel();
  }

  // Schedule new timeout if needed to advertise UP but UNSTABLE interfaces
  // once their backoff is clear.
  if (retryTime.count() != 0) {
    advertiseIfaceAddrTimer_->scheduleTimeout(retryTime);
    VLOG(2) << "advertiseIfaceAddr timer scheduled in " << retryTime.count()
            << " ms";
  }
}

void
LinkMonitor::advertiseInterfaces() {
  tData_.addStatValue("link_monitor.advertise_links", 1, fbzmq::SUM);

  // Create interface database
  thrift::InterfaceDatabase ifDb;
  ifDb.thisNodeName = nodeId_;
  for (auto& kv : interfaces_) {
    auto& ifName = kv.first;
    auto& interface = kv.second;
    // Perform regex match
    if (not checkIncludeExcludeRegex(
            ifName, includeRegexList_, excludeRegexList_)) {
      continue;
    }
    // Get interface info and override active status
    auto interfaceInfo = interface.getInterfaceInfo();
    interfaceInfo.isUp = interface.isActive();
    ifDb.interfaces.emplace(ifName, std::move(interfaceInfo));
  }

  // publish new interface database to other modules (Fib & Spark)
  interfaceUpdatesQueue_.push(std::move(ifDb));
}

void
LinkMonitor::advertiseRedistAddrs() {
  if (std::chrono::steady_clock::now() < adjHoldUntilTimePoint_) {
    // Too early for advertising my own prefixes. Let timeout advertise it
    // and skip here.
    return;
  }

  std::vector<thrift::PrefixEntry> prefixes;

  // Add static prefixes
  for (auto const& prefix : staticPrefixes_) {
    auto prefixEntry = openr::thrift::PrefixEntry();
    prefixEntry.prefix = prefix;
    prefixEntry.type = thrift::PrefixType::LOOPBACK;
    prefixEntry.data = "";
    prefixEntry.forwardingType = forwardingTypeMpls_
        ? thrift::PrefixForwardingType::SR_MPLS
        : thrift::PrefixForwardingType::IP;
    prefixEntry.forwardingAlgorithm = forwardingAlgoKsp2Ed_
        ? thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP
        : thrift::PrefixForwardingAlgorithm::SP_ECMP;
    prefixEntry.ephemeral = folly::none;
    prefixes.push_back(prefixEntry);
  }

  // Add redistribute addresses
  for (auto& kv : interfaces_) {
    auto& interface = kv.second;
    // Ignore in-active interfaces
    if (not interface.isActive()) {
      continue;
    }
    // Perform regex match
    if (not matchRegexSet(interface.getIfName(), redistRegexList_)) {
      continue;
    }
    // Add all prefixes of this interface
    for (auto& prefix : interface.getGlobalUnicastNetworks(enableV4_)) {
      prefix.forwardingType = forwardingTypeMpls_
          ? thrift::PrefixForwardingType::SR_MPLS
          : thrift::PrefixForwardingType::IP;
      prefix.forwardingAlgorithm = forwardingAlgoKsp2Ed_
          ? thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP
          : thrift::PrefixForwardingAlgorithm::SP_ECMP;
      prefixes.emplace_back(std::move(prefix));
    }
  }

  // Advertise via prefix manager client
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::SYNC_PREFIXES_BY_TYPE;
  request.type = openr::thrift::PrefixType::LOOPBACK;
  request.prefixes = std::move(prefixes);
  prefixUpdatesQueue_.push(std::move(request));
}

std::chrono::milliseconds
LinkMonitor::getRetryTimeOnUnstableInterfaces() {
  bool hasUnstableInterface = false;
  std::chrono::milliseconds minRemainMs = flapMaxBackoff_;
  for (auto& kv : interfaces_) {
    auto& interface = kv.second;
    if (interface.isActive()) {
      continue;
    }

    const auto& curRemainMs = interface.getBackoffDuration();
    if (curRemainMs.count() > 0) {
      VLOG(2) << "Interface " << interface.getIfName()
              << " is in backoff state for " << curRemainMs.count() << "ms";
      minRemainMs = std::min(minRemainMs, curRemainMs);
      hasUnstableInterface = true;
    }
  }

  return hasUnstableInterface ? minRemainMs : std::chrono::milliseconds(0);
}

InterfaceEntry* FOLLY_NULLABLE
LinkMonitor::getOrCreateInterfaceEntry(const std::string& ifName) {
  // Return null if ifName doesn't quality regex match criteria
  if (not checkIncludeExcludeRegex(
          ifName, includeRegexList_, excludeRegexList_) and
      not matchRegexSet(ifName, redistRegexList_)) {
    return nullptr;
  }

  // Return existing element if any
  auto it = interfaces_.find(ifName);
  if (it != interfaces_.end()) {
    return &(it->second);
  }

  // Create one and return it's reference
  auto res = interfaces_.emplace(
      ifName,
      InterfaceEntry(
          ifName,
          flapInitialBackoff_,
          flapMaxBackoff_,
          *advertiseIfaceAddrThrottled_,
          *advertiseIfaceAddrTimer_));

  return &(res.first->second);
}

void
LinkMonitor::createNetlinkSystemHandlerClient() {
  // Reset client if channel is not good
  if (socket_ && (!socket_->good() || socket_->hangup())) {
    client_.reset();
    socket_.reset();
  }

  // Do not create new client if one exists already
  if (client_) {
    return;
  }

  // Create socket to thrift server and set some connection parameters
  socket_ = apache::thrift::async::TAsyncSocket::newSocket(
      &evb_,
      Constants::kPlatformHost.toString(),
      platformThriftPort_,
      Constants::kPlatformConnTimeout.count());

  // Create channel and set timeout
  auto channel = apache::thrift::HeaderClientChannel::newChannel(socket_);
  channel->setTimeout(Constants::kPlatformProcTimeout.count());

  // Set BinaryProtocol and Framed client type for talkiing with thrift1 server
  channel->setProtocolId(apache::thrift::protocol::T_BINARY_PROTOCOL);
  channel->setClientType(THRIFT_FRAMED_DEPRECATED);

  // Reset client_
  client_ =
      std::make_unique<thrift::SystemServiceAsyncClient>(std::move(channel));
}

bool
LinkMonitor::syncInterfaces() {
  VLOG(1) << "Syncing Interface DB from Netlink Platform";

  //
  // Retrieve latest link snapshot from SystemService
  //
  std::vector<thrift::Link> links;
  try {
    createNetlinkSystemHandlerClient();
    client_->sync_getAllLinks(links);
  } catch (const std::exception& e) {
    client_.reset();
    LOG(ERROR) << "Failed to sync LinkDb from NetlinkSystemHandler. Error: "
               << folly::exceptionStr(e);
    return false;
  }

  //
  // Process received data and make updates in InterfaceEntry objects
  //
  for (const auto& link : links) {
    // Get interface entry
    auto interfaceEntry = getOrCreateInterfaceEntry(link.ifName);
    if (not interfaceEntry) {
      continue;
    }

    std::unordered_set<folly::CIDRNetwork> newNetworks;
    for (const auto& network : link.networks) {
      newNetworks.emplace(toIPNetwork(network, false /* no masking */));
    }
    const auto& oldNetworks = interfaceEntry->getNetworks();

    // Update link attributes
    const bool wasUp = interfaceEntry->isUp();
    interfaceEntry->updateAttrs(link.ifIndex, link.isUp, link.weight);
    logLinkEvent(
        interfaceEntry->getIfName(),
        wasUp,
        interfaceEntry->isUp(),
        interfaceEntry->getBackoffDuration());

    // Remove old addresses if they are not in new
    for (auto const& oldNetwork : oldNetworks) {
      if (newNetworks.count(oldNetwork) == 0) {
        interfaceEntry->updateAddr(oldNetwork, false);
      }
    }

    // Add new addresses if they are not in old
    for (auto const& newNetwork : newNetworks) {
      if (oldNetworks.count(newNetwork) == 0) {
        interfaceEntry->updateAddr(newNetwork, true);
      }
    }
  }

  return true;
}

void
LinkMonitor::processNeighborEvent(thrift::SparkNeighborEvent&& event) {
  auto neighborAddrV4 = event.neighbor.transportAddressV4;
  auto neighborAddrV6 = event.neighbor.transportAddressV6;

  VLOG(1) << "Received neighbor event for " << event.neighbor.nodeName
          << " from " << event.neighbor.ifName << " at " << event.ifName
          << " with addrs " << toString(neighborAddrV6) << " and "
          << (enableV4_ ? toString(neighborAddrV4) : "")
          << " Area:" << event.area;

  switch (event.eventType) {
  case thrift::SparkNeighborEventType::NEIGHBOR_UP: {
    logNeighborEvent(event);
    neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
    break;
  }

  case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING: {
    logNeighborEvent(event);
    neighborRestartingEvent(event.neighbor.nodeName, event.ifName, event.area);
    break;
  }

  case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED: {
    logNeighborEvent(event);
    neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
    break;
  }

  case thrift::SparkNeighborEventType::NEIGHBOR_DOWN: {
    logNeighborEvent(event);
    neighborDownEvent(event.neighbor.nodeName, event.ifName, event.area);
    break;
  }

  case thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE: {
    if (!useRttMetric_) {
      break;
    }

    logNeighborEvent(event);

    int32_t newRttMetric = getRttMetric(event.rttUs);
    VLOG(1) << "Metric value changed for neighbor " << event.neighbor.nodeName
            << " to " << newRttMetric;
    auto it = adjacencies_.find({event.neighbor.nodeName, event.ifName});
    if (it != adjacencies_.end()) {
      auto& adj = it->second.adjacency;
      adj.metric = newRttMetric;
      adj.rtt = event.rttUs;
      advertiseAdjacenciesThrottled_->operator()();
    }
    break;
  }

  default:
    LOG(ERROR) << "Unknown event type " << (int32_t)event.eventType;
  }
}

// NOTE: add commands which set/unset overload bit or metric values will
// immediately advertise new adjacencies into the KvStore.
folly::SemiFuture<folly::Unit>
LinkMonitor::setNodeOverload(bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), isOverloaded]() mutable {
    std::string cmd =
        isOverloaded ? "SET_NODE_OVERLOAD" : "UNSET_NODE_OVERLOAD";
    if (config_.isOverloaded == isOverloaded) {
      LOG(INFO) << "Skip cmd: [" << cmd << "]. Node already in target state: ["
                << (isOverloaded ? "OVERLOADED" : "NOT OVERLOADED") << "]";
    } else {
      config_.isOverloaded = isOverloaded;
      SYSLOG(INFO) << (isOverloaded ? "Setting" : "Unsetting")
                   << " overload bit for node";
      advertiseAdjacencies();
    }
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setInterfaceOverload(
    std::string interfaceName, bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, isOverloaded]() mutable {
        std::string cmd =
            isOverloaded ? "SET_LINK_OVERLOAD" : "UNSET_LINK_OVERLOAD";
        if (0 == interfaces_.count(interfaceName)) {
          LOG(ERROR) << "Skip cmd: [" << cmd
                     << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (isOverloaded && config_.overloadedLinks.count(interfaceName)) {
          LOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                    << " is already overloaded";
          p.setValue();
          return;
        }

        if (!isOverloaded && !config_.overloadedLinks.count(interfaceName)) {
          LOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                    << " is currently NOT overloaded";
          p.setValue();
          return;
        }

        if (isOverloaded) {
          config_.overloadedLinks.insert(interfaceName);
          SYSLOG(INFO) << "Setting overload bit for interface "
                       << interfaceName;
        } else {
          config_.overloadedLinks.erase(interfaceName);
          SYSLOG(INFO) << "Unsetting overload bit for interface "
                       << interfaceName;
        }
        advertiseAdjacenciesThrottled_->operator()();
        p.setValue();
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setLinkMetric(
    std::string interfaceName, std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, overrideMetric]() mutable {
        std::string cmd = overrideMetric.has_value() ? "SET_LINK_METRIC"
                                                     : "UNSET_LINK_METRIC";
        if (0 == interfaces_.count(interfaceName)) {
          LOG(ERROR) << "Skip cmd: [" << cmd
                     << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value() &&
            config_.linkMetricOverrides.count(interfaceName) &&
            config_.linkMetricOverrides[interfaceName] ==
                overrideMetric.value()) {
          LOG(INFO) << "Skip cmd: " << cmd
                    << ". Overridden metric: " << overrideMetric.value()
                    << " already set for interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (!overrideMetric.has_value() &&
            !config_.linkMetricOverrides.count(interfaceName)) {
          LOG(INFO) << "Skip cmd: " << cmd
                    << ". No overridden metric found for interface: "
                    << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value()) {
          config_.linkMetricOverrides[interfaceName] = overrideMetric.value();
          SYSLOG(INFO) << "Overriding metric for interface " << interfaceName
                       << " to " << overrideMetric.value();
        } else {
          config_.linkMetricOverrides.erase(interfaceName);
          SYSLOG(INFO) << "Removing metric override for interface "
                       << interfaceName;
        }
        advertiseAdjacenciesThrottled_->operator()();
        p.setValue();
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setAdjacencyMetric(
    std::string interfaceName,
    std::string adjNodeName,
    std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        adjNodeName,
                        overrideMetric]() mutable {
    std::string cmd = overrideMetric.has_value() ? "SET_ADJACENCY_METRIC"
                                                 : "UNSET_ADJACENCY_METRIC";
    thrift::AdjKey adjKey;
    adjKey.ifName = interfaceName;
    adjKey.nodeName = adjNodeName;

    // Invalid adj encountered. Ignore
    if (!adjacencies_.count(std::make_pair(adjNodeName, interfaceName))) {
      LOG(ERROR) << "Skip cmd: [" << cmd << "] due to unknown adj: ["
                 << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value() &&
        config_.adjMetricOverrides.count(adjKey) &&
        config_.adjMetricOverrides[adjKey] == overrideMetric.value()) {
      LOG(INFO) << "Skip cmd: " << cmd
                << ". Overridden metric: " << overrideMetric.value()
                << " already set for: [" << adjNodeName << ":" << interfaceName
                << "]";
      p.setValue();
      return;
    }

    if (!overrideMetric.has_value() &&
        !config_.adjMetricOverrides.count(adjKey)) {
      LOG(INFO) << "Skip cmd: " << cmd << ". No overridden metric found for: ["
                << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value()) {
      config_.adjMetricOverrides[adjKey] = overrideMetric.value();
      SYSLOG(INFO) << "Overriding metric for adjacency: [" << adjNodeName << ":"
                   << interfaceName << "] to " << overrideMetric.value();
    } else {
      config_.adjMetricOverrides.erase(adjKey);
      SYSLOG(INFO) << "Removing metric override for adjacency: [" << adjNodeName
                   << ":" << interfaceName << "]";
    }
    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
LinkMonitor::getInterfaces() {
  VLOG(2) << "Dump Links requested, replying withV " << interfaces_.size()
          << " links";

  folly::Promise<std::unique_ptr<thrift::DumpLinksReply>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    // reply with the dump of known interfaces and their states
    thrift::DumpLinksReply reply;
    reply.thisNodeName = nodeId_;
    reply.isOverloaded = config_.isOverloaded;

    // Fill interface details
    for (auto& kv : interfaces_) {
      auto& ifName = kv.first;
      auto& interface = kv.second;
      auto ifDetails = thrift::InterfaceDetails(
          apache::thrift::FRAGILE,
          interface.getInterfaceInfo(),
          config_.overloadedLinks.count(ifName) > 0,
          0 /* custom metric value */,
          0 /* link flap back off time */);

      // Add metric override if any
      folly::Optional<int32_t> maybeMetric;
      if (config_.linkMetricOverrides.count(ifName) > 0) {
        maybeMetric.assign(config_.linkMetricOverrides.at(ifName));
      }
      ifDetails.metricOverride = maybeMetric;

      // Add link-backoff
      auto backoffMs = interface.getBackoffDuration();
      if (backoffMs.count() != 0) {
        ifDetails.linkFlapBackOffMs = backoffMs.count();
      } else {
        ifDetails.linkFlapBackOffMs = folly::none;
      }

      reply.interfaceDetails.emplace(ifName, std::move(ifDetails));
    }
    p.setValue(std::make_unique<thrift::DumpLinksReply>(std::move(reply)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::AdjacencyDatabase>>
LinkMonitor::getLinkMonitorAdjacencies() {
  VLOG(2) << "Dump adj requested, reply with " << adjacencies_.size()
          << " adjs";

  folly::Promise<std::unique_ptr<thrift::AdjacencyDatabase>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    // build adjacency database
    thrift::AdjacencyDatabase adjDb;
    adjDb.thisNodeName = nodeId_;
    adjDb.isOverloaded = config_.isOverloaded;
    adjDb.nodeLabel = config_.nodeLabel;

    // fill adjacency details
    for (const auto& adjKv : adjacencies_) {
      // NOTE: copy on purpose
      auto adj = folly::copy(adjKv.second.adjacency);

      // Set link overload bit
      adj.isOverloaded = config_.overloadedLinks.count(adj.ifName) > 0;

      // Override metric with link metric if it exists
      adj.metric = folly::get_default(
          config_.linkMetricOverrides, adj.ifName, adj.metric);

      // Override metric with adj metric if it exists
      thrift::AdjKey adjKey;
      adjKey.nodeName = adj.otherNodeName;
      adjKey.ifName = adj.ifName;
      adj.metric =
          folly::get_default(config_.adjMetricOverrides, adjKey, adj.metric);

      adjDb.adjacencies.emplace_back(std::move(adj));
    }
    p.setValue(std::make_unique<thrift::AdjacencyDatabase>(std::move(adjDb)));
  });
  return sf;
}

void
LinkMonitor::submitCounters() {
  VLOG(3) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["link_monitor.adjacencies"] = adjacencies_.size();
  counters["link_monitor.zmq_event_queue_size"] =
      getEvb()->getNotificationQueueSize();
  for (const auto& kv : adjacencies_) {
    auto& adj = kv.second.adjacency;
    counters["link_monitor.metric." + adj.otherNodeName] = adj.metric;
  }

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

void
LinkMonitor::logNeighborEvent(thrift::SparkNeighborEvent const& event) {
  fbzmq::LogSample sample{};
  sample.addString(
      "event",
      apache::thrift::TEnumTraits<thrift::SparkNeighborEventType>::findName(
          event.eventType));
  sample.addString("node_name", nodeId_);
  sample.addString("neighbor", event.neighbor.nodeName);
  sample.addString("interface", event.ifName);
  sample.addString("remote_interface", event.neighbor.ifName);
  sample.addInt("rtt_us", event.rttUs);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

void
LinkMonitor::logLinkEvent(
    const std::string& iface,
    bool wasUp,
    bool isUp,
    std::chrono::milliseconds backoffTime) {
  // Do not log if no state transition
  if (wasUp == isUp) {
    return;
  }

  fbzmq::LogSample sample{};
  const std::string event = isUp ? "UP" : "DOWN";

  sample.addString("event", folly::sformat("IFACE_{}", event));
  sample.addString("node_name", nodeId_);
  sample.addString("interface", iface);
  sample.addInt("backoff_ms", backoffTime.count());

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));

  SYSLOG(INFO) << "Interface " << iface << " is " << event
               << " and has backoff of " << backoffTime.count() << "ms";
}

void
LinkMonitor::logPeerEvent(
    const std::string& event,
    const std::string& peerName,
    const thrift::PeerSpec& peerSpec) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("node_name", nodeId_);
  sample.addString("peer_name", peerName);
  sample.addString("pub_url", peerSpec.pubUrl);
  sample.addString("cmd_url", peerSpec.cmdUrl);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

} // namespace openr
