#pragma once

// Ambient measurements, separate from the ESP's internal chip temperature.
namespace HalEnvironmentSensor {
struct Reading {
  float temperatureC = 0;
  float humidityPct = 0;
  bool valid = false;
};

bool supported();
// Single shot on the shared, already initialized sensor bus. Always attempts
// to put SHTC3 back to sleep, including on CRC/I2C errors. No cached old values.
bool read(Reading& reading);
}  // namespace HalEnvironmentSensor
