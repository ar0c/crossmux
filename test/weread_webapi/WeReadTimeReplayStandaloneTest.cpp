// Host-only: real query/parser/ledger/transaction; network and clock are fake.
#include "WeReadTimeCloud.h"
#include "WeReadTimeTransaction.h"
#include "WeReadTimeBaseline.h"
#include "WeReadHttpClient.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
std::string payload;
int http = 200;
size_t fragment = 1;
unsigned requests = 0;
}
namespace WeReadHttpClient {
Result requestVerified(const char* url, const RequestOptions& options, const DataCallback& data,
                       const HeaderCallback&, int& status) {
  ++requests;
  const bool key = std::strstr(url, "apikeyGet?only_show=1");
  assert(key || !std::strcmp(url, "https://i.weread.qq.com/api/agent/gateway"));
  assert(!std::strcmp(options.method, key ? "GET" : "POST"));
  if (!key) {
    const std::string body(reinterpret_cast<const char*>(options.body), options.bodySize);
    assert(body.find("/readdata/detail") != std::string::npos); // No report endpoint.
  }
  status = key ? 200 : http;
  const std::string bytes = key ? R"({"apikey":"wrk-test"})" : payload;
  for (size_t offset = 0; offset < bytes.size(); offset += fragment)
    if (!data(reinterpret_cast<const uint8_t*>(bytes.data() + offset),
              std::min(fragment, bytes.size() - offset))) return Result::Aborted;
  return Result::Ok;
}
}
using namespace WeReadTime;
struct MemoryLog : ByteLog {
  std::vector<uint8_t> bytes;
  ReadState size(uint64_t& n) override { n=bytes.size(); return n ? ReadState::Ready : ReadState::Missing; }
  bool read(uint64_t offset,uint8_t* out,size_t n) override {
    if (offset > bytes.size() || n > bytes.size()-offset) return false;
    std::memcpy(out,bytes.data()+offset,n); return true;
  }
  bool appendAndSync(const uint8_t* p,size_t n) override { bytes.insert(bytes.end(),p,p+n); return true; }
};
struct ReplayTransport : TimeTransport {
  WeReadTimeCloud::Query query;
  uint64_t now;
  uint32_t tick=30000;
  bool querying=false;
  unsigned entries=0,reports=0;
  explicit ReplayTransport(uint64_t epoch):now(epoch) {}
  uint64_t epochSeconds() const override { return now; }
  uint32_t monotonicMs() const override { return tick; }
  Read prepare(const Identity&) override { return Read::Ready; }
  Read snapshot(AccountSnapshot& out) override {
    if (!querying) { assert(query.begin("wr_vid=test; wr_skey=test",now)); querying=true; }
    auto state=query.step();
    if (state==WeReadTimeCloud::Result::Pending) return Read::Pending;
    querying=false;
    if (state!=WeReadTimeCloud::Result::Ready) return Read::Failed;
    const auto& cloud=query.snapshot();
    Identity identity{}; std::strcpy(identity.account,"test");
    out=cloudBaseline(cloud,identity,now);
    return Read::Ready;
  }
  Write enter() override { ++entries; return Write::Accepted; }
  Write report(uint32_t seconds) override { assert(seconds==30); ++reports; return Write::Accepted; }
};
void run(const std::string& json,uint64_t now,bool hasDay,int status=200,
         WeReadTimeCloud::Result expected=WeReadTimeCloud::Result::Ready) {
  for (size_t chunk : {1U,7U,257U,4096U}) {
    payload=json; http=status; fragment=chunk; requests=0;
    MemoryLog log; PacedJournal journal(log); ExternalTime receipt;
    std::strcpy(receipt.identity.account,"test"); std::strcpy(receipt.identity.book,"book");
    std::strcpy(receipt.identity.source,"replay"); receipt.identity.day=20708;
    receipt.sourceMs=105*60*1000; assert(journal.open(receipt));
    auto original=log.bytes;
    SendCoordinator coordinator(0); ReplayTransport transport(now);
    TimeTransaction transaction(journal,coordinator,transport); assert(transaction.begin());
    for (int i=0;i<10;++i) {
      auto state=transaction.step();
      if (state==TimeTransaction::State::NotSent || state==TimeTransaction::State::Entering) break;
    }
    assert(requests==2 && transport.entries==0 && transport.reports==0);
    assert(transport.query.result()==expected && transport.query.httpStatus()==status);
    if (expected==WeReadTimeCloud::Result::Ready) assert(transport.query.snapshot().hasDay==hasDay);
    if (expected==WeReadTimeCloud::Result::Ready) {
      assert(transaction.state()==TimeTransaction::State::Entering);
      assert(journal.ledger().remaining()==105*60-30);
      const auto baseline=transport.query.snapshot();
      assert(transaction.step()==TimeTransaction::State::Sending);
      assert(transaction.step()==TimeTransaction::State::ReadbackWait);
      transport.now+=6; transport.tick+=6000;
      payload="{\"baseTime\":"+std::to_string(baseline.month)+",\"totalReadTime\":"+
          std::to_string(baseline.monthSeconds+30)+",\"readTimes\":{\""+
          std::to_string(baseline.day)+"\":"+std::to_string(baseline.daySeconds+30)+"}}";
      assert(transaction.step()==TimeTransaction::State::ReadingBack);
      assert(transaction.step()==TimeTransaction::State::ReadingBack);
      assert(transaction.step()==TimeTransaction::State::Confirmed);
      for (int i=0;i<50;++i) assert(transaction.step()==TimeTransaction::State::Confirmed);
      assert(transport.entries==1 && transport.reports==1 && requests==4);
      PacedJournal reopened(log); assert(reopened.open(receipt));
      assert(reopened.ledger().remaining()==105*60-30);
    } else {
      assert(transaction.state()==TimeTransaction::State::NotSent);
      assert(log.bytes==original && journal.ledger().remaining()==105*60);
      if (status==200 && transport.query.result()==WeReadTimeCloud::Result::Ready) {
        // Appended issue code: incomplete baseline, not the successful Query::Ready.
        assert(transaction.issue()==TimeTransaction::Issue::BaselineIncomplete);
      }
    }
  }
}
void bootstrapAmbiguity() {
  // Two different causes can produce exactly the same accepted ACK + account
  // aggregates: our report credited, OR our report uncredited and another
  // client credited 30 seconds. Never upgrade this observation to attribution.
  WeReadTimeCloud::Response before, after;
  constexpr uint64_t month=1788192000, day=1789488000, now=1789534986;
  const std::string prior=R"({"baseTime":1788192000,"totalReadTime":100,"readTimes":{"1789401600":100}})";
  const std::string later=R"({"baseTime":1788192000,"totalReadTime":130,"readTimes":{"1789401600":100,"1789488000":30}})";
  before.reset(WeReadTimeCloud::Response::Mode::Stats,month,day);
  after.reset(WeReadTimeCloud::Response::Mode::Stats,month,day);
  assert(before.feed(reinterpret_cast<const uint8_t*>(prior.data()),prior.size()) && before.complete());
  assert(after.feed(reinterpret_cast<const uint8_t*>(later.data()),later.size()) && after.complete());
  assert(!before.snapshot().hasDay && after.snapshot().hasDay);
  assert(after.snapshot().monthSeconds==before.snapshot().monthSeconds+30);
  assert(after.snapshot().daySeconds==30);
  ExternalTime receipt;
  std::strcpy(receipt.identity.account,"test"); std::strcpy(receipt.identity.book,"book");
  std::strcpy(receipt.identity.source,"bootstrap-check"); receipt.identity.day=20708;
  receipt.sourceMs=105*60*1000;
  PacedLedger ledger; assert(ledger.initialize(receipt));
  auto baseline=cloudBaseline(before.snapshot(),receipt.identity,now);
  auto observed=cloudBaseline(after.snapshot(),receipt.identity,now+6);
  assert(ledger.reserve(baseline,now,true));
  assert(ledger.acknowledge(true,now+1));
  // User-approved statistical confirmation, not uniquely attributed credit.
  uint8_t encoded[PacedLedger::kSize]; assert(ledger.encode(encoded));
  PacedLedger restarted; assert(restarted.decode(encoded));
  auto mismatch=observed; mismatch.monthSeconds+=30;
  assert(!restarted.verify(mismatch));
  mismatch=observed; mismatch.complete=false;
  assert(!restarted.verify(mismatch));
  mismatch=observed; mismatch.daySeconds=60;
  assert(!restarted.verify(mismatch));
  assert(restarted.verify(observed) && restarted.remaining()==105*60-30);
  assert(!restarted.verify(observed));
  std::cout<<"Bootstrap statistical confirmation: exact increment, day presence, reboot and no double credit PASS\n";
}
int main(int argc,char** argv) {
  bootstrapAmbiguity();
  // Sept 16 noon China: Sept 15 exists but today's key is absent, matching device evidence.
  constexpr uint64_t now=1789534986;
  run(R"({"baseTime":1788192000,"totalReadTime":100,"readTimes":{"1789401600":100}})",now,false);
  run(R"({"baseTime":1788192000,"totalReadTime":100,"readTimes":{"1789488000":0}})",now,true);
  run(R"({"baseTime":1788192000,"totalReadTime":100,"readTimes":{"1789488000":30}})",now,true);
  run(R"({"baseTime":1788192000,"totalReadTime":100,"readTimes":{}})",now,false);
  run(R"({"baseTime":1788192000,"totalReadTime":100,"readTimes":{"1789516800":30}})",now,false);
  run(R"({"baseTime":1788192000,"totalReadTime":100,"readTimes":)",now,false,200,WeReadTimeCloud::Result::Protocol);
  run(R"({"baseTime":0,"totalReadTime":100,"readTimes":{}})",now,false,200,WeReadTimeCloud::Result::Protocol);
  run("{}",now,false,401,WeReadTimeCloud::Result::LoginRequired);
  run("{}",now,false,403,WeReadTimeCloud::Result::LoginRequired);
  run("{}",now,false,429,WeReadTimeCloud::Result::Unavailable);
  if (argc==4) {
    std::ifstream input(argv[1],std::ios::binary); assert(input.good());
    std::string json((std::istreambuf_iterator<char>(input)),{});
    assert(json.size()<=1024*1024);
    run(json,std::stoull(argv[2]),std::string(argv[3])=="present");
    std::cout<<"Private capture replay passed (4 chunk sizes); real time reports=0\n";
  } else assert(argc==1);
  std::cout<<"Replay integration: 10 scenarios x 4 chunk sizes passed; real network calls=0\n";
}
