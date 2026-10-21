#pragma once
#include "TestState.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_SLEEP_WAKEUP_TIMER = 1, ESP_SLEEP_WAKEUP_GPIO = 2;
inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t delay) {
  testState.delayUs = delay;
  if (testState.pressOnArm) testState.lowPin = 4;
  return testState.timerError;
}
inline esp_err_t esp_light_sleep_start() {
  ++testState.sleeps;
  if (testState.pressOnSleep) testState.lowPin = 38;
  return testState.sleepError;
}
inline int esp_sleep_get_wakeup_cause() { return testState.cause; }
inline esp_err_t esp_sleep_disable_wakeup_source(int source) {
  if (source == ESP_SLEEP_WAKEUP_TIMER) ++testState.timerClears;
  return testState.cleanupError;
}
