/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>

#include <folly/init/Init.h>

#include <openr/common/OpenrClient.h>
#include <openr/kvstore/KvStore.h>

DEFINE_string(host, "::1", "Host to connect to");
DEFINE_int32(port, openr::Constants::kOpenrCtrlPort, "OpenrCtrl server port");
DEFINE_int32(connect_timeout_ms, 1000, "Connect timeout for client");
DEFINE_int32(processing_timeout_ms, 5000, "Processing timeout for client");

int
main(int argc, char** argv) {
  // Initialize all params
  folly::init(&argc, &argv);

  // Define and start event base
  folly::EventBase evb;
  std::thread evbThread([&evb]() { evb.loopForever(); });

  // Create Open/R client
  auto client =
      openr::getOpenrCtrlPlainTextClient<apache::thrift::RocketClientChannel>(
          evb,
          folly::IPAddress(FLAGS_host),
          FLAGS_port,
          std::chrono::milliseconds(FLAGS_connect_timeout_ms),
          std::chrono::milliseconds(FLAGS_processing_timeout_ms));
  auto response = client->semifuture_subscribeAndGetKvStore().get();
  auto& globalKeyVals = response.response.keyVals;
  LOG(INFO) << "Stream is connected, updates will follow";
  LOG(INFO) << "Received " << globalKeyVals.size()
            << " entries in initial dump.";
  LOG(INFO) << "";

  auto subscription =
      std::move(response.stream)
          .subscribeExTry(
              folly::Executor::getKeepAliveToken(&evb),
              [&globalKeyVals](
                  folly::Try<openr::thrift::Publication>&& maybePub) mutable {
                if (maybePub.hasException()) {
                  LOG(ERROR) << maybePub.exception().what();
                  return;
                }
                auto& pub = maybePub.value();
                // Print expired key-vals
                for (const auto& key : pub.expiredKeys) {
                  std::cout << "Expired Key: " << key << std::endl;
                  std::cout << "" << std::endl;
                }

                // Print updates
                auto updatedKeyVals =
                    openr::KvStore::mergeKeyValues(globalKeyVals, pub.keyVals);
                for (auto& kv : updatedKeyVals) {
                  std::cout
                      << (kv.second.value.hasValue() ? "Updated" : "Refreshed")
                      << " KeyVal: " << kv.first << std::endl;
                  std::cout << "  version: " << kv.second.version << std::endl;
                  std::cout << "  originatorId: " << kv.second.originatorId
                            << std::endl;
                  std::cout << "  ttl: " << kv.second.ttl << std::endl;
                  std::cout << "  ttlVersion: " << kv.second.ttlVersion
                            << std::endl;
                  std::cout << "  hash: " << kv.second.hash.value() << std::endl
                            << std::endl; // intended
                }
              });

  evbThread.join();
  subscription.cancel();
  std::move(subscription).detach();
  client.reset();

  return 0;
}
