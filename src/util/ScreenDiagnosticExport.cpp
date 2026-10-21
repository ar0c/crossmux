#include "ScreenDiagnosticExport.h"

#include <HalStorage.h>
#include <StreamingJsonParser.h>
#include <WeReadTimeDiagnostic.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace ScreenDiagnosticExport {
namespace {
constexpr const char* kTemp = "/Diagnostics/Screen/latest.tmp";
constexpr const char* kBackup = "/Diagnostics/Screen/latest.bak";
constexpr size_t kLimit = 512 * 1024;

class Writer {
 public:
  explicit Writer(HalFile& file) : file_(file) {}
  bool ok = true;
  size_t size = 0;
  void data(const char* text, size_t n) {
    if (!ok) return;
    if (n > kLimit - size || file_.write(reinterpret_cast<const uint8_t*>(text), n) != n) {
      ok = false;
      return;
    }
    size += n;
  }
  void text(const char* value) { data(value, std::strlen(value)); }
  template <typename... Args>
  void format(const char* pattern, Args... args) {
    char buffer[128];
    const int n = std::snprintf(buffer, sizeof(buffer), pattern, args...);
    if (n <= 0 || size_t(n) >= sizeof(buffer)) {
      ok = false;
    } else {
      data(buffer, size_t(n));
    }
  }
  void escaped(const char* value) {
    if (!value) return;
    // Only firmware/board labels are accepted, bounded even if a caller errs.
    for (size_t i = 0; i < 128 && value[i]; ++i) {
      switch (value[i]) {
        case '&':
          text("&amp;");
          break;
        case '<':
          text("&lt;");
          break;
        case '>':
          text("&gt;");
          break;
        case '"':
          text("&quot;");
          break;
        default:
          data(value + i, 1);
          break;
      }
    }
  }

 private:
  HalFile& file_;
};

class Base64 {
 public:
  explicit Base64(Writer& out) : out_(out) {}
  void byte(uint8_t value) {
    triple_[count_++] = value;
    if (count_ == 3) emit();
  }
  void finish() {
    if (count_) emit();
    flush();
  }

 private:
  void emit() {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint32_t n = (uint32_t(triple_[0]) << 16) | (uint32_t(triple_[1]) << 8) | triple_[2];
    buffer_[used_++] = alphabet[(n >> 18) & 63];
    buffer_[used_++] = alphabet[(n >> 12) & 63];
    buffer_[used_++] = count_ > 1 ? alphabet[(n >> 6) & 63] : '=';
    buffer_[used_++] = count_ > 2 ? alphabet[n & 63] : '=';
    count_ = 0;
    triple_[0] = triple_[1] = triple_[2] = 0;
    if (used_ == sizeof(buffer_)) flush();
  }
  void flush() {
    out_.data(buffer_, used_);
    used_ = 0;
  }
  Writer& out_;
  char buffer_[96]{};  // Bounded SD write buffer; Base64 state stays below the C3 stack budget.
  uint8_t triple_[3]{};
  size_t used_ = 0;
  unsigned count_ = 0;
};

void hexBytes(Writer& out, const uint8_t* bytes, size_t count) {
  constexpr char digits[] = "0123456789ABCDEF";
  for (size_t i = 0; i < count; ++i) {
    const char hex[] = {digits[bytes[i] >> 4], digits[bytes[i] & 0x0F]};
    if (i) {
      out.text(" ");
    }
    out.data(hex, sizeof(hex));
  }
}

void hardwareDiagnostic(Writer& out, const HalDisplayDiagnostics::Snapshot& hw) {
  out.text(
      "<h2>Display controller diagnostics</h2>"
      "<p>Source: cached boot probe (no new probe). The active driver alone does not identify the IC. "
      "valid=0 means no probe result; verdict is meaningful only when valid=1. "
      "mtpValid indicates captured bytes, not a confirmed IC.</p><pre>");
  out.format("BoardConfig::ACTIVE.displayController: %u\nBoardConfig::ACTIVE.displayControllerVariant: 0x%02X\n",
             unsigned(hw.displayController), unsigned(hw.displayControllerVariant));
  out.format("valid: %u\nflg: 0x%02X\nverdict: %u\npromoted: %u\nmtpValid: %u\n", unsigned(hw.valid), unsigned(hw.flg),
             unsigned(hw.verdict), unsigned(hw.promoted), unsigned(hw.mtpValid));
  out.text("active_driver: ");
  out.escaped(hw.controllerName);
  out.text("\nprobe_status: ");
  out.escaped(hw.verdictName);
  out.text("\nver[5]: ");
  hexBytes(out, hw.ver, sizeof(hw.ver));
  out.text("\nmtp[48]: ");
  hexBytes(out, hw.mtp, sizeof(hw.mtp));
  out.text("\n</pre>\n");
}

void put32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}

