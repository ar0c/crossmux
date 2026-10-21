#pragma once

#if CROSSMUX_USB_DEBUG
#include <cstdint>
namespace debugbridge {
void poll();
bool preventAutoSleep();
uint32_t sleepTimeout(uint32_t normalMs);
}  // namespace debugbridge
#endif
