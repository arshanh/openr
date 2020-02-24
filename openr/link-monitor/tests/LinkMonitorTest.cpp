/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkSystemHandler.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/platform/PlatformPublisher.h>
#include <openr/prefix-manager/PrefixManager.h>

using namespace std;
using namespace openr;

using apache::thrift::FRAGILE;
using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;
using ::testing::InSequence;

// node-1 connects node-2 via interface iface_2_1 and iface_2_2, node-3 via
// interface iface_3_1
namespace {

re2::RE2::Options regexOpts;

const auto peerSpec_2_1 = createPeerSpec(
    "tcp://[fe80::2%iface_2_1]:10001",
    "tcp://[fe80::2%iface_2_1]:10002",
    false);

const auto peerSpec_2_2 = createPeerSpec(
    "tcp://[fe80::2%iface_2_2]:10001",
    "tcp://[fe80::2%iface_2_2]:10002",
    false);

const auto peerSpec_3_1 = createPeerSpec(
    "tcp://[fe80::3%iface_3_1]:10001",
    "tcp://[fe80::3%iface_3_1]:10002",
    false);

const auto nb2 = createSparkNeighbor(
    "domain",
    "node-2",
    0, /* hold time */
    toBinaryAddress(folly::IPAddress("192.168.0.2")),
    toBinaryAddress(folly::IPAddress("fe80::2")),
    10001,
    10002,
    "" /* ifName */);

const auto nb3 = createSparkNeighbor(
    "domain",
    "node-3",
    0, /* hold time */
    toBinaryAddress(folly::IPAddress("192.168.0.3")),
    toBinaryAddress(folly::IPAddress("fe80::3")),
    10001,
    10002,
    "" /* ifName */);

const uint64_t kTimestamp{1000000};
const uint64_t kNodeLabel{0};

const auto adj_2_1 = createThriftAdjacency(
    "node-2",
    "iface_2_1",
    "fe80::2",
    "192.168.0.2",
    1 /* metric */,
    1 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto adj_2_2 = createThriftAdjacency(
    "node-2",
    "iface_2_2",
    "fe80::2",
    "192.168.0.2",
    1 /* metric */,
    2 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto adj_3_1 = createThriftAdjacency(
    "node-3",
    "iface_3_1",
    "fe80::3",
    "192.168.0.3",
    1 /* metric */,
    1 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto staticPrefix1 = toIpPrefix("fc00:face:b00c::/64");
const auto staticPrefix2 = toIpPrefix("fc00:cafe:babe::/64");

thrift::SparkNeighborEvent
createNeighborEvent(
    thrift::SparkNeighborEventType eventType,
    const std::string& ifName,
    const thrift::SparkNeighbor& neighbor,
    int64_t rttUs,
    int32_t label,
    const std::string& area = std::string{
        openr::thrift::KvStore_constants::kDefaultArea()}) {
  return createSparkNeighborEvent(
      eventType, ifName, neighbor, rttUs, label, false, area);
}

thrift::AdjacencyDatabase
createAdjDatabase(
    const std::string& thisNodeName,
    const std::vector<thrift::Adjacency>& adjacencies,
    int32_t nodeLabel,
    const std::string& area =
        openr::thrift::KvStore_constants::kDefaultArea()) {
  return createAdjDb(thisNodeName, adjacencies, nodeLabel, false, area);
}

void
printAdjDb(const thrift::AdjacencyDatabase& adjDb) {
  LOG(INFO) << "Node: " << adjDb.thisNodeName
            << ", Overloaded: " << adjDb.isOverloaded
            << ", Label: " << adjDb.nodeLabel
            << ", area: " << adjDb.area.value();
  for (auto const& adj : adjDb.adjacencies) {
    LOG(INFO) << "  " << adj.otherNodeName << "@" << adj.ifName
              << ", metric: " << adj.metric << ", label: " << adj.adjLabel
              << ", overloaded: " << adj.isOverloaded << ", rtt: " << adj.rtt
              << ", ts: " << adj.timestamp << ", " << toString(adj.nextHopV4)
              << ", " << toString(adj.nextHopV6);
  }
}

const std::string kTestVethNamePrefix = "vethLMTest";
const std::vector<uint64_t> kTestVethIfIndex = {1240, 1241, 1242};
const std::string kConfigStorePath = "/tmp/lm_ut_config_store.bin";
} // namespace

class LinkMonitorTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {}

  virtual void
  SetUp(
      std::unordered_set<std::string> areas,
      std::chrono::milliseconds flapInitalBackoff =
          std::chrono::milliseconds(1),
      std::chrono::milliseconds flapMaxBackoff = std::chrono::milliseconds(8)) {
    // Cleanup any existing config file on disk
    system(folly::sformat("rm -rf {}", kConfigStorePath).c_str());

    mockNlHandler = std::make_shared<MockNetlinkSystemHandler>(
        context, "inproc://platform-pub-url");

    server = make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockNlHandler);

    systemThriftThread.start(server);
    port = systemThriftThread.getAddress()->getPort();

    // spin up a config store
    configStore = std::make_unique<PersistentStore>(
        "1",
        kConfigStorePath,
        context,
        Constants::kPersistentStoreInitialBackoff,
        Constants::kPersistentStoreMaxBackoff,
        true /* dryrun */);

    configStoreThread = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "ConfigStore thread starting";
      configStore->run();
      LOG(INFO) << "ConfigStore thread finishing";
    });
    configStore->waitUntilRunning();

