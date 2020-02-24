/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <functional>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Optional.h>
#include <folly/SocketAddress.h>
#include <folly/container/EvictingCacheMap.h>
#include <folly/fibers/FiberManager.h>
#include <folly/stats/BucketedTimeSeries.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/OpenrEventBase.h>
#include <openr/common/StepDetector.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Spark_types.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/spark/IoProvider.h>

namespace openr {

enum class PacketValidationResult {
  SUCCESS = 1,
  FAILURE = 2,
  NEIGHBOR_RESTART = 3,
  SKIP_LOOPED_SELF = 4,
  INVALID_AREA_CONFIGURATION = 5,
};

//
// Define SparkNeighState for Spark2 usage. This is used to define
// transition state for neighbors as part of the Finite State Machine.
//
enum class SparkNeighState {
  IDLE = 0,
  WARM = 1,
  NEGOTIATE = 2,
  ESTABLISHED = 3,
  RESTART = 4,
  UNEXPECTED_STATE = 5,
};

enum class SparkNeighEvent {
  HELLO_RCVD_INFO = 0,
  HELLO_RCVD_NO_INFO = 1,
  HELLO_RCVD_RESTART = 2,
  HEARTBEAT_RCVD = 3,
  HANDSHAKE_RCVD = 4,
  HEARTBEAT_TIMER_EXPIRE = 5,
  NEGOTIATE_TIMER_EXPIRE = 6,
  GR_TIMER_EXPIRE = 7,
  UNEXPECTED_EVENT = 8,
};

//
// Spark is responsible of telling our peer of our existence
// and also tracking the neighbor liveness. It publishes the
// neighbor state changes to a single downstream consumer
// via a PAIR socket.
//
// It receives commands in form of "add interface" / "remove inteface"
// and starts hello process on those interfaces.
//

class Spark final : public OpenrEventBase {
 public:
  Spark(
      std::string const& myDomainName,
      std::string const& myNodeName,
      uint16_t const udpMcastPort,
      std::chrono::milliseconds myHoldTime,
      std::chrono::milliseconds myKeepAliveTime,
      std::chrono::milliseconds fastInitKeepAliveTime,
      std::chrono::milliseconds myHelloTime,
      std::chrono::milliseconds myHelloFastInitTime,
      std::chrono::milliseconds myHandshakeTime,
      std::chrono::milliseconds myHeartbeatTime,
      std::chrono::milliseconds myNegotiateHoldTime,
      std::chrono::milliseconds myHeartbeatHoldTime,
      folly::Optional<int> ipTos,
      bool enableV4,
      bool enableSubnetValidation,
      messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue,
      messaging::ReplicateQueue<thrift::SparkNeighborEvent>& nbrUpdatesQueue,
      MonitorSubmitUrl const& monitorSubmitUrl,
      KvStorePubPort kvStorePubPort,
      KvStoreCmdPort kvStoreCmdPort,
      OpenrCtrlThriftPort openrCtrlThriftPort,
      std::pair<uint32_t, uint32_t> version,
      fbzmq::Context& zmqContext,
      std::shared_ptr<IoProvider> ioProvider,
      bool enableFloodOptimization = false,
      bool enableSpark2 = false,
      bool increaseHelloInterval = false,
      folly::Optional<std::unordered_set<std::string>> areas = folly::none);

  ~Spark() override = default;

  // get the current state of neighborNode, used for unit-testing
  folly::Optional<SparkNeighState> getSparkNeighState(
      std::string const& ifName, std::string const& neighborName);

  // get counters
  std::unordered_map<std::string, int64_t> getCounters();

  // override eventloop stop()
  void stop() override;

 private:
  //
  // Interface tracking
  //
  class Interface {
   public:
    Interface(
        int ifIndex,
        const folly::CIDRNetwork& v4Network,
        const folly::CIDRNetwork& v6LinkLocalNetwork)
        : ifIndex(ifIndex),
          v4Network(v4Network),
          v6LinkLocalNetwork(v6LinkLocalNetwork) {}

