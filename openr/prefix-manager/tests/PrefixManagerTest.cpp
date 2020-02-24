/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/prefix-manager/PrefixManager.h>

using namespace openr;

using apache::thrift::CompactSerializer;

namespace {

const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");
const auto addr5 = toIpPrefix("ffff:10:1:5::/64");
const auto addr6 = toIpPrefix("ffff:10:2:6::/64");
const auto addr7 = toIpPrefix("ffff:10:3:7::0/64");
const auto addr8 = toIpPrefix("ffff:10:4:8::/64");
const auto addr9 = toIpPrefix("ffff:10:4:9::/64");
const auto addr10 = toIpPrefix("ffff:10:4:10::/64");

const auto prefixEntry1 = createPrefixEntry(addr1, thrift::PrefixType::DEFAULT);
const auto prefixEntry2 =
    createPrefixEntry(addr2, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry3 = createPrefixEntry(addr3, thrift::PrefixType::DEFAULT);
const auto prefixEntry4 =
    createPrefixEntry(addr4, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry5 = createPrefixEntry(addr5, thrift::PrefixType::DEFAULT);
const auto prefixEntry6 =
    createPrefixEntry(addr6, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry7 = createPrefixEntry(addr7, thrift::PrefixType::DEFAULT);
const auto prefixEntry8 =
    createPrefixEntry(addr8, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto ephemeralPrefixEntry9 = createPrefixEntry(
    addr9,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    true);

const auto persistentPrefixEntry9 = createPrefixEntry(
    addr9,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    false);
const auto ephemeralPrefixEntry10 = createPrefixEntry(
    addr10,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    true);
const auto persistentPrefixEntry10 = createPrefixEntry(
    addr10,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    false);
} // namespace

class PrefixManagerTestFixture : public testing::TestWithParam<bool> {
 public:
  void
  SetUp() override {
    // spin up a config store
    storageFilePath = folly::sformat(
        "/tmp/pm_ut_config_store.bin.{}",
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    configStore = std::make_unique<PersistentStore>(
        "1",
        storageFilePath,
        context,
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(0),
        true);

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
        std::unordered_map<std::string, thrift::PeerSpec>{});
    kvStoreWrapper->run();
    LOG(INFO) << "The test KV store is running";

    kvStoreClient = std::make_unique<KvStoreClient>(
        context,
        &evl,
        "node-1",
        kvStoreWrapper->localCmdUrl,
        kvStoreWrapper->localPubUrl);
    // start a prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        "node-1",
        prefixUpdatesQueue.getReader(),
        configStore.get(),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        MonitorSubmitUrl{"inproc://monitor_submit"},
        PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
        perPrefixKeys_ /* per prefix keys */,
        true /* prefix-mananger perf measurement */,
        std::chrono::seconds{0},
        Constants::kKvStoreDbTtl,
        context);

    prefixManagerThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "PrefixManager thread starting";
      prefixManager->run();
      LOG(INFO) << "PrefixManager thread finishing";
    });
    prefixManager->waitUntilRunning();
  }

  void
  TearDown() override {
    // Close queues
    prefixUpdatesQueue.close();

    // this will be invoked before linkMonitorThread's d-tor
    LOG(INFO) << "Stopping the linkMonitor thread";
    prefixManager->stop();
    prefixManagerThread->join();
    prefixManager.reset();

    // Erase data from config store
    configStore->erase("prefix-manager-config").get();

    // stop config store
    configStore->stop();
    configStoreThread->join();
    configStore.reset();

    // stop the kvStore
    kvStoreWrapper->stop();
    kvStoreClient.reset();
    kvStoreWrapper.reset();
    LOG(INFO) << "The test KV store is stopped";
  }

  // In case of separate IP prefix keys, collect all the prefix Entries
  // (advertised from a specific node) and return as a list
  std::vector<thrift::PrefixEntry>
  getPrefixDb(const std::string& keyPrefix) {
    std::vector<thrift::PrefixEntry> prefixEntries{};
    auto marker = PrefixDbMarker{Constants::kPrefixDbMarker.toString()};
    auto keyPrefixDbs = kvStoreClient->dumpAllWithPrefix(keyPrefix);
    for (const auto& pkey : keyPrefixDbs.value()) {
      if (pkey.first.find(marker) == 0) {
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            pkey.second.value.value(), serializer);
        // skip prefixes marked for delete
        if (!prefixDb.deletePrefix) {
          prefixEntries.insert(
              prefixEntries.begin(),
              prefixDb.prefixEntries.begin(),
              prefixDb.prefixEntries.end());
        }
      }
    }
    return prefixEntries;
  }

