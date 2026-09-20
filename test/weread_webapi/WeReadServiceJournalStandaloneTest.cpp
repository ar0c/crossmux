#include <cassert>
#include <iostream>
#include <vector>

#include "LegacyServiceJournalFixture.h"
#include "WeReadServiceJournal.h"
using namespace WeReadTime;
struct Log : ByteLog {
  std::vector<uint8_t> bytes;
  bool torn = false, space = true;
  size_t failAfter = SIZE_MAX;
  ReadState size(uint64_t& n) override {
    n = bytes.size();
    return n ? ReadState::Ready : ReadState::Missing;
  }
  bool read(uint64_t o, uint8_t* b, size_t n) override {
    if (o + n > bytes.size()) return false;
    std::memcpy(b, bytes.data() + o, n);
    return true;
  }
  bool appendAndSync(const uint8_t* b, size_t n) override {
    bytes.insert(bytes.end(), b, b + std::min(n, torn ? n / 2 : failAfter));
    return !torn && failAfter == SIZE_MAX;
  }
  bool canReserve(size_t) override { return space; }
};
static constexpr const char* device = "abcdef012345678901234567";
void incremental(const Identity& id) {
  Log log;
  ServiceJournal j(log);
  assert(j.open(id, 0, 3000) && j.reserve(device) && j.accept(false));
  assert(j.open(id, 0, 3600) && j.reserve(device));
  assert(j.start() == 3000 && j.end() == 3600 && j.unacknowledged() == 600);
  char before[128], after[128];
  assert(j.jobId(before, sizeof(before)));
  assert(j.open(id, 0, 4200) && j.selectReserved() && j.jobId(after, sizeof(after)) && !strcmp(before, after));
  assert(!j.reserve(device) && j.accept(false) && j.reserve(device) && j.start() == 3600 && j.end() == 4200);
  assert(j.accept(false) && j.selectAccepted() && j.start() == 0 && j.accept(false, 60, 100));
  assert(!j.accept(false, 0, 101));                            // Regressed server backup cannot erase observed credit.
  assert(j.selectAccepted(3000) && j.accept(true, 600, 101));  // Out-of-order receipts are not a prefix.
  assert(j.confirmed() == 660 && j.pending() == 3540);
  assert(j.open(id, 0, 4200) && j.confirmed() == 660 && j.selectAccepted() && j.credit() == 60);
  assert(j.checkedAt() == 0);  // The third receipt still has no clock observation.
  log.space = false;
  assert(!j.reserve(device));
}
void legacy(const Identity& id) {
  for (unsigned state = 1; state <= 3; ++state) {
    Log log;
    LegacyServiceJournal old(log);
    assert(old.open(id, 90, 3090) && old.reserve(device));
    if (state >= 2) assert(old.accept(state == 3));
    char original[128], recovered[128];
    assert(old.jobId(original, sizeof(original)));
    const auto prefix = log.bytes;
    ServiceJournal j(log);
    assert(j.open(id, 90, 3690) && j.jobId(recovered, sizeof(recovered)) && !strcmp(original, recovered));
    if (state == 1) assert(j.accept(false));
    assert(j.reserve(device) && j.start() == 3090 && j.end() == 3690);
    assert(std::equal(prefix.begin(), prefix.end(), log.bytes.begin()));
    LegacyServiceJournal downgraded(log);
    assert(!downgraded.open(id, 90, 3690));
    assert(j.open(id, 90, 3690) && j.unacknowledged() == 600);
    assert(j.confirmed() == (state == 3 ? 3000 : 0));
  }
}
void capacity(const Identity& id) {
  Log log;
  ServiceJournal j(log);
  for (size_t i = 0; i < ServiceJournal::kMaxPending; ++i) {
    assert(j.open(id, 0, i + 1) && j.reserve(device) && j.accept(false));
  }
  assert(j.open(id, 0, 100) && !j.capacity() && !j.reserve(device));
  assert(j.selectAccepted() && j.accept(true) && j.capacity() && j.reserve(device));
  assert(j.start() == ServiceJournal::kMaxPending && j.end() == 100);
}
void powerCuts(const Identity& id) {
  // Every byte boundary of the migration/reservation/receipt frame, including
  // complete writes that returned failure. Never send again based on RAM state.
  for (unsigned phase = 0; phase < 3; ++phase)
    for (size_t cut = 0; cut <= ServiceJournal::kSize; ++cut) {
      Log log;
      LegacyServiceJournal old(log);
      assert(old.open(id, 0, 3000) && old.reserve(device));
      if (phase > 0) assert(old.accept(false));
      ServiceJournal j(log);
      assert(j.open(id, 0, 3600));
      if (phase == 2) assert(j.reserve(device));
      log.failAfter = cut;
      assert(!(phase == 1 ? j.reserve(device) : j.accept(false)));
      assert(!j.reserve(device));
      ServiceJournal reboot(log);
      const bool opened = reboot.open(id, 0, 3600);
      if (cut > 0 && cut < ServiceJournal::kSize)
        assert(!opened);
      else {
        assert(opened);
        assert(reboot.owned() == (phase == 1 && cut == 0 ? 3000 : (phase == 0 ? 3000 : 3600)));
        assert(reboot.confirmed() == 0);
      }
    }
}
int main() {
  Ledger v;
  assert(v.bind("123", "26435427", "source", 20716));
  const auto id = v.identity();
  incremental(id);
  legacy(id);
  capacity(id);
  powerCuts(id);
  Log log;
  ServiceJournal j(log);
  assert(j.open(id, 90, 690) && j.owned() == 0 && j.reserve("abcdef012345678901234567"));
  assert(j.owned() == 600 && j.confirmed() == 0 && j.pending() == 600);
  char first[128], again[128];
  assert(j.jobId(first, sizeof(first)));
  ServiceJournal reboot(log);
  assert(reboot.open(id, 90, 720) && reboot.state() == ServiceJournal::State::Reserved);
  assert(reboot.jobId(again, sizeof(again)) && !std::strcmp(first, again));
  assert(!reboot.reserve("abcdef012345678901234567"));
  assert(!reboot.matchesDevice("000000000000000000000000"));
  assert(reboot.accept(false) && reboot.confirmed() == 0);
  ServiceJournal ack(log);
  assert(ack.open(id, 90, 720) && ack.state() == ServiceJournal::State::Accepted);
  assert(ack.accept(true) && ack.confirmed() == 600 && ack.pending() == 0);
  assert(ack.reserve("abcdef012345678901234567") && ack.start() == 690 && ack.end() == 720);
  assert(ack.accept(true) && ack.confirmed() == 630);
  ServiceJournal changed(log);
  auto other = id;
  std::strcpy(other.book, "999");
  assert(!changed.open(other, 90, 720));
  assert(!changed.open(id, 91, 720));
  assert(!changed.open(id, 90, 719));
  assert(changed.open(id, 90, 750));
  log.torn = true;
  assert(!changed.reserve("abcdef012345678901234567"));
  ServiceJournal torn(log);
  assert(!torn.open(id, 90, 750));
  Log corrupt;
  corrupt.bytes = log.bytes;
  corrupt.bytes.resize(ServiceJournal::kSize);
  corrupt.bytes[216] ^= 1;
  ServiceJournal crc(corrupt);
  assert(!crc.open(id, 90, 750));
  std::cout << "PASS service journal: 50+10 incremental queue, v1 migration/downgrade rejection, all 771 power-cut "
               "boundaries, bounded capacity, partial/out-of-order receipts, identity guards\n";
}
