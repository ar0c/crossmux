#pragma once
#include <atomic>
#include <cstdint>
inline std::atomic<uint32_t> testTick{0};
inline uint32_t millis() { return testTick.load(); }
struct EspStub {
  uint32_t free = 1024 * 1024, largest = 512 * 1024;
  uint32_t getFreeHeap() { return free; }
  uint32_t getMaxAllocHeap() { return largest; }
};
inline EspStub ESP;