    // spin up a kvstore
    kvStoreWrapper = std::make_shared<KvStoreWrapper>(
        context,
        "test_store1",
        std::chrono::seconds(1) /* db sync interval */,
        std::chrono::seconds(600) /* counter submit interval */,
        std::unordered_map<std::string, thrift::PeerSpec>{},
        std::nullopt,
        std::nullopt,
        Constants::kTtlDecrement,
        false,
        false,
        areas);
    kvStoreWrapper->run();
    LOG(INFO) << "The test KV store is running";

    // create prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        "node-1",
        prefixUpdatesQueue.getReader(),
        configStore.get(),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        MonitorSubmitUrl{"inproc://monitor_submit"},
        PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
        false /* create IP prefix keys */,
        false,
        std::chrono::seconds(0),
        Constants::kKvStoreDbTtl,
        context);
    prefixManagerThread = std::make_unique<std::thread>([this] {
      LOG(INFO) << "prefix manager starting";
      prefixManager->run();
      LOG(INFO) << "prefix manager stopped";
    });

    regexOpts.set_case_sensitive(false);
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
    includeRegexList->Add("iface.*", &regexErr);
    includeRegexList->Compile();

    std::unique_ptr<re2::RE2::Set> excludeRegexList;

    auto redistRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    redistRegexList->Add("loopback", &regexErr);
    redistRegexList->Compile();

    // start a link monitor
    linkMonitor = make_shared<LinkMonitor>(
        context,
        "node-1",
        port, /* thrift service port */
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        std::move(excludeRegexList),
        std::move(redistRegexList), // redistribute interface name
        std::vector<thrift::IpPrefix>{staticPrefix1, staticPrefix2},
        false /* useRttMetric */,
        false /* enable perf measurement */,
        true /* enable v4 */,
        true /* enable segment routing */,
        false /* prefix type mpls */,
        false /* prefix fwd algo KSP2_ED_ECMP */,
        AdjacencyDbMarker{"adj:"},
        interfaceUpdatesQueue,
        neighborUpdatesQueue.getReader(),
        MonitorSubmitUrl{"inproc://monitor-rep"},
        configStore.get(),
        false,
        prefixUpdatesQueue,
        PlatformPublisherUrl{"inproc://platform-pub-url"},
        std::chrono::seconds(1),
        // link flap backoffs, set low to keep UT runtime low
        flapInitalBackoff,
        flapMaxBackoff,
        Constants::kKvStoreDbTtl,
        areas);

    linkMonitorThread = std::make_unique<std::thread>([this]() {
      folly::setThreadName("LinkMonitor");
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitor->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitor->waitUntilRunning();
  }

  void
  TearDown() override {
    LOG(INFO) << "LinkMonitor test/basic operations is done";

    interfaceUpdatesQueue.close();
    neighborUpdatesQueue.close();
    prefixUpdatesQueue.close();

    LOG(INFO) << "Stopping the LinkMonitor thread";
    linkMonitor->stop();
    linkMonitorThread->join();
    linkMonitor.reset();
    LOG(INFO) << "LinkMonitor thread got stopped";

    LOG(INFO) << "Stopping prefix manager thread";
    prefixManager->stop();
    prefixManagerThread->join();

    // Erase data from config store
    LOG(INFO) << "Erasing data from config store";
    configStore->erase("link-monitor-config").get();

    // stop config store
    LOG(INFO) << "Stopping config store";
    configStore->stop();
    configStoreThread->join();

    // stop the kvStore
    LOG(INFO) << "Stopping KvStoreWrapper";
    kvStoreWrapper->stop();
    LOG(INFO) << "KvStoreWrapper got stopped";

    // stop mocked nl platform
    LOG(INFO) << "Stopping mocked thrift handlers";
    mockNlHandler->stop();
    systemThriftThread.stop();
    LOG(INFO) << "Mocked thrift handlers got stopped";
  }

  // Receive and process interface updates from the update queue
  void
  recvAndReplyIfUpdate() {
    auto ifDb = interfaceUpdatesReader.get();
    ASSERT_TRUE(ifDb.hasValue());
    sparkIfDb = std::move(ifDb.value().interfaces);
    LOG(INFO) << "----------- Interface Updates ----------";
    for (const auto& kv : sparkIfDb) {
      LOG(INFO) << "  Name=" << kv.first << ", Status=" << kv.second.isUp
                << ", IfIndex=" << kv.second.ifIndex
                << ", networks=" << kv.second.networks.size();
    }
  }

  // collate Interfaces into a map so UT can run some checks
  using CollatedIfData = struct {
    int isUpCount{0};
    int isDownCount{0};
    int v4AddrsMaxCount{0};
    int v4AddrsMinCount{0};
    int v6LinkLocalAddrsMaxCount{0};
    int v6LinkLocalAddrsMinCount{0};
  };
  using CollatedIfUpdates = std::map<string, CollatedIfData>;

  CollatedIfUpdates
  collateIfUpdates(
      const std::map<std::string, thrift::InterfaceInfo>& interfaces) {
    CollatedIfUpdates res;
    for (const auto& kv : interfaces) {
      const auto& ifName = kv.first;
      if (kv.second.isUp) {
        res[ifName].isUpCount++;
      } else {
        res[ifName].isDownCount++;
      }
      int v4AddrsCount = 0;
      int v6LinkLocalAddrsCount = 0;
      for (const auto& network : kv.second.networks) {
        const auto& ipNetwork = toIPNetwork(network);
        if (ipNetwork.first.isV4()) {
          v4AddrsCount++;
        } else if (ipNetwork.first.isV6() && ipNetwork.first.isLinkLocal()) {
          v6LinkLocalAddrsCount++;
        }
      }

      res[ifName].v4AddrsMaxCount =
          max(v4AddrsCount, res[ifName].v4AddrsMaxCount);

      res[ifName].v4AddrsMinCount =
          min(v4AddrsCount, res[ifName].v4AddrsMinCount);

      res[ifName].v6LinkLocalAddrsMaxCount =
          max(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMaxCount);

      res[ifName].v6LinkLocalAddrsMinCount =
          min(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMinCount);
    }
    return res;
  }

  folly::Optional<thrift::Value>
  getPublicationValueForKey(
      std::string const& key,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea(),
      std::chrono::seconds timeout = std::chrono::seconds(10)) {
    LOG(INFO) << "Waiting to receive publication for key " << key << " area "
              << area;
    auto pub = kvStoreWrapper->recvPublication(timeout);
    EXPECT_TRUE(pub.area.hasValue());
    if (pub.area.value() != area) {
      return folly::none;
    }

    VLOG(1) << "Received publication with keys in area " << pub.area.value();
    for (auto const& kv : pub.keyVals) {
      VLOG(1) << "  " << kv.first;
    }

    auto kv = pub.keyVals.find(key);
    if (kv == pub.keyVals.end() or !kv->second.value) {
      return folly::none;
    }

    return kv->second;
  }

  // recv publicatons from kv store until we get what we were
  // expecting for a given key
  void
  checkNextAdjPub(
      std::string const& key,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea()) {
    CHECK(!expectedAdjDbs.empty());

    printAdjDb(expectedAdjDbs.front());

    while (true) {
      folly::Optional<thrift::Value> value;
      try {
        value = getPublicationValueForKey(key, area);
        if (not value.hasValue()) {
          continue;
        }
      } catch (std::exception const& e) {
        LOG(ERROR) << "Exception: " << folly::exceptionStr(e);
        EXPECT_TRUE(false);
        return;
      }

      auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
          value->value.value(), serializer);
      printAdjDb(adjDb);

      // we can not know what the nodeLabel will be
      adjDb.nodeLabel = kNodeLabel;
      // nor the timestamp, so we override with our predefinded const values
      for (auto& adj : adjDb.adjacencies) {
        adj.timestamp = kTimestamp;
      }
      auto adjDbstr = fbzmq::util::writeThriftObjStr(adjDb, serializer);
      auto expectedAdjDbstr =
          fbzmq::util::writeThriftObjStr(expectedAdjDbs.front(), serializer);
      if (adjDbstr == expectedAdjDbstr) {
        expectedAdjDbs.pop();
        return;
      }
    }
  }

  // kvstore shall reveive cmd to add/del peers
  void
  checkPeerDump(
      std::string const& nodeName,
      thrift::PeerSpec peerSpec,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea()) {
    auto const peers = kvStoreWrapper->getPeers(area);
    EXPECT_EQ(peers.count(nodeName), 1);
    if (!peers.count(nodeName)) {
      return;
    }
    EXPECT_EQ(peers.at(nodeName).pubUrl, peerSpec.pubUrl);
    EXPECT_EQ(peers.at(nodeName).cmdUrl, peerSpec.cmdUrl);
  }

  std::unordered_set<thrift::IpPrefix>
  getNextPrefixDb(
      std::string const& key,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea()) {
    while (true) {
      auto value =
          getPublicationValueForKey(key, area, std::chrono::seconds(3));
      if (not value.hasValue()) {
        continue;
      }

      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value->value.value(), serializer);
      std::unordered_set<thrift::IpPrefix> prefixes;
      for (auto const& prefixEntry : prefixDb.prefixEntries) {
        prefixes.insert(prefixEntry.prefix);
      }
      return prefixes;
    }
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread systemThriftThread;

  fbzmq::Context context{};

  messaging::ReplicateQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue;
  messaging::ReplicateQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue;
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue;
  messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesReader{
      interfaceUpdatesQueue.getReader()};

  unique_ptr<PersistentStore> configStore;
  unique_ptr<std::thread> configStoreThread;

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<LinkMonitor> linkMonitor;
  std::unique_ptr<std::thread> linkMonitorThread;

  std::unique_ptr<PrefixManager> prefixManager;
  std::unique_ptr<std::thread> prefixManagerThread;

  std::shared_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<MockNetlinkSystemHandler> mockNlHandler;

  std::queue<thrift::AdjacencyDatabase> expectedAdjDbs;
  std::map<std::string, thrift::InterfaceInfo> sparkIfDb;
};

