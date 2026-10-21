#include <BoardConfig.h>
#include <HalStorage.h>

#include "ScreenDiagnosticExport.h"
#ifndef SIMULATOR
#include <XteinkDetect.h>
#endif
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#ifndef SIMULATOR
freeink::XteinkDisplayProbeDiag cachedProbe;
unsigned cacheReads = 0;
// Only the cache accessor is linked. Any accidental call to a probing function
// would fail to link, and the simulator must not depend on this accessor.
const freeink::XteinkDisplayProbeDiag& freeink::getXteinkDisplayProbeDiag() {
  ++cacheReads;
  return cachedProbe;
}
#endif

void hardwareDiagnostics() {
  fixture::reset();
  ScreenDiagnosticExport::Info info;
  info.width = 8;
  info.height = 8;
  const uint8_t frame[8]{};
  info.hardware = HalDisplayDiagnostics::readCached();
  assert(!info.hardware.valid);
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::Saved);
  const auto unprobed = fixture::files.at(ScreenDiagnosticExport::kPath);
  assert(unprobed.find("valid: 0\n") != std::string::npos);
  assert(unprobed.find("probe_status: NotProbed\n") != std::string::npos);
#ifndef SIMULATOR
  // Deliberately differs from the default driver and includes 00/FF and leading
  // zeros. Check every byte, including MTP's final byte, at the SD boundary.
  BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8179;
  BoardConfig::ACTIVE.displayControllerVariant = 0xAB;
  cachedProbe.valid = true;
  const uint8_t ver[5] = {0x00, 0xFF, 0x01, 0xA5, 0x80};
  std::memcpy(cachedProbe.ver, ver, sizeof(ver));
  cachedProbe.flg = 0x13;
  cachedProbe.verdict = static_cast<uint8_t>(freeink::DisplayControllerVerdict::Uc81xxConfirmed);
  cachedProbe.promoted = true;
  cachedProbe.mtpValid = true;
  for (size_t i = 0; i < sizeof(cachedProbe.mtp); ++i) cachedProbe.mtp[i] = uint8_t(i);
  cachedProbe.mtp[47] = 0xFF;
  info.hardware = HalDisplayDiagnostics::readCached();
  assert(cacheReads == 2);
  // The export consumes the captured snapshot, not mutable defaults/current cache.
  BoardConfig::ACTIVE = {};
  cachedProbe = {};
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::Saved);
  assert(cacheReads == 2);
  const auto& report = fixture::files.at(ScreenDiagnosticExport::kPath);
  for (const char* expected :
       {"BoardConfig::ACTIVE.displayController: 7\n", "active_driver: UC8179\n",
        "BoardConfig::ACTIVE.displayControllerVariant: 0xAB\n", "valid: 1\n", "flg: 0x13\n", "verdict: 1\n",
        "probe_status: Uc81xxConfirmed\n", "promoted: 1\n", "mtpValid: 1\n", "ver[5]: 00 FF 01 A5 80\n",
        "mtp[48]: 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F "
        "10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F "
        "20 21 22 23 24 25 26 27 28 29 2A 2B 2C 2D 2E FF\n"}) {
    assert(report.find(expected) != std::string::npos);
  }
  for (uint8_t verdict : {0, 2, 255}) {
    cachedProbe.valid = true;
    cachedProbe.verdict = verdict;
    info.hardware = HalDisplayDiagnostics::readCached();
    assert(!info.hardware.promoted && !info.hardware.mtpValid);
    assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::Saved);
    const char* label = verdict == 0 ? "PrimaryAssumed" : verdict == 2 ? "Inconclusive" : "Unknown";
    assert(fixture::files.at(ScreenDiagnosticExport::kPath).find(std::string("probe_status: ") + label + "\n") !=
           std::string::npos);
  }
  cachedProbe.valid = false;
  cachedProbe.ver[4] = 0xA5;
  cachedProbe.mtp[47] = 0xFF;
  info.hardware = HalDisplayDiagnostics::readCached();
  assert(!info.hardware.valid && !info.hardware.mtpValid);
  assert(info.hardware.ver[4] == 0xA5 && info.hardware.mtp[47] == 0xFF);
  assert(std::strcmp(info.hardware.verdictName, "NotProbed") == 0);
#endif
}

