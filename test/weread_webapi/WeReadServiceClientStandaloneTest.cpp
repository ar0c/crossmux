#include <cassert>
#include <iostream>
#include <string>

#include "WeReadHttpClient.h"
#include "WeReadServiceClient.h"
#include "WeReadTimeStorage.h"
using namespace WeReadTime;
std::string reply, lastBody, lastMethod;
int responseCode = 200;
bool transportFailure = false;
int postMilestoneCallbacks = 0;
namespace WeReadHttpClient {
Result requestVerified(const char* url, const RequestOptions& o, const DataCallback& data, const HeaderCallback&,
                       int& status) {
  assert(std::string(url).starts_with("https://wesync.ar0c.com/api/v1/"));
  bool auth = false, agent = false;
  for (size_t i = 0; i < o.headerCount; ++i) {
    if (!strcmp(o.headers[i].name, "Authorization")) auth = std::string(o.headers[i].value).starts_with("Bearer ");
    if (!strcmp(o.headers[i].name, "User-Agent")) agent = !strcmp(o.headers[i].value, "weread-sync-device/0.1");
  }
  assert(auth && agent);
  // Match the production transport's input gate before accepting a request.
  if (!o.readBuffer || o.readBufferSize < 2) {
    status = -1;
    if (o.diagnostic) {
      o.diagnostic->stage = NetworkDiagnostic::Stage::Input;
      o.diagnostic->error = 258;  // ESP_ERR_INVALID_ARG
    }
    return Result::NetworkError;
  }
  lastMethod = o.method;
  lastBody = o.body ? std::string(reinterpret_cast<const char*>(o.body), o.bodySize) : "";
  if (o.body) {
    assert(o.onStage && o.stageContext == o.diagnostic);
    for (auto stage :
         {NetworkDiagnostic::Stage::Open, NetworkDiagnostic::Stage::Write, NetworkDiagnostic::Stage::Headers,
          NetworkDiagnostic::Stage::Body, NetworkDiagnostic::Stage::Complete}) {
      o.diagnostic->stage = stage;
      o.onStage(o.stageContext, stage);
      ++postMilestoneCallbacks;
    }
  } else {
    assert(!o.onStage);
  }
  status = responseCode;
  if (transportFailure) return Result::NetworkError;
  for (size_t i = 0; i < reply.size(); i += 7) {
    const auto n = std::min(size_t(7), reply.size() - i);
    if (!data(reinterpret_cast<const uint8_t*>(reply.data() + i), n)) return Result::Aborted;
  }
  return Result::Ok;
}
}  // namespace WeReadHttpClient
int main() {
  const std::string cfg = "123\nabcdef012345678901234567\nvalid-device-token\n";
  fakeStorage::files["/WeReadSync/service.conf"] = {cfg.begin(), cfg.end()};
  ServiceClient c;
  assert(c.configured());
  reply = R"({"account":"999","device_id":"abcdef012345678901234567"})";
  assert(!c.connect("123"));
  reply = R"({"account":"123","device_id":"abcdef012345678901234567"})";
  assert(c.connect("123"));
  Ledger v;
  assert(v.bind("123", "26435427", "source", 20716));
  SdByteLog log;
  assert(log.configure("123", "26435427", "source", 20716, SdByteLog::Format::Service));
  ServiceJournal j(log);
  assert(j.open(v.identity(), 0, 600) && j.reserve(c.device()));
  char jid[128];
  assert(j.jobId(jid, sizeof(jid)));
  const std::string job =
      std::string(R"({"account":"123","device_id":"abcdef012345678901234567","id":")") + jid +
      R"(","book_id":"26435427","source_id":"source","source_date":"2026-09-20","start_seconds":0,"end_seconds":600,"confirmed_seconds":0,"state":"queued","batches":null})";
  reply = "{\"durably_accepted\":true,\"cloud_confirmed\":false,\"job\":" + job + "}";
  transportFailure = true;
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed);
  assert(postMilestoneCallbacks == 5);
  const auto attemptedBody = lastBody;
  assert(j.state() == ServiceJournal::State::Reserved);
  transportFailure = false;
  responseCode = 202;
  const auto acceptedReply = reply;
  reply = job;
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed);
  reply = "{\"durably_accepted\":false,\"job\":" + job + "}";
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed && j.state() == ServiceJournal::State::Reserved);
  responseCode = 429;
  reply = R"({"error":"queue_capacity_reached"})";
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Full && j.state() == ServiceJournal::State::Reserved);
  responseCode = 202;
  reply = acceptedReply;
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Accepted && j.confirmed() == 0 &&
         lastBody == attemptedBody);
  reply = job;
  responseCode = 200;
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Accepted && lastMethod == "GET");
  responseCode = 404;
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed && lastMethod == "GET");
  responseCode = 200;
  reply = job;
  reply.replace(reply.find("\"confirmed_seconds\":0"), 21, "\"confirmed_seconds\":60");
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Accepted && j.confirmed() == 60);
  reply = job;
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed && j.confirmed() == 60);
  reply = job.substr(0, job.size() - 1);
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed);
  reply = job;
  reply.replace(reply.find("\"end_seconds\":600"), 17, "\"end_seconds\":601");
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed);
  reply = job;
  reply.replace(reply.find("\"queued\""), 8, "\"confirmed\"");
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Failed);
  reply.replace(reply.find("\"confirmed_seconds\":0"), 21, "\"confirmed_seconds\":600");
  assert(c.exchange(v.identity(), j) == ServiceClient::Result::Confirmed && j.confirmed() == 600);
  std::cout << "PASS production service client: TLS entrypoint, account check, stable POST, GET-only recovery, strict "
               "receipts\n";
}
