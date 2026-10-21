#pragma once
#include <cstdint>

namespace BoardConfig {
enum class DisplayController : uint8_t {
  SSD1677 = 0,
  UC8253 = 2,
  ED2208 = 3,
  LgfxEpd = 4,
  IT8951 = 5,
  UC8279 = 6,
  UC8179 = 7,
  UC8279C = 8
};
struct BoardProfile {
  DisplayController displayController = DisplayController::SSD1677;
  uint8_t displayControllerVariant = 0;
};
inline BoardProfile ACTIVE;
}  // namespace BoardConfig
