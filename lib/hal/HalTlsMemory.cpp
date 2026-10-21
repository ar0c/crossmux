#include "HalTlsMemory.h"

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR)
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include <cstdint>

namespace {
bool externalTls = false;
void* tlsCalloc(size_t count, size_t size) {
  if (size != 0 && count > SIZE_MAX / size) return nullptr;
  // TLS owns variable-sized, zeroed CPU buffers until context cleanup. They
  // cannot live on a task stack/static arena. Espressif supports external TLS
  // memory; explicitly selecting it avoids the pinned SDK's INTERNAL allocator.
  // Do not fall back to internal RAM and consume the resumed reader's reserve.
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
}  // namespace

bool HalTlsMemory::initialize() {
  if (externalTls) return true;
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0) return false;
  externalTls = mbedtls_platform_set_calloc_free(tlsCalloc, heap_caps_free) == 0;
  return externalTls;
}
bool HalTlsMemory::usesPsram() { return externalTls; }
#else
bool HalTlsMemory::initialize() { return false; }
bool HalTlsMemory::usesPsram() { return false; }
#endif
