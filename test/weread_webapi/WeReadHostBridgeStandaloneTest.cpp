#include "WeReadHostBridge.h"
#include <cassert>
#include <iostream>
// Synthetic server snapshots exercise production accounting, not cloud acceptance.
void continuousBatches() {
  using namespace WeReadTime;
  Ledger id;
  assert(id.bind("123", "456", "offline-only", 20709));
  ExternalTime source;
  source.identity = id.identity(); source.sourceMs = 105 * 60 * 1000 + 17000;
  PacedLedger initial;
  assert(initial.initialize(source));
  std::vector<uint8_t> journal(PacedLedger::kSize);
  assert(initial.encode(journal.data()));
  AccountSnapshot baseline;
  std::memcpy(baseline.account, "123", 4);
  baseline.month = 1788192000; baseline.day = 1789488000;
  baseline.sampledAt = 1789549000;
  baseline.monthSeconds = 10000; baseline.daySeconds = 100;
  baseline.complete = baseline.monthComplete = true;
  for (unsigned batch = 0; batch < 106; ++batch) {
    HostBridge bridge;
    assert(bridge.open(journal)); // Process restart before every reservation.
    const auto seconds = std::min<uint64_t>(60, bridge.ledger().remaining());
    assert(seconds > 0 && bridge.reserve(baseline, baseline.sampledAt, seconds));
    HostBridge reopened;
    assert(reopened.open(bridge.bytes()));
    assert(!reopened.reserve(baseline, baseline.sampledAt, seconds));
    auto observed = baseline;
    observed.sampledAt += 10;
    observed.monthSeconds += seconds; observed.daySeconds += seconds;
    assert(reopened.settle(true, baseline.sampledAt + 2, observed));
    assert(reopened.ledger().batchSeconds() == 0);
    assert(std::equal(journal.begin(), journal.end(), reopened.bytes().begin()));
    journal = reopened.bytes();
    baseline = observed; baseline.sampledAt += 30;
  }
  HostBridge final;
  assert(final.open(journal));
  assert(final.ledger().verified() == 6317 && final.ledger().remaining() == 0);
  assert(!final.reserve(baseline, baseline.sampledAt, 1));
  std::cout << "Synthetic sequential batches: 105 x 60s + 17s, reopen each batch, no network PASS\n";
}
int main() {
  continuousBatches();
  using namespace WeReadTime;
  ExternalTime source;
  Ledger id;
  assert(id.bind("123", "456", "source", 20709));
  source.identity = id.identity(); source.sourceMs = 6325686;
  source.coveredSeconds = 3000; source.confirmedSeconds = 2880; source.unknownSeconds = 120;
  PacedLedger initial;
  assert(initial.initialize(source));
  std::vector<uint8_t> bytes(PacedLedger::kSize);
  assert(initial.encode(bytes.data()));
  HostBridge bridge;
  assert(bridge.open(bytes));
  AccountSnapshot baseline;
  std::memcpy(baseline.account,"123",4);
  baseline.month = 1788192000; baseline.day = 1789488000;
  baseline.sampledAt = 1789549000; baseline.monthSeconds = 16579; baseline.daySeconds = 178;
  baseline.complete = baseline.monthComplete = true;
  assert(bridge.reserve(baseline, baseline.sampledAt, 60));
  assert(bridge.ledger().remaining() == 3265);
  HostBridge reopened;
  assert(reopened.open(bridge.bytes()));
  assert(!reopened.reserve(baseline, baseline.sampledAt, 60));
  assert(std::equal(bytes.begin(),bytes.end(),bridge.bytes().begin()));
  auto observed = baseline;
  observed.sampledAt += 10; observed.monthSeconds += 60; observed.daySeconds += 60;
  assert(reopened.settle(true,baseline.sampledAt+2,observed));
  assert(reopened.ledger().verified()==60 && reopened.ledger().batchSeconds()==0);
  assert(bridge.settle(false,baseline.sampledAt+2,observed));
  assert(bridge.ledger().verified()==0 && bridge.ledger().batchSeconds()==60);
  assert(!bridge.reserve(observed,observed.sampledAt,60));
  HostBridge partial;
  assert(partial.open(bytes) && partial.reserve(baseline,baseline.sampledAt,60));
  observed.daySeconds -= 53; observed.monthSeconds -= 53;
  assert(partial.settle(true,baseline.sampledAt+2,observed));
  assert(partial.ledger().batchSeconds()==60 && partial.ledger().verified()==0);
  const auto pendingBytes = partial.bytes();
  assert(!partial.canConfirm(observed));
  observed.daySeconds += 53; observed.monthSeconds += 53;
  assert(partial.canConfirm(observed));
  assert(partial.bytes() == pendingBytes);
  assert(partial.ledger().verified() == 0 && partial.ledger().batchSeconds() == 60);
  observed.sampledAt = baseline.sampledAt + 123;
  assert(!partial.canConfirm(observed)); // Exact but outside the production readback window.
  const auto preserved = partial.bytes();
  const auto remaining = partial.ledger().remaining();
  const auto isolated = partial.ledger().quarantinedSeconds();
  assert(!partial.isolate(baseline.sampledAt + 122));
  assert(partial.isolate(baseline.sampledAt + 123));
  assert(partial.bytes().size() == preserved.size() + PacedLedger::kSize);
  assert(std::equal(preserved.begin(), preserved.end(), partial.bytes().begin()));
  assert(partial.ledger().verified() == 0 && partial.ledger().remaining() == remaining);
  assert(partial.ledger().quarantinedSeconds() == isolated + 60);
  assert(!partial.isolate(baseline.sampledAt + 124));
  HostBridge recovered;
  assert(recovered.open(partial.bytes()));
  baseline.sampledAt += 154;
  assert(recovered.reserve(baseline, baseline.sampledAt, 60));
  assert(recovered.ledger().remaining() == remaining - 60);
  auto corrupt=partial.bytes(); corrupt.back()^=1;
  assert(!reopened.open(corrupt));
  std::cout << "Host bridge: production reservation survives reopen, immutable prefix PASS\n";
}
