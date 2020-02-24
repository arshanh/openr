/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SparkWrapper.h"

using namespace fbzmq;

namespace openr {

SparkWrapper::SparkWrapper(
    std::string const& myDomainName,
    std::string const& myNodeName,
    std::chrono::milliseconds myHoldTime,
    std::chrono::milliseconds myKeepAliveTime,
    std::chrono::milliseconds myFastInitKeepAliveTime,
    bool enableV4,
    bool enableSubnetValidation,
    MonitorSubmitUrl const& monitorCmdUrl,
    std::pair<uint32_t, uint32_t> version,
    fbzmq::Context& zmqContext,
    std::shared_ptr<IoProvider> ioProvider,
    folly::Optional<std::unordered_set<std::string>> areas,
    bool enableSpark2,
    bool increaseHelloInterval,
    SparkTimeConfig timeConfig)
    : myNodeName_(myNodeName) {
  spark_ = std::make_shared<Spark>(
      myDomainName,
      myNodeName,
      static_cast<uint16_t>(6666),
      myHoldTime,
      myKeepAliveTime,
      myFastInitKeepAliveTime, // fastInitKeepAliveTime
      timeConfig.myHelloTime, // spark2_hello_time
      timeConfig.myHelloFastInitTime, // spark2_hello_fast_init_time
      timeConfig.myHandshakeTime, // spark2_handshake_time
      timeConfig.myHeartbeatTime, // spark2_heartbeat_time
      timeConfig.myNegotiateHoldTime, // spark2_negotiate_hold_time
      timeConfig.myHeartbeatHoldTime, // spark2_heartbeat_hold_time
      folly::none /* ip-tos */,
      enableV4,
      enableSubnetValidation,
      interfaceUpdatesQueue_.getReader(),
      neighborUpdatesQueue_,
      monitorCmdUrl,
      KvStorePubPort{10001},
      KvStoreCmdPort{10002},
      OpenrCtrlThriftPort{2018},
      version,
      zmqContext,
      std::move(ioProvider),
      true,
      enableSpark2,
      increaseHelloInterval,
      areas);

  // start spark
  run();
}

SparkWrapper::~SparkWrapper() {
  stop();
}

void
SparkWrapper::run() {
  thread_ = std::make_unique<std::thread>([this]() {
    VLOG(1) << "Spark running.";
    spark_->run();
    VLOG(1) << "Spark stopped.";
  });
  spark_->waitUntilRunning();
}

void
SparkWrapper::stop() {
  interfaceUpdatesQueue_.close();
  spark_->stop();
  spark_->waitUntilStopped();
  thread_->join();
}

bool
SparkWrapper::updateInterfaceDb(
    const std::vector<SparkInterfaceEntry>& interfaceEntries) {
  thrift::InterfaceDatabase ifDb(
      apache::thrift::FRAGILE, myNodeName_, {}, thrift::PerfEvents());
  ifDb.perfEvents = folly::none;

  for (const auto& interface : interfaceEntries) {
    ifDb.interfaces.emplace(
        interface.ifName,
        thrift::InterfaceInfo(
            apache::thrift::FRAGILE,
            true,
            interface.ifIndex,
            // TO BE DEPRECATED SOON
            {toBinaryAddress(interface.v4Network.first)},
            {toBinaryAddress(interface.v4Network.first)},
            {toIpPrefix(interface.v4Network),
             toIpPrefix(interface.v6LinkLocalNetwork)}));
  }

  interfaceUpdatesQueue_.push(std::move(ifDb));
  return true;
}

folly::Expected<thrift::SparkNeighborEvent, Error>
SparkWrapper::recvNeighborEvent(
    folly::Optional<std::chrono::milliseconds> timeout) {
  auto startTime = std::chrono::steady_clock::now();
  while (not neighborUpdatesReader_.size()) {
    // Break if timeout occurs
    auto now = std::chrono::steady_clock::now();
    if (timeout.hasValue() && now - startTime > timeout.value()) {
      return folly::makeUnexpected(Error(-1, std::string("timed out")));
    }
    // Yield the thread
    std::this_thread::yield();
  }

  return neighborUpdatesReader_.get().value();
}

folly::Optional<thrift::SparkNeighborEvent>
SparkWrapper::waitForEvent(
    const thrift::SparkNeighborEventType eventType,
    folly::Optional<std::chrono::milliseconds> rcvdTimeout,
    folly::Optional<std::chrono::milliseconds> procTimeout) noexcept {
  auto startTime = std::chrono::steady_clock::now();

  while (true) {
    // check if it is beyond procTimeout
    auto endTime = std::chrono::steady_clock::now();
    if (endTime - startTime > procTimeout.value()) {
      LOG(ERROR) << "Timeout receiving event. Time limit: "
                 << procTimeout.value().count();
      break;
    }
    auto maybeEvent = recvNeighborEvent(rcvdTimeout);
    if (maybeEvent.hasError()) {
      LOG(ERROR) << "recvNeighborEvent failed: " << maybeEvent.error();
      continue;
    }
    auto& event = maybeEvent.value();
    if (eventType == event.eventType) {
      return event;
    }
  }
  return folly::none;
}

std::pair<folly::IPAddress, folly::IPAddress>
SparkWrapper::getTransportAddrs(const thrift::SparkNeighborEvent& event) {
  return {toIPAddress(event.neighbor.transportAddressV4),
          toIPAddress(event.neighbor.transportAddressV6)};
}

folly::Optional<SparkNeighState>
SparkWrapper::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  return spark_->getSparkNeighState(ifName, neighborName);
}

} // namespace openr
