#include "Accessories.h"

#include <M5Unified.h>

#include <cstring>

namespace {
constexpr uint8_t kAddress = 0x63, kMotorPin = 1, kPwmChannel = 4;
constexpr uint32_t kI2cHz = 100000;
bool reached(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
}  // namespace

void Accessories::begin() {
  // Existing user-confirmed wiring: Joystick2 SDA9/SCL10, vibration HAT G1.
  // Never drive the boot strap G0. Channel 7 is the display backlight.
  pinMode(kMotorPin, OUTPUT);
  digitalWrite(kMotorPin, LOW);
  pwmReady = ledcAttachChannel(kMotorPin, 10000, 8, kPwmChannel);
  if (pwmReady) ledcWriteChannel(kPwmChannel, 0);
  M5.Power.setExtOutput(true);  // Shared 5V supply keeps the joystick available.
  M5.Ex_I2C.begin(I2C_NUM_0, 9, 10);
  bootAt_ = millis();
  gesture_.reset();
}

bool Accessories::pulse(uint32_t now, uint32_t duration) {
  if (!pwmReady || uint32_t(now - bootAt_) < 2500 || duration < 10 || duration > 250) return false;
  ledcWriteChannel(kPwmChannel, 128);
  motorActive = true;
  motorUntil_ = now + duration;
  ++pulses;
  return true;
}

bool Accessories::inject(const char* direction, uint32_t duration, uint32_t now) {
  if (busy() || !centered() || duration < 30 || duration > 2000) return false;
  virtualX_ = virtualY_ = 0;
  virtualPressed_ = false;
  if (!strcmp(direction, "UP"))
    virtualY_ = -100;
  else if (!strcmp(direction, "DOWN"))
    virtualY_ = 100;
  else if (!strcmp(direction, "LEFT"))
    virtualX_ = -100;
  else if (!strcmp(direction, "RIGHT"))
    virtualX_ = 100;
  else if (!strcmp(direction, "CLICK"))
    virtualPressed_ = true;
  else
    return false;
  // The synthetic input shares the physical classifier and release path.
  gesture_.reset();
  gesture_.update(0, 0, false, now);
  virtualUntil_ = now + duration;
  return true;
}

Action Accessories::poll(uint32_t now, bool linked) {
  if (motorActive && reached(now, motorUntil_)) {
    ledcWriteChannel(kPwmChannel, 0);
    motorActive = false;
  }
  if (lastLinked_ != linked) {
    gesture_.reset();
    lastLinked_ = linked;
  }
  if (reached(now, nextRead_)) {
    nextRead_ = now + 20;
    if (!online && reached(now, nextProbe_)) {
      nextProbe_ = now + 2000;
      online = M5.Ex_I2C.scanID(kAddress, kI2cHz) && M5.Ex_I2C.readRegister(kAddress, 0xfe, &firmware, 1, kI2cHz);
      if (online) {
        failureStreak_ = 0;
        gesture_.reset();
      }
    }
    if (online) {
      uint8_t axes[2]{}, button = 1;
      // Registers from the official M5Unit-Joystick2 driver. Read only: never
      // rewrite calibration, I2C address, firmware or bootloader registers.
      if (M5.Ex_I2C.readRegister(kAddress, 0x60, axes, 2, kI2cHz) &&
          M5.Ex_I2C.readRegister(kAddress, 0x20, &button, 1, kI2cHz)) {
        x = int8_t(axes[0]);
        y = int8_t(axes[1]);
        pressed = button == 0;
        ++reads;
        failureStreak_ = 0;
        sampleValid_ = true;
      } else {
        ++failures;
        sampleValid_ = false;
        gesture_.reset();
        if (++failureStreak_ >= 3) {
          online = false;
          x = y = 0;
          pressed = false;
          gesture_.reset();
        }
        return Action::None;
      }
    }
  }
  const bool injecting = virtualUntil_ != 0 && !reached(now, virtualUntil_);
  const bool releasing = virtualUntil_ != 0 && !injecting;
  if (releasing) virtualUntil_ = 0;
  if (!linked || ((!online || !sampleValid_) && !injecting && !releasing)) return Action::None;
  const auto axes = mountedJoystickAxes(x, y);
  // USB directions are already logical; only the physical raw sample is mounted.
  const Action action = gesture_.update(injecting ? virtualX_ : axes.x, injecting ? virtualY_ : axes.y,
                                        injecting ? virtualPressed_ : pressed, now);
  if (action != Action::None) ++events;
  return action;
}
