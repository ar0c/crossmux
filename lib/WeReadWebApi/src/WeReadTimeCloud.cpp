#include "WeReadTimeCloud.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "WeReadHttpClient.h"

namespace WeReadTimeCloud {
namespace {
void wipe(void* data, size_t size) {
  auto* p = static_cast<volatile uint8_t*>(data);
  while (size--) *p++ = 0;
}
bool cookieValue(const char* cookie, const char* name, char* out, size_t capacity) {
  bool found = false;
  const size_t nameSize = strlen(name);
  for (const char* start = cookie; *start;) {
    while (*start == ' ' || *start == ';') ++start;
    const char* end = strchr(start, ';');
    if (!end) end = start + strlen(start);
    if (static_cast<size_t>(end - start) > nameSize && memcmp(start, name, nameSize) == 0 &&
        start[nameSize] == '=') {
      const char* value = start + nameSize + 1;
      const size_t length = end - value;
      if (found || !length || length >= capacity) return false;
      memcpy(out, value, length);
      out[length] = 0;
      found = true;
    }
    start = *end ? end + 1 : end;
  }
  return found;
}
}

Query::~Query() { clear(); }
void Query::clear() {
  wipe(cookie_, sizeof(cookie_));
  wipe(account_, sizeof(account_));
  wipe(skey_, sizeof(skey_));
  wipe(authorization_, sizeof(authorization_));
  wipe(io_, sizeof(io_));
  response_.reset(Response::Mode::Key);
  phase_ = Phase::Done;
  result_ = Result::Protocol;
  scope_ = {};
}

bool Query::begin(const char* cookie, uint64_t now) {
  httpStatus_ = 0;
  wipe(cookie_, sizeof(cookie_));
  wipe(account_, sizeof(account_));
  wipe(skey_, sizeof(skey_));
  wipe(authorization_, sizeof(authorization_));
  response_.reset(Response::Mode::Key);
  phase_ = Phase::Done;
  result_ = Result::Clock;
  // China calendar, independent of the device's display timezone.
  if (now < 1704067200 || now > 4102444799ULL) return false;
  time_t local = static_cast<time_t>(now + 28800);
  tm date = {};
#ifdef _WIN32
  if (gmtime_s(&date, &local) != 0) return false;
#else
  if (!gmtime_r(&local, &date)) return false;
#endif
  scope_.day = (now + 28800) / 86400 * 86400 - 28800;
  scope_.month = scope_.day - static_cast<uint64_t>(date.tm_mday - 1) * 86400;
  result_ = Result::LoginRequired;
  if (!cookie || !cookie[0] || strlen(cookie) >= sizeof(cookie_)) return false;
  for (const char* p = cookie; *p; ++p) if (static_cast<unsigned char>(*p) < 32 || *p == 127) return false;
  if (!cookieValue(cookie, "wr_vid", account_, sizeof(account_)) ||
      !cookieValue(cookie, "wr_skey", skey_, sizeof(skey_))) return false;
  strcpy(cookie_, cookie);
  response_.reset(Response::Mode::Key);
  phase_ = Phase::Key;
  result_ = Result::Pending;
  return true;
}

Result Query::step() {
  if (phase_ == Phase::Done) return result_;
  const bool stats = phase_ == Phase::Stats;
  char body[160] = {};
  if (stats) {
    snprintf(body, sizeof(body),
             "{\"api_name\":\"/readdata/detail\",\"skill_version\":\"1.0.4\",\"mode\":\"monthly\",\"baseTime\":%llu}",
             static_cast<unsigned long long>(scope_.month));
    response_.reset(Response::Mode::Stats, scope_.month, scope_.day);
  }
  WeReadHttpClient::Header headers[] = {
      {"Accept", "application/json"}, {"Content-Type", "application/json"},
      {"Referer", "https://weread.qq.com/r/weread-skills"},
      {"Origin", "https://weread.qq.com"},
      {"User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36 Edg/135.0.0.0"},
      {stats ? "Authorization" : "Cookie", stats ? authorization_ : cookie_},
      {"X-Vid", account_}, {"X-Skey", skey_}};
  WeReadHttpClient::RequestOptions options;
  options.method = stats ? "POST" : "GET";
  options.body = stats ? reinterpret_cast<const uint8_t*>(body) : nullptr;
  options.bodySize = stats ? strlen(body) : 0;
  options.headers = headers;
  options.headerCount = stats ? 6 : 8;
  options.timeoutMs = 20000;
  options.readBuffer = io_;
  options.readBufferSize = sizeof(io_);
  int status = 0;
  size_t received = 0;
  const auto result = WeReadHttpClient::requestVerified(
      stats ? "https://i.weread.qq.com/api/agent/gateway"
            : "https://weread.qq.com/api/skills/apikeyGet?only_show=1",
      options,
      [this, &received](const uint8_t* data, size_t size) {
        if (size > 1024 * 1024 - received) return false;
        received += size;
        return response_.feed(data, size);
      }, {}, status);
  httpStatus_ = status;
  if (status == 401 || status == 403) result_ = Result::LoginRequired;
  else if (result == WeReadHttpClient::Result::NetworkError) result_ = Result::Network;
  else if (status != 200) result_ = Result::Unavailable;
  else if (result != WeReadHttpClient::Result::Ok || !response_.complete()) result_ = Result::Protocol;
  else if (!stats) {
    snprintf(authorization_, sizeof(authorization_), "Bearer %s", response_.key());
    wipe(cookie_, sizeof(cookie_));
    wipe(account_, sizeof(account_));
    wipe(skey_, sizeof(skey_));
    response_.reset(Response::Mode::Stats);
    phase_ = Phase::Stats;
    return Result::Pending;
  } else result_ = Result::Ready;
  phase_ = Phase::Done;
  wipe(cookie_, sizeof(cookie_));
  wipe(account_, sizeof(account_));
  wipe(skey_, sizeof(skey_));
  wipe(authorization_, sizeof(authorization_));
  wipe(io_, sizeof(io_));
  return result_;
}
}  // namespace WeReadTimeCloud