  fbzmq::Context context;
  OpenrEventBase evl;

  // Queue for publishing entries to PrefixManager
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue;

  std::string storageFilePath;
  std::unique_ptr<PersistentStore> configStore;
  std::unique_ptr<std::thread> configStoreThread;

  // Create the serializer for write/read
  CompactSerializer serializer;
  std::unique_ptr<PrefixManager> prefixManager{nullptr};
  std::unique_ptr<std::thread> prefixManagerThread{nullptr};
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper{nullptr};
  std::unique_ptr<KvStoreClient> kvStoreClient{nullptr};
  const bool perPrefixKeys_ = GetParam();
};

INSTANTIATE_TEST_CASE_P(
    PrefixManagerInstance, PrefixManagerTestFixture, ::testing::Bool());

TEST_P(PrefixManagerTestFixture, AddRemovePrefix) {
  // Expect no throw
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_FALSE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry1}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry4}).get());
  EXPECT_FALSE(prefixManager->advertisePrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(
      prefixManager
          ->advertisePrefixes({prefixEntry1, prefixEntry2, prefixEntry3})
          .get());
  EXPECT_TRUE(
      prefixManager->withdrawPrefixes({prefixEntry1, prefixEntry2}).get());
  EXPECT_FALSE(
      prefixManager->withdrawPrefixes({prefixEntry1, prefixEntry2}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({ephemeralPrefixEntry9}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({ephemeralPrefixEntry9}).get());
}

TEST_P(PrefixManagerTestFixture, RemoveUpdateType) {
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry5}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry6}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry7}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry8}).get());

  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry1}).get());
  EXPECT_TRUE(
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get());
  // can't withdraw twice
  EXPECT_FALSE(
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get());

  // all the DEFAULT type should be gone
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry3}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry5}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry7}).get());

  // The PREFIX_ALLOCATOR type should still be there to be withdrawed
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry6}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry8}).get());

  EXPECT_FALSE(
      prefixManager
          ->withdrawPrefixesByType(thrift::PrefixType::PREFIX_ALLOCATOR)
          .get());

  // update all allocated prefixes
  EXPECT_TRUE(
      prefixManager->advertisePrefixes({prefixEntry2, prefixEntry4}).get());

  // Test sync logic
  EXPECT_TRUE(prefixManager
                  ->syncPrefixesByType(
                      thrift::PrefixType::PREFIX_ALLOCATOR,
                      {prefixEntry6, prefixEntry8})
                  .get());
  EXPECT_FALSE(prefixManager
                   ->syncPrefixesByType(
                       thrift::PrefixType::PREFIX_ALLOCATOR,
                       {prefixEntry6, prefixEntry8})
                   .get());

  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry2}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry6}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry8}).get());
}

TEST_P(PrefixManagerTestFixture, RemoveInvalidType) {
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry2}).get());

  // Verify that prefix type has to match for withdrawing prefix
  auto prefixEntryError = prefixEntry1;
  prefixEntryError.type = thrift::PrefixType::PREFIX_ALLOCATOR;

  auto resp1 =
      prefixManager->withdrawPrefixes({prefixEntryError, prefixEntry2}).get();
  EXPECT_FALSE(resp1);

  // Verify that all prefixes are still present
  auto resp2 = prefixManager->getPrefixes().get();
  EXPECT_TRUE(resp2);
  EXPECT_EQ(2, resp2->size());

  // Verify withdrawing of multiple prefixes
  auto resp3 =
      prefixManager->withdrawPrefixes({prefixEntry1, prefixEntry2}).get();
  EXPECT_TRUE(resp3);

  // Verify that there are no prefixes
  auto resp4 = prefixManager->getPrefixes().get();
  EXPECT_TRUE(resp4);
  EXPECT_EQ(0, resp4->size());
}

