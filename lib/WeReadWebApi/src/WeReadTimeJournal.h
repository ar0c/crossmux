#pragma once

#include "WeReadTimeLedger.h"

namespace WeReadTime {

// System boundary: implementations must distinguish a missing file from an
// unreadable/truncated existing file. No credentials are stored here.
class ByteLog {
 public:
  enum class ReadState { Missing, Ready, Error };
  virtual ~ByteLog() = default;
  virtual ReadState size(uint64_t& length) = 0;
  virtual bool read(uint64_t offset, uint8_t* out, size_t count) = 0;
  virtual bool appendAndSync(const uint8_t* data, size_t count) = 0;
  // Pre-send budget only. Acknowledgement/verification must still be allowed
  // to use reserved space after a network write has already happened.
  virtual bool canReserve(size_t) { return true; }
};

// Append-only checkpoints: a torn tail blocks reporting instead of silently
// falling back to a pre-send state. Deliberately no compaction/reset API.
// One activity owns the journal. No concurrent writers or copied SD journals.
class Journal {
 public:
  explicit Journal(ByteLog& log) : log_(log) {}

  bool open(const char* account, const char* book, const char* source, uint32_t day) {
    ready_ = false;
    Ledger candidate;
    if (!candidate.bind(account, book, source, day)) return false;
    uint64_t length = 0;
    const auto state = log_.size(length);
    if (state == ByteLog::ReadState::Missing) {
      length_ = 0;
      ready_ = true;
      return commit(candidate);
    }
    if (state != ByteLog::ReadState::Ready || length == 0 || length % Ledger::kEncodedSize != 0) return false;
    // Read one frame at a time: memory is constant regardless of history size.
    for (uint64_t offset = 0; offset < length; offset += Ledger::kEncodedSize) {
      if (!log_.read(offset, scratch_, sizeof(scratch_)) || !candidate.decode(scratch_, sizeof(scratch_)) ||
          !candidate.bind(account, book, source, day))
        return false;
    }
    ledger_ = candidate;
    length_ = length;
    ready_ = true;
    return true;
  }

  bool collect(uint64_t measuredMs) {
    if (!ready_) return false;
    Ledger candidate = ledger_;
    if (!candidate.collect(measuredMs)) return false;
    if (candidate.measuredMs() == ledger_.measuredMs()) return true;
    return commit(candidate);
  }

  bool reserve(const Snapshot& baseline, uint64_t now, bool dateReportingSupported) {
    if (!ready_) return false;
    Ledger candidate = ledger_;
    return candidate.reserve(baseline, now, dateReportingSupported) && commit(candidate);
  }

  bool verify(const Snapshot& observed) {
    if (!ready_) return false;
    Ledger candidate = ledger_;
    return candidate.verify(observed) && commit(candidate);
  }

  bool ready() const { return ready_; }
  const Ledger& ledger() const { return ledger_; }

 private:
  bool commit(const Ledger& candidate) {
    // Fail closed after *any* uncertain storage result. The caller must not
    // send until a reservation returns true after a persisted readback.
    ready_ = false;
    uint64_t actual = 0;
    const auto state = log_.size(actual);
    if (state == ByteLog::ReadState::Error || actual != length_ ||
        (state == ByteLog::ReadState::Missing && length_ != 0) || !candidate.encode(scratch_, sizeof(scratch_)))
      return false;
    if (!log_.appendAndSync(scratch_, sizeof(scratch_))) return false;
    if (log_.size(actual) != ByteLog::ReadState::Ready || actual != length_ + sizeof(scratch_) ||
        !log_.read(length_, scratch_, sizeof(scratch_)))
      return false;
    Ledger persisted;
    if (!persisted.decode(scratch_, sizeof(scratch_))) return false;
    // Compare canonical serialization, including all scope and attempt fields.
    // Keep observed bytes in a member buffer, not on the embedded task stack.
    std::memcpy(expected_, scratch_, sizeof(scratch_));
    if (!candidate.encode(scratch_, sizeof(scratch_)) || std::memcmp(scratch_, expected_, sizeof(scratch_)) != 0)
      return false;
    length_ = actual;
    ledger_ = persisted;
    ready_ = true;
    return true;
  }

  ByteLog& log_;
  Ledger ledger_;
  uint64_t length_ = 0;
  bool ready_ = false;
  // Fixed activity-owned buffers avoid heap churn and >256-byte local frames.
  uint8_t scratch_[Ledger::kEncodedSize] = {};
  uint8_t expected_[Ledger::kEncodedSize] = {};
};

static_assert(sizeof(Journal) <= 1024, "Journal workspace must remain bounded");

}  // namespace WeReadTime
