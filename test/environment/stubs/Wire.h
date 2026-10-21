#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

struct TestWire {
  std::vector<uint16_t> commands;
  std::vector<uint8_t> payload{0xBE, 0xEF, 0x92, 0xBE, 0xEF, 0x92};
  uint16_t pending = 0;
  size_t cursor = 0;
  unsigned failAt = 0;
  void beginTransmission(uint8_t address) {
    assert(address == 0x70);
    pending = 0;
  }
  size_t write(uint8_t byte) {
    pending = static_cast<uint16_t>((pending << 8) | byte);
    return 1;
  }
  int endTransmission() {
    commands.push_back(pending);
    return commands.size() == failAt ? 2 : 0;
  }
  uint8_t requestFrom(uint8_t address, uint8_t count, uint8_t stop) {
    assert(address == 0x70 && count == 6 && stop == 1);
    cursor = 0;
    return static_cast<uint8_t>(payload.size());
  }
  int read() { return payload.at(cursor++); }
};
extern TestWire Wire;