TEST_P(PrefixManagerTestFixture, VerifyKvStore) {
  prefixManager->advertisePrefixes({prefixEntry1}).get();

  std::string keyStr{"prefix:node-1"};

  if (perPrefixKeys_) {
    auto prefixKey = PrefixKey(
        "node-1",
        folly::IPAddress::createNetwork(toString(prefixEntry1.prefix)),
        thrift::KvStore_constants::kDefaultArea());

    keyStr = prefixKey.getPrefixKey();
  }

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue.hasError());
  auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue.value().value.value(), serializer);
  EXPECT_EQ(db.thisNodeName, "node-1");
  EXPECT_EQ(db.prefixEntries.size(), 1);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }

  prefixManager->withdrawPrefixes({prefixEntry1}).get();
  prefixManager->advertisePrefixes({prefixEntry2}).get();
  prefixManager->advertisePrefixes({prefixEntry3}).get();
  prefixManager->advertisePrefixes({prefixEntry4}).get();
  prefixManager->advertisePrefixes({prefixEntry5}).get();
  prefixManager->advertisePrefixes({prefixEntry6}).get();
  prefixManager->advertisePrefixes({prefixEntry7}).get();
  prefixManager->advertisePrefixes({prefixEntry8}).get();
  prefixManager->advertisePrefixes({ephemeralPrefixEntry9}).get();

  /* Verify that before throttle expires, we don't see any update */
  std::this_thread::sleep_for(Constants::kPrefixMgrKvThrottleTimeout / 2);
  auto maybeValue1 = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue1.hasError());
  auto db1 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue1.value().value.value(), serializer);
  auto prefixDb = getPrefixDb("prefix:node-1");
  EXPECT_EQ(prefixDb.size(), 1);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue2 = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue2.hasError());
  auto db2 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue2.value().value.value(), serializer);
  prefixDb = getPrefixDb("prefix:node-1");
  EXPECT_EQ(prefixDb.size(), 8);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }

  // now make a change and check again
  prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get();

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue3 = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue3.hasError());
  auto db3 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue3.value().value.value(), serializer);
  prefixDb = getPrefixDb("prefix:node-1");
  EXPECT_EQ(prefixDb.size(), 5);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }
}

/**
 * Test prefix advertisement in KvStore with multiple clients.
 * NOTE: Priority LOOPBACK > DEFAULT > BGP
 * 1. Inject prefix1 with client-bgp - Verify KvStore
 * 2. Inject prefix1 with client-loopback and client-default - Verify KvStore
 * 3. Withdraw prefix1 with client-loopback - Verify KvStore
 * 4. Withdraw prefix1 with client-bgp, client-default - Verify KvStore
 */
