#pragma once

#include <cstddef>
#include <cstdint>

#if __has_include(<mbedtls/sha256.h>)
#include <mbedtls/sha256.h>
#else
#include <psa/crypto.h>
#endif

class HalSha256 {
 public:
#if __has_include(<mbedtls/sha256.h>)
  HalSha256() { mbedtls_sha256_init(&context_); }
  ~HalSha256() { mbedtls_sha256_free(&context_); }

  bool start() { return mbedtls_sha256_starts(&context_, 0) == 0; }
  bool update(const uint8_t* data, size_t length) { return mbedtls_sha256_update(&context_, data, length) == 0; }
  bool finish(uint8_t (&digest)[32]) { return mbedtls_sha256_finish(&context_, digest) == 0; }
#else
  HalSha256() = default;
  ~HalSha256() { psa_hash_abort(&operation_); }

  bool start() {
    return psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&operation_, PSA_ALG_SHA_256) == PSA_SUCCESS;
  }
  bool update(const uint8_t* data, size_t length) { return psa_hash_update(&operation_, data, length) == PSA_SUCCESS; }
  bool finish(uint8_t (&digest)[32]) {
    size_t length = 0;
    return psa_hash_finish(&operation_, digest, sizeof(digest), &length) == PSA_SUCCESS && length == sizeof(digest);
  }
#endif

  HalSha256(const HalSha256&) = delete;
  HalSha256& operator=(const HalSha256&) = delete;

  static bool compute(const uint8_t* data, size_t size, uint8_t (&digest)[32]) {
    HalSha256 hash;
    return hash.start() && hash.update(data, size) && hash.finish(digest);
  }

 private:
#if __has_include(<mbedtls/sha256.h>)
  mbedtls_sha256_context context_;
#else
  psa_hash_operation_t operation_ = PSA_HASH_OPERATION_INIT;
#endif
};
