#pragma once

#include <HalGPIO.h>

namespace inputCapabilities {
inline bool hasWheelAndBootButtons(const HalGPIO& gpio) {
#if CROSSPOINT_EMULATED
  // The pinned simulator HAL has no wheel query; only the explicit Waveshare
  // UI profile represents this layout, without emulating its physical inputs.
  (void)gpio;
#if CROSSPOINT_SIM_UI_WAVESHARE_397
  return true;
#else
  return false;
#endif
#else
  return gpio.hasWheelAndBootButtons();
#endif
}
}  // namespace inputCapabilities
