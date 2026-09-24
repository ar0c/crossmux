#pragma once
#include <cstdio>

#include "WeReadExternalTime.h"
#include "WeReadTimeJournal.h"

namespace WeReadTime {
// Version 2 is appended in-place after a completely validated v1 prefix.
// No rewrite/reset: old IDs survive and v1 readers reject v2 before sending.
class ServiceJournal {
 public:
  static constexpr size_t kSize = 256, kMaxPending = 24;
  static constexpr uint64_t kMaxBytes = 4 * 1024 * 1024;
  enum class State : uint8_t { Empty, Reserved, Accepted, Confirmed };
  explicit ServiceJournal(ByteLog& log) : log_(log) {}
  bool open(const Identity& id, uint64_t consumed, uint64_t measured) {
    ready_ = false;
    length_ = 0;
    id_ = id;
    selected_ = 0;
    base_ = end_ = consumed;
    measured_ = measured;
    confirmed_ = lastCheck_ = 0;
    device_[0] = 0;
    version2_ = false;
    for (auto& task : tasks_) task = {};
    Ledger validator;
    if (!validator.bind(id.account, id.book, id.source, id.day) || consumed > measured || measured > 86400)
      return false;
    const auto status = log_.size(length_);
    if (status == ByteLog::ReadState::Missing) {
      ready_ = true;
      return true;
    }
    if (status != ByteLog::ReadState::Ready || !length_ || length_ % kSize || length_ > kMaxBytes) return false;
    for (uint64_t offset = 0; offset < length_; offset += kSize) {
      if (!log_.read(offset, scratch_, kSize) || !decodeNext()) return false;
    }
    ready_ = true;
    return true;
  }
  State state() const { return tasks_[selected_].state; }
  uint64_t owned() const { return end_ - base_; }
  uint64_t confirmed() const { return confirmed_; }
  uint64_t pending() const { return owned() - confirmed_; }
  uint64_t unacknowledged() const {
    for (const auto& task : tasks_)
      if (task.state == State::Reserved) return task.end - task.start;
    return 0;
  }
  uint64_t start() const { return tasks_[selected_].start; }
  uint64_t end() const { return tasks_[selected_].end; }
  uint64_t credit() const { return tasks_[selected_].credit; }
  // Oldest cached observation for outstanding accepted work, not a live claim.
  uint64_t checkedAt() const {
    uint64_t at = lastCheck_;
    for (const auto& task : tasks_)
      if (task.state == State::Accepted && task.checked < at) at = task.checked;
    return at;
  }
  const char* device() const { return device_; }
  bool matchesDevice(const char* d) const { return !owned() || !std::strcmp(device_, d); }
  bool selectReserved() {
    for (size_t i = 0; i < kMaxPending; ++i)
      if (tasks_[i].state == State::Reserved) {
        selected_ = i;
        return true;
      }
    return false;
  }
  bool selectAccepted(uint64_t from = 0) {
    size_t best = kMaxPending;
    for (size_t i = 0; i < kMaxPending; ++i)
      if (tasks_[i].state == State::Accepted && tasks_[i].start >= from &&
          (best == kMaxPending || tasks_[i].start < tasks_[best].start))
        best = i;
    if (best == kMaxPending) return false;
    selected_ = best;
    return true;
  }
  bool capacity() const { return freeSlot() < kMaxPending && length_ + 3 * kSize <= kMaxBytes; }
  bool reserve(const char* device) {
    if (!ready_ || !validDevice(device) || !matchesDevice(device) || unacknowledged() || end_ >= measured_ ||
        !capacity() || !log_.canReserve(3 * kSize))
      return false;
    std::strcpy(device_, device);
    selected_ = freeSlot();
    tasks_[selected_] = {end_, measured_, 0, 0, State::Reserved};
    end_ = measured_;
    return commit();
  }
  bool accept(bool full, uint64_t credit, uint64_t checked = 0) {
    auto& task = tasks_[selected_];
    if (!ready_ || task.state == State::Empty || credit < task.credit || credit > task.end - task.start ||
        (full && credit != task.end - task.start) || (!full && credit == task.end - task.start))
      return false;
    if (task.state == State::Confirmed) return full;
    const auto next = full ? State::Confirmed : State::Accepted;
    if (checked < task.checked) checked = task.checked;
    // A read-only poll is not an accounting change. Keep the older persisted
    // timestamp instead of growing the SD journal on every unchanged receipt.
    if (next == task.state && credit == task.credit) return true;
    confirmed_ += credit - task.credit;
    task.credit = credit;
    task.checked = checked;
    task.state = next;
    if (checked > lastCheck_) lastCheck_ = checked;
    return commit();
  }
  bool accept(bool full) { return accept(full, full ? end() - start() : credit()); }
  bool jobId(char* out, size_t size) const {
    const int n = std::snprintf(out, size, "s-%s-%lu-%llu-%llu", id_.source, static_cast<unsigned long>(id_.day),
                                static_cast<unsigned long long>(start()), static_cast<unsigned long long>(end()));
    return state() != State::Empty && n > 0 && size_t(n) < size;
  }

