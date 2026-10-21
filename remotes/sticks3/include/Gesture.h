#pragma once
#include <cstdint>

enum class Action : uint8_t { None, Prev, Next, Back, Confirm, Up, Down, Left, Right };
constexpr uint32_t kLongPressMs = 650;
constexpr uint32_t kDebounceMs = 25;

// One action per gesture, including a long press followed by release.
class Gesture {
 public:
  Action update(bool pressed, uint32_t now, Action shortAction, Action longAction) {
    if (pressed && !held_) {
      held_ = true;
      fired_ = false;
      started_ = now;
    }
    if (pressed && !fired_ && uint32_t(now - started_) >= kLongPressMs) {
      fired_ = true;
      return longAction;
    }
    if (!pressed && held_) {
      held_ = false;
      if (!fired_ && uint32_t(now - started_) >= kDebounceMs) return shortAction;
    }
    return Action::None;
  }

 private:
  bool held_ = false;
  bool fired_ = false;
  uint32_t started_ = 0;
};

inline const char* actionName(Action action) {
  switch (action) {
    case Action::None:
      return "NONE";
    case Action::Prev:
      return "PREV";
    case Action::Next:
      return "NEXT";
    case Action::Back:
      return "BACK";
    case Action::Confirm:
      return "CONFIRM";
    case Action::Up:
      return "UP";
    case Action::Down:
      return "DOWN";
    case Action::Left:
      return "LEFT";
    case Action::Right:
      return "RIGHT";
  }
  return "NONE";
}
inline uint8_t hidCode(Action action) {
  switch (action) {
    case Action::None:
      return 0;
    case Action::Prev:
      return 0x4b;
    case Action::Next:
      return 0x4e;
    case Action::Back:
      return 0x29;
    case Action::Confirm:
      return 0x28;
    case Action::Up:
      return 0x52;
    case Action::Down:
      return 0x51;
    case Action::Left:
      return 0x50;
    case Action::Right:
      return 0x4f;
  }
  return 0;
}
