#include "RecentBooksStore.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "util/BookCacheUtils.h"

bool RecentBooksStore::saveToFile() const {
  // Update the dirty flag under the snapshot/write lock so an older successful
  // save cannot clear a newer failure from another task.
  std::lock_guard<std::mutex> lock(storeMutex);
  JsonDocument doc;
  toJson(doc);
  const bool saved = writeDocToFile(getFilePath(), doc);
  lastSaveAttemptMs.store(static_cast<uint32_t>(millis()));
  savePending.store(!saved);
  return saved;
}

bool RecentBooksStore::retryPendingSave(const bool force) const {
  if (!savePending.load()) return true;
  constexpr uint32_t retryIntervalMs = 5000;
  const uint32_t elapsed = static_cast<uint32_t>(millis()) - lastSaveAttemptMs.load();
  if (!force && elapsed < retryIntervalMs) return false;
  return saveToFile();
}

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'books' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  recentBooks.reserve(std::min(arr.size(), static_cast<size_t>(MAX_RECENT_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_RECENT_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", getCount());
  return true;
}

bool RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    // Move the existing entry to the front instead of copying it. The reader calls this after its first
    // render, so fresh string buffers would land among reader allocations and outlive them, splitting the
    // largest free block once the reader exits.
    std::rotate(recentBooks.begin(), it, it + 1);
    RecentBook& book = recentBooks.front();
    if (book.title != title) book.title = title;
    if (book.author != author) book.author = author;
    if (book.coverBmpPath != coverBmpPath) book.coverBmpPath = coverBmpPath;
  } else {
    recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});
    if (recentBooks.size() > MAX_RECENT_BOOKS) {
      recentBooks.resize(MAX_RECENT_BOOKS);
    }
  }

  return saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    saveToFile();
  }
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

bool RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return true;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  return saveToFile();
}

bool RecentBooksStore::updatePathPrefix(const std::string& oldPrefix, const std::string& newPrefix) {
  bool changed = false;
  for (auto& book : recentBooks) {
    if (!FsHelpers::isSameOrDescendantPath(book.path, oldPrefix)) continue;
    const std::string oldPath = book.path;
    const std::string oldCachePath = bookCachePath(oldPath);
    book.path = FsHelpers::rebasePath(oldPath, oldPrefix, newPrefix);
    const std::string newCachePath = bookCachePath(book.path);
    if (!oldCachePath.empty() && book.coverBmpPath.rfind(oldCachePath, 0) == 0) {
      book.coverBmpPath = newCachePath + book.coverBmpPath.substr(oldCachePath.size());
    }
    changed = true;
  }
  return !changed || saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}
