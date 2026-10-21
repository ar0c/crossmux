#include "DebugBridge.h"

#if !CROSSMUX_USB_DEBUG || !FREEINK_DEVICE_WAVESHARE_EPAPER_397
#error "DebugBridge must only be compiled by the explicit Waveshare USB debug target"
#endif

#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "DebugProtocol.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/apps/weread/webapi/WeReadTimeSync.h"

namespace debugbridge {
namespace {
constexpr char identity[] = "CROSSMUX-USB-DEBUG-V1;";
LineBuffer input;
bool keepAwake = true;
uint32_t timeoutOverride = 0;
uint32_t session = 0, lastSequence = 0, pending = 0, pendingSince = 0, quietSince = 0;
uint32_t quietFrame = 0;
bool applied = false;

void error(uint32_t requestSession, uint32_t sequence, const char* reason) {
  logSerial.printf("\nDBG1 {\"session\":%lu,\"id\":%lu,\"event\":\"error\",\"reason\":\"%s\"}\n",
                   static_cast<unsigned long>(requestSession), static_cast<unsigned long>(sequence), reason);
}

void status(uint32_t sequence, const char* event) {
  const auto ram = HalMemory::getInternalHeap();
  const auto psram = HalMemory::getPsramHeap();
  WeReadTimeSync::Status sync;
  const bool syncAvailable = WeReadTimeSync::debugStatus(sync);
  // Activity names are code identifiers, never book titles, paths or credentials.
  char activity[64]{};
  const char* name = activityManager.debugActivityName();
  for (size_t i = 0; i < sizeof(activity) - 1 && name[i]; ++i) {
    const char c = name[i];
    activity[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
  }
  logSerial.printf(
      "\nDBG1 {\"session\":%lu,\"id\":%lu,\"event\":\"%s\",\"marker\":\"%s\",\"version\":\"%s\","
      "\"uptime_ms\":%lu,\"activity\":\"%s\",\"frame\":%lu,\"ready\":%u,\"usb\":%u,\"keep_awake\":%u,"
      "\"pending\":%lu,\"sleep_timeout_ms\":%lu,\"normal_sleep_timeout_ms\":%lu,"
      "\"ram_free\":%u,\"ram_largest\":%u,\"psram_free\":%u,\"psram_largest\":%u,"
      "\"sync_available\":%u,\"sync_active\":%u,\"sync_revision\":%lu,\"sync_queue\":%u,\"sync_phase\":%u,"
      "\"sync_issue\":%u,\"sync_start_failure\":%u}\n",
      static_cast<unsigned long>(session), static_cast<unsigned long>(sequence), event, identity, CROSSPOINT_VERSION,
      millis(), activity, static_cast<unsigned long>(activityManager.debugFrame()), activityManager.debugReady(),
      gpio.isUsbConnected(), keepAwake, static_cast<unsigned long>(pending),
      static_cast<unsigned long>(sleepTimeout(SETTINGS.getSleepTimeoutMs())),
      static_cast<unsigned long>(SETTINGS.getSleepTimeoutMs()), static_cast<unsigned>(ram.freeBytes),
      static_cast<unsigned>(ram.largestBlockBytes), static_cast<unsigned>(psram.freeBytes),
      static_cast<unsigned>(psram.largestBlockBytes), syncAvailable, WeReadTimeSync::active(),
      static_cast<unsigned long>(sync.revision), static_cast<unsigned>(sync.queue), static_cast<unsigned>(sync.phase),
      static_cast<unsigned>(sync.issue), static_cast<unsigned>(WeReadTimeSync::lastStartFailure()));
}

void screenshot(uint32_t sequence) {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || !activityManager.debugReady()) {
    error(session, sequence, "render_busy");
    return;
  }
  display.waitRefreshComplete();
  const auto length = display.getBufferSize();
  const auto* pixels = display.getFrameBuffer();
  if (!pixels) {
    error(session, sequence, "no_framebuffer");
    return;
  }
  logSerial.printf(
      "\nDBG1 {\"session\":%lu,\"id\":%lu,\"event\":\"image\",\"bytes\":%lu,\"width\":%u,\"height\":%u,"
      "\"checksum\":%lu,\"frame\":%lu,\"inverted\":%u}\n",
      static_cast<unsigned long>(session), static_cast<unsigned long>(sequence), static_cast<unsigned long>(length),
      HalDisplay::DISPLAY_WIDTH, HalDisplay::DISPLAY_HEIGHT, static_cast<unsigned long>(checksum(pixels, length)),
      static_cast<unsigned long>(activityManager.debugFrame()), display.isInverted());
  if (logSerial.write(pixels, length) != length) {
    error(session, sequence, "short_image_write");
    return;
  }
  status(sequence, "done");
}

bool button(const char* name, HalGPIO::DebugButton& result) {
  using Button = HalGPIO::DebugButton;
  constexpr const char* names[] = {"UP", "PRESS", "DOWN", "BOOT", "PWR"};
  constexpr Button buttons[] = {Button::RockerUp, Button::RockerPress, Button::RockerDown, Button::Boot, Button::Power};
  for (size_t i = 0; i < 5; ++i) {
    if (!std::strcmp(name, names[i])) {
      result = buttons[i];
      return true;
    }
  }
  return false;
}

void dispatch(const Command& command) {
  if (!std::strcmp(command.verb, "HELLO") && command.fields == 3 && command.sequence == 1) {
    if (pending || command.session == session) {
      error(command.session, command.sequence, "busy_or_duplicate_session");
      return;
    }
    session = command.session;
    lastSequence = 1;
    status(1, "done");
    return;
  }
  if (command.session != session || command.sequence <= lastSequence) {
    error(command.session, command.sequence, "session_or_sequence");
    return;
  }
  lastSequence = command.sequence;  // Invalid requests are consumed too; never replay a write.
  if (!std::strcmp(command.verb, "STATUS") && command.fields == 3) {
    status(command.sequence, "done");
    return;
  }
  if (pending) {
    error(session, command.sequence, "operation_pending");
    return;
  }
  if (!std::strcmp(command.verb, "KEEP") && command.fields == 4 &&
      (!std::strcmp(command.arg, "0") || !std::strcmp(command.arg, "1"))) {
    keepAwake = command.arg[0] == '1';
    status(command.sequence, "done");
    return;
  }
  if (!std::strcmp(command.verb, "SCREEN") && command.fields == 3) {
    screenshot(command.sequence);
    return;
  }
  uint32_t timeout = 0;
  if (!std::strcmp(command.verb, "TIMEOUT") && command.fields == 4 && number(command.arg, timeout) &&
      (timeout == 0 || (timeout >= 1000 && timeout <= 600000))) {
    timeoutOverride = timeout;
    status(command.sequence, "done");
    return;
  }
  HalGPIO::DebugButton key;
  uint32_t duration = 0;
  if (!std::strcmp(command.verb, "PULSE") && command.fields == 5 && button(command.arg, key) &&
      number(command.duration, duration)) {
    if (!activityManager.debugReady() || !gpio.debugPulse(key, duration)) {
      error(session, command.sequence, "busy_or_invalid_pulse");
      return;
    }
    pending = command.sequence;
    pendingSince = millis();
    quietSince = 0;
    applied = false;
    status(command.sequence, "accepted");
    return;
  }
  error(session, command.sequence, "invalid_command");
}
}  // namespace

bool preventAutoSleep() { return keepAwake && gpio.isUsbConnected(); }
uint32_t sleepTimeout(uint32_t normalMs) { return timeoutOverride ? timeoutOverride : normalMs; }

void poll() {
  if (pending) {
    switch (gpio.debugInputResult()) {
      case DebugInputState::Result::Complete:
        if (!applied) {
          applied = true;
          status(pending, "applied");
        }
        if (!activityManager.debugReady()) {
          quietSince = 0;
        } else if (quietSince == 0 || quietFrame != activityManager.debugFrame()) {
          quietSince = millis();
          quietFrame = activityManager.debugFrame();
        } else if (millis() - quietSince >= 200) {
          const auto sequence = pending;
          pending = 0;
          status(sequence, "done");
        }
        break;
      case DebugInputState::Result::Cancelled:
        error(session, pending, "input_cancelled");
        pending = 0;
        break;
      case DebugInputState::Result::Idle:
      case DebugInputState::Result::Pending:
        break;
    }
    if (pending && millis() - pendingSince > 15000) {
      error(session, pending, "operation_timeout_outcome_unknown");
      pending = 0;
    }
  }
  // Bounded work per UI tick; no String allocations or read-until-newline blocking.
  for (unsigned i = 0; i < 64 && logSerial.available() > 0; ++i) {
    const int c = logSerial.read();
    if (c < 0 || !input.feed(static_cast<char>(c), millis())) continue;
    Command command;
    if (parse(input.line(), command))
      dispatch(command);
    else
      error(0, 0, "invalid_frame");
  }
}
}  // namespace debugbridge
