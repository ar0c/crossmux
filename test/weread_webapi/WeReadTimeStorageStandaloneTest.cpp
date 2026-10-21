#include <cassert>
#include <iostream>

#include "WeReadServiceJournal.h"
#include "WeReadTimeStorage.h"
using namespace WeReadTime;
void replayHandles() {
  fakeStorage::reset();
  SdByteLog log;
  Journal journal(log);
  assert(log.configure("account", "book", "source", 20709));
  assert(journal.open("account", "book", "source", 20709));
  for (uint64_t i = 1; i <= 63; ++i) assert(journal.collect(i * 1000));
  assert(fakeStorage::activeHandles == 0 && !fakeStorage::writeWithOtherHandle);
  const auto original = fakeStorage::files;
  fakeStorage::opens = fakeStorage::reads = fakeStorage::seeks = 0;
  assert(journal.open("account", "book", "source", 20709));
  assert(fakeStorage::opens == 1 && fakeStorage::reads == 64 && fakeStorage::seeks == 0);
  assert(fakeStorage::activeHandles == 0 && fakeStorage::files == original);
  assert(fakeStorage::liveStates == 0);
  fakeStorage::failOpen = true;
  assert(!journal.open("account", "book", "source", 20709));
  assert(fakeStorage::activeHandles == 0 && !journal.ready());
  assert(fakeStorage::liveStates == 0);
  fakeStorage::failOpen = false;
  fakeStorage::shortReadOn = fakeStorage::reads + 2;
  assert(!journal.open("account", "book", "source", 20709));
  assert(fakeStorage::activeHandles == 0 && !journal.ready());
  assert(fakeStorage::liveStates == 0);
  fakeStorage::shortReadOn = 0;
  const char* legacyPath = "/.crosspoint/weread/time/account/source/20709.wrtl";
  // Corruption in an early frame must still fail even when the tail is valid.
  fakeStorage::files[legacyPath][Ledger::kEncodedSize + 7] ^= 1;
  assert(!journal.open("account", "book", "source", 20709));
  assert(fakeStorage::activeHandles == 0 && !journal.ready());
  assert(fakeStorage::liveStates == 0);
  fakeStorage::files = original;
  assert(journal.open("account", "book", "source", 20709));
  assert(journal.collect(64000));  // A completed replay must not retain a stale read handle.
  assert(fakeStorage::activeHandles == 0 && !fakeStorage::writeWithOtherHandle);
  assert(journal.open("account", "book", "source", 20709) && journal.ledger().measuredMs() == 64000);

  // The same session also supports offset reads, fresh size and append readback.
  {
    ByteLog::ReadScope scope(log);
    uint64_t size = 0;
    uint8_t frame[Ledger::kEncodedSize];
    assert(log.size(size) == ByteLog::ReadState::Ready && fakeStorage::activeHandles == 1);
    assert(log.read(Ledger::kEncodedSize, frame, sizeof(frame)));
    fakeStorage::failSeek = true;
    assert(!log.read(0, frame, sizeof(frame)));
    fakeStorage::failSeek = false;
    assert(log.read(size - sizeof(frame), frame, sizeof(frame)));
    assert(log.appendAndSync(frame, sizeof(frame)) && fakeStorage::activeHandles == 0);
    uint64_t updated = 0;
    assert(log.size(updated) == ByteLog::ReadState::Ready && updated == size + sizeof(frame));
    assert(log.read(size, frame, sizeof(frame)) && !fakeStorage::writeWithOtherHandle);
  }
  assert(fakeStorage::activeHandles == 0);
  assert(fakeStorage::liveStates == 0);

  ExternalTime receipt;
  receipt.identity = journal.ledger().identity();
  assert(log.configure("account", "book", "source", 20709, SdByteLog::Format::Paced30));
  PacedJournal paced(log);
  assert(paced.open(receipt));
  for (uint64_t i = 1; i <= 63; ++i) assert(paced.collect(i * 1000));
  fakeStorage::opens = fakeStorage::reads = fakeStorage::seeks = 0;
  assert(paced.open(receipt));
  assert(fakeStorage::opens == 1 && fakeStorage::reads == 64 && fakeStorage::seeks == 0);
  assert(fakeStorage::activeHandles == 0 && !fakeStorage::writeWithOtherHandle);
  assert(fakeStorage::liveStates == 0);
  const char* pacedPath = "/.crosspoint/weread/time/account/source/20709.wrp2";
  fakeStorage::files[pacedPath][PacedLedger::kSize + 7] ^= 1;
  assert(!paced.open(receipt) && !paced.takeSendPermit() && fakeStorage::activeHandles == 0);

  assert(log.configure("account", "book", "source", 20709, SdByteLog::Format::Service));
  ServiceJournal service(log);
  assert(service.open(receipt.identity, 0, 3000) && service.reserve("abcdef0123456789"));
  for (uint64_t credit = 1; credit <= 63; ++credit) assert(service.accept(false, credit, credit));
  fakeStorage::opens = fakeStorage::reads = fakeStorage::seeks = 0;
  assert(service.open(receipt.identity, 0, 3000));
  assert(fakeStorage::opens == 1 && fakeStorage::reads == 64 && fakeStorage::seeks == 0);
  assert(fakeStorage::activeHandles == 0 && service.credit() == 63 && !fakeStorage::writeWithOtherHandle);
  assert(fakeStorage::liveStates == 0);
  const char* servicePath = "/.crosspoint/weread/time/account/source/20709.wrs1";
  fakeStorage::files[servicePath][ServiceJournal::kSize + 7] ^= 1;
  assert(!service.open(receipt.identity, 0, 3000) && !service.reserve("abcdef0123456789"));
  assert(fakeStorage::activeHandles == 0);
  // Reconfiguration during a read releases the old path before validation fails.
  log.beginRead();
  uint64_t size = 0;
  assert(log.size(size) == ByteLog::ReadState::Ready);
  assert(!log.configure("", "book", "source", 20709) && fakeStorage::activeHandles == 0);
  log.endRead();
  assert(fakeStorage::liveStates == 0);
}
int main() {
  replayHandles();
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
