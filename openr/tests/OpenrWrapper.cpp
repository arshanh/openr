/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OpenrWrapper.h"

#include <folly/MapUtil.h>
#include <re2/re2.h>
#include <re2/set.h>

namespace openr {

template <class Serializer>
OpenrWrapper<Serializer>::OpenrWrapper(
    fbzmq::Context& context,
    std::string nodeId,
    bool v4Enabled,
    std::chrono::seconds kvStoreDbSyncInterval,
    std::chrono::seconds kvStoreMonitorSubmitInterval,
    std::chrono::milliseconds sparkHoldTime,
    std::chrono::milliseconds sparkKeepAliveTime,
    std::chrono::milliseconds sparkFastInitKeepAliveTime,
    std::chrono::seconds linkMonitorAdjHoldTime,
    std::chrono::milliseconds linkFlapInitialBackoff,
    std::chrono::milliseconds linkFlapMaxBackoff,
    std::chrono::seconds fibColdStartDuration,
    std::shared_ptr<IoProvider> ioProvider,
    int32_t systemPort,
    uint32_t memLimit,
    bool per_prefix_keys)
    : context_(context),
      nodeId_(nodeId),
      ioProvider_(std::move(ioProvider)),
      monitorSubmitUrl_(folly::sformat("inproc://{}-monitor-submit", nodeId_)),
      monitorPubUrl_(folly::sformat("inproc://{}-monitor-pub", nodeId_)),
      kvStoreLocalPubUrl_(folly::sformat("inproc://{}-kvstore-pub", nodeId_)),
      kvStoreGlobalCmdUrl_(
          folly::sformat("inproc://{}-kvstore-cmd-global", nodeId_)),
      kvStoreGlobalPubUrl_(
          folly::sformat("inproc://{}-kvstore-pub-global", nodeId_)),
      platformPubUrl_(folly::sformat("inproc://{}-platform-pub", nodeId_)),
      kvStoreReqSock_(context),
      platformPubSock_(context),
      systemPort_(systemPort),
      per_prefix_keys_(per_prefix_keys) {
  // LM ifName
  std::string ifName = "vethLMTest_" + nodeId_;

  // create config-store
  configStore_ = std::make_unique<PersistentStore>(
      nodeId_,
      folly::sformat("/tmp/{}_aq_config_store.bin", nodeId_),
      context_);
  // start config-store thread
  std::thread configStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " ConfigStore running.";
    configStore_->run();
    VLOG(1) << nodeId_ << " ConfigStore stopped.";
  });
  configStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(configStoreThread));

  // create zmq monitor
  monitor_ = std::make_unique<fbzmq::ZmqMonitor>(
      MonitorSubmitUrl{monitorSubmitUrl_},
      MonitorPubUrl{monitorPubUrl_},
      context_);

  //
  // create kvstore
  //

  std::optional<KvStoreFilters> filters{std::nullopt};
  kvStore_ = std::make_unique<KvStore>(
      context_,
      nodeId_,
      kvStoreUpdatesQueue_,
      KvStoreLocalPubUrl{kvStoreLocalPubUrl_},
      KvStoreGlobalPubUrl{kvStoreGlobalPubUrl_},
      KvStoreGlobalCmdUrl{kvStoreGlobalCmdUrl_},
      MonitorSubmitUrl{monitorSubmitUrl_},
      folly::none /* ip-tos */,
      kvStoreDbSyncInterval,
      kvStoreMonitorSubmitInterval,
      std::unordered_map<std::string, thrift::PeerSpec>{},
      std::move(filters));

  kvStoreLocalCmdUrl_ = kvStore_->inprocCmdUrl;
  // kvstore client socket
  kvStoreReqSock_.connect(fbzmq::SocketUrl{kvStore_->inprocCmdUrl}).value();

  // kvstore client
  kvStoreClient_ = std::make_unique<KvStoreClient>(
      context_,
      &eventBase_,
      nodeId_,
      KvStoreLocalCmdUrl{kvStoreLocalCmdUrl_},
      KvStoreLocalPubUrl{kvStoreLocalPubUrl_});

  // Subscribe our own prefixDb
  kvStoreClient_->subscribeKey(
      folly::sformat("prefix:{}", nodeId_),
      [&](const std::string& /* key */,
          folly::Optional<thrift::Value> value) noexcept {
        if (!value.hasValue()) {
          return;
        }
        // Parse PrefixDb
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            value.value().value.value(), serializer_);

        SYNCHRONIZED(ipPrefix_) {
          bool received = false;

          for (auto& prefix : prefixDb.prefixEntries) {
            if (prefix.type == thrift::PrefixType::PREFIX_ALLOCATOR) {
              received = true;
              ipPrefix_ = prefix.prefix;
              break;
            }
          }
          if (!received) {
            ipPrefix_ = folly::none;
          }
        }
      },
      false);

  //
  // create spark
  //
  spark_ = std::make_unique<Spark>(
      "terragraph", // domain name
      nodeId_, // node name
      static_cast<uint16_t>(6666), // multicast port
      sparkHoldTime, // hold time ms
      sparkKeepAliveTime, // keep alive ms
      sparkFastInitKeepAliveTime, // fastInitKeepAliveTime ms
      std::chrono::milliseconds{0}, /* spark2_hello_time */
      std::chrono::milliseconds{0}, /* spark2_hello_fast_init_time */
      std::chrono::milliseconds{0}, /* spark2_handshake_time */
      std::chrono::milliseconds{0}, /* spark2_heartbeat_time */
      std::chrono::milliseconds{0}, /* spark2_negotiate_hold_time */
      std::chrono::milliseconds{0}, /* spark2_heartbeat_hold_time */
      folly::none, // ip-tos
      v4Enabled, // enable v4
      true, // enable subnet validation
      interfaceUpdatesQueue_.getReader(),
      neighborUpdatesQueue_,
      MonitorSubmitUrl{monitorSubmitUrl_},
      KvStorePubPort{0}, // these port numbers won't be used
      KvStoreCmdPort{0},
      OpenrCtrlThriftPort{0},
      std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      context_,
      ioProvider_);

  //
  // create link monitor
  //
  std::vector<thrift::IpPrefix> networks;
  networks.emplace_back(toIpPrefix(folly::IPAddress::createNetwork("::/0")));
  re2::RE2::Options options;
  options.set_case_sensitive(false);
  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(ifName + ".*", &regexErr);
  includeRegexList->Compile();
  std::unique_ptr<re2::RE2::Set> excludeRegexList;
  std::unique_ptr<re2::RE2::Set> redistRegexList;

  // start prefix manager
  prefixManager_ = std::make_unique<PrefixManager>(
      nodeId_,
      prefixUpdatesQueue_.getReader(),
      configStore_.get(),
      KvStoreLocalCmdUrl{kvStoreLocalCmdUrl_},
      KvStoreLocalPubUrl{kvStoreLocalPubUrl_},
      MonitorSubmitUrl{monitorSubmitUrl_},
      PrefixDbMarker{"prefix:"},
      per_prefix_keys_ /* create IP prefix keys */,
      false /* prefix-mananger perf measurement */,
      std::chrono::seconds(0),
      Constants::kKvStoreDbTtl,
      context_);

  linkMonitor_ = std::make_unique<LinkMonitor>(
      context_,
      nodeId_,
      static_cast<int32_t>(60099), // platfrom pub port
      KvStoreLocalCmdUrl{kvStoreLocalCmdUrl_},
      KvStoreLocalPubUrl{kvStoreLocalPubUrl_},
      std::move(includeRegexList),
      std::move(excludeRegexList),
      // redistribute interface names
      std::move(redistRegexList),
      networks,
      false, /* use rtt metric */
      false /* enable perf measurement */,
      false /* enable v4 */,
      true /* enable segment routing */,
      false /* prefix type mpls */,
      false /* prefix fwd algo KSP2_ED_ECMP */,
      AdjacencyDbMarker{"adj:"},
      interfaceUpdatesQueue_,
      neighborUpdatesQueue_.getReader(),
      MonitorSubmitUrl{monitorSubmitUrl_},
      configStore_.get(),
      false,
      prefixUpdatesQueue_,
      PlatformPublisherUrl{platformPubUrl_},
      linkMonitorAdjHoldTime,
      linkFlapInitialBackoff,
      linkFlapMaxBackoff,
      Constants::kKvStoreDbTtl);

  //
  // create decision
  //
  decision_ = std::make_unique<Decision>(
      nodeId_,
      v4Enabled, // enable v4
      true, // computeLfaPaths
      false, // enableOrderedFib
      false, // bgpDryRun
      false, // bgpUseIgpMetric
      AdjacencyDbMarker{"adj:"},
      PrefixDbMarker{"prefix:"},
      std::chrono::milliseconds(10),
      std::chrono::milliseconds(250),
      folly::none,
      kvStoreUpdatesQueue_.getReader(),
      routeUpdatesQueue_,
      MonitorSubmitUrl{monitorSubmitUrl_},
      context_);

  //
  // create FIB
  //
  fib_ = std::make_unique<Fib>(
      nodeId_,
      static_cast<int32_t>(60100), // fib agent port
      true, // dry run mode
      false, // segment routing
      false, // ordered Fib
      fibColdStartDuration,
      false, // waitOnDecision
      routeUpdatesQueue_.getReader(),
      interfaceUpdatesQueue_.getReader(),
      MonitorSubmitUrl{monitorSubmitUrl_},
      KvStoreLocalCmdUrl{kvStoreLocalCmdUrl_},
      KvStoreLocalPubUrl{kvStoreLocalPubUrl_},
      context_);

  // Watchdog thread to monitor thread aliveness
  watchdog = std::make_unique<Watchdog>(
      nodeId_, std::chrono::seconds(1), std::chrono::seconds(60), memLimit);

  // Zmq monitor client to get counters
  zmqMonitorClient = std::make_unique<fbzmq::ZmqMonitorClient>(
      context_,
      MonitorSubmitUrl{folly::sformat("inproc://{}-monitor-submit", nodeId_)});
}

