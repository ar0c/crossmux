#pragma once
#include <cstdint>

namespace Waveshare397Standby {
enum class Wake : uint8_t { Timer, Input, Failed };
bool available();
// Retains RAM and all application/input state. Never powers off the PMIC,
// consumes a key event or waits for release. Caller holds the render lock.
Wake sleepFor(uint32_t seconds);
}  // namespace Waveshare397Standby
