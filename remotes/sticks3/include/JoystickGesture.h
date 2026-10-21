#pragma once
#include "Gesture.h"

struct JoystickAxes {
  int x;
  int y;
};
// Current physical samples: Up=(127,3), Down=(-126,2), Left=(0,-127),
// Right=(0,127). Screen axes are +x right and +y down.
// Swap raw axes and invert logical Y; do not infer orientation from UI motion.
constexpr JoystickAxes mountedJoystickAxes(int rawX, int rawY) { return {rawY, -rawX}; }

// Require center after boot/reconnect, each deflection, and a click. No repeats
// can spill across an e-paper activity transition while the stick remains held.
class JoystickGesture {
 public:
  void reset() {
    armed_ = false;
    click_ = Gesture{};
    buttonBlocked_ = true;
  }
  Action update(int x, int y, bool pressed, uint32_t now) {
    const int ax = x < 0 ? -x : x;
    const int ay = y < 0 ? -y : y;
    if (buttonBlocked_) {
      if (pressed) return Action::None;
      buttonBlocked_ = false;
    }
    const Action click = click_.update(pressed, now, Action::Confirm, Action::Back);
    if (pressed || click != Action::None) {
      armed_ = false;
      return click;
    }
    if (ax < 22 && ay < 22) {
      armed_ = true;
      return Action::None;
    }
    if (!armed_ || (ax < 34 && ay < 34)) return Action::None;
    // Ambiguous diagonals wait for an intentional dominant axis.
    if (ax <= ay + 7 && ay <= ax + 7) return Action::None;
    armed_ = false;
    if (ax > ay) return x < 0 ? Action::Left : Action::Right;
    return y < 0 ? Action::Up : Action::Down;
  }

 private:
  bool armed_ = false;
  bool buttonBlocked_ = true;
  Gesture click_;
};
