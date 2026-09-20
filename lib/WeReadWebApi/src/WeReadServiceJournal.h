#pragma once
#include "WeReadExternalTime.h"
#include "WeReadTimeJournal.h"
#include <cstdio>

namespace WeReadTime {
// Separate ownership ledger. Never converts durable acceptance into cloud credit.
// Every frame is checked; a torn tail blocks both delegation and direct upload.
class ServiceJournal {
 public:
  static constexpr size_t kSize = 256;
  enum class State : uint8_t { Empty, Reserved, Accepted, Confirmed };
  explicit ServiceJournal(ByteLog& log) : log_(log) {}
  bool open(const Identity& id, uint64_t consumed, uint64_t measured) {
    ready_ = false; length_ = 0; state_ = State::Empty; id_ = id;
    base_ = start_ = end_ = consumed; measured_ = measured; device_[0] = 0;
    Ledger validator;
    if (!validator.bind(id.account, id.book, id.source, id.day) || consumed > measured || measured > 86400) return false;
    const auto status = log_.size(length_);
    if (status == ByteLog::ReadState::Missing) { ready_ = true; return true; }
    if (status != ByteLog::ReadState::Ready || !length_ || length_ % kSize || length_ > 4 * 1024 * 1024) return false;
    for (uint64_t offset = 0; offset < length_; offset += kSize) {
      if (!log_.read(offset, scratch_, kSize) || !decodeNext()) return false;
    }
    ready_ = true; return true;
  }
  State state() const { return state_; }
  uint64_t owned() const { return end_ - base_; }
  uint64_t confirmed() const { return (state_ == State::Confirmed ? end_ : start_) - base_; }
  uint64_t pending() const { return owned() - confirmed(); }
  uint64_t start() const { return start_; }
  uint64_t end() const { return end_; }
  const char* device() const { return device_; }
  bool matchesDevice(const char* d) const { return state_ == State::Empty || !std::strcmp(device_, d); }
  bool reserve(const char* device) {
    if (!ready_ || !device || !*device || std::strlen(device) >= sizeof(device_) ||
        !matchesDevice(device) || (state_ != State::Empty && state_ != State::Confirmed) ||
        end_ >= measured_ || !log_.canReserve(3 * kSize)) return false;
    for (const char* p = device; *p; ++p) if (!((*p >= 'a' && *p <= 'f') || (*p >= '0' && *p <= '9'))) return false;
    std::strcpy(device_, device); start_ = end_; end_ = measured_;
    state_ = State::Reserved; return commit();
  }
  bool accept(bool confirmed) {
    if (!ready_ || state_ == State::Empty) return false;
    if (state_ == State::Confirmed) return confirmed;
    const auto next = confirmed ? State::Confirmed : State::Accepted;
    if (next == state_) return true;
    state_ = next; return commit();
  }
  bool jobId(char* out, size_t size) const {
    const int n = std::snprintf(out, size, "s-%s-%lu-%llu-%llu", id_.source,
        static_cast<unsigned long>(id_.day), static_cast<unsigned long long>(start_), static_cast<unsigned long long>(end_));
    return n > 0 && size_t(n) < size;
  }
 private:
  static void put(uint8_t* p, uint64_t v) { for (unsigned i=0; i<8; ++i) p[i]=uint8_t(v>>(8*i)); }
  bool decodeNext() {
    const auto* b = scratch_;
    if (std::memcmp(b,"WRS1",4) || b[4] != 1 || b[5]<1 || b[5]>3 || b[6] || b[7] ||
        ExternalTime::number(b+248) != ExternalTime::checksum(b,248) ||
        std::memcmp(b+8,id_.account,32) || std::memcmp(b+40,id_.book,64) ||
        std::memcmp(b+104,id_.source,64) || ExternalTime::number(b+168)!=id_.day ||
        ExternalTime::number(b+208)!=base_ || !std::memchr(b+176,0,32) || !b[176]) return false;
    const uint64_t start = ExternalTime::number(b+216), end = ExternalTime::number(b+224);
    const auto next = static_cast<State>(b[5]);
    if (start < base_ || end <= start || end > measured_) return false;
    if (state_ == State::Empty) {
      if (next != State::Reserved || start != base_) return false;
      std::memcpy(device_, b+176, 32);
    } else {
      if (std::memcmp(device_,b+176,32)) return false;
      if (state_ == State::Confirmed) {
        if (next != State::Reserved || start != end_) return false;
      } else if (start != start_ || end != end_ || unsigned(next) <= unsigned(state_)) return false;
    }
    start_=start; end_=end; state_=next; return true;
  }
  bool commit() {
    ready_=false;
    std::memset(scratch_,0,kSize); std::memcpy(scratch_,"WRS1",4);
    scratch_[4]=1; scratch_[5]=uint8_t(state_);
    std::memcpy(scratch_+8,id_.account,32); std::memcpy(scratch_+40,id_.book,64);
    std::memcpy(scratch_+104,id_.source,64); put(scratch_+168,id_.day);
    std::memcpy(scratch_+176,device_,32); put(scratch_+208,base_);
    put(scratch_+216,start_); put(scratch_+224,end_);
    put(scratch_+248,ExternalTime::checksum(scratch_,248));
    if (!log_.appendAndSync(scratch_,kSize)) return false;
    uint64_t size=0; uint8_t check[kSize];
    if (log_.size(size)!=ByteLog::ReadState::Ready || size!=length_+kSize ||
        !log_.read(length_,check,kSize) || std::memcmp(check,scratch_,kSize)) return false;
    length_=size; ready_=true; return true;
  }
  ByteLog& log_;
  Identity id_{};
  char device_[32] = {};
  uint64_t base_=0,start_=0,end_=0,measured_=0,length_=0;
  State state_=State::Empty;
  bool ready_=false;
  uint8_t scratch_[kSize]{};
};
}