// Start LinkMonitor and ensure empty adjacency database and prefixes are
// received upon initial hold-timeout expiry
TEST_F(LinkMonitorTestFixture, NoNeighborEvent) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  // Verify that we receive empty adjacency database
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel));
  checkNextAdjPub("adj:node-1");
}

// receive neighbor up/down events from "spark"
// form peer connections and inform KvStore of adjacencies
TEST_F(LinkMonitorTestFixture, BasicOperation) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  const int linkMetric = 123;
  const int adjMetric = 100;

  {
    InSequence dummy;

    {
      // create an interface
      mockNlHandler->sendLinkEvent("iface_2_1", 100, true);
      recvAndReplyIfUpdate();
      thrift::InterfaceDatabase intfDb(
          FRAGILE,
          "node-1",
          {
              {
                  "iface_2_1",
                  thrift::InterfaceInfo(
                      FRAGILE,
                      true, // isUp
                      100, // ifIndex
                      {}, // v4Addrs: TO BE DEPRECATED SOON
                      {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
                      {} // networks
                      ),
              },
          },
          thrift::PerfEvents());
      intfDb.perfEvents = folly::none;
    }

    {
      // expect neighbor up first
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit set
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;
      adj_2_1_modified.isOverloaded = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;
      adj_2_1_modified.isOverloaded = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value unset
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // set adjacency metric, this should override the link metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = adjMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // unset adjacency metric, this will remove the override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // link-monitor and config-store is restarted but state will be
      // retained. expect neighbor up with overrides
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1");
  }

  // testing for set/unset overload bit and custom metric values
  // 1. set overload bit
  // 2. set custom metric on link: No adjacency advertisement
  // 3. set link overload
  // 4. unset overload bit
  // 5. unset link overload bit - custom metric value should be in effect
  // 6. unset custom metric on link
  // 7: set overload bit
  // 8: custom metric on link
  // 9. Set overload bit and link metric value
  // 10. set and unset adjacency metric
  {
    const std::string interfaceName = "iface_2_1";
    const std::string nodeName = "node-2";

    LOG(INFO) << "Testing set node overload command!";
    auto ret = linkMonitor->setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    auto res = linkMonitor->getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(res->isOverloaded);
    EXPECT_EQ(1, res->interfaceDetails.size());
    EXPECT_FALSE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_FALSE(
        res->interfaceDetails.at(interfaceName).metricOverride.hasValue());

    LOG(INFO) << "Testing set link metric command!";
    ret = linkMonitor->setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link overload command!";
    ret = linkMonitor->setInterfaceOverload(interfaceName, true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(res->isOverloaded);
    EXPECT_TRUE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails.at(interfaceName).metricOverride.value());

    LOG(INFO) << "Testing unset node overload command!";
    ret = linkMonitor->setNodeOverload(false).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->getInterfaces().get();
    EXPECT_FALSE(res->isOverloaded);
    EXPECT_TRUE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails.at(interfaceName).metricOverride.value());

    LOG(INFO) << "Testing unset link overload command!";
    ret = linkMonitor->setInterfaceOverload(interfaceName, false).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset link metric command!";
    ret = linkMonitor->setLinkMetric(interfaceName, std::nullopt).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    res = linkMonitor->getInterfaces().get();
    EXPECT_FALSE(res->isOverloaded);
    EXPECT_FALSE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_FALSE(
        res->interfaceDetails.at(interfaceName).metricOverride.hasValue());

    LOG(INFO) << "Testing set node overload command( AGAIN )!";
    ret = linkMonitor->setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link metric command( AGAIN )!";
    ret = linkMonitor->setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set adj metric command!";
    ret = linkMonitor->setAdjacencyMetric(interfaceName, nodeName, adjMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset adj metric command!";
    ret = linkMonitor->setAdjacencyMetric(interfaceName, nodeName, std::nullopt)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
  }

  // 11. neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1");
  }

  // mock "restarting" link monitor with existing config store

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Compile();
  std::unique_ptr<re2::RE2::Set> excludeRegexList;
  std::unique_ptr<re2::RE2::Set> redistRegexList;

  // stop linkMonitor
  LOG(INFO) << "Mock restarting link monitor!";

  neighborUpdatesQueue.close();
  linkMonitor->stop();
  linkMonitorThread->join();
  linkMonitor.reset();

  // Create new neighbor update queue. Previous one is closed
  neighborUpdatesQueue =
      messaging::ReplicateQueue<thrift::SparkNeighborEvent>();

  linkMonitor = make_shared<LinkMonitor>(
      context,
      "node-1",
      port, // platform pub port
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      std::move(includeRegexList),
      std::move(excludeRegexList),
      // redistribute interface names
      std::move(redistRegexList),
      std::vector<thrift::IpPrefix>{}, // static prefixes
      false /* useRttMetric */,
      false /* enable perf measurement */,
      false /* enable v4 */,
      true /* enable segment routing */,
      false /* prefix type MPLS */,
      false /* prefix fwd algo KSP2_ED_ECMP */,
      AdjacencyDbMarker{"adj:"},
      interfaceUpdatesQueue,
      neighborUpdatesQueue.getReader(),
      MonitorSubmitUrl{"inproc://monitor-rep2"},
      configStore.get(),
      false,
      prefixUpdatesQueue,
      PlatformPublisherUrl{"inproc://platform-pub-url2"},
      std::chrono::seconds(1),
      // link flap backoffs, set low to keep UT runtime low
      std::chrono::milliseconds(1),
      std::chrono::milliseconds(8),
      Constants::kKvStoreDbTtl);

  linkMonitorThread = std::make_unique<std::thread>([this]() {
    LOG(INFO) << "LinkMonitor thread starting";
    linkMonitor->run();
    LOG(INFO) << "LinkMonitor thread finishing";
  });
  linkMonitor->waitUntilRunning();

  // 12. neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor up event!";
    checkNextAdjPub("adj:node-1");
  }

  // 13. neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1");
  }
}

