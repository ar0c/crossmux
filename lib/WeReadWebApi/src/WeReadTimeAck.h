#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WeReadTime {
// Only field categories, never the server's raw synckey or response body.
enum class AckField : int8_t { Missing = -1, Zero = 0, One = 1, Other = 2 };
struct AckObservation {
  AckField succ = AckField::Missing;
  AckField synckey = AckField::Missing;
};
// Deliberately restricted flat response grammar. Unknown fields/shapes require
// review rather than widening acknowledgement semantics. No generic JSON parser
// tolerance (duplicate keys, trailing data, nested success) reaches a time ACK.
inline bool acceptedTimeAck(const char* text, size_t length, bool entry, AckObservation* observation = nullptr) {
  if (observation) *observation = {};
  if (!text || !length || length > 512) return false;
  size_t at = 0;
  const auto space = [&]() {
    while (at < length && (text[at] == ' ' || text[at] == '\r' || text[at] == '\n' || text[at] == '\t')) ++at;
  };
  const auto take = [&](char value) { space(); return at < length && text[at++] == value; };
  if (!take('{')) return false;
  uint8_t seen = 0;
  bool acknowledged = false;
  space();
  if (at < length && text[at] == '}') { ++at; space(); return entry && at == length; }
  for (unsigned fields = 0; fields < 5; ++fields) {
    if (!take('"')) return false;
    const size_t start = at;
    while (at < length && text[at] != '"') {
      if (text[at] < 'A' || text[at] > 'z') return false;
      ++at;
    }
    if (at >= length) return false;
    const size_t count = at++ - start;
    uint8_t field = 0;
    const char* keys[] = {"succ", "synckey", "errcode", "errCode", "errorCode"};
    for (unsigned i = 0; i < 5; ++i) {
      if (std::strlen(keys[i]) == count && !std::memcmp(keys[i], text + start, count)) field = uint8_t(1U << i);
    }
    if (!field || (seen & field) || !take(':')) return false;
    seen |= field;
    space();
    uint64_t value = 0;
    if (field == 1 && length - at >= 4 && !std::memcmp(text + at, "true", 4)) at += 4, value = 1;
    else {
      if (at >= length || text[at] < '0' || text[at] > '9') return false;
      const bool leadingZero = text[at] == '0';
      unsigned digits = 0;
      while (at < length && text[at] >= '0' && text[at] <= '9') {
        const unsigned digit = unsigned(text[at++] - '0');
        if ((leadingZero && digits) || value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit; ++digits;
      }
    }
    if (observation) {
      const auto category = value == 0 ? AckField::Zero : value == 1 ? AckField::One : AckField::Other;
      if (field == 1) observation->succ = category;
      if (field == 2) observation->synckey = category;
    }
    if (field == 1) { if (value != 1) return false; acknowledged = true; }
    else if (field == 2) acknowledged = true;
    else if (value != 0) return false;
    space();
    if (at >= length) return false;
    if (text[at] == '}') { ++at; space(); return at == length && (entry || acknowledged); }
    if (text[at++] != ',') return false;
  }
  return false;
}
}  // namespace WeReadTime
