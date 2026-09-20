#pragma once
#include <cstdio>
#include <cstring>
#include "WeReadTimeLedger.h"

namespace WeReadTime {
// Exact canonical schema emitted by the audited companion installer. Rejects
// unknown fields, duplicate keys/days, changed hashes, trailing data and manual
// edits. Deliberately not a permissive general-purpose JSON parser.
class HandoverManifest {
 public:
  enum class Next { More, End, Invalid };
  // Diagnostic only: reuses the fixed scratch buffer, and never opens the
  // upload gate. Exact scope/schema prevents labeling corrupt data as a pause.
  template<class File>
  bool hostPaused(File& file, const Identity& id) {
    closed_ = false;
    offset_ = previous_ = 0;
    Ledger validator;
    if (!validator.bind(id.account, id.book, id.source, id.day) || !file.seek64(0)) return false;
    if (!match(file, std::snprintf(text_, sizeof(text_),
        "{\n  \"account\": \"%s\",\n  \"book\": \"%s\",\n  \"handback_plan_sha256\": \"", id.account, id.book))) return false;
    for (unsigned i = 0; i < 64; ++i) {
      uint8_t c = 0;
      if (file.read(&c, 1) != 1 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
      ++offset_;
    }
    return match(file, std::snprintf(text_, sizeof(text_),
        "\",\n  \"schema\": 1,\n  \"sender_enabled\": false,\n  \"source\": \"%s\",\n  \"state\": \"host_probe_paused\"\n}", id.source)) &&
        offset_ == file.fileSize64();
  }
  template<class File>
  Next next(File& file) const {
    uint8_t value = 0;
    if (file.read(&value, 1) != 1 || !file.seek64(offset_)) return Next::Invalid;
    return value == ',' ? Next::More : (value == '\n' ? Next::End : Next::Invalid);
  }
  template<class File>
  bool begin(File& file, const Identity& identity) {
    closed_ = false;
    offset_ = previous_ = 0;
    Ledger validator;
    if (!validator.bind(identity.account, identity.book, identity.source, identity.day)) return false;
    identity_ = identity;
    return match(file, std::snprintf(text_, sizeof(text_),
      "{\n  \"account\": \"%s\",\n  \"book\": \"%s\",\n  \"receipts\": [\n", identity.account, identity.book));
  }
  template<class File>
  bool receipt(File& file, const Identity& id, const char* sha256) {
    if (!same(id) || id.day <= previous_ || !sha256 || std::strlen(sha256) != 64) return false;
    for (const char* p = sha256; *p; ++p) if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f')) return false;
    const bool first = previous_ == 0;
    previous_ = id.day;
    return match(file, std::snprintf(text_, sizeof(text_),
      "%s    {\n      \"day\": %lu,\n      \"receipt\": \"weread-external-%s-%s-%lu.bin\",\n      \"sha256\": \"%s\"\n    }",
      first ? "" : ",\n", static_cast<unsigned long>(id.day), id.account, id.source,
      static_cast<unsigned long>(id.day), sha256));
  }
  template<class File>
  bool finish(File& file, const Identity& id) {
    closed_ = previous_ && id.day == previous_ && same(id) && match(file, std::snprintf(text_, sizeof(text_),
      "\n  ],\n  \"schema\": 1,\n  \"sender_enabled\": false,\n  \"source\": \"%s\",\n  \"state\": \"prepared\"\n}", id.source)) &&
      offset_ == file.fileSize64();
    return closed_;
  }
  bool deviceOwnedDay(const Identity& id) const {
    return closed_ && same(id) && id.day > previous_;
  }
 private:
  bool same(const Identity& id) const {
    return !std::strncmp(id.account, identity_.account, sizeof(id.account)) &&
           !std::strncmp(id.book, identity_.book, sizeof(id.book)) &&
           !std::strncmp(id.source, identity_.source, sizeof(id.source));
  }
  template<class File>
  bool match(File& file, int length) {
    if (length <= 0 || size_t(length) >= sizeof(text_)) return false;
    uint8_t part[96];
    for (size_t at = 0; at < size_t(length);) {
      const size_t n = size_t(length) - at < sizeof(part) ? size_t(length) - at : sizeof(part);
      if (file.read(part, n) != int(n) || std::memcmp(part, text_ + at, n)) return false;
      at += n; offset_ += n;
    }
    return true;
  }
  char text_[512] = {};
  uint64_t offset_ = 0;
  uint32_t previous_ = 0;
  bool closed_ = false;
  Identity identity_;
};
}  // namespace WeReadTime
