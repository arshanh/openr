/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IosxrslFibHandler.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>

#include <openr/common/NetworkUtil.h>

using apache::thrift::FRAGILE;

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {

const uint8_t kAqRouteProtoId = 99;

namespace {

// convert a routeDb into thrift exportable route spec
std::vector<thrift::UnicastRoute>
makeRoutes(const std::unordered_map<
           folly::CIDRNetwork,
           std::unordered_set<std::pair<std::string, folly::IPAddress>>>&
               routeDb) {
  std::vector<thrift::UnicastRoute> routes;

  for (auto const& kv : routeDb) {
    auto const& prefix = kv.first;
    auto const& nextHops = kv.second;

    auto binaryNextHops = from(nextHops) |
        mapped([](const std::pair<std::string, folly::IPAddress>& nextHop) {
                            VLOG(2)
                                << "mapping next-hop " << nextHop.second.str()
                                << " dev " << nextHop.first;
                            auto binaryAddr = toBinaryAddress(nextHop.second);
                            binaryAddr.ifName = nextHop.first;
                            return binaryAddr;
                          }) |
        as<std::vector>();

    routes.emplace_back(thrift::UnicastRoute(
        apache::thrift::FRAGILE,
        thrift::IpPrefix(
            apache::thrift::FRAGILE,
            toBinaryAddress(prefix.first),
            static_cast<int16_t>(prefix.second)),
        std::move(binaryNextHops)));
  }
  return routes;
}

std::string
getClientName(const int16_t clientId) {
  auto it = thrift::_FibClient_VALUES_TO_NAMES.find(
      static_cast<thrift::FibClient>(clientId));
  if (it == thrift::_FibClient_VALUES_TO_NAMES.end()) {
    return folly::sformat("UNKNOWN(id={})", clientId);
  }
  return it->second;
}

} // namespace

void
IosxrslFibHandler::setVrfContext(std::string vrfName)
{
  // The VRF context gets set for all following operations
  iosxrslRshuttle_->setIosxrslRouteVrf(vrfName);
}


IosxrslFibHandler::IosxrslFibHandler(fbzmq::ZmqEventLoop* zmqEventLoop,
                                     std::vector<VrfData> vrfSet,
                                     std::shared_ptr<grpc::Channel> Channel)
  :startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count())
{
   iosxrslRshuttle_ = std::make_unique<IosxrslRshuttle>(zmqEventLoop,
                                        vrfSet,
                                        kAqRouteProtoId,
                                        Channel);

}

folly::Future<folly::Unit>
IosxrslFibHandler::future_addUnicastRoute(
    int16_t, std::unique_ptr<thrift::UnicastRoute> route) {
  DCHECK(route->nexthops.size());

  auto prefix = std::make_pair(
      toIPAddress(route->dest.prefixAddress), route->dest.prefixLength);

  auto newNextHops =
      from(route->nexthops) | mapped([](const thrift::BinaryAddress& addr) {
        return std::make_pair(addr.ifName.value(), toIPAddress(addr));
      }) |
      as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

  LOG(INFO)
      << "Adding route for " << folly::IPAddress::networkToString(prefix)
      << " via "
      << folly::join(
             ", ",
             from(route->nexthops) |
                 mapped([](const thrift::BinaryAddress& addr) {
                   return (toIPAddress(addr).str() + "@" + addr.ifName.value());
                 }) |
                 as<std::set<std::string>>());

  return iosxrslRshuttle_->addUnicastRoute(prefix, newNextHops);
}

folly::Future<folly::Unit>
IosxrslFibHandler::future_deleteUnicastRoute(
    int16_t, std::unique_ptr<thrift::IpPrefix> prefix) {
  auto myPrefix =
      std::make_pair(toIPAddress(prefix->prefixAddress), prefix->prefixLength);

  LOG(INFO) << "Deleting route for "
            << folly::IPAddress::networkToString(myPrefix);

  return iosxrslRshuttle_->deleteUnicastRoute(myPrefix);
}

folly::Future<folly::Unit>
IosxrslFibHandler::future_addUnicastRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Adding routes to FIB. Client: " << getClientName(clientId);

  std::vector<folly::Future<folly::Unit>> futures;
  for (auto& route : *routes) {
    auto routePtr = folly::make_unique<thrift::UnicastRoute>(std::move(route));
    auto future = future_addUnicastRoute(clientId, std::move(routePtr));
    futures.emplace_back(std::move(future));
  }
  // Return an aggregate future which is fulfilled when all routes are added
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  folly::collectAll(futures).then(
      [promise = std::move(promise)]() mutable { promise.setValue(); });
  return future;
}

folly::Future<folly::Unit>
IosxrslFibHandler::future_deleteUnicastRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) {
  LOG(INFO) << "Deleting routes from FIB. Client: " << getClientName(clientId);

  std::vector<folly::Future<folly::Unit>> futures;
  for (auto& prefix : *prefixes) {
    auto prefixPtr = folly::make_unique<thrift::IpPrefix>(std::move(prefix));
    auto future = future_deleteUnicastRoute(clientId, std::move(prefixPtr));
    futures.emplace_back(std::move(future));
  }
  // Return an aggregate future which is fulfilled when all routes are deleted
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  folly::collectAll(futures).then(
      [promise = std::move(promise)]() mutable { promise.setValue(); });
  return future;
}

folly::Future<folly::Unit>
IosxrslFibHandler::future_syncFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Syncing FIB with provided routes. Client: "
            << getClientName(clientId);

  // Build new routeDb
  UnicastRoutes newRouteDb;

  for (auto const& route : *routes) {
    if (route.nexthops.size() == 0) {
      LOG(ERROR) << "Got empty nexthops for prefix " << toString(route.dest)
                 << " ... Skipping";
      continue;
    }

    auto prefix = std::make_pair(
        toIPAddress(route.dest.prefixAddress), route.dest.prefixLength);
    auto newNextHops =
        from(route.nexthops) | mapped([](const thrift::BinaryAddress& addr) {
          return std::make_pair(addr.ifName.value(), toIPAddress(addr));
        }) |
        as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();
    newRouteDb[prefix] = newNextHops;
  }

  return iosxrslRshuttle_->syncRoutes(newRouteDb);
}

folly::Future<int64_t>
IosxrslFibHandler::future_periodicKeepAlive(int16_t /* clientId */) {
  VLOG(3) << "Received KeepAlive from OpenR";
  return folly::makeFuture(keepAliveId_++);
}

int64_t
IosxrslFibHandler::aliveSince() {
  return startTime_;
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
IosxrslFibHandler::future_getRouteTableByClient(int16_t clientId) {
  LOG(INFO) << "Get routes from FIB for clientId " << clientId;

  return iosxrslRshuttle_->getUnicastRoutes()
      .then([](UnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(res));
      })
      .onError([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get routing table by client: " << ex.what()
                   << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(UnicastRoutes({})));
      });
}

void
IosxrslFibHandler::getCounters(std::map<std::string, int64_t>& counters) {
  LOG(INFO) << "Get counters requested";
  auto routes = iosxrslRshuttle_->getUnicastRoutes().get();
  counters["fibagent.num_of_routes"] = routes.size();
}

} // namespace openr