template <class Serializer>
void
OpenrWrapper<Serializer>::run() {
  // start monitor thread
  std::thread monitorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " Monitor running.";
    monitor_->run();
    VLOG(1) << nodeId_ << " Monitor stopped.";
  });
  monitor_->waitUntilRunning();
  allThreads_.emplace_back(std::move(monitorThread));

  // start kvstore thread
  std::thread kvStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " KvStore running.";
    kvStore_->run();
    VLOG(1) << nodeId_ << " KvStore stopped.";
  });
  kvStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(kvStoreThread));

  try {
    // bind out publisher socket
    VLOG(2) << "Platform Publisher: Binding pub url '" << platformPubUrl_
            << "'";
    platformPubSock_.bind(fbzmq::SocketUrl{platformPubUrl_}).value();
  } catch (std::exception const& e) {
    LOG(FATAL) << "Platform Publisher: could not bind to '" << platformPubUrl_
               << "'" << folly::exceptionStr(e);
  }

  eventBase_.scheduleTimeout(std::chrono::milliseconds(100), [this]() {
    auto link = thrift::LinkEntry(
        apache::thrift::FRAGILE, "vethLMTest_" + nodeId_, 5, true, 1);

    thrift::PlatformEvent msgLink;
    msgLink.eventType = thrift::PlatformEventType::LINK_EVENT;
    msgLink.eventData = fbzmq::util::writeThriftObjStr(link, serializer_);

    // send header of event in the first 2 byte
    platformPubSock_.sendMore(
        fbzmq::Message::from(static_cast<uint16_t>(msgLink.eventType)).value());
    const auto sendNeighEntryLink =
        platformPubSock_.sendThriftObj(msgLink, serializer_);
    if (sendNeighEntryLink.hasError()) {
      LOG(ERROR) << "Error in sending PlatformEventType Entry, event Type: "
                 << folly::get_default(
                        thrift::_PlatformEventType_VALUES_TO_NAMES,
                        msgLink.eventType,
                        "UNKNOWN");
    }
  });

  const auto seedPrefix =
      folly::IPAddress::createNetwork("fc00:cafe:babe::/62");
  const uint8_t allocPrefixLen = 64;
  prefixAllocator_ = std::make_unique<PrefixAllocator>(
      nodeId_,
      KvStoreLocalCmdUrl{kvStoreLocalCmdUrl_},
      KvStoreLocalPubUrl{kvStoreLocalPubUrl_},
      prefixUpdatesQueue_,
      MonitorSubmitUrl{monitorSubmitUrl_},
      AllocPrefixMarker{"allocprefix:"}, // alloc_prefix_marker
      std::make_pair(seedPrefix, allocPrefixLen),
      false /* set loopback addr */,
      false /* override global address */,
      "" /* loopback interface name */,
      false /* prefix fwd type MPLS */,
      false /* prefix fwd algo KSP2_ED_ECMP */,
      Constants::kPrefixAllocatorSyncInterval,
      configStore_.get(),
      context_,
      systemPort_ /* system agent port*/);

  // Spawn a PrefixManager thread
  std::thread prefixManagerThread([this]() noexcept {
    VLOG(1) << nodeId_ << " PrefixManager running.";
    prefixManager_->run();
    VLOG(1) << nodeId_ << " PrefixManager stopped.";
  });
  prefixManager_->waitUntilRunning();
  allThreads_.emplace_back(std::move(prefixManagerThread));

  // Spawn a PrefixAllocator thread
  std::thread prefixAllocatorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " PrefixAllocator running.";
    prefixAllocator_->run();
    VLOG(1) << nodeId_ << " PrefixAllocator stopped.";
  });
  prefixAllocator_->waitUntilRunning();
  allThreads_.emplace_back(std::move(prefixAllocatorThread));

  // start spark thread
  std::thread sparkThread([this]() {
    VLOG(1) << nodeId_ << " Spark running.";
    spark_->run();
    VLOG(1) << nodeId_ << " Spark stopped.";
  });
  spark_->waitUntilRunning();
  allThreads_.emplace_back(std::move(sparkThread));

  // start link monitor
  std::thread linkMonitorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " LinkMonitor running.";
    linkMonitor_->setAsMockMode();
    linkMonitor_->run();
    VLOG(1) << nodeId_ << " LinkMonitor stopped.";
  });
  linkMonitor_->waitUntilRunning();
  allThreads_.emplace_back(std::move(linkMonitorThread));

  // start decision
  std::thread decisionThread([this]() noexcept {
    VLOG(1) << nodeId_ << " Decision running.";
    decision_->run();
    VLOG(1) << nodeId_ << " Decision stopped.";
  });
  decision_->waitUntilRunning();
  allThreads_.emplace_back(std::move(decisionThread));

  // start fib
  std::thread fibThread([this]() noexcept {
    VLOG(1) << nodeId_ << " FIB running.";
    fib_->run();
    VLOG(1) << nodeId_ << " FIB stopped.";
  });
  fib_->waitUntilRunning();
  allThreads_.emplace_back(std::move(fibThread));

  // start watchdog
  std::thread watchdogThread([this]() noexcept {
    VLOG(1) << nodeId_ << " watchdog running.";
    watchdog->run();
    VLOG(1) << nodeId_ << " watchdog stopped.";
  });
  watchdog->waitUntilRunning();
  allThreads_.emplace_back(std::move(watchdogThread));

  // start eventBase_
  allThreads_.emplace_back([&]() {
    VLOG(1) << nodeId_ << " Starting eventBase_";
    eventBase_.run();
    VLOG(1) << nodeId_ << " Stopping eventBase_";
  });
}

