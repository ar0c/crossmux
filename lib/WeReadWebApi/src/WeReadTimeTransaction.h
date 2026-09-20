#pragma once

#include "WeReadPacedTime.h"

namespace WeReadTime {

// One instance shared by all time transactions in a firmware process. A
// cancelled/unknown write keeps its reservation; this lock only owns execution.
class SendCoordinator {
 public:
  explicit SendCoordinator(uint32_t startedMs) : gate_(startedMs) {}
  SendCoordinator(const SendCoordinator&) = delete;
  SendCoordinator& operator=(const SendCoordinator&) = delete;
  bool acquire(const void* owner, uint32_t nowMs, uint32_t waitMs = 30000) {
    if (!owner || owner_ || !gate_.ready(nowMs, waitMs)) return false;
    owner_ = owner;
    responseRecorded_ = false;
    return true;
  }
  uint32_t remainingSeconds(uint32_t nowMs, uint32_t waitMs) const {
    return (gate_.remainingMs(nowMs, waitMs) + 999) / 1000;
  }
  // Anchor pacing at the timed HTTP response, not after cloud readback. Keep
  // ownership until verification finishes, so overlapping writes remain blocked.
  void response(const void* owner, uint32_t nowMs) {
    if (!owner || owner_ != owner || responseRecorded_) return;
    gate_.response(nowMs);
    responseRecorded_ = true;
  }
  void release(const void* owner, uint32_t nowMs) {
    if (!owner || owner_ != owner) return;
    // Pre-write failure/cancellation retains the conservative fallback. After
    // a timed response, verification and journal I/O already count as waiting.
    if (!responseRecorded_) gate_.response(nowMs);
    owner_ = nullptr;
  }
 private:
  PacedGate gate_;
  const void* owner_ = nullptr;
  bool responseRecorded_ = false;
};

// The adapter must use verified TLS, a fresh cloud position (never local
// approximate progress), an explicit live signing token and zero write retries.
// Pending is allowed only for read/prepare phases; report is one invocation.
class TimeTransport {
 public:
  enum class Read { Pending, Ready, Failed };
  enum class Write { Accepted, Unknown };
  virtual ~TimeTransport() = default;
  virtual Read prepare(const Identity& identity) = 0;
  virtual Read snapshot(AccountSnapshot& result) = 0;
  // Reset only transport scratch for a NEW transaction, never accounting.
  virtual void reset() {}
  // Explicit opt-in for transient failures of read-only preparation. Never
  // called to retry entry/report, recovery, authentication or protocol errors.
  virtual bool retryablePreparation() const { return false; }
  // Non-timed entry, still performed only AFTER the durable reservation.
  virtual Write enter() = 0;
  virtual Write report(uint32_t seconds) = 0;
  virtual uint64_t epochSeconds() const = 0;
  virtual uint32_t monotonicMs() const = 0;
};

// Single batch, foreground, no loops/sleeps or automatic second reservation.
// Each step invokes at most one transport operation. The caller renders state
// before stepping; cancellation is checked between synchronous requests.
class TimeTransaction {
 public:
  enum class BatchMode { Paced30, Bounded60 };
  enum class Issue { None, UnknownWrite, Expired, ClockInvalid, ReadbackFailed, Mismatch, LowSpace,
                     BaselineIncomplete };
  enum class State {
    Idle, Waiting, Preparing, Baseline, Reserving, Entering, Sending,
    ReadbackWait, ReadingBack, Confirmed, NotSent, Uncertain, StorageError, Cancelled, RetryWait
  };
  TimeTransaction(PacedJournal& journal, SendCoordinator& coordinator, TimeTransport& transport,
                  BatchMode mode = BatchMode::Paced30)
      : journal_(journal), coordinator_(coordinator), transport_(transport), mode_(mode) {}
  ~TimeTransaction() { release(); }
  TimeTransaction(const TimeTransaction&) = delete;
  TimeTransaction& operator=(const TimeTransaction&) = delete;