// Test throttling
TEST_F(LinkMonitorTestFixture, Throttle) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  {
    InSequence dummy;

    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // before throttled function kicks in

  // another neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // neighbor 3 down immediately
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
}

// parallel adjacencies between two nodes via different interfaces
TEST_F(LinkMonitorTestFixture, ParallelAdj) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  {
    InSequence dummy;

    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor up on another interface
    // still use iface_2_1 because it's the "min" and will not call addPeers

    {
      // note: adj_2_2 is hashed ahead of adj_2_1
      auto adjDb = createAdjDatabase("node-1", {adj_2_2, adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor down
    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_2}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor up on another interface
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        2 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_2.otherNodeName, peerSpec_2_2);
}

// Verify neighbor-restarting event (including parallel case)
TEST_F(LinkMonitorTestFixture, NeighborRestart) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});

  /* Single link case */
  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restart
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // peers should be gone
  EXPECT_TRUE(kvStoreWrapper->getPeers().empty());

  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  /* Parallel case */
  // neighbor up on another interface
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        2 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restarting on iface_2_1
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_2.otherNodeName, peerSpec_2_2);

  // neighbor restarted
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restarting iface_2_2
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restarting iface_2_1
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // peers should be gone
  EXPECT_TRUE(kvStoreWrapper->getPeers().empty());
}