TEST_P(PrefixManagerTestFixture, VerifyKvStoreMultipleClients) {
  const auto loopback_prefix =
      createPrefixEntry(addr1, thrift::PrefixType::LOOPBACK);
  const auto default_prefix =
      createPrefixEntry(addr1, thrift::PrefixType::DEFAULT);
  const auto bgp_prefix = createPrefixEntry(addr1, thrift::PrefixType::BGP);

  std::string keyStr{"prefix:node-1"};
  if (perPrefixKeys_) {
    keyStr = PrefixKey(
                 "node-1",
                 toIPNetwork(addr1),
                 thrift::KvStore_constants::kDefaultArea())
                 .getPrefixKey();
  }

  // Synchronization primitive
  folly::Baton baton;
  folly::Optional<thrift::PrefixEntry> expectedPrefix;
  bool gotExpected = true;

  kvStoreClient->subscribeKey(
      keyStr,
      [&](std::string const& _, folly::Optional<thrift::Value> val) mutable {
        ASSERT_TRUE(val.hasValue());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            val->value.value(), serializer);
        EXPECT_EQ(db.thisNodeName, "node-1");
        if (expectedPrefix.hasValue() and db.prefixEntries.size() != 0) {
          // we should always be advertising one prefix until we withdraw all
          EXPECT_EQ(db.prefixEntries.size(), 1);
          EXPECT_EQ(expectedPrefix, db.prefixEntries.at(0));
          gotExpected = true;
        } else {
          EXPECT_TRUE(not perPrefixKeys_ or db.deletePrefix);
          EXPECT_TRUE(not perPrefixKeys_ or db.prefixEntries.size() == 1);
        }

        // Signal verification
        if (gotExpected) {
          baton.post();
        }
      });

  // Start event loop in it's own thread
  std::thread evlThread([&]() { evl.run(); });
  evl.waitUntilRunning();

  //
  // 1. Inject prefix1 with client-bgp - Verify KvStore
  //
  expectedPrefix = bgp_prefix;
  gotExpected = false;
  prefixManager->advertisePrefixes({bgp_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 2. Inject prefix1 with client-loopback and client-default - Verify KvStore
  //
  expectedPrefix = loopback_prefix; // lowest client-id will win
  gotExpected = false;
  prefixManager->advertisePrefixes({loopback_prefix, default_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 3. Withdraw prefix1 with client-loopback - Verify KvStore
  //
  expectedPrefix = default_prefix;
  gotExpected = false;
  prefixManager->withdrawPrefixes({loopback_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 4. Withdraw prefix1 with client-bgp, client-default - Verify KvStore
  //
  expectedPrefix = folly::none;
  gotExpected = true;
  prefixManager->withdrawPrefixes({bgp_prefix, default_prefix}).get();
  baton.wait();
  baton.reset();

  // Nuke test environment
  evl.stop();
  evlThread.join();
}

/**
 * test to check prefix key add, withdraw does not trigger update for all
 * the prefixes managed by the prefix manager. This test does not apply to
 * the old key format
 */
TEST_P(PrefixManagerTestFixture, PrefixKeyUpdates) {
  int waitDuration{0};
  // test only if 'create ip prefixes' is enabled
  if (!perPrefixKeys_) {
    return;
  }

  auto prefixKey1 = PrefixKey(
      "node-1",
      folly::IPAddress::createNetwork(toString(prefixEntry1.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  auto prefixKey2 = PrefixKey(
      "node-1",
      folly::IPAddress::createNetwork(toString(prefixEntry2.prefix)),
      thrift::KvStore_constants::kDefaultArea());

  // Schedule callback to set keys from client1 (this will be executed first)
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry1});
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);
      });

  // add another key
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry2}).get();
      });

  // version of first key should still be 1
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 4 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        EXPECT_EQ(maybeValue2.value().version, 1);
      });

  // withdraw prefixEntry2
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        prefixManager->withdrawPrefixes({prefixEntry2}).get();
      });

  // version of prefixEntry1 should still be 1
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);

        // verify key is withdrawn
        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue2.value().value.value(), serializer);
        EXPECT_NE(db.prefixEntries.size(), 0);
        EXPECT_TRUE(db.deletePrefix);
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { evl.stop(); });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "main loop starting.";
    evl.run();
    LOG(INFO) << "main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();
}

/**
 * Test prefix key subscription callback from Kvstore client.
 * The test verifies the callback takes the action that reflects the current
 * state of prefix in the prefix manager (either exists or does not exist) and
 * appropriately udpates Kvstore
 */
