#pragma once
#include <algorithm>
#include <cstddef>

#include "Arduino.h"
#ifdef BOARD_HAS_PSRAM
#include <esp_heap_caps.h>
#endif
class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes, totalBytes, minFreeBytes, largestBlockBytes;
  };
  static HeapStats getInternalHeap() { return {ESP.free, ESP.free, ESP.free, ESP.largest}; }
  static HeapStats getDefaultHeap() {
    auto heap = getInternalHeap();
#ifdef BOARD_HAS_PSRAM
    if (fakePsram::available && fakePsram::defaultEligible) {
      heap.freeBytes += fakePsram::freeBytes;
      heap.largestBlockBytes = std::max(heap.largestBlockBytes, fakePsram::largestBlock);
    }
#endif
    return heap;
  }
  static HeapStats getPsramHeap() {
#ifdef BOARD_HAS_PSRAM
    return {heap_caps_get_free_size(MALLOC_CAP_SPIRAM), 0, 0, heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)};
#else
    return {};
#endif
  }
};
