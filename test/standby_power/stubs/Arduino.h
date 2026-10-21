#pragma once
#include "TestState.h"
constexpr int LOW = 0, HIGH = 1;
inline int digitalRead(int pin) { return testState.lowPin == pin ? LOW : HIGH; }