    bool
    operator==(const Interface& interface) const {
      return (
          (ifIndex == interface.ifIndex) &&
          (v4Network == interface.v4Network) &&
          (v6LinkLocalNetwork == interface.v6LinkLocalNetwork));
    }

    int ifIndex{0};
    folly::CIDRNetwork v4Network;
    folly::CIDRNetwork v6LinkLocalNetwork;
  };

  // Spark is non-copyable
  Spark(Spark const&) = delete;
  Spark& operator=(Spark const&) = delete;

  // Initializes ZMQ sockets
  void prepare(folly::Optional<int> maybeIpTos) noexcept;

  // check neighbor's hello packet; return true if packet is valid and
  // passed the following checks:
  //
  // (1) neighbor is not self (packet not looped back)
  // (2) validate hello packet sequence number. detects neighbor restart if
  //     sequence number gets wrapped up again.
  // (3) performs various other validation e.g. domain, subnet validation etc.
  PacketValidationResult sanityCheckHelloPkt(
      std::string const& domainName,
      std::string const& neighborName,
      std::string const& remoteIfName,
      uint32_t const& remoteVersion);

  PacketValidationResult validateHelloPacket(
      std::string const& ifName, thrift::SparkHelloPacket const& helloPacket);

  // invoked when a neighbor's rtt changes
  void processNeighborRttChange(
      std::string const& ifName,
      thrift::SparkNeighbor const& originator,
      int64_t const newRtt);

  // Invoked when a neighbor's hold timer is expired. We remove the neighbor
  // from our tracking list.
  void processNeighborHoldTimeout(
      std::string const& ifName, std::string const& neighborName);

  // Determine if we should process the next packte from this ifName, addr pair
  bool shouldProcessHelloPacket(
      std::string const& ifName, folly::IPAddress const& addr);

  // process hello packet from a neighbor. we want to see if
  // the neighbor could be added as adjacent peer.
  void processHelloPacket();

  // originate my hello packet on given interface
  void sendHelloPacket(
      std::string const& ifName,
      bool inFastInitState = false,
      bool restarting = false);

  // Function processes interface updates from LinkMonitor and appropriately
  // enable/disable neighbor discovery
  void processInterfaceUpdates(thrift::InterfaceDatabase&& interfaceUpdates);

  // util function to delete interface in spark
  void deleteInterfaceFromDb(const std::set<std::string>& toDel);

  // util function to add interface in spark
  void addInterfaceToDb(
      const std::set<std::string>& toAdd,
      const std::unordered_map<std::string, Interface>& newInterfaceDb);

  // util function yo update interface in spark
  void updateInterfaceInDb(
      const std::set<std::string>& toUpdate,
      const std::unordered_map<std::string, Interface>& newInterfaceDb);

  // find an interface name in the interfaceDb given an ifIndex
  folly::Optional<std::string> findInterfaceFromIfindex(int ifIndex);

  // Utility function to generate a new label for neighbor on given interface.
  // If there is only one neighbor per interface then labels are expected to be
  // same across process-restarts
  int32_t getNewLabelForIface(std::string const& ifName);

  // Sumbmits the counter/stats to monitor
  void submitCounters();

  // find common area, must be only one or none
  folly::Expected<std::string, folly::Unit> findCommonArea(
      folly::Optional<std::unordered_set<std::string>> areas,
      const std::string& nodeName);

  // function to receive and parse received pkt
  bool parsePacket(
      thrift::SparkHelloPacket& pkt /* packet( type will be renamed later) */,
      std::string& ifName /* interface */,
      std::chrono::microseconds& recvTime /* kernel timestamp when recved */);

  // function to validate v4Address with its subnet
  PacketValidationResult validateV4AddressSubnet(
      std::string const& ifName, thrift::BinaryAddress neighV4Addr);

  // function wrapper to update RTT for neighbor
  void updateNeighborRtt(
      std::chrono::microseconds const& myRecvTimeInUs,
      std::chrono::microseconds const& mySentTimeInUs,
      std::chrono::microseconds const& nbrRecvTimeInUs,
      std::chrono::microseconds const& nbrSentTimeInUs,
      std::string const& neighborName,
      std::string const& remoteIfName,
      std::string const& ifName);

