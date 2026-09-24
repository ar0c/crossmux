#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
// Hash boundary stub; manifest codec/hash integration has its own tests.
struct mbedtls_sha256_context {};
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context*, int) { return 0; }
inline int mbedtls_sha256_update(mbedtls_sha256_context*, const uint8_t*, size_t) { return 0; }
inline int mbedtls_sha256_finish(mbedtls_sha256_context*, uint8_t* out) {
  std::memset(out, 0xaa, 32);
  return 0;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
