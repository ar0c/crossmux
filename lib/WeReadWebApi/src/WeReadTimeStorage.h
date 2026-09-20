#pragma once

#include <HalStorage.h>

#include <cstdio>

#include "WeReadTimeJournal.h"
#include "WeReadExternalTime.h"
#include "WeReadPacedTime.h"
#include "WeReadTimeDataLayout.h"

namespace WeReadTime {

class TimeDataStore {
 public:
  TimeFileKind kind(const char* path) {
    if (!Storage.ready()) return TimeFileKind::Invalid;
    if (!Storage.exists(path)) return TimeFileKind::Missing;
    auto file = Storage.open(path);
    return file && !file.isDirectory() ? TimeFileKind::File : TimeFileKind::Invalid;
  }
  bool ensureDirectory() { return Storage.ensureDirectoryExists(kTimeDataDirectory); }
  bool moveNoReplace(const char* oldPath, const char* newPath) {
    return !Storage.exists(newPath) && Storage.rename(oldPath, newPath);
  }
};

// Fixed activity-owned scratch, no history-sized allocation. Missing external
// accounting must not turn the companion's reserved prefix back into balance.
class ExternalTimeStorage {
 public:
  bool organizeManifest() {
    TimeDataStore store;
    return relocateTimeFile(store, kLegacyTimeManifest, kTimeManifest);
  }
  bool hasExternal(const Identity& id) {
    // Invalid/truncated paths must block the device-owned day path too.
    return !externalPaths(id) || Storage.exists(path_) || Storage.exists(organizedPath_);
  }
  bool reconcile(const Ledger& ledger, uint64_t& pending, uint64_t& confirmed, uint64_t& unknown) {
    hasReceipt_ = false;
    pending = confirmed = unknown = 0;
    if (!Storage.ready()) return false;
    const auto& id = ledger.identity();
    TimeDataStore store;
    if (!externalPaths(id) || !relocateTimeFile(store, path_, organizedPath_)) return false;
    if (!Storage.exists(organizedPath_)) {
      // Archiving diagnostic JSON must not bypass missing-receipt protection.
      for (const char* folder : {"/", kTimeDataDirectory, kTimeHistoryDirectory}) {
        if (!Storage.exists(folder)) continue;
        auto directory = Storage.open(folder);
        if (!directory || !directory.isDirectory()) return false;
        while (true) {
          auto file = directory.openNextFile();
          if (!file) break;
          if (!file.getName(path_,sizeof(path_))) return false;
          if (!std::strncmp(path_,"weread-time-",12) || !std::strncmp(path_,"weread-backlog-",15)) return false;
        }
      }
      pending = ledger.pendingSeconds();
      return true;
    }
    HalFile file;
    if (!Storage.openFileForRead("WRTime",organizedPath_,file) || file.fileSize64() != sizeof(bytes_) ||
        file.read(bytes_,sizeof(bytes_)) != sizeof(bytes_) || !receipt_.decode(bytes_,sizeof(bytes_)) ||
        !receipt_.balance(ledger,pending)) return false;
    confirmed = receipt_.confirmedSeconds;
    unknown = receipt_.unknownSeconds;
    hasReceipt_ = true;
    return true;
  }
  const ExternalTime* receipt() const { return hasReceipt_ ? &receipt_ : nullptr; }
  const uint8_t* receiptBytes() const { return hasReceipt_ ? bytes_ : nullptr; }
 private:
  bool externalPaths(const Identity& id) {
    const int n = std::snprintf(path_, sizeof(path_), "/weread-external-%s-%s-%lu.bin", id.account, id.source,
                                static_cast<unsigned long>(id.day));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path_)) return false;
    const int target = std::snprintf(organizedPath_, sizeof(organizedPath_), "%s%s", kTimeDataDirectory, path_);
    return target > 0 && static_cast<size_t>(target) < sizeof(organizedPath_);
  }
  char path_[176] = {};
  // Activity-owned fixed scratch, reused across all source days; no per-day allocation.
  char organizedPath_[192] = {};
  uint8_t bytes_[ExternalTime::kSize] = {};
  ExternalTime receipt_;
  bool hasReceipt_ = false;
};

// HAL adapter. The filename excludes the remote book ID so re-binding the same
// local source/day cannot create a second journal and submit the time twice.
class SdByteLog final : public ByteLog {
 public:
  enum class Format { Legacy60, Paced30, Service };
  bool configure(const char* account, const char* book, const char* source, uint32_t day,
                 Format format = Format::Legacy60) {
    configured_ = false;
    Ledger validation;
    if (!validation.bind(account, book, source, day)) return false;
    const int dirSize =
        std::snprintf(directory_, sizeof(directory_), "/.crosspoint/weread/time/%s/%s", account, source);
    if (dirSize <= 0 || static_cast<size_t>(dirSize) >= sizeof(directory_)) return false;
    const int pathSize =
        std::snprintf(path_, sizeof(path_), "%s/%lu.%s", directory_, static_cast<unsigned long>(day),
                      format == Format::Legacy60 ? "wrtl" : (format == Format::Paced30 ? "wrp2" : "wrs1"));
    configured_ = pathSize > 0 && static_cast<size_t>(pathSize) < sizeof(path_);
    return configured_;
  }

  ReadState size(uint64_t& length) override {
    length = 0;
    if (!configured_ || !Storage.ready()) return ReadState::Error;
    if (!Storage.exists(path_)) return ReadState::Missing;
    HalFile file;
    if (!Storage.openFileForRead("WRTime", path_, file)) return ReadState::Error;
    length = file.fileSize64();
    return ReadState::Ready;
  }

  bool read(uint64_t offset, uint8_t* out, size_t count) override {
    if (!configured_) return false;
    HalFile file;
    return Storage.openFileForRead("WRTime", path_, file) && file.seek64(offset) &&
           file.read(out, count) == static_cast<int>(count);
  }

  bool appendAndSync(const uint8_t* data, size_t count) override {
    if (!configured_ || !Storage.ensureDirectoryExists(directory_)) return false;
    auto file = Storage.open(path_, O_WRONLY | O_CREAT | O_APPEND);
    if (!file || file.write(data, count) != count) return false;
    file.flush();
    // Journal reopens and checks the exact frame after this handle is destroyed.
    return true;
  }

  bool canReserve(size_t bytes) override {
    constexpr uint64_t kFreeReserve = 1024 * 1024;
    constexpr uint64_t kJournalBudget = 4 * 1024 * 1024;
    uint64_t total = 0, free = 0, length = 0;
    if (!configured_ || !Storage.getSpace(total, free) || free > total ||
        bytes > kJournalBudget || free < kFreeReserve + bytes || size(length) == ReadState::Error)
      return false;
    return length <= kJournalBudget - bytes;
  }

 private:
  char directory_[144] = {};
  char path_[168] = {};
  bool configured_ = false;
};

}  // namespace WeReadTime
