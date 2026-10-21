#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
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
inline bool failOpen = false, failSeek = false;
inline size_t opens = 0, reads = 0, seeks = 0, bytesRead = 0, writes = 0, existsChecks = 0;
inline size_t activeHandles = 0, peakHandles = 0, liveStates = 0, shortReadOn = 0;
inline bool writeWithOtherHandle = false;
inline void reset() {
  files.clear();
  directories = {"/"};
  failRename = false;
  freeBytes = 8 * 1024 * 1024;
  failSpace = false;
  failOpen = failSeek = false;
  opens = reads = seeks = bytesRead = writes = existsChecks = 0;
  activeHandles = peakHandles = liveStates = shortReadOn = 0;
  writeWithOtherHandle = false;
}
}  // namespace fakeStorage
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string p) {
    ++fakeStorage::opens;
    if (!fakeStorage::failOpen && (fakeStorage::files.count(p) || fakeStorage::directories.count(p))) {
      impl = std::make_unique<Impl>(std::move(p));
      fakeStorage::peakHandles = std::max(fakeStorage::peakHandles, ++fakeStorage::activeHandles);
    }
  }
  ~HalFile() { close(); }
  HalFile(HalFile&& other) { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) {
    if (this == &other) return *this;
    close();
    impl = std::move(other.impl);
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  bool close() {
    if (impl && impl->opened) {
      --fakeStorage::activeHandles;
      impl->opened = false;
    }
    return true;
  }
  explicit operator bool() const {
    return impl && impl->opened && (fakeStorage::files.count(impl->path) || fakeStorage::directories.count(impl->path));
  }
  bool isDirectory() const { return impl && fakeStorage::directories.count(impl->path); }
  uint64_t fileSize64() const {
    if (!impl) return 0;
    auto found = fakeStorage::files.find(impl->path);
    return found == fakeStorage::files.end() ? 0 : found->second.size();
  }
  bool seek64(uint64_t at) {
    ++fakeStorage::seeks;
    if (!impl || !impl->opened || fakeStorage::failSeek) return false;
    impl->offset = at;
    return at <= fileSize64();
  }
  int read(uint8_t* out, size_t n) {
    ++fakeStorage::reads;
    if (!impl || !impl->opened) return -1;
    auto found = fakeStorage::files.find(impl->path);
    if (found == fakeStorage::files.end() || impl->offset > found->second.size()) return -1;
    n = std::min(n, found->second.size() - impl->offset);
    if (n && fakeStorage::reads == fakeStorage::shortReadOn) --n;
    std::memcpy(out, found->second.data() + impl->offset, n);
    impl->offset += n;
    fakeStorage::bytesRead += n;
    return static_cast<int>(n);
  }
  size_t write(const uint8_t* data, size_t n) {
    ++fakeStorage::writes;
    if (fakeStorage::activeHandles > 1) fakeStorage::writeWithOtherHandle = true;
    if (!impl || !impl->opened) return 0;
    auto& bytes = fakeStorage::files[impl->path];
    bytes.insert(bytes.end(), data, data + n);
    return n;
  }
  void flush() {}
  bool getName(char* out, size_t n) {
    if (!impl) return false;
    const auto name = impl->path.substr(impl->path.find_last_of('/') + 1);
    if (name.size() >= n) return false;
    std::memcpy(out, name.c_str(), name.size() + 1);
    return true;
  }
  HalFile openNextFile() {
    if (!impl) return {};
    const auto prefix = impl->path == "/" ? impl->path : impl->path + "/";
    size_t index = 0;
    for (const auto& entry : fakeStorage::files) {
      if (entry.first.compare(0, prefix.size(), prefix) || entry.first.find('/', prefix.size()) != std::string::npos)
        continue;
      if (index++ == impl->next) {
        ++impl->next;
        return HalFile(entry.first);
      }
    }
    return {};
  }

 private:
  // Match HAL's out-of-line file state rather than inflating firmware workspaces
  // by embedding the host std::string and directory cursor in every handle.
  struct Impl {
    explicit Impl(std::string p) : path(std::move(p)) { ++fakeStorage::liveStates; }
    ~Impl() { --fakeStorage::liveStates; }
    std::string path;
    size_t offset = 0, next = 0;
    bool opened = true;
  };
  std::unique_ptr<Impl> impl;
};
class StorageStub {
 public:
  bool ready() { return true; }
  bool getSpace(uint64_t& total, uint64_t& free) {
    total = 16 * 1024 * 1024;
    free = fakeStorage::freeBytes;
    return !fakeStorage::failSpace;
  }
  bool exists(const char* p) {
    ++fakeStorage::existsChecks;
    return fakeStorage::files.count(p) || fakeStorage::directories.count(p);
  }
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
