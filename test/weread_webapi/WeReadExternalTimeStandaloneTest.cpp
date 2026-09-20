#include <cassert>
#include <cstdio>
#include <fstream>
#include "WeReadExternalTime.h"

int main(int argc, char** argv) {
  using namespace WeReadTime;
  Ledger ledger;
  assert(ledger.bind("account","book","source",20709));
  assert(ledger.collect(6325686));
  ExternalTime receipt;
  receipt.identity = ledger.identity();
  receipt.sourceMs = 6325686;
  receipt.coveredSeconds = 3000;
  receipt.confirmedSeconds = 2880;
  receipt.unknownSeconds = 120;
  uint64_t pending = 999;
  assert(receipt.balance(ledger,pending) && pending == 3325);
  assert(receipt.balance(ledger,pending) && pending == 3325);
  receipt.identity.day++;
  assert(!receipt.balance(ledger,pending) && pending == 0);
  receipt.identity.day--;
  receipt.confirmedSeconds++;
  assert(!receipt.balance(ledger,pending));
  receipt.confirmedSeconds--;
  receipt.sourceMs++;
  assert(!receipt.balance(ledger,pending));
  receipt.sourceMs--;
  uint8_t frame[240] = {};
  assert(!receipt.decode(frame,sizeof(frame)));
  assert(ledger.encode(frame,sizeof(frame)));
  std::memcpy(frame,"WRTX",4);
  auto put = [&](size_t at, uint64_t value) {
    for (unsigned i=0;i<8;++i) frame[at+i]=static_cast<uint8_t>(value>>(i*8));
  };
  put(184,3000); put(192,2880); put(200,120);
  put(232,ExternalTime::checksum(frame,232));
  assert(receipt.decode(frame,sizeof(frame)));
  assert(receipt.balance(ledger,pending) && pending==3325);
  frame[200] ^= 1;
  assert(!receipt.decode(frame,sizeof(frame)));
  assert(receipt.balance(ledger,pending) && pending==3325);
  if (argc == 2) {
    std::ifstream file(argv[1],std::ios::binary);
    file.read(reinterpret_cast<char*>(frame),sizeof(frame));
    assert(file.gcount()==sizeof(frame) && file.peek()==EOF);
    ExternalTime exported;
    assert(exported.decode(frame,sizeof(frame)));
    Ledger device;
    const auto& identity=exported.identity;
    assert(device.bind(identity.account,identity.book,identity.source,identity.day));
    assert(device.collect(exported.sourceMs));
    assert(exported.balance(device,pending));
    assert(exported.confirmedSeconds==2880 && exported.unknownSeconds==120 && pending==3325);
    std::puts("Actual Rust export decoded by firmware C++: confirmed=2880 unknown=120 pending=3325 PASS");
  }
  std::puts("External confirmed/unknown exclusion, identity and counter guards: PASS");
}
