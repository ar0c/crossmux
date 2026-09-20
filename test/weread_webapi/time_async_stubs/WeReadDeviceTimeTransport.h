#pragma once
#include <condition_variable>
#include <mutex>

#include "Arduino.h"
#include "WeReadTimeDiagnostic.h"
#include "WeReadTimeTransaction.h"
namespace fakeTransport {
inline std::mutex mutex;
inline std::condition_variable cv;
inline bool blockPrepare = false, blockReport = false, enteredPrepare = false, enteredReport = false;
inline bool unknown = false, failPrepare = false;
inline unsigned reports = 0, seconds = 0;
inline void reset() {
  blockPrepare = blockReport = enteredPrepare = enteredReport = unknown = failPrepare = false;
  reports = seconds = 0;
}
inline void wait(bool& blocked, bool& entered) {
  std::unique_lock<std::mutex> lock(mutex);
  entered = true;
  cv.notify_all();
  cv.wait(lock, [&] { return !blocked; });
}
}  // namespace fakeTransport
namespace WeReadClient {
class DeviceTimeTransport final : public WeReadTime::TimeTransport {
#ifdef BOARD_HAS_PSRAM
  // Model the production transport's fixed workspace for fragmentation tests.
  [[maybe_unused]] char workspace_[16000]{};
#endif
 public:
  Read prepare(const WeReadTime::Identity& id) override {
    if (strcmp(id.account, "a") || strcmp(id.book, "b")) return Read::Failed;
    fakeTransport::wait(fakeTransport::blockPrepare, fakeTransport::enteredPrepare);
    return fakeTransport::failPrepare ? Read::Failed : Read::Ready;
  }
  Read snapshot(WeReadTime::AccountSnapshot& out) override {
    out = {};
    strcpy(out.account, "a");
    out.month = 1788192000;
    out.day = 1789401600;
    out.monthSeconds = 16234 + fakeTransport::seconds;
    out.daySeconds = 2947 + fakeTransport::seconds;
    out.sampledAt = epochSeconds();
    out.complete = true;
    return Read::Ready;
  }
  Write enter() override { return Write::Accepted; }
  Write report(uint32_t seconds) override {
    ++fakeTransport::reports;
    fakeTransport::seconds += seconds;
    fakeTransport::wait(fakeTransport::blockReport, fakeTransport::enteredReport);
    return fakeTransport::unknown ? Write::Unknown : Write::Accepted;
  }
  uint64_t epochSeconds() const override { return 1789453168 + millis() / 1000; }
  uint32_t monotonicMs() const override { return millis(); }
  WeReadTime::Diagnostic diagnostic() const { return {}; }
};
}  // namespace WeReadClient
