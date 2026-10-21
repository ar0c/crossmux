#include <cassert>
#include <iostream>

#include "JoystickGesture.h"
int main() {
  struct MountedCase {
    int rawX;
    int rawY;
    Action expected;
  };
  // Raw samples captured from the user's physical Up, Down, Left, Right.
  // USB logical directions are not transformed.
  constexpr MountedCase cases[] = {
      {127, 3, Action::Up},    {-126, 2, Action::Down}, {0, -127, Action::Left},
      {0, 127, Action::Right}, {0, -128, Action::Left}, {-128, 0, Action::Down},
  };
  for (const auto& sample : cases) {
    JoystickGesture physical;
    const auto axes = mountedJoystickAxes(sample.rawX, sample.rawY);
    physical.update(0, 0, false, 0);
    if (physical.update(axes.x, axes.y, false, 20) != sample.expected) {
      std::cerr << "mounted direction mismatch for raw " << sample.rawX << "," << sample.rawY << "\n";
      return 1;
    }
    assert(physical.update(axes.x, axes.y, false, 2000) == Action::None);
  }
  JoystickGesture g;
  assert(g.update(100, 0, false, 0) == Action::None);  // held at boot
  assert(g.update(0, 0, false, 20) == Action::None);
  assert(g.update(20, 0, false, 40) == Action::None);    // dead zone
  assert(g.update(100, 98, false, 60) == Action::None);  // diagonal
  assert(g.update(100, 0, false, 80) == Action::Right);
  assert(g.update(100, 0, false, 2000) == Action::None);   // no cross-page repeat
  assert(g.update(-100, 0, false, 2020) == Action::None);  // center required
  g.update(0, 0, false, 2040);
  assert(g.update(-100, 0, false, 2060) == Action::Left);
  g.reset();
  assert(g.update(0, 0, true, 2100) == Action::None);  // held click after reconnect
  assert(g.update(0, 0, false, 2800) == Action::None);
  assert(g.update(0, 0, true, 2900) == Action::None);
  assert(g.update(0, 0, false, 2980) == Action::Confirm);
  g.update(0, 0, true, 3000);
  assert(g.update(0, 0, true, 3650) == Action::Back);
  assert(g.update(0, 0, false, 3700) == Action::None);
  g.update(0, 0, false, 3720);
  assert(g.update(0, -100, false, 3740) == Action::Up);
  g.update(0, 0, false, 3760);
  assert(g.update(0, 100, false, 3780) == Action::Down);
  std::cout << "joystick: center gate, diagonal, dead zone, no repeat, reconnect, click/long passed\n";
}
