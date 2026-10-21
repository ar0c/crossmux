#include "Waveshare397Standby.h"

#include <BoardConfig.h>

#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
#include <Arduino.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#if FREEINK_CAP_BLE_HID_HOST
#include <BleKeyboardHost.h>
#endif
#include "Waveshare397Power.h"

namespace {
uint64_t wakeMask() {
  const auto& input = BoardConfig::ACTIVE.input;
  const int pins[] = {input.back, input.confirm, input.left, input.right, Waveshare397Power::POWER_IRQ_PIN};
  uint64_t mask = 0;
  for (const int pin : pins) {
    if (pin < 0 || pin >= 64) return 0;
    mask |= uint64_t{1} << pin;
  }
  return mask;
}

bool inputPending(const uint64_t mask) {
  for (int pin = 0; pin < 64; ++pin)
    if ((mask & (uint64_t{1} << pin)) && digitalRead(pin) == LOW) return true;
  return false;
}
}  // namespace
#endif

bool Waveshare397Standby::available() {
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
  // Manual light sleep cannot preserve radio links. Leave the existing BLE
  // lifecycle alone rather than disconnecting a paired remote for standby.
  if (!Waveshare397Power::isReady() || WiFi.getMode() != WIFI_MODE_NULL) return false;
#if FREEINK_CAP_BLE_HID_HOST
  if (freeink::BleKeyboardHost::getInstance().isRunning()) return false;
#endif
#if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
  return false;  // PMIC IRQ GPIO38 needs the digital GPIO light-sleep wake source.
#else
  return wakeMask() != 0;
#endif
#else
  return false;
#endif
}

Waveshare397Standby::Wake Waveshare397Standby::sleepFor(const uint32_t seconds) {
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
  if (seconds == 0 || seconds > 60 || !available()) return Wake::Failed;
  const uint64_t mask = wakeMask();
  if (inputPending(mask)) return Wake::Input;
  // GPIO0/4/5/6 are already pulled-up by InputManager; GPIO38 by PMIC startup.
  // The SDK arms digital level wake, not RTC EXT1, and rolls back partial setup.
  if (!freeink::PowerManager::armLightSleepWakeup(mask, true)) return Wake::Failed;
  const esp_err_t armed = esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  esp_err_t slept = ESP_OK;
  const bool pending = inputPending(mask);  // close the input/setup race
  if (armed == ESP_OK && !pending) slept = esp_light_sleep_start();
  const auto cause = esp_sleep_get_wakeup_cause();
  const bool input = pending || inputPending(mask);
  freeink::PowerManager::clearLightSleepWakeup(mask);
  const esp_err_t cleaned = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  if (armed != ESP_OK || slept != ESP_OK || cleaned != ESP_OK) {
    LOG_ERR("PWR", "Waveshare light sleep failed: timer=%d sleep=%d cleanup=%d", armed, slept, cleaned);
    return Wake::Failed;
  }
  if (input || cause == ESP_SLEEP_WAKEUP_GPIO) return Wake::Input;
  return cause == ESP_SLEEP_WAKEUP_TIMER ? Wake::Timer : Wake::Failed;
#else
  (void)seconds;
  return Wake::Failed;
#endif
}
