#pragma once

#include <array>
#include <cstdint>

namespace HalSystem {
using DeviceId = std::array<uint8_t, 6>;
inline DeviceId deviceId{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
inline bool getDeviceId(DeviceId& out) {
  out = deviceId;
  return true;
}
}  // namespace HalSystem
