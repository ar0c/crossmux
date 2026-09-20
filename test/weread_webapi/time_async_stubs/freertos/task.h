#pragma once
#include <chrono>
#include <thread>

#include "Arduino.h"
using TaskHandle_t = void*;
namespace fakeTask {
inline bool failCreate = false;
inline std::thread worker;
inline void join() {
  if (worker.joinable()) worker.join();
}
}  // namespace fakeTask
inline int xTaskCreate(void (*fn)(void*), const char*, unsigned, void* arg, unsigned, TaskHandle_t* task) {
  if (fakeTask::failCreate) return 0;
  fakeTask::join();
  fakeTask::worker = std::thread(fn, arg);
  *task = reinterpret_cast<void*>(1);
  return 1;
}
inline void vTaskDelay(unsigned delay) {
  if (delay >= 50) testTick.fetch_add(1000);
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
inline void vTaskDelete(void*) {}
inline unsigned uxTaskGetStackHighWaterMark(void*) { return 4096; }
