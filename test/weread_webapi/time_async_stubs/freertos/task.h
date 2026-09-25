#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Arduino.h"
using TaskHandle_t = void*;
namespace fakeTask {
inline bool failCreate = false;
inline uint32_t largestAfterCreate = 0;
inline std::mutex gateMutex;
inline std::condition_variable gate;
inline bool notified = false;
inline bool canceled = false;
inline std::thread worker;
inline void join() {
  if (worker.joinable()) worker.join();
}
}  // namespace fakeTask
inline int xTaskCreate(void (*fn)(void*), const char*, unsigned, void* arg, unsigned, TaskHandle_t* task) {
  if (fakeTask::failCreate) return 0;
  fakeTask::join();
  {
    std::lock_guard<std::mutex> lock(fakeTask::gateMutex);
    fakeTask::notified = false;
    fakeTask::canceled = false;
  }
  if (fakeTask::largestAfterCreate) ESP.largest = fakeTask::largestAfterCreate;
  fakeTask::worker = std::thread(fn, arg);
  *task = reinterpret_cast<void*>(1);
  return 1;
}
inline void vTaskDelay(unsigned delay) {
  if (delay >= 50) testTick.fetch_add(1000);
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
inline unsigned ulTaskNotifyTake(int, uint32_t) {
  std::unique_lock<std::mutex> lock(fakeTask::gateMutex);
  fakeTask::gate.wait(lock, [] { return fakeTask::notified || fakeTask::canceled; });
  if (fakeTask::canceled) return 0;
  fakeTask::notified = false;
  return 1;
}
inline void xTaskNotifyGive(TaskHandle_t) {
  {
    std::lock_guard<std::mutex> lock(fakeTask::gateMutex);
    fakeTask::notified = true;
  }
  fakeTask::gate.notify_all();
}
inline void vTaskDelete(void* task) {
  if (!task) return;
  {
    std::lock_guard<std::mutex> lock(fakeTask::gateMutex);
    fakeTask::canceled = true;
  }
  fakeTask::gate.notify_all();
  fakeTask::join();
}
inline unsigned uxTaskGetStackHighWaterMark(void*) { return 4096; }
