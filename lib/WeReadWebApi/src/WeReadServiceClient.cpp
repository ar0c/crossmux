#include "WeReadServiceClient.h"
#ifdef ENABLE_CHINESE_VERSION
#include <HalStorage.h>
#ifdef ARDUINO
#include <Logging.h>
#endif
#include <StreamingJsonParser.h>

#include <cstdlib>
#include <ctime>

#include "WeReadHttpClient.h"

namespace WeReadTime {
namespace {
constexpr const char* configPath = "/WeReadSync/service.conf";
#ifdef ARDUINO
// No credentials, URLs, account IDs, or response bodies. The Wi-Fi file server
// can expose this small failure record when USB serial is unavailable.
void persistServiceDiagnostic(const char* phase, int result = -1, int http = 0,
                              const WeReadHttpClient::NetworkDiagnostic* network = nullptr, unsigned parse = 0,
                              unsigned bad = 0, unsigned depth = 0, unsigned roots = 0, unsigned fields = 0) {
  char record[384];
  const int n = std::snprintf(
      record, sizeof(record),
      "{\"version\":1,\"phase\":\"%s\",\"result\":%d,\"http\":%d,\"stage\":%u,\"error\":%d,\"socket\":%d,"
      "\"tls\":%d,\"verify\":%d,\"elapsed_ms\":%u,\"parse\":%u,\"bad\":%u,\"depth\":%u,\"roots\":%u,\"fields\":%u,"
      "\"heap_free\":%u,\"heap_largest\":%u,\"stack_hwm\":%u}\n",
      phase, result, http, network ? unsigned(network->stage) : 0, network ? network->error : 0,
      network ? network->socket : 0, network ? network->tls : 0, network ? network->verify : 0,
      network ? unsigned(network->elapsedMs) : 0, parse, bad, depth, roots, fields,
      static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  if (n <= 0 || size_t(n) >= sizeof(record) || !Storage.ensureDirectoryExists("/WeReadSync")) return;
  HalFile file;
  if (!Storage.openFileForWrite("WRSvc", "/WeReadSync/last-service-diagnostic.json", file)) return;
  if (file.write(reinterpret_cast<const uint8_t*>(record), size_t(n)) != size_t(n))
    LOG_ERR("WRSvc", "service diagnostic write failed");
  file.flush();
}
#else
template <typename... Args>
void persistServiceDiagnostic(const char*, Args...) {}
#endif
// Fixed, bounded response workspace: no account history or batch array allocation.
struct Response {
  char account[32]{}, device[32]{}, id[128]{}, book[64]{}, source[64]{}, date[16]{}, state[20]{}, key[32]{};
  uint64_t start = 0, end = 0, confirmed = 0;
  unsigned batchLimit = 0;
  unsigned fields = 0, depth = 0, roots = 0;
  int http = 0;
  bool bad = false, acceptedSeen = false, accepted = false;
  void field(unsigned bit) {
    if (fields & bit) bad = true;
    fields |= bit;
  }
  void string(const char* value, size_t n) {
    char* dest = nullptr;
    size_t cap = 0;
    unsigned bit = 0;
    if (!std::strcmp(key, "account")) {
      dest = account;
      cap = sizeof(account);
      bit = 1;
    } else if (!std::strcmp(key, "device_id")) {
      dest = device;
      cap = sizeof(device);
      bit = 2;
    } else if (!std::strcmp(key, "id")) {
      dest = id;
      cap = sizeof(id);
      bit = 4;
    } else if (!std::strcmp(key, "book_id")) {
      dest = book;
      cap = sizeof(book);
      bit = 8;
    } else if (!std::strcmp(key, "source_id")) {
      dest = source;
      cap = sizeof(source);
      bit = 16;
    } else if (!std::strcmp(key, "source_date")) {
      dest = date;
      cap = sizeof(date);
      bit = 32;
    } else if (!std::strcmp(key, "state")) {
      dest = state;
      cap = sizeof(state);
      bit = 64;
    }
    if (!dest) return;
    field(bit);
    if (!n || n >= cap) {
      bad = true;
      return;
    }
    std::memcpy(dest, value, n);
    dest[n] = 0;
  }
  void number(const char* value, size_t n) {
    if (!std::strcmp(key, "time_batch_limit")) {
      if (n == 2 && value[0] == '1' && value[1] == '6') batchLimit = 16;
      return;
    }
    uint64_t* dest = nullptr;
    unsigned bit = 0;
    if (!std::strcmp(key, "start_seconds")) {
      dest = &start;
      bit = 128;
    } else if (!std::strcmp(key, "end_seconds")) {
      dest = &end;
      bit = 256;
    } else if (!std::strcmp(key, "confirmed_seconds")) {
      dest = &confirmed;
      bit = 512;
    }
    if (!dest) return;
    field(bit);
    if (!n || n > 5) {
      bad = true;
      return;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
      if (value[i] < '0' || value[i] > '9') {
        bad = true;
        return;
      }
      v = v * 10 + value[i] - '0';
    }
    if (v > 86400) {
      bad = true;
      return;
    }
    *dest = v;
  }
};

bool batchIdentity(const Identity& source, const ServiceBatch::Item& item, char* job, size_t jobSize, char* date,
                   size_t dateSize) {
  const int n = std::snprintf(job, jobSize, "s-%s-%lu-%llu-%llu", source.source, static_cast<unsigned long>(item.day),
                              static_cast<unsigned long long>(item.start), static_cast<unsigned long long>(item.end));
  if (n <= 0 || size_t(n) >= jobSize) return false;
  const std::time_t at = std::time_t(item.day) * 86400;
  std::tm tm{};
#ifdef _WIN32
  if (gmtime_s(&tm, &at)) return false;
#else
  if (!gmtime_r(&at, &tm)) return false;
#endif
  return std::strftime(date, dateSize, "%Y-%m-%d", &tm) != 0;
}

struct BatchResponse {
  const Identity& source;
  ServiceBatch& batch;
  const char* account;
  const char* device;
  Response item{};
  char key[32]{};
  unsigned depth = 0, roots = 0, fields = 0;
  size_t count = 0;
  bool bad = false, array = false, arrayEnded = false, review = false;

  void field(unsigned bit) {
    if (fields & bit) bad = true;
    fields |= bit;
  }
  void string(const char* value, size_t n) {
    if (depth == 1 && !array) {
      const char* expected = nullptr;
      if (!std::strcmp(key, "account")) {
        field(2);
        expected = account;
      } else if (!std::strcmp(key, "device_id")) {
        field(4);
        expected = device;
      }
      if (!expected || std::strlen(expected) != n || std::memcmp(expected, value, n)) bad = true;
      return;
    }
    if (depth != 2 || !array ||
        (std::strcmp(item.key, "id") && std::strcmp(item.key, "book_id") && std::strcmp(item.key, "source_id") &&
         std::strcmp(item.key, "source_date") && std::strcmp(item.key, "state"))) {
      bad = true;
      return;
    }
    item.string(value, n);
  }
  void number(const char* value, size_t n) {
    if (depth == 1 && !array && !std::strcmp(key, "schema")) {
      field(1);
      if (n != 1 || *value != '1') bad = true;
      return;
    }
    if (depth != 2 || !array ||
        (std::strcmp(item.key, "start_seconds") && std::strcmp(item.key, "end_seconds") &&
         std::strcmp(item.key, "confirmed_seconds"))) {
      bad = true;
      return;
    }
    item.number(value, n);
  }
  void finishItem() {
    if (count >= batch.count || item.bad || item.fields != 1020) {
      bad = true;
      return;
    }
    auto& expected = batch.items[count];
    char job[128], date[16];
    if (!batchIdentity(source, expected, job, sizeof(job), date, sizeof(date)) || std::strcmp(item.id, job) ||
        std::strcmp(item.book, source.book) || std::strcmp(item.source, source.source) ||
        std::strcmp(item.date, date) || item.start != expected.start || item.end != expected.end ||
        item.confirmed < expected.credit || item.confirmed > expected.end - expected.start) {
      bad = true;
      return;
    }
    if (!std::strcmp(item.state, "confirmed"))
      expected.receipt = ServiceBatch::Receipt::Confirmed;
    else if (!std::strcmp(item.state, "queued"))
      expected.receipt = ServiceBatch::Receipt::Queued;
    else if (!std::strcmp(item.state, "running"))
      expected.receipt = ServiceBatch::Receipt::Running;
    else if (!std::strcmp(item.state, "uncertain")) {
      expected.receipt = ServiceBatch::Receipt::Uncertain;
      review = true;
    } else {
      bad = true;
      return;
    }
    const bool full = expected.receipt == ServiceBatch::Receipt::Confirmed;
    if (full != (item.confirmed == expected.end - expected.start)) {
      bad = true;
      return;
    }
    expected.receivedCredit = item.confirmed;
    ++count;
  }
};

bool batchRequestBody(const Identity& source, ServiceBatch& batch) {
  if (!batch.count || batch.count > ServiceBatch::kMaxItems) return false;
  size_t used = 0;
  batch.body[used++] = '{';
  std::memcpy(batch.body + used, "\"jobs\":[", 8);
  used += 8;
  for (size_t i = 0; i < batch.count; ++i) {
    const auto& item = batch.items[i];
    Ledger validator;
    if (!validator.bind(source.account, source.book, source.source, item.day) || item.start >= item.end ||
        item.end > 86400 || item.credit > item.end - item.start ||
        (item.action == ServiceBatch::Action::Submit && item.credit))
      return false;
    char job[128], date[16];
    if (!batchIdentity(source, item, job, sizeof(job), date, sizeof(date))) return false;
    const int n = std::snprintf(
        batch.body + used, sizeof(batch.body) - used,
        "%s{\"action\":\"%s\",\"id\":\"%s\",\"book_id\":\"%s\",\"source_id\":\"%s\",\"source_date\":\"%s\","
        "\"start_seconds\":%llu,\"end_seconds\":%llu}",
        i ? "," : "", item.action == ServiceBatch::Action::Submit ? "submit" : "query", job, source.book, source.source,
        date, static_cast<unsigned long long>(item.start), static_cast<unsigned long long>(item.end));
    if (n <= 0 || size_t(n) >= sizeof(batch.body) - used) return false;
    used += size_t(n);
  }
  if (used + 3 > sizeof(batch.body)) return false;
  std::memcpy(batch.body + used, "]}", 3);
  return true;
}

bool request(const char* path, const char* token, const char* body, Response& response) {
  char url[256], auth[144];
  const int n = std::snprintf(url, sizeof(url), "https://wesync.ar0c.com%s", path);
  if (n <= 0 || size_t(n) >= sizeof(url)) return false;
  std::snprintf(auth, sizeof(auth), "Bearer %s", token);
  JsonCallbacks callbacks{};
  callbacks.ctx = &response;
  callbacks.onKey = [](void* p, const char* key, size_t len) {
    auto& r = *static_cast<Response*>(p);
    if (len >= sizeof(r.key)) {
      r.bad = true;
      return;
    }
    std::memcpy(r.key, key, len);
    r.key[len] = 0;
  };
  callbacks.onString = [](void* p, const char* v, size_t n) { static_cast<Response*>(p)->string(v, n); };
  callbacks.onNumber = [](void* p, const char* v, size_t n) { static_cast<Response*>(p)->number(v, n); };
  callbacks.onBool = [](void* p, bool value) {
    auto& r = *static_cast<Response*>(p);
    if (!std::strcmp(r.key, "durably_accepted")) {
      if (r.acceptedSeen || r.depth != 1) r.bad = true;
      r.acceptedSeen = true;
      r.accepted = value;
    }
  };
  callbacks.onObjectStart = [](void* p) {
    auto& r = *static_cast<Response*>(p);
    if (r.depth++ == 0) ++r.roots;
  };
  callbacks.onObjectEnd = [](void* p) {
    auto& r = *static_cast<Response*>(p);
    if (!r.depth)
      r.bad = true;
    else
      --r.depth;
  };
  callbacks.onArrayStart = [](void* p) { static_cast<Response*>(p)->bad = true; };
  StreamingJsonParser parser(callbacks);
  WeReadHttpClient::Header headers[] = {
      {"Authorization", auth}, {"Content-Type", "application/json"}, {"User-Agent", "weread-sync-device/0.1"}};
  WeReadHttpClient::RequestOptions options;
  options.method = body ? "POST" : "GET";
  options.headers = headers;
  options.headerCount = 3;
  options.timeoutMs = 15000;
  options.body = reinterpret_cast<const uint8_t*>(body);
  options.bodySize = body ? std::strlen(body) : 0;
  // requestVerified requires caller-owned receive storage. The response is
  // streamed into the parser, so one small buffer covers both GET and POST.
  uint8_t readBuffer[512];
  options.readBuffer = readBuffer;
  options.readBufferSize = sizeof(readBuffer);
  WeReadHttpClient::NetworkDiagnostic diagnostic;
  options.diagnostic = &diagnostic;
  size_t received = 0;
  int status = 0;
  const auto result = WeReadHttpClient::requestVerified(
      url, options,
      [&](const uint8_t* data, size_t len) {
        received += len;
        if (received > 4096) return false;
        parser.feed(reinterpret_cast<const char*>(data), len);
        return !parser.hasError() && !response.bad;
      },
      {}, status);
  parser.feed(" ", 1);
  response.http = status;
  const bool ok = result == WeReadHttpClient::Result::Ok && (status == 200 || status == 202) && !parser.hasError() &&
                  !response.bad && response.depth == 0 && response.roots == 1;
  if (!ok) {
    persistServiceDiagnostic("request", int(result), status, &diagnostic, unsigned(parser.hasError()),
                             unsigned(response.bad), response.depth, response.roots, response.fields);
  }
  return ok;
}
bool copyLine(char*& cursor, char* out, size_t cap) {
  char* end = std::strchr(cursor, '\n');
  if (!end) return false;
  size_t n = size_t(end - cursor);
  if (n && cursor[n - 1] == '\r') --n;
  if (!n || n >= cap) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = cursor[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
          c == '-'))
      return false;
  }
  std::memcpy(out, cursor, n);
  out[n] = 0;
  cursor = end + 1;
  return true;
}
}  // namespace
bool ServiceClient::configured() {
  return Storage.exists(configPath) || Storage.exists("/.crosspoint/weread/managed-account");
}
bool ServiceClient::configure(const char* account) {
  HalFile file;
  char content[256]{};
  if (!Storage.openFileForRead("WRSvc", configPath, file)) {
    persistServiceDiagnostic("config_open");
    return false;
  }
  const auto size = file.fileSize64();
  if (!size || size >= sizeof(content) || file.read(reinterpret_cast<uint8_t*>(content), size) != int(size)) {
    persistServiceDiagnostic("config_read");
    return false;
  }
  char* cursor = content;
  if (!copyLine(cursor, account_, sizeof(account_)) || !copyLine(cursor, device_, sizeof(device_)) ||
      !copyLine(cursor, token_, sizeof(token_)) || *cursor || (account && std::strcmp(account_, account))) {
    persistServiceDiagnostic("config_parse");
    return false;
  }
  return true;
}
bool ServiceClient::connect(const char* account) {
  batchLimit_ = 0;
  if (!configure(account)) return false;
  persistServiceDiagnostic("connect_start");
  Response r;
  if (!request("/api/v1/device", token_, nullptr, r)) return false;
  const bool matched = r.fields == 3 && !std::strcmp(r.account, account_) && !std::strcmp(r.device, device_);
  if (!matched) {
    persistServiceDiagnostic("response_identity", -1, r.http, nullptr, 0, 0, r.depth, r.roots, r.fields);
  } else {
    batchLimit_ = r.batchLimit;
    persistServiceDiagnostic("connected", 0, r.http);
  }
  return matched;
}
ServiceClient::Result ServiceClient::exchange(const Identity& id, ServiceJournal& journal) {
  if (std::strcmp(id.account, account_) || !journal.matchesDevice(device_) ||
      journal.state() == ServiceJournal::State::Empty)
    return Result::Failed;
  char job[128], date[16], path[180], body[640];
  if (!journal.jobId(job, sizeof(job))) return Result::Failed;
  const std::time_t at = std::time_t(id.day) * 86400;
  std::tm tm{};
#ifdef _WIN32
  if (gmtime_s(&tm, &at)) return Result::Failed;
#else
  if (!gmtime_r(&at, &tm)) return Result::Failed;
#endif
  if (!std::strftime(date, sizeof(date), "%Y-%m-%d", &tm)) return Result::Failed;
  const bool post = journal.state() == ServiceJournal::State::Reserved;
  if (post)
    std::strcpy(path, "/api/v1/jobs?compact=1");
  else
    std::snprintf(path, sizeof(path), "/api/v1/jobs/%s?compact=1", job);
  const int n = std::snprintf(body, sizeof(body),
                              "{\"id\":\"%s\",\"book_id\":\"%s\",\"source_id\":\"%s\",\"source_date\":\"%s\",\"start_"
                              "seconds\":%llu,\"end_seconds\":%llu}",
                              job, id.book, id.source, date, static_cast<unsigned long long>(journal.start()),
                              static_cast<unsigned long long>(journal.end()));
  if (n <= 0 || size_t(n) >= sizeof(body)) return Result::Failed;
  persistServiceDiagnostic(post ? "post_start" : "query_start");
  Response r;
  if (!request(path, token_, post ? body : nullptr, r)) return post && r.http == 429 ? Result::Full : Result::Failed;
  if (r.fields != 1023 || std::strcmp(r.account, account_) || std::strcmp(r.device, device_) ||
      std::strcmp(r.id, job) || std::strcmp(r.book, id.book) || std::strcmp(r.source, id.source) ||
      std::strcmp(r.date, date) || r.start != journal.start() || r.end != journal.end() ||
      r.confirmed > r.end - r.start)
    return Result::Failed;
  if (post && (!r.acceptedSeen || !r.accepted)) return Result::Failed;
  const bool confirmed = !std::strcmp(r.state, "confirmed");
  if (confirmed && r.confirmed != r.end - r.start) return Result::Failed;
  if (!confirmed && std::strcmp(r.state, "queued") && std::strcmp(r.state, "running") &&
      std::strcmp(r.state, "uncertain"))
    return Result::Failed;
  const auto now = std::time(nullptr);
  if (!journal.accept(confirmed, r.confirmed, now > 0 ? uint64_t(now) : 0)) return Result::Failed;
  persistServiceDiagnostic(confirmed ? "confirmed" : "accepted", 0, r.http);
  if (!std::strcmp(r.state, "uncertain")) return Result::Review;
  return confirmed ? Result::Confirmed : Result::Accepted;
}

