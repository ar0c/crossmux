#pragma once
enum { STR_ENVIRONMENT_READING, STR_ENVIRONMENT_UNAVAILABLE, STR_ENVIRONMENT_BEFORE_SLEEP };
inline const char* tr(int key) {
  switch (key) {
    case STR_ENVIRONMENT_READING:
      return "%.1f°C  %u%%RH";
    case STR_ENVIRONMENT_BEFORE_SLEEP:
      return "Before sleep: %s";
    default:
      return "Temperature / humidity unavailable";
  }
}