TEST_P(PrefixManagerTestFixture, PrefixKeySubscribtion) {
  int waitDuration{0};
  int keyVersion{0};
  std::string prefixKeyStr{"prefix:node-1"};
  const auto prefixEntry =
      createPrefixEntry(toIpPrefix("5001::/64"), thrift::PrefixType::DEFAULT);
  auto prefixKey = PrefixKey(
      "node-1",
      folly::IPAddress::createNetwork(toString(prefixEntry.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  if (perPrefixKeys_) {
    prefixKeyStr = prefixKey.getPrefixKey();
  }

  // Schedule callback to set keys from client1 (this will be executed first)
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry}).get();
      });

  // Wait for throttled update to announce to kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        keyVersion = maybeValue.value().version;
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(db.thisNodeName, "node-1");
        EXPECT_EQ(db.prefixEntries.size(), 1);
        EXPECT_EQ(db.prefixEntries[0], prefixEntry);
      });

  thrift::PrefixDatabase emptyPrefxDb;
  emptyPrefxDb.thisNodeName = "node-1";
  emptyPrefxDb.prefixEntries = {};
  const auto emptyPrefxDbStr =
      fbzmq::util::writeThriftObjStr(emptyPrefxDb, serializer);

  // increment the key version in kvstore and set empty value. kvstoreClient
  // will detect value changed, and retain the value present in peristent DB,
  // and advertise with higher key version.
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 10), [&]() noexcept {
        kvStoreClient->setKey(
            prefixKeyStr,
            emptyPrefxDbStr,
            keyVersion + 1,
            Constants::kKvStoreDbTtl);
      });

  // Wait for throttled update to announce to kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(maybeValue.value().version, keyVersion + 2);
        EXPECT_EQ(db.thisNodeName, "node-1");
        EXPECT_EQ(db.prefixEntries.size(), 1);
        EXPECT_EQ(db.prefixEntries[0], prefixEntry);
      });

  // Clear key from prefix DB map, which will delete key from persistent
  // store and update kvstore with empty prefix entry list
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { prefixManager->withdrawPrefixes({prefixEntry}).get(); });

  // verify key is withdrawn from kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(maybeValue.value().version, keyVersion + 3);
        EXPECT_EQ(db.thisNodeName, "node-1");
        // delete prefix must be set to TRUE, applies only when per prefix key
        // is enabled
        if (perPrefixKeys_) {
          EXPECT_NE(db.prefixEntries.size(), 0);
          EXPECT_TRUE(db.deletePrefix);
        }
      });

  thrift::PrefixDatabase nonEmptyPrefxDb;
  nonEmptyPrefxDb.thisNodeName = "node-1";
  nonEmptyPrefxDb.prefixEntries = {prefixEntry};
  const auto nonEmptyPrefxDbStr =
      fbzmq::util::writeThriftObjStr(nonEmptyPrefxDb, serializer);

  // Insert same key in kvstore with any higher version, and non empty value
  // Prefix manager should get the update and re-advertise with empty Prefix
  // with higher key version.
  int staleKeyVersion{100};
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        kvStoreClient->setKey(
            prefixKeyStr,
            nonEmptyPrefxDbStr,
            staleKeyVersion,
            Constants::kKvStoreDbTtl);
      });

  // prefix manager will override the key inserted above with higher key
  // version and empty prefix DB
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(maybeValue.value().version, staleKeyVersion + 1);
        EXPECT_EQ(db.thisNodeName, "node-1");
        // delete prefix must be set to TRUE, applies only when per prefix key
        // is enabled
        if (perPrefixKeys_) {
          EXPECT_NE(db.prefixEntries.size(), 0);
          EXPECT_TRUE(db.deletePrefix);
        }
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { evl.stop(); });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "main loop starting.";
    evl.run();
    LOG(INFO) << "main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();
}

TEST_P(PrefixManagerTestFixture, PrefixWithdrawExpiry) {
  int waitDuration{0};

  if (!perPrefixKeys_) {
    return;
  }

  std::chrono::milliseconds ttl{100};
  // spin up a new PrefixManager add verify that it loads the config
  auto prefixManager2 = std::make_unique<PrefixManager>(
      "node-2",
      prefixUpdatesQueue.getReader(),
      configStore.get(),
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      perPrefixKeys_ /* create IP prefix keys */,
      false /* prefix-mananger perf measurement */,
      std::chrono::seconds(0),
      ttl,
      context);

  auto prefixManagerThread2 = std::make_unique<std::thread>([&]() {
    LOG(INFO) << "PrefixManager thread starting";
    prefixManager2->run();
    LOG(INFO) << "PrefixManager thread finishing";
  });
  prefixManager2->waitUntilRunning();

  auto prefixKey1 = PrefixKey(
      "node-2",
      folly::IPAddress::createNetwork(toString(prefixEntry1.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  auto prefixKey2 = PrefixKey(
      "node-2",
      folly::IPAddress::createNetwork(toString(prefixEntry2.prefix)),
      thrift::KvStore_constants::kDefaultArea());

  // insert two prefixes
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 0), [&]() noexcept {
        prefixManager2->advertisePrefixes({prefixEntry1}).get();
        prefixManager2->advertisePrefixes({prefixEntry2}).get();
      });

  // check both prefixes are in kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        EXPECT_EQ(maybeValue2.value().version, 1);
      });

  // withdraw prefixEntry1
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        prefixManager2->withdrawPrefixes({prefixEntry1}).get();
      });

  // check prefix entry1 should have been expired, prefix 2 should be there
  // with same version
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration +=
          2 * Constants::kPrefixMgrKvThrottleTimeout.count() + ttl.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasValue());

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        EXPECT_EQ(maybeValue2.value().version, 1);
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { evl.stop(); });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "main loop starting.";
    evl.run();
    LOG(INFO) << "main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();

  // cleanup
  prefixUpdatesQueue.close();
  prefixManager2->stop();
  prefixManagerThread2->join();
}