ServiceClient::Result ServiceClient::exchangeBatch(const Identity& source, ServiceBatch& batch) {
  if (!supportsBatch() || std::strcmp(source.account, account_) || !batchRequestBody(source, batch))
    return Result::Failed;
  BatchResponse response{source, batch, account_, device_};
  JsonCallbacks callbacks{};
  callbacks.ctx = &response;
  callbacks.onKey = [](void* p, const char* key, size_t n) {
    auto& r = *static_cast<BatchResponse*>(p);
    char* target = r.depth == 2 && r.array ? r.item.key : r.key;
    if ((r.depth != 1 && r.depth != 2) || n >= sizeof(r.key)) {
      r.bad = true;
      return;
    }
    std::memcpy(target, key, n);
    target[n] = 0;
    if (r.depth == 1 && (r.array || (std::strcmp(target, "schema") && std::strcmp(target, "account") &&
                                     std::strcmp(target, "device_id") && std::strcmp(target, "durably_accepted") &&
                                     std::strcmp(target, "jobs"))))
      r.bad = true;
    if (r.depth == 2 && (!r.array || (std::strcmp(target, "id") && std::strcmp(target, "book_id") &&
                                      std::strcmp(target, "source_id") && std::strcmp(target, "source_date") &&
                                      std::strcmp(target, "state") && std::strcmp(target, "start_seconds") &&
                                      std::strcmp(target, "end_seconds") && std::strcmp(target, "confirmed_seconds"))))
      r.bad = true;
  };
  callbacks.onString = [](void* p, const char* value, size_t n) { static_cast<BatchResponse*>(p)->string(value, n); };
  callbacks.onNumber = [](void* p, const char* value, size_t n) { static_cast<BatchResponse*>(p)->number(value, n); };
  callbacks.onBool = [](void* p, bool value) {
    auto& r = *static_cast<BatchResponse*>(p);
    if (r.depth != 1 || r.array || std::strcmp(r.key, "durably_accepted") || !value) r.bad = true;
    r.field(8);
  };
  callbacks.onNull = [](void* p) { static_cast<BatchResponse*>(p)->bad = true; };
  callbacks.onObjectStart = [](void* p) {
    auto& r = *static_cast<BatchResponse*>(p);
    if (r.depth == 0) {
      if (++r.roots != 1) r.bad = true;
    } else if (r.depth == 1 && r.array)
      r.item = {};
    else
      r.bad = true;
    ++r.depth;
  };
  callbacks.onObjectEnd = [](void* p) {
    auto& r = *static_cast<BatchResponse*>(p);
    if (r.depth == 2 && r.array)
      r.finishItem();
    else if (r.depth != 1 || r.array)
      r.bad = true;
    if (r.depth) --r.depth;
  };
  callbacks.onArrayStart = [](void* p) {
    auto& r = *static_cast<BatchResponse*>(p);
    if (r.depth != 1 || r.array || std::strcmp(r.key, "jobs")) r.bad = true;
    r.field(16);
    r.array = true;
  };
  callbacks.onArrayEnd = [](void* p) {
    auto& r = *static_cast<BatchResponse*>(p);
    if (r.depth != 1 || !r.array || r.count != r.batch.count) r.bad = true;
    r.array = false;
    r.arrayEnded = true;
  };
  StreamingJsonParser parser(callbacks);
  char auth[144];
  std::snprintf(auth, sizeof(auth), "Bearer %s", token_);
  WeReadHttpClient::Header headers[] = {
      {"Authorization", auth}, {"Content-Type", "application/json"}, {"User-Agent", "weread-sync-device/0.1"}};
  WeReadHttpClient::RequestOptions options;
  options.method = "POST";
  options.headers = headers;
  options.headerCount = 3;
  options.timeoutMs = 15000;
  options.body = reinterpret_cast<const uint8_t*>(batch.body);
  options.bodySize = std::strlen(batch.body);
  uint8_t readBuffer[512];
  options.readBuffer = readBuffer;
  options.readBufferSize = sizeof(readBuffer);
  WeReadHttpClient::NetworkDiagnostic diagnostic;
  options.diagnostic = &diagnostic;
  size_t received = 0;
  int http = 0;
  persistServiceDiagnostic("batch_start");
  const auto result = WeReadHttpClient::requestVerified(
      "https://wesync.ar0c.com/api/v1/jobs/batch", options,
      [&](const uint8_t* data, size_t n) {
        received += n;
        if (received > ServiceBatch::kMaxItems * 512 + 256) return false;
        parser.feed(reinterpret_cast<const char*>(data), n);
        return !parser.hasError() && !response.bad;
      },
      {}, http);
  parser.feed(" ", 1);
  const bool ok = result == WeReadHttpClient::Result::Ok && (http == 200 || http == 202) && !parser.hasError() &&
                  !response.bad && response.depth == 0 && response.roots == 1 && response.fields == 31 &&
                  response.arrayEnded && response.count == batch.count;
  persistServiceDiagnostic(ok ? "batch_accepted" : "batch_failed", int(result), http, &diagnostic,
                           unsigned(parser.hasError()), unsigned(response.bad), response.depth, response.roots,
                           response.fields);
  if (!ok) return http == 429 ? Result::Full : Result::Failed;
  return response.review ? Result::Review : Result::Accepted;
}
}  // namespace WeReadTime
#endif
