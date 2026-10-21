#include "Gesture.h"

#include <cassert>
#include <iostream>
int main() {
  Gesture g;
  assert(g.update(true, 100, Action::Prev, Action::Back) == Action::None);
  assert(g.update(false, 180, Action::Prev, Action::Back) == Action::Prev);
  assert(g.update(false, 190, Action::Prev, Action::Back) == Action::None);
  assert(g.update(true, 200, Action::Prev, Action::Back) == Action::None);
  assert(g.update(true, 850, Action::Prev, Action::Back) == Action::Back);
  assert(g.update(true, 2000, Action::Prev, Action::Back) == Action::None);
  assert(g.update(false, 2100, Action::Prev, Action::Back) == Action::None);
  assert(g.update(true, 2200, Action::Next, Action::Confirm) == Action::None);
  assert(g.update(false, 2210, Action::Next, Action::Confirm) == Action::None);
  assert(g.update(true, 0xfffffff0u, Action::Next, Action::Confirm) == Action::None);
  assert(g.update(false, 0x40, Action::Next, Action::Confirm) == Action::Next);
  assert(hidCode(Action::Prev) == 0x4b && hidCode(Action::Next) == 0x4e);
  std::cout << "gesture: short, long, no repeat, bounce, wrap passed\n";
}