std::vector<uint8_t> bitmap(const std::string& report) {
  const auto start = report.find("base64,") + 7;
  const auto end = report.find('"', start);
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> result;
  uint32_t bits = 0;
  unsigned count = 0;
  for (size_t i = start; i < end && report[i] != '='; ++i) {
    const auto n = alphabet.find(report[i]);
    assert(n != std::string::npos);
    bits = (bits << 6) | unsigned(n);
    count += 6;
    if (count >= 8) {
      count -= 8;
      result.push_back(uint8_t(bits >> count));
    }
  }
  return result;
}
int main(int argc, char** argv) {
  hardwareDiagnostics();
  fixture::reset();
  ScreenDiagnosticExport::Info info{};
  info.firmware = "test-ar0c";
  info.model = "test-board";
  info.width = 8;
  info.height = 8;
  const uint8_t frame[8] = {0x80, 0x04, 0x20, 0x40, 0x03, 0x28, 0x10, 1};
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::Saved);
  const auto& report = fixture::files.at(ScreenDiagnosticExport::kPath);
  assert(report.find("test-ar0c") != std::string::npos);
  assert(report.find("data:image/bmp;base64,") != std::string::npos);
  assert(report.find("</html>") != std::string::npos);
  assert(fixture::files.size() == 1);
  fixture::files["/WeReadSync/last-sync-diagnostic.json"] =
      "{\"schema\":3,\"stage\":\"reader_request\",\"http\":-1,\"error\":2,\"retries\":3,"
      "\"network\":{\"stage\":5,\"tls\":-123},\"cookie\":\"PRIVATE-COOKIE\",\"token\":99999999}";
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::Saved);
  const auto& diagnostic = fixture::files.at(ScreenDiagnosticExport::kPath);
  assert(diagnostic.find("reader_request") != std::string::npos);
  assert(diagnostic.find("tls: -123") != std::string::npos);
  assert(diagnostic.find("PRIVATE-COOKIE") == std::string::npos);
  assert(diagnostic.find("99999999") == std::string::npos);
  const auto original = fixture::files.at(ScreenDiagnosticExport::kPath);
  fixture::freeBytes = 1;
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::NoSpace);
  assert(fixture::files.at(ScreenDiagnosticExport::kPath) == original);
  fixture::freeBytes = 4 * 1024 * 1024;
  fixture::failWrite = true;
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::StorageError);
  assert(fixture::files.at(ScreenDiagnosticExport::kPath) == original);
  fixture::failWrite = false;
  fixture::failRename = true;
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::StorageError);
  assert(fixture::files.at(ScreenDiagnosticExport::kPath) == original);
  fixture::failRename = false;
  fixture::failClose = true;
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::StorageError);
  assert(fixture::files.at(ScreenDiagnosticExport::kPath) == original);
  fixture::failClose = false;
  fixture::failRenameAt = fixture::renameCalls + 2;  // Fail promotion, then permit rollback.
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::StorageError);
  assert(fixture::files.at(ScreenDiagnosticExport::kPath) == original);
  fixture::failRenameAt = 0;
  fixture::files["/Diagnostics/Screen/latest.bak"] = original;
  fixture::files.erase(ScreenDiagnosticExport::kPath);  // Power loss after previous-report rename.
  fixture::failWrite = true;
  assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::StorageError);
  assert(fixture::files.at(ScreenDiagnosticExport::kPath) == original);
  fixture::failWrite = false;
  assert(ScreenDiagnosticExport::save(info, nullptr, sizeof(frame)) == ScreenDiagnosticExport::Result::InvalidFrame);
  assert(ScreenDiagnosticExport::save(info, frame, 1) == ScreenDiagnosticExport::Result::InvalidFrame);
  fixture::files["/Diagnostics/Screen/unrelated.txt"] = "KEEP";
  for (unsigned rotation = 0; rotation < 4; ++rotation) {
    info.orientation = rotation;
    for (bool invert : {false, true}) {
      info.inverted = invert;
      assert(ScreenDiagnosticExport::save(info, frame, sizeof(frame)) == ScreenDiagnosticExport::Result::Saved);
      const auto bmp = bitmap(fixture::files.at(ScreenDiagnosticExport::kPath));
      assert(bmp.size() == 62 + 4 * 8 && bmp[0] == 'B' && bmp[1] == 'M');
      for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
          unsigned px = 0, py = 0;
          if (rotation == 0) {
            px = y;
            py = 7 - x;
          }
          if (rotation == 1) {
            px = 7 - x;
            py = 7 - y;
          }
          if (rotation == 2) {
            px = 7 - y;
            py = x;
          }
          if (rotation == 3) {
            px = x;
            py = y;
          }
          const auto expected = ((frame[py] >> (7 - px)) & 1) ^ invert;
          assert(((bmp[62 + (7 - y) * 4] >> (7 - x)) & 1) == expected);
        }
    }
  }
  assert(fixture::files.at("/Diagnostics/Screen/unrelated.txt") == "KEEP");
  // Non-square high-resolution panels, including row sizes beyond old 68-byte screenshots.
  info.width = 800;
  info.height = 600;
  info.orientation = 0;
  info.inverted = false;
  std::vector<uint8_t> large(800 * 600 / 8, 0xaa);
  assert(ScreenDiagnosticExport::save(info, large.data(), large.size()) == ScreenDiagnosticExport::Result::Saved);
  assert(bitmap(fixture::files.at(ScreenDiagnosticExport::kPath)).size() == 62 + 76 * 800);
  assert(fixture::files.size() == 3);  // latest report, source diagnostic and unrelated file only.
  if (argc == 2) {
    std::ofstream sample(argv[1], std::ios::binary);
    sample << fixture::files.at(ScreenDiagnosticExport::kPath);
  }
  fixture::reset();
  fixture::directories.insert(ScreenDiagnosticExport::kPath);
  assert(ScreenDiagnosticExport::save(info, large.data(), large.size()) ==
         ScreenDiagnosticExport::Result::StorageError);
  assert(fixture::directories.count(ScreenDiagnosticExport::kPath) == 1);
  fixture::reset();
  fixture::files["/WeReadSync/last-sync-diagnostic.json"] =
      "{\"stage\":\"<script>SECRET</script>\",\"cookie\":{\"http\":12345678},\"error\":\"SECRET2\"}";
  info.model = "<script>board</script>";
  assert(ScreenDiagnosticExport::save(info, large.data(), large.size()) == ScreenDiagnosticExport::Result::Saved);
  const auto& safe = fixture::files.at(ScreenDiagnosticExport::kPath);
  assert(safe.find("SECRET") == std::string::npos && safe.find("12345678") == std::string::npos);
  assert(safe.find("<script>") == std::string::npos && safe.find("&lt;script&gt;board") != std::string::npos);
  std::cout << "Screen diagnostic export PASS\n";
}
