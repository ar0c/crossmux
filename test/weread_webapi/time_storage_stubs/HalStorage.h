#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
inline constexpr int O_WRONLY = 1, O_CREAT = 2, O_APPEND = 4;
namespace fakeStorage {
inline std::map<std::string, std::vector<uint8_t>> files;
inline std::set<std::string> directories{"/"};
inline bool failRename = false;
inline uint64_t freeBytes = 8 * 1024 * 1024;
inline bool failSpace = false;
inline void reset() {
  files.clear();
  directories = {"/"};
  failRename = false;
  freeBytes = 8 * 1024 * 1024;
  failSpace = false;
}
}  // namespace fakeStorage
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string p) : path(std::move(p)) {}
  explicit operator bool() const { return fakeStorage::files.count(path) || fakeStorage::directories.count(path); }
  bool isDirectory() const { return fakeStorage::directories.count(path); }
  uint64_t fileSize64() const {
    auto found = fakeStorage::files.find(path);
    return found == fakeStorage::files.end() ? 0 : found->second.size();
  }
  bool seek64(uint64_t at) {
    offset = at;
    return at <= fileSize64();
  }
  int read(uint8_t* out, size_t n) {
    auto found = fakeStorage::files.find(path);
    if (found == fakeStorage::files.end() || offset > found->second.size()) return -1;
    n = std::min(n, found->second.size() - offset);
    std::memcpy(out, found->second.data() + offset, n);
    offset += n;
    return static_cast<int>(n);
  }
  size_t write(const uint8_t* data, size_t n) {
    auto& bytes = fakeStorage::files[path];
    bytes.insert(bytes.end(), data, data + n);
    return n;
  }
  void flush() {}
  bool getName(char* out, size_t n) {
    const auto name = path.substr(path.find_last_of('/') + 1);
    if (name.size() >= n) return false;
    std::memcpy(out, name.c_str(), name.size() + 1);
    return true;
  }
  HalFile openNextFile() {
    const auto prefix = path == "/" ? path : path + "/";
    size_t index = 0;
    for (const auto& entry : fakeStorage::files) {
      if (entry.first.compare(0, prefix.size(), prefix) || entry.first.find('/', prefix.size()) != std::string::npos)
        continue;
      if (index++ == next) {
        ++next;
        return HalFile(entry.first);
      }
    }
    return {};
  }

 private:
  std::string path;
  size_t offset = 0, next = 0;
};
class StorageStub {
 public:
  bool ready() { return true; }
  bool getSpace(uint64_t& total, uint64_t& free) {
    total = 16 * 1024 * 1024;
    free = fakeStorage::freeBytes;
    return !fakeStorage::failSpace;
  }
  bool exists(const char* p) { return fakeStorage::files.count(p) || fakeStorage::directories.count(p); }
  HalFile open(const char* p, int flags = 0) {
    if (flags & O_CREAT) fakeStorage::files.try_emplace(p);
    return HalFile(p);
  }
  bool openFileForRead(const char*, const char* p, HalFile& file) {
    file = open(p);
    return bool(file);
  }
  bool openFileForWrite(const char*, const char* p, HalFile& file) {
    fakeStorage::files[p].clear();
    file = open(p, O_CREAT);
    return bool(file);
  }
  bool ensureDirectoryExists(const char* p) {
    if (fakeStorage::files.count(p)) return false;
    fakeStorage::directories.insert(p);
    return true;
  }
  bool rename(const char* oldPath, const char* newPath) {
    if (fakeStorage::failRename || exists(newPath) || !fakeStorage::files.count(oldPath)) return false;
    fakeStorage::files[newPath] = fakeStorage::files.at(oldPath);
    fakeStorage::files.erase(oldPath);
    return true;
  }
};
inline StorageStub Storage;
