#pragma once
#include <cstddef>
#include <cstdlib>

constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
namespace fakeHeap {
inline size_t total = 0;
inline bool fail = false;
inline unsigned caps = 0, allocations = 0, frees = 0;
}  // namespace fakeHeap
inline size_t heap_caps_get_total_size(unsigned) { return fakeHeap::total; }
inline void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
  ++fakeHeap::allocations;
  fakeHeap::caps = caps;
  return fakeHeap::fail ? nullptr : std::calloc(count, size);
}
inline void heap_caps_free(void* ptr) {
  ++fakeHeap::frees;
  std::free(ptr);
}
