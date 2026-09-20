#include "WeReadTimeTransaction.h"
#include "WeReadTimeAck.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace WeReadTime;
using State = TimeTransaction::State;
struct Log : ByteLog {
  std::vector<uint8_t> bytes;
  bool fail = false;
  bool space = true;
  unsigned readCount = 0;
  bool canReserve(size_t) override { return space; }
  ReadState size(uint64_t& n) override { n = bytes.size(); return n ? ReadState::Ready : ReadState::Missing; }
  bool read(uint64_t at, uint8_t* out, size_t n) override {
    ++readCount;
    if (at + n > bytes.size()) return false;
    std::memcpy(out, bytes.data() + at, n); return true;
  }
  bool appendAndSync(const uint8_t* p, size_t n) override {
    if (fail) return false;
    bytes.insert(bytes.end(), p, p + n); return true;
  }
};
struct Transport : TimeTransport {
  uint64_t now = 1789453168;
  uint32_t tick = 30000;
  int prepares = 0, reads = 0, entries = 0, sends = 0;
  bool rejectPrepare = false, rejectEntry = false, unknownSend = false, staleRead = false;
  bool missingDay = false, pendingRead = false;
  unsigned failedReads = 0;
  unsigned failedPrepares = 0;
  bool retryable = false;
  unsigned resets = 0;
  bool retryablePreparation() const override { return retryable; }
  void reset() override { ++resets; }
  uint64_t credit = 30;
  uint32_t sentSeconds = 0;
  Log* log = nullptr;
  bool failAfterSend = false;
  Read prepare(const Identity& id) override {
    ++prepares; assert(!std::strcmp(id.account, "a") && !std::strcmp(id.book, "b"));
    if (failedPrepares) { --failedPrepares; return Read::Failed; }
    return rejectPrepare ? Read::Failed : Read::Ready;
  }
  Read snapshot(AccountSnapshot& result) override {
    ++reads;
    if (failedReads) { --failedReads; return Read::Failed; }
    if (pendingRead) return Read::Pending;
    result = {}; std::strcpy(result.account, "a");
    result.month = 1788192000; result.day = 1789401600;
    result.monthSeconds = 16234 + (sends ? credit : 0);
    result.daySeconds = 2947 + (sends ? credit : 0);
    result.sampledAt = now - (staleRead ? 200 : 0);
    result.complete = !missingDay;
    return Read::Ready;
  }
  Write enter() override { ++entries; return rejectEntry ? Write::Unknown : Write::Accepted; }
  Write report(uint32_t seconds) override {
    sentSeconds = seconds;
    ++sends;
    // Initial + reservation frame must already exist at the network boundary.
    assert(log && log->bytes.size() >= 2 * PacedLedger::kSize);
    if (failAfterSend) log->fail = true;
    return unknownSend ? Write::Unknown : Write::Accepted;
  }
  uint64_t epochSeconds() const override { return now; }
  uint32_t monotonicMs() const override { return tick; }
  void advance(uint32_t seconds) { now += seconds; tick += seconds * 1000; }
};
struct Fixture {
  Log log;
  ExternalTime receipt;
  PacedJournal journal{log};
  SendCoordinator coordinator{0};
  Transport transport;
  TimeTransaction transaction{journal, coordinator, transport};
  Fixture() {
    std::strcpy(receipt.identity.account, "a"); std::strcpy(receipt.identity.book, "b");
    std::strcpy(receipt.identity.source, "s"); receipt.identity.day = 20709;
    receipt.sourceMs = 600000; transport.log = &log;
    assert(journal.open(receipt));
    assert(transaction.begin());
  }
  void toReserve() {
    assert(transaction.step() == State::Preparing);
    assert(transaction.step() == State::Baseline);
    assert(transaction.step() == State::Reserving);
  }
  void toSend() {
    toReserve(); assert(transaction.step() == State::Entering);
    assert(transaction.step() == State::Sending);
  }
  void stableTerminal(State state) {
    int writes = transport.sends;
    for (int i = 0; i < 10; ++i) { assert(transaction.step() == state); transaction.cancel(); }
    assert(transport.sends == writes);
    assert(!transaction.begin());
  }
};
int main() {
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    Fixture f; f.transport.rejectPrepare = true; f.transport.retryable = true;
    const auto original = f.log.bytes;
    assert(f.transaction.step() == State::Preparing);
    assert(f.transaction.step() == State::RetryWait);
    if (scenario == 0) {
      for (unsigned i = 0; i < 40; ++i) { f.transport.advance(1); f.transaction.step(); }
      assert(f.transaction.state() == State::NotSent && f.transport.prepares == 4);
    } else if (scenario == 1) {
      f.transaction.cancel(); assert(f.transaction.state() == State::Cancelled);
    } else if (scenario == 2) {
      f.transport.advance(121); assert(f.transaction.step() == State::NotSent);
      assert(f.transport.prepares == 1);
    } else {
      f.transport.advance(2); assert(f.transaction.step() == State::Preparing);
      f.transport.retryable = false; assert(f.transaction.step() == State::NotSent);
      assert(f.transport.prepares == 2);
    }
    assert(f.log.bytes == original && f.transport.sends == 0 && f.transport.entries == 0);
    PacedJournal reopened(f.log); assert(reopened.open(f.receipt));
    assert(reopened.ledger().remaining() == 600 && reopened.ledger().state() == PacedLedger::State::Idle);
  }
  {
    Fixture f; f.transport.failedPrepares = 3; f.transport.retryable = true;
    const auto before = f.log.bytes;
    assert(f.transaction.step() == State::Preparing);
    for (unsigned delay : {2U, 5U, 10U}) {
      assert(f.transaction.step() == State::RetryWait);
      assert(f.transaction.waitingSeconds() == delay);
      const auto calls = f.transport.prepares;
      for (unsigned i = 0; i < delay; ++i) {
        assert(f.transaction.step() == State::RetryWait);
        assert(f.transport.prepares == calls && f.transport.sends == 0);
        assert(f.log.bytes == before);
        f.transport.advance(1);
      }
      assert(f.transaction.step() == State::Preparing);
    }
    assert(f.transport.resets == 3);
    for (unsigned i = 0; i < 20; ++i) { f.transaction.step(); f.transport.advance(1); }
    assert(f.transaction.state() == State::Confirmed);
    assert(f.transport.sends == 1 && f.journal.ledger().verified() == 30);
  }
  for (const bool partial : {false, true}) {
    Log log; ExternalTime receipt;
    std::strcpy(receipt.identity.account,"a"); std::strcpy(receipt.identity.book,"b");
    std::strcpy(receipt.identity.source,"s"); receipt.identity.day=20709;
    receipt.sourceMs=6325686; receipt.coveredSeconds=3000;
    receipt.confirmedSeconds=2880; receipt.unknownSeconds=120;
    PacedJournal journal(log); assert(journal.open(receipt));
    SendCoordinator gate(0); Transport transport; transport.log=&log;
    transport.credit=partial ? 7 : 60;
    TimeTransaction tx(journal,gate,transport,TimeTransaction::BatchMode::Bounded60);
    assert(tx.begin());
    assert(tx.waitingSeconds()==30);
    for(int i=0;i<90;++i) { tx.step(); transport.advance(1); }
    assert(transport.sends==1 && transport.sentSeconds==60);
    assert(tx.state()==(partial ? State::Uncertain : State::Confirmed));
    assert(tx.confirmedSeconds()==(partial ? 0U : 60U));
    PacedJournal reboot(log); assert(reboot.open(receipt));
    assert(!reboot.takeSendPermit());
    assert(reboot.ledger().remaining()==3265);
    assert(reboot.ledger().verified()==(partial ? 0U : 60U));
    for(int i=0;i<20;++i) tx.step();
    assert(transport.sends==1);
  }
  {
    Fixture f; f.toReserve(); f.log.space = false;
    assert(f.transaction.step() == State::NotSent);
    assert(f.transaction.issue() == TimeTransaction::Issue::LowSpace);
    assert(f.transport.sends == 0 && f.journal.ledger().state() == PacedLedger::State::Idle);
  }
  {
    Fixture f; f.toSend(); f.transaction.step(); f.transport.advance(5);
    f.transaction.step(); assert(f.transaction.step() == State::Confirmed);
    const auto length = f.log.bytes.size();
    f.log.readCount = 0;
    assert(f.journal.continueVerified(f.receipt, f.receipt.sourceMs));
    assert(f.log.readCount == 1);
    assert(f.log.bytes.size() == length);
    f.log.bytes.back() ^= 1;
    assert(!f.journal.continueVerified(f.receipt, f.receipt.sourceMs));
    assert(!f.journal.reserve({}, f.transport.now, true));
  }
  {
    Fixture f; f.toSend(); f.transaction.step(); f.transport.failedReads = 3;
    for (int i=0; i<3; ++i) {
      f.transport.advance(5); f.transaction.step();
      assert(f.transaction.step() == (i==2 ? State::Uncertain : State::ReadbackWait));
    }
    assert(f.transport.sends == 1 && f.journal.ledger().remaining() == 570);
    assert(f.transaction.issue() == TimeTransaction::Issue::ReadbackFailed);
  }
  {
    Fixture f; f.toSend(); f.transaction.step(); f.transaction.cancel();
    f.transport.advance(30);
    PacedJournal reboot(f.log); assert(reboot.open(f.receipt));
    TimeTransaction recovery(reboot, f.coordinator, f.transport);
    assert(recovery.beginRecovery());
    for (int i=0; i<10 && recovery.state()!=State::Confirmed; ++i) recovery.step();
    assert(recovery.state() == State::Confirmed && f.transport.sends == 1);
    assert(reboot.ledger().verified() == 30);
  }
  {
    Fixture f; f.toSend(); f.transaction.step();
    f.transport.failedReads = 1;
    f.transport.advance(5); f.transaction.step();
    assert(f.transaction.step() == State::ReadbackWait);
    assert(f.transaction.issue() == TimeTransaction::Issue::ReadbackFailed);
    for (int i=0; i<10; ++i) assert(f.transaction.step() == State::ReadbackWait);
    f.transport.advance(5); f.transaction.step();
    assert(f.transaction.step() == State::Confirmed && f.transport.sends == 1);
    assert(f.transaction.issue() == TimeTransaction::Issue::None);
  }
  {
    Fixture f; f.toSend(); f.transaction.step(); f.transaction.cancel();
    f.transport.advance(121);
    PacedJournal reboot(f.log); assert(reboot.open(f.receipt));
    TimeTransaction recovery(reboot, f.coordinator, f.transport);
    const auto bytes = f.log.bytes;
    assert(!recovery.beginRecovery());
    assert(recovery.issue() == TimeTransaction::Issue::Expired);
    assert(f.transport.sends == 1 && bytes == f.log.bytes);
  }
  {
    SendCoordinator gate(0);
    int first = 0, second = 0;
    assert(gate.acquire(&first, 30000));
    gate.response(&second, 40000);  // A non-owner cannot modify the clock.
    gate.response(&first, 45000);   // A slow POST is paced from its response.
    gate.response(&first, 55000);   // Duplicate notifications cannot extend it.
    assert(!gate.acquire(&second, 80000));  // Verification still owns execution.
    gate.release(&first, 80000);
    assert(gate.acquire(&second, 80000));  // No extra cooldown after readback.
  }
  {
    SendCoordinator gate(0xffff8000U);
    int first = 0, second = 0;
    assert(gate.acquire(&first, 0xfffff530U));
    gate.response(&first, 0xfffffff0U);
    gate.release(&first, 5000);
    assert(!gate.acquire(&second, 29983));
    assert(gate.acquire(&second, 29984));  // millis() wrap preserves 30 seconds.
  }
  for (const char* valid : {"{\"succ\":1}", " {\"succ\":true,\"errcode\":0} ", "{\"synckey\":123}"}) {
    assert(acceptedTimeAck(valid, std::strlen(valid), false));
  }
  assert(acceptedTimeAck("{}", 2, true));
  for (const char* invalid : {"", "{}", "[]", "{\"succ\":1,}", "{\"succ\":1}garbage",
       "{\"succ\":1,\"succ\":1}", "{\"data\":{\"succ\":1}}", "{\"succ\":\"1\"}",
       "{\"succ\":1.0}", "{\"succ\":01}", "{\"succ\":1e0}", "{\"succ\":truex}",
       "{\"succ\":0,\"synckey\":1}", "{\"succ\":1,\"errCode\":-1}", "{\"succ\":1,\"errorCode\":1}",
       "{\"synckey\":18446744073709551616}", "{\"succ\":1\"errcode\":0}", "{\"succ\":1,\"unknown\":0}"}) {
    assert(!acceptedTimeAck(invalid, std::strlen(invalid), false));
  }
  {
    Fixture f; f.transport.tick = 29999;
    assert(f.transaction.step() == State::Waiting && f.transport.prepares == 0);
    f.transport.tick = 30000; f.toSend();
    assert(f.transaction.step() == State::ReadbackWait && f.transport.sends == 1);
    for (int i = 0; i < 10; ++i) assert(f.transaction.step() == State::ReadbackWait);
    f.transport.advance(5);
    assert(f.transaction.step() == State::ReadingBack);
    assert(f.transaction.step() == State::Confirmed);
    assert(f.journal.ledger().verified() == 30);
    f.stableTerminal(State::Confirmed);
  }
  for (uint64_t credit : {0ULL, 7ULL, 31ULL}) {
    Fixture f; f.transport.credit = credit; f.toSend();
    assert(f.transaction.step() == State::ReadbackWait);
    for (int i = 0; i < 3; ++i) {
      f.transport.advance(5); assert(f.transaction.step() == State::ReadingBack);
      assert(f.transaction.step() == (i == 2 ? State::Uncertain : State::ReadbackWait));
    }
    assert(f.journal.ledger().verified() == 0 && f.journal.ledger().remaining() == 570);
    f.stableTerminal(State::Uncertain);
  }
  {
    Fixture f; f.transport.credit = 0; f.toSend(); f.transaction.step();
    f.transport.advance(5); f.transaction.step(); assert(f.transaction.step() == State::ReadbackWait);
    f.transport.credit = 30; f.transport.advance(5); f.transaction.step();
    assert(f.transaction.step() == State::Confirmed && f.transport.sends == 1);
  }
  {
    Fixture f; f.toReserve(); f.log.fail = true;
    assert(f.transaction.step() == State::StorageError && f.transport.entries == 0 && f.transport.sends == 0);
    f.stableTerminal(State::StorageError);
  }
  {
    Fixture f; f.transport.failAfterSend = true; f.toSend();
    assert(f.transaction.step() == State::StorageError && f.transport.sends == 1);
    f.stableTerminal(State::StorageError);
    f.log.fail = false; PacedJournal reboot(f.log);
    assert(reboot.open(f.receipt) && !reboot.takeSendPermit());
    TimeTransaction again(reboot, f.coordinator, f.transport);
    assert(!again.begin() && f.transport.sends == 1);
  }
  {
    Fixture f; f.transport.rejectEntry = true; f.toReserve(); f.transaction.step();
    assert(f.transaction.step() == State::Uncertain && f.transport.sends == 0);
    f.stableTerminal(State::Uncertain);
  }
  {
    Fixture f; f.transport.unknownSend = true; f.toSend();
    assert(f.transaction.step() == State::Uncertain && f.transport.sends == 1);
    assert(f.journal.ledger().state() == PacedLedger::State::Uncertain);
    f.stableTerminal(State::Uncertain);
  }
  for (int phase = 0; phase < 6; ++phase) {
    Fixture f; for (int i = 0; i < phase; ++i) f.transaction.step();
    f.transaction.cancel();
    f.stableTerminal(phase >= 4 ? State::Uncertain : State::Cancelled);
    assert(f.transport.sends == 0);
  }
  {
    Fixture f; f.toSend(); f.transport.advance(31);
    assert(f.transaction.step() == State::Uncertain && f.transport.sends == 0);
  }
  {
    Fixture f; f.toSend(); --f.transport.now;
    assert(f.transaction.step() == State::Uncertain && f.transport.sends == 0);
  }
  {
    Fixture f; f.transport.now = 1789401600 + 86400 - 10; f.toSend();
    f.transport.advance(11);  // Do not cross midnight with yesterday's baseline.
    assert(f.transaction.step() == State::Uncertain && f.transport.sends == 0);
  }
  {
    Fixture f; f.transport.rejectPrepare = true;
    assert(f.transaction.step() == State::Preparing);
    assert(f.transaction.step() == State::NotSent && f.transport.sends == 0);
  }
  {
    Fixture f; f.transport.pendingRead = true;
    assert(f.transaction.step() == State::Preparing);
    assert(f.transaction.step() == State::Baseline);
    assert(f.transaction.step() == State::Baseline);
    f.transport.advance(121);
    assert(f.transaction.step() == State::NotSent && f.transport.entries == 0);
  }
  {
    Fixture f; f.transport.missingDay = true; f.toReserve();
    assert(f.transaction.step() == State::NotSent && f.transport.sends == 0);
  }
  {
    Fixture f; f.toSend(); f.transaction.step(); f.transport.advance(121);
    assert(f.transaction.step() == State::Uncertain && f.transport.sends == 1);
  }
  {
    Fixture f; f.toReserve();
    Log secondLog; PacedJournal second(secondLog); assert(second.open(f.receipt));
    TimeTransaction other(second, f.coordinator, f.transport); assert(other.begin());
    assert(other.step() == State::Waiting);
    f.transaction.cancel(); assert(other.step() == State::Waiting);
    f.transport.advance(30); assert(other.step() == State::Preparing);
  }
  for (uint32_t verificationSeconds : {5U, 20U, 35U}) {
    Fixture f; f.toSend();
    assert(f.transaction.step() == State::ReadbackWait);
    f.transport.advance(verificationSeconds);
    assert(f.transaction.step() == State::ReadingBack);
    assert(f.transaction.step() == State::Confirmed);
    TimeTransaction next(f.journal, f.coordinator, f.transport);
    assert(next.begin());
    if (verificationSeconds < 30) {
      assert(next.step() == State::Waiting);
      f.transport.advance(29 - verificationSeconds);
      assert(next.step() == State::Waiting);
      f.transport.advance(1);
    }
    // Readback consumes the response interval; completion must not restart it.
    assert(next.step() == State::Preparing);
  }
  std::cout << "Time transaction: single write, durability, cancellation, reboot, pacing, bounded readback PASS\n";
}