TEST_F(LinkMonitorTestFixture, DampenLinkFlaps) {
  SetUp(
      {openr::thrift::KvStore_constants::kDefaultArea()},
      std::chrono::milliseconds(2000),
      std::chrono::milliseconds(4000));
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Add("iface.*", &regexErr);
  includeRegexList->Compile();

  std::unique_ptr<re2::RE2::Set> excludeRegexList;

  auto redistRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  redistRegexList->Add("loopback", &regexErr);
  redistRegexList->Compile();

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Updates will be coalesced by throttling

  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }

  VLOG(2) << "*** start link flaps ***";

  // Bringing up the interface
  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate(); // Updates will be coalesced by throttling

  // at this point, both interface should have no backoff
  auto links = linkMonitor->getInterfaces().get();
  EXPECT_EQ(2, links->interfaceDetails.size());
  for (const auto& ifName : ifNames) {
    EXPECT_FALSE(
        links->interfaceDetails.at(ifName).linkFlapBackOffMs.hasValue());
  }

  VLOG(2) << "*** bring down 2 interfaces ***";
  auto linkDownTs = std::chrono::steady_clock::now();
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms

  // at this point, both interface should have backoff=~2s
  {
    // we expect all interfaces are down at this point because backoff hasn't
    // been cleared up yet
    auto res = collateIfUpdates(sparkIfDb);
    auto links1 = linkMonitor->getInterfaces().get();
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links1->interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_GE(
          links1->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 0);
      EXPECT_LE(
          links1->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 2000);
    }
  }

  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate(); // No-op throttling update
  recvAndReplyIfUpdate(); // linkX - UP update
  recvAndReplyIfUpdate(); // linkY - UP update
  auto linkUpTs = std::chrono::steady_clock::now();

  // Elapsed total time between interface down->up must be greater than
  // backoff time of 2s. Also ensure upper bound
  EXPECT_LE(std::chrono::seconds(2), linkUpTs - linkDownTs);
  EXPECT_GE(std::chrono::seconds(3), linkUpTs - linkDownTs);

  // Expect interface update after backoff time passes with state=UP
  {
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      // Both report UP twice (link up and addr add)
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      // We get the link local notification
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }
  VLOG(2) << "*** end link flaps ***";

  // Bringing down the interfaces
  VLOG(2) << "*** bring down 2 interfaces ***";
  linkDownTs = std::chrono::steady_clock::now();
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms

  // at this point, both interface should have backoff=~4sec
  {
    // expect sparkIfDb to have two interfaces DOWN
    auto res = collateIfUpdates(sparkIfDb);
    auto links2 = linkMonitor->getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links2->interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_GE(
          links2->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 2000);
      EXPECT_LE(
          links2->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 4000);
    }
  }

  // Bringing up the interfaces
  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate(); // No-op throttling update
  recvAndReplyIfUpdate(); // linkX - UP update
  recvAndReplyIfUpdate(); // linkY - UP update
  linkUpTs = std::chrono::steady_clock::now();

  // Elapsed total time between interface down->up must be greater than
  // backoff time of 4s. Also ensure upper bound
  EXPECT_LE(std::chrono::seconds(4), linkUpTs - linkDownTs);
  EXPECT_GE(std::chrono::seconds(5), linkUpTs - linkDownTs);

  // at this point, both interface should have backoff back to init value
  {
    // expect sparkIfDb to have two interfaces UP
    // Make sure to wait long enough to clear out backoff timers
    auto res = collateIfUpdates(sparkIfDb);
    auto links3 = linkMonitor->getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links3->interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_FALSE(
          links3->interfaceDetails.at(ifName).linkFlapBackOffMs.hasValue());
    }
  }
}

