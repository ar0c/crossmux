#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "WeReadExternalTime.h"
#include "WeReadHandoverManifest.h"

struct File {
  std::string bytes;
  size_t at = 0;
  int read(uint8_t* out, size_t n) {
    if (at + n > bytes.size()) return -1;
    std::memcpy(out, bytes.data() + at, n);
    at += n;
    return int(n);
  }
  uint64_t fileSize64() const { return bytes.size(); }
  bool seek64(uint64_t pos) {
    if (pos > bytes.size()) return false;
    at = pos;
    return true;
  }
};
std::string readFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file.good());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
  using namespace WeReadTime;
  const char* hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  Identity id;
  std::strcpy(id.account, "a");
  std::strcpy(id.book, "b");
  std::strcpy(id.source, "s");
  id.day = 20709;
  const std::string original =
      "{\n  \"account\": \"a\",\n  \"book\": \"b\",\n  \"receipts\": [\n"
      "    {\n      \"day\": 20709,\n      \"receipt\": \"weread-external-a-s-20709.bin\",\n"
      "      \"sha256\": \"" +
      std::string(hash) +
      "\"\n    }\n  ],\n  \"schema\": 1,\n  \"sender_enabled\": false,\n"
      "  \"source\": \"s\",\n  \"state\": \"prepared\"\n}";
  auto valid = [&](const std::string& data) {
    File file{data};
    HandoverManifest m;
    return m.begin(file, id) && m.receipt(file, id, hash) && m.finish(file, id);
  };
  assert(valid(original));
  {
    const std::string paused = "{\n  \"account\": \"a\",\n  \"book\": \"b\",\n  \"handback_plan_sha256\": \"" +
                               std::string(hash) +
                               "\",\n  \"schema\": 1,\n  \"sender_enabled\": false,\n  \"source\": \"s\",\n  "
                               "\"state\": \"host_probe_paused\"\n}";
    File file{paused};
    HandoverManifest m;
    assert(!m.begin(file, id));
    assert(m.hostPaused(file, id));  // Diagnostic only; cannot authorize reporting.
    auto future = id;
    ++future.day;
    assert(!m.deviceOwnedDay(future));
    auto other = id;
    std::strcpy(other.book, "other");
    assert(!m.hostPaused(file, other));
    for (auto data : {original, paused + "\n", paused.substr(0, paused.size() - 1)}) {
      File invalid{data};
      assert(!m.hostPaused(invalid, id));
    }
    auto changed = paused;
    changed[changed.find(hash)] = 'z';
    File invalid{changed};
    assert(!m.hostPaused(invalid, id));
    assert(!valid(paused));
  }
  {
    File file{original};
    HandoverManifest m;
    auto future = id;
    ++future.day;
    assert(!m.deviceOwnedDay(future));
    assert(m.begin(file, id) && m.receipt(file, id, hash));
    assert(!m.deviceOwnedDay(future));
    assert(m.next(file) == HandoverManifest::Next::End);
    assert(m.next(file) == HandoverManifest::Next::End);  // Peek never consumes the tail.
    assert(m.finish(file, id));
    assert(m.deviceOwnedDay(future));
    assert(!m.deviceOwnedDay(id));
    auto other = future;
    std::strcpy(other.account, "other");
    assert(!m.deviceOwnedDay(other));
    other = future;
    std::strcpy(other.source, "other");
    assert(!m.deviceOwnedDay(other));
    other = future;
    std::strcpy(other.book, "other");
    assert(!m.deviceOwnedDay(other));
    assert(m.next(file) == HandoverManifest::Next::Invalid);
  }
  assert(!valid(original + "\n"));
  assert(!valid(original.substr(0, original.size() - 1)));
  for (const std::string field : {"prepared", "false", "20709", "sha256", "\"account\""}) {
    auto changed = original;
    changed[changed.find(field)] = 'X';
    assert(!valid(changed));
  }
  {
    File file{original};
    HandoverManifest m;
    assert(m.begin(file, id));
    auto other = id;
    std::strcpy(other.book, "other");
    assert(!m.receipt(file, other, hash));
  }
  {
    File file{original};
    HandoverManifest m;
    assert(m.begin(file, id) && m.receipt(file, id, hash));
    assert(!m.receipt(file, id, hash));  // No duplicate day.
  }
  if (argc > 1) {
    assert(argc >= 4 && (argc - 2) % 2 == 0);
    File file{readFile(argv[1])};
    HandoverManifest manifest;
    ExternalTime receipt;
    for (int i = 2; i < argc; i += 2) {
      const auto bytes = readFile(argv[i]);
      assert(receipt.decode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
      if (i == 2) assert(manifest.begin(file, receipt.identity));
      assert(manifest.receipt(file, receipt.identity, argv[i + 1]));
      assert(manifest.next(file) == (i + 2 == argc ? HandoverManifest::Next::End : HandoverManifest::Next::More));
    }
    assert(manifest.finish(file, receipt.identity));
    std::cout << "Actual prepared manifest and receipt hashes match firmware codec PASS\n";
  }
  std::cout << "Handover identity, hash, duplicate day, schema and truncation tests PASS\n";
}
