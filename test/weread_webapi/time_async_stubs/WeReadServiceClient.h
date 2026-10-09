#pragma once
#include <HalStorage.h>

#include <condition_variable>
#include <mutex>

#include "WeReadServiceBatch.h"
namespace fakeService {
inline bool authenticated = true, confirmed = false, fail = false, review = false, full = false;
inline unsigned requests = 0, posts = 0, gets = 0;
inline uint64_t lastStart = 0, lastEnd = 0;
inline unsigned batchRequests = 0;
inline bool blockBatch = false, enteredBatch = false;
inline bool batchSupported = true;
inline unsigned batchElapsedMs = 0;
inline std::mutex mutex;
inline std::condition_variable cv;
}  // namespace fakeService
namespace WeReadTime {
class ServiceClient {
 public:
  enum class Result { Accepted, Confirmed, Review, Failed, Full };
  static bool configured() { return Storage.exists("/WeReadSync/service.conf"); }
  bool connect(const char*) { return fakeService::authenticated; }
  bool supportsBatch() const { return fakeService::batchSupported; }
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
  Result exchangeBatch(const Identity&, ServiceBatch& batch) {
    ++fakeService::batchRequests;
    testTick.fetch_add(fakeService::batchElapsedMs);
    {
      std::unique_lock<std::mutex> lock(fakeService::mutex);
      fakeService::enteredBatch = true;
      fakeService::cv.notify_all();
      fakeService::cv.wait(lock, [] { return !fakeService::blockBatch; });
    }
    bool submit = false;
    for (size_t i = 0; i < batch.count; ++i) {
      auto& item = batch.items[i];
      ++fakeService::requests;
      if (item.action == ServiceBatch::Action::Submit) {
        ++fakeService::posts;
        submit = true;
        fakeService::lastStart = item.start;
        fakeService::lastEnd = item.end;
      } else {
        ++fakeService::gets;
      }
      item.receivedCredit = fakeService::confirmed ? item.end - item.start : item.credit;
      item.receipt = fakeService::confirmed
                         ? ServiceBatch::Receipt::Confirmed
                         : (fakeService::review ? ServiceBatch::Receipt::Uncertain : ServiceBatch::Receipt::Queued);
    }
    if (fakeService::fail) return Result::Failed;
    if (fakeService::full && submit) return Result::Full;
    return fakeService::review ? Result::Review : Result::Accepted;
  }
};
}  // namespace WeReadTime