// Test Interface events to Spark
TEST_F(LinkMonitorTestFixture, verifyLinkEventSubscription) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate();
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate();

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate a link up event publication
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate();
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate();

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);

      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });
}

TEST_F(LinkMonitorTestFixture, verifyAddrEventSubscription) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  mockNlHandler->sendAddrEvent(linkX, "10.0.0.1/31", true /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "10.0.0.2/31", true /* is valid */);
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  // Emulate add address event: v6 while interfaces are in UP state. Both
  // v4 and v6 addresses should be reported.
  mockNlHandler->sendAddrEvent(linkX, "fe80::1/128", true /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "fe80::2/128", true /* is valid */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(1, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(1, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate delete address event: v4
  mockNlHandler->sendAddrEvent(linkX, "10.0.0.1/31", false /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "10.0.0.2/31", false /* is valid */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(1, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate delete address event: v6
  mockNlHandler->sendAddrEvent(linkX, "fe80::1/128", false /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "fe80::2/128", false /* is valid */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);

      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);

      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate address and link events coming in out of order
  const std::string linkZ = kTestVethNamePrefix + "Z";

  // Addr event comes in first
  mockNlHandler->sendAddrEvent(linkZ, "fe80::3/128", true /* is valid */);
  // Link event comes in later
  mockNlHandler->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex[2] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 3 interfaces
    EXPECT_EQ(3, res.size());
    EXPECT_EQ(1, res.at(linkZ).isUpCount);
    EXPECT_EQ(0, res.at(linkZ).isDownCount);
    EXPECT_EQ(0, res.at(linkZ).v4AddrsMaxCount);
    EXPECT_EQ(0, res.at(linkZ).v4AddrsMinCount);
    EXPECT_EQ(1, res.at(linkZ).v6LinkLocalAddrsMaxCount);
    EXPECT_EQ(0, res.at(linkZ).v6LinkLocalAddrsMinCount);
  });

  // Link goes down
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex[2] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling
  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 3 interfaces
    EXPECT_EQ(3, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }
}

// Test getting unique nodeLabels
TEST_F(LinkMonitorTestFixture, NodeLabelAlloc) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  size_t kNumNodesToTest = 10;

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Compile();
  std::unique_ptr<re2::RE2::Set> excludeRegexList;
  std::unique_ptr<re2::RE2::Set> redistRegexList;

  // spin up kNumNodesToTest - 1 new link monitors. 1 is spun up in setup()
  std::vector<std::unique_ptr<LinkMonitor>> linkMonitors;
  std::vector<std::unique_ptr<std::thread>> linkMonitorThreads;
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    auto lm = std::make_unique<LinkMonitor>(
        context,
        folly::sformat("lm{}", i + 1),
        0, // platform pub port
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        std::move(excludeRegexList),
        std::move(redistRegexList),
        std::vector<thrift::IpPrefix>(),
        false /* useRttMetric */,
        false /* enable perf measurement */,
        false /* enable v4 */,
        true /* enable segment routing */,
        false /* prefix type MPLS */,
        false /* prefix fwd algo KSP2_ED_ECMP */,
        AdjacencyDbMarker{"adj:"},
        interfaceUpdatesQueue,
        neighborUpdatesQueue.getReader(),
        MonitorSubmitUrl{"inproc://monitor-rep"},
        configStore.get(),
        false,
        prefixUpdatesQueue,
        PlatformPublisherUrl{"inproc://platform-pub-url"},
        std::chrono::seconds(1),
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8),
        Constants::kKvStoreDbTtl);
    linkMonitors.emplace_back(std::move(lm));

    auto lmThread = std::make_unique<std::thread>([&linkMonitors]() {
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitors.back()->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitorThreads.emplace_back(std::move(lmThread));
    linkMonitors.back()->waitUntilRunning();
  }

  // map of nodeId to value allocated
  std::unordered_map<std::string, int32_t> nodeLabels;

  // recv kv store publications until we have valid labels from each node
  while (nodeLabels.size() < kNumNodesToTest) {
    auto pub = kvStoreWrapper->recvPublication(std::chrono::seconds(10));
    for (auto const& kv : pub.keyVals) {
      if (kv.first.find("adj:") == 0 and kv.second.value) {
        auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
            kv.second.value.value(), serializer);
        nodeLabels[adjDb.thisNodeName] = adjDb.nodeLabel;
        if (adjDb.nodeLabel == 0) {
          nodeLabels.erase(adjDb.thisNodeName);
        }
      }
    }
  }

  // ensure that we have unique values
  std::set<int32_t> labelSet;
  for (auto const& kv : nodeLabels) {
    labelSet.insert(kv.second);
  }

  EXPECT_EQ(kNumNodesToTest, labelSet.size());
  // cleanup
  neighborUpdatesQueue.close();
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    linkMonitors[i]->stop();
    linkMonitorThreads[i]->join();
  }
}

