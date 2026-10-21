#include <cassert>
#include <cstdint>
#include <initializer_list>

#include "StandbySchedule.h"
#include "TestState.h"
#include "Waveshare397Standby.h"

TestState testState;
namespace Waveshare397Power {
bool isReady() { return testState.ready; }
}  // namespace Waveshare397Power

int main() {
  using Waveshare397Standby::Wake;
  using namespace standby_schedule;
  assert(sampleDue(0, 0, false, false));
  assert(!sampleDue(999, 0, true, false));
  assert(sampleDue(999, 0, true, true));          // entry just before a clock boundary still aligns sampling
  assert(sampleDue(25000, 0, true, true));        // coalesce with the clock's first minute boundary
  assert(!sampleDue(60000, 25000, true, false));  // no independent extra redraw at t=60
  assert(sampleDue(85000, 25000, true, false));
  assert(sampleDue(984, UINT32_MAX - 15, true, true));  // millis rollover
  assert(sleepSeconds(3600, 0, 0, true) == 60);
  assert(sleepSeconds(40, 0, 0, true) == 40);
  assert(sleepSeconds(3600, 59001, 0, true) == 1);
  assert(sleepSeconds(0, 0, 0, false) == 1);
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 && !defined(SIMULATOR)
  constexpr uint64_t mask = (1ULL << 0) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 38);
  assert(Waveshare397Standby::available());
  assert(Waveshare397Standby::sleepFor(60) == Wake::Timer);
  assert(testState.sleeps == 1 && testState.delayUs == 60000000);
  assert(testState.lastMask == mask);
  assert(testState.arms == 1 && testState.clears == 1 && testState.timerClears == 1 && !testState.armedMask);
  for (int pin : {0, 4, 5, 6, 38}) {
    testState = {};
    testState.lowPin = pin;
    assert(Waveshare397Standby::sleepFor(20) == Wake::Input);
    assert(testState.lowPin == pin && testState.arms == 0 && testState.sleeps == 0);
    testState.lowPin = -1;
    testState.cause = 2;
    assert(Waveshare397Standby::sleepFor(20) == Wake::Input);
    assert(testState.armedMask == 0 && testState.timerClears == 1);
  }
  for (int guard = 0; guard < 3; ++guard) {
    testState = {};
    if (guard == 0) testState.ready = false;
    if (guard == 1) testState.ble = true;
    if (guard == 2) testState.wifi = 1;
    assert(!Waveshare397Standby::available());
    assert(Waveshare397Standby::sleepFor(60) == Wake::Failed);
    assert(testState.arms == 0 && testState.sleeps == 0);
  }
  for (int failure = 0; failure < 5; ++failure) {
    testState = {};
    if (failure == 0) testState.gpioOk = false;
    if (failure == 1) testState.timerError = 1;
    if (failure == 2) testState.sleepError = 1;
    if (failure == 3) testState.cleanupError = 1;
    if (failure == 4) testState.cause = 3;
    assert(Waveshare397Standby::sleepFor(60) == Wake::Failed);
    assert(testState.armedMask == 0);
    if (failure != 0) assert(testState.clears == 1 && testState.timerClears == 1);
  }
  testState = {};
  testState.pressOnArm = true;
  assert(Waveshare397Standby::sleepFor(20) == Wake::Input);
  assert(testState.lowPin == 4 && testState.sleeps == 0 && testState.armedMask == 0);
  testState = {};
  testState.pressOnSleep = true;
  assert(Waveshare397Standby::sleepFor(20) == Wake::Input);  // input wins over timer
  assert(testState.lowPin == 38 && testState.armedMask == 0);
  for (uint32_t interval : {0u, 61u, UINT32_MAX}) {
    testState = {};
    assert(Waveshare397Standby::sleepFor(interval) == Wake::Failed);
    assert(testState.arms == 0);
  }
#else
  assert(!Waveshare397Standby::available());
  assert(Waveshare397Standby::sleepFor(60) == Wake::Failed);
  assert(testState.arms == 0);
#endif
}
