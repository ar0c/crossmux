#include <HalTlsMemory.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include <cassert>
#include <cstdint>

int main() {
  assert(!HalTlsMemory::usesPsram());
  assert(!HalTlsMemory::initialize());
  assert(fakeTls::installs == 0);
  fakeHeap::total = 8 * 1024 * 1024;
#ifdef BOARD_HAS_PSRAM
  fakeTls::result = -1;
  assert(!HalTlsMemory::initialize());
  assert(!HalTlsMemory::usesPsram());
  fakeTls::result = 0;
  assert(HalTlsMemory::initialize());
  assert(HalTlsMemory::usesPsram());
  const auto installs = fakeTls::installs;
  assert(HalTlsMemory::initialize());
  assert(fakeTls::installs == installs);  // No live global allocator switching.
  auto* bytes = static_cast<unsigned char*>(fakeTls::allocate(2, 16384));
  assert(bytes && fakeHeap::caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  for (size_t i = 0; i < 32768; ++i) assert(bytes[i] == 0);
  fakeTls::release(bytes);
  assert(fakeHeap::frees == 1);
  const auto allocations = fakeHeap::allocations;
  assert(!fakeTls::allocate(SIZE_MAX, 2));
  assert(fakeHeap::allocations == allocations);
  fakeHeap::fail = true;
  assert(!fakeTls::allocate(1, 16384));  // No hidden internal fallback.
  assert(fakeHeap::allocations == allocations + 1);
#else
  assert(!HalTlsMemory::initialize());
  assert(!HalTlsMemory::usesPsram() && fakeTls::installs == 0);
#endif
}