/**
 * Unit-test to test advertisement of static and loopback prefixes
 * - verify initial prefix-db is set to static prefixes
 * - add addresses via addrEvent and verify from KvStore prefix-db
 * - remove address via addrEvent and verify from KvStore prefix-db
 * - announce network instead of address via addrEvent and verify it doesn't
 *   change anything
 * - set link to down state and verify that it removes all associated addresses
 */
TEST_F(LinkMonitorTestFixture, StaticLoopbackPrefixAdvertisement) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  // Verify that initial DB has static prefix entries
  std::unordered_set<thrift::IpPrefix> prefixes;
  prefixes.clear();
  while (prefixes.size() != 2) {
    LOG(INFO) << "Testing initial prefix database";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 2) {
      LOG(INFO) << "Looking for 2 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
  }

  //
  // Send link up event
  // Advertise some dummy and wrong prefixes
  //
  mockNlHandler->sendLinkEvent("loopback", 101, true);

  // push some invalid loopback addresses
  mockNlHandler->sendAddrEvent("loopback", "fe80::1/128", true);
  mockNlHandler->sendAddrEvent("loopback", "fe80::2/64", true);

  // push some valid loopback addresses
  mockNlHandler->sendAddrEvent("loopback", "10.127.240.1/32", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/128", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:cafe:babe::1/128", true);

  // push some valid interface addresses with subnet
  mockNlHandler->sendAddrEvent("loopback", "10.128.241.1/24", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/64", true);

  // Get interface updates
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  // verify
  prefixes.clear();
  while (prefixes.size() != 7) {
    LOG(INFO) << "Testing address advertisements";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 7) {
      LOG(INFO) << "Looking for 7 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::1/128")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:cafe:babe::1/128")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("10.127.240.1/32")));

    EXPECT_EQ(1, prefixes.count(toIpPrefix("10.128.241.0/24")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::/64")));
  }

  //
  // Withdraw prefix and see it is being withdrawn
  //

  // withdraw some addresses
  mockNlHandler->sendAddrEvent("loopback", "10.127.240.1/32", false);
  mockNlHandler->sendAddrEvent("loopback", "2803:cafe:babe::1/128", false);

  // withdraw addresses with subnet
  mockNlHandler->sendAddrEvent("loopback", "10.128.241.1/24", false);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/64", false);

  // Get interface updates
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  // verify
  prefixes.clear();
  while (prefixes.size() != 3) {
    LOG(INFO) << "Testing address withdraws";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 3) {
      LOG(INFO) << "Looking for 3 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::1/128")));
  }

  //
  // Send link down event
  //

  mockNlHandler->sendLinkEvent("loopback", 101, false);
  recvAndReplyIfUpdate();

  //
  // Verify all addresses are withdrawn on link down event
  //
  prefixes.clear();
  while (prefixes.size() != 2) {
    LOG(INFO) << "Testing prefix withdraws";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 2) {
      LOG(INFO) << "Looking for 2 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
  }
}

