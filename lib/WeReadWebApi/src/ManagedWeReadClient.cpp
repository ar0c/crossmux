#include "ManagedWeReadClient.h"
#ifdef ENABLE_CHINESE_VERSION
#include <HalStorage.h>
#include <StreamingJsonParser.h>
#include <mbedtls/sha256.h>
#include <strings.h>

#include <cstdio>
#include <cstring>

namespace WeReadClient {
namespace {
constexpr const char* kMarker = "/.crosspoint/weread/managed-account";
constexpr size_t kMaxMetadata = 8192;
struct Parse {
  ManagedWeReadClient::Metadata* out;
  char key[32]{};
  char account[32]{}, device[32]{};
  unsigned depth = 0, roots = 0;
  bool bad = false, capArray = false;
  unsigned seen = 0;
  bool schema = false;
};
}  // namespace
bool ManagedWeReadClient::required() { return WeReadTime::ServiceClient::configured() || Storage.exists(kMarker); }
bool ManagedWeReadClient::loadAccount(WeReadStore::Session& session) {
  if (!required()) return WeReadStore::loadSession(session) && session.valid();
  session.clear();
  WeReadTime::ServiceClient config;
  if (!config.configure()) return false;
  HalFile marker;
  if (Storage.exists(kMarker)) {
    char owner[32]{};
    if (!Storage.openFileForRead("WRManaged", kMarker, marker)) return false;
    const auto n = marker.fileSize64();
    if (!n || n >= sizeof(owner) || marker.read(reinterpret_cast<uint8_t*>(owner), n) != int(n) ||
        strcmp(owner, config.account()))
      return false;
  }
  WeReadStore::Session previous;
  if (WeReadStore::loadSession(previous) && previous.vid[0] && strcmp(previous.vid, config.account())) return false;
  strncpy(session.vid, config.account(), sizeof(session.vid) - 1);
  return true;
}
WeReadHttpClient::Result ManagedWeReadClient::request(const char* path, const uint8_t* body, size_t length,
                                                      uint8_t* buffer, size_t size,
                                                      const WeReadHttpClient::DataCallback& sink, int& status,
                                                      uint32_t* nextCursor) const {
  char url[256], auth[144];
  WeReadTime::ServiceClient service;
  if (!path || !buffer || !size || !account_ || !device_ || !service.configure(account_) ||
      strcmp(service.device(), device_))
    return WeReadHttpClient::Result::NetworkError;
  const int n = snprintf(url, sizeof(url), "https://wesync.ar0c.com%s", path);
  if (n <= 0 || size_t(n) >= sizeof(url)) return WeReadHttpClient::Result::NetworkError;
  snprintf(auth, sizeof(auth), "Bearer %s", service.bearer());
  WeReadHttpClient::Header headers[] = {
      {"Authorization", auth}, {"Content-Type", "application/json"}, {"User-Agent", "weread-sync-device/0.2"}};
  WeReadHttpClient::RequestOptions options;
  options.method = body ? "POST" : "GET";
  options.body = body;
  options.bodySize = length;
  options.headers = headers;
  options.headerCount = 3;
  options.readBuffer = buffer;
  options.readBufferSize = size;
  options.timeoutMs = 80000;
  if (nextCursor) *nextCursor = UINT32_MAX;
  const auto onHeader = [nextCursor](const char* key, const char* value) {
    if (!nextCursor || strcasecmp(key, "X-Next-Cursor")) return;
    uint32_t number = 0;
    size_t n = strlen(value);
    if (!n || n > 5) return;
    for (size_t i = 0; i < n; i++) {
      if (value[i] < '0' || value[i] > '9') return;
      number = number * 10 + value[i] - '0';
    }
    *nextCursor = number;
  };
  WeReadHttpClient::NetworkDiagnostic diagnostic;
  options.diagnostic = &diagnostic;
  {
    constexpr char start[] = "{\"version\":1,\"phase\":\"request_start\",\"result\":-1}\n";
    HalFile file;
    if (Storage.ensureDirectoryExists("/WeReadSync") &&
        Storage.openFileForWrite("WRManaged", "/WeReadSync/last-managed-diagnostic.json", file)) {
      file.write(reinterpret_cast<const uint8_t*>(start), sizeof(start) - 1);
      file.flush();
    }
  }
  const auto result = WeReadHttpClient::requestVerified(url, options, sink, onHeader, status);
  char record[256];
  const int written = snprintf(record, sizeof(record),
                               "{\"version\":1,\"operation\":\"%s\",\"http\":%d,\"result\":%d,\"stage\":%u,\"error\":%"
                               "d,\"socket\":%d,\"tls\":%d,\"verify\":%d,\"elapsed_ms\":%u}\n",
                               !strcmp(path, "/api/v2/reading")         ? "reading"
                               : !strcmp(path, "/api/v2/device/status") ? "status"
                                                                        : "progress",
                               status, int(result), unsigned(diagnostic.stage), diagnostic.error, diagnostic.socket,
                               diagnostic.tls, diagnostic.verify, unsigned(diagnostic.elapsedMs));
  HalFile file;
  if (written > 0 && size_t(written) < sizeof(record) && Storage.ensureDirectoryExists("/WeReadSync") &&
      Storage.openFileForWrite("WRManaged", "/WeReadSync/last-managed-diagnostic.json", file)) {
    file.write(reinterpret_cast<const uint8_t*>(record), size_t(written));
    file.flush();
  }
  return result;
}
bool ManagedWeReadClient::metadata(const char* path, const uint8_t* body, size_t length, uint8_t* buffer, size_t size,
                                   Metadata& out) const {
  out = {};
  Parse p{&out};
  JsonCallbacks cb{};
  cb.ctx = &p;
  cb.onKey = [](void* raw, const char* key, size_t n) {
    auto& p = *static_cast<Parse*>(raw);
    if (n >= sizeof(p.key)) {
      p.bad = true;
      return;
    }
    memcpy(p.key, key, n);
    p.key[n] = 0;
  };
  cb.onObjectStart = [](void* raw) {
    auto& p = *static_cast<Parse*>(raw);
    if (p.depth++ == 0) ++p.roots;
  };
  cb.onObjectEnd = [](void* raw) {
    auto& p = *static_cast<Parse*>(raw);
    if (!p.depth) {
      p.bad = true;
      return;
    }
    if (--p.depth == 0) p.out->complete = true;
  };
  cb.onArrayStart = [](void* raw) {
    auto& p = *static_cast<Parse*>(raw);
    p.capArray = p.depth == 1 && !strcmp(p.key, "capabilities");
    ++p.depth;
  };
  cb.onArrayEnd = [](void* raw) {
    auto& p = *static_cast<Parse*>(raw);
    if (!p.depth) {
      p.bad = true;
      return;
    }
    --p.depth;
    p.capArray = false;
  };
  cb.onString = [](void* raw, const char* v, size_t n) {
    auto& p = *static_cast<Parse*>(raw);
    if (p.capArray && n == 18 && !memcmp(v, "managed_reading_v1", 18)) p.out->capability = true;
    char* dest = nullptr;
    size_t cap = 0;
    unsigned bit = 0;
    if (!strcmp(p.key, "account")) bit = 1;
    if (!strcmp(p.key, "device_id")) bit = 2;
    if (!strcmp(p.key, "context_id")) bit = 4;
    if (!strcmp(p.key, "id")) bit = 8;
    if (!strcmp(p.key, "state")) bit = 16;
    if (!strcmp(p.key, "book_id")) bit = 32;
    if (bit) {
      if (p.seen & bit) {
        p.bad = true;
        return;
      }
      p.seen |= bit;
    }
    if (p.depth == 1 && !strcmp(p.key, "context_id")) {
      dest = p.out->context;
      cap = sizeof(p.out->context);
    }
    if (!strcmp(p.key, "state") && p.depth == 2) {
      dest = p.out->state;
      cap = sizeof(p.out->state);
    }
    if ((p.depth == 1 || p.depth == 2) && !strcmp(p.key, "account")) {
      dest = p.account;
      cap = sizeof(p.account);
    }
    if ((p.depth == 1 || p.depth == 2) && !strcmp(p.key, "device_id")) {
      dest = p.device;
      cap = sizeof(p.device);
    }
    if (p.depth == 2 && !strcmp(p.key, "id")) {
      dest = p.out->job;
      cap = sizeof(p.out->job);
    }
    if (p.depth == 2 && !strcmp(p.key, "book_id")) {
      dest = p.out->book;
      cap = sizeof(p.out->book);
    }
    if (dest) {
      if (!n || n >= cap) {
        p.bad = true;
        return;
      }
      memcpy(dest, v, n);
      dest[n] = 0;
    }
  };
  cb.onNumber = [](void* raw, const char* v, size_t n) {
    auto& p = *static_cast<Parse*>(raw);
    if (p.depth == 1 && !strcmp(p.key, "schema_version")) {
      if (p.schema) {
        p.bad = true;
        return;
      }
      p.schema = n == 1 && v[0] == '2';
      if (!p.schema) p.bad = true;
      return;
    }
    if (p.depth != 1 || strcmp(p.key, "next_cursor")) return;
    uint32_t x = 0;
    if (n > 5) {
      p.bad = true;
      return;
    }
    for (size_t i = 0; i < n; i++) {
      if (v[i] < '0' || v[i] > '9') {
        p.bad = true;
        return;
      }
      x = x * 10 + v[i] - '0';
    }
    p.out->nextCursor = x;
  };
  cb.onBool = [](void* raw, bool value) {
    auto& p = *static_cast<Parse*>(raw);
    if (p.depth == 1 && !strcmp(p.key, "durably_accepted")) p.out->accepted = value;
  };
  StreamingJsonParser parser(cb);
  size_t received = 0;
  int status = 0;
  const auto result = request(
      path, body, length, buffer, size,
      [&](const uint8_t* b, size_t n) {
        received += n;
        if (received > kMaxMetadata) return false;
        parser.feed(reinterpret_cast<const char*>(b), n);
        return !p.bad && !parser.hasError();
      },
      status);
  parser.feed(" ", 1);
  {
    char record[192];
    const int n = snprintf(record, sizeof(record),
                           "{\"version\":1,\"parse\":%u,\"bad\":%u,\"depth\":%u,\"roots\":%u,\"fields\":%u,\"schema\":%"
                           "u,\"capability\":%u,\"identity\":%u,\"bytes\":%u}\n",
                           unsigned(parser.hasError()), unsigned(p.bad), p.depth, p.roots, p.seen, unsigned(p.schema),
                           unsigned(out.capability),
                           unsigned(!strcmp(p.account, account_) && !strcmp(p.device, device_)), unsigned(received));
    HalFile file;
    if (n > 0 && size_t(n) < sizeof(record) &&
        Storage.openFileForWrite("WRManaged", "/WeReadSync/last-managed-parser-diagnostic.json", file)) {
      file.write(reinterpret_cast<const uint8_t*>(record), size_t(n));
      file.flush();
    }
  }

  if (result != WeReadHttpClient::Result::Ok || (status != 200 && status != 202) || p.bad || parser.hasError() ||
      p.depth || p.roots != 1 || !out.complete)
    return false;
  if (!strcmp(path, "/api/v2/device/status"))
    return p.schema && out.capability && !strcmp(p.account, account_) && !strcmp(p.device, device_);
  if (!strncmp(path, "/api/v2/progress-jobs", 21))
    return out.accepted && strlen(out.job) == 64 && out.book[0] && !strcmp(p.account, account_) &&
           !strcmp(p.device, device_);
  return true;
}
bool ManagedWeReadClient::connect(uint8_t* buffer, size_t size, char* account, size_t accountSize, char* device,
                                  size_t deviceSize) {
  WeReadStore::Session owner;
  if (!loadAccount(owner)) return false;
  WeReadTime::ServiceClient service;
  if (!service.connect(nullptr)) return false;
  if (!account || !device || strlen(service.account()) >= accountSize || strlen(service.device()) >= deviceSize)
    return false;
  strcpy(account, service.account());
  strcpy(device, service.device());
  account_ = account;
  device_ = device;
  Metadata meta;
  if (!metadata("/api/v2/device/status", nullptr, 0, buffer, size, meta)) return false;
  // Persistent identity-only marker prevents direct/QR fallback when config is
  // subsequently removed. The existing local web Session file is untouched.
  HalFile file;
  if (Storage.exists(kMarker)) {
    char stored[32]{};
    if (!Storage.openFileForRead("WRManaged", kMarker, file)) return false;
    auto n = file.fileSize64();
    return n > 0 && n < sizeof(stored) && file.read(reinterpret_cast<uint8_t*>(stored), n) == int(n) &&
           !strcmp(stored, account_);
  }
  if (!Storage.ensureDirectoryExists("/.crosspoint/weread") || !Storage.openFileForWrite("WRManaged", kMarker, file))
    return false;
  const size_t n = strlen(account_);
  if (file.write(reinterpret_cast<const uint8_t*>(account_), n) != n) return false;
  file.flush();
  return true;
}
bool ManagedWeReadClient::resourceID(const char* url, char out[65]) {
  if (!url || strlen(url) > 511) return false;
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  const bool ok = mbedtls_sha256_starts(&ctx, 0) == 0 &&
                  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(url), strlen(url)) == 0 &&
                  mbedtls_sha256_finish(&ctx, hash) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;
  for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 65 - i * 2, "%02x", hash[i]);
  return true;
}
}  // namespace WeReadClient
#endif
