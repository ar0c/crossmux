#include "WeReadTimeSync.h"
#ifdef ENABLE_CHINESE_VERSION
#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "ReadingStatsStore.h"
#include "WeReadDeviceTimeTransport.h"
#include "WeReadHandoverManifest.h"
#include "WeReadTimeStorage.h"
#include "WeReadServiceJournal.h"
#include "WeReadServiceClient.h"

namespace WeReadTimeSync {
struct Accounting::Scratch {
  WeReadTime::SdByteLog serviceLog;
  WeReadTime::ServiceJournal service{serviceLog};
  WeReadTime::SdByteLog legacyLog, pacedLog;
  WeReadTime::Journal legacy{legacyLog};
  WeReadTime::PacedJournal paced{pacedLog};
  WeReadTime::ExternalTimeStorage external;
  WeReadTime::HandoverManifest manifest;
  WeReadTime::ExternalTime receipt;
  uint8_t digest[32];
  char hex[65];
  char path[176];
};

Accounting::Accounting() = default;
Accounting::~Accounting() = default;
void Accounting::clear() { scratch_.reset(); }
bool Accounting::audit(const Source& source, const char* account, Totals& totals, WeReadTime::ExternalTime* selected,
                       uint64_t* measured) {
  totals.hostPaused = false;
  totals.selectedDay = 0;
  totals.pending = totals.externalConfirmed = totals.externalUnknown = 0;
  totals.deviceConfirmed = totals.deviceUnknown = 0;
  totals.servicePending = totals.serviceConfirmed = 0;
  totals.serviceMode = WeReadTime::ServiceClient::configured();
  if (!source.days || !source.count || source.count > 4096) return false;
  static_assert(sizeof(Scratch) < 6 * 1024, "Handover audit must stay bounded");
  // Fixed audit scratch, reused across every batch; too large for the task stack.
  if (!scratch_) scratch_ = makeUniqueNoThrow<Scratch>();
  auto* work = scratch_.get();
  if (!work) {
    LOG_ERR("WRTime", "OOM: handover audit (%u bytes)", unsigned(sizeof(Scratch)));
    return false;
  }
  if (!work->external.organizeManifest()) return false;
  bool blocked = false;
  for (unsigned pass = 0; pass < 2; ++pass) {
    HalFile manifest;
    if (!Storage.openFileForRead("WRTime", WeReadTime::kTimeManifest, manifest)) return false;
    uint64_t assigned = 0;
    uint32_t previousDay = 0;
    bool sealed = false;
    for (size_t index = 0; index < source.count; ++index) {
      // Long histories must also give the reader and C3 idle watchdog CPU time.
      vTaskDelay(1);
      const auto& day = source.days[index];
      if (day.dayOrdinal <= previousDay || day.readingMs > UINT64_MAX - assigned) return false;
      previousDay = day.dayOrdinal;
      assigned += day.readingMs;
      if (!work->legacyLog.configure(account, source.book, source.source, day.dayOrdinal)) return false;
      uint64_t size = 0;
      const auto legacyState = work->legacyLog.size(size);
      if ((!sealed && legacyState != WeReadTime::ByteLog::ReadState::Ready) ||
          legacyState == WeReadTime::ByteLog::ReadState::Error ||
          !work->legacy.open(account, source.book, source.source, day.dayOrdinal) ||
          !work->legacy.collect(day.readingMs))
        return false;
      uint64_t pending = 0, confirmed = 0, unknown = 0;
      if (!sealed) {
        if (!work->external.reconcile(work->legacy.ledger(), pending, confirmed, unknown) || !work->external.receipt())
          return false;
        work->receipt = *work->external.receipt();
        if (index == 0 && !work->manifest.begin(manifest, work->receipt.identity)) {
          totals.hostPaused = work->manifest.hostPaused(manifest, work->receipt.identity);
          return false;
        }
        if (mbedtls_sha256(work->external.receiptBytes(), WeReadTime::ExternalTime::kSize, work->digest, 0) != 0)
          return false;
        for (unsigned i = 0; i < 32; ++i) std::snprintf(work->hex + i * 2, 3, "%02x", work->digest[i]);
        if (!work->manifest.receipt(manifest, work->receipt.identity, work->hex)) return false;
        switch (work->manifest.next(manifest)) {
          case WeReadTime::HandoverManifest::Next::Invalid:
            return false;
          case WeReadTime::HandoverManifest::Next::More:
            break;
          case WeReadTime::HandoverManifest::Next::End:
            if (!work->manifest.finish(manifest, work->receipt.identity)) return false;
            sealed = true;
            break;
        }
      } else {
        // Dates AFTER the completely verified handover belong to the device.
        // No synthetic duration: seed zero coverage, import measured source ms.
        work->receipt = {};
        work->receipt.identity = work->legacy.ledger().identity();
        if (!work->manifest.deviceOwnedDay(work->receipt.identity)) return false;
        if (work->external.hasExternal(work->receipt.identity) ||
            !work->receipt.balance(work->legacy.ledger(), pending))
          return false;
      }
      const auto& receipt = work->receipt;
      // First pass validates the complete handover before creating any WRP2 journal.
      if (!pass) continue;
      if (!work->pacedLog.configure(account, source.book, source.source, day.dayOrdinal,
                                    WeReadTime::SdByteLog::Format::Paced30) ||
          !work->paced.open(receipt) || !work->paced.collect(day.readingMs))
        return false;
      const auto& ledger = work->paced.ledger();
      totals.serviceMode = WeReadTime::ServiceClient::configured();
      if (!work->serviceLog.configure(account, source.book, source.source, day.dayOrdinal,
                                     WeReadTime::SdByteLog::Format::Service)) return false;
      const uint64_t consumed = ledger.measuredMs() / 1000 - ledger.remaining();
      if (!work->service.open(ledger.identity(), consumed, ledger.measuredMs() / 1000)) return false;
      // Losing configuration never gives an already delegated range back to the
      // direct sender. Changed direct counters invalidate the service journal.
      if (work->service.owned() && (!totals.serviceMode || ledger.state() != WeReadTime::PacedLedger::State::Idle)) return false;
      if (work->service.owned() > ledger.remaining()) return false;
      const uint64_t remaining = ledger.remaining() - work->service.owned();
      totals.pending += remaining;
      totals.servicePending += work->service.pending();
      totals.serviceConfirmed += work->service.confirmed();
      totals.externalConfirmed += confirmed;
      totals.externalUnknown += unknown;
      totals.deviceConfirmed += ledger.verified();
      totals.deviceUnknown += ledger.quarantinedSeconds();
      const bool unresolved = ledger.state() != WeReadTime::PacedLedger::State::Idle;
      const bool choose = (unresolved && !blocked) || (!blocked && !totals.selectedDay && (remaining >= 1 || work->service.pending()));
      if (choose) {
        totals.selectedDay = day.dayOrdinal;
        if (selected) *selected = receipt;
        if (measured) *measured = day.readingMs;
      }
      if (unresolved) {
        totals.deviceUnknown += ledger.batchSeconds();
        blocked = true;
      }
    }
    if (!sealed || assigned != source.totalMs) return false;
  }
  return true;  // Unresolved records are selected for read-only recovery, never resends.
}

namespace {
// Main-task-owned lifetime, independent of screens. One process-wide sender.
WeReadTime::SendCoordinator& coordinator() {
  static WeReadTime::SendCoordinator value(millis());
  return value;
}
portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
Status published;
char publishedBook[64] = {};
char publishedAccount[32] = {};
std::atomic<bool> workerDone{true};
bool wifiOwned = false;

void publish(Status value) {
  taskENTER_CRITICAL(&statusLock);
  value.revision = published.revision + 1;
  published = value;
  taskEXIT_CRITICAL(&statusLock);
}

void persistDiagnostic(const Status& status) {
  char record[768];
  auto diagnostic = status.diagnostic;
  diagnostic.preparationRetries = status.retries;
  const int n = diagnostic.encode(record, sizeof(record), unsigned(status.queue), unsigned(status.phase),
                                  unsigned(status.issue), millis());
  HalFile file;
  if (!n || !Storage.ensureDirectoryExists("/WeReadSync") ||
      !Storage.openFileForWrite("WRDiag", "/WeReadSync/last-sync-diagnostic.json", file)) {
    LOG_ERR("WRTime", "Diagnostic persistence failed; accounting unchanged");
    return;
  }
  if (file.write(reinterpret_cast<const uint8_t*>(record), n) != size_t(n)) {
    LOG_ERR("WRTime", "Diagnostic write failed; accounting unchanged");
  }
  file.flush();
}

struct Job final : WeReadTime::TimeQueueSource {
  char account[32] = {}, book[64] = {}, sourceId[64] = {};
  std::unique_ptr<ReadingDayStats[]> days;
  size_t dayCount = 0;
  uint64_t totalMs = 0;
  Accounting accounting;
  WeReadTime::SdByteLog log;
  WeReadTime::PacedJournal journal{log};
  WeReadClient::DeviceTimeTransport transport;
  WeReadTime::TimeQueue queue{*this, journal, coordinator(), transport,
                              WeReadTime::TimeTransaction::BatchMode::Bounded60};
  WeReadTime::ExternalTime selected;
  uint64_t measured = 0;
  std::atomic<bool> stopping{false};
  Status current;

