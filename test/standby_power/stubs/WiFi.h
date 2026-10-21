#pragma once
#include "TestState.h"
constexpr int WIFI_MODE_NULL = 0;
inline struct TestWifi {
  int getMode() const { return testState.wifi; }
} WiFi;
