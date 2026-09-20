#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "WeReadTimeDiagnostic.h"
#include "WeReadTimeQueue.h"

struct ReadingDayStats;

namespace WeReadTimeSync {
struct Source {
  const char* book;
  const char* source;
  const ReadingDayStats* days;
  size_t count;
  uint64_t totalMs;
};

struct Totals {
  uint64_t servicePending = 0, serviceConfirmed = 0;
  uint64_t serviceCheckedAt = 0;
  bool serviceMode = false;
  uint64_t pending = 0, externalConfirmed = 0, externalUnknown = 0;
  uint64_t deviceConfirmed = 0, deviceUnknown = 0;
  uint32_t selectedDay = 0;
  bool hostPaused = false;
};

// Shared accounting implementation. The caller owns a stable source view.
class Accounting {
 public:
  Accounting();
  ~Accounting();
  void clear();
  bool audit(const Source& source, const char* account, Totals& totals, WeReadTime::ExternalTime* selected = nullptr,
             uint64_t* measured = nullptr);

 private:
  struct Scratch;
  std::unique_ptr<Scratch> scratch_;
};

struct Status {
  bool available = false, running = false, auditFailed = false;
  bool serviceQueueFull = false;
  uint32_t revision = 0;
  Totals totals;
  WeReadTime::TimeQueue::State queue = WeReadTime::TimeQueue::State::Idle;
  WeReadTime::TimeTransaction::State phase = WeReadTime::TimeTransaction::State::Idle;
  WeReadTime::TimeTransaction::Issue issue = WeReadTime::TimeTransaction::Issue::None;
  uint64_t confirmed = 0;
  uint32_t waitSeconds = 0;
  uint8_t retries = 0;
  WeReadTime::Diagnostic diagnostic;
};

// Safe on the render task: reads only the published state, without allocation.
bool showSyncIndicator();

// Other public calls are made on the main task. Only published Status crosses
// tasks; the worker never references an Activity, renderer or ReadingStatsStore.
bool start(const Source& source, const char* account);
enum class StartFailure : uint8_t { None, Busy, InvalidSource, Network, Headroom, JobMemory, SourceMemory, TaskMemory };
StartFailure lastStartFailure();  // Main task only; separate from a running job's status.
bool active();
bool status(const char* book, Status& result, const char* account = nullptr);
void dismiss(const char* book);
void pause();
void poll();  // Reap finished work; retain only bounded, credential-free status.
// Defer conflicting navigation/sleep until a requested pause has drained TLS
// and journal I/O. Never kill a task while it owns an SD or transport lock.
bool prepareToLeaveReading();
bool canContinueIn(const char* activityName, bool reader, bool home);
bool ownsWifi();
void releaseWifi();
}  // namespace WeReadTimeSync
