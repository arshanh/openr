/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/messaging/Queue.h>

namespace openr {
namespace messaging {

/**
 * Multiple writers and readers. Each reader gets every written element push by
 * every writer. Writer pays the cost of replicating data to all readers. If no
 * reader exists then all the messages are silently dropped.
 *
 * Pushed object must be copy constructible.
 */
template <typename ValueType>
class ReplicateQueue {
 public:
  ReplicateQueue();

  ~ReplicateQueue();

  /**
   * non-copyable
   */
  ReplicateQueue(ReplicateQueue const&) = delete;
  ReplicateQueue& operator=(ReplicateQueue const&) = delete;

  /**
   * movable
   */
  ReplicateQueue(ReplicateQueue&&) = default;
  ReplicateQueue& operator=(ReplicateQueue&&) = default;

  /**
   * Push any value into the queue. Will get replicated to all the readers.
   * This also cleans up any lingering queue which has no active reader
   */
  template <typename ValueTypeT>
  bool push(ValueTypeT&& value);

  /**
   * Get new reader stream of this queue. Stream will get closed automatically
   * when reader is destructed.
   */
  RQueue<ValueType> getReader();

  /**
   * Number of replicated streams/readers
   */
  size_t getNumReaders();

  /**
   * Close the underlying queue. All subsequent writes and reads will fails.
   */
  void close();

 private:
  folly::Synchronized<std::list<std::shared_ptr<RWQueue<ValueType>>>> readers_;
  bool closed_{false}; // Protected by above Synchronized lock
};

} // namespace messaging
} // namespace openr

#include <openr/messaging/ReplicateQueue-inl.h>
