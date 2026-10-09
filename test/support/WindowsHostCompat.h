#pragma once

// Native Windows CRT environment helpers for host fixtures. Firmware and POSIX
// builds continue to use their own setenv/unsetenv implementations.
#ifdef _WIN32
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline int setenv(const char* name, const char* value, int overwrite) {
  if (!name || !*name || strchr(name, '=') || !value) {
    errno = EINVAL;
    return -1;
  }
  if (!overwrite && getenv(name)) return 0;
  const int result = _putenv_s(name, value);
  if (result) errno = result;
  return result ? -1 : 0;
}

static inline int unsetenv(const char* name) { return setenv(name, "", 1); }

static inline struct tm* localtime_r(const time_t* time, struct tm* result) {
  return localtime_s(result, time) == 0 ? result : NULL;
}

static inline struct tm* gmtime_r(const time_t* time, struct tm* result) {
  return gmtime_s(result, time) == 0 ? result : NULL;
}
#endif
