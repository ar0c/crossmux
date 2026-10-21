#pragma once

#include <cstdint>

namespace HalDisplayDiagnostics {
// Fixed-size copy of boot state; no display-bus access or heap allocation.
struct Snapshot {
  uint8_t displayController = 0;
  const char* controllerName = "Unknown";
  uint8_t displayControllerVariant = 0;
  bool valid = false;
  uint8_t ver[5]{};
  uint8_t flg = 0;
  uint8_t verdict = 0;
  const char* verdictName = "NotProbed";
  bool promoted = false;
  bool mtpValid = false;
  uint8_t mtp[48]{};
};

// Reads BoardConfig::ACTIVE and the SDK's cached probe only. Unsupported
// targets/simulator retain valid=false; the active driver is not proof of IC.
Snapshot readCached();
}  // namespace HalDisplayDiagnostics