TEST(LinkMonitor, getPeersFromAdjacencies) {
  std::unordered_map<AdjacencyKey, AdjacencyValue> adjacencies;
  std::unordered_map<std::string, thrift::PeerSpec> peers;

  const auto peerSpec0 = createPeerSpec(
      "tcp://[fe80::2%iface0]:10001", "tcp://[fe80::2%iface0]:10002", false);
  const auto peerSpec1 = createPeerSpec(
      "tcp://[fe80::2%iface1]:10001", "tcp://[fe80::2%iface1]:10002", false);
  const auto peerSpec2 = createPeerSpec(
      "tcp://[fe80::2%iface2]:10001", "tcp://[fe80::2%iface2]:10002", false);
  const auto peerSpec3 = createPeerSpec(
      "tcp://[fe80::2%iface3]:10001", "tcp://[fe80::2%iface3]:10002", false);

  // Get peer spec
  adjacencies[{"node1", "iface1"}] = {peerSpec1, thrift::Adjacency()};
  adjacencies[{"node2", "iface2"}] = {peerSpec2, thrift::Adjacency()};
  peers["node1"] = peerSpec1;
  peers["node2"] = peerSpec2;
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Add {node2, iface3} to adjacencies and see no changes peers
  adjacencies[{"node2", "iface3"}] = {peerSpec3, thrift::Adjacency()};
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Add {node1, iface0} to adjacencies and see node1 changes to peerSpec0
  adjacencies[{"node1", "iface0"}] = {peerSpec0, thrift::Adjacency()};
  peers["node1"] = peerSpec0;
  EXPECT_EQ(4, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Remove {node2, iface2} from adjacencies and see node2 changes to peerSpec3
  adjacencies.erase({"node2", "iface2"});
  peers["node2"] = peerSpec3;
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Remove {node2, iface3} from adjacencies and see node2 no longer exists
  adjacencies.erase({"node2", "iface3"});
  peers.erase("node2");
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(1, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Test for empty adjacencies
  adjacencies.clear();
  peers.clear();
  EXPECT_EQ(0, adjacencies.size());
  EXPECT_EQ(0, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // with different areas
  adjacencies[{"node1", "iface1"}] = {
      peerSpec1, thrift::Adjacency(), false, "pod"};
  adjacencies[{"node2", "iface2"}] = {
      peerSpec2, thrift::Adjacency(), false, "pod"};
  peers["node1"] = peerSpec1;
  peers["node2"] = peerSpec2;
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies, "pod"));

  adjacencies[{"node2", "iface3"}] = {
      peerSpec3, thrift::Adjacency(), false, "plane"};
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(
      1, LinkMonitor::getPeersFromAdjacencies(adjacencies, "plane").size());
}

TEST_F(LinkMonitorTestFixture, AreaTest) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea(), "pod", "plane"});

  // Verify that we receive empty adjacency database in all 3 areas
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel, "plane"));
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel, "pod"));
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel));
  checkNextAdjPub("adj:node-1", "plane");
  checkNextAdjPub("adj:node-1", "pod");
  checkNextAdjPub(
      "adj:node-1", openr::thrift::KvStore_constants::kDefaultArea());

  // add link up event. AdjDB should get updated with link interface
  // Will be updated in all areas
  // TODO: Change this when interfaced base areas is implemented, in which
  // case only corresponding area kvstore should get the update

  {
    InSequence dummy;

    mockNlHandler->sendLinkEvent("iface_2_1", 100, true);
    recvAndReplyIfUpdate();
    // expect neighbor up first
    // node-2 neighbor up in iface_2_1
    auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    {
      auto neighborEvent = createNeighborEvent(
          thrift::SparkNeighborEventType::NEIGHBOR_UP,
          "iface_2_1",
          nb2,
          100 /* rtt-us */,
          1 /* label */);
      neighborUpdatesQueue.push(std::move(neighborEvent));
      LOG(INFO) << "Testing neighbor UP event in default area!";

      checkNextAdjPub(
          "adj:node-1", openr::thrift::KvStore_constants::kDefaultArea());
    }

    // bring up iface3_1, neighbor up event in plane area. Adj db in "plane"
    // area should contain only 'adj_3_1'
    mockNlHandler->sendLinkEvent("iface_3_1", 100, true);
    recvAndReplyIfUpdate();
    adjDb = createAdjDatabase("node-1", {adj_3_1}, kNodeLabel, "plane");
    expectedAdjDbs.push(std::move(adjDb));
    {
      auto neighborEvent = createNeighborEvent(
          thrift::SparkNeighborEventType::NEIGHBOR_UP,
          "iface_3_1",
          nb3,
          100 /* rtt-us */,
          1 /* label */,
          "plane");
      neighborUpdatesQueue.push(std::move(neighborEvent));
      LOG(INFO) << "Testing neighbor UP event in plane area!";

      checkNextAdjPub("adj:node-1", "plane");
    }
  }
  // neighbor up on default area
  {
    auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub(
        "adj:node-1", openr::thrift::KvStore_constants::kDefaultArea());
    checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);
  }
  // neighbor up on "plane" area
  {
    auto adjDb = createAdjDatabase("node-1", {adj_3_1}, kNodeLabel, "plane");
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */,
        "plane");
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1", "plane");
    checkPeerDump(adj_3_1.otherNodeName, peerSpec_3_1, "plane");
  }
  // verify neighbor down in "plane" area
  {
    auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel, "plane");
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */,
        "plane");
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1", "plane");
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  testing::InitGoogleMock(&argc, argv);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  auto rc = RUN_ALL_TESTS();

  // Run the tests
  return rc;
}