TEST_P(PrefixManagerTestFixture, CheckReload) {
  prefixManager->advertisePrefixes({prefixEntry1}).get();
  prefixManager->advertisePrefixes({prefixEntry2}).get();
  prefixManager->advertisePrefixes({ephemeralPrefixEntry9}).get();

  // spin up a new PrefixManager add verify that it loads the config
  auto prefixManager2 = std::make_unique<PrefixManager>(
      "node-2",
      prefixUpdatesQueue.getReader(),
      configStore.get(),
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      perPrefixKeys_ /* create IP prefix keys */,
      false /* prefix-mananger perf measurement */,
      std::chrono::seconds(0),
      Constants::kKvStoreDbTtl,
      context);

  auto prefixManagerThread2 = std::make_unique<std::thread>([&]() {
    LOG(INFO) << "PrefixManager thread starting";
    prefixManager2->run();
    LOG(INFO) << "PrefixManager thread finishing";
  });
  prefixManager2->waitUntilRunning();

  // verify that the new manager has only persistent prefixes.
  // Ephemeral prefixes will not be reloaded.
  EXPECT_TRUE(prefixManager2->withdrawPrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager2->withdrawPrefixes({prefixEntry2}).get());
  EXPECT_FALSE(prefixManager2->withdrawPrefixes({ephemeralPrefixEntry9}).get());

  // cleanup
  prefixUpdatesQueue.close();
  prefixManager2->stop();
  prefixManagerThread2->join();
}

TEST_P(PrefixManagerTestFixture, GetPrefixes) {
  prefixManager->advertisePrefixes({prefixEntry1});
  prefixManager->advertisePrefixes({prefixEntry2});
  prefixManager->advertisePrefixes({prefixEntry3});
  prefixManager->advertisePrefixes({prefixEntry4});
  prefixManager->advertisePrefixes({prefixEntry5});
  prefixManager->advertisePrefixes({prefixEntry6});
  prefixManager->advertisePrefixes({prefixEntry7});

  auto resp1 = prefixManager->getPrefixes().get();
  ASSERT_TRUE(resp1);
  auto& prefixes1 = *resp1;
  EXPECT_EQ(7, prefixes1.size());
  EXPECT_NE(
      std::find(prefixes1.begin(), prefixes1.end(), prefixEntry4),
      prefixes1.end());
  EXPECT_EQ(
      std::find(prefixes1.begin(), prefixes1.end(), prefixEntry8),
      prefixes1.end());

  auto resp2 =
      prefixManager->getPrefixesByType(thrift::PrefixType::DEFAULT).get();
  ASSERT_TRUE(resp2);
  auto& prefixes2 = *resp2;
  EXPECT_EQ(4, prefixes2.size());
  EXPECT_NE(
      std::find(prefixes2.begin(), prefixes2.end(), prefixEntry3),
      prefixes2.end());
  EXPECT_EQ(
      std::find(prefixes2.begin(), prefixes2.end(), prefixEntry2),
      prefixes2.end());

  auto resp3 =
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get();
  EXPECT_TRUE(resp3);

  auto resp4 =
      prefixManager->getPrefixesByType(thrift::PrefixType::DEFAULT).get();
  EXPECT_TRUE(resp4);
  EXPECT_EQ(0, resp4->size());
}

