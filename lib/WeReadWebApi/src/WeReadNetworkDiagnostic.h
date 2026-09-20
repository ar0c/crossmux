#pragma once
#include <cstdint>

namespace WeReadHttpClient {
// Request-owned numeric evidence only: no URLs, identities or credentials.
struct NetworkDiagnostic {
  enum class Stage : uint8_t { None, Network, Input, Init, Setup, Open, Write, Headers, Body, Incomplete, Complete };
  Stage stage = Stage::None;
  int error = 0, socket = 0, tls = 0, verify = 0;
  uint32_t elapsedMs = 0;
  bool transientReadFailure() const {
    return verify == 0 && (stage == Stage::Network || stage == Stage::Open ||
        stage == Stage::Write || stage == Stage::Headers || stage == Stage::Body || stage == Stage::Incomplete);
  }
};
}