void image(Writer& out, const Info& info, const uint8_t* framebuffer) {
  const bool portrait = info.orientation == 0 || info.orientation == 2;
  const unsigned width = portrait ? info.height : info.width;
  const unsigned height = portrait ? info.width : info.height;
  const unsigned rowBytes = ((width + 31) / 32) * 4;
  uint8_t header[62]{};
  header[0] = 'B';
  header[1] = 'M';
  put32(header + 2, sizeof(header) + rowBytes * height);
  put32(header + 10, sizeof(header));
  put32(header + 14, 40);
  put32(header + 18, width);
  put32(header + 22, height);
  header[26] = 1;
  header[28] = 1;
  put32(header + 34, rowBytes * height);
  put32(header + 46, 2);
  header[58] = header[59] = header[60] = 255;
  out.text("<img alt=\"Software framebuffer\" src=\"data:image/bmp;base64,");
  Base64 encoded(out);
  for (uint8_t value : header) encoded.byte(value);
  for (unsigned line = 0; line < height && out.ok; ++line) {
    if (info.cooperate && line % 16 == 0) info.cooperate();
    const unsigned y = height - 1 - line;
    uint8_t packed = 0;
    for (unsigned x = 0; x < width; ++x) {
      unsigned px = 0, py = 0;
      switch (info.orientation) {
        case 0:
          px = y;
          py = info.height - 1 - x;
          break;
        case 1:
          px = info.width - 1 - x;
          py = info.height - 1 - y;
          break;
        case 2:
          px = info.width - 1 - y;
          py = x;
          break;
        case 3:
          px = x;
          py = y;
          break;
      }
      const unsigned bit = ((framebuffer[py * (info.width / 8) + px / 8] >> (7 - px % 8)) & 1) ^ info.inverted;
      packed |= uint8_t(bit << (7 - x % 8));
      if (x % 8 == 7) {
        encoded.byte(packed);
        packed = 0;
      }
    }
    if (width % 8) encoded.byte(packed);
    const unsigned pixelBytes = (width + 7) / 8;
    for (unsigned i = pixelBytes; i < rowBytes; ++i) encoded.byte(0);
  }
  encoded.finish();
  out.text("\">\n");
}

bool removeFile(const char* path) {
  if (!Storage.exists(path)) return true;
  {
    HalFile file;
    if (!Storage.openFileForRead("ScreenDiag", path, file) || file.isDirectory()) return false;
  }
  return Storage.remove(path);
}

