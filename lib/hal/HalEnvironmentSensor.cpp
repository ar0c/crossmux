#include "HalEnvironmentSensor.h"

#include <BoardConfig.h>

#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
#include <Arduino.h>
#include <Wire.h>

namespace {
// Sensirion SHTC3 datasheet, sections 5.2-5.9. SHT40's one-byte commands
// and humidity conversion are incompatible with this sensor.
constexpr uint8_t ADDRESS = 0x70;
constexpr uint16_t WAKE = 0x3517;
constexpr uint16_t MEASURE = 0x7866;  // normal mode, temperature first, no clock stretching
constexpr uint16_t SLEEP = 0xB098;

bool command(const uint16_t value) {
  Wire.beginTransmission(ADDRESS);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value));
  return Wire.endTransmission() == 0;
}

bool validCrc(const uint8_t* bytes) {
  uint8_t crc = 0xFF;
  for (int b = 0; b < 2; ++b) {
    crc ^= bytes[b];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
  }
  return crc == bytes[2];
}

bool measure(HalEnvironmentSensor::Reading& reading) {
  if (!command(WAKE)) return false;
  delay(1);  // wake-up max 240 us
  if (!command(MEASURE)) return false;
  delay(13);  // normal-mode conversion max 12.1 ms
  if (Wire.requestFrom(ADDRESS, static_cast<uint8_t>(6), static_cast<uint8_t>(true)) != 6) return false;
  uint8_t bytes[6];
  for (auto& byte : bytes) byte = static_cast<uint8_t>(Wire.read());
  if (!validCrc(bytes) || !validCrc(bytes + 3)) return false;
  const uint16_t rawT = static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
  const uint16_t rawRh = static_cast<uint16_t>((bytes[3] << 8) | bytes[4]);
  reading.temperatureC = -45.0f + 175.0f * rawT / 65536.0f;
  reading.humidityPct = 100.0f * rawRh / 65536.0f;
  return true;
}
}  // namespace
#endif

bool HalEnvironmentSensor::supported() {
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
  return true;
#else
  return false;
#endif
}

bool HalEnvironmentSensor::read(Reading& reading) {
  reading = {};
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
  // Board power/RTC startup initialized Wire. Do not reset/reconfigure
  // their bus or add a task. The caller serializes its snapshot with rendering.
  Reading next;
  const bool measured = measure(next);
  const bool sleeping = command(SLEEP);
  if (measured && sleeping) {
    next.valid = true;
    reading = next;
  }
#endif
  return reading.valid;
}
