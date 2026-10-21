"""Run production recent-book registration/persistence across failures and reader exit."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/tests"))
from test_reading_ui_regressions import method


def run(compiler, arduinojson):
    store = (ROOT / "src/RecentBooksStore.cpp").read_text(encoding="utf-8")
    reader = (ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")
    header = (ROOT / "src/activities/reader/ReaderActivity.h").read_text(encoding="utf-8")
    functions = [method(store, signature) for signature in (
        "void RecentBooksStore::toJson(", "bool RecentBooksStore::fromJson(",
        "bool RecentBooksStore::addBook(", "bool RecentBooksStore::removeByPath(",
        "bool RecentBooksStore::isMissing(", "bool RecentBooksStore::pruneMissing(",
    )]
    for signature in ("bool RecentBooksStore::saveToFile(", "bool RecentBooksStore::retryPendingSave("):
        if signature in store:
            functions.append(method(store, signature))
    registration_flag = re.search(r"  bool book(?:Remembered|Registered) = false;", header)[0]
    persist_stub = r'''
#pragma once
#include <ArduinoJson.h>
#include <mutex>
unsigned long millis();
bool writeRecentFile(const JsonDocument&);
class PersistableStoreBase {
 protected:
  mutable std::mutex storeMutex;
  static bool writeDocToFile(const char*, const JsonDocument& doc) { return writeRecentFile(doc); }
};
template<class T> class PersistableStore : public PersistableStoreBase {
 public:
  static T& getInstance() { static T instance; return instance; }
  bool saveToFile() const {
    std::lock_guard<std::mutex> lock(storeMutex);
    JsonDocument doc; static_cast<const T*>(this)->toJson(doc);
    return writeDocToFile(T::getFilePath(), doc);
  }
};
'''
    program = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include "RecentBooksStore.h"
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(2); } } while (0)
uint32_t clockMs = 0;
unsigned long millis() { return clockMs; }
std::string diskPath;
bool failWrites = false;
unsigned attempts = 0, openEvents = 0, stateWrites = 0;
bool writeRecentFile(const JsonDocument& doc) {
  ++attempts;
  if (failWrites) return false;
  std::string json; serializeJson(doc, json);
  std::ofstream file(diskPath, std::ios::binary); file << json;
  return file.good();
}
JsonDocument saved() {
  std::ifstream file(diskPath, std::ios::binary);
  std::string json{std::istreambuf_iterator<char>(file), {}};
  JsonDocument doc; CHECK(!deserializeJson(doc, json)); return doc;
}
struct { bool exists(const char*) const { return true; } } Storage;
struct { std::string openEpubPath; bool saveToFile() { ++stateWrites; return true; } } APP_STATE;
namespace pluginevents {
struct Var { const char* name; const char* value; };
enum class Event { ReaderOpen };
void emit(Event, const Var*, int) { ++openEvents; }
}
struct ReaderActivity {
  std::string bookPath = "/WeRead/second.epub";
  std::atomic<bool> pageRendered{false};
''' + registration_flag + r'''
  std::string getBookTitle() const { return "Second"; }
  std::string getBookAuthor() const { return "Author"; }
  std::string getBookThumbBmpPath() const { return "/.crosspoint/second/thumb_[HEIGHT].bmp"; }
  void rememberBookOnceRendered();
};
// The pre-fix implementation has no retry hook: retain that behavior for the red run.
template<class T> void poll(T& store, bool force = false) {
  if constexpr (requires { store.retryPendingSave(force); }) store.retryPendingSave(force);
}
void reset() {
  failWrites = false; clockMs = 0;
  JsonDocument empty; CHECK(RECENT_BOOKS.fromJson(empty.as<JsonVariantConst>()));
  CHECK(RECENT_BOOKS.addBook("/WeRead/first.epub", "First", "", ""));
  attempts = openEvents = stateWrites = 0;
}
''' + "\n".join(functions) + "\n" + method(reader, "void ReaderActivity::rememberBookOnceRendered(") + r'''
int main(int argc, char** argv) {
  CHECK(argc == 2); diskPath = argv[1];
  reset();
  {
    ReaderActivity reader;
    reader.rememberBookOnceRendered(); CHECK(attempts == 0 && openEvents == 0);
    failWrites = true; reader.pageRendered = true; reader.rememberBookOnceRendered();
    CHECK(RECENT_BOOKS.getCount() == 2 && saved()["books"].size() == 1);
    CHECK(attempts == 1 && openEvents == 1 && stateWrites == 1);
    for (int i = 0; i < 100; ++i) reader.rememberBookOnceRendered();
    CHECK(openEvents == 1 && attempts == 1);
  } // The reader is gone; the pending save must survive on Home.
  failWrites = false; clockMs = 4999; poll(RECENT_BOOKS); CHECK(attempts == 1);
  clockMs = 5000; poll(RECENT_BOOKS);
  CHECK(saved()["books"].size() == 2);
  CHECK(saved()["books"][0]["title"] == "Second");
  CHECK(openEvents == 1 && stateWrites == 1);
  auto reboot = saved(); CHECK(RECENT_BOOKS.fromJson(reboot.as<JsonVariantConst>()));
  CHECK(RECENT_BOOKS.getCount() == 2 && RECENT_BOOKS.getBooks()[0].title == "Second");
  const auto writesAfterSuccess = attempts; clockMs = 60000; poll(RECENT_BOOKS);
  CHECK(attempts == writesAfterSuccess);

  reset(); failWrites = true;
  CHECK(!RECENT_BOOKS.addBook("/WeRead/second.epub", "Second", "", ""));
  CHECK(RECENT_BOOKS.removeByPath("/WeRead/second.epub"));
  failWrites = false; clockMs = 5000; poll(RECENT_BOOKS);
  CHECK(saved()["books"].size() == 1 && saved()["books"][0]["title"] == "First");

  reset(); failWrites = true;
  CHECK(!RECENT_BOOKS.addBook("/WeRead/second.epub", "Second", "", ""));
  clockMs = 1; poll(RECENT_BOOKS, true); CHECK(attempts == 2);
  failWrites = false; clockMs = 2; poll(RECENT_BOOKS, true);
  CHECK(saved()["books"].size() == 2);

  reset(); clockMs = UINT32_MAX - 1000; failWrites = true;
  CHECK(!RECENT_BOOKS.addBook("/WeRead/second.epub", "Second", "", ""));
  failWrites = false; clockMs = 999; poll(RECENT_BOOKS); CHECK(attempts == 1);
  clockMs = 4000; poll(RECENT_BOOKS); CHECK(saved()["books"].size() == 2);
  std::puts("Recent-book retry survives reader exit/reboot, preserves removal, throttles failures and wraps millis safely.");
}
'''
    with tempfile.TemporaryDirectory(prefix="recent-books-") as directory:
        path = Path(directory)
        (path / "PersistableStore.h").write_text(persist_stub, encoding="utf-8")
        (path / "check.cpp").write_text(program, encoding="utf-8")
        binary = path / "check.exe"
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-pthread",
                        "-I" + str(path), "-I" + str(ROOT / "src"), "-I" + arduinojson,
                        str(path / "check.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(path / "recent.json")], check=True)

    manager = method((ROOT / "src/activities/ActivityManager.cpp").read_text(encoding="utf-8"),
                     "void ActivityManager::loop(")
    exclusive = manager.index("requiresExclusiveStorageLoop()")
    retry = manager.index("RECENT_BOOKS.retryPendingSave()")
    assert exclusive < manager.index("return;", exclusive) < retry
    assert "RECENT_BOOKS.retryPendingSave(true)" in method(reader, "void ReaderActivity::onExit(")
    assert "RECENT_BOOKS.retryPendingSave(true)" in method(
        (ROOT / "src/main.cpp").read_text(encoding="utf-8"), "void enterDeepSleep(")


if __name__ == "__main__":
    run(*sys.argv[1:])
