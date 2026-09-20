#pragma once
#include <HalStorage.h>

#include "WeReadServiceJournal.h"
namespace fakeService {
inline bool authenticated = true, confirmed = false, fail = false, review = false, full = false;
inline unsigned requests = 0, posts = 0, gets = 0;
inline uint64_t lastStart = 0, lastEnd = 0;
}  // namespace fakeService
namespace WeReadTime {
class ServiceClient {
 public:
  enum class Result { Accepted, Confirmed, Review, Failed, Full };
  static bool configured() { return Storage.exists("/WeReadSync/service.conf"); }
  bool connect(const char*) { return fakeService::authenticated; }
  const char* device() const { return "abcdef012345678901234567"; }
  Result exchange(const Identity&, ServiceJournal& j) {
    ++fakeService::requests;
    if (j.state() == ServiceJournal::State::Reserved)
      ++fakeService::posts;
    else
      ++fakeService::gets;
    fakeService::lastStart = j.start();
    fakeService::lastEnd = j.end();
    if (fakeService::fail) return Result::Failed;
    if (fakeService::full && j.state() == ServiceJournal::State::Reserved) return Result::Full;
    if (!j.accept(fakeService::confirmed)) return Result::Failed;
    if (fakeService::review) return Result::Review;
    return fakeService::confirmed ? Result::Confirmed : Result::Accepted;
  }
};
}  // namespace WeReadTime
