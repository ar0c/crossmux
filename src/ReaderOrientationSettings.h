#pragma once

#include <CrossPointSettings.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <span>

// Persist the manual fallback separately from the automatic policy. Sensor
// updates never change either setting or write settings.json.
namespace ReaderOrientationSettings {
constexpr uint8_t AUTO = CrossPointSettings::ORIENTATION_COUNT;
inline constexpr StrId LABELS[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                   StrId::STR_LANDSCAPE_CCW, StrId::STR_ORIENTATION_AUTO};

inline bool available() {
#if defined(SIMULATOR)
  // The pinned native HAL has no posture API or physical IMU.
  return false;
#else
  return halTiltSensor.supportsAutoRotation();
#endif
}

inline std::span<const StrId> labels() { return {LABELS, available() ? AUTO + 1u : AUTO}; }

inline uint8_t selected() { return SETTINGS.readerAutoRotate && available() ? AUTO : SETTINGS.orientation; }

inline bool select(uint8_t option) {
  if (option >= labels().size()) return false;
  const uint8_t automatic = option == AUTO;
  const bool changed = SETTINGS.readerAutoRotate != automatic || (!automatic && SETTINGS.orientation != option);
  SETTINGS.readerAutoRotate = automatic;
  if (!automatic) SETTINGS.orientation = option;
  return changed;
}
}  // namespace ReaderOrientationSettings