  Source source() const { return {book, sourceId, days.get(), dayCount, totalMs}; }
  Result selectNext() override {
    // Audit a frozen source snapshot. Reading can append live stats concurrently
    // without racing a vector, changing this run's budget or invalidating pointers.
    if (!accounting.audit(source(), account, current.totals, &selected, &measured)) {
      current.auditFailed = true;
      return Result::Error;
    }
    if (!current.totals.selectedDay) return Result::Complete;
    if (!log.configure(account, book, sourceId, current.totals.selectedDay, WeReadTime::SdByteLog::Format::Paced30) ||
        !journal.open(selected) || !journal.collect(measured))
      return Result::Error;
    return Result::Ready;
  }
  void sample() {
    current.queue = queue.state();
    current.phase = queue.phase();
    current.issue = queue.issue();
    current.confirmed = queue.confirmed();
    current.retries = queue.preparationRetries();
    const auto waiting = queue.waitingSeconds();
    current.waitSeconds =
        current.phase == WeReadTime::TimeTransaction::State::RetryWait ? waiting : ((waiting + 4) / 5) * 5;
    current.diagnostic = transport.diagnostic();
  }
  void run() {
    if (WeReadTime::ServiceClient::configured()) { runService(); return; }
    current.available = current.running = true;
    queue.begin();
    sample();
    publish(current);
    for (;;) {
      const auto before = current;
      const auto journalBefore = journal.ledger().state();
      const auto batchBefore = journal.ledger().batchSeconds();
      // Cooperative cancellation only BETWEEN complete TLS/journal operations.
      // A received ACK is persisted by step() before cancellation is observed.
      if (stopping.load())
        queue.cancel();
      else
        queue.step();
      sample();
      if (before.queue == WeReadTime::TimeQueue::State::Running &&
          journalBefore == WeReadTime::PacedLedger::State::Idle &&
          journal.ledger().state() != WeReadTime::PacedLedger::State::Idle) {
        const auto reserved = journal.ledger().batchSeconds();
        current.totals.pending = current.totals.pending >= reserved ? current.totals.pending - reserved : 0;
        current.totals.deviceUnknown += reserved;
      }
      if (before.queue == WeReadTime::TimeQueue::State::Running &&
          current.queue == WeReadTime::TimeQueue::State::Selecting) {
        current.totals.deviceConfirmed += batchBefore;
        current.totals.deviceUnknown =
            current.totals.deviceUnknown >= batchBefore ? current.totals.deviceUnknown - batchBefore : 0;
      }
      const bool changed = before.queue != current.queue || before.phase != current.phase ||
                           before.waitSeconds != current.waitSeconds || before.retries != current.retries;
      using Q = WeReadTime::TimeQueue::State;
      switch (current.queue) {
        case Q::Complete:
        case Q::Paused:
        case Q::Uncertain:
        case Q::StorageError:
          current.auditFailed = !accounting.audit(source(), account, current.totals);
          current.running = false;
          persistDiagnostic(current);
          publish(current);
          LOG_INF("WRTime", "Background finished queue=%u confirmed=%llu stack=%u", unsigned(current.queue),
                  static_cast<unsigned long long>(current.confirmed), unsigned(uxTaskGetStackHighWaterMark(nullptr)));
          return;
        case Q::Idle:
        case Q::Selecting:
        case Q::Running:
          break;
      }
      if (changed) publish(current);
      if (before.phase != current.phase && current.phase == WeReadTime::TimeTransaction::State::RetryWait) {
        persistDiagnostic(current);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  void runService() {
    using Q = WeReadTime::TimeQueue::State;
    current.available = current.running = true;
    current.queue = Q::Running;
    current.phase = WeReadTime::TimeTransaction::State::Sending;
    publish(current);
    // Reuse the existing fallibly allocated job/source snapshot. These bounded
    // service-only stack objects are below 2 KiB; no full-history allocation.
    WeReadTime::ServiceClient client;
    WeReadTime::SdByteLog serviceLog;
    WeReadTime::ServiceJournal service(serviceLog);
    const auto finish = [&](Q result) {
      current.auditFailed = !accounting.audit(source(), account, current.totals);
      current.queue = result; current.running = false;
      current.confirmed = current.totals.serviceConfirmed;
      publish(current);
    };
    if (!accounting.audit(source(), account, current.totals)) { finish(Q::StorageError); return; }
    if (!client.connect(account)) { finish(Q::Paused); return; }
    for (size_t index = 0; index < dayCount; ++index) {
      if (stopping.load()) { finish(Q::Paused); return; }
      const auto day = days[index].dayOrdinal;
      // The complete audit above has validated handover and created WRP2 state.
      if (!log.configure(account, book, sourceId, day, WeReadTime::SdByteLog::Format::Paced30)) { finish(Q::StorageError); return; }
      uint64_t length=0;
      uint8_t frame[WeReadTime::PacedLedger::kSize];
      WeReadTime::PacedLedger ledger;
      if (log.size(length)!=WeReadTime::ByteLog::ReadState::Ready || length<sizeof(frame) ||
          !log.read(length-sizeof(frame),frame,sizeof(frame)) || !ledger.decode(frame) ||
          ledger.state()!=WeReadTime::PacedLedger::State::Idle ||
          !serviceLog.configure(account,book,sourceId,day,WeReadTime::SdByteLog::Format::Service) ||
          !service.open(ledger.identity(),ledger.measuredMs()/1000-ledger.remaining(),ledger.measuredMs()/1000) ||
          !service.matchesDevice(client.device())) { finish(Q::StorageError); return; }
      if (service.pending()) {
        const auto result=client.exchange(ledger.identity(),service);
        if(result==WeReadTime::ServiceClient::Result::Failed){finish(Q::Paused);return;}
        if(result==WeReadTime::ServiceClient::Result::Review){finish(Q::Uncertain);return;}
      }
      if (stopping.load()) { finish(Q::Paused); return; }
      if (!service.pending() && ledger.remaining()>service.owned()) {
        if(!service.reserve(client.device())){finish(Q::StorageError);return;}
        const auto result=client.exchange(ledger.identity(),service);
        if(result==WeReadTime::ServiceClient::Result::Failed){finish(Q::Paused);return;}
        if(result==WeReadTime::ServiceClient::Result::Review){finish(Q::Uncertain);return;}
      }
      vTaskDelay(1);
    }
    // Acceptance ends the device network task; cloud work continues on server.
    finish(Q::Complete);
  }
};
static_assert(sizeof(Job) < 21 * 1024, "Background job exceeds fixed workspace budget");
struct JobDeleter {
  bool external = false;
  void operator()(Job* ptr) const {
    if (!ptr) return;
    if (external) {
      ptr->~Job();
      memory::FreeDeleter{}(reinterpret_cast<uint8_t*>(ptr));
    } else {
      delete ptr;
    }
  }
};
using JobPtr = std::unique_ptr<Job, JobDeleter>;
JobPtr job;

void worker(void* argument) {
  static_cast<Job*>(argument)->run();
  // Last access to Job precedes this release. Main may now destroy all scratch.
  workerDone.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
}  // namespace

bool active() { return job && !workerDone.load(std::memory_order_acquire); }
bool showSyncIndicator() {
  taskENTER_CRITICAL(&statusLock);
  const bool running = published.available && published.running;
  taskEXIT_CRITICAL(&statusLock);
  return running;
}
bool ownsWifi() { return wifiOwned; }
void releaseWifi() {
  if (!wifiOwned || active()) return;
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  wifiOwned = false;
}
void poll() {
  if (job && workerDone.load(std::memory_order_acquire)) {
    job.reset();
    releaseWifi();
  }
}
void pause() {
  if (active()) job->stopping.store(true);
}
bool prepareToLeaveReading() {
  pause();
  poll();
  return !active();
}
bool canContinueIn(const char* activityName, bool reader, bool home) {
  if (reader || home) return true;
  if (!activityName) return false;
  // Explicit local surfaces only. New network/settings/raw-storage screens
  // must drain the worker before their onEnter() can change shared resources.
  static constexpr const char* localScreens[] = {"WeReadProgressSync",
                                                 "EpubReaderMenu",
                                                 "EpubReaderBookmarks",
                                                 "EpubReaderFootnotes",
                                                 "EpubReaderChapterSelection",
                                                 "EpubReaderPercentSelection",
                                                 "TxtReaderChapterSelection",
                                                 "XtcReaderChapterSelection",
                                                 "DictionaryWordSelect",
                                                 "DictionaryDefinition",
                                                 "QrDisplay"};
  for (const auto* name : localScreens) {
    if (!strcmp(activityName, name)) return true;
  }
  return false;
}
bool status(const char* book, Status& result, const char* account) {
  taskENTER_CRITICAL(&statusLock);
  const bool matches =
      book && published.available && !strcmp(book, publishedBook) && (!account || !strcmp(account, publishedAccount));
  if (matches) result = published;
  taskEXIT_CRITICAL(&statusLock);
  return matches;
}

void dismiss(const char* book) {
  if (active()) return;
  taskENTER_CRITICAL(&statusLock);
  if (book && !strcmp(book, publishedBook)) published.available = false;
  taskEXIT_CRITICAL(&statusLock);
}

namespace {
StartFailure startFailure = StartFailure::None;
}
StartFailure lastStartFailure() { return startFailure; }

bool start(const Source& source, const char* account) {
  poll();
  startFailure = StartFailure::None;
  // A busy rejection must not compete with the live worker for SD access.
  if (active()) {
    startFailure = StartFailure::Busy;
    return false;
  }
  unsigned freeHeap = ESP.getFreeHeap();
  unsigned largestBlock = ESP.getMaxAllocHeap();
  size_t budget = 0, contiguous = 0;
  const auto fail = [&](StartFailure reason) {
    startFailure = reason;
    // Bounded, credential-free evidence for file-manager-only devices. Keep
    // startup evidence separate from the last transaction/cloud receipt.
    char record[384];
    const int n = std::snprintf(record, sizeof(record),
        "{\"schema\":1,\"reason\":%u,\"free_heap\":%u,\"largest_block\":%u,"
        "\"budget\":%u,\"contiguous\":%u,\"free_reserve\":98304,\"block_reserve\":32768,"
        "\"wifi_connected\":%u,\"days\":%u,\"uptime_ms\":%lu}\n",
        unsigned(reason), freeHeap, largestBlock, unsigned(budget), unsigned(contiguous),
        unsigned(WiFi.status() == WL_CONNECTED), unsigned(source.count), static_cast<unsigned long>(millis()));
    HalFile file;
    if (n > 0 && size_t(n) < sizeof(record) && Storage.ensureDirectoryExists("/WeReadSync") &&
        Storage.openFileForWrite("WRTime", "/WeReadSync/last-start-diagnostic.json", file)) {
      if (file.write(reinterpret_cast<const uint8_t*>(record), size_t(n)) != size_t(n))
        LOG_ERR("WRTime", "Startup diagnostic write failed");
      file.flush();
    }
    LOG_ERR("WRTime", "Startup rejected reason=%u free=%u largest=%u budget=%u contiguous=%u",
            unsigned(reason), freeHeap, largestBlock, unsigned(budget), unsigned(contiguous));
    return false;
  };
  if (!account || !*account || strlen(account) >= 32 || !source.book || !*source.book || strlen(source.book) >= 64 ||
      !source.source || !*source.source || strlen(source.source) >= 64 || !source.days || !source.count ||
      source.count > 4096)
    return fail(StartFailure::InvalidSource);
  if (WiFi.status() != WL_CONNECTED) return fail(StartFailure::Network);
  // 8 KiB task stack + <=21 KiB job + <5 KiB audit + 16 bytes per source day
  // (<=64 KiB). Fallible, allocated only on explicit start and freed on finish.
  // A live vector reference races reading; a maximum-sized static array would
  // permanently consume C3 RAM. Keep headroom for TLS AND the resumed reader.
  constexpr size_t stackBytes = 8192;
  // CPU-only workspace, never an ISR or DMA buffer. Allocate <=21 KiB once
  // in PSRAM where available; a task-stack/static workspace is unsuitable for
  // its size/lifetime. Preserve the C3 fallback and both internal reserves.
  memory::ByteBuffer externalJob;
  if (memory::psramHasHeadroom(sizeof(Job), sizeof(Job), 32 * 1024))
    externalJob = memory::makePsramByteBufferNoThrow(sizeof(Job));
  const size_t internalJobBytes = externalJob ? 0 : sizeof(Job);
  freeHeap = ESP.getFreeHeap();
  largestBlock = ESP.getMaxAllocHeap();
  static_assert(sizeof(ReadingDayStats) <= 16, "Source snapshot day budget changed");
#if !defined(SIMULATOR)
  budget = internalJobBytes + source.count * sizeof(ReadingDayStats) + stackBytes + 6 * 1024 + 1024;
  contiguous = std::max({internalJobBytes, source.count * sizeof(ReadingDayStats), stackBytes});
  if (!memory::hasAllocationHeadroom(freeHeap, largestBlock, budget,
                                     contiguous, 96 * 1024,
                                     32 * 1024)) {
    LOG_ERR("WRTime", "Insufficient heap for background sync and reader (%u bytes)", unsigned(budget));
    return fail(StartFailure::Headroom);
  }
#endif
  JobPtr next;
  if (externalJob) {
    // Placement construction performs no allocation; storage was checked above.
    auto* instance = new (externalJob.get()) Job();
    externalJob.release();
    next = JobPtr(instance, JobDeleter{true});
  } else {
    auto internal = makeUniqueNoThrow<Job>();
    next = JobPtr(internal.release(), JobDeleter{false});
  }
  if (!next) {
    LOG_ERR("WRTime", "OOM: background job");
    return fail(StartFailure::JobMemory);
  }
  next->days = makeUniqueNoThrow<ReadingDayStats[]>(source.count);
  if (!next->days) {
    LOG_ERR("WRTime", "OOM: background source snapshot");
    return fail(StartFailure::SourceMemory);
  }
  std::copy_n(source.days, source.count, next->days.get());
  next->dayCount = source.count;
  next->totalMs = source.totalMs;
  strcpy(next->account, account);
  strcpy(next->book, source.book);
  strcpy(next->sourceId, source.source);
  job = std::move(next);
  Status initial;
  initial.available = initial.running = true;
  initial.queue = WeReadTime::TimeQueue::State::Selecting;
  initial.phase = WeReadTime::TimeTransaction::State::Preparing;
  taskENTER_CRITICAL(&statusLock);
  strcpy(publishedBook, source.book);
  strcpy(publishedAccount, account);
  taskEXIT_CRITICAL(&statusLock);
  publish(initial);
  workerDone.store(false, std::memory_order_release);
  TaskHandle_t task = nullptr;
  if (xTaskCreate(worker, "WeReadTime", stackBytes, job.get(), 1, &task) != pdPASS) {
    workerDone.store(true, std::memory_order_release);
    job.reset();
    initial.available = false;  // No run exists; do not retain zero totals as an audit result.
    initial.running = false;
    initial.queue = WeReadTime::TimeQueue::State::Paused;
    initial.phase = WeReadTime::TimeTransaction::State::NotSent;
    initial.diagnostic.stage = WeReadTime::Diagnostic::Stage::StartupMemory;
    publish(initial);
    LOG_ERR("WRTime", "OOM: background task stack/TCB");
    return fail(StartFailure::TaskMemory);
  }
  wifiOwned = true;
  return true;
}
}  // namespace WeReadTimeSync
#endif
