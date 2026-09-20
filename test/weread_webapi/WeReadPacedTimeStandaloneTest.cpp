#include "WeReadPacedTime.h"

#include <cassert>
#include <iostream>
#include <fstream>
#include <vector>

using namespace WeReadTime;
struct MemoryLog : ByteLog {
  std::vector<uint8_t> bytes;
  bool torn = false, readError = false;
  ReadState size(uint64_t& size) override {
    size = bytes.size();
    return bytes.empty() ? ReadState::Missing : ReadState::Ready;
  }
  bool read(uint64_t at, uint8_t* out, size_t count) override {
    if (readError || at + count > bytes.size()) return false;
    std::memcpy(out, bytes.data() + at, count);
    return true;
  }
  bool appendAndSync(const uint8_t* data, size_t count) override {
    bytes.insert(bytes.end(), data, data + (torn ? count / 2 : count));
    return !torn;
  }
};

AccountSnapshot sample(uint64_t at, uint64_t total = 16234, uint64_t day = 2947) {
  AccountSnapshot result;
  std::strcpy(result.account, "account");
  result.month = 1788192000; result.day = 1789401600;
  result.monthSeconds = total; result.daySeconds = day;
  result.sampledAt = at; result.complete = true;
  return result;
}

int main(int argc, char** argv) {
  ExternalTime external;
  std::strcpy(external.identity.account, "account");
  std::strcpy(external.identity.book, "book");
  std::strcpy(external.identity.source, "source");
  external.identity.day = 20709;
  external.sourceMs = 6325686;
  external.coveredSeconds = 3000; external.confirmedSeconds = 2880; external.unknownSeconds = 120;
  const uint64_t at = 1789453168;
  for (const bool torn : {false, true}) {
    MemoryLog log; PacedJournal journal(log); assert(journal.open(external));
    assert(journal.reserve(sample(at),at,true));
    assert(journal.takeSendPermit() && journal.acknowledge(false,at+1));
    const auto original=log.bytes;
    assert(!journal.quarantine(at) && !journal.quarantine(at+121));
    log.torn=torn;
    assert(journal.quarantine(at+122)==!torn);
    assert(!journal.takeSendPermit());
    assert(std::equal(original.begin(),original.end(),log.bytes.begin()));
    PacedJournal reboot(log);
    if(torn) { assert(!reboot.open(external)); continue; }
    assert(reboot.open(external));
    assert(reboot.ledger().quarantinedSeconds()==30 && reboot.ledger().verified()==0);
    assert(reboot.ledger().remaining()==3295 && !reboot.quarantine(at+500));
    assert(reboot.reserve(sample(at+152),at+152,true,3295));
    assert(reboot.takeSendPermit() && reboot.acknowledge(true,at+153));
    assert(reboot.verify(sample(at+154,19529,6242)));
    PacedJournal confirmed(log); assert(confirmed.open(external));
    assert(confirmed.ledger().verified()==3295 && confirmed.ledger().quarantinedSeconds()==30);
    assert(confirmed.ledger().remaining()==0);
    log.bytes.insert(log.bytes.end(),original.begin(),original.begin()+PacedLedger::kSize);
    assert(!confirmed.open(external));
  }
  {
    // Actual v2 wire layout: implicit 30 seconds, upper month word zero.
    PacedLedger legacy; assert(legacy.initialize(external));
    assert(legacy.reserve(sample(at),at,true));
    uint8_t bytes[PacedLedger::kSize]; assert(legacy.encode(bytes));
    bytes[4]=2; std::memset(bytes+204,0,4);
    const uint64_t crc=ExternalTime::checksum(bytes,248);
    for(unsigned i=0;i<8;++i) bytes[248+i]=static_cast<uint8_t>(crc>>(8*i));
    PacedLedger decoded; assert(decoded.decode(bytes));
    assert(decoded.batchSeconds()==30 && decoded.remaining()==3295);
    assert(!decoded.reserve(sample(at),at,true,3295));
    assert(decoded.acknowledge(true,at+1));
    assert(decoded.verify(sample(at+2,16264,2977)));
    assert(decoded.reserve(sample(at+32,16264,2977),at+32,true,3295));
    assert(decoded.encode(bytes));
    PacedLedger reboot; assert(reboot.decode(bytes));
    assert(reboot.batchSeconds()==3295 && reboot.remaining()==0);
    assert(!reboot.reserve(sample(at+32),at+32,true,3295));
    assert(reboot.acknowledge(true,at+33));
    assert(!reboot.verify(sample(at+34,16294,3007)));
    assert(reboot.verify(sample(at+34,19559,6272)));
    assert(reboot.verified()==3325 && reboot.encode(bytes));
    assert(decoded.decode(bytes) && decoded.verified()==3325);
  }
  {
    PacedLedger bounds; assert(bounds.initialize(external));
    assert(!bounds.reserve(sample(at),at,true,0));
    assert(!bounds.reserve(sample(at),at,true,3326));
    assert(!bounds.reserve(sample(at),at,true,uint64_t(UINT32_MAX)+1));
    assert(!bounds.reserve(sample(at,UINT64_MAX),at,true,3325));
  }
  {
    MemoryLog mixed;
    PacedLedger old; assert(old.initialize(external));
    uint8_t frame[PacedLedger::kSize]; assert(old.encode(frame));
    // A real v1 initial frame, followed by v2 missing-day reservation/ACK/credit.
    frame[4]=1;
    uint64_t checksum=ExternalTime::checksum(frame,248);
    for (unsigned i=0;i<8;++i) frame[248+i]=static_cast<uint8_t>(checksum>>(8*i));
    mixed.bytes.assign(frame,frame+sizeof(frame));
    PacedJournal bootstrap(mixed); assert(bootstrap.open(external));
    auto baseline=sample(at,16234,0); baseline.complete=false; baseline.monthComplete=true;
    assert(bootstrap.reserve(baseline,at,true));
    assert(mixed.bytes[PacedLedger::kSize+4]==4 && mixed.bytes[PacedLedger::kSize+6]==1);
    PacedJournal powerLoss(mixed); assert(powerLoss.open(external));
    assert(!powerLoss.takeSendPermit() && !powerLoss.reserve(baseline,at,true));
    assert(bootstrap.takeSendPermit() && bootstrap.acknowledge(true,at+1));
    PacedJournal resumed(mixed); assert(resumed.open(external));
    assert(!resumed.verify(sample(at+2,16264,0)));
    auto absent=sample(at+2,16264,30); absent.complete=false; absent.monthComplete=true;
    assert(!resumed.verify(absent));
    assert(resumed.verify(sample(at+2,16264,30)));
    PacedJournal credited(mixed); assert(credited.open(external));
    assert(credited.ledger().verified()==30 && !credited.verify(sample(at+3,16264,30)));
    assert(credited.continueVerified(external,external.sourceMs));
  }
  MemoryLog log;
  PacedJournal journal(log);
  assert(journal.open(external) && journal.ledger().remaining() == 3325);
  assert(!journal.reserve(sample(at), at, false));
  assert(journal.reserve(sample(at), at, true));
  assert(journal.ledger().remaining() == 3295);
  assert(!journal.acknowledge(true, at + 1));  // No send permit consumed.
  assert(journal.takeSendPermit() && !journal.takeSendPermit());
  PacedJournal reboot(log);
  assert(reboot.open(external) && !reboot.takeSendPermit());
  assert(!reboot.reserve(sample(at + 31), at + 31, true));
  assert(!reboot.acknowledge(true, at + 1));
  assert(journal.acknowledge(true, at + 1));
  assert(!journal.verify(sample(at + 2, 16241, 2954)));  // Partial credit.
  assert(!journal.verify(sample(at + 2, 16264, 2947)));  // Month-only credit.
  auto wrong = sample(at + 2, 16264, 2977);
  std::strcpy(wrong.account, "other");
  assert(!journal.verify(wrong));
  assert(!journal.verify(sample(at + 200, 16264, 2977)));
  PacedJournal ackReboot(log);
  assert(ackReboot.open(external));
  assert(ackReboot.verify(sample(at + 2, 16264, 2977)));
  assert(ackReboot.ledger().verified() == 30 && ackReboot.ledger().remaining() == 3295);
  assert(!ackReboot.reserve(sample(at + 10, 16264, 2977), at + 10, true));
  assert(ackReboot.reserve(sample(at + 31, 16264, 2977), at + 31, true));
  assert(ackReboot.takeSendPermit());
  assert(ackReboot.acknowledge(false, at + 32));
  assert(!ackReboot.verify(sample(at + 33, 16294, 3007))); // No ACK, no confirmation.
  PacedJournal unknownReboot(log);
  assert(unknownReboot.open(external) && !unknownReboot.takeSendPermit());
  assert(!unknownReboot.reserve(sample(at + 100), at + 100, true));
  assert(unknownReboot.collect(6326000));
  assert(!unknownReboot.collect(6325686));
  assert(unknownReboot.ledger().remaining() == 3266);
  auto changed = external;
  changed.coveredSeconds = 0; changed.confirmedSeconds = 0; changed.unknownSeconds = 0;
  PacedJournal wrongReceipt(log);
  assert(!wrongReceipt.open(changed));
  // A valid old Idle frame appended after uncertainty is not a legal reset.
  const std::vector<uint8_t> idle(log.bytes.begin(), log.bytes.begin() + PacedLedger::kSize);
  log.bytes.insert(log.bytes.end(), idle.begin(), idle.end());
  PacedJournal illegalReset(log);
  assert(!illegalReset.open(external));

  for (bool torn : {false, true}) {
    MemoryLog broken;
    PacedJournal writer(broken);
    assert(writer.open(external));
    broken.torn = torn; broken.readError = !torn;
    assert(!writer.reserve(sample(at), at, true));
    assert(!writer.takeSendPermit());
    broken.readError = false;
    PacedJournal recovered(broken);
    if (torn) assert(!recovered.open(external));
    else assert(recovered.open(external) && !recovered.takeSendPermit());
  }
  PacedGate gate(UINT32_MAX - 10000);
  assert(!gate.ready(5000)); assert(gate.ready(20000));
  gate.response(20000);
  assert(!gate.ready(10000)); assert(!gate.ready(49999)); assert(gate.ready(50000));
  std::cout << "Paced 30s journal: reboot, one-use permit, exact readback, unknown isolation, torn writes, monotonic gate PASS\n";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--journal")) {
      assert(++i < argc);
      std::ifstream file(argv[i],std::ios::binary | std::ios::ate);
      const auto size=file.tellg();
      assert(size>0 && size<=4*1024*1024 && size%PacedLedger::kSize==0);
      MemoryLog imported; imported.bytes.resize(static_cast<size_t>(size));
      file.seekg(0); assert(file.read(reinterpret_cast<char*>(imported.bytes.data()),size));
      PacedLedger first; assert(first.decode(imported.bytes.data()));
      ExternalTime seed; seed.identity=first.identity(); seed.sourceMs=first.measuredMs();
      seed.coveredSeconds=seed.unknownSeconds=ExternalTime::number(imported.bytes.data()+184);
      PacedJournal replay(imported); assert(replay.open(seed));
      assert(!replay.takeSendPermit());
      std::cout << "Actual journal replay: frames=" << size/PacedLedger::kSize
                << " verified=" << replay.ledger().verified()
                << " pending=" << replay.ledger().batchSeconds() << " PASS\n";
      if(replay.ledger().state()!=PacedLedger::State::Idle && replay.ledger().responseAt()) {
        const auto before=replay.ledger();
        assert(replay.quarantine(before.responseAt()+121));
        assert(replay.ledger().remaining()==before.remaining());
        assert(replay.ledger().verified()==before.verified());
        assert(replay.ledger().quarantinedSeconds()==before.quarantinedSeconds()+before.batchSeconds());
        PacedJournal restarted(imported); assert(restarted.open(seed));
        assert(!restarted.takeSendPermit());
        std::cout << "Actual journal in-memory quarantine/reopen: unchanged balance and credit PASS\n";
      }
      continue;
    }
    std::ifstream input(argv[i], std::ios::binary);
    uint8_t bytes[ExternalTime::kSize];
    assert(input.read(reinterpret_cast<char*>(bytes), sizeof(bytes)));
    assert(input.peek() == std::char_traits<char>::eof());
    ExternalTime receipt;
    assert(receipt.decode(bytes, sizeof(bytes)));
    MemoryLog imported;
    PacedJournal device(imported);
    assert(device.open(receipt));
    assert(device.ledger().remaining() == receipt.sourceMs / 1000 - receipt.coveredSeconds);
    std::cout << "Actual handover day=" << receipt.identity.day << " excluded=" << receipt.coveredSeconds
              << " remaining=" << device.ledger().remaining() << " seconds PASS\n";
  }
}
