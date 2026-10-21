#pragma once
#include "TestState.h"
namespace freeink {
struct BleKeyboardHost {
  static BleKeyboardHost& getInstance() {
    static BleKeyboardHost host;
    return host;
  }
  bool isRunning() const { return testState.ble; }
};
}  // namespace freeink
