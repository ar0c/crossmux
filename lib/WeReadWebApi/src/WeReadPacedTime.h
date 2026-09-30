#pragma once

#include "WeReadExternalTime.h"
#include "WeReadTimeJournal.h"

namespace WeReadTime {

// One account-wide gate must be shared across books/days by the sender. A new
// process starts with a full 30-second wait, irrespective of wall-clock jumps.
class PacedGate {
 public:
  explicit PacedGate(uint32_t startedMs) : lastMs_(startedMs) {}
  bool ready(uint32_t nowMs, uint32_t waitMs = 30000) const {
    const uint32_t elapsed = nowMs - lastMs_;
    return elapsed >= waitMs && elapsed < 0x80000000U;
  }
  uint32_t remainingMs(uint32_t nowMs, uint32_t waitMs) const {
    const uint32_t elapsed = nowMs - lastMs_;
    return elapsed < 0x80000000U ? (elapsed >= waitMs ? 0 : waitMs - elapsed) : waitMs;
  }
  void response(uint32_t nowMs) { lastMs_ = nowMs; }

 private:
  uint32_t lastMs_;
};

// Distinct contract from WRTL v1: upload-day account aggregates, not book/day.
struct AccountSnapshot {
  char account[32] = {};
  uint32_t month = 0;
  uint32_t day = 0;
  uint64_t monthSeconds = 0;
  uint64_t daySeconds = 0;
  uint64_t sampledAt = 0;
  bool complete = false;
  bool monthComplete = false;  // Valid monthly response, even when today's bucket is absent.
};

class PacedLedger {
 public:
  enum class State : uint8_t { Idle, Reserved, Acknowledged, Uncertain };
  static constexpr size_t kSize = 256;
  static constexpr uint64_t kBatch = 30;

