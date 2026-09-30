#pragma once
#include "WeReadHttpClient.h"
#include "WeReadServiceClient.h"
#include "WeReadStore.h"

namespace WeReadClient {
// No additional heap buffers. The owning Operation supplies its existing
// 4 KiB receive buffer and sinks. Identity pointers reference the owner's
// existing account and unused legacy scratch; secrets are read on the stack.
class ManagedWeReadClient {
 public:
  struct Metadata {
    char context[33]{};
    char state[24]{};
    char job[65]{}, book[64]{};
    uint32_t nextCursor = 0;
    bool capability = false;
    bool accepted = false;
    bool complete = false;
  };
  static bool required();
  static bool loadAccount(WeReadStore::Session& session);
  bool connect(uint8_t* buffer, size_t size, char* account, size_t accountSize, char* device, size_t deviceSize);
  WeReadHttpClient::Result request(const char* path, const uint8_t* body, size_t length, uint8_t* buffer, size_t size,
                                   const WeReadHttpClient::DataCallback& sink, int& status,
                                   uint32_t* nextCursor = nullptr) const;
  bool metadata(const char* path, const uint8_t* body, size_t length, uint8_t* buffer, size_t size,
                Metadata& out) const;
  static bool resourceID(const char* url, char out[65]);

 private:
  const char* account_ = nullptr;
  const char* device_ = nullptr;
};
}  // namespace WeReadClient
