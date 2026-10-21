#pragma once
namespace BoardConfig {
struct Profile {
  struct Pins {
    int back, confirm, left, right;
  } input;
};
inline constexpr Profile ACTIVE{{0, 5, 4, 6}};
}  // namespace BoardConfig