  //
  // Spark2 related function call
  //
  struct Spark2Neighbor {
    Spark2Neighbor(
        std::string const& domainName,
        std::string const& nodeName,
        std::string const& remoteIfName,
        uint32_t label,
        uint64_t seqNum,
        std::chrono::milliseconds const& samplingPeriod,
        std::function<void(const int64_t&)> rttChangeCb,
        const std::string& area);

    // util function to transfer to SparkNeighbor
    thrift::SparkNeighbor
    toThrift() const {
      thrift::SparkNeighbor res;
      res.domainName = domainName;
      res.nodeName = nodeName;
      res.holdTime = gracefulRestartHoldTime.count();
      res.transportAddressV4 = transportAddressV4;
      res.transportAddressV6 = transportAddressV6;
      res.kvStorePubPort = kvStorePubPort;
      res.kvStoreCmdPort = kvStoreCmdPort;
      res.ifName = remoteIfName;
      return res;
    }

    // doamin name
    const std::string domainName;

    // node name
    const std::string nodeName;

    // interface name
    const std::string remoteIfName;

    // SR Label to reach Neighbor over this specific adjacency. Generated
    // using ifIndex to this neighbor. Only local within the node.
    const uint32_t label{0};

    // Last sequence number received from neighbor
    uint64_t seqNum{0};

    // neighbor state
    SparkNeighState state;

    // timer to periodically send out handshake pkt
    std::unique_ptr<fbzmq::ZmqTimeout> negotiateTimer{nullptr};

    // negotiate stage hold-timer
    std::unique_ptr<fbzmq::ZmqTimeout> negotiateHoldTimer{nullptr};

    // heartbeat hold-timer
    std::unique_ptr<fbzmq::ZmqTimeout> heartbeatHoldTimer{nullptr};

    // graceful restart hold-timer
    std::unique_ptr<fbzmq::ZmqTimeout> gracefulRestartHoldTimer{nullptr};

    // KvStore related port. Info passed to LinkMonitor for neighborEvent
    int32_t kvStorePubPort{0};
    int32_t kvStoreCmdPort{0};
    int32_t openrCtrlThriftPort{0};

    // hold time
    std::chrono::milliseconds heartbeatHoldTime{0};
    std::chrono::milliseconds gracefulRestartHoldTime{0};

    // v4/v6 network address
    thrift::BinaryAddress transportAddressV4;
    thrift::BinaryAddress transportAddressV6;

    // Timestamps of last hello packet received from this neighbor.
    // All timestamps are derived from std::chrono::steady_clock.
    std::chrono::microseconds neighborTimestamp{0};
    std::chrono::microseconds localTimestamp{0};

    // Currently RTT value being used to neighbor. Must be initialized to zero
    std::chrono::microseconds rtt{0};

    // Lastest measured RTT on receipt of every hello packet
    std::chrono::microseconds rttLatest{0};

    // detect rtt changes
    StepDetector<int64_t, std::chrono::milliseconds> stepDetector;

    // area on which adjacency is formed
    std::string area{};
  };

  std::unordered_map<
      std::string /* ifName */,
      std::unordered_map<std::string /* neighborName */, Spark2Neighbor>>
      spark2Neighbors_{};

  // util function to log Spark neighbor state transition
  void logStateTransition(
      std::string const& neighborName,
      std::string const& ifName,
      SparkNeighState const& oldState,
      SparkNeighState const& newState);

  // util function to check SparkNeighState
  void checkNeighborState(
      Spark2Neighbor const& neighbor, SparkNeighState const& state);

  // wrapper call to declare neighborship down
  void neighborUpWrapper(
      Spark2Neighbor& neighbor,
      std::string const& ifName,
      std::string const& neighborName);

  // wrapper call to declare neighborship down
  void neighborDownWrapper(
      Spark2Neighbor const& neighbor,
      std::string const& ifName,
      std::string const& neighborName);

