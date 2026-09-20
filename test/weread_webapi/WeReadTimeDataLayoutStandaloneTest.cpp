#include "WeReadTimeDataLayout.h"
#include <cassert>
#include <map>
#include <string>
#include <iostream>
using namespace WeReadTime;
struct Store {
  std::map<std::string, std::string> files;
  bool failDirectory = false, failMove = false, corruptResult = false;
  unsigned moves = 0;
  TimeFileKind kind(const char* path) {
    auto it = files.find(path);
    if (it == files.end()) return TimeFileKind::Missing;
    return it->second == "directory" ? TimeFileKind::Invalid : TimeFileKind::File;
  }
  bool ensureDirectory() { return !failDirectory; }
  bool moveNoReplace(const char* oldPath, const char* newPath) {
    if (failMove || files.count(newPath)) return false;
    ++moves;
    files[newPath] = files.at(oldPath);
    if (!corruptResult) files.erase(oldPath);
    return true;
  }
};
int main() {
  Store s;
  assert(relocateTimeFile(s, kLegacyTimeManifest, kTimeManifest) && s.moves == 0);
  s.files[kLegacyTimeManifest] = "immutable byte-exact accounting";
  assert(relocateTimeFile(s, kLegacyTimeManifest, kTimeManifest));
  assert(s.moves == 1 && s.files.at(kTimeManifest) == "immutable byte-exact accounting");
  assert(relocateTimeFile(s, kLegacyTimeManifest, kTimeManifest) && s.moves == 1);
  s.files[kLegacyTimeManifest] = "conflicting accounting";
  assert(!relocateTimeFile(s, kLegacyTimeManifest, kTimeManifest) && s.moves == 1);
  for (unsigned mode = 0; mode < 4; ++mode) {
    Store f;
    f.files[kLegacyTimeManifest] = mode == 3 ? "directory" : "bytes";
    f.failDirectory = mode == 0; f.failMove = mode == 1; f.corruptResult = mode == 2;
    assert(!relocateTimeFile(f, kLegacyTimeManifest, kTimeManifest));
    assert(f.files.count(kLegacyTimeManifest));
  }
  // A reboot between files continues from the remaining old file, never copying
  // or replacing the already relocated manifest/receipt.
  Store partial;
  partial.files[kTimeManifest] = "manifest";
  partial.files["/receipt"] = "receipt";
  assert(relocateTimeFile(partial, kLegacyTimeManifest, kTimeManifest));
  assert(relocateTimeFile(partial, "/receipt", "/WeReadSync/receipt"));
  assert(partial.moves == 1 && partial.files.at(kTimeManifest) == "manifest");
  std::cout << "Time data layout: byte preservation, restart, conflicts, directories and rename failures PASS\n";
}
