#pragma once

#include <BoardConfig.h>

// Compile-time board capabilities shared by hardware and desktop UI builds.
// The pinned simulator HAL predates the Waveshare wheel capability; keeping
// this trait in the project HAL avoids adding methods to dependency classes.
namespace HalDeviceCapabilities {
// Explicit opt-in, independent of having an IMU (tilt page turn uses other boards too).
// Future ports enable this only after their BSP axes and sleep lifecycle are validated.
constexpr bool supportsReaderAutoRotation() {
#if defined(FREEINK_DEVICE_WAVESHARE_EPAPER_397) && FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
  return BoardConfig::ACTIVE.sensors.imuType == BoardConfig::ImuType::Qmi8658;
#else
  return false;
#endif
}

constexpr bool hasWheelAndBootButtons() {
#if defined(SIMULATOR) && defined(CROSSPOINT_SIM_UI_WAVESHARE_397)
  return true;  // UI geometry only; the pinned SDK does not emulate this board.
#elif defined(FREEINK_DEVICE_WAVESHARE_EPAPER_397) && FREEINK_DEVICE_WAVESHARE_EPAPER_397
  return true;
#else
  return false;
#endif
}
}  // namespace HalDeviceCapabilities
