#include <GfxRenderer.h>
#include <HalEnvironmentSensor.h>
#include <Wire.h>

#include <cassert>
#include <cmath>
#include <vector>

#include "components/EnvironmentOverlay.h"

TestWire Wire;
unsigned delayMs = 0;

int main() {
  using HalEnvironmentSensor::Reading;
  assert(!EnvironmentOverlay::visibleReadingChanged({25.21f, 50.1f, true}, {25.24f, 50.2f, true}));
  assert(EnvironmentOverlay::visibleReadingChanged({25.21f, 50.1f, true}, {25.31f, 50.2f, true}));
  assert(EnvironmentOverlay::visibleReadingChanged({25.21f, 50.1f, true}, {25.21f, 51.1f, true}));
  assert(EnvironmentOverlay::visibleReadingChanged({}, {25, 50, true}));
  assert(EnvironmentOverlay::visibleReadingChanged({25, 50, true}, {}));
  assert(!EnvironmentOverlay::visibleReadingChanged({}, {}));
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
  assert(HalEnvironmentSensor::supported());
  Reading reading;
  assert(HalEnvironmentSensor::read(reading));
  assert(reading.valid);
  assert(std::abs(reading.temperatureC - 85.5218f) < 0.001f);
  assert(std::abs(reading.humidityPct - 74.5834f) < 0.001f);
  assert((Wire.commands == std::vector<uint16_t>{0x3517, 0x7866, 0xB098}));
  assert(delayMs == 14);

  // Failure at wake/measurement/sleep never leaves a valid or stale reading.
  for (unsigned failAt = 1; failAt <= 3; ++failAt) {
    Wire = {};
    Wire.failAt = failAt;
    reading = {25, 50, true};
    assert(!HalEnvironmentSensor::read(reading));
    assert(!reading.valid && reading.temperatureC == 0 && reading.humidityPct == 0);
    assert(Wire.commands.back() == 0xB098);
    assert(Wire.commands.size() <= 3);
  }
  // A short response, and either independently corrupted CRC, are all rejected.
  for (int failure = 0; failure < 3; ++failure) {
    Wire = {};
    if (failure == 0)
      Wire.payload.pop_back();
    else
      Wire.payload[failure == 1 ? 2 : 5] ^= 1;
    assert(!HalEnvironmentSensor::read(reading));
    assert(!reading.valid && Wire.commands.back() == 0xB098);
  }
  // Recover on the next explicit request, with no automatic replay or bus reset.
  Wire = {};
  assert(HalEnvironmentSensor::read(reading));

  GfxRenderer renderer;
  for (bool landscape : {false, true}) {
    renderer.width = landscape ? 800 : 480;
    renderer.height = landscape ? 480 : 800;
    renderer.mode = GfxRenderer::BW;
    renderer.absolute = false;
    EnvironmentOverlay::draw(renderer, {25.25f, 99.8f, true}, false, 38);
    assert(renderer.text == "25.2°C  100%RH");
    assert(!renderer.fillBlack);
    assert(renderer.fillY + renderer.fillHeight == renderer.height - 4 - 38);
    assert(EnvironmentOverlay::reservedHeight(renderer, 38) == 66);
    EnvironmentOverlay::draw(renderer, {}, true);
    assert(renderer.text == "Before sleep: Temperature / humidity unavailable");
    for (auto plane : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.mode = plane;
      const int before = renderer.textDraws;
      EnvironmentOverlay::draw(renderer, reading, true);
      assert(renderer.fillBlack && renderer.textDraws == before);
      renderer.absolute = true;
      EnvironmentOverlay::draw(renderer, reading, true);
      assert(!renderer.fillBlack && renderer.textDraws == before + 1);
      renderer.absolute = false;
    }
  }
#else
  assert(!HalEnvironmentSensor::supported());
  Reading reading{25, 50, true};
  assert(!HalEnvironmentSensor::read(reading) && !reading.valid);
  assert(Wire.commands.empty());
  GfxRenderer renderer;
  EnvironmentOverlay::draw(renderer, reading, true);
  assert(renderer.fills == 0 && renderer.textDraws == 0);
  assert(EnvironmentOverlay::reservedHeight(renderer, 38) == 0);
#endif
}