 private:
  struct Task {
    uint64_t start = 0, end = 0, credit = 0, checked = 0;
    State state = State::Empty;
  };
  static bool validDevice(const char* device) {
    if (!device || !*device || std::strlen(device) >= 32) return false;
    for (const char* p = device; *p; ++p)
      if (!((*p >= 'a' && *p <= 'f') || (*p >= '0' && *p <= '9'))) return false;
    return true;
  }
  size_t freeSlot() const {
    for (size_t i = 0; i < kMaxPending; ++i)
      if (tasks_[i].state == State::Empty || tasks_[i].state == State::Confirmed) return i;
    return kMaxPending;
  }
  static void put(uint8_t* p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
  }
  bool decodeNext() {
    const auto* b = scratch_;
    if (std::memcmp(b, "WRS1", 4) || (b[4] != 1 && b[4] != 2) || (version2_ && b[4] != 2) || b[5] < 1 || b[5] > 3 ||
        b[6] || b[7] || ExternalTime::number(b + 248) != ExternalTime::checksum(b, 248) ||
        std::memcmp(b + 8, id_.account, 32) || std::memcmp(b + 40, id_.book, 64) ||
        std::memcmp(b + 104, id_.source, 64) || ExternalTime::number(b + 168) != id_.day ||
        ExternalTime::number(b + 208) != base_ || !std::memchr(b + 176, 0, 32) ||
        !validDevice(reinterpret_cast<const char*>(b + 176)))
      return false;
    const uint64_t start = ExternalTime::number(b + 216), end = ExternalTime::number(b + 224);
    const auto next = static_cast<State>(b[5]);
    if (start < base_ || end <= start || end > measured_) return false;
    if (!owned())
      std::memcpy(device_, b + 176, 32);
    else if (std::memcmp(device_, b + 176, 32))
      return false;
    const bool v2 = b[4] == 2;
    const uint64_t credit = v2 ? ExternalTime::number(b + 232) : (next == State::Confirmed ? end - start : 0);
    const uint64_t checked = v2 ? ExternalTime::number(b + 240) : 0;
    if (!v2 && (ExternalTime::number(b + 232) || ExternalTime::number(b + 240))) return false;
    if (credit > end - start || (next == State::Confirmed ? credit != end - start : credit == end - start))
      return false;
    if (next == State::Reserved) {
      if (start != end_ || unacknowledged() || freeSlot() == kMaxPending || credit || checked ||
          (!v2 && owned() && state() != State::Confirmed))
        return false;
      selected_ = freeSlot();
      tasks_[selected_] = {start, end, 0, 0, State::Reserved};
      end_ = end;
    } else {
      size_t i = 0;
      for (; i < kMaxPending; ++i)
        if (tasks_[i].start == start && tasks_[i].end == end &&
            (tasks_[i].state == State::Reserved || tasks_[i].state == State::Accepted))
          break;
      if (i == kMaxPending) return false;
      auto& task = tasks_[i];
      if (credit < task.credit || checked < task.checked || (!v2 && unsigned(next) <= unsigned(task.state)) ||
          (v2 && next == task.state && credit == task.credit && checked == task.checked))
        return false;
      confirmed_ += credit - task.credit;
      task = {start, end, credit, checked, next};
      selected_ = i;
    }
    if (checked > lastCheck_) lastCheck_ = checked;
    version2_ = v2;
    return true;
  }
  bool commit() {
    ready_ = false;
    if (length_ + kSize > kMaxBytes) return false;
    std::memset(scratch_, 0, kSize);
    std::memcpy(scratch_, "WRS1", 4);
    scratch_[4] = 2;
    scratch_[5] = uint8_t(state());
    std::memcpy(scratch_ + 8, id_.account, 32);
    std::memcpy(scratch_ + 40, id_.book, 64);
    std::memcpy(scratch_ + 104, id_.source, 64);
    put(scratch_ + 168, id_.day);
    std::memcpy(scratch_ + 176, device_, 32);
    put(scratch_ + 208, base_);
    put(scratch_ + 216, start());
    put(scratch_ + 224, end());
    put(scratch_ + 232, credit());
    put(scratch_ + 240, tasks_[selected_].checked);
    put(scratch_ + 248, ExternalTime::checksum(scratch_, 248));
    if (!log_.appendAndSync(scratch_, kSize)) return false;
    uint64_t size = 0;
    uint8_t check[kSize];
    if (log_.size(size) != ByteLog::ReadState::Ready || size != length_ + kSize || !log_.read(length_, check, kSize) ||
        std::memcmp(check, scratch_, kSize))
      return false;
    length_ = size;
    version2_ = true;
    ready_ = true;
    return true;
  }
  ByteLog& log_;
  Identity id_{};
  char device_[32] = {};
  uint64_t base_ = 0, end_ = 0, measured_ = 0, length_ = 0, confirmed_ = 0, lastCheck_ = 0;
  Task tasks_[kMaxPending]{};
  size_t selected_ = 0;
  bool ready_ = false, version2_ = false;
  uint8_t scratch_[kSize]{};
};
static_assert(sizeof(ServiceJournal) < 2048, "Bounded service journal workspace");
}  // namespace WeReadTime
