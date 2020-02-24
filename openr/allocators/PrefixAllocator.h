/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/PrefixManager_types.h>
#include <openr/if/gen-cpp2/SystemService.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/messaging/ReplicateQueue.h>

#include "RangeAllocator.h"

namespace openr {

enum class PrefixAllocatorModeStatic {};
enum class PrefixAllocatorModeSeeded {};

using PrefixAllocatorParams = std::pair<folly::CIDRNetwork, uint8_t>;
using PrefixAllocatorMode = std::variant<
    PrefixAllocatorModeStatic,
    PrefixAllocatorModeSeeded,
    PrefixAllocatorParams>;

/**
 * The class assigns local node unique prefixes from a given seed prefix in
 * a distributed manner.
 *
 * allocOptions:
 * > PrefixAllocatorModeStatic
 *   => looks for static allocation key in kvstore and use the prefix
 * > PrefixAllocatorModeSeeded
 *   => looks for PrefixAllocatorParams in kvstore and elects a subprefix
 * > PrefixAllocatorParams
 *   => elects subprefix from prefix allocator params
 */
class PrefixAllocator : public OpenrEventBase {
 public:
  PrefixAllocator(
      const std::string& myNodeName,
      const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
      const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
      messaging::ReplicateQueue<thrift::PrefixUpdateRequest>& prefixUpdatesQ,
      const MonitorSubmitUrl& monitorSubmitUrl,
      const AllocPrefixMarker& allocPrefixMarker,
      // Allocation params
      const PrefixAllocatorMode& allocMode,
      // configure loopback address or not
      bool setLoopbackAddress,
      // override all global addresses on loopback interface
      bool overrideGlobalAddress,
      // loopback interface name
      const std::string& loopbackIfaceName,
      // prefix fowrading type MPLS
      bool forwardingTypeMpls,
      // prefix forwarding algorithm KSP2_ED_ECMP
      bool forwardingAlgoKsp2Ed,
      // period to check prefix collision
      std::chrono::milliseconds syncInterval,
      PersistentStore* configStore,
      fbzmq::Context& zmqContext,
      int32_t systemServicePort);

  PrefixAllocator(PrefixAllocator const&) = delete;
  PrefixAllocator& operator=(PrefixAllocator const&) = delete;

  //
  // Visitor init functions => 3 different ways to initialize
  // PrefixAllocator. Only meant to be used internally.
  //
  void operator()(PrefixAllocatorModeStatic const&);
  void operator()(PrefixAllocatorModeSeeded const&);
  void operator()(PrefixAllocatorParams const&);

  // Thread safe API for testing only
  folly::Optional<uint32_t> getMyPrefixIndex();

  // Static function to parse string representation of allocation params to
  // strong types.
  static folly::Expected<PrefixAllocatorParams, fbzmq::Error> parseParamsStr(
      const std::string& paramStr) noexcept;

  // Static function to get available prefix count from allocation params
  static uint32_t getPrefixCount(
      PrefixAllocatorParams const& allocParams) noexcept;

 private:
  //
  // Private methods
  //

  // Function to process static allocation update from kvstore
  void processStaticPrefixAllocUpdate(thrift::Value const& value);

  //  Function to process allocation param update from kvstore
  void processAllocParamUpdate(thrift::Value const& value);

  // calculate and save alloc index obtained from e2e-network-allocation
  void processNetworkAllocationsUpdate(thrift::Value const& value);

  // check if index is already in use by e2e-network-allocations
  bool checkE2eAllocIndex(uint32_t index);

  // get my existing prefix index from kvstore if it's present
  folly::Optional<uint32_t> loadPrefixIndexFromKvStore();

  // load prefix index from disk
  folly::Optional<uint32_t> loadPrefixIndexFromDisk();

  // save newly elected prefix index to disk
  void savePrefixIndexToDisk(folly::Optional<uint32_t> prefixIndex);

  // initialize my prefix
  uint32_t getInitPrefixIndex();