  bool initialize(const ExternalTime& external) {
    if (identity_.day) return false;
    Ledger validator;
    if (!validator.bind(external.identity.account, external.identity.book, external.identity.source,
                        external.identity.day) ||
        external.coveredSeconds > external.sourceMs / 1000 || external.confirmedSeconds > external.coveredSeconds ||
        external.unknownSeconds != external.coveredSeconds - external.confirmedSeconds)
      return false;
    identity_ = external.identity;
    measuredMs_ = external.sourceMs;
    external_ = external.coveredSeconds;
    return true;
  }
  bool matches(const ExternalTime& external) const {
    return identity_.day == external.identity.day && measuredMs_ >= external.sourceMs &&
           external_ == external.coveredSeconds && !std::strcmp(identity_.account, external.identity.account) &&
           !std::strcmp(identity_.book, external.identity.book) &&
           !std::strcmp(identity_.source, external.identity.source);
  }
  bool collect(uint64_t measured) {
    if (!identity_.day || measured < measuredMs_) return false;
    measuredMs_ = measured;
    return true;
  }
  bool reserve(const AccountSnapshot& baseline, uint64_t now, bool monotonicPaceReady, uint64_t seconds = kBatch) {
    if (state_ != State::Idle || !seconds || seconds > UINT32_MAX || remaining() < seconds || !monotonicPaceReady ||
        !validSnapshot(baseline, true) || now < baseline.sampledAt || now - baseline.sampledAt > 30 ||
        baseline.day != (now + 28800) / 86400 * 86400 - 28800 || identity_.day > (now + 28800) / 86400 ||
        (responseAt_ && (now < responseAt_ || now - responseAt_ < kBatch)) ||
        baseline.monthSeconds > UINT64_MAX - seconds || baseline.daySeconds > UINT64_MAX - seconds)
      return false;
    batch_ = static_cast<uint32_t>(seconds);
    month_ = baseline.month;
    day_ = baseline.day;
    baselineMonth_ = baseline.monthSeconds;
    baselineDay_ = baseline.daySeconds;
    missingDay_ = !baseline.complete;
    reservedAt_ = now;
    responseAt_ = 0;
    state_ = State::Reserved;
    return true;
  }
  bool acknowledge(bool accepted, uint64_t at) {
    if (state_ != State::Reserved || at < reservedAt_ || at > UINT32_MAX) return false;
    responseAt_ = at;
    state_ = accepted ? State::Acknowledged : State::Uncertain;
    return true;
  }
  bool verify(const AccountSnapshot& observed) {
    // Statistical confirmation under the single-active-client policy. A missing
    // baseline bucket must APPEAR as exactly this first batch, not merely exist.
    const uint64_t expectedDay = missingDay_ ? batch_ : baselineDay_ + batch_;
    if (state_ != State::Acknowledged || !validSnapshot(observed) || observed.month != month_ || observed.day != day_ ||
        observed.sampledAt <= responseAt_ || observed.sampledAt - responseAt_ > 120 ||
        observed.monthSeconds != baselineMonth_ + batch_ || observed.daySeconds != expectedDay)
      return false;
    verified_ += batch_;
    batch_ = 0;
    state_ = State::Idle;
    missingDay_ = false;
    reservedAt_ = baselineMonth_ = baselineDay_ = 0;
    month_ = day_ = 0;
    return true;
  }
  uint64_t remaining() const { return measuredMs_ / 1000 - external_ - verified_ - quarantined_ - batch_; }
  uint32_t quarantinedSeconds() const { return quarantined_; }
  // Explicit new foreground run only. Preserve the old frames and permanently
  // exclude the entire unresolved range, without claiming any cloud credit.
  bool quarantine(uint64_t now) {
    const uint64_t last = responseAt_ ? responseAt_ : reservedAt_;
    if (state_ == State::Idle || !batch_ || now > UINT32_MAX || now < last || now - last <= 120 ||
        batch_ > UINT32_MAX - quarantined_)
      return false;
    quarantined_ += batch_;
    batch_ = 0;
    state_ = State::Idle;
    missingDay_ = false;
    reservedAt_ = baselineMonth_ = baselineDay_ = 0;
    month_ = day_ = 0;
    responseAt_ = now;
    return true;
  }
  uint32_t batchSeconds() const { return batch_; }
  uint64_t measuredMs() const { return measuredMs_; }
  uint64_t verified() const { return verified_; }
  uint64_t responseAt() const { return responseAt_; }
  const Identity& identity() const { return identity_; }
  State state() const { return state_; }

