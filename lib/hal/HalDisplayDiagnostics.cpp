#include "HalDisplayDiagnostics.h"

#include <BoardConfig.h>
#ifndef SIMULATOR
#include <XteinkDetect.h>
#endif
#include <cstring>

namespace HalDisplayDiagnostics {
Snapshot readCached() {
  Snapshot result;
  const auto controller = BoardConfig::ACTIVE.displayController;
  result.displayController = static_cast<uint8_t>(controller);
  result.displayControllerVariant = BoardConfig::ACTIVE.displayControllerVariant;
  switch (controller) {
    case BoardConfig::DisplayController::SSD1677:
      result.controllerName = "SSD1677";
      break;
    case BoardConfig::DisplayController::UC8253:
      result.controllerName = "UC8253";
      break;
    case BoardConfig::DisplayController::UC8279:
      result.controllerName = "UC8279";
      break;
    case BoardConfig::DisplayController::UC8179:
      result.controllerName = "UC8179";
      break;
    case BoardConfig::DisplayController::UC8279C:
      result.controllerName = "UC8279C";
      break;
#ifndef SIMULATOR
    case BoardConfig::DisplayController::ED2208:
      result.controllerName = "ED2208";
      break;
    case BoardConfig::DisplayController::LgfxEpd:
      result.controllerName = "LgfxEpd";
      break;
    case BoardConfig::DisplayController::IT8951:
      result.controllerName = "IT8951";
      break;
#endif
    default:
      break;
  }
#ifndef SIMULATOR
  const auto& probe = freeink::getXteinkDisplayProbeDiag();
  result.valid = probe.valid;
  static_assert(sizeof(result.ver) == sizeof(probe.ver));
  static_assert(sizeof(result.mtp) == sizeof(probe.mtp));
  std::memcpy(result.ver, probe.ver, sizeof(result.ver));
  result.flg = probe.flg;
  result.verdict = probe.verdict;
  result.promoted = probe.promoted;
  result.mtpValid = probe.mtpValid;
  std::memcpy(result.mtp, probe.mtp, sizeof(result.mtp));
  if (probe.valid) {
    switch (static_cast<freeink::DisplayControllerVerdict>(probe.verdict)) {
      case freeink::DisplayControllerVerdict::PrimaryAssumed:
        result.verdictName = "PrimaryAssumed";
        break;
      case freeink::DisplayControllerVerdict::Uc81xxConfirmed:
        result.verdictName = "Uc81xxConfirmed";
        break;
      case freeink::DisplayControllerVerdict::Inconclusive:
        result.verdictName = "Inconclusive";
        break;
      default:
        result.verdictName = "Unknown";
        break;
    }
  }
#endif
  return result;
}
}  // namespace HalDisplayDiagnostics
