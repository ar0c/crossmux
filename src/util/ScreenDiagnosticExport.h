#pragma once
#include <HalDisplayDiagnostics.h>

#include <cstddef>
#include <cstdint>

namespace ScreenDiagnosticExport {
inline constexpr const char* kPath = "/Diagnostics/Screen/latest.html";
enum class Result { Saved, InvalidFrame, NoSpace, StorageError };
struct Info {
  const char* firmware = "";
  const char* model = "";
  uint16_t width = 0, height = 0;
  uint8_t orientation = 0;
  bool inverted = false, asyncRefresh = false;
  uint32_t refreshPages = 0, freeHeap = 0, largestHeap = 0, freePsram = 0;
  uint64_t uptimeSeconds = 0;
  HalDisplayDiagnostics::Snapshot hardware{};
  void (*cooperate)() = nullptr;
};
// Caller owns the render lock and has waited for any asynchronous refresh.
// Fixed latest report; no network access, credentials, logs or book metadata.
Result save(const Info& info, const uint8_t* framebuffer, size_t bytes);
}  // namespace ScreenDiagnosticExport
