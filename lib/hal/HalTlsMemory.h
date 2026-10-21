#pragma once

class HalTlsMemory {
 public:
  // Call once during setup, before any networking task or TLS allocation.
  // Never change the global allocator while a TLS context may be live.
  static bool initialize();
  static bool usesPsram();
};