TEST(PrefixManagerTest, HoldTimeout) {
  fbzmq::Context context;
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue;

  // spin up a config store
  auto configStore = std::make_unique<PersistentStore>(
      "1",
      folly::sformat(
          "/tmp/pm_ut_config_store.bin.{}",
          std::hash<std::thread::id>{}(std::this_thread::get_id())),
      context,
      Constants::kPersistentStoreInitialBackoff,
      Constants::kPersistentStoreMaxBackoff,
      true);
  std::thread configStoreThread([&]() noexcept {
    LOG(INFO) << "ConfigStore thread starting";
    configStore->run();
    LOG(INFO) << "ConfigStore thread finishing";
  });
  configStore->waitUntilRunning();

  // spin up a kvstore
  auto kvStoreWrapper = std::make_unique<KvStoreWrapper>(
      context,
      "test_store1",
      std::chrono::seconds(1) /* db sync interval */,
      std::chrono::seconds(600) /* counter submit interval */,
      std::unordered_map<std::string, thrift::PeerSpec>{});
  kvStoreWrapper->run();
  LOG(INFO) << "The test KV store is running";

  // start a prefix manager with timeout
  const std::chrono::seconds holdTime{2};
  const auto startTime = std::chrono::steady_clock::now();
  auto prefixManager = std::make_unique<PrefixManager>(
      "node-1",
      prefixUpdatesQueue.getReader(),
      configStore.get(),
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      false /* create IP prefix keys */,
      false /* prefix-mananger perf measurement */,
      holdTime,
      Constants::kKvStoreDbTtl,
      context);
  std::thread prefixManagerThread([&]() {
    LOG(INFO) << "PrefixManager thread starting";
    prefixManager->run();
    LOG(INFO) << "PrefixManager thread finishing";
  });
  prefixManager->waitUntilRunning();

  // We must receive publication after holdTime
  auto publication = kvStoreWrapper->recvPublication(holdTime * 2);
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime);
  CHECK_GE(
      elapsedTime.count(),
      std::chrono::duration_cast<std::chrono::milliseconds>(holdTime).count());
  CHECK_EQ(1, publication.keyVals.size());
  CHECK_EQ(1, publication.keyVals.count("prefix:node-1"));

  // Stop the test
  prefixUpdatesQueue.close();
  prefixManager->stop();
  prefixManagerThread.join();
  kvStoreWrapper->stop();
  configStore->stop();
  configStoreThread.join();
}

// Verify that persist store is updated only when
// non-ephemeral types are effected
TEST_P(PrefixManagerTestFixture, CheckPersistStoreUpdate) {
  ASSERT_EQ(0, configStore->getNumOfDbWritesToDisk());
  // Verify that any action on persistent entries leads to update of store
  prefixManager->advertisePrefixes({prefixEntry1, prefixEntry2, prefixEntry3})
      .get();
  // 3 prefixes leads to 1 write
  ASSERT_EQ(1, configStore->getNumOfDbWritesToDisk());

  prefixManager->withdrawPrefixes({prefixEntry1}).get();
  ASSERT_EQ(2, configStore->getNumOfDbWritesToDisk());

  prefixManager
      ->syncPrefixesByType(
          thrift::PrefixType::PREFIX_ALLOCATOR, {prefixEntry2, prefixEntry4})
      .get();
  ASSERT_EQ(3, configStore->getNumOfDbWritesToDisk());

  prefixManager->withdrawPrefixesByType(thrift::PrefixType::PREFIX_ALLOCATOR)
      .get();
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  // Verify that any actions on ephemeral entries does not lead to update of
  // store
  prefixManager
      ->advertisePrefixes({ephemeralPrefixEntry9, ephemeralPrefixEntry10})
      .get();
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  prefixManager->withdrawPrefixes({ephemeralPrefixEntry9}).get();
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  prefixManager
      ->syncPrefixesByType(thrift::PrefixType::BGP, {ephemeralPrefixEntry10})
      .get();
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  prefixManager->withdrawPrefixesByType(thrift::PrefixType::BGP).get();
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());
}

