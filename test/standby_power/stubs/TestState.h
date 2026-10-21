#pragma once
#include <cstdint>
struct TestState {
  bool ready = true, ble = false, gpioOk = true, pressOnArm = false, pressOnSleep = false;
  int wifi = 0, lowPin = -1, timerError = 0, sleepError = 0, cleanupError = 0;
  int cause = 1, sleeps = 0, arms = 0, clears = 0, timerClears = 0;
  uint64_t armedMask = 0, lastMask = 0, delayUs = 0;
};
extern TestState testState;
