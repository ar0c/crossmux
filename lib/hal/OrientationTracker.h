#pragma once

#include <cmath>
#include <cstdint>

namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

// Sensor axes are normalized by the BSP: X points right, Y points up in portrait.
// Pure, allocation-free gravity classifier shared by every opted-in board.
class OrientationTracker {
  static constexpr uint32_t STABLE_MS = 900;
  static constexpr uint32_t MAX_SAMPLE_GAP_MS = 350;
  static constexpr uint8_t NONE = 255;
  uint8_t candidate_ = NONE;
  uint32_t since_ = 0;
  uint32_t lastSample_ = 0;
  float anchorX_ = 0, anchorY_ = 0, anchorZ_ = 0;

 public:
  void reset() { candidate_ = NONE; }

  bool update(float x, float y, float z, uint32_t now, uint8_t current, uint8_t& result) {
    const float length2 = x * x + y * y + z * z;
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    const float dominant = ax > ay ? ax : ay;
    const float other = ax > ay ? ay : ax;
    // Reject invalid/free-fall/shaken samples, near-flat poses and diagonal ambiguity.
    if (!std::isfinite(length2) || length2 < 0.49f || length2 > 1.69f || dominant < 0.65f || dominant < other * 1.4f) {
      reset();
      return false;
    }
    const uint8_t next = ay >= ax
                             ? (y > 0 ? CrossPointOrientation::PORTRAIT : CrossPointOrientation::INVERTED)
                             : (x < 0 ? CrossPointOrientation::LANDSCAPE_CW : CrossPointOrientation::LANDSCAPE_CCW);
    if (next == current) {
      reset();
      return false;
    }
    // Require the gravity vector to stay within 10 degrees of its dwell anchor.
    // This detects ongoing movement without relying on a gyroscope's health.
    // Squared dot products avoid a square root or trigonometry per sample.
    const float dot = x * anchorX_ + y * anchorY_ + z * anchorZ_;
    const float anchorLength2 = anchorX_ * anchorX_ + anchorY_ * anchorY_ + anchorZ_ * anchorZ_;
    const bool moved = dot <= 0 || dot * dot < 0.969846f * length2 * anchorLength2;
    if (next != candidate_ || now - lastSample_ > MAX_SAMPLE_GAP_MS || moved) {
      candidate_ = next;
      since_ = now;
      anchorX_ = x;
      anchorY_ = y;
      anchorZ_ = z;
    }
    lastSample_ = now;
    if (now - since_ < STABLE_MS) return false;
    result = next;
    reset();
    return true;
  }
};
