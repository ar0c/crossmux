#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
// Hash boundary stub; manifest codec/hash integration has its own tests.
inline int mbedtls_sha256(const uint8_t*, size_t, uint8_t* out, int) {
  std::memset(out, 0xaa, 32);
  return 0;
}
