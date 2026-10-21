#pragma once
#include <cstdint>
extern unsigned delayMs;
inline void delay(unsigned ms) { delayMs += ms; }
