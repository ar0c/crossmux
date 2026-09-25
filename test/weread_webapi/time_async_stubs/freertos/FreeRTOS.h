#pragma once
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#define taskENTER_CRITICAL(p) (p)->lock()
#define taskEXIT_CRITICAL(p) (p)->unlock()
#define pdMS_TO_TICKS(v) (v)
constexpr int pdPASS = 1;
constexpr int pdTRUE = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