template <class Serializer>
void
OpenrWrapper<Serializer>::stop() {
  // Close all queues
  routeUpdatesQueue_.close();
  interfaceUpdatesQueue_.close();
  neighborUpdatesQueue_.close();
  prefixUpdatesQueue_.close();
  kvStoreUpdatesQueue_.close();

  // stop all modules in reverse order
  eventBase_.stop();
  eventBase_.waitUntilStopped();
  watchdog->stop();
  watchdog->waitUntilStopped();
  fib_->stop();
  fib_->waitUntilStopped();
  decision_->stop();
  decision_->waitUntilStopped();
  linkMonitor_->stop();
  linkMonitor_->waitUntilStopped();
  spark_->stop();
  spark_->waitUntilStopped();
  prefixAllocator_->stop();
  prefixAllocator_->waitUntilStopped();
  prefixManager_->stop();
  prefixManager_->waitUntilStopped();
  kvStore_->stop();
  kvStore_->waitUntilStopped();
  monitor_->stop();
  monitor_->waitUntilStopped();
  configStore_->stop();
  configStore_->waitUntilStopped();

  // wait for all threads to finish
  for (auto& t : allThreads_) {
    t.join();
  }

  LOG(INFO) << nodeId_ << " OpenR stopped";
}

template <class Serializer>
folly::Optional<thrift::IpPrefix>
OpenrWrapper<Serializer>::getIpPrefix() {
  SYNCHRONIZED(ipPrefix_) {
    if (ipPrefix_.hasValue()) {
      return ipPrefix_;
    }
  }
  auto keys =
      kvStoreClient_->dumpAllWithPrefix(folly::sformat("prefix:{}", nodeId_));

  SYNCHRONIZED(ipPrefix_) {
    for (const auto& key : keys.value()) {
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          key.second.value.value(), serializer_);
      if (prefixDb.deletePrefix) {
        // Skip prefixes which are about to be deleted
        continue;
      }

      for (auto& prefix : prefixDb.prefixEntries) {
        if (prefix.type == thrift::PrefixType::PREFIX_ALLOCATOR) {
          ipPrefix_ = prefix.prefix;
          break;
        }
      }
    }
  }
  return ipPrefix_.copy();
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::checkKeyExists(std::string key) {
  auto keys = kvStoreClient_->dumpAllWithPrefix(key);
  return keys.hasValue();
}

