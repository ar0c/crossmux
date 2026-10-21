#pragma once
#include <cstddef>
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_INTERNAL = 2, MALLOC_CAP_8BIT = 4;
namespace fakePsram {
inline bool available = false, fail = false, defaultEligible = true;
inline unsigned allocations = 0;
inline size_t freeBytes = 8 * 1024 * 1024, largestBlock = 8 * 1024 * 1024;
}  // namespace fakePsram
inline size_t heap_caps_get_free_size(unsigned) { return fakePsram::available ? fakePsram::freeBytes : 0; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return fakePsram::available ? fakePsram::largestBlock : 0; }
inline void* heap_caps_calloc(size_t count, size_t size, unsigned) {
  if (fakePsram::fail) return nullptr;
  ++fakePsram::allocations;
  return std::calloc(count, size);
}
inline void* heap_caps_malloc(size_t size, unsigned) { return heap_caps_calloc(1, size, 0); }
