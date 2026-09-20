#pragma once
// Host-only adapter: deliberately reuses the firmware's codec and transitions.
// No network, file writes, reset or send permission in this layer.
#include "WeReadPacedTime.h"
#include <vector>
namespace WeReadTime {
class HostBridge {
 public:
  bool open(const std::vector<uint8_t>& bytes) {
    ready_ = false;
    if (bytes.empty() || bytes.size() > 4*1024*1024 || bytes.size()%PacedLedger::kSize) return false;
    PacedLedger previous;
    for (size_t at=0; at<bytes.size(); at+=PacedLedger::kSize) {
      PacedLedger candidate;
      if (!candidate.decode(bytes.data()+at) || (at && !candidate.follows(previous))) return false;
      previous = candidate;
    }
    ledger_ = previous; bytes_ = bytes; ready_ = true;
    return true;
  }
  bool reserve(const AccountSnapshot& baseline, uint64_t now, uint64_t seconds) {
    auto next = ledger_;
    return ready_ && next.reserve(baseline,now,true,seconds) && append(next);
  }
  // Explicit host recovery only. Original frames and occupied quota survive;
  // the sender must persist/re-read this append under its exclusive lease.
  bool isolate(uint64_t now) {
    auto next = ledger_;
    return ready_ && next.quarantine(now) && append(next);
  }
  bool settle(bool accepted, uint64_t responded, const AccountSnapshot& observed) {
    auto next = ledger_;
    if (!ready_ || !next.acknowledge(accepted,responded) || !append(next)) return false;
    // Failure/partial credit preserves the entire unresolved amount.
    return !next.verify(observed) || append(next);
  }
  // Pure audit of a saved snapshot; never advances even the in-memory journal.
  bool canConfirm(const AccountSnapshot& observed) const {
    auto candidate = ledger_;
    return ready_ && candidate.verify(observed);
  }
  const PacedLedger& ledger() const { return ledger_; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
 private:
  bool append(const PacedLedger& next) {
    uint8_t frame[PacedLedger::kSize];
    if (bytes_.size()+sizeof(frame)>4*1024*1024 || !next.follows(ledger_) || !next.encode(frame)) return false;
    bytes_.insert(bytes_.end(),frame,frame+sizeof(frame)); ledger_=next; return true;
  }
  bool ready_ = false;
  PacedLedger ledger_;
  std::vector<uint8_t> bytes_;
};
}
