#pragma once
#include <HalStorage.h>
#include "WeReadServiceJournal.h"
namespace fakeService { inline bool authenticated=true,confirmed=false,fail=false; inline unsigned requests=0; }
namespace WeReadTime {
class ServiceClient {
 public:
  enum class Result { Accepted, Confirmed, Review, Failed };
  static bool configured(){return Storage.exists("/WeReadSync/service.conf");}
  bool connect(const char*){return fakeService::authenticated;}
  const char* device()const{return "abcdef012345678901234567";}
  Result exchange(const Identity&,ServiceJournal& j){
    ++fakeService::requests;
    if(fakeService::fail)return Result::Failed;
    if(!j.accept(fakeService::confirmed))return Result::Failed;
    return fakeService::confirmed?Result::Confirmed:Result::Accepted;
  }
};
}
