#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "WeReadTimeQueue.h"

using namespace WeReadTime;
using Q = TimeQueue::State;
using T = TimeTransaction::State;
struct Log : ByteLog {
  std::vector<uint8_t> bytes;
  bool fail = false;
  ReadState size(uint64_t& n) override {
    n = bytes.size();
    return n ? ReadState::Ready : ReadState::Missing;
  }
  bool read(uint64_t at, uint8_t* out, size_t n) override {
    if (at + n > bytes.size()) return false;
    std::memcpy(out, bytes.data() + at, n);
    return true;
  }
  bool appendAndSync(const uint8_t* p, size_t n) override {
    if (fail) return false;
    bytes.insert(bytes.end(), p, p + n);
    return true;
  }
};
struct Source : TimeQueueSource, ByteLog {
  std::array<Log, 3> logs;
  PacedJournal journal{*this};
  std::array<uint64_t, 3> measured{{907675, 6325686, 284271}};
  unsigned active = 0;
  bool error = false;
  bool full = false;
  ExternalTime receipt(unsigned i) {
    ExternalTime r;
    std::strcpy(r.identity.account, "a");
    std::strcpy(r.identity.book, "b");
    std::strcpy(r.identity.source, "s");
    r.identity.day = 20708 + i;
    r.sourceMs = measured[i];
    if (i == 1) {
      r.coveredSeconds = 3000;
      r.confirmedSeconds = 2880;
      r.unknownSeconds = 120;
    }
    return r;
  }
  ReadState size(uint64_t& n) override { return logs[active].size(n); }
  bool read(uint64_t at, uint8_t* p, size_t n) override { return logs[active].read(at, p, n); }
  bool appendAndSync(const uint8_t* p, size_t n) override { return logs[active].appendAndSync(p, n); }
  Result selectNext() override {
    if (error) return Result::Error;
    int candidate = -1;
    for (unsigned i = 0; i < logs.size(); ++i) {
      active = i;
      if (!journal.open(receipt(i)) || !journal.collect(measured[i])) return Result::Error;
      if (journal.ledger().state() != PacedLedger::State::Idle) return Result::Ready;
      if (candidate < 0 && journal.ledger().remaining() >= (full ? 1U : 30U)) candidate = int(i);
    }
    if (candidate < 0) return Result::Complete;
    active = unsigned(candidate);
    return journal.open(receipt(active)) ? Result::Ready : Result::Error;
  }
};
struct Transport : TimeTransport {
  Source& source;
  explicit Transport(Source& s) : source(s) {}
  uint64_t now = 1789453168, month = 16234, day = 2947;
  uint32_t tick = 30000, lastSend = 0;
  unsigned sends = 0, entries = 0, resets = 0;
  unsigned failedPrepares = 0;
  bool failRead = false, unknown = false, rejectPrepare = false;
  unsigned credit = 30;
  void reset() override { ++resets; }
  bool retryablePreparation() const override { return true; }
  Read prepare(const Identity& id) override {
    assert(!strcmp(id.account, "a") && !strcmp(id.book, "b"));
    if (failedPrepares) {
      --failedPrepares;
      return Read::Failed;
    }
    return rejectPrepare ? Read::Failed : Read::Ready;
  }
  Read snapshot(AccountSnapshot& out) override {
    if (failRead) return Read::Failed;
    out = {};
    strcpy(out.account, "a");
    out.month = 1788192000;
    out.day = 1789401600;
    out.monthSeconds = month;
    out.daySeconds = day;
    out.sampledAt = now;
    out.complete = true;
    return Read::Ready;
  }
  Write enter() override {
    ++entries;
    return Write::Accepted;
  }
  Write report(uint32_t seconds) override {
    assert(source.full ? (seconds >= 1 && seconds <= 60) : seconds == 30);
    assert(source.journal.ledger().state() == PacedLedger::State::Reserved);
    assert(source.logs[source.active].bytes.size() >= PacedLedger::kSize * 2);
    if (source.full) assert(tick >= 60000);
    if (lastSend) assert(uint32_t(tick - lastSend) >= (source.full ? 70000U : 30000U));
    const auto increment = source.full && credit == 30 ? seconds : credit;
    lastSend = tick;
    ++sends;
    month += increment;
    day += increment;
    return unknown ? Write::Unknown : Write::Accepted;
  }
  uint64_t epochSeconds() const override { return now; }
  uint32_t monotonicMs() const override { return tick; }
  void advance() {
    ++now;
    tick += 1000;
  }
};
struct Fixture {
  Source source;
  Transport transport{source};
  SendCoordinator coordinator{0};
  TimeQueue queue{source, source.journal, coordinator, transport};
  explicit Fixture(TimeTransaction::BatchMode mode = TimeTransaction::BatchMode::Paced30)
      : queue(source, source.journal, coordinator, transport, mode) {
    source.full = mode == TimeTransaction::BatchMode::Bounded60;
    assert(queue.begin());
  }
  void until(T phase) {
    for (unsigned i = 0; i < 500 && queue.phase() != phase; ++i) {
      queue.step();
      transport.advance();
    }
    assert(queue.phase() == phase && queue.state() == Q::Running);
  }
  Q finish(TimeQueue& q, unsigned steps = 20000) {
    for (unsigned i = 0; i < steps && (q.state() == Q::Selecting || q.state() == Q::Running); ++i) {
      q.step();
      transport.advance();
    }
    return q.state();
  }
};
int main() {
  for (bool cancel : {false, true}) {
    Fixture f(TimeTransaction::BatchMode::Bounded60);
    f.transport.failedPrepares = 2;
    f.until(T::RetryWait);
    assert(f.queue.preparationRetries() == 1 && f.queue.waitingSeconds() == 1);
    const auto bytes = f.source.logs[f.source.active].bytes;
    assert(f.transport.sends == 0 && f.transport.entries == 0);
    if (cancel) {
      f.queue.cancel();
      assert(f.finish(f.queue) == Q::Paused);
      assert(f.source.logs[f.source.active].bytes == bytes && f.transport.sends == 0);
    } else {
      assert(f.finish(f.queue) == Q::Complete);
      assert(f.queue.confirmed() == 4516 && f.transport.sends == 77);
    }
  }
  for (unsigned fault = 0; fault < 5; ++fault) {
    Fixture f(TimeTransaction::BatchMode::Bounded60);
    f.until(T::ReadbackWait);
    assert(f.transport.sends == 1 && f.source.journal.ledger().batchSeconds() == 60);
    switch (fault) {
      case 0:
        f.queue.cancel();
        break;
      case 1:
        f.transport.failRead = true;
        break;
      case 2:
        f.transport.month -= 60;
        f.transport.day -= 60;
        break;
      case 3:
        f.transport.month -= 30;
        f.transport.day -= 30;
        break;
      case 4:
        f.source.logs[f.source.active].fail = true;
        break;
    }
    assert(f.finish(f.queue) == (fault == 4 ? Q::StorageError : Q::Uncertain));
    f.transport.now += 180;
    f.transport.tick += 180000;
    f.source.logs[f.source.active].fail = false;
    f.transport.failRead = false;
    const auto bytes = f.source.logs[0].bytes;
    TimeQueue reboot(f.source, f.source.journal, f.coordinator, f.transport, TimeTransaction::BatchMode::Bounded60);
    assert(reboot.begin() && f.finish(reboot) == Q::Uncertain && f.transport.sends == 1);
    assert(f.source.logs[0].bytes == bytes);
  }
  {
    Fixture f;
    f.queue.cancel();
    f.source.full = true;
    TimeQueue bulk(f.source, f.source.journal, f.coordinator, f.transport, TimeTransaction::BatchMode::Bounded60);
    assert(bulk.begin() && !bulk.begin());
    assert(f.finish(bulk) == Q::Complete);
    assert(bulk.confirmed() == 4516 && f.transport.sends == 77);
    const auto sends = f.transport.sends;
    TimeQueue reopen(f.source, f.source.journal, f.coordinator, f.transport, TimeTransaction::BatchMode::Bounded60);
    assert(reopen.begin() && f.finish(reopen) == Q::Complete && f.transport.sends == sends);
  }
  {
    Fixture f;
    f.transport.unknown = true;
    assert(f.finish(f.queue) == Q::Uncertain && f.transport.sends == 1);
    f.transport.now += 121;
    f.transport.tick += 121000;
    f.transport.unknown = false;
    f.source.full = true;
    const auto prefix = f.source.logs[0].bytes;
    TimeQueue bulk(f.source, f.source.journal, f.coordinator, f.transport, TimeTransaction::BatchMode::Bounded60);
    assert(bulk.begin() && f.finish(bulk) == Q::Uncertain);
    assert(bulk.confirmed() == 0 && f.transport.sends == 1);
    assert(prefix == f.source.logs[0].bytes);
    f.source.active = 0;
    assert(f.source.journal.open(f.source.receipt(0)));
    assert(f.source.journal.ledger().quarantinedSeconds() == 0);
    assert(f.source.journal.ledger().batchSeconds() == 30);
    TimeQueue repeat(f.source, f.source.journal, f.coordinator, f.transport, TimeTransaction::BatchMode::Bounded60);
    assert(repeat.begin() && f.finish(repeat) == Q::Uncertain && f.transport.sends == 1);
  }
  {
    Fixture f;
    f.source.full = true;
    f.transport.credit = 7;
    f.queue.cancel();
    TimeQueue bulk(f.source, f.source.journal, f.coordinator, f.transport, TimeTransaction::BatchMode::Bounded60);
    assert(bulk.begin() && f.finish(bulk) == Q::Uncertain);
    assert(f.transport.sends == 1 && bulk.confirmed() == 0);
    for (int i = 0; i < 500; ++i) {
      bulk.step();
      f.transport.advance();
    }
    assert(f.transport.sends == 1 && f.source.journal.ledger().quarantinedSeconds() == 0);
  }
  {
    Fixture f;
    assert(f.finish(f.queue) == Q::Complete);
    // Real measured history minus the already covered 3000s; daily tails retained.
    assert(f.queue.confirmed() == 4470 && f.transport.sends == 149);
    unsigned sends = f.transport.sends;
    for (int i = 0; i < 20; ++i) {
      f.queue.step();
      f.queue.cancel();
    }
    assert(f.transport.sends == sends);
    TimeQueue reopened(f.source, f.source.journal, f.coordinator, f.transport);
    assert(reopened.begin() && f.finish(reopened) == Q::Complete && f.transport.sends == sends);
  }
  {
    Fixture f;
    f.until(T::ReadbackWait);
    f.queue.cancel();
    assert(f.queue.state() == Q::Uncertain && f.transport.sends == 1);
    TimeQueue recovery(f.source, f.source.journal, f.coordinator, f.transport);
    assert(recovery.begin());
    for (int i = 0; i < 100 && recovery.confirmed() == 0; ++i) {
      recovery.step();
      f.transport.advance();
    }
    assert(recovery.confirmed() == 30 && f.transport.sends == 1 && f.transport.entries == 1);
    recovery.cancel();
  }
  {
    Fixture f;
    f.until(T::Sending);
    f.queue.cancel();  // Reserved, never submitted.
    TimeQueue reboot(f.source, f.source.journal, f.coordinator, f.transport);
    assert(reboot.begin() && f.finish(reboot) == Q::Uncertain && f.transport.sends == 0);
  }
  {
    Fixture f;
    f.transport.unknown = true;
    assert(f.finish(f.queue) == Q::Uncertain && f.transport.sends == 1);
    TimeQueue reboot(f.source, f.source.journal, f.coordinator, f.transport);
    assert(reboot.begin() && f.finish(reboot) == Q::Uncertain && f.transport.sends == 1);
  }
  {
    Fixture f;
    f.until(T::ReadbackWait);
    f.queue.cancel();
    f.transport.now += 121;
    f.transport.tick += 121000;
    TimeQueue recovery(f.source, f.source.journal, f.coordinator, f.transport);
    assert(recovery.begin() && f.finish(recovery) == Q::Uncertain && f.transport.sends == 1);
  }
  {
    Fixture f;
    f.transport.credit = 31;
    assert(f.finish(f.queue) == Q::Uncertain && f.queue.confirmed() == 0 && f.transport.sends == 1);
  }
  {
    Fixture f;
    f.transport.rejectPrepare = true;
    assert(f.finish(f.queue) == Q::Paused && f.transport.sends == 0);
    f.transport.rejectPrepare = false;
    TimeQueue resume(f.source, f.source.journal, f.coordinator, f.transport);
    assert(resume.begin() && f.finish(resume) == Q::Complete && f.transport.sends == 149);
  }
  {
    Fixture f;
    f.source.error = true;
    assert(f.finish(f.queue) == Q::StorageError && f.transport.sends == 0);
  }
  {
    Fixture f;
    f.until(T::Reserving);
    f.source.logs[f.source.active].fail = true;
    assert(f.finish(f.queue) == Q::StorageError && f.transport.sends == 0);
  }
  {
    Fixture f;
    f.until(T::Preparing);
    f.queue.cancel();
    assert(f.queue.state() == Q::Paused && f.transport.sends == 0);
    TimeQueue resume(f.source, f.source.journal, f.coordinator, f.transport);
    assert(resume.begin() && f.finish(resume) == Q::Complete && f.transport.sends == 149);
  }
  {
    Fixture f;
    f.until(T::ReadbackWait);
    f.transport.failRead = true;
    assert(f.finish(f.queue) == Q::Uncertain && f.transport.sends == 1);
  }
  std::cout << "Queue: bounded60 77 batches + legacy30 149 batches; tails, cooldown, repeat, cancellation, "
               "partial/zero credit, storage failure and frozen recovery PASS\n";
}