void syncDiagnostic(Writer& out) {
  out.text("<h2>Last saved WeRead diagnostic (not a live cloud query)</h2><pre>");
  HalFile file;
  if (!Storage.openFileForRead("ScreenDiag", "/WeReadSync/last-sync-diagnostic.json", file)) {
    out.text("not available</pre>\n");
    return;
  }
  if (file.isDirectory() || file.fileSize64() > 768) {
    out.text("record rejected: invalid type/size</pre>\n");
    return;
  }
  struct Filter {
    Writer& out;
    unsigned depth = 0;
    unsigned roots = 0;
    const char* key = nullptr;
    const char* group = nullptr;
  } filter{out};
  JsonCallbacks callbacks{};
  callbacks.ctx = &filter;
  callbacks.onKey = [](void* ctx, const char* value, size_t n) {
    auto& f = *static_cast<Filter*>(ctx);
    f.key = nullptr;
    if (f.depth != 1 && !(f.depth == 2 && f.group)) return;
    static constexpr const char* keys[] = {
        "schema",       "stage",           "error",          "http",     "detail",  "queue",          "phase",
        "issue",        "uptime_ms",       "retries",        "network",  "report",  "socket",         "tls",
        "verify",       "elapsed_ms",      "attempted",      "accepted", "seconds", "prepare_age_ms", "entry_gap_ms",
        "has_previous", "previous_gap_ms", "response_bytes", "guard",    "succ",    "synckey"};
    const auto key = std::find_if(std::begin(keys), std::end(keys), [value, n](const char* candidate) {
      return std::strlen(candidate) == n && !std::memcmp(value, candidate, n);
    });
    if (key != std::end(keys)) f.key = *key;
  };
  callbacks.onObjectStart = [](void* ctx) {
    auto& f = *static_cast<Filter*>(ctx);
    if (++f.depth == 1) ++f.roots;
    if (f.depth == 2)
      f.group = f.key && (!std::strcmp(f.key, "network") || !std::strcmp(f.key, "report")) ? f.key : nullptr;
    f.key = nullptr;
  };
  callbacks.onObjectEnd = [](void* ctx) {
    auto& f = *static_cast<Filter*>(ctx);
    if (f.depth == 2) f.group = nullptr;
    if (f.depth) --f.depth;
    f.key = nullptr;
  };
  callbacks.onArrayStart = [](void* ctx) {
    auto& f = *static_cast<Filter*>(ctx);
    ++f.depth;
    f.key = nullptr;
  };
  callbacks.onArrayEnd = callbacks.onObjectEnd;
  callbacks.onNumber = [](void* ctx, const char* value, size_t n) {
    auto& f = *static_cast<Filter*>(ctx);
    const char* key = f.key;
    f.key = nullptr;
    if (!key || !n || n > 11 || f.roots != 1) return;
    size_t i = value[0] == '-' ? 1 : 0;
    if (i == n) return;
    for (; i < n; ++i)
      if (value[i] < '0' || value[i] > '9') return;
    if (f.group) {
      f.out.text(f.group);
      f.out.text(".");
    }
    f.out.text(key);
    f.out.text(": ");
    f.out.data(value, n);
    f.out.text("\n");
  };
  callbacks.onString = [](void* ctx, const char* value, size_t n) {
    auto& f = *static_cast<Filter*>(ctx);
    const bool stage = f.depth == 1 && f.roots == 1 && f.key && !std::strcmp(f.key, "stage");
    f.key = nullptr;
    if (!stage) return;
    // Never copy arbitrary JSON strings, even strings stored under known keys.
    for (unsigned i = 0; i <= unsigned(WeReadTime::Diagnostic::Stage::Report); ++i) {
      WeReadTime::Diagnostic diagnostic;
      diagnostic.stage = static_cast<WeReadTime::Diagnostic::Stage>(i);
      const char* name = diagnostic.name();
      if (std::strlen(name) == n && !std::memcmp(value, name, n)) {
        f.out.text("stage: ");
        f.out.text(name);
        f.out.text("\n");
        return;
      }
    }
  };
  StreamingJsonParser parser(callbacks);
  char chunk[128];
  uint64_t remaining = file.fileSize64();
  bool readOk = true;
  while (remaining && out.ok) {
    const size_t n = remaining < sizeof(chunk) ? size_t(remaining) : sizeof(chunk);
    if (file.read(chunk, n) != int(n)) {
      readOk = false;
      break;
    }
    parser.feed(chunk, n);
    remaining -= n;
  }
  if (!readOk || parser.hasError() || filter.depth || filter.roots != 1)
    out.text("WARNING: incomplete/invalid record; diagnostic values may be partial\n");
  out.text("</pre>\n");
}
}  // namespace

