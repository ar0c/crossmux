#pragma once
#include "WeReadServiceBatch.h"

namespace WeReadTime {
class ServiceClient {
 public:
  enum class Result { Accepted, Confirmed, Review, Failed, Full };
  static bool configured();
  bool configure(const char* account = nullptr);
  bool connect(const char* account);
  const char* device() const { return device_; }
  const char* account() const { return account_; }
  // Runtime-only transport credential; never log or persist it outside config.
  const char* bearer() const { return token_; }
  Result exchange(const Identity& id, ServiceJournal& journal);
  // Validate the entire ordered response before the worker persists any receipt.
  Result exchangeBatch(const Identity& source, ServiceBatch& batch);
  bool supportsBatch() const { return batchLimit_ >= ServiceBatch::kMaxItems; }

 private:
  char account_[32]{}, device_[32]{}, token_[128]{};
  unsigned batchLimit_ = 0;
};
}  // namespace WeReadTime
