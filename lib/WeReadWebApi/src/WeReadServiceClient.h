#pragma once
#include "WeReadServiceJournal.h"

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

 private:
  char account_[32]{}, device_[32]{}, token_[128]{};
};
}  // namespace WeReadTime
