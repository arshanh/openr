/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/ctrl-server/OpenrCtrlHandler.h>

#include <re2/re2.h>

#include <folly/ExceptionString.h>
#include <folly/io/async/SSLContext.h>
#include <folly/io/async/ssl/OpenSSLUtils.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/PersistentStore_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/prefix-manager/PrefixManager.h>

namespace openr {

OpenrCtrlHandler::OpenrCtrlHandler(
    const std::string& nodeName,
    const std::unordered_set<std::string>& acceptablePeerCommonNames,
    Decision* decision,
    Fib* fib,
    KvStore* kvStore,
    LinkMonitor* linkMonitor,
    PersistentStore* configStore,
    PrefixManager* prefixManager,
    MonitorSubmitUrl const& monitorSubmitUrl,
    KvStoreLocalPubUrl const& kvStoreLocalPubUrl,
    fbzmq::ZmqEventLoop& evl,
    fbzmq::Context& context)
    : facebook::fb303::FacebookBase2("openr"),
      nodeName_(nodeName),
      acceptablePeerCommonNames_(acceptablePeerCommonNames),
      decision_(decision),
      fib_(fib),
      kvStore_(kvStore),
      linkMonitor_(linkMonitor),
      configStore_(configStore),
      prefixManager_(prefixManager),
      evl_(evl),
      kvStoreSubSock_(context) {
  // Create monitor client
  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(context, monitorSubmitUrl);

  // Connect to KvStore
  const auto kvStoreSub =
      kvStoreSubSock_.connect(fbzmq::SocketUrl{kvStoreLocalPubUrl});
  if (kvStoreSub.hasError()) {
    LOG(FATAL) << "Error binding to URL " << std::string(kvStoreLocalPubUrl)
               << " " << kvStoreSub.error();
  }

  // Subscribe to everything
  const auto kvStoreSubOpt = kvStoreSubSock_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (kvStoreSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << std::string(kvStoreLocalPubUrl) << " "
               << kvStoreSubOpt.error();
  }

  evl_.runInEventLoop([this]() noexcept {
    evl_.addSocket(
        fbzmq::RawZmqSocketPtr{*kvStoreSubSock_},
        ZMQ_POLLIN,
        [&](int) noexcept {
          apache::thrift::CompactSerializer serializer;
          // Read publication from socket and process it
          auto maybePublication =
              kvStoreSubSock_.recvThriftObj<thrift::Publication>(serializer);
          if (maybePublication.hasError()) {
            LOG(ERROR) << "Failed to read publication from KvStore SUB socket. "
                       << "Exception: " << maybePublication.error();
            return;
          }

          SYNCHRONIZED(kvStorePublishers_) {
            for (auto& kv : kvStorePublishers_) {
              kv.second.next(maybePublication.value());
            }
          }

          bool isAdjChanged = false;
          // check if any of KeyVal has 'adj' update
          for (auto& kv : maybePublication.value().keyVals) {
            auto& key = kv.first;
            auto& val = kv.second;
            // check if we have any value update.
            // Ttl refreshing won't update any value.
            if (!val.value.hasValue()) {
              continue;
            }

            // "adj:*" key has changed. Update local collection
            if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
              VLOG(3) << "Adj key: " << key << " change received";
              isAdjChanged = true;
              break;
            }
          }

          if (isAdjChanged) {
            // thrift::Publication contains "adj:*" key change.
            // Clean ALL pending promises
            longPollReqs_.withWLock([&](auto& longPollReqs) {
              for (auto& kv : longPollReqs) {
                auto& p = kv.second.first;
                p.setValue(true);
              }
              longPollReqs.clear();
            });
          } else {
            longPollReqs_.withWLock([&](auto& longPollReqs) {
              auto now = getUnixTimeStampMs();
              std::vector<int64_t> reqsToClean;
              for (auto& kv : longPollReqs) {
                auto& clientId = kv.first;
                auto& req = kv.second;

                auto& p = req.first;
                auto& timeStamp = req.second;
                if (now - timeStamp >=
                    Constants::kLongPollReqHoldTime.count()) {
                  LOG(INFO) << "Elapsed time: " << now - timeStamp
                            << " is over hold limit: "
                            << Constants::kLongPollReqHoldTime.count();
                  reqsToClean.emplace_back(clientId);
                  p.setValue(false);
                }
              }

              // cleanup expired requests since no ADJ change observed
              for (auto& clientId : reqsToClean) {
                longPollReqs.erase(clientId);
              }
            });
          }
        });
  });
}

