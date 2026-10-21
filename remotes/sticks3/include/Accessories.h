#pragma once
#include <cstdint>

#include "JoystickGesture.h"

class Accessories {
 public:
  void begin();
  Action poll(uint32_t now, bool linked);
  bool inject(const char* direction, uint32_t duration, uint32_t now);
  bool pulse(uint32_t now, uint32_t duration = 35);
  bool busy() const { return virtualUntil_ != 0; }
  bool centered() const { return x > -22 && x < 22 && y > -22 && y < 22 && !pressed; }
  bool online = false, pressed = false, motorActive = false, pwmReady = false;
  int8_t x = 0, y = 0;
  uint8_t firmware = 0;
  uint32_t reads = 0, failures = 0, events = 0, pulses = 0;

 private:
  JoystickGesture gesture_;
  bool lastLinked_ = false, virtualPressed_ = false;
  bool sampleValid_ = false;
  int8_t virtualX_ = 0, virtualY_ = 0;
  uint8_t failureStreak_ = 0;
  uint32_t nextRead_ = 0, nextProbe_ = 0, virtualUntil_ = 0, motorUntil_ = 0, bootAt_ = 0;
};
