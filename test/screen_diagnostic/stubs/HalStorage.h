#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
namespace fixture {
inline std::map<std::string, std::string> files;
inline std::set<std::string> directories;
inline bool failWrite = false, failRename = false, failClose = false;
inline unsigned renameCalls = 0, failRenameAt = 0;
inline uint64_t freeBytes = 4 * 1024 * 1024;
inline void reset() {
  files.clear();
  directories.clear();
  failWrite = failRename = failClose = false;
  renameCalls = failRenameAt = 0;
  freeBytes = 4 * 1024 * 1024;
}
}  // namespace fixture
class HalFile {
 public:
  std::string path;
  size_t offset = 0;
  explicit operator bool() const { return fixture::files.count(path) || fixture::directories.count(path); }
  bool isDirectory() const { return fixture::directories.count(path); }
  uint64_t fileSize64() const { return fixture::files.at(path).size(); }
  size_t write(const uint8_t* bytes, size_t n) {
    if (fixture::failWrite) return 0;
    fixture::files[path].append(reinterpret_cast<const char*>(bytes), n);
    return n;
  }
  int read(void* bytes, size_t n) {
    const auto& data = fixture::files.at(path);
    n = std::min(n, data.size() - offset);
    std::memcpy(bytes, data.data() + offset, n);
    offset += n;
    return int(n);
  }
  bool close() { return !fixture::failClose; }
  void flush() {}
};
class StorageBoundary {
 public:
  bool getSpace(uint64_t& total, uint64_t& free) {
    total = 8 * 1024 * 1024;
    free = fixture::freeBytes;
    return true;
  }
  bool ensureDirectoryExists(const char* p) {
    if (fixture::files.count(p)) return false;
    fixture::directories.insert(p);
    return true;
  }
  bool exists(const char* p) { return fixture::files.count(p) || fixture::directories.count(p); }
  bool openFileForWrite(const char*, const char* p, HalFile& f) {
    if (fixture::directories.count(p)) return false;
    fixture::files[p].clear();
    f.path = p;
    return true;
  }
  bool openFileForRead(const char*, const char* p, HalFile& f) {
    if (!exists(p)) return false;
    f.path = p;
    return true;
  }
  bool remove(const char* p) { return fixture::files.erase(p) == 1; }
  bool rename(const char* from, const char* to) {
    ++fixture::renameCalls;
    if (fixture::failRename || fixture::renameCalls == fixture::failRenameAt || exists(to)) return false;
    if (fixture::directories.erase(from)) {
      fixture::directories.insert(to);
      return true;
    }
    if (!fixture::files.count(from)) return false;
    fixture::files[to] = fixture::files.at(from);
    fixture::files.erase(from);
    return true;
  }
};
inline StorageBoundary Storage;
