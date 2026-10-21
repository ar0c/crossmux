#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <M5Unified.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_mac.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>

#include "Accessories.h"
#include "Gesture.h"

namespace {
constexpr char kName[] = "StickS3 CrossMux";
constexpr char kMarker[] = "STICKS3-REMOTE-USB-V1";
constexpr char kVersion[] = "0.2.3";
constexpr size_t kUsbTxBytes = 2048;
constexpr uint32_t kReportHoldMs = 70;
// Keyboard report ID 1; payload remains 8 bytes, ID is in Report Reference.
uint8_t reportMap[] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
                       0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
                       0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0};
std::atomic<bool> connected{false}, encrypted{false};
std::atomic<bool> notifications{false};
std::atomic<uint32_t> notifyCallbacks{0}, notifyErrors{0}, connections{0};
BLEServer* server = nullptr;
BLECharacteristic* input = nullptr;
BLEHIDDevice* hid = nullptr;
bool initialized = false;
bool usbInitialized = false;
char mac[18] = {};
uint32_t session = 0, sequence = 0, issued = 0, completed = 0, dropped = 0;
uint32_t reportsSubmitted = 0;
uint32_t releaseAt = 0, virtualStarted = 0, virtualDuration = 0;
char virtualButton = 0;
Action active = Action::None, last = Action::None;
Gesture gestureA, gestureB;
Accessories accessories;

bool subscribed() { return connected && notifications; }
bool ready() { return initialized && connected && encrypted && subscribed(); }
bool busy() { return active != Action::None || virtualButton != 0 || accessories.busy(); }

class ServerCallbacks final : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    encrypted = false;
    notifications = false;
    connected = true;
    ++connections;
  }
  void onDisconnect(BLEServer*) override {
    connected = false;
    encrypted = false;
    notifications = false;
  }
} serverCallbacks;

class SecurityCallbacks final : public BLESecurityCallbacks {
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return false; }
  uint32_t onPassKeyRequest() override { return 0; }
  void onAuthenticationComplete(ble_gap_conn_desc* result) override {
    encrypted = result && result->sec_state.encrypted;
  }
} securityCallbacks;

class ReportCallbacks final : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc*, uint16_t value) override {
    notifications = (value & 1) != 0;
  }
  void onStatus(BLECharacteristic*, Status status, uint32_t) override {
    // Arduino 3.3.7 calls this for both submission and notify_tx completion.
    // This is callback telemetry, never a report count or receiver ACK.
    if (status == SUCCESS_NOTIFY)
      ++notifyCallbacks;
    else
      ++notifyErrors;
  }
} reportCallbacks;

void status(uint32_t requestSession, uint32_t id, const char* event = "done", const char* reason = "") {
  // Fixed application storage avoids Print::printf's per-response malloc.
  // HWCDC truncated these ~550-byte replies even with an enlarged queue in
  // the dual-USB hardware test. Use the pinned ESP-IDF driver as sole USB owner.
  static char reply[1024];
  const int length =
      snprintf(reply, sizeof(reply),
               "\nREM1 {\"session\":%lu,\"id\":%lu,\"event\":\"%s\",\"reason\":\"%s\","
               "\"marker\":\"%s\",\"version\":\"%s\",\"model\":\"StickS3\",\"mac\":\"%s\","
               "\"connected\":%d,\"encrypted\":%d,\"subscribed\":%d,\"ready\":%d,\"busy\":%d,"
               "\"issued\":%lu,\"completed\":%lu,\"dropped\":%lu,\"reports_submitted\":%lu,\"notify_callbacks\":%lu,"
               "\"notify_errors\":%lu,"
               "\"connections\":%lu,\"last_action\":\"%s\",\"heap\":%lu,\"psram\":%lu,\"uptime_ms\":%lu,"
               "\"joy_axis_mode\":\"swap_xy_invert_y\",\"joy_online\":%d,\"joy_fw\":%u,\"joy_x\":%d,\"joy_y\":%d,\"joy_"
               "pressed\":%d,"
               "\"joy_reads\":%lu,\"joy_failures\":%lu,\"joy_events\":%lu,\"haptic_ready\":%d,\"haptic_active\":%d,"
               "\"haptic_pulses\":%lu}\n",
               (unsigned long)requestSession, (unsigned long)id, event, reason, kMarker, kVersion, mac,
               int(connected.load()), int(encrypted.load()), int(subscribed()), int(ready()), int(busy()),
               (unsigned long)issued, (unsigned long)completed, (unsigned long)dropped, (unsigned long)reportsSubmitted,
               (unsigned long)notifyCallbacks.load(), (unsigned long)notifyErrors.load(),
               (unsigned long)connections.load(), actionName(last), (unsigned long)ESP.getFreeHeap(),
               (unsigned long)ESP.getFreePsram(), (unsigned long)millis(), int(accessories.online),
               unsigned(accessories.firmware), int(accessories.x), int(accessories.y), int(accessories.pressed),
               (unsigned long)accessories.reads, (unsigned long)accessories.failures, (unsigned long)accessories.events,
               int(accessories.pwmReady), int(accessories.motorActive), (unsigned long)accessories.pulses);
  if (length <= 0 || size_t(length) >= sizeof(reply)) return;
  // Never block the key-release scheduler behind an unplugged or stalled USB host.
  // A missing reply is an explicit unknown outcome to the client, never retried.
  if (!usbInitialized) return;
  // Zero wait: copy into the driver's bounded ring, never wait on a USB host.
  // A short write leaves an unknown outcome; the host never replays a mutation.
  usb_serial_jtag_write_bytes(reply, size_t(length), 0);
}

