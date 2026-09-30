#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "ManagedWeReadClient.h"

namespace WeReadStore {
void Session::clear() { std::memset(this, 0, sizeof(*this)); }
bool loadSession(Session& session) {
  session.clear();
  return false;
}
}  // namespace WeReadStore
static std::string reply;
static int code = 200;
static unsigned calls = 0;
namespace WeReadHttpClient {
Result requestVerified(const char* url, const RequestOptions& options, const DataCallback& data, const HeaderCallback&,
                       int& status) {
  ++calls;
  assert(std::string(url).starts_with("https://wesync.ar0c.com/api/"));
  assert(options.headers && !std::strcmp(options.headers[0].value, "Bearer fixture-device-token"));
  assert(options.readBuffer && options.readBufferSize >= 512);
  if (!std::string(url).ends_with("/api/v1/device")) assert(options.timeoutMs == 80000);
  status = code;
  const std::string body =
      std::string(url).ends_with("/api/v1/device") ? "{\"device_id\":\"fixture-device\",\"account\":\"123\"}" : reply;
  for (size_t i = 0; i < body.size(); i += 3)
    if (!data(reinterpret_cast<const uint8_t*>(body.data() + i), std::min(size_t(3), body.size() - i)))
      return Result::Aborted;
  return Result::Ok;
}
}  // namespace WeReadHttpClient
int main() {
  const std::string config = "123\nfixture-device\nfixture-device-token\n";
  fakeStorage::files["/WeReadSync/service.conf"] = {config.begin(), config.end()};
  uint8_t buffer[4096];
  char account[64], device[64];
  WeReadClient::ManagedWeReadClient client;
  const std::string valid =
      "{\"schema_version\":2,\"account\":\"123\",\"device_id\":\"fixture-device\",\"capabilities\":[\"managed_reading_"
      "v1\"]}";
  reply = valid;
  assert(client.connect(buffer, sizeof(buffer), account, sizeof(account), device, sizeof(device)));
  assert(fakeStorage::files.count("/.crosspoint/weread/managed-account"));
  WeReadClient::ManagedWeReadClient::Metadata out;
  for (const auto& invalid : {std::string("<html>challenge</html>"), std::string("{}"), valid + valid,
                              std::string("{\"schema_version\":2,\"account\":\"456\",\"device_id\":\"fixture-device\","
                                          "\"capabilities\":[\"managed_reading_v1\"]}"),
                              std::string("{\"schema_version\":2,\"account\":\"123\",\"account\":\"123\",\"device_id\":"
                                          "\"fixture-device\",\"capabilities\":[\"managed_reading_v1\"]}"),
                              std::string(9000, ' ')}) {
    reply = invalid;
    assert(!client.metadata("/api/v2/device/status", nullptr, 0, buffer, sizeof(buffer), out));
  }
  reply = valid;
  code = 401;
  assert(!client.metadata("/api/v2/device/status", nullptr, 0, buffer, sizeof(buffer), out));
  code = 200;
  auto hash = std::string(64, 'a');
  reply = "{\"durably_accepted\":true,\"job\":{\"id\":\"" + hash +
          "\",\"book_id\":\"123\",\"state\":\"uncertain\",\"account\":\"123\",\"device_id\":\"fixture-device\"}}";
  const uint8_t body[] = {'{', '}'};
  assert(client.metadata("/api/v2/progress-jobs", body, sizeof(body), buffer, sizeof(buffer), out));
  assert(!std::strcmp(out.state, "uncertain"));  // Receipt does not mean verified.
  fakeStorage::files.erase("/WeReadSync/service.conf");
  WeReadStore::Session session;
  assert(WeReadClient::ManagedWeReadClient::required());
  assert(!WeReadClient::ManagedWeReadClient::loadAccount(session));
  const auto before = calls;
  assert(client.request(
             "/api/v2/device/status", nullptr, 0, buffer, sizeof(buffer), [](const uint8_t*, size_t) { return true; },
             code) == WeReadHttpClient::Result::NetworkError);
  assert(calls == before);
  std::cout << "PASS managed identity, capability/schema, malformed/duplicate/oversized replies, uncertain receipt and "
               "no direct fallback\n";
}
