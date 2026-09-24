#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WeReadTime {

// Physical device + the existing content-based local book ID. This is a
// different ledger namespace from the unowned, pre-migration source ID.
inline bool deviceSource(const uint8_t device[6], const char* localBookId, char (&out)[64]) {
  out[0] = '\0';
  if (!device || !localBookId || std::strlen(localBookId) != 32) return false;
  bool allZero = true, allOnes = true;
  for (unsigned i = 0; i < 6; ++i) {
    allZero &= device[i] == 0;
    allOnes &= device[i] == 0xff;
  }
  if (allZero || allOnes) return false;
  for (unsigned i = 0; i < 32; ++i) {
    const char c = localBookId[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  constexpr char hex[] = "0123456789abcdef";
  out[0] = 'd';
  for (unsigned i = 0; i < 6; ++i) {
    out[1 + i * 2] = hex[device[i] >> 4];
    out[2 + i * 2] = hex[device[i] & 15];
  }
  out[13] = '_';
  std::memcpy(out + 14, localBookId, 32);
  out[46] = '\0';
  return true;
}

inline bool belongsToDevice(const char* source, const uint8_t device[6]) {
  if (!source || !device || std::strlen(source) != 46 || source[0] != 'd' || source[13] != '_') return false;
  char expected[64];
  return deviceSource(device, source + 14, expected) && std::strcmp(source, expected) == 0;
}

}  // namespace WeReadTime