void sendReport(uint8_t key) {
  const uint8_t bytes[8] = {0, 0, key, 0, 0, 0, 0, 0};
  input->setValue(bytes, sizeof(bytes));
  ++reportsSubmitted;
  input->notify();  // Stack submission only. Receiver acceptance requires receiver evidence.
}

void queue(Action action, uint32_t now) {
  if (action == Action::None) return;
  if (!ready() || active != Action::None) {
    ++dropped;
    return;
  }
  active = last = action;
  ++issued;
  sendReport(hidCode(action));
  accessories.pulse(now);  // Local submission feedback; never a receiver ACK.
  releaseAt = now + kReportHoldMs;
}

bool number(const char* text, int base, uint32_t& out) {
  if (!text || !*text || *text == '-' || *text == '+') return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = strtoul(text, &end, base);
  if (errno || *end || !parsed || parsed > UINT32_MAX) return false;
  out = uint32_t(parsed);
  return true;
}

void command(char* line) {
  char* context = nullptr;
  const char* prefix = strtok_r(line, " ", &context);
  const char* sessionText = strtok_r(nullptr, " ", &context);
  const char* idText = strtok_r(nullptr, " ", &context);
  const char* verb = strtok_r(nullptr, " ", &context);
  const char* arg = strtok_r(nullptr, " ", &context);
  const char* durationText = strtok_r(nullptr, " ", &context);
  const char* extra = strtok_r(nullptr, " ", &context);
  uint32_t requestSession = 0, id = 0;
  if (!prefix || strcmp(prefix, "REM1") || !number(sessionText, 16, requestSession) || !number(idText, 10, id) ||
      !verb || extra)
    return;
  if (!strcmp(verb, "HELLO") && !arg) {
    // A repeat HELLO cannot roll back the sequence within an existing session.
    if (requestSession == session && id <= sequence) {
      status(requestSession, id, "error", "duplicate");
      return;
    }
    session = requestSession;
    sequence = id;
    status(session, id);
    return;
  }
  if (requestSession != session || id <= sequence) {
    status(requestSession, id, "error", "session_or_sequence");
    return;
  }
  sequence = id;
  if (!strcmp(verb, "STATUS") && !arg) {
    status(session, id);
    return;
  }
  if (!strcmp(verb, "HAPTIC") && arg && !durationText) {
    uint32_t duration = 0;
    if (!number(arg, 10, duration) || duration < 10 || duration > 250 || busy() ||
        !accessories.pulse(millis(), duration)) {
      status(session, id, "error", "haptic_unavailable_or_invalid");
      return;
    }
    status(session, id);
    return;
  }
  if (!strcmp(verb, "JOY") && arg && durationText) {
    uint32_t duration = 0;
    if (!number(durationText, 10, duration) || !ready() || busy() || M5.BtnA.isPressed() || M5.BtnB.isPressed() ||
        !accessories.inject(arg, duration, millis())) {
      status(session, id, "error", "invalid_joystick_or_busy");
      return;
    }
    status(session, id);
    return;
  }
  if (!strcmp(verb, "BUTTON") && arg && durationText && !extra) {
    uint32_t duration = 0;
    if (strlen(arg) != 1 || (*arg != 'A' && *arg != 'B') || !number(durationText, 10, duration) || duration < 30 ||
        duration > 2000) {
      status(session, id, "error", "invalid_button");
      return;
    }
    if (!ready() || busy() || !accessories.centered() || M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
      status(session, id, "error", "not_ready_or_busy");
      return;
    }
    virtualButton = *arg;
    virtualStarted = millis();
    virtualDuration = duration;
    status(session, id);  // Accepted; caller waits for busy=0 and completed increment.
    return;
  }
  if (!strcmp(verb, "DISCONNECT") && !arg) {
    if (!connected || busy()) {
      status(session, id, "error", "not_connected_or_busy");
      return;
    }
    server->disconnect(server->getConnId());
    status(session, id);
    return;
  }
  status(session, id, "error", "unknown_command");
}