OpenrCtrlHandler::~OpenrCtrlHandler() {
  evl_.removeSocket(fbzmq::RawZmqSocketPtr{*kvStoreSubSock_});
  kvStoreSubSock_.close();

  std::vector<apache::thrift::ServerStreamPublisher<thrift::Publication>>
      publishers;
  // NOTE: We're intentionally creating list of publishers to and then invoke
  // `complete()` on them.
  // Reason => `complete()` returns only when callback `onComplete` associated
  // with publisher returns. Since we acquire lock within `onComplete` callback,
  // we will run into the deadlock if `complete()` is invoked within
  // SYNCHRONIZED block
  SYNCHRONIZED(kvStorePublishers_) {
    for (auto& kv : kvStorePublishers_) {
      publishers.emplace_back(std::move(kv.second));
    }
  }
  LOG(INFO) << "Terminating " << publishers.size()
            << " active KvStore snoop stream(s).";
  for (auto& publisher : publishers) {
    std::move(publisher).complete();
  }

  LOG(INFO) << "Cleanup all pending request(s).";
  longPollReqs_.withWLock([&](auto& longPollReqs) { longPollReqs.clear(); });
}

void
OpenrCtrlHandler::authorizeConnection() {
  auto connContext = getConnectionContext()->getConnectionContext();
  auto peerCommonName = connContext->getPeerCommonName();
  auto peerAddr = connContext->getPeerAddress();

  // We legitely accepts all connections (secure/non-secure) from localhost
  if (peerAddr->isLoopbackAddress()) {
    return;
  }

  if (peerCommonName.empty() || acceptablePeerCommonNames_.empty()) {
    // for now, we will allow non-secure connections, but lets log the event so
    // we know how often this is happening.
    fbzmq::LogSample sample{};

    sample.addString(
        "event",
        peerCommonName.empty() ? "UNENCRYPTED_CTRL_CONNECTION"
                               : "UNRESTRICTED_AUTHORIZATION");
    sample.addString("node_name", nodeName_);
    sample.addString(
        "peer_address", connContext->getPeerAddress()->getAddressStr());
    sample.addString("peer_common_name", peerCommonName);

    zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
        apache::thrift::FRAGILE,
        Constants::kEventLogCategory.toString(),
        {sample.toJson()}));

    LOG(INFO) << "Authorizing request with issues: " << sample.toJson();
    return;
  }

  if (!acceptablePeerCommonNames_.count(peerCommonName)) {
    throw thrift::OpenrError(
        folly::sformat("Peer name {} is unacceptable", peerCommonName));
  }
}

facebook::fb303::cpp2::fb_status
OpenrCtrlHandler::getStatus() {
  return facebook::fb303::cpp2::fb_status::ALIVE;
}

folly::SemiFuture<std::unique_ptr<std::vector<fbzmq::thrift::EventLog>>>
OpenrCtrlHandler::semifuture_getEventLogs() {
  folly::Promise<std::unique_ptr<std::vector<fbzmq::thrift::EventLog>>> p;

  auto eventLogs = zmqMonitorClient_->getLastEventLogs();
  if (eventLogs.hasValue()) {
    p.setValue(std::make_unique<std::vector<fbzmq::thrift::EventLog>>(
        eventLogs.value()));
  } else {
    p.setException(
        thrift::OpenrError(std::string("Fail to retrieve eventlogs")));
  }

  return p.getSemiFuture();
}

void
OpenrCtrlHandler::getCounters(std::map<std::string, int64_t>& _return) {
  FacebookBase2::getCounters(_return);
  for (auto const& kv : zmqMonitorClient_->dumpCounters()) {
    _return.emplace(kv.first, static_cast<int64_t>(kv.second.value));
  }
}

void
OpenrCtrlHandler::getRegexCounters(
    std::map<std::string, int64_t>& _return,
    std::unique_ptr<std::string> regex) {
  // Compile regex
  re2::RE2 compiledRegex(*regex);
  if (not compiledRegex.ok()) {
    return;
  }

  // Get all counters
  std::map<std::string, int64_t> counters;
  getCounters(counters);

  // Filter counters
  for (auto const& kv : counters) {
    if (RE2::PartialMatch(kv.first, compiledRegex)) {
      _return.emplace(kv);
    }
  }
}

void
OpenrCtrlHandler::getSelectedCounters(
    std::map<std::string, int64_t>& _return,
    std::unique_ptr<std::vector<std::string>> keys) {
  // Get all counters
  std::map<std::string, int64_t> counters;
  getCounters(counters);

  // Filter counters
  for (auto const& key : *keys) {
    auto it = counters.find(key);
    if (it != counters.end()) {
      _return.emplace(*it);
    }
  }
}

