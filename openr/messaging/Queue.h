/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <any>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include <folly/Expected.h>
#include <folly/fibers/Baton.h>
#if FOLLY_HAS_COROUTINES
#include <folly/experimental/coro/Task.h>
#endif

namespace openr {
namespace messaging {

enum class QueueError {
  QUEUE_CLOSED,
};

template <typename ValueType>
class RWQueue;

/**
 * Read-only interface for RWQueue class.
 */
template <typename ValueType>
class RQueue {
 public:
  explicit RQueue(std::shared_ptr<RWQueue<ValueType>> queue);
  virtual ~RQueue() {}

  /**
   * Blocking read for native threads/fibers. In-case of fibers, the fiber
   * performing blocking read will be suspended.
   */
  folly::Expected<ValueType, QueueError> get();

#if FOLLY_HAS_COROUTINES
  /**
   * Read methods for co-routines
   */
  folly::coro::Task<folly::Expected<ValueType, QueueError>> getCoro();
#endif

  // Utility function to retrieve size of pending data in underlying queue
  size_t size();

 protected:
  // We only hold reference of above queue
  std::shared_ptr<RWQueue<ValueType>> queue_{nullptr};
};

/**
 * Multiple writers and readers. We use lock internally to protect the data.
 * Code in critical path is minimal and ensures that readers/writers will never
 * block each other because of lock.
 *
 * This is polymorphic queue which means, you can push any type of object. There
 * are various get (blocking and async) methods to retrieve typed object.
 *
 * After closing queue, all subsequent push are ignored and return false. All
 * subsequent reads return QUEUE_CLOSED error
 */
template <typename ValueType>
class RWQueue {
 public:
  RWQueue();
  ~RWQueue();

  /**
   * Non blocking push. Any typed value can be pushed!
   * Return true/false!!
   */
  template <typename ValueTypeT>
  bool push(ValueTypeT&& val);

  /**
   * Blocking read for native threads/fibers. In-case of fibers, the fiber
   * performing blocking read will be suspended.
   */
  folly::Expected<ValueType, QueueError> get();

#if FOLLY_HAS_COROUTINES
  /**
   * Read methods for co-routines
   */
  folly::coro::Task<folly::Expected<ValueType, QueueError>> getCoro();
#endif

  /**
   * Close the queue. All new push will be ignored and pending data will be lost
   */
  void close();
  bool isClosed();

  /**
   * Return size of the current queue (number of data elements)
   */
  size_t size();

  /**
   * Return number of active reads
   */
  size_t numPendingReads();

 private:
  struct PendingRead {
    folly::fibers::Baton baton;
    folly::Optional<ValueType> data;
  };

  /**
   * Implementation for push
   */
  bool getAnyImpl(PendingRead& pendingRead);

  // Lock to protect below private variables
  std::mutex lock_;

  // State of queue
  bool closed_{false};

  // Pending reads - readers are actively waiting for data
  std::deque<std::reference_wrapper<PendingRead>> pendingReads_;

  // Pending data
  std::deque<ValueType> queue_;
};

} // namespace messaging
} // namespace openr

#include <openr/messaging/Queue-inl.h>