template <class Serializer>
folly::Expected<std::unordered_map<std::string, thrift::PeerSpec>, fbzmq::Error>
OpenrWrapper<Serializer>::getKvStorePeers() {
  return kvStoreClient_->getPeers();
}

template <class Serializer>
std::unordered_map<std::string /* key */, thrift::Value>
OpenrWrapper<Serializer>::kvStoreDumpAll(std::string const& prefix) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyDumpParams params;

  params.prefix = prefix;
  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams = params;

  // Make ZMQ call and wait for response
  kvStoreReqSock_.sendThriftObj(request, serializer_).value();
  auto reply =
      kvStoreReqSock_.recvThriftObj<thrift::Publication>(serializer_).value();

  // Return the result
  return reply.keyVals;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::sparkUpdateInterfaceDb(
    const std::vector<SparkInterfaceEntry>& interfaceEntries) {
  thrift::InterfaceDatabase ifDb(
      apache::thrift::FRAGILE, nodeId_, {}, thrift::PerfEvents());
  ifDb.perfEvents = folly::none;

  for (const auto& interface : interfaceEntries) {
    ifDb.interfaces.emplace(
        interface.ifName,
        thrift::InterfaceInfo(
            apache::thrift::FRAGILE,
            true,
            interface.ifIndex,
            {}, // v4Addrs: TO BE DEPRECATED SOON
            {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
            {toIpPrefix(interface.v4Network),
             toIpPrefix(interface.v6LinkLocalNetwork)}));
  }

  interfaceUpdatesQueue_.push(std::move(ifDb));
  return true;
}

template <class Serializer>
thrift::RouteDatabase
OpenrWrapper<Serializer>::fibDumpRouteDatabase() {
  auto routes = fib_->getRouteDb().get();
  return std::move(*routes);
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::addPrefixEntries(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::ADD_PREFIXES;
  request.prefixes = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::withdrawPrefixEntries(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES;
  request.prefixes = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::checkPrefixExists(
    const thrift::IpPrefix& prefix, const thrift::RouteDatabase& routeDb) {
  for (auto const& route : routeDb.unicastRoutes) {
    if (prefix == route.dest) {
      return true;
    }
  }
  return false;
}

// define template instance for some common serializers
template class OpenrWrapper<apache::thrift::CompactSerializer>;
template class OpenrWrapper<apache::thrift::BinarySerializer>;
template class OpenrWrapper<apache::thrift::SimpleJSONSerializer>;

} // namespace openr
