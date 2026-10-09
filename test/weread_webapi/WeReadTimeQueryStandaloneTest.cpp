#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "WeReadHttpClient.h"
#include "WeReadTimeCloud.h"

namespace {
int calls = 0;
int failAt = -1;
int responseStatus = 200;
std::string stats = R"({"baseTime":1788192000,"totalReadTime":16234,"readTimes":{"1789401600":2947}})";
std::string lastBody;
size_t chunkSize = 1;
size_t disconnectAfter = SIZE_MAX;
}  // namespace

namespace WeReadHttpClient {
Result requestVerified(const char* url, const RequestOptions& options, const DataCallback& onData,
                       const HeaderCallback&, int& status) {
  ++calls;
  status = responseStatus;
  if (calls == failAt) return Result::NetworkError;
  const bool key = strstr(url, "apikeyGet?only_show=1") != nullptr;
  assert(key || strcmp(url, "https://i.weread.qq.com/api/agent/gateway") == 0);
  assert(strcmp(options.method, key ? "GET" : "POST") == 0);
  assert(options.readBufferSize == 3072 && options.timeoutMs == 20000);
  bool cookie = false, authorization = false, vid = false, skey = false;
  for (size_t i = 0; i < options.headerCount; ++i) {
    if (strcmp(options.headers[i].name, "Cookie") == 0) {
      cookie = true;
      assert(strcmp(options.headers[i].value, "wr_vid=test; wr_skey=test") == 0);
    }
    if (strcmp(options.headers[i].name, "Authorization") == 0) {
      authorization = true;
      assert(strcmp(options.headers[i].value, "Bearer wrk-test") == 0);
    }
    if (strcmp(options.headers[i].name, "X-Vid") == 0) {
      vid = true;
      assert(strcmp(options.headers[i].value, "test") == 0);
    }
    if (strcmp(options.headers[i].name, "X-Skey") == 0) {
      skey = true;
      assert(strcmp(options.headers[i].value, "test") == 0);
    }
  }
  assert(cookie == key && authorization != key);  // No cookie sent to gateway.
  assert(vid == key && skey == key);
  if (!key) lastBody.assign(reinterpret_cast<const char*>(options.body), options.bodySize);
  const std::string body = key ? R"({"apikey":"wrk-test"})" : stats;
  for (size_t i = 0; i < body.size();) {
    if (!key && i >= disconnectAfter) return Result::NetworkError;
    const size_t count = std::min({chunkSize, options.readBufferSize, body.size() - i});
    memcpy(options.readBuffer, body.data() + i, count);
    if (!onData(options.readBuffer, count)) return Result::Aborted;
    i += count;
  }
  return Result::Ok;
}
}  // namespace WeReadHttpClient

int main() {
  using WeReadTimeCloud::Query;
  using WeReadTimeCloud::Result;
  Query query;
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::Pending && query.readingStats());
  assert(query.step() == Result::Ready);
  assert(query.snapshot().hasDay && query.snapshot().daySeconds == 2947);
  assert(lastBody ==
         R"({"api_name":"/readdata/detail","skill_version":"1.0.4","mode":"monthly","baseTime":1788192000})");
  assert(calls == 2 && query.step() == Result::Ready && calls == 2);
  assert(!query.begin("cookie", 0));
  assert(query.step() == Result::Clock && calls == 2);
  assert(!query.begin("bad\r\ncookie", 1789453168));
  assert(query.step() == Result::LoginRequired && calls == 2);
  for (int phase = 1; phase <= 2; ++phase) {
    failAt = calls + phase;
    assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
    if (phase == 2) assert(query.step() == Result::Pending);
    assert(query.step() == Result::Network);
    const int stopped = calls;
    assert(query.step() == Result::Network && calls == stopped);
  }
  failAt = -1;
  responseStatus = 401;
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::LoginRequired);
  assert(query.httpStatus() == 401);
  responseStatus = 302;
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::Unavailable);  // No redirect/retry at this layer.
  assert(query.httpStatus() == 302);
  responseStatus = 200;
  const std::string validStats = stats;
  std::string largeStats = validStats.substr(0, validStats.size() - 1) + ",\"unused\":[";
  for (int i = 0; i < 3000; ++i) largeStats += i ? ",12345" : "12345";
  largeStats += "]}";
  // Exercise workspace-sized reads, a split token at the 3 KiB boundary,
  // and the unchanged 1 MiB response cap without retaining the whole response.
  for (const size_t chunk : {size_t{1}, size_t{3071}, size_t{3072}, size_t{4096}}) {
    chunkSize = chunk;
    stats = std::string(3070, ' ') + largeStats;
    assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
    assert(query.step() == Result::Pending);
    assert(query.step() == Result::Ready);
    assert(query.snapshot().daySeconds == 2947);
  }
  chunkSize = 3072;
  stats = validStats + std::string(1024 * 1024 - validStats.size(), ' ');
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::Pending);
  assert(query.step() == Result::Ready);
  stats.push_back(' ');
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::Pending);
  assert(query.step() == Result::Protocol);
  stats = std::string(4000, ' ') + validStats;
  disconnectAfter = 3072;
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::Pending);
  assert(query.step() == Result::Network);
  disconnectAfter = SIZE_MAX;
  stats = std::string(3070, ' ') + validStats + "{}";
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::Pending);
  assert(query.step() == Result::Protocol);
  stats = validStats;
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  assert(query.step() == Result::Pending);
  stats = "{}";
  assert(query.step() == Result::Protocol);
  query.clear();
  const int clearedCalls = calls;
  assert(query.step() == Result::Protocol && calls == clearedCalls);
  assert(query.begin("wr_vid=test; wr_skey=test", 1789453168));
  query.clear();
  assert(query.step() == Result::Protocol && calls == clearedCalls);
  std::cout << "Time query state/transport tests passed; workspace=" << sizeof(Query) << " bytes\n";
}