void serialPoll() {
  static char line[128];
  static size_t length = 0;
  static bool overflow = false;
  if (!usbInitialized) return;
  char received[128];
  const int count = usb_serial_jtag_read_bytes(received, sizeof(received), 0);
  for (int index = 0; index < count; ++index) {
    const char c = received[index];
    if (c == '\r') continue;
    if (c == '\n') {
      if (!overflow && length) {
        line[length] = 0;
        command(line);
      }
      length = 0;
      overflow = false;
    } else if (length < sizeof(line) - 1 && !overflow)
      line[length++] = c;
    else
      overflow = true;  // Discard entire oversized line, including a valid-looking suffix.
  }
}

void draw(uint32_t now) {
  static uint32_t lastDraw = 0;
  if (uint32_t(now - lastDraw) < 1000) return;
  lastDraw = now;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(8, 8);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.println("CrossMux Remote");
  M5.Display.setTextColor(ready() ? TFT_GREEN : TFT_YELLOW);
  M5.Display.setCursor(8, 38);
  M5.Display.println(ready() ? "Connected" : connected ? "Pairing..." : "Waiting for reader");
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 72);
  M5.Display.println("A: Next       Hold: Back");
  M5.Display.setCursor(8, 90);
  M5.Display.println("B: Previous   Hold: Confirm");
  M5.Display.setCursor(8, 102);
  M5.Display.printf("Joy:%s Click:OK Hold:Back", accessories.online ? "on" : "off");
  M5.Display.setCursor(8, 112);
  M5.Display.printf("Battery %d%%    %s", M5.Power.getBatteryLevel(), actionName(last));
  static uint32_t lastBattery = 0;
  if (hid && uint32_t(now - lastBattery) >= 60000) {
    lastBattery = now;
    const int battery = M5.Power.getBatteryLevel();
    hid->setBatteryLevel(uint8_t(constrain(battery, 0, 100)));
  }
}
}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  cfg.serial_baudrate = 0;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(50);
  accessories.begin();
  // Driver allocates bounded TX (2 KiB) and RX (256 B) buffers once at boot.
  // Stack storage cannot outlive this setup call; check installation failure.
  usb_serial_jtag_driver_config_t usbConfig{};
  usbConfig.tx_buffer_size = kUsbTxBytes;
  usbConfig.rx_buffer_size = 256;
  usbInitialized = usb_serial_jtag_driver_install(&usbConfig) == ESP_OK;
  if (!usbInitialized) {
    M5.Display.println("USB buffer error");
  } else {
    // The pinned SDK also mirrors console output to USB. Route that VFS through
    // the same driver so it cannot write the hardware FIFO behind its ISR.
    usb_serial_jtag_vfs_use_driver();
  }
  uint8_t address[6];
  esp_read_mac(address, ESP_MAC_WIFI_STA);
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2], address[3],
           address[4], address[5]);
  if (!BLEDevice::init(kName)) return;
  BLEDevice::setSecurityCallbacks(&securityCallbacks);
  BLESecurity::setAuthenticationMode(true, false, false);  // Bonded legacy JustWorks, matching CrossMux host.
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setInitEncryptionKey();
  BLESecurity::setRespEncryptionKey();
  server = BLEDevice::createServer();
  if (!server) return;
  server->setCallbacks(&serverCallbacks);
  server->advertiseOnDisconnect(true);
  // Static wrapper avoids an application allocation. The library owns GATT objects,
  // allocated once during init; fixed buffers and one report slot bound runtime storage.
  static BLEHIDDevice device(server);
  hid = &device;
  input = hid->inputReport(1);
  if (!input) return;
  input->setCallbacks(&reportCallbacks);
  hid->manufacturer()->setValue("Personal CrossMux Remote");
  hid->hidInfo(0, 1);
  hid->reportMap(reportMap, sizeof(reportMap));
  hid->setBatteryLevel(uint8_t(constrain(M5.Power.getBatteryLevel(), 0, 100)));
  hid->protocolMode()->setValue(uint8_t(1));
  hid->startServices();
  auto* advertising = server->getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->setScanResponse(true);
  initialized = advertising->start();
  status(0, 0, "boot");
}

void loop() {
  M5.update();
  serialPoll();
  // Commands can start a virtual gesture. Sample time after parsing so a
  // command arriving across a millisecond tick cannot underflow elapsed time.
  const uint32_t now = millis();
  // Release a report before accepting another input; never leave a held HID key.
  if (active != Action::None && int32_t(now - releaseAt) >= 0) {
    if (ready()) {
      sendReport(0);
      ++completed;
    } else
      ++dropped;
    active = Action::None;
  }
  queue(accessories.poll(now, ready()), now);
  const bool virtualHeld = virtualButton && uint32_t(now - virtualStarted) < virtualDuration;
  queue(gestureA.update(M5.BtnA.isPressed() || (virtualButton == 'A' && virtualHeld), now, Action::Next, Action::Back),
        now);
  queue(
      gestureB.update(M5.BtnB.isPressed() || (virtualButton == 'B' && virtualHeld), now, Action::Prev, Action::Confirm),
      now);
  if (virtualButton && !virtualHeld) virtualButton = 0;
  draw(now);
  delay(3);
}