  bool begin() {
    if (state_ != State::Idle) return false;
    if (journal_.ledger().state() != PacedLedger::State::Idle ||
        journal_.ledger().remaining() < (mode_ == BatchMode::Paced30 ? 30U : 1U)) {
      state_ = State::NotSent;
      return false;
    }
    state_ = State::Waiting;
    return true;
  }
  State state() const { return state_; }
  Issue issue() const { return issue_; }
  uint32_t waitingSeconds() const {
    if (state_ == State::RetryWait) {
      const uint32_t elapsed = transport_.monotonicMs() - retryStartedMs_;
      return elapsed >= retryDelayMs_ ? 0 : (retryDelayMs_ - elapsed + 999) / 1000;
    }
    return state_ == State::Waiting ? coordinator_.remainingSeconds(transport_.monotonicMs(), waitMs()) : 0;
  }
  uint8_t preparationRetries() const { return retries_; }
  uint32_t confirmedSeconds() const { return state_ == State::Confirmed ? batchSeconds_ : 0; }
  bool beginRecovery() {
    if (state_ != State::Idle) return false;
    if (journal_.ledger().state() != PacedLedger::State::Acknowledged) {
      issue_ = Issue::UnknownWrite;
      state_ = State::Uncertain;
      return false;
    }
    recovery_ = reserved_ = true;
    batchSeconds_ = journal_.ledger().batchSeconds();
    responseAt_ = journal_.ledger().responseAt();
    responseMs_ = transport_.monotonicMs();
    readbackMs_ = responseMs_;
    if (!readbackFresh(transport_.epochSeconds(), responseMs_)) {
      state_ = State::Uncertain;
      return false;
    }
    state_ = State::Waiting;
    return true;
  }
  void cancel() {
    if (terminal()) return;
    // After reservation, even cancelling before the POST does not return the
    // batch to availability. Never undo a persisted reservation to retry.
    finish(reserved_ ? State::Uncertain : State::Cancelled);
  }
  State step() {
    const uint64_t now = transport_.epochSeconds();
    const uint32_t tick = transport_.monotonicMs();
    switch (state_) {
      case State::Waiting:
        if (coordinator_.acquire(this, tick, waitMs())) {
          acquired_ = true;
          startedMs_ = tick;
          state_ = State::Preparing;
        }
        break;
      case State::Preparing:
        if (expired(tick)) return finish(State::NotSent);
        switch (transport_.prepare(journal_.ledger().identity())) {
          case TimeTransport::Read::Pending: break;
          case TimeTransport::Read::Ready:
            state_ = recovery_ ? State::ReadingBack : State::Baseline;
            break;
          case TimeTransport::Read::Failed:
            if (!reserved_ && !recovery_ && retries_ < 3 && transport_.retryablePreparation() &&
                !expired(transport_.monotonicMs())) {
              constexpr uint32_t delays[] = {2000, 5000, 10000};
              retryDelayMs_ = delays[retries_++];
              retryStartedMs_ = transport_.monotonicMs();
              state_ = State::RetryWait;
              break;
            }
            return finish(State::NotSent);
        }
        break;
      case State::RetryWait:
        if (expired(tick)) return finish(State::NotSent);
        if (uint32_t(tick - retryStartedMs_) >= retryDelayMs_) {
          // Discard partial signing/session scratch; retain the single owner,
          // the original 120s deadline and the untouched durable journal.
          transport_.reset();
          snapshot_ = {};
          state_ = State::Preparing;
        }
        break;
      case State::Baseline:
        if (expired(tick)) return finish(State::NotSent);
        switch (transport_.snapshot(snapshot_)) {
          case TimeTransport::Read::Pending: break;
          case TimeTransport::Read::Ready: state_ = State::Reserving; break;
          case TimeTransport::Read::Failed: return finish(State::NotSent);
        }
        break;
      case State::Reserving: {
        if (expired(tick)) return finish(State::NotSent);
        if (!snapshot_.complete && !snapshot_.monthComplete) {
          issue_ = Issue::BaselineIncomplete; return finish(State::NotSent);
        }
        if (!journal_.hasReserveSpace()) { issue_ = Issue::LowSpace; return finish(State::NotSent); }
        auto candidate = journal_.ledger();
        const uint64_t seconds = batchSize(candidate.remaining());
        if (!candidate.reserve(snapshot_, now, true, seconds)) return finish(State::NotSent);
        if (!journal_.reserve(snapshot_, now, true, seconds)) return finish(State::StorageError);
        batchSeconds_ = candidate.batchSeconds();
        reserved_ = true;
        reservedAt_ = now;
        reservedMs_ = tick;
        state_ = State::Entering;
        break;
      }
      case State::Entering:
        if (!fresh(now, tick)) return finish(State::Uncertain);
        if (transport_.enter() != TimeTransport::Write::Accepted) return finish(State::Uncertain);
        state_ = State::Sending;
        break;
      case State::Sending: {
        if (!fresh(now, tick) || !journal_.takeSendPermit()) return finish(State::Uncertain);
        // Leave Sending BEFORE invoking the write. No result, exception, later
        // step, cancellation or network error can cause a second invocation.
        state_ = State::Uncertain;
        const bool accepted = transport_.report(batchSeconds_) == TimeTransport::Write::Accepted;
        const uint64_t responded = transport_.epochSeconds();
        responseMs_ = transport_.monotonicMs();
        readbackMs_ = responseMs_;
        // Bounded60 preserves the measured host schedule: read back first,
        // then wait a full 60 seconds before preparing the next request.
        if (mode_ == BatchMode::Paced30) coordinator_.response(this, responseMs_);
        if (!journal_.acknowledge(accepted, responded)) return finish(State::StorageError);
        if (!accepted) { issue_ = Issue::UnknownWrite; return finish(State::Uncertain); }
        responseAt_ = responded;
        state_ = State::ReadbackWait;
        break;
      }
      case State::ReadbackWait:
        if (!readbackFresh(now, tick)) return finish(State::Uncertain);
        if (uint32_t(tick - readbackMs_) >= readbackDelayMs()) {
          snapshot_ = {};
          state_ = State::ReadingBack;
        }
        break;
      case State::ReadingBack:
        if (!readbackFresh(now, tick)) return finish(State::Uncertain);
        switch (transport_.snapshot(snapshot_)) {
          case TimeTransport::Read::Pending: break;
          case TimeTransport::Read::Failed:
            issue_ = Issue::ReadbackFailed;
            ++readbacks_;
            if (!readbackFresh(transport_.epochSeconds(), transport_.monotonicMs()) || readbacks_ >= 3)
              return finish(State::Uncertain);
            readbackMs_ = transport_.monotonicMs();
            state_ = State::ReadbackWait;
            break;
          case TimeTransport::Read::Ready: {
            ++readbacks_;
            if (!readbackFresh(transport_.epochSeconds(), transport_.monotonicMs())) return finish(State::Uncertain);
            // Distinguish an unproven increment from failed confirmation I/O.
            auto candidate = journal_.ledger();
            if (snapshot_.sampledAt <= transport_.epochSeconds() && candidate.verify(snapshot_)) {
              issue_ = Issue::None;
              return finish(journal_.verify(snapshot_) ? State::Confirmed : State::StorageError);
            }
            issue_ = Issue::Mismatch;
            if (readbacks_ >= 3) return finish(State::Uncertain);
            readbackMs_ = transport_.monotonicMs();
            state_ = State::ReadbackWait;
            break;
          }
        }
        break;
      case State::Idle:
      case State::Confirmed:
      case State::NotSent:
      case State::Uncertain:
      case State::StorageError:
      case State::Cancelled:
        break;
    }
    return state_;
  }
 private:
  uint32_t waitMs() const {
    switch (mode_) {
      case BatchMode::Paced30: return 30000;
      case BatchMode::Bounded60: return 60000;
    }
    return 60000;
  }
  uint32_t readbackDelayMs() const {
    switch (mode_) {
      case BatchMode::Paced30: return 5000;
      case BatchMode::Bounded60: return 10000;
    }
    return 10000;
  }
  uint64_t batchSize(uint64_t remaining) const {
    switch (mode_) {
      case BatchMode::Paced30: return 30;
      case BatchMode::Bounded60: return remaining < 60 ? remaining : 60;
    }
    return 0;
  }
  bool terminal() const {
    switch (state_) {
      case State::Confirmed: case State::NotSent: case State::Uncertain:
      case State::StorageError: case State::Cancelled: return true;
      case State::Idle: case State::Waiting: case State::Preparing: case State::Baseline:
      case State::Reserving: case State::Entering: case State::Sending:
      case State::ReadbackWait: case State::ReadingBack: case State::RetryWait: return false;
    }
    return true;
  }
  bool expired(uint32_t tick) const { return uint32_t(tick - startedMs_) > 120000U; }
  bool fresh(uint64_t now, uint32_t tick) const {
    return now >= reservedAt_ && now - reservedAt_ <= 30 && uint32_t(tick - reservedMs_) <= 30000U &&
           (now + 28800) / 86400 == (reservedAt_ + 28800) / 86400;
  }
  bool readbackFresh(uint64_t now, uint32_t tick) {
    if (now < responseAt_) { issue_ = Issue::ClockInvalid; return false; }
    if (now - responseAt_ > 120 || uint32_t(tick - responseMs_) > 120000U) {
      issue_ = Issue::Expired;
      return false;
    }
    return true;
  }
  State finish(State result) { state_ = result; release(); return state_; }
  void release() {
    if (!acquired_) return;
    coordinator_.release(this, transport_.monotonicMs());
    acquired_ = false;
  }
  PacedJournal& journal_;
  SendCoordinator& coordinator_;
  TimeTransport& transport_;
  BatchMode mode_;
  AccountSnapshot snapshot_;
  State state_ = State::Idle;
  Issue issue_ = Issue::None;
  uint64_t reservedAt_ = 0, responseAt_ = 0;
  uint32_t startedMs_ = 0, reservedMs_ = 0, responseMs_ = 0, readbackMs_ = 0;
  uint32_t batchSeconds_ = 0;
  uint8_t readbacks_ = 0;
  uint8_t retries_ = 0;
  uint32_t retryStartedMs_ = 0, retryDelayMs_ = 0;
  bool acquired_ = false, reserved_ = false, recovery_ = false;
};
static_assert(sizeof(TimeTransaction) <= 256, "Time transaction workspace must remain bounded");
}  // namespace WeReadTime