  bool encode(uint8_t* bytes) const {
    if (!identity_.day) return false;
    std::memset(bytes, 0, kSize);
    std::memcpy(bytes, "WRP2", 4);
    bytes[4] = 4;
    bytes[5] = static_cast<uint8_t>(state_);
    bytes[6] = missingDay_ ? 1 : 0;
    std::memcpy(bytes + 8, identity_.account, 32);
    std::memcpy(bytes + 40, identity_.book, 64);
    std::memcpy(bytes + 104, identity_.source, 64);
    put(bytes + 168, identity_.day);
    put(bytes + 176, measuredMs_);
    put(bytes + 184, external_);
    put(bytes + 192, verified_);
    // v3 reuses the previously zero high word; frame size remains 256 bytes.
    put(bytes + 200, uint64_t(month_) | (uint64_t(batch_) << 32));
    put(bytes + 208, uint64_t(day_) | (uint64_t(quarantined_) << 32));
    put(bytes + 216, baselineMonth_);
    put(bytes + 224, baselineDay_);
    put(bytes + 232, reservedAt_);
    put(bytes + 240, responseAt_);
    put(bytes + 248, ExternalTime::checksum(bytes, 248));
    return true;
  }
  bool decode(const uint8_t* bytes) {
    if (std::memcmp(bytes, "WRP2", 4) || bytes[4] < 1 || bytes[4] > 4 || bytes[5] > 3 || bytes[6] > 1 || bytes[7] ||
        (bytes[4] == 1 && bytes[6]) || ExternalTime::number(bytes + 248) != ExternalTime::checksum(bytes, 248))
      return false;
    PacedLedger candidate;
    ExternalTime identity;
    std::memcpy(identity.identity.account, bytes + 8, 32);
    std::memcpy(identity.identity.book, bytes + 40, 64);
    std::memcpy(identity.identity.source, bytes + 104, 64);
    const uint64_t day = ExternalTime::number(bytes + 168);
    if (!day || day > UINT32_MAX) return false;
    identity.identity.day = static_cast<uint32_t>(day);
    identity.sourceMs = ExternalTime::number(bytes + 176);
    identity.coveredSeconds = identity.unknownSeconds = ExternalTime::number(bytes + 184);
    if (!candidate.initialize(identity)) return false;
    candidate.verified_ = ExternalTime::number(bytes + 192);
    const uint64_t packedMonth = ExternalTime::number(bytes + 200);
    const uint64_t month = bytes[4] >= 3 ? uint32_t(packedMonth) : packedMonth;
    const uint64_t packedDay = ExternalTime::number(bytes + 208);
    const uint64_t uploadDay = bytes[4] >= 4 ? uint32_t(packedDay) : packedDay;
    candidate.quarantined_ = bytes[4] >= 4 ? uint32_t(packedDay >> 32) : 0;
    if (month > UINT32_MAX || uploadDay > UINT32_MAX) return false;
    candidate.month_ = static_cast<uint32_t>(month);
    candidate.day_ = static_cast<uint32_t>(uploadDay);
    candidate.baselineMonth_ = ExternalTime::number(bytes + 216);
    candidate.baselineDay_ = ExternalTime::number(bytes + 224);
    candidate.reservedAt_ = ExternalTime::number(bytes + 232);
    candidate.responseAt_ = ExternalTime::number(bytes + 240);
    candidate.state_ = static_cast<State>(bytes[5]);
    candidate.batch_ = bytes[4] >= 3 ? uint32_t(packedMonth >> 32) : (candidate.state_ == State::Idle ? 0 : kBatch);
    candidate.missingDay_ = bytes[6] != 0;
    if (candidate.verified_ > candidate.measuredMs_ / 1000 - candidate.external_ ||
        (bytes[4] < 3 && candidate.verified_ % kBatch) || candidate.responseAt_ > UINT32_MAX)
      return false;
    if (candidate.quarantined_ > candidate.measuredMs_ / 1000 - candidate.external_ - candidate.verified_) return false;
    if (candidate.state_ == State::Idle) {
      if (candidate.batch_ || candidate.missingDay_ || candidate.reservedAt_ || candidate.month_ || candidate.day_ ||
          candidate.baselineMonth_ || candidate.baselineDay_)
        return false;
    } else {
      if (!candidate.batch_ ||
          candidate.measuredMs_ / 1000 - candidate.external_ - candidate.verified_ - candidate.quarantined_ <
              candidate.batch_ ||
          !candidate.validSnapshot(candidate.baseline(), true) ||
          candidate.baselineMonth_ > UINT64_MAX - candidate.batch_ ||
          candidate.baselineDay_ > UINT64_MAX - candidate.batch_ ||
          (candidate.state_ == State::Reserved ? candidate.responseAt_ != 0
                                               : candidate.responseAt_ < candidate.reservedAt_))
        return false;
    }
    *this = candidate;
    return true;
  }
  // Replay only legal transitions; a valid CRC is not permission to reset an
  // uncertain frame, reduce counters, or swap the source/remote identity.
  bool follows(const PacedLedger& previous) const {
    PacedLedger expected = previous;
    if (!expected.collect(measuredMs_)) return false;
    if (state_ != previous.state_) {
      if (previous.state_ == State::Idle && state_ == State::Reserved) {
        if (!expected.reserve(baseline(), reservedAt_, true, batch_)) return false;
      } else if (previous.state_ == State::Reserved && (state_ == State::Acknowledged || state_ == State::Uncertain)) {
        if (!expected.acknowledge(state_ == State::Acknowledged, responseAt_)) return false;
      } else if (state_ == State::Idle && quarantined_ > previous.quarantined_) {
        if (!expected.quarantine(responseAt_)) return false;
      } else if (previous.state_ == State::Acknowledged && state_ == State::Idle) {
        auto observed = previous.baseline();
        observed.sampledAt = previous.responseAt_ + 1;
        observed.complete = true;
        observed.monthSeconds += previous.batch_;
        observed.daySeconds += previous.batch_;
        if (!expected.verify(observed)) return false;
      } else
        return false;
    }
    return equal(expected);
  }

