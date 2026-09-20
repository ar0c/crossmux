"""Compile production time POST + payload code with offline platform boundaries.

No device/network access. Crypto is a deterministic SDK stand-in here; these
tests check session lifetime, serialized fields and single-send behavior, not
cryptographic correctness or cloud credit. Real signing vectors live in Inklet.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    # Production functions have their outer closing brace in column zero.
    return source[start:source.index("\n}\n", start) + 3]


class TimeContextTest(unittest.TestCase):
    def test_entry_and_report_keep_one_context_across_seconds(self):
        source = (ROOT / "lib/WeReadWebApi/src/WeReadClient.cpp").read_text(encoding="utf-8")
        header = (ROOT / "lib/WeReadWebApi/src/WeReadDeviceTimeTransport.h").read_text(encoding="utf-8")
        # Keep the production declaration; replace only platform/storage/query
        # dependencies below. Expose state to seed an already-prepared fixture.
        header = "\n".join(line for line in header.splitlines()
                           if not line.startswith(("#include", "#pragma")))
        header = header.replace(" private:", " public:")
        helpers = source[source.index("bool appendText("):source.index("bool sha256Hex(")]
        helpers += function(source, "bool isSafeProtocolToken(")
        for name in ["bool appendEncodedId(", "bool appendProgressQuery(", "bool makeProgressBody("]:
            helpers += function(source, name)
        methods = source[source.index("DeviceTimeTransport::~DeviceTimeTransport()"):source.index(
            "WeReadTime::TimeTransport::Read DeviceTimeTransport::prepare(")]
        methods += source[source.index("WeReadTime::TimeTransport::Write DeviceTimeTransport::enter()"):source.index(
            "}  // namespace WeReadClient", source.index("WeReadTime::TimeTransport::Write DeviceTimeTransport::enter()"))]
        prelude = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "WeReadTimeTransaction.h"
#include "WeReadTimeDiagnostic.h"
#include "WeReadProtocol.h"
namespace WeReadTimeCloud { struct Query { void clear() {} }; }
namespace WeReadStore {
struct Session { char vid[64] = {}; void clear() { vid[0] = 0; } };
struct TocRecord { char chapterUid[64] = {}, title[128] = {}; uint32_t chapterIdx = 0; };
}
static uint32_t clockSeconds = 1000, clockMillis = 100000;
namespace TimeUtils { uint32_t getCurrentValidTimestamp() { return clockSeconds; } }
uint32_t millis() { return clockMillis; }
long random(long, long) { return 17; }
bool hashFails = false;
bool md5Hex(const uint8_t*, size_t, char out[33]) {
  if (hashFails) return false;
  std::memset(out, 'a', 32); out[32] = 0; return true;
}
bool sha256Hex(const char*, char* out, size_t size) {
  if (size < 65) return false;
  std::memset(out, 'b', 64); out[64] = 0; return true;
}
constexpr const char* kUserAgent = "Fixture offline platform";
#define LOG_INF(...) ((void)0)
'''
        boundary = r'''
namespace WeReadClient {
enum class Error { Ok, Protocol, Network };
struct ResponseSink {
  void* ctx;
  bool (*reset)(void*);
  bool (*write)(void*, const uint8_t*, size_t);
  bool (*finish)(void*);
  Error writeError;
};
bool noOpFinish(void*) { return true; }
std::vector<std::string> requests;
bool networkFails = false;
Error requestOnce(const char* method, const char* path, const uint8_t* body, size_t length,
                  WeReadStore::Session*, const char* referer, ResponseSink& sink, int& status,
                  char*, size_t, char*, size_t, uint8_t*, size_t, void*, bool) {
  assert(std::string(method) == "POST" && std::string(path) == "/web/book/read");
  assert(std::string(referer) == "https://weread.qq.com/web/reader/fixture");
  requests.emplace_back(reinterpret_cast<const char*>(body), length);
  if (networkFails) return Error::Network;
  status = 200;
  const char ack[] = "{\"succ\":1}";
  assert(sink.reset(sink.ctx));
  assert(sink.write(sink.ctx, reinterpret_cast<const uint8_t*>(ack), sizeof(ack)-1));
  assert(sink.finish(sink.ctx));
  return Error::Ok;
}
// Preparation/cloud reads are outside this POST-path test and never executed.
WeReadTime::TimeTransport::Read DeviceTimeTransport::prepare(const WeReadTime::Identity&) { std::abort(); }
WeReadTime::TimeTransport::Read DeviceTimeTransport::snapshot(WeReadTime::AccountSnapshot&) { std::abort(); }
'''
        main = r'''
} // namespace WeReadClient
std::string field(const std::string& json, const std::string& name) {
  const auto prefix = "\"" + name + "\":\"";
  const auto at = json.find(prefix);
  assert(at != std::string::npos);
  const auto start = at + prefix.size();
  return json.substr(start, json.find('"', start)-start);
}
void ready(WeReadClient::DeviceTimeTransport& device, const char* pc) {
  device.reset();
  std::strcpy(device.session_.vid, "123");
  std::strcpy(device.identity_.account, "123");
  std::strcpy(device.identity_.book, "456");
  std::strcpy(device.chapter_.chapterUid, "42");
  std::strcpy(device.chapter_.title, "Chapter");
  std::strcpy(device.ps_, "fixture-ps");
  std::strcpy(device.token_, "fixture-token");
  std::strcpy(device.pc_, pc);
  device.remote_.percent = 37;
  device.remote_.chapterOffset = 900;
  device.preparedAt_ = clockSeconds;
  device.preparedMs_ = clockMillis;
  device.referer_ = "https://weread.qq.com/web/reader/fixture";
  device.phase_ = WeReadClient::DeviceTimeTransport::Phase::Ready;
}
int main() {
  using namespace WeReadClient;
  using Write = WeReadTime::TimeTransport::Write;
  DeviceTimeTransport device;
  using D = WeReadTime::Diagnostic::Stage;
  using N = WeReadHttpClient::NetworkDiagnostic::Stage;
  for (auto stage : {D::ReaderRequest, D::ProgressRequest, D::Login, D::Report, D::CloudStats}) {
    device.reset(); device.phase_ = DeviceTimeTransport::Phase::Failed;
    device.diagnostic_ = {stage, static_cast<int>(Error::Network), -1};
    device.diagnostic_.network.stage = N::Open;
    assert(device.retryablePreparation() == (stage == D::ReaderRequest || stage == D::ProgressRequest));
    device.diagnostic_.network.verify = 1; assert(!device.retryablePreparation());
    device.diagnostic_.network.verify = 0; device.reportEvidence_.attempted = true;
    assert(!device.retryablePreparation());
    device.reportEvidence_.attempted = false; device.diagnostic_.error = static_cast<int>(Error::Protocol);
    assert(!device.retryablePreparation());
  }
  for (const char* initial : {"", "0", "existing-pc"}) {
    for (const unsigned gap : {0U, 1U, 3U, 29U}) {
      requests.clear(); ready(device, initial);
      assert(device.enter() == Write::Accepted);
      clockSeconds += gap; clockMillis += gap * 1000;
      assert(device.report(60) == Write::Accepted);
      assert(requests.size() == 2);
      assert(field(requests[0], "pc") == field(requests[1], "pc"));
      assert(field(requests[0], "ps") == field(requests[1], "ps"));
      if (*initial && std::strcmp(initial, "0")) assert(field(requests[0], "pc") == initial);
      assert(requests[0].find("\"rt\"") == std::string::npos);
      assert(requests[1].find("\"rt\":60") != std::string::npos);
      assert(device.report(60) == Write::Unknown && requests.size() == 2);
      const auto old = field(requests[0], "pc");
      device.reset(); assert(!device.pc_[0]);
      if (!*initial) {
        ++clockSeconds; clockMillis += 1000; ready(device, "");
        assert(device.enter() == Write::Accepted);
        assert(field(requests.back(), "pc") != old);
      }
    }
  }
  requests.clear(); ready(device, ""); hashFails = true;
  assert(device.enter() == Write::Unknown && requests.empty()); hashFails = false;
  ready(device, ""); assert(device.enter() == Write::Accepted);
  networkFails = true;
  assert(device.report(60) == Write::Unknown);
  assert(device.report(60) == Write::Unknown && requests.size() == 2);
  networkFails = false;
  requests.clear(); ready(device, ""); assert(device.enter() == Write::Accepted);
  clockSeconds += 31; clockMillis += 31000;
  assert(device.report(60) == Write::Unknown && requests.size() == 1);
  puts("PASS production POST/body: stable pc, reset, stale context, SDK failure, no retry; HTTP is fake");
}
'''
        with tempfile.TemporaryDirectory(prefix="weread-context-") as temporary:
            path = Path(temporary)
            cpp = path / "context.cpp"
            exe = path / ("context.exe" if os.name == "nt" else "context")
            cpp.write_text("\n".join([prelude, header, helpers, boundary, methods, main]), encoding="utf-8")
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "lib/WeReadWebApi/src"), "-I" + str(ROOT / "lib/JsonParser"), str(cpp),
                str(ROOT / "lib/WeReadWebApi/src/WeReadProtocol.cpp"),
                str(ROOT / "lib/JsonParser/StreamingJsonParser.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
