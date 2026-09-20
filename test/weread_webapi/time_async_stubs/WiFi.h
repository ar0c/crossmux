#pragma once
#include <atomic>
constexpr int WL_CONNECTED = 3, WIFI_OFF = 0;
struct WifiStub {
  std::atomic<bool> connected{true};
  int status() { return connected ? WL_CONNECTED : 0; }
  void disconnect(bool) { connected = false; }
  void mode(int) { connected = false; }
};
inline WifiStub WiFi;