int64_t
OpenrCtrlHandler::getCounter(std::unique_ptr<std::string> key) {
  auto counter = zmqMonitorClient_->getCounter(*key);
  if (counter.hasValue()) {
    return static_cast<int64_t>(counter->value);
  }
  return 0;
}

void
OpenrCtrlHandler::getMyNodeName(std::string& _return) {
  _return = std::string(nodeName_);
}

void
OpenrCtrlHandler::getOpenrVersion(thrift::OpenrVersions& _openrVersion) {
  _openrVersion.version = Constants::kOpenrVersion;
  _openrVersion.lowestSupportedVersion = Constants::kOpenrSupportedVersion;
}

void
OpenrCtrlHandler::getBuildInfo(thrift::BuildInfo& _buildInfo) {
  _buildInfo = getBuildInfoThrift();
}

//
// PrefixManager APIs
//
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_advertisePrefixes(
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  CHECK(prefixManager_);
  return prefixManager_->advertisePrefixes(std::move(*prefixes))
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_withdrawPrefixes(
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  CHECK(prefixManager_);
  return prefixManager_->withdrawPrefixes(std::move(*prefixes))
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_withdrawPrefixesByType(
    thrift::PrefixType prefixType) {
  CHECK(prefixManager_);
  return prefixManager_->withdrawPrefixesByType(prefixType)
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_syncPrefixesByType(
    thrift::PrefixType prefixType,
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  CHECK(prefixManager_);
  return prefixManager_->syncPrefixesByType(prefixType, std::move(*prefixes))
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
OpenrCtrlHandler::semifuture_getPrefixes() {
  CHECK(prefixManager_);
  return prefixManager_->getPrefixes();
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
OpenrCtrlHandler::semifuture_getPrefixesByType(thrift::PrefixType prefixType) {
  CHECK(prefixManager_);
  return prefixManager_->getPrefixesByType(prefixType);
}

//
// Fib APIs
//
folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
OpenrCtrlHandler::semifuture_getRouteDb() {
  CHECK(fib_);
  return fib_->getRouteDb();
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
OpenrCtrlHandler::semifuture_getUnicastRoutesFiltered(
    std::unique_ptr<std::vector<std::string>> prefixes) {
  CHECK(fib_);
  return fib_->getUnicastRoutes(std::move(*prefixes));
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
OpenrCtrlHandler::semifuture_getUnicastRoutes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::UnicastRoute>>> p;
  CHECK(fib_);
  return fib_->getUnicastRoutes({});
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
OpenrCtrlHandler::semifuture_getMplsRoutes() {
  CHECK(fib_);
  return fib_->getMplsRoutes({});
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
OpenrCtrlHandler::semifuture_getMplsRoutesFiltered(
    std::unique_ptr<std::vector<int32_t>> labels) {
  CHECK(fib_);
  return fib_->getMplsRoutes(std::move(*labels));
}

folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>>
OpenrCtrlHandler::semifuture_getPerfDb() {
  CHECK(fib_);
  return fib_->getPerfDb();
}

//
// Decision APIs
//
folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
OpenrCtrlHandler::semifuture_getRouteDbComputed(
    std::unique_ptr<std::string> nodeName) {
  CHECK(decision_);
  return decision_->getDecisionRouteDb(*nodeName);
}

folly::SemiFuture<std::unique_ptr<thrift::AdjDbs>>
OpenrCtrlHandler::semifuture_getDecisionAdjacencyDbs() {
  CHECK(decision_);
  return decision_->getDecisionAdjacencyDbs();
}

folly::SemiFuture<std::unique_ptr<thrift::PrefixDbs>>
OpenrCtrlHandler::semifuture_getDecisionPrefixDbs() {
  CHECK(decision_);
  return decision_->getDecisionPrefixDbs();
}

//
// KvStore APIs
//
folly::SemiFuture<std::unique_ptr<thrift::AreasConfig>>
OpenrCtrlHandler::semifuture_getAreasConfig() {
  CHECK(kvStore_);
  return kvStore_->getAreasConfig();
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyVals(
    std::unique_ptr<std::vector<std::string>> filterKeys) {
  thrift::KeyGetParams params;
  params.keys = std::move(*filterKeys);

  CHECK(kvStore_);
  return kvStore_->getKvStoreKeyVals(std::move(params));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyValsArea(
    std::unique_ptr<std::vector<std::string>> filterKeys,
    std::unique_ptr<std::string> area) {
  thrift::KeyGetParams params;
  params.keys = std::move(*filterKeys);

  CHECK(kvStore_);
  return kvStore_->getKvStoreKeyVals(std::move(params), std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyValsFiltered(
    std::unique_ptr<thrift::KeyDumpParams> filter) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreKeys(std::move(*filter));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyValsFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreKeys(std::move(*filter), std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreHashFiltered(
    std::unique_ptr<thrift::KeyDumpParams> filter) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreHashes(std::move(*filter));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreHashFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreHashes(std::move(*filter), std::move(*area));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->setKvStoreKeyVals(std::move(*setParams), std::move(*area));
}

folly::SemiFuture<bool>
OpenrCtrlHandler::semifuture_longPollKvStoreAdj(
    std::unique_ptr<thrift::KeyVals> snapshot) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();

  auto timeStamp = getUnixTimeStampMs();
  auto requestId = pendingRequestId_++;

  thrift::KeyDumpParams params;

  // build thrift::KeyVals with "adj:" key ONLY
  // to ensure KvStore ONLY compare "adj:" key
  thrift::KeyVals adjKeyVals;
  for (auto& kv : *snapshot) {
    if (kv.first.find(Constants::kAdjDbMarker.toString()) == 0) {
      adjKeyVals.emplace(kv.first, kv.second);
    }
  }

  // Only care about "adj:" key
  params.prefix = Constants::kAdjDbMarker;
  // Only dump difference between KvStore and client snapshot
  params.keyValHashes = std::move(adjKeyVals);

  // Explicitly do SYNC call to get HASH_DUMP from KvStore
  std::unique_ptr<thrift::Publication> thriftPub{nullptr};
  try {
    thriftPub = semifuture_getKvStoreKeyValsFiltered(
                    std::make_unique<thrift::KeyDumpParams>(params))
                    .get();
  } catch (std::exception const& ex) {
    p.setException(thrift::OpenrError(ex.what()));
    return sf;
  }

  if (thriftPub->keyVals.size() > 0) {
    VLOG(3) << "AdjKey has been added/modified. Notify immediately";
    p.setValue(true);
  } else if (
      thriftPub->tobeUpdatedKeys.hasValue() &&
      thriftPub->tobeUpdatedKeys.value().size() > 0) {
    VLOG(3) << "AdjKey has been deleted/expired. Notify immediately";
    p.setValue(true);
  } else {
    // Client provided data is consistent with KvStore.
    // Store req for future processing when there is publication
    // from KvStore.
    VLOG(3) << "No adj change detected. Store req as pending request";
    longPollReqs_.withWLock([&](auto& longPollReq) {
      longPollReq.emplace(requestId, std::make_pair(std::move(p), timeStamp));
    });
  }
  return sf;
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_processKvStoreDualMessage(
    std::unique_ptr<thrift::DualMessages> messages,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->processKvStoreDualMessage(
      std::move(*messages), std::move(*area));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_updateFloodTopologyChild(
    std::unique_ptr<thrift::FloodTopoSetParams> params,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->updateFloodTopologyChild(
      std::move(*params), std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::SptInfos>>
OpenrCtrlHandler::semifuture_getSpanningTreeInfos(
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->getSpanningTreeInfos(std::move(*area));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_addUpdateKvStorePeers(
    std::unique_ptr<thrift::PeersMap> peers,
    std::unique_ptr<std::string> area) {
  thrift::PeerAddParams params;
  params.peers = std::move(*peers);

  CHECK(kvStore_);
  return kvStore_->addUpdateKvStorePeers(std::move(params), std::move(*area));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_deleteKvStorePeers(
    std::unique_ptr<std::vector<std::string>> peerNames,
    std::unique_ptr<std::string> area) {
  thrift::PeerDelParams params;
  params.peerNames = std::move(*peerNames);

  CHECK(kvStore_);
  return kvStore_->deleteKvStorePeers(std::move(params), std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
OpenrCtrlHandler::semifuture_getKvStorePeers() {
  CHECK(kvStore_);
  return kvStore_->getKvStorePeers();
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
OpenrCtrlHandler::semifuture_getKvStorePeersArea(
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->getKvStorePeers(std::move(*area));
}

apache::thrift::ServerStream<thrift::Publication>
OpenrCtrlHandler::subscribeKvStore() {
  // Get new client-ID (monotonically increasing)
  auto clientToken = publisherToken_++;

  auto streamAndPublisher =
      apache::thrift::ServerStream<thrift::Publication>::createPublisher(
          [this, clientToken]() {
            SYNCHRONIZED(kvStorePublishers_) {
              if (kvStorePublishers_.erase(clientToken)) {
                LOG(INFO) << "KvStore snoop stream-" << clientToken
                          << " ended.";
              } else {
                LOG(ERROR) << "Can't remove unknown KvStore snoop stream-"
                           << clientToken;
              }
            }
          });

  SYNCHRONIZED(kvStorePublishers_) {
    assert(kvStorePublishers_.count(clientToken) == 0);
    LOG(INFO) << "KvStore snoop stream-" << clientToken << " started.";
    kvStorePublishers_.emplace(
        clientToken, std::move(streamAndPublisher.second));
  }
  return std::move(streamAndPublisher.first);
}

folly::SemiFuture<apache::thrift::ResponseAndServerStream<
    thrift::Publication,
    thrift::Publication>>
OpenrCtrlHandler::semifuture_subscribeAndGetKvStore() {
  return semifuture_getKvStoreKeyValsFiltered(
             std::make_unique<thrift::KeyDumpParams>())
      .defer(
          [stream = subscribeKvStore()](
              folly::Try<std::unique_ptr<thrift::Publication>>&& pub) mutable {
            pub.throwIfFailed();
            return apache::thrift::ResponseAndServerStream<
                thrift::Publication,
                thrift::Publication>{std::move(*pub.value()),
                                     std::move(stream)};
          });
}

//
// LinkMonitor APIs
//
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setNodeOverload() {
  CHECK(linkMonitor_);
  return linkMonitor_->setNodeOverload(true);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetNodeOverload() {
  CHECK(linkMonitor_);
  return linkMonitor_->setNodeOverload(false);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setInterfaceOverload(
    std::unique_ptr<std::string> interfaceName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setInterfaceOverload(std::move(*interfaceName), true);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetInterfaceOverload(
    std::unique_ptr<std::string> interfaceName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setInterfaceOverload(std::move(*interfaceName), false);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setInterfaceMetric(
    std::unique_ptr<std::string> interfaceName, int32_t overrideMetric) {
  CHECK(linkMonitor_);
  return linkMonitor_->setLinkMetric(std::move(*interfaceName), overrideMetric);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetInterfaceMetric(
    std::unique_ptr<std::string> interfaceName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setLinkMetric(std::move(*interfaceName), std::nullopt);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setAdjacencyMetric(
    std::unique_ptr<std::string> interfaceName,
    std::unique_ptr<std::string> adjNodeName,
    int32_t overrideMetric) {
  CHECK(linkMonitor_);
  return linkMonitor_->setAdjacencyMetric(
      std::move(*interfaceName), std::move(*adjNodeName), overrideMetric);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetAdjacencyMetric(
    std::unique_ptr<std::string> interfaceName,
    std::unique_ptr<std::string> adjNodeName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setAdjacencyMetric(
      std::move(*interfaceName), std::move(*adjNodeName), std::nullopt);
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
OpenrCtrlHandler::semifuture_getInterfaces() {
  CHECK(linkMonitor_);
  return linkMonitor_->getInterfaces();
}

folly::SemiFuture<std::unique_ptr<thrift::AdjacencyDatabase>>
OpenrCtrlHandler::semifuture_getLinkMonitorAdjacencies() {
  CHECK(linkMonitor_);
  return linkMonitor_->getLinkMonitorAdjacencies();
}

//
// ConfigStore API
//
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setConfigKey(
    std::unique_ptr<std::string> key, std::unique_ptr<std::string> value) {
  CHECK(configStore_);
  return configStore_->store(std::move(*key), std::move(*value));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_eraseConfigKey(std::unique_ptr<std::string> key) {
  CHECK(configStore_);
  auto sf = configStore_->erase(std::move(*key));
  return std::move(sf).defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<std::unique_ptr<std::string>>
OpenrCtrlHandler::semifuture_getConfigKey(std::unique_ptr<std::string> key) {
  CHECK(configStore_);
  auto sf = configStore_->load(std::move(*key));
  return std::move(sf).defer(
      [](folly::Try<std::optional<std::string>>&& val) mutable {
        if (val.hasException() or not val->has_value()) {
          throw thrift::OpenrError("key doesn't exists");
        }
        return std::make_unique<std::string>(std::move(val).value().value());
      });
}

} // namespace openr
