#pragma once

#include <HalStorage.h>

#include <cstdio>

#include "WeReadTimeJournal.h"

namespace WeReadTime {

// HAL adapter. The filename excludes the remote book ID so re-binding the same
// local source/day cannot create a second journal and submit the time twice.
class SdByteLog final : public ByteLog {
 public:
  bool configure(const char* account, const char* book, const char* source, uint32_t day) {
    configured_ = false;
    Ledger validation;
    if (!validation.bind(account, book, source, day)) return false;
    const int dirSize =
        std::snprintf(directory_, sizeof(directory_), "/.crosspoint/weread/time/%s/%s", account, source);
    if (dirSize <= 0 || static_cast<size_t>(dirSize) >= sizeof(directory_)) return false;
    const int pathSize =
        std::snprintf(path_, sizeof(path_), "%s/%lu.wrtl", directory_, static_cast<unsigned long>(day));
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

 private:
  char directory_[144] = {};
  char path_[168] = {};
  bool configured_ = false;
};

}  // namespace WeReadTime