  // utility call to send SparkNeighborEvent
  void notifySparkNeighborEvent(
      thrift::SparkNeighborEventType type,
      std::string const& ifName,
      thrift::SparkNeighbor const& originator,
      int64_t rtt,
      int32_t label,
      bool supportFloodOptimization,
      const std::string& area =
          openr::thrift::KvStore_constants::kDefaultArea());

  // callback function for rtt change
  void processRttChange(
      std::string const& ifName,
      std::string const& neighborName,
      int64_t const newRtt);

  // utility call to send handshake msg
  void sendHandshakeMsg(std::string const& ifName, bool isAdjEstablished);

  // utility call to send heartbeat msg
  void sendHeartbeatMsg(std::string const& ifName);

  // wrapper function to process GR msg
  void processGRMsg(
      std::string const& neighborName,
      std::string const& ifName,
      Spark2Neighbor& neighbor);

  // process helloMsg in Spark2 context
  void processHelloMsg(
      thrift::SparkHelloMsg const& helloMsg,
      std::string const& ifName,
      std::chrono::microseconds const& myRecvTimeInUs);

  // process heartbeatMsg in Spark2 context
  void processHeartbeatMsg(
      thrift::SparkHeartbeatMsg const& heartbeatMsg, std::string const& ifName);

  // process handshakeMsg to update spark2Neighbors_ db
  void processHandshakeMsg(
      thrift::SparkHandshakeMsg const& handshakeMsg, std::string const& ifName);

  // process timeout for heartbeat
  void processHeartbeatTimeout(
      std::string const& ifName, std::string const& neighborName);

  // process timeout for negotiate stage
  void processNegotiateTimeout(
      std::string const& ifName, std::string const& neighborName);

  // process timeout for graceful restart
  void processGRTimeout(
      std::string const& ifName, std::string const& neighborName);

  // Util function to convert ENUM SparlNeighborState to string
  static std::string sparkNeighborStateToStr(SparkNeighState state);

  // Util function for state transition
  static SparkNeighState getNextState(
      folly::Optional<SparkNeighState> const& currState,
      SparkNeighEvent const& event);

  //
  // Private state
  //

  // This node's domain name
  const std::string myDomainName_{};

  // this node's name
  const std::string myNodeName_{};

  // UDP port for send/recv of spark hello messages
  const uint16_t udpMcastPort_{6666};

  // the hold time to announce on all interfaces. Can't be less than 3s
  const std::chrono::milliseconds myHoldTime_{0};

  // hello message (keepAlive) exchange interval. Must be less than holdtime
  // and greater than 0
  const std::chrono::milliseconds myKeepAliveTime_{0};

  // hello message exchange interval during fast init state, much faster than
  // usual keep alive interval
  const std::chrono::milliseconds fastInitKeepAliveTime_{0};

  // Spark2 hello msg sendout interval
  const std::chrono::milliseconds myHelloTime_{0};

  // Spark2 hello msg sendout interval under fast-init case
  const std::chrono::milliseconds myHelloFastInitTime_{0};

  // Spark2 handshake msg sendout interval
  const std::chrono::milliseconds myHandshakeTime_{0};

  // Spark2 heartbeat msg sendout interval
  const std::chrono::milliseconds myHeartbeatTime_{0};

  // Spark2 negotiate stage hold time
  const std::chrono::milliseconds myNegotiateHoldTime_{0};

  // Spark2 heartbeat msg hold time
  const std::chrono::milliseconds myHeartbeatHoldTime_{0};

  // This flag indicates that we will also exchange v4 transportAddress in
  // Spark HelloMessage
  const bool enableV4_{false};

  // If enabled, then all newly formed adjacency will be validated on v4 subnet
  // If subnets are different on each end of adjacency, neighboring session will
  // not be formed
  const bool enableSubnetValidation_{true};

  // the next sequence number to be used on any interface for outgoing hellos
  // NOTE: we increment this on hello sent out of any interfaces
  uint64_t mySeqNum_{1};

  // the multicast socket we use
  int mcastFd_{-1};

  // state transition matrix for Finite-State-Machine
  static const std::vector<std::vector<folly::Optional<SparkNeighState>>>
      stateMap_;

