#include <cassert>
#include <iostream>

#include "WeReadTimeStorage.h"
using namespace WeReadTime;
int main() {
  {
    fakeStorage::reset();
    SdByteLog log;
    assert(log.configure("a", "b", "s", 20709, SdByteLog::Format::Paced30));
    assert(log.canReserve(768));
    fakeStorage::freeBytes = 1024 * 1024 + 767;
    assert(!log.canReserve(768));
    fakeStorage::failSpace = true;
    assert(!log.canReserve(768));
    // Space gate must never prevent writing a receipt after sending.
    const uint8_t receipt[] = {1, 2, 3};
    assert(log.appendAndSync(receipt, sizeof(receipt)));
    fakeStorage::reset();
  }
  Ledger ledger;
  assert(ledger.bind("account", "book", "source", 20709) && ledger.collect(6325686));
  uint64_t pending = 0, confirmed = 0, unknown = 0;
  for (const char* location : {"/", "/WeReadSync", "/WeReadSync/history"}) {
    fakeStorage::reset();
    Storage.ensureDirectoryExists(location);
    const std::string prefix = std::string(location) == "/" ? "/" : std::string(location) + "/";
    fakeStorage::files[prefix + "weread-backlog-reserved.json"] = {1};
    ExternalTimeStorage external;
    assert(!external.reconcile(ledger, pending, confirmed, unknown));
    assert(pending == 0);  // Archiving must never release unknown historical time.
  }
  fakeStorage::reset();
  uint8_t frame[240]{};
  assert(ledger.encode(frame, sizeof(frame)));
  std::memcpy(frame, "WRTX", 4);
  auto put = [&](size_t at, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) frame[at + i] = static_cast<uint8_t>(value >> (8 * i));
  };
  put(184, 3000);
  put(192, 2880);
  put(200, 120);
  put(232, ExternalTime::checksum(frame, 232));
  const char* oldPath = "/weread-external-account-source-20709.bin";
  const char* newPath = "/WeReadSync/weread-external-account-source-20709.bin";
  const std::vector<uint8_t> original(frame, frame + sizeof(frame));
  fakeStorage::files[oldPath] = original;
  ExternalTimeStorage external;
  assert(external.reconcile(ledger, pending, confirmed, unknown));
  assert(pending == 3325 && confirmed == 2880 && unknown == 120);
  assert(!Storage.exists(oldPath) && fakeStorage::files.at(newPath) == original);
  assert(external.reconcile(ledger, pending, confirmed, unknown) && pending == 3325);
  assert(external.hasExternal(ledger.identity()));
  fakeStorage::files[oldPath] = original;
  assert(!external.reconcile(ledger, pending, confirmed, unknown));
  assert(fakeStorage::files.at(oldPath) == original && fakeStorage::files.at(newPath) == original);
  fakeStorage::reset();
  fakeStorage::files[oldPath] = original;
  fakeStorage::failRename = true;
  assert(!external.reconcile(ledger, pending, confirmed, unknown) && Storage.exists(oldPath));
  std::cout
      << "HAL time storage: archived reservations stay excluded, exact receipt migration, conflicts and failure PASS\n";
}
