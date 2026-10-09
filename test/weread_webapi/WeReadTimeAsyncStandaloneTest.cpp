#include <freertos/task.h>

#ifdef BOARD_HAS_PSRAM
#include <esp_heap_caps.h>
#endif
#include <cassert>
#include <chrono>
#include <iostream>

#include "HalSystem.h"
#include "ReadingStatsStore.h"
#include "WeReadDeviceTimeSource.h"
#include "WeReadDeviceTimeTransport.h"
#include "WeReadServiceClient.h"
#include "WeReadTimeStorage.h"
#include "WeReadTimeSync.h"
#include "WiFi.h"
using namespace WeReadTime;
namespace Sync = WeReadTimeSync;

void fixture(uint64_t ms) {
  assert(!Sync::active());
  fakeTask::join();
  Sync::poll();
  Sync::dismiss("b");
  fakeStorage::reset();
  fakeTransport::reset();
  WiFi.connected = true;
  SdByteLog log;
  Journal legacy{log};
  assert(log.configure("a", "b", "s", 20709));
  assert(legacy.open("a", "b", "s", 20709) && legacy.collect(ms));
  uint8_t frame[240]{};
  assert(legacy.ledger().encode(frame, sizeof(frame)));
  memcpy(frame, "WRTX", 4);
  // Zero external coverage, all duration is verifiable local time.
  for (size_t i = 184; i < 232; ++i) frame[i] = 0;
  const auto crc = ExternalTime::checksum(frame, 232);
  for (unsigned i = 0; i < 8; ++i) frame[232 + i] = uint8_t(uint64_t(crc) >> (8 * i));
  fakeStorage::files["/weread-external-a-s-20709.bin"] = {frame, frame + 240};
  const std::string manifest =
      "{\n  \"account\": \"a\",\n  \"book\": \"b\",\n  \"receipts\": [\n"
      "    {\n      \"day\": 20709,\n      \"receipt\": \"weread-external-a-s-20709.bin\",\n"
      "      \"sha256\": \"" +
      std::string(64, 'a') +
      "\"\n    }\n  ],\n  \"schema\": 1,\n  \"sender_enabled\": false,\n"
      "  \"source\": \"s\",\n  \"state\": \"prepared\"\n}";
  fakeStorage::files[kLegacyTimeManifest] = {manifest.begin(), manifest.end()};
}
Sync::Status finish() {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  unsigned responsiveTicks = 0;
  while (Sync::active() && std::chrono::steady_clock::now() < deadline) {
    Sync::Status s;
    Sync::status("b", s);
    ++responsiveTicks;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(!Sync::active());
  fakeTask::join();
  Sync::poll();
  assert(!Sync::showSyncIndicator());
  assert(!Sync::ownsWifi() && WiFi.status() != WL_CONNECTED);
  Sync::Status s;
  assert(Sync::status("b", s) && !s.running);
  (void)responsiveTicks;
  return s;
}
void waitFor(bool& entered) {
  std::unique_lock<std::mutex> lock(fakeTransport::mutex);
  assert(fakeTransport::cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
}
void unblock(bool& blocked) {
  std::lock_guard<std::mutex> lock(fakeTransport::mutex);
  blocked = false;
  fakeTransport::cv.notify_all();
}
int main() {
  assert(!Sync::showSyncIndicator());
  assert(Sync::canContinueIn("EpubReader", true, false));
  assert(Sync::canContinueIn("Home", false, true));
  assert(Sync::canContinueIn("WeReadProgressSync", false, false));
  assert(Sync::canContinueIn("EpubReaderMenu", false, false));
  for (const char* name :
       {"Settings", "WeRead", "WifiSelection", "UsbDrive", "KOReaderSync", "Sleep", "EpubReaderNetwork"}) {
    assert(!Sync::canContinueIn(name, false, false));
  }

#ifdef BOARD_HAS_PSRAM
  // Measured X4 Pro rejection: enough total memory, fragmented internal heap.
  ReadingDayStats probeDay{20709, 125000};
  Sync::Source probe{"b", "s", &probeDay, 1, probeDay.readingMs};
  fixture(probeDay.readingMs);
  ESP.free = 144536;
  ESP.largest = 47092;
  // The fake transport includes its fixed production-sized workspace.
  // Without PSRAM the task allocations leave too little contiguous TLS room.
  fakeTask::largestAfterCreate = 32760;
  assert(!Sync::start(probe, "a"));
  assert(Sync::lastStartFailure() == Sync::StartFailure::Headroom);
  fakeTask::largestAfterCreate = 0;
  fakePsram::available = true;
  ESP.largest = 47092;
  fakeTransport::blockPrepare = true;
  assert(Sync::start(probe, "a"));
  const auto& startDiagnostic = fakeStorage::files.at("/WeReadSync/last-start-diagnostic.json");
  assert(std::string(startDiagnostic.begin(), startDiagnostic.end()).find("\"reason\":0") != std::string::npos);
  waitFor(fakeTransport::enteredPrepare);
  assert(fakePsram::allocations > 0);
  Sync::pause();
  unblock(fakeTransport::blockPrepare);
  finish();
  fixture(probeDay.readingMs);
  // Captured Waveshare startup: a 40,948-byte largest block is 12 bytes below
  // the old combined stack+TLS threshold. Starting is safe if the separate
  // task-stack allocation leaves the TLS reserve intact.
  ESP.free = 139884;
  ESP.largest = 40948;
  fakeTask::freeAfterCreate = 126660;
  fakeTask::largestAfterCreate = 32760;
  assert(!Sync::start(probe, "a"));
  assert(Sync::lastStartFailure() == Sync::StartFailure::Headroom);
  assert(!Sync::active() && !Sync::ownsWifi());
  assert(fakeTask::lastStackBytes == 12 * 1024);
  fakeTask::largestAfterCreate = 0;
  ESP.free = 139884;
  ESP.largest = 40948;
  fakeTransport::blockPrepare = true;
  assert(Sync::start(probe, "a"));
  waitFor(fakeTransport::enteredPrepare);
  Sync::pause();
  unblock(fakeTransport::blockPrepare);
  finish();
  fakeTask::freeAfterCreate = 0;
  fixture(probeDay.readingMs);
  fakePsram::fail = true;
  fakeTask::largestAfterCreate = 32760;
  assert(!Sync::start(probe, "a"));
  assert(Sync::lastStartFailure() == Sync::StartFailure::Headroom);
  fakeTask::largestAfterCreate = 0;
  fakePsram::fail = false;
  fakePsram::available = false;
  ESP.free = 1024 * 1024;
  ESP.largest = 512 * 1024;
#endif
  ReadingDayStats day{20709, 125000};
  Sync::Source source{"b", "s", &day, 1, day.readingMs};
  fixture(day.readingMs);
  {
    Sync::Accounting accounting;
    Sync::Totals totals;
    assert(accounting.audit(source, "a", totals));
    assert(totals.pending == 125 && totals.selectedDay == 20709);
  }
  fakeTask::failCreate = true;
  assert(!Sync::start(source, "a") && !Sync::active() && !Sync::ownsWifi());
  assert(!Sync::showSyncIndicator());
  Sync::Status failedStart;
  assert(!Sync::status("b", failedStart));
  assert(Sync::lastStartFailure() == Sync::StartFailure::TaskMemory);
  assert(fakeStorage::files.count("/WeReadSync/last-start-diagnostic.json"));
  fakeTask::failCreate = false;
  ESP.free = 100;
  assert(!Sync::start(source, "a"));
  assert(Sync::lastStartFailure() == Sync::StartFailure::Headroom);
  ESP.free = 1024 * 1024;
  assert(!Sync::start(source, ""));
  assert(Sync::lastStartFailure() == Sync::StartFailure::InvalidSource);
  WiFi.connected = false;
  assert(!Sync::start(source, "a"));
  assert(Sync::lastStartFailure() == Sync::StartFailure::Network);
  WiFi.connected = true;
  fakeTransport::blockPrepare = true;
  assert(Sync::start(source, "a"));
  waitFor(fakeTransport::enteredPrepare);
  assert(Sync::showSyncIndicator());
  assert(!Sync::start(source, "a"));  // No second worker/accounting writer.
  // Caller data can disappear/change immediately; worker owns its frozen copy.
  day.readingMs = 999999;
  Sync::Status s;
  assert(Sync::status("b", s) && s.running);
  assert(!Sync::status("other-book", s));
  assert(!Sync::status("b", s, "other-account"));
  Sync::dismiss("b");
  assert(Sync::status("b", s));  // Cannot dismiss live work.
  unblock(fakeTransport::blockPrepare);
  s = finish();
  assert(!Sync::showSyncIndicator());
  assert(s.queue == TimeQueue::State::Complete && s.confirmed == 125);
  assert(s.totals.pending == 0 && s.totals.deviceConfirmed == 125);
  assert(fakeTransport::reports == 3 && fakeTransport::seconds == 125);
  Sync::dismiss("b");
  assert(!Sync::status("b", s));

  day.readingMs = 125000;
  fixture(day.readingMs);
  fakeTransport::blockPrepare = true;
  assert(Sync::start(source, "a"));
  waitFor(fakeTransport::enteredPrepare);
  const auto started = std::chrono::steady_clock::now();
  assert(!Sync::prepareToLeaveReading());  // Pause request must not wait on TLS.
  assert(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
  assert(Sync::active() && Sync::ownsWifi());
  unblock(fakeTransport::blockPrepare);
  s = finish();
  assert(s.queue == TimeQueue::State::Paused && fakeTransport::reports == 0 && s.totals.pending == 125);
  assert(Sync::prepareToLeaveReading());

  fixture(day.readingMs);
  fakeTransport::blockReport = true;
  assert(Sync::start(source, "a"));
  waitFor(fakeTransport::enteredReport);
  assert(!Sync::prepareToLeaveReading());
  unblock(fakeTransport::blockReport);
  s = finish();
  assert(s.queue == TimeQueue::State::Uncertain && fakeTransport::reports == 1);
  assert(s.totals.deviceUnknown == 60 && s.confirmed == 0);  // ACK alone is not credit.
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  // Recovery reads back the ACKed batch, then sends only the remaining 65s.
  assert(s.queue == TimeQueue::State::Complete && s.totals.deviceConfirmed == 125);
  assert(fakeTransport::reports == 3 && fakeTransport::seconds == 125);

  fixture(day.readingMs);
  fakeTransport::blockReport = true;
  fakeTransport::unknown = true;
  assert(Sync::start(source, "a"));
  waitFor(fakeTransport::enteredReport);
  assert(!Sync::prepareToLeaveReading() && Sync::ownsWifi());
  unblock(fakeTransport::blockReport);
  s = finish();
  assert(s.queue == TimeQueue::State::Uncertain && fakeTransport::reports == 1);
  assert(s.totals.pending == 65 && s.totals.deviceUnknown == 60 && s.confirmed == 0);
  // Reopening/restarting cannot replay an unknown write or release its duration.
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Uncertain && fakeTransport::reports == 1);
  assert(s.totals.pending == 65 && s.totals.deviceUnknown == 60);
  fixture(day.readingMs);
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::requests = 0;
  fakeService::confirmed = false;
  fakeService::fail = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Paused && s.totals.servicePending == 0 && s.totals.pending == 125 &&
         fakeTransport::reports == 0);
  fakeService::fail = false;
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Complete && s.totals.servicePending == 125 && s.totals.serviceConfirmed == 0);
  assert(fakeService::requests == 2 && fakeTransport::reports == 0 && s.totals.pending == 0);
  fakeStorage::files.erase("/WeReadSync/service.conf");
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.auditFailed && fakeTransport::reports == 0);  // Config removal cannot enable direct fallback.
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::confirmed = true;
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.totals.serviceConfirmed == 125 && s.totals.servicePending == 0 && s.confirmed == 125);
  assert(fakeTransport::reports == 0 && fakeService::requests == 3);
  // 50-minute task remains accepted but uncredited when another 10 minutes arrive.
  day.readingMs = 3000000;
  source.totalMs = day.readingMs;
  fixture(day.readingMs);
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::confirmed = false;
  fakeService::posts = fakeService::gets = 0;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.totals.servicePending == 3000 && s.totals.pending == 0 && fakeService::posts == 1);
  day.readingMs = 3600000;
  source.totalMs = day.readingMs;
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Complete && s.totals.pending == 0 && s.totals.servicePending == 3600);
  assert(fakeService::posts == 2 && fakeService::lastStart == 3000 && fakeService::lastEnd == 3600);
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(fakeService::posts == 2 && s.totals.serviceConfirmed == 0);  // Repeated click is GET only.
  // Review on the old task holds cloud execution, not admission of new measured time.
  day.readingMs = 4200000;
  source.totalMs = day.readingMs;
  fakeService::review = true;
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(fakeService::posts == 3 && s.queue == TimeQueue::State::Uncertain && s.totals.pending == 0);
  fakeService::review = false;
  fakeService::full = true;
  day.readingMs = 4800000;
  source.totalMs = day.readingMs;
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.serviceQueueFull && s.queue == TimeQueue::State::Paused && s.totals.pending == 600 &&
         s.totals.servicePending == 4200);
  fakeService::full = false;
  fakeService::fail = true;
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.totals.pending == 600);  // Lost receipt retains the reserved 600s tail.
  fakeService::fail = false;
  fakeService::confirmed = true;
  WiFi.connected = true;
  assert(Sync::start(source, "a"));
  s = finish();
  assert(s.totals.serviceConfirmed == 4800 && fakeTransport::reports == 0);
  // A reader can audit and upload only its own measured source without any
  // companion manifest. Moving the same SD card changes the physical match.
  fakeStorage::reset();
  fakeTransport::reset();
  WiFi.connected = true;
  char ownedSourceId[64] = {};
  assert(deviceSource(HalSystem::deviceId.data(), "0123456789abcdef0123456789abcdef", ownedSourceId));
  ReadingDayStats ownedDay{20709, 125000};
  Sync::Source ownedSource{"b", ownedSourceId, &ownedDay, 1, ownedDay.readingMs, true};
  {
    Sync::Accounting accounting;
    Sync::Totals totals;
    assert(accounting.audit(ownedSource, "a", totals));
    assert(totals.pending == 125 && totals.selectedDay == ownedDay.dayOrdinal);
    assert(!fakeStorage::files.count(kTimeManifest));
    const auto originalId = HalSystem::deviceId;
    HalSystem::deviceId[5] ^= 1;
    assert(!accounting.audit(ownedSource, "a", totals));
    HalSystem::deviceId = originalId;
  }
  assert(Sync::start(ownedSource, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Complete && s.totals.deviceConfirmed == 125);
  assert(fakeTransport::reports == 3 && fakeTransport::seconds == 125);
  const auto originalId = HalSystem::deviceId;
  HalSystem::deviceId[5] ^= 1;
  WiFi.connected = true;
  assert(Sync::start(ownedSource, "a"));
  s = finish();
  assert(s.auditFailed && fakeTransport::reports == 3);
  HalSystem::deviceId = originalId;
  // The user returns to reading and immediately requests deep sleep while
  // five dates are in-flight. Sleep must not set the worker's pause flag.
  fakeStorage::reset();
  fakeTransport::reset();
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::confirmed = fakeService::fail = fakeService::review = fakeService::full = false;
  fakeService::posts = fakeService::gets = fakeService::batchRequests = 0;
  fakeService::enteredBatch = false;
  fakeService::blockBatch = true;
  ReadingDayStats fiveDays[] = {{20716, 210000}, {20717, 1200000}, {20718, 315000}, {20719, 58000}, {20720, 6000}};
  Sync::Source fiveSource{"b", ownedSourceId, fiveDays, 5, 1789000, true};
  WiFi.connected = true;
  assert(Sync::start(fiveSource, "a"));
  {
    std::unique_lock<std::mutex> lock(fakeService::mutex);
    assert(fakeService::cv.wait_for(lock, std::chrono::seconds(3), [] { return fakeService::enteredBatch; }));
  }
  const auto sleepStarted = std::chrono::steady_clock::now();
  assert(!Sync::prepareForSleep() && Sync::active() && Sync::ownsWifi());
  assert(std::chrono::steady_clock::now() - sleepStarted < std::chrono::milliseconds(100));
  {
    std::lock_guard<std::mutex> lock(fakeService::mutex);
    fakeService::blockBatch = false;
    fakeService::cv.notify_all();
  }
  s = finish();
  assert(Sync::prepareForSleep());
  assert(s.queue == TimeQueue::State::Complete && s.totals.pending == 0 && s.totals.servicePending == 1789);
  assert(fakeService::batchRequests == 1 && fakeService::posts == 5 && fakeTransport::reports == 0);
  // Re-entering after wake queries receipts in one batch, without resubmission.
  WiFi.connected = true;
  fakeService::confirmed = true;
  assert(Sync::start(fiveSource, "a"));
  s = finish();
  assert(s.totals.serviceConfirmed == 1789 && s.totals.servicePending == 0);
  assert(fakeService::batchRequests == 2 && fakeService::posts == 5 && fakeService::gets == 5);
  // A lost receipt plus newly accrued time recovers the exact old range before
  // reserving the successor; two batches, no history reset or fabricated delta.
  fakeStorage::reset();
  fakeTransport::reset();
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::confirmed = false;
  fakeService::fail = true;
  fakeService::posts = fakeService::gets = fakeService::batchRequests = 0;
  ownedDay.readingMs = 125000;
  ownedSource.totalMs = ownedDay.readingMs;
  WiFi.connected = true;
  assert(Sync::start(ownedSource, "a"));
  s = finish();
  assert(s.totals.pending == 125 && fakeService::lastStart == 0 && fakeService::lastEnd == 125);
  ownedDay.readingMs = 250000;
  ownedSource.totalMs = ownedDay.readingMs;
  fakeService::fail = false;
  WiFi.connected = true;
  assert(Sync::start(ownedSource, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Complete && s.totals.servicePending == 250 && s.totals.pending == 0);
  assert(fakeService::batchRequests == 3 && fakeService::lastStart == 125 && fakeService::lastEnd == 250);
  // The 24-slot local ledger can query old receipts before reserving a new
  // range. Otherwise a full ledger would permanently prevent its own recovery.
  fakeStorage::reset();
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::posts = fakeService::gets = fakeService::batchRequests = 0;
  for (unsigned seconds = 1; seconds <= 24; ++seconds) {
    ownedDay.readingMs = seconds * 1000;
    ownedSource.totalMs = ownedDay.readingMs;
    WiFi.connected = true;
    assert(Sync::start(ownedSource, "a"));
    s = finish();
    assert(s.queue == TimeQueue::State::Complete && s.totals.servicePending == seconds);
  }
  ownedDay.readingMs = 25000;
  ownedSource.totalMs = ownedDay.readingMs;
  fakeService::confirmed = true;
  fakeService::batchRequests = 0;
  WiFi.connected = true;
  assert(Sync::start(ownedSource, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Complete && s.totals.pending == 0);
  assert(s.totals.serviceConfirmed == 5 && s.totals.servicePending == 20);
  assert(fakeService::posts == 25 && fakeService::batchRequests == 2);
  fakeService::confirmed = false;
  // More than 16 dates use two bounded batches; crossing the boundary must not
  // lose the current journal selection or turn new receipts into submissions.
  fakeStorage::reset();
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::posts = fakeService::gets = fakeService::batchRequests = 0;
  ReadingDayStats manyDays[20];
  for (size_t i = 0; i < 20; ++i) manyDays[i] = {uint32_t(20716 + i), 1000};
  Sync::Source manySource{"b", ownedSourceId, manyDays, 20, 20000, true};
  WiFi.connected = true;
  assert(Sync::start(manySource, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Complete && s.totals.pending == 0 && s.totals.servicePending == 20);
  assert(fakeService::batchRequests == 2 && fakeService::posts == 20 && fakeService::gets == 0);
  WiFi.connected = true;
  assert(Sync::start(manySource, "a"));
  s = finish();
  assert(fakeService::batchRequests == 4 && fakeService::posts == 20 && fakeService::gets == 20);
  // When the between-operation budget expires, retain the finished batch and
  // the unsent remainder. A later explicit run resumes without replay.
  fakeStorage::reset();
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::posts = fakeService::gets = fakeService::batchRequests = 0;
  fakeService::batchElapsedMs = 60000;
  WiFi.connected = true;
  assert(Sync::start(manySource, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Paused && s.totals.pending == 4 && s.totals.servicePending == 16);
  assert(fakeService::batchRequests == 1 && fakeService::posts == 16 && Sync::prepareForSleep());
  fakeService::batchElapsedMs = 0;
  WiFi.connected = true;
  assert(Sync::start(manySource, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Complete && s.totals.pending == 0 && s.totals.servicePending == 20);
  assert(fakeService::posts == 20 && fakeService::gets == 16 && fakeService::batchRequests == 3);
  // Older backends stop before making any WRS1 reservation; never silently
  // switch a service-owned history back to the direct cloud sender.
  fakeStorage::reset();
  fakeStorage::files["/WeReadSync/service.conf"] = {'x'};
  fakeService::batchSupported = false;
  fakeService::posts = fakeService::batchRequests = 0;
  WiFi.connected = true;
  assert(Sync::start(manySource, "a"));
  s = finish();
  assert(s.queue == TimeQueue::State::Paused && s.totals.pending == 20);
  assert(fakeService::posts == 0 && fakeService::batchRequests == 0 && fakeTransport::reports == 0);
  for (const auto& file : fakeStorage::files) assert(!file.first.ends_with(".wrs1"));
  fakeService::batchSupported = true;
  std::cout << "Batch handoff: five dates in one request, immediate sleep drain, immutable-tail recovery, "
               "16-item boundary and capability negotiation PASS\n";
  std::cout << "Service handoff: lost receipt retry, no ACK credit, config removal fail closed, readback PASS\n";
  std::cout << "Background lifetime, immutable source, concurrent snapshots, duplicate start, OOM, cooperative pause, "
               "Wi-Fi release and no replay PASS\n";
}
