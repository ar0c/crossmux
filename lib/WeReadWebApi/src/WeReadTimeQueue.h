#pragma once

#include <optional>

#include "WeReadTimeTransaction.h"

namespace WeReadTime {
// Source reopens the earliest eligible source/day journal only between batches.
// It must prioritize unresolved journals over new sends and fail closed on audit errors.
class TimeQueueSource {
 public:
  enum class Result { Ready, Complete, Error };
  virtual ~TimeQueueSource() = default;
  virtual Result selectNext() = 0;
};

// One user-authorized run. The caller owns scheduling (foreground or worker).
// Fixed storage, no per-batch allocation or automatic retries of unknown writes.
class TimeQueue {
 public:
  enum class State { Idle, Selecting, Running, Complete, Paused, Uncertain, StorageError };
  TimeQueue(TimeQueueSource& source, PacedJournal& journal, SendCoordinator& coordinator, TimeTransport& transport,
            TimeTransaction::BatchMode mode = TimeTransaction::BatchMode::Paced30)
      : source_(source), journal_(journal), coordinator_(coordinator), transport_(transport), mode_(mode) {}
  bool begin() {
    if (state_ != State::Idle) return false;
    state_ = State::Selecting;
    return true;
  }
  void cancel() {
    if (state_ != State::Running && state_ != State::Selecting) return;
    if (transaction_) {
      transaction_->cancel();
      phase_ = transaction_->state();
    }
    state_ = journal_.ledger().state() == PacedLedger::State::Idle ? State::Paused : State::Uncertain;
    transaction_.reset();
  }
  State step() {
    switch (state_) {
      case State::Selecting: {
        transaction_.reset();
        switch (source_.selectNext()) {
          case TimeQueueSource::Result::Complete:
            state_ = State::Complete;
            return state_;
          case TimeQueueSource::Result::Error:
            state_ = State::StorageError;
            return state_;
          case TimeQueueSource::Result::Ready:
            break;
        }
        // Unknown/expired batches stay frozen. Never quarantine automatically
        // to skip past them; recovery is read-only and exact-credit only.
        transport_.reset();
        transaction_.emplace(journal_, coordinator_, transport_, mode_);
        const bool idle = journal_.ledger().state() == PacedLedger::State::Idle;
        const bool started = idle ? transaction_->begin() : transaction_->beginRecovery();
        phase_ = transaction_->state();
        issue_ = transaction_->issue();
        state_ = started ? State::Running : (idle ? State::StorageError : State::Uncertain);
        return state_;
      }
      case State::Running: {
        phase_ = transaction_->step();
        issue_ = transaction_->issue();
        using T = TimeTransaction::State;
        switch (phase_) {
          case T::Confirmed:
            confirmed_ += transaction_->confirmedSeconds();
            transaction_.reset();
            state_ = State::Selecting;
            break;
          case T::StorageError:
            state_ = State::StorageError;
            break;
          case T::Uncertain:
            state_ = State::Uncertain;
            break;
          case T::NotSent:
          case T::Cancelled:
            state_ = journal_.ledger().state() == PacedLedger::State::Idle ? State::Paused : State::Uncertain;
            break;
          case T::Idle:
          case T::Waiting:
          case T::Preparing:
          case T::Baseline:
          case T::Reserving:
          case T::Entering:
          case T::Sending:
          case T::ReadbackWait:
          case T::ReadingBack:
          case T::RetryWait:
            break;
        }
        return state_;
      }
      case State::Idle:
      case State::Complete:
      case State::Paused:
      case State::Uncertain:
      case State::StorageError:
        return state_;
    }
    return state_;
  }
  State state() const { return state_; }
  TimeTransaction::State phase() const { return phase_; }
  TimeTransaction::Issue issue() const { return issue_; }
  uint64_t confirmed() const { return confirmed_; }
  uint32_t waitingSeconds() const { return transaction_ ? transaction_->waitingSeconds() : 0; }
  uint8_t preparationRetries() const { return transaction_ ? transaction_->preparationRetries() : 0; }

 private:
  TimeQueueSource& source_;
  PacedJournal& journal_;
  SendCoordinator& coordinator_;
  TimeTransport& transport_;
  TimeTransaction::BatchMode mode_;
  std::optional<TimeTransaction> transaction_;
  State state_ = State::Idle;
  TimeTransaction::State phase_ = TimeTransaction::State::Idle;
  TimeTransaction::Issue issue_ = TimeTransaction::Issue::None;
  uint64_t confirmed_ = 0;
};
static_assert(sizeof(TimeQueue) < 384, "Time queue must stay bounded");
}  // namespace WeReadTime