Result save(const Info& info, const uint8_t* framebuffer, size_t bytes) {
  if (!framebuffer || !info.width || !info.height || info.width % 8 || info.width > 2048 || info.height > 2048 ||
      info.orientation > 3 || size_t(info.width / 8) * info.height > 256 * 1024 ||
      bytes != size_t(info.width / 8) * info.height)
    return Result::InvalidFrame;
  uint64_t total = 0, free = 0;
  if (!Storage.getSpace(total, free)) return Result::StorageError;
  if (free < 1024 * 1024) return Result::NoSpace;
  if (!Storage.ensureDirectoryExists("/Diagnostics") || !Storage.ensureDirectoryExists("/Diagnostics/Screen"))
    return Result::StorageError;
  for (const char* path : {kPath, kTemp, kBackup}) {
    if (!Storage.exists(path)) continue;
    HalFile existing;
    if (!Storage.openFileForRead("ScreenDiag", path, existing) || existing.isDirectory()) return Result::StorageError;
  }
  // Recover an interrupted promotion before replacing any complete report.
  if (!Storage.exists(kPath) && Storage.exists(kBackup) && !Storage.rename(kBackup, kPath)) return Result::StorageError;
  if (!removeFile(kTemp)) return Result::StorageError;
  size_t written = 0;
  bool complete = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("ScreenDiag", kTemp, file)) return Result::StorageError;
    Writer out(file);
    out.text(
        "<!doctype html><html><meta charset=\"utf-8\"><title>Screen diagnostics</title>"
        "<style>body{font:16px sans-serif;max-width:1000px;margin:24px auto}img{max-width:100%;height:auto;border:1px "
        "solid #aaa}pre{white-space:pre-wrap}</style>"
        "<h1>Screen diagnostics</h1><p>Manual About-page capture. Software framebuffer only: physical ghosting, "
        "partial panel failure and grayscale optical appearance are not captured.</p><pre>firmware: ");
    out.escaped(info.firmware);
    out.text("\nmodel: ");
    out.escaped(info.model);
    out.format("\nphysical_pixels: %u x %u\norientation: %u\ninverted: %u\nasync_refresh: %u\n", unsigned(info.width),
               unsigned(info.height), unsigned(info.orientation), unsigned(info.inverted), unsigned(info.asyncRefresh));
    out.format("full_refresh_pages: %u\nuptime_seconds: %llu\nfree_heap_bytes: %u\n", unsigned(info.refreshPages),
               static_cast<unsigned long long>(info.uptimeSeconds), unsigned(info.freeHeap));
    out.format("largest_heap_bytes: %u\nfree_psram_bytes: %u\n</pre>\n", unsigned(info.largestHeap),
               unsigned(info.freePsram));
    hardwareDiagnostic(out, info.hardware);
    image(out, info, framebuffer);
    syncDiagnostic(out);
    out.text("</html>\n");
    written = out.size;
    file.flush();
    complete = out.ok && file.close();
  }
  if (!complete) {
    removeFile(kTemp);
    return Result::StorageError;
  }
  {
    HalFile check;
    if (!Storage.openFileForRead("ScreenDiag", kTemp, check) || check.fileSize64() != written)
      return Result::StorageError;
  }
  if (!removeFile(kBackup)) return Result::StorageError;
  if (Storage.exists(kPath) && !Storage.rename(kPath, kBackup)) return Result::StorageError;
  if (!Storage.rename(kTemp, kPath)) {
    if (Storage.exists(kBackup)) Storage.rename(kBackup, kPath);
    return Result::StorageError;
  }
  return removeFile(kBackup) ? Result::Saved : Result::StorageError;
}
}  // namespace ScreenDiagnosticExport
