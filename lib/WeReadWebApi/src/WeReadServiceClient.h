#pragma once
#include "WeReadServiceJournal.h"

namespace WeReadTime {
class ServiceClient {
 public:
  enum class Result { Accepted, Confirmed, Review, Failed, Full };
  static bool configured();
  bool connect(const char* account);
  const char* device() const { return device_; }
  Result exchange(const Identity& id, ServiceJournal& journal);

 private:
  char account_[32]{}, device_[32]{}, token_[128]{};
};
}  // namespace WeReadTime
