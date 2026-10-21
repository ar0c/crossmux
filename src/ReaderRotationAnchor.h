#pragma once

#include <cstdint>
#include <optional>

// A new layout starts before the original visible position. Keep that original
// position through consecutive rotations instead of repeatedly rounding it down
// to each new page boundary. Navigation and other layout changes reset it.
class ReaderRotationAnchor {
  std::optional<uint32_t> offset_;
  int spine_ = -1;

 public:
  void reset() { offset_.reset(); }

  std::optional<uint32_t> capture(int spine, std::optional<uint32_t> visibleOffset) {
    if (!visibleOffset) {
      reset();
      return std::nullopt;
    }
    if (!offset_ || spine != spine_) {
      offset_ = visibleOffset;
      spine_ = spine;
    }
    return offset_;
  }
};