  // start allocating prefixes, can be called again with new prefix
  // or `folly::none` if seed prefix is no longer valid to withdraw
  // what we had before!
  void startAllocation(
      folly::Optional<PrefixAllocatorParams> const& allocParams,
      bool checkParams = true);

  // use my newly allocated prefix
  void applyMyPrefixIndex(folly::Optional<uint32_t> prefixIndex);
  void applyMyPrefix();

  // update prefix
  void updateMyPrefix(folly::CIDRNetwork prefix);

  // withdraw prefix
  void withdrawMyPrefix();

  void logPrefixEvent(
      std::string event,
      folly::Optional<uint32_t> oldPrefix,
      folly::Optional<uint32_t> newPrefix,
      folly::Optional<PrefixAllocatorParams> const& oldParams = folly::none,
      folly::Optional<PrefixAllocatorParams> const& newParams = folly::none);

  void syncIfaceAddrs(
      const std::string& ifName,
      int family,
      int scope,
      const std::vector<folly::CIDRNetwork>& prefixes);

  void delIfaceAddr(
      const std::string& ifName, const folly::CIDRNetwork& prefix);

  void getIfacePrefixes(
      const std::string& iface,
      int family,
      std::vector<folly::CIDRNetwork>& addrs);

  // Create client when necessary
  void createThriftClient(
      folly::EventBase& evb,
      std::shared_ptr<apache::thrift::async::TAsyncSocket>& socket,
      std::unique_ptr<thrift::SystemServiceAsyncClient>& client,
      int32_t port);

  //
  // Const private variables
  //

  // this node's name
  const std::string myNodeName_{};

  // this node's key marker for prefix allocation
  const std::string allocPrefixMarker_{};

  // Parameter to set loopback addresses
  const bool setLoopbackAddress_{false};
  const bool overrideGlobalAddress_{false};
  const std::string loopbackIfaceName_;
  const bool forwardingTypeMpls_{false};
  const bool forwardingAlgoKsp2Ed_{false};

  // Sync interval for range allocator
  const std::chrono::milliseconds syncInterval_;

  // hash node ID into prefix space
  const std::hash<std::string> hasher{};

  //
  // Non-const private variables
  //

  // Allocation parameters e.g., fc00:cafe::/56, 64
  folly::Optional<PrefixAllocatorParams> allocParams_;

  // index of my currently claimed prefix within seed prefix
  folly::Optional<uint32_t> myPrefixIndex_;

  apache::thrift::CompactSerializer serializer_;

  // we'll use this to get the full dump from the KvStore
  // and get and set my assigned prefix
  std::unique_ptr<KvStoreClient> kvStoreClient_{nullptr};

  // client to interact with ConfigStore
  PersistentStore* configStore_{nullptr};

  // RangAlloctor to get unique prefix index for this node
  std::unique_ptr<RangeAllocator<uint32_t>> rangeAllocator_;

  // Queue to send prefix update request to PrefixManager
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest>& prefixUpdatesQueue_;

  // Monitor client for submitting counters/logs
  fbzmq::ZmqMonitorClient zmqMonitorClient_;

  // Thriftclient for system service
  int32_t systemServicePort_{0};
  folly::EventBase evb_;
  std::shared_ptr<apache::thrift::async::TAsyncSocket> socket_{nullptr};
  std::unique_ptr<thrift::SystemServiceAsyncClient> client_{nullptr};

  // AsyncTimeout for initialization
  std::unique_ptr<folly::AsyncTimeout> initTimer_;

  /**
   * applyMyPrefix use this state to decide how to program address to kernel
   * boolean field means the address is beed applied or not.
   * When Optional value is empty, it means cleanup addresses on the iface
   * otherwise applys the Optional value to the iface
   */
  std::pair<bool, folly::Optional<folly::CIDRNetwork>> applyState_;

  // save alloc index from e2e-network-alllocation <value version, indices set>
  std::pair<int64_t, std::unordered_set<uint32_t>> e2eAllocIndex_{-1, {}};

  // areas
  const std::string area_{openr::thrift::KvStore_constants::kDefaultArea()};
};

} // namespace openr
