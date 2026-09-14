#pragma once

#include <cstdint>
#include <cstring>

// No networking or heap allocations. One ledger belongs to one immutable
// account/book/source/day. Import cumulative measured milliseconds, not deltas.
namespace WeReadTime {

struct Identity {
  char account[32] = {};
  char book[64] = {};
  char source[64] = {};
  uint32_t day = 0;
};

enum class Status : uint8_t { Pending, AwaitingVerification };

struct Snapshot {
  Identity identity;
  uint64_t bookDaySeconds = 0;
  uint64_t accountDaySeconds = 0;
  uint64_t sampledAt = 0;
  // Set only after the response has explicit book/day/account coverage. An
  // absent book in a top-ten ranking is NOT proof of zero reading time.
  bool complete = false;
};

class Ledger {
 public:
  static constexpr size_t kEncodedSize = 240;
  bool bind(const char* account, const char* book, const char* source, uint32_t day) {
    if (!validId(account, sizeof(identity_.account)) || !validId(book, sizeof(identity_.book)) ||
        !validId(source, sizeof(identity_.source)) || day == 0)
      return false;
    if (identity_.day != 0) {
      return identity_.day == day && std::strcmp(identity_.account, account) == 0 &&
             std::strcmp(identity_.book, book) == 0 && std::strcmp(identity_.source, source) == 0;
    }
    std::strcpy(identity_.account, account);
    std::strcpy(identity_.book, book);
    std::strcpy(identity_.source, source);
    identity_.day = day;
    return true;
  }

  bool collect(uint64_t measuredMs) {
    // A reset/import/correction must not turn an old cumulative counter into
    // another upload. Keep the previous high-water mark and require review.
    if (identity_.day == 0 || measuredMs < measuredMs_) return false;
    measuredMs_ = measuredMs;
    return true;
  }

  // Reserve before network I/O; callers MUST durably save and read back this
  // state before sending. There is deliberately no reset/retry transition.
  bool reserve(const Snapshot& baseline, uint64_t now, bool dateReportingSupported) {
    if (status_ != Status::Pending || pendingSeconds() < 60 || !dateReportingSupported || !matches(baseline) ||
        baseline.sampledAt == 0 || now < baseline.sampledAt || now - baseline.sampledAt > 120 ||
        baseline.bookDaySeconds > UINT64_MAX - 60 || baseline.accountDaySeconds > UINT64_MAX - 60)
      return false;
    baselineBook_ = baseline.bookDaySeconds;
    baselineAccount_ = baseline.accountDaySeconds;
    reservedAt_ = now;
    status_ = Status::AwaitingVerification;
    return true;
  }

  bool verify(const Snapshot& observed) {
    if (status_ != Status::AwaitingVerification || !matches(observed) || observed.sampledAt <= reservedAt_ ||
        observed.bookDaySeconds != baselineBook_ + 60 || observed.accountDaySeconds != baselineAccount_ + 60)
      return false;
    verifiedSeconds_ += 60;
    status_ = Status::Pending;
    baselineBook_ = baselineAccount_ = reservedAt_ = 0;
    return true;
  }

  uint64_t pendingSeconds() const { return measuredMs_ / 1000 - verifiedSeconds_; }
  uint64_t measuredMs() const { return measuredMs_; }
  uint64_t verifiedSeconds() const { return verifiedSeconds_; }
  Status status() const { return status_; }
  const Identity& identity() const { return identity_; }

  bool encode(uint8_t* bytes, size_t size) const {
    if (!bytes || size != kEncodedSize || identity_.day == 0) return false;
    std::memset(bytes, 0, size);
    std::memcpy(bytes, "WRTL", 4);
    bytes[4] = 1;
    bytes[5] = static_cast<uint8_t>(status_);
    std::memcpy(bytes + 8, identity_.account, 32);
    std::memcpy(bytes + 40, identity_.book, 64);
    std::memcpy(bytes + 104, identity_.source, 64);
    put(bytes + 168, identity_.day);
    put(bytes + 176, measuredMs_);
    put(bytes + 184, verifiedSeconds_);
    put(bytes + 192, baselineBook_);
    put(bytes + 200, baselineAccount_);
    put(bytes + 208, reservedAt_);
    put(bytes + 232, checksum(bytes, 232));
    return true;
  }

  bool decode(const uint8_t* bytes, size_t size) {
    if (!bytes || size != kEncodedSize || std::memcmp(bytes, "WRTL", 4) != 0 || bytes[4] != 1 || bytes[5] > 1 ||
        get(bytes + 232) != checksum(bytes, 232))
      return false;
    // Validate an isolated candidate: corrupt storage must not reset a live
    // ledger and accidentally make previously submitted minutes sendable.
    Ledger candidate;
    std::memcpy(candidate.identity_.account, bytes + 8, 32);
    std::memcpy(candidate.identity_.book, bytes + 40, 64);
    std::memcpy(candidate.identity_.source, bytes + 104, 64);
    const uint64_t day = get(bytes + 168);
    if (day == 0 || day > UINT32_MAX || !validId(candidate.identity_.account, 32) ||
        !validId(candidate.identity_.book, 64) || !validId(candidate.identity_.source, 64))
      return false;
    candidate.identity_.day = static_cast<uint32_t>(day);
    candidate.measuredMs_ = get(bytes + 176);
    candidate.verifiedSeconds_ = get(bytes + 184);
    candidate.baselineBook_ = get(bytes + 192);
    candidate.baselineAccount_ = get(bytes + 200);
    candidate.reservedAt_ = get(bytes + 208);
    candidate.status_ = static_cast<Status>(bytes[5]);
    if (candidate.verifiedSeconds_ > candidate.measuredMs_ / 1000) return false;
    if (candidate.status_ == Status::AwaitingVerification &&
        (candidate.pendingSeconds() < 60 || candidate.reservedAt_ == 0 || candidate.baselineBook_ > UINT64_MAX - 60 ||
         candidate.baselineAccount_ > UINT64_MAX - 60))
      return false;
    if (candidate.status_ == Status::Pending &&
        (candidate.reservedAt_ || candidate.baselineBook_ || candidate.baselineAccount_))
      return false;
    *this = candidate;
    return true;
  }

 private:
  bool matches(const Snapshot& snapshot) const {
    return identity_.day != 0 && snapshot.complete && snapshot.identity.day == identity_.day &&
           std::strncmp(snapshot.identity.account, identity_.account, 32) == 0 &&
           std::strncmp(snapshot.identity.book, identity_.book, 64) == 0 &&
           std::strncmp(snapshot.identity.source, identity_.source, 64) == 0;
  }

  static void put(uint8_t* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
  }
  static uint64_t get(const uint8_t* in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(in[i]) << (i * 8);
    return value;
  }
  static uint32_t checksum(const uint8_t* bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
      crc ^= bytes[i];
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
  }
  static bool validId(const char* text, size_t capacity) {
    if (!text || !*text) return false;
    for (size_t i = 0; i < capacity; ++i) {
      if (text[i] == '\0') return true;
      const char c = text[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
        return false;
    }
    return false;
  }

  Identity identity_;
  uint64_t measuredMs_ = 0;
  uint64_t verifiedSeconds_ = 0;
  uint64_t baselineBook_ = 0;
  uint64_t baselineAccount_ = 0;
  uint64_t reservedAt_ = 0;
  Status status_ = Status::Pending;
};

static_assert(sizeof(Ledger) <= 256, "Ledger copies must fit the embedded local-variable budget");

}  // namespace WeReadTime
