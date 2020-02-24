/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/config-store/PersistentStore.h>

namespace openr {

class PersistentStoreWrapper {
 public:
  PersistentStoreWrapper(
      fbzmq::Context& context,
      const unsigned long tid,
      // persistent store DB saving backoffs
      std::chrono::milliseconds saveInitialBackoff =
          Constants::kPersistentStoreInitialBackoff,
      std::chrono::milliseconds saveMaxBackoff =
          Constants::kPersistentStoreMaxBackoff);

  // Destructor will try to save DB to disk before destroying the object
  ~PersistentStoreWrapper() {
    stop();
  }

  PersistentStore* operator->() {
    return store_.get();
  }

  /**
   * Synchronous APIs to run and stop PersistentStore. This creates a thread
   * and stop it on destruction.
   *
   * Synchronous => function call with return only after thread is
   *                running/stopped completely.
   */
  void run() noexcept;
  void stop();

 public:
  const std::string nodeName;
  const std::string filePath;

 private:
  std::unique_ptr<PersistentStore> store_;

  // Thread in which PersistentStore will be running.
  std::thread storeThread_;
};
} // namespace openr
