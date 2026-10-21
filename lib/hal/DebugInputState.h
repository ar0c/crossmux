#pragma once

#include <cstdint>

// Fixed storage, used only by the USB debug build. No scheduler, heap or SDK.
class DebugInputState {
 public:
  enum class Result : uint8_t { Idle, Pending, Complete, Cancelled };
  bool start(uint8_t button, uint32_t duration) {
    if (result_ == Result::Pending || button >= 7 || duration < 30 || duration > 3000) return false;
    bit_ = 1u << button;
    duration_ = duration;
    started_ = false;
    result_ = Result::Pending;
    return true;
  }
  void update(uint32_t now, bool usb, bool physicalActivity) {
    cancelled_ = false;
    pressed_ = released_ = 0;
    if (result_ != Result::Pending) return;
    if (!usb || physicalActivity) {
      cancel();
      return;
    }
    if (!started_) {
      started_ = true;
      start_ = now;
      held_ = bit_;
      pressed_ = bit_;
    } else if (now - start_ >= duration_) {
      held_ = 0;
      released_ = bit_;
      result_ = Result::Complete;
    }
    elapsed_ = now - start_;
  }
  void cancel() {
    if (result_ == Result::Pending) {
      result_ = Result::Cancelled;
      cancelled_ = true;
    }
    held_ = pressed_ = released_ = 0;
  }
  bool pressed(uint8_t button) const { return button < 7 && (pressed_ & (1u << button)); }
  bool released(uint8_t button) const { return button < 7 && (released_ & (1u << button)); }
  bool held(uint8_t button) const { return button < 7 && (held_ & (1u << button)); }
  bool anyPressed() const { return pressed_ != 0; }
  bool anyReleased() const { return released_ != 0; }
  bool hasDuration() const { return held_ || released_; }
  uint32_t elapsed() const { return elapsed_; }
  Result result() const { return result_; }
  bool cancelledThisFrame() const { return cancelled_; }

 private:
  Result result_ = Result::Idle;
  uint8_t bit_ = 0, held_ = 0, pressed_ = 0, released_ = 0;
  bool started_ = false;
  bool cancelled_ = false;
  uint32_t start_ = 0, duration_ = 0, elapsed_ = 0;
};
