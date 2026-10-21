#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace debugbridge {
struct Command {
  uint32_t session = 0, sequence = 0;
  char verb[16]{}, arg[16]{}, duration[11]{};
  int fields = 0;
};
inline bool number(const char* text, uint32_t& value, int base = 10) {
  if (!text || !*text || *text == '-' || *text == '+') return false;
  if (std::strlen(text) > (base == 16 ? 8u : 10u)) return false;
  for (const char* p = text; *p; ++p) {
    if (!(*p >= '0' && *p <= '9') && !(base == 16 && ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))))
      return false;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(text, &end, base);
  if (*end || parsed > UINT32_MAX) return false;
  value = static_cast<uint32_t>(parsed);
  return true;
}
inline bool parse(const char* line, Command& out) {
  char session[9]{}, sequence[11]{}, extra[2]{};
  out.fields =
      std::sscanf(line, "DBG1 %8s %10s %15s %15s %10s %1s", session, sequence, out.verb, out.arg, out.duration, extra);
  return out.fields >= 3 && out.fields <= 5 && number(session, out.session, 16) && out.session != 0 &&
         number(sequence, out.sequence) && out.sequence != 0;
}
// Partial or oversized input must never block the UI or become a truncated command.
class LineBuffer {
 public:
  bool feed(char c, uint32_t now) {
    if ((used_ || dropping_) && now - lastByte_ > 500) {
      used_ = 0;
      dropping_ = true;  // discard the rest of the interrupted line
    }
    lastByte_ = now;
    if (c == '\r') return false;
    if (c == '\n') {
      if (dropping_) {
        used_ = 0;
        dropping_ = false;
        return false;
      }
      data_[used_] = 0;
      const bool ready = used_ != 0;
      used_ = 0;
      return ready;
    }
    if (dropping_) return false;
    if (used_ == sizeof(data_) - 1) {
      used_ = 0;
      dropping_ = true;
      return false;
    }
    data_[used_++] = c;
    return false;
  }
  const char* line() const { return data_; }

 private:
  char data_[128]{};
  size_t used_ = 0;
  uint32_t lastByte_ = 0;
  bool dropping_ = false;
};
inline uint32_t checksum(const uint8_t* data, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; ++i) hash = (hash ^ data[i]) * 16777619u;
  return hash;
}
}  // namespace debugbridge