 private:
  bool equal(const PacedLedger& other) const {
    return !std::strcmp(identity_.account, other.identity_.account) &&
           !std::strcmp(identity_.book, other.identity_.book) &&
           !std::strcmp(identity_.source, other.identity_.source) && identity_.day == other.identity_.day &&
           measuredMs_ == other.measuredMs_ && external_ == other.external_ && verified_ == other.verified_ &&
           state_ == other.state_ && batch_ == other.batch_ && quarantined_ == other.quarantined_ &&
           missingDay_ == other.missingDay_ && month_ == other.month_ && day_ == other.day_ &&
           baselineMonth_ == other.baselineMonth_ && baselineDay_ == other.baselineDay_ &&
           reservedAt_ == other.reservedAt_ && responseAt_ == other.responseAt_;
  }
  bool validSnapshot(const AccountSnapshot& snapshot, bool allowMissing = false) const {
    return (snapshot.complete || (allowMissing && snapshot.monthComplete && snapshot.daySeconds == 0)) &&
           !std::strncmp(snapshot.account, identity_.account, sizeof(snapshot.account)) &&
           snapshot.sampledAt >= 1704067200 && snapshot.sampledAt <= UINT32_MAX && snapshot.month != 0 &&
           snapshot.month <= snapshot.day && snapshot.day - snapshot.month <= 30 * 86400 &&
           (uint64_t(snapshot.day) + 28800) % 86400 == 0 && (uint64_t(snapshot.month) + 28800) % 86400 == 0 &&
           snapshot.daySeconds <= snapshot.monthSeconds;
  }
  AccountSnapshot baseline() const {
    AccountSnapshot result;
    std::memcpy(result.account, identity_.account, sizeof(result.account));
    result.month = month_;
    result.day = day_;
    result.monthSeconds = baselineMonth_;
    result.daySeconds = baselineDay_;
    result.sampledAt = reservedAt_;
    result.complete = !missingDay_;
    result.monthComplete = true;
    return result;
  }
  static void put(uint8_t* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
  }
  Identity identity_;
  uint64_t measuredMs_ = 0, external_ = 0, verified_ = 0;
  uint64_t baselineMonth_ = 0, baselineDay_ = 0, reservedAt_ = 0, responseAt_ = 0;
  uint32_t month_ = 0, day_ = 0, batch_ = 0, quarantined_ = 0;
  State state_ = State::Idle;
  bool missingDay_ = false;
};
static_assert(sizeof(PacedLedger) <= 256, "Paced checkpoint must remain bounded");

class PacedJournal {
 public:
  explicit PacedJournal(ByteLog& log) : log_(log) {}
  bool open(const ExternalTime& receipt) {
    ready_ = permit_ = issued_ = false;
    PacedLedger candidate;
    if (!candidate.initialize(receipt)) return false;
    uint64_t size = 0;
    const auto state = log_.size(size);
    if (state == ByteLog::ReadState::Missing) {
      length_ = 0;
      return commit(candidate);
    }
    if (state != ByteLog::ReadState::Ready || !size || size % PacedLedger::kSize) return false;
    for (uint64_t at = 0; at < size; at += PacedLedger::kSize) {
      if (!log_.read(at, buffer_, sizeof(buffer_)) || !candidate.decode(buffer_) || !candidate.matches(receipt))
        return false;
      if (at && !candidate.follows(ledger_)) return false;
      ledger_ = candidate;
    }
    length_ = size;
    ready_ = true;
    return true;
  }
  bool collect(uint64_t ms) {
    if (!ready_) return false;
    PacedLedger candidate = ledger_;
    if (!candidate.collect(ms)) return false;
    return ms == ledger_.measuredMs() || commit(candidate);
  }
  bool reserve(const AccountSnapshot& baseline, uint64_t now, bool paceReady, uint64_t seconds = PacedLedger::kBatch) {
    if (!ready_ || !log_.canReserve(PacedLedger::kSize * 3)) return false;
    PacedLedger candidate = ledger_;
    if (!candidate.reserve(baseline, now, paceReady, seconds) || !commit(candidate)) return false;
    permit_ = true;
    return true;
  }
  bool hasReserveSpace() { return ready_ && log_.canReserve(PacedLedger::kSize * 3); }
  bool takeSendPermit() {
    if (!ready_ || !permit_ || ledger_.state() != PacedLedger::State::Reserved) return false;
    permit_ = false;
    issued_ = true;
    return true;
  }
  bool acknowledge(bool accepted, uint64_t at) {
    if (!ready_ || !issued_) return false;
    issued_ = false;
    PacedLedger candidate = ledger_;
    return candidate.acknowledge(accepted, at) && commit(candidate);
  }
  bool verify(const AccountSnapshot& observed) {
    if (!ready_) return false;
    PacedLedger candidate = ledger_;
    return candidate.verify(observed) && commit(candidate);
  }
  bool quarantine(uint64_t now) {
    if (!ready_ || !log_.canReserve(PacedLedger::kSize)) return false;
    PacedLedger candidate = ledger_;
    if (!candidate.quarantine(now) || !commit(candidate)) return false;
    issued_ = false;
    return true;
  }
  const PacedLedger& ledger() const { return ledger_; }
  // Only an already fully audited, activity-owned journal may use this path.
  // The foreground sync activity excludes another writer. Reboot, day/source
  // change and activity exit always require full replay again.
  bool continueVerified(const ExternalTime& receipt, uint64_t measured) {
    uint64_t size = 0;
    if (!ready_ || ledger_.state() != PacedLedger::State::Idle || ledger_.remaining() < 1 ||
        !ledger_.matches(receipt) || ledger_.measuredMs() != measured || length_ < sizeof(buffer_) ||
        log_.size(size) != ByteLog::ReadState::Ready || size != length_ || !ledger_.encode(expected_) ||
        !log_.read(length_ - sizeof(buffer_), buffer_, sizeof(buffer_)) ||
        std::memcmp(buffer_, expected_, sizeof(buffer_))) {
      ready_ = permit_ = false;
      return false;
    }
    return true;
  }

 private:
  bool commit(const PacedLedger& candidate) {
    ready_ = permit_ = false;
    uint64_t size = 0;
    const auto state = log_.size(size);
    if (state == ByteLog::ReadState::Error || size != length_ || !candidate.encode(expected_) ||
        !log_.appendAndSync(expected_, sizeof(expected_)) || log_.size(size) != ByteLog::ReadState::Ready ||
        size != length_ + sizeof(expected_) || !log_.read(length_, buffer_, sizeof(buffer_)) ||
        std::memcmp(buffer_, expected_, sizeof(buffer_)))
      return false;
    ledger_ = candidate;
    length_ = size;
    ready_ = true;
    return true;
  }
  ByteLog& log_;
  PacedLedger ledger_;
  uint64_t length_ = 0;
  bool ready_ = false, permit_ = false, issued_ = false;
  uint8_t buffer_[PacedLedger::kSize] = {}, expected_[PacedLedger::kSize] = {};
};
static_assert(sizeof(PacedJournal) <= 1024, "Paced journal must remain bounded");
}  // namespace WeReadTime
