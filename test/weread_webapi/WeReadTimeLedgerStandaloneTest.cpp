#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "WeReadTimeJournal.h"
#include "WeReadTimeLedger.h"

class Disk : public WeReadTime::ByteLog {
 public:
  std::vector<uint8_t> bytes;
  bool fail = false;
  bool torn = false;
  bool exists = false;
  ReadState size(uint64_t& length) override {
    length = bytes.size();
    return exists ? ReadState::Ready : ReadState::Missing;
  }
  bool read(uint64_t offset, uint8_t* out, size_t count) override {
    if (offset + count > bytes.size()) return false;
    std::memcpy(out, bytes.data() + offset, count);
    return true;
  }
  bool appendAndSync(const uint8_t* data, size_t count) override {
    exists = true;
    if (fail) return false;
    bytes.insert(bytes.end(), data, data + (torn ? count / 2 : count));
    return !torn;
  }
};

class FileDisk : public WeReadTime::ByteLog {
 public:
  explicit FileDisk(std::filesystem::path path) : path_(std::move(path)) {}
  ReadState size(uint64_t& length) override {
    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    length = 0;
    if (error) return ReadState::Error;
    if (!exists) return ReadState::Missing;
    length = std::filesystem::file_size(path_, error);
    return error ? ReadState::Error : ReadState::Ready;
  }
  bool read(uint64_t offset, uint8_t* out, size_t count) override {
    std::ifstream file(path_, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count));
    return file.good();
  }
  bool appendAndSync(const uint8_t* bytes, size_t count) override {
    std::ofstream file(path_, std::ios::binary | std::ios::app);
    file.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(count));
    file.flush();
    return file.good();
  }

 private:
  std::filesystem::path path_;
};

int main() {
  using namespace WeReadTime;
  Ledger unbound;
  assert(!unbound.collect(6300000));
  assert(!unbound.bind("account", "book", "source", 0));
  Ledger ledger;
  assert(ledger.bind("account", "book", "local-book", 20709));
  assert(ledger.collect(6300000));
  assert(ledger.pendingSeconds() == 6300);
  assert(ledger.collect(6300000));
  assert(ledger.pendingSeconds() == 6300);
  std::puts("Historical 105-minute import is idempotent: PASS");
  Snapshot before{ledger.identity(), 100, 500, 1000, true};
  assert(ledger.reserve(before, 1000, true));
  assert(ledger.status() == Status::AwaitingVerification);
  assert(!ledger.reserve(before, 1001, true));
  assert(ledger.pendingSeconds() == 6300);
  uint8_t encoded[Ledger::kEncodedSize];
  assert(ledger.encode(encoded, sizeof(encoded)));
  Ledger rebooted;
  assert(rebooted.decode(encoded, sizeof(encoded)));
  assert(!rebooted.reserve(before, 1001, true));
  Snapshot after{ledger.identity(), 160, 560, 1001, true};
  assert(rebooted.verify(after));
  assert(rebooted.pendingSeconds() == 6240);
  assert(rebooted.verifiedSeconds() == 60);
  assert(!rebooted.verify(after));
  std::puts("Reservation survives reboot and needs scoped cloud readback: PASS");
  assert(!rebooted.bind("other", "book", "local-book", 20709));
  assert(!rebooted.collect(6299999));
  assert(rebooted.pendingSeconds() == 6240);
  assert(!rebooted.reserve(before, 1121, true));
  assert(!rebooted.reserve(before, 999, true));
  assert(!rebooted.reserve(before, 1000, false));
  auto otherAccount = after;
  std::strcpy(otherAccount.identity.account, "other");
  assert(!rebooted.reserve(otherAccount, 1001, true));
  auto otherBook = after;
  std::strcpy(otherBook.identity.book, "other");
  assert(!rebooted.reserve(otherBook, 1001, true));
  assert(rebooted.reserve(after, 1001, true));
  auto wrong = after;
  wrong.bookDaySeconds += 60;
  wrong.accountDaySeconds += 60;
  wrong.sampledAt = 1002;
  wrong.identity.day++;
  assert(!rebooted.verify(wrong));
  wrong.identity = rebooted.identity();
  wrong.complete = false;
  assert(!rebooted.verify(wrong));
  wrong.complete = true;
  wrong.accountDaySeconds++;
  assert(!rebooted.verify(wrong));
  wrong.accountDaySeconds--;
  wrong.bookDaySeconds--;
  assert(!rebooted.verify(wrong));
  assert(rebooted.status() == Status::AwaitingVerification);
  for (size_t i = 0; i < sizeof(encoded); ++i) {
    encoded[i] ^= 1;
    assert(!rebooted.decode(encoded, sizeof(encoded)));
    assert(rebooted.status() == Status::AwaitingVerification);
    encoded[i] ^= 1;
  }
  assert(!rebooted.decode(encoded, sizeof(encoded) - 1));
  assert(!rebooted.bind("../account", "book", "source", 20709));
  std::puts("Wrong scope, stale reads, incomplete data, corrections and corruption fail closed: PASS");
  Disk disk;
  Journal journal(disk);
  assert(journal.open("account", "book", "local-book", 20709));
  assert(journal.collect(6300000));
  const auto size = disk.bytes.size();
  assert(journal.collect(6300000));
  assert(disk.bytes.size() == size);
  disk.fail = true;
  assert(!journal.reserve(before, 1000, true));
  assert(!journal.ready());
  disk.fail = false;
  Journal restarted(disk);
  assert(restarted.open("account", "book", "local-book", 20709));
  assert(restarted.reserve(before, 1000, true));
  Journal reserved(disk);
  assert(reserved.open("account", "book", "local-book", 20709));
  assert(!reserved.reserve(before, 1001, true));
  disk.torn = true;
  assert(!reserved.verify(after));
  Journal damaged(disk);
  assert(!damaged.open("account", "book", "local-book", 20709));
  assert(!damaged.ready());
  assert(!damaged.reserve(before, 1002, true));
  std::puts("Durable reservation, write failure and torn journal never authorize retransmission: PASS");
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("crossmux-time-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(directory));
  const auto path = directory / "day.wrtl";
  {
    FileDisk file(path);
    Journal original(file);
    assert(original.open("account", "book", "local-book", 20709));
    assert(original.collect(6300000));
    assert(original.reserve(before, 1000, true));
  }
  {
    FileDisk file(path);
    Journal restored(file);
    assert(restored.open("account", "book", "local-book", 20709));
    assert(restored.ledger().pendingSeconds() == 6300);
    assert(!restored.reserve(before, 1001, true));
    assert(restored.verify(after));
  }
  {
    FileDisk file(path);
    Journal restored(file);
    assert(restored.open("account", "book", "local-book", 20709));
    assert(restored.ledger().verifiedSeconds() == 60);
    assert(restored.ledger().pendingSeconds() == 6240);
    assert(restored.collect(6300000));
    assert(restored.ledger().pendingSeconds() == 6240);
    Journal wrongBook(file);
    assert(!wrongBook.open("account", "another-book", "local-book", 20709));
  }
  assert(std::filesystem::remove(path));
  assert(std::filesystem::remove(directory));
  std::puts("Real host-file close/reopen retains pending and verified counters: PASS");
}
