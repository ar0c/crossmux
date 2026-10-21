#include <algorithm>
#include <cassert>
#include <cstdint>

#include "StandbySchedule.h"

constexpr uint32_t kMaxLightSleepSeconds = 60;
uint32_t nowMs = 35000;
uint32_t millis() { return nowMs; }
bool backgroundSync = false, pendingSwitch = false, usb = false;
bool environmentSupported = true, hardwareAllowed = true;
int soundStops = 0, faceTicks = 0, updates = 0, sleeps = 0, renderLocks = 0;
uint32_t sleptSeconds = 0;
struct RenderLock {
  RenderLock() { ++renderLocks; }
  ~RenderLock() { --renderLocks; }
};
struct MappedInputManager {
  enum class Button : uint8_t { Back, Confirm, Left, Right, Up, Down, Power, Count };
  static constexpr uint8_t kButtonCount = static_cast<uint8_t>(Button::Count);
  bool held[kButtonCount]{};
  bool isPressed(Button button) const { return held[static_cast<uint8_t>(button)]; }
};
struct {
  bool isSwitchPending() const { return pendingSwitch; }
} activityManager;
namespace WeReadTimeSync {
bool active() { return backgroundSync; }
}  // namespace WeReadTimeSync
constexpr int WIFI_MODE_NULL = 0;
struct {
  int mode = 0;
  int getMode() const { return mode; }
} WiFi;
struct {
  bool isUsbConnected() const { return usb; }
} gpio;
namespace SoundFeedback {
void shutdown() {
  assert(renderLocks == 1);
  ++soundStops;
}
}  // namespace SoundFeedback
namespace HalEnvironmentSensor {
bool supported() { return environmentSupported; }
}  // namespace HalEnvironmentSensor
struct HalPowerManager {
  enum class LightSleepWakeReason : uint8_t { Timer, PowerButton, Input, Failed };
  LightSleepWakeReason reason = LightSleepWakeReason::Timer;
  bool canStandbyLightSleep(...) const { return hardwareAllowed; }
  LightSleepWakeReason lightSleepFor(uint32_t seconds) {
    assert(renderLocks == 1);
    ++sleeps;
    sleptSeconds = seconds;
    return reason;
  }
} powerManager;
namespace standby_time {
bool shouldLightSleep(uint32_t idle, bool syncing, bool wifi, bool connected, bool allowed) {
  return idle >= 35000 && !syncing && !wifi && !connected && allowed;
}
}  // namespace standby_time
struct Face {
  uint32_t seconds = 45;
  uint32_t secondsUntilNextWake() const { return seconds; }
} face;
struct StandbyActivity {
  enum class DisplayMode { Normal, Immersive };
  enum class SyncState { Idle, Delayed };
  DisplayMode mode_ = DisplayMode::Immersive;
  SyncState syncState_ = SyncState::Idle;
  Face* currentFace_ = &face;
  uint32_t lastInputMs_ = 0, environmentPollMs_ = 0;
  bool environmentSampled_ = true;
  MappedInputManager mappedInput;
  bool tryLightSleep(uint32_t idleMs);
  void processFaceTick(bool wait) {
    assert(wait);
    ++faceTicks;
  }
  void requestUpdate() { ++updates; }
};

// The harness inserts the production tryLightSleep() method here.
// PRODUCTION_METHOD

void reset() {
  assert(renderLocks == 0);
  backgroundSync = pendingSwitch = usb = false;
  hardwareAllowed = environmentSupported = true;
  soundStops = faceTicks = updates = sleeps = 0;
  WiFi.mode = 0;
  powerManager.reason = HalPowerManager::LightSleepWakeReason::Timer;
}

int main() {
  StandbyActivity activity;
  assert(!activity.tryLightSleep(34999));
  assert(sleeps == 0 && soundStops == 0);
  assert(activity.tryLightSleep(35000));
  assert(sleeps == 1 && soundStops == 1 && faceTicks == 1 && sleptSeconds == 25);
  assert(activity.lastInputMs_ == 0);  // timer wake immediately resumes sleeping after refresh
  for (int guard = 0; guard < 8; ++guard) {
    reset();
    activity = {};
    if (guard == 0) backgroundSync = true;
    if (guard == 1) pendingSwitch = true;
    if (guard == 2) usb = true;
    if (guard == 3) WiFi.mode = 1;
    if (guard == 4) hardwareAllowed = false;
    if (guard == 5) activity.syncState_ = StandbyActivity::SyncState::Delayed;
    if (guard == 6) activity.mode_ = StandbyActivity::DisplayMode::Normal;
    if (guard == 7) activity.currentFace_ = nullptr;
    assert(!activity.tryLightSleep(35000));
    assert(sleeps == 0 && soundStops == 0);
  }
  for (uint8_t button = 0; button < MappedInputManager::kButtonCount; ++button) {
    reset();
    activity = {};
    activity.mappedInput.held[button] = true;
    assert(!activity.tryLightSleep(35000));  // includes PMIC Power after its IRQ was acknowledged
    assert(activity.mappedInput.held[button] && sleeps == 0);
  }
  for (const auto reason :
       {HalPowerManager::LightSleepWakeReason::Input, HalPowerManager::LightSleepWakeReason::Failed}) {
    reset();
    activity = {};
    powerManager.reason = reason;
    assert(activity.tryLightSleep(35000));
    assert(activity.lastInputMs_ == nowMs && updates == 0 && faceTicks == 0);
    assert(activity.mode_ == StandbyActivity::DisplayMode::Immersive);
  }
}