  // Queue to publish neighbor events
  messaging::ReplicateQueue<thrift::SparkNeighborEvent>& neighborUpdatesQueue_;

  // this is used to inform peers about my kvstore tcp ports
  const uint16_t kKvStorePubPort_{0};
  const uint16_t kKvStoreCmdPort_{0};
  const uint16_t kOpenrCtrlThriftPort_{0};

  // current version and supported version
  const thrift::OpenrVersions kVersion_;

  // enable dual or not
  const bool enableFloodOptimization_{false};

  // enable Spark2 or not
  const bool enableSpark2_{false};

  // increase Hello interval in Spark2
  const bool increaseHelloInterval_{false};

  // Map of interface entries keyed by ifName
  std::unordered_map<std::string, Interface> interfaceDb_{};

  // Hello packet send timers for each interface
  std::unordered_map<
      std::string /* ifName */,
      std::unique_ptr<fbzmq::ZmqTimeout>>
      ifNameToHelloTimers_;

  // heartbeat packet send timers for each interface
  std::unordered_map<
      std::string /* ifName */,
      std::unique_ptr<fbzmq::ZmqTimeout>>
      ifNameToHeartbeatTimers_;

  // number of active neighbors for each interface
  std::unordered_map<
      std::string /* ifName */,
      std::unordered_set<std::string> /* neighbors */>
      ifNameToActiveNeighbors_;

  // Ordered set to keep track of allocated labels
  std::set<int32_t> allocatedLabels_;

  //
  // Neighbor state tracking
  //

  // Struct for neighbor information per interface
  struct Neighbor {
    Neighbor(
        thrift::SparkNeighbor const& info,
        uint32_t label,
        uint64_t seqNum,
        std::unique_ptr<fbzmq::ZmqTimeout> holdTimer,
        const std::chrono::milliseconds& samplingPeriod,
        std::function<void(const int64_t&)> rttChangeCb,
        std::string area = openr::thrift::KvStore_constants::kDefaultArea());

    // Neighbor info
    thrift::SparkNeighbor info;

    // Hold timer. If expired will declare the neighbor as stopped.
    const std::unique_ptr<fbzmq::ZmqTimeout> holdTimer{nullptr};

    // SR Label to reach Neighbor over this specific adjacency. Generated
    // using ifIndex to this neighbor. Only local within the node.
    const uint32_t label{0};

    // Last sequence number received from neighbor
    uint64_t seqNum{0};

    // Timestamps of last hello packet received from this neighbor. All
    // timestamps are derived from std::chrono::steady_clock.
    std::chrono::microseconds neighborTimestamp{0};
    std::chrono::microseconds localTimestamp{0};

    // Do we have adjacency with this neighbor. We use this to see if an UP/DOWN
    // notification is needed
    bool isAdjacent{false};

    // counters to track number of restarting packets received
    int numRecvRestarting{0};

    // Currently RTT value being used to neighbor. Must be initialized to zero
    std::chrono::microseconds rtt{0};

    // Lastest measured RTT on receipt of every hello packet
    std::chrono::microseconds rttLatest{0};

    // detect rtt changes
    StepDetector<int64_t, std::chrono::milliseconds> stepDetector;

    // area on which adjacency is formed
    std::string area{};
  };

  std::unordered_map<
      std::string /* ifName */,
      std::unordered_map<std::string /* neighborName */, Neighbor>>
      neighbors_{};

  // to serdeser messages over ZMQ sockets
  apache::thrift::CompactSerializer serializer_;

  // The IO primitives provider; this is used for mocking
  // the IO during unit-tests. This could be shared with other
  // instances, hence the shared_ptr
  std::shared_ptr<IoProvider> ioProvider_{nullptr};

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // vector of BucketedTimeSeries to make sure we don't take too many
  // hello packets from any one iface, address pair
  std::vector<folly::BucketedTimeSeries<int64_t, std::chrono::steady_clock>>
      timeSeriesVector_{};

  // DS to hold local stats/counters
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // areas that this node belongs to.
  folly::Optional<std::unordered_set<std::string>> areas_ = folly::none;
};
} // namespace openr
