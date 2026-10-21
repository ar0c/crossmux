#pragma once
#include <algorithm>
#include <cstdint>

namespace standby_schedule {
inline constexpr uint32_t ENVIRONMENT_INTERVAL_MS = 60000;

inline bool sampleDue(uint32_t now, uint32_t last, bool sampled, bool faceChanged) {
  const uint32_t elapsed = now - last;
  // Align to every clock tick, even if entry was just before a minute boundary.
  // A minimum elapsed guard here could leave two redraws one second apart.
  return !sampled || elapsed >= ENVIRONMENT_INTERVAL_MS || faceChanged;
}

inline uint32_t sleepSeconds(uint32_t faceSeconds, uint32_t now, uint32_t last, bool sampled) {
  const uint32_t elapsed = now - last;
  const uint32_t remaining = !sampled || elapsed >= ENVIRONMENT_INTERVAL_MS ? 0 : ENVIRONMENT_INTERVAL_MS - elapsed;
  return std::clamp<uint32_t>(std::min(faceSeconds, (remaining + 999) / 1000), 1, 60);
}
}  // namespace standby_schedule
