#include "WeReadServiceJournal.h"
#include <cassert>
#include <vector>
#include <iostream>
using namespace WeReadTime;
struct Log : ByteLog {
  std::vector<uint8_t> bytes;bool torn=false;
  ReadState size(uint64_t& n)override{n=bytes.size();return n?ReadState::Ready:ReadState::Missing;}
  bool read(uint64_t o,uint8_t* b,size_t n)override{if(o+n>bytes.size())return false;std::memcpy(b,bytes.data()+o,n);return true;}
  bool appendAndSync(const uint8_t* b,size_t n)override{bytes.insert(bytes.end(),b,b+(torn?n/2:n));return !torn;}
};
int main(){
  Ledger v;assert(v.bind("123","26435427","source",20716));const auto id=v.identity();
  Log log;ServiceJournal j(log);
  assert(j.open(id,90,690)&&j.owned()==0&&j.reserve("abcdef012345678901234567"));
  assert(j.owned()==600&&j.confirmed()==0&&j.pending()==600);
  char first[128],again[128];assert(j.jobId(first,sizeof(first)));
  ServiceJournal reboot(log);assert(reboot.open(id,90,720)&&reboot.state()==ServiceJournal::State::Reserved);
  assert(reboot.jobId(again,sizeof(again))&&!std::strcmp(first,again));
  assert(!reboot.reserve("abcdef012345678901234567"));
  assert(!reboot.matchesDevice("000000000000000000000000"));
  assert(reboot.accept(false)&&reboot.confirmed()==0);
  ServiceJournal ack(log);assert(ack.open(id,90,720)&&ack.state()==ServiceJournal::State::Accepted);
  assert(ack.accept(true)&&ack.confirmed()==600&&ack.pending()==0);
  assert(ack.reserve("abcdef012345678901234567")&&ack.start()==690&&ack.end()==720);
  assert(ack.accept(true)&&ack.confirmed()==630);
  ServiceJournal changed(log);auto other=id;std::strcpy(other.book,"999");
  assert(!changed.open(other,90,720));assert(!changed.open(id,91,720));assert(!changed.open(id,90,719));
  assert(changed.open(id,90,750));log.torn=true;assert(!changed.reserve("abcdef012345678901234567"));
  ServiceJournal torn(log);assert(!torn.open(id,90,750));
  Log corrupt;corrupt.bytes=log.bytes;corrupt.bytes.resize(ServiceJournal::kSize);corrupt.bytes[216]^=1;
  ServiceJournal crc(corrupt);assert(!crc.open(id,90,750));
  std::cout<<"PASS service journal: restart, stable retry, no ACK credit, range/account/device guards, torn tail\n";
}