// Verify that persist store is update properly when both persistent
// and ephemeral entries are mixed for same prefix type
TEST_P(PrefixManagerTestFixture, CheckEphemeralAndPersistentUpdate) {
  ASSERT_EQ(0, configStore->getNumOfDbWritesToDisk());
  // Verify that any action on persistent entries leads to update of store
  prefixManager
      ->advertisePrefixes({persistentPrefixEntry9, ephemeralPrefixEntry10})
      .get();
  ASSERT_EQ(1, configStore->getNumOfDbWritesToDisk());

  // Change persistance characterstic. Expect disk update
  prefixManager
      ->syncPrefixesByType(
          thrift::PrefixType::BGP,
          {ephemeralPrefixEntry9, persistentPrefixEntry10})
      .get();
  ASSERT_EQ(2, configStore->getNumOfDbWritesToDisk());

  // Only ephemeral entry withdrawn, so no update to disk
  prefixManager->withdrawPrefixes({ephemeralPrefixEntry9}).get();
  ASSERT_EQ(2, configStore->getNumOfDbWritesToDisk());

  // Persistent entry withdrawn, expect update to disk
  prefixManager->withdrawPrefixes({persistentPrefixEntry10}).get();
  ASSERT_EQ(3, configStore->getNumOfDbWritesToDisk());

  // Restore the state to mix of ephemeral and persistent of a type
  prefixManager
      ->advertisePrefixes({persistentPrefixEntry9, ephemeralPrefixEntry10})
      .get();
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  // Verify that withdraw by type, updates disk
  prefixManager->withdrawPrefixesByType(thrift::PrefixType::BGP).get();
  ASSERT_EQ(5, configStore->getNumOfDbWritesToDisk());

  // Restore the state to mix of ephemeral and persistent of a type
  prefixManager
      ->advertisePrefixes({persistentPrefixEntry9, ephemeralPrefixEntry10})
      .get();
  ASSERT_EQ(6, configStore->getNumOfDbWritesToDisk());

  // Verify that entry in DB being deleted is persistent so file is update
  prefixManager
      ->syncPrefixesByType(thrift::PrefixType::BGP, {ephemeralPrefixEntry10})
      .get();
  ASSERT_EQ(7, configStore->getNumOfDbWritesToDisk());
}

TEST_P(PrefixManagerTestFixture, PrefixUpdatesQueue) {
  // Helper function to receive expected number of updates from KvStore
  auto recvPublication = [this](int num) {
    for (int i = 0; i < num; ++i) {
      EXPECT_NO_THROW(kvStoreWrapper->recvPublication(std::chrono::seconds(1)));
    }
  };

  // Receive initial empty prefix database from KvStore when per-prefix key is
  recvPublication(perPrefixKeys_ ? 0 : 1);

  // ADD_PREFIXES
  {
    // Send update request in queue
    thrift::PrefixUpdateRequest request;
    request.cmd = thrift::PrefixUpdateCommand::ADD_PREFIXES;
    request.prefixes = {prefixEntry1, persistentPrefixEntry9};
    prefixUpdatesQueue.push(std::move(request));

    // Wait for update in KvStore (PrefixManager has processed the update)
    recvPublication(perPrefixKeys_ ? 2 : 1);

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(2, prefixes->size());
    EXPECT_THAT(*prefixes, testing::Contains(prefixEntry1));
    EXPECT_THAT(*prefixes, testing::Contains(persistentPrefixEntry9));
  }

  // WITHDRAW_PREFIXES_BY_TYPE
  {
    // Send update request in queue
    thrift::PrefixUpdateRequest request;
    request.cmd = thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES_BY_TYPE;
    request.type = thrift::PrefixType::BGP;
    prefixUpdatesQueue.push(std::move(request));

    // Wait for update in KvStore (PrefixManager has processed the update)
    recvPublication(1);

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(1, prefixes->size());
    EXPECT_THAT(*prefixes, testing::Contains(prefixEntry1));
  }

  // SYNC_PREFIXES_BY_TYPE
  {
    // Send update request in queue
    thrift::PrefixUpdateRequest request;
    request.cmd = thrift::PrefixUpdateCommand::SYNC_PREFIXES_BY_TYPE;
    request.type = thrift::PrefixType::DEFAULT;
    request.prefixes = {prefixEntry3};
    prefixUpdatesQueue.push(std::move(request));

    // Wait for update in KvStore (PrefixManager has processed the update)
    recvPublication(perPrefixKeys_ ? 2 : 1);

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(1, prefixes->size());
    EXPECT_THAT(*prefixes, testing::Contains(prefixEntry3));
  }

  // WITHDRAW_PREFIXES
  {
    // Send update request in queue
    thrift::PrefixUpdateRequest request;
    request.cmd = thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES;
    request.prefixes = {prefixEntry3};
    prefixUpdatesQueue.push(std::move(request));

    // Wait for update in KvStore (PrefixManager has processed the update)
    recvPublication(1);

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(0, prefixes->size());
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
