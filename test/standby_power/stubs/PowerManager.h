#pragma once
#include <cassert>

#include "TestState.h"
namespace freeink {
struct PowerManager {
  static bool armLightSleepWakeup(uint64_t mask, bool low) {
    assert(low);
    ++testState.arms;
    testState.lastMask = mask;
    testState.armedMask = testState.gpioOk ? mask : 0;
    return testState.gpioOk;
  }
  static void clearLightSleepWakeup(uint64_t mask) {
    assert(mask == testState.armedMask);
    testState.armedMask = 0;
    ++testState.clears;
  }
};
}  // namespace freeink
