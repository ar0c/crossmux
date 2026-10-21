#pragma once
#include <cstddef>
namespace fakeTls {
inline int result = 0;
inline unsigned installs = 0;
inline void* (*allocate)(size_t, size_t) = nullptr;
inline void (*release)(void*) = nullptr;
}  // namespace fakeTls
inline int mbedtls_platform_set_calloc_free(void* (*allocate)(size_t, size_t), void (*release)(void*)) {
  ++fakeTls::installs;
  if (fakeTls::result == 0) {
    fakeTls::allocate = allocate;
    fakeTls::release = release;
  }
  return fakeTls::result;
}
