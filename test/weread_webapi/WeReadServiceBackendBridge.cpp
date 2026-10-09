// Local contract bridge: production ServiceClient and journal, pipe-only transport.
// Never contacts the public service or uses real account credentials.
#include <cassert>
#include <iostream>
#include <string>

#include "WeReadHttpClient.h"
#include "WeReadServiceClient.h"
#include "WeReadTimeStorage.h"

static std::string hex(const std::string& value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}
static std::string unhex(const std::string& value) {
  assert(value.size() % 2 == 0);
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  std::string out;
  out.reserve(value.size() / 2);
  for (size_t i = 0; i < value.size(); i += 2) out += char(digit(value[i]) * 16 + digit(value[i + 1]));
  return out;
}
namespace WeReadHttpClient {
Result requestVerified(const char* url, const RequestOptions& options, const DataCallback& data, const HeaderCallback&,
                       int& status) {
  const std::string prefix = "https://wesync.ar0c.com";
  assert(std::string(url).starts_with(prefix));
  assert(options.readBuffer && options.readBufferSize >= 2);
  std::string authorization;
  for (size_t i = 0; i < options.headerCount; ++i)
    if (!strcmp(options.headers[i].name, "Authorization")) authorization = options.headers[i].value;
  const std::string body =
      options.body ? std::string(reinterpret_cast<const char*>(options.body), options.bodySize) : "";
  std::cout << options.method << '\n'
            << std::string(url).substr(prefix.size()) << '\n'
            << authorization << '\n'
            << hex(body) << std::endl;
  std::string code, encoded;
  assert(std::getline(std::cin, code) && std::getline(std::cin, encoded));
  status = std::stoi(code);
  if (status < 0) return Result::NetworkError;
  const auto response = unhex(encoded);
  for (size_t i = 0; i < response.size(); i += 7)
    if (!data(reinterpret_cast<const uint8_t*>(response.data() + i), std::min(size_t(7), response.size() - i)))
      return Result::Aborted;
  return Result::Ok;
}
}  // namespace WeReadHttpClient

int main(int argc, char** argv) {
  assert(argc == 3 || argc == 4);
  const std::string cfg = std::string("123\n") + argv[1] + "\n" + argv[2] + "\n";
  fakeStorage::files["/WeReadSync/service.conf"] = {cfg.begin(), cfg.end()};
  using namespace WeReadTime;
  ServiceClient client;
  assert(client.connect("123"));
  if (argc == 4) {
    assert(client.supportsBatch());
    Ledger source;
    assert(source.bind("123", "26435427", "local-contract-source", 20716));
    ServiceBatch batch;
    for (uint32_t i = 0; i < 5; ++i) {
      batch.items[i].day = 20716 + i;
      batch.items[i].end = 120 + i;
      batch.items[i].action = ServiceBatch::Action::Submit;
    }
    batch.count = 5;
    assert(client.exchangeBatch(source.identity(), batch) == ServiceClient::Result::Failed);
    assert(client.exchangeBatch(source.identity(), batch) == ServiceClient::Result::Accepted);
    for (auto& item : batch.items) item.action = ServiceBatch::Action::Query;
    assert(client.exchangeBatch(source.identity(), batch) == ServiceClient::Result::Failed);
    assert(client.exchangeBatch(source.identity(), batch) == ServiceClient::Result::Accepted);
    assert(client.exchangeBatch(source.identity(), batch) == ServiceClient::Result::Failed);
    assert(client.exchangeBatch(source.identity(), batch) == ServiceClient::Result::Failed);
    for (size_t i = 0; i < batch.count; ++i) assert(batch.items[i].receivedCredit == 0);
    std::cerr << "PASS real batch client / backend: five durable jobs, lost receipt+restart, immutable retry, "
                 "query-only recovery, invalid last receipt, missing receipt, revocation\n";
    return 0;
  }
  Ledger ledger;
  assert(ledger.bind("123", "26435427", "local-contract-source", 20716));
  SdByteLog log;
  assert(log.configure("123", "26435427", "local-contract-source", 20716, SdByteLog::Format::Service));
  ServiceJournal journal(log);
  assert(journal.open(ledger.identity(), 0, 120) && journal.reserve(client.device()));
  assert(client.exchange(ledger.identity(), journal) == ServiceClient::Result::Failed);
  assert(journal.state() == ServiceJournal::State::Reserved);
  assert(client.exchange(ledger.identity(), journal) == ServiceClient::Result::Accepted);
  assert(journal.confirmed() == 0);
  // Account blocked server-side; durable ownership still belongs to the backend.
  assert(client.exchange(ledger.identity(), journal) == ServiceClient::Result::Accepted);
  // A forged cross-device receipt must not advance or clear the journal.
  assert(client.exchange(ledger.identity(), journal) == ServiceClient::Result::Failed);
  assert(journal.confirmed() == 0 && journal.state() == ServiceJournal::State::Accepted);
  assert(client.exchange(ledger.identity(), journal) == ServiceClient::Result::Accepted);
  // Missing receipt and revoked token must never trigger another POST.
  assert(client.exchange(ledger.identity(), journal) == ServiceClient::Result::Failed);
  assert(client.exchange(ledger.identity(), journal) == ServiceClient::Result::Failed);
  assert(journal.state() == ServiceJournal::State::Accepted && journal.confirmed() == 0);
  std::cerr << "PASS real C++ client / Go backend: durable loss+restart, identical retry, GET recovery, identity "
               "rejection, revocation\n";
}
