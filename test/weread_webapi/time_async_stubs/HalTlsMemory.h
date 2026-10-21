#pragma once
namespace fakeTlsMemory {
inline bool external = true;
}
class HalTlsMemory {
 public:
  static bool usesPsram() { return fakeTlsMemory::external; }
};
