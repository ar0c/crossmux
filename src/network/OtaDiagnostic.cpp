#include "OtaDiagnostic.h"

#ifdef SIMULATOR
namespace OtaDiagnostic {
void begin(size_t, uint32_t) {}
void checkpoint(const char*, size_t, size_t, uint32_t, int) {}
void recordBoot() {}
void recordSetupDone() {}
}  // namespace OtaDiagnostic
#else

#include <Arduino.h>
#include <HalOtaSlot.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace OtaDiagnostic {
namespace {
constexpr char MODULE[] = "OTADIAG";
bool bootRecorded = false;

// A short, flushed append is preferable to keeping an open SD file across a
// long TLS transfer and a possible reset. HalFile's small implementation is
// the only heap allocation; the record and readback buffers stay on the stack.
void append(const char* stage, size_t writtenBytes, size_t expectedBytes, uint32_t targetSlot, int errorCode) {
  if (!Storage.ready()) return;
  char record[224];
  const int length = std::snprintf(
      record, sizeof(record),
      "{\"schema\":1,\"stage\":\"%s\",\"running\":\"" CROSSPOINT_VERSION
      "\",\"ms\":%lu,\"written\":%zu,\"expected\":%zu,\"target_slot\":%lu,\"code\":%d,\"free\":%u,\"largest\":%u}\n",
      stage, millis(), writtenBytes, expectedBytes, static_cast<unsigned long>(targetSlot), errorCode,
      static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (length < 0 || static_cast<size_t>(length) >= sizeof(record)) {
    LOG_ERR(MODULE, "OTA diagnostic record overflow");
    return;
  }
  auto file = Storage.open(PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file || file.write(record, static_cast<size_t>(length)) != static_cast<size_t>(length)) {
    LOG_ERR(MODULE, "Failed to append %s", PATH);
    return;
  }
  file.flush();
}
}  // namespace

void begin(size_t expectedBytes, uint32_t targetSlot) {
  if (!Storage.ready()) return;
  // Keep one bounded attempt. A new attempt deliberately replaces the old
  // journal, so users should export it before retrying a failed update.
  {
    HalFile file;
    if (!Storage.openFileForWrite(MODULE, PATH, file)) {
      LOG_ERR(MODULE, "Failed to start %s", PATH);
      return;
    }
    file.flush();
  }
  checkpoint("begin", 0, expectedBytes, targetSlot);
}

void checkpoint(const char* stage, size_t writtenBytes, size_t expectedBytes, uint32_t targetSlot, int errorCode) {
  append(stage, writtenBytes, expectedBytes, targetSlot, errorCode);
}

void recordBoot() {
  if (!Storage.ready()) return;
  auto file = Storage.open(PATH);
  if (!file) return;
  const size_t size = file.size();
  if (size == 0) return;
  char tail[256];
  const size_t start = size > sizeof(tail) - 1 ? size - (sizeof(tail) - 1) : 0;
  if (!file.seekSet(start)) return;
  const int read = file.read(tail, size - start);
  if (read <= 0) return;
  tail[read] = '\0';
  int end = read;
  while (end > 0 && (tail[end - 1] == '\n' || tail[end - 1] == '\r')) --end;
  int lineStart = end;
  while (lineStart > 0 && tail[lineStart - 1] != '\n') --lineStart;
  tail[end] = '\0';
  if (std::strstr(tail + lineStart, "\"stage\":\"reboot_pending\"") == nullptr) return;
  // Release the read handle before opening the append handle.
  file = HalFile();
  checkpoint("boot_started", 0, 0, 0, static_cast<int>(HalOtaSlot::runningImageState()));
  bootRecorded = true;
}

void recordSetupDone() {
  if (!bootRecorded) return;
  checkpoint("setup_done", 0, 0, 0, static_cast<int>(HalOtaSlot::runningImageState()));
  bootRecorded = false;
}
}  // namespace OtaDiagnostic
#endif
