#include "WeReadTimeDiagnostic.h"
#include "WeReadTimeAck.h"
#include <cassert>
#include <cstring>
#include <iostream>
int main() {
  using D = WeReadTime::Diagnostic;
  char out[768];
  for (unsigned i=0; i<=static_cast<unsigned>(D::Stage::Report); ++i) {
    D d{static_cast<D::Stage>(i), -123, 403, 7};
    assert(std::strcmp(d.name(), "invalid"));
    const int n = d.encode(out, sizeof(out), 6, 15, 5, 0xffffffffUL);
    assert(n > 0 && n < 768 && std::strlen(out) == static_cast<size_t>(n));
    assert(std::strstr(out, "\"http\":403") && std::strstr(out, d.name()));
    assert(!std::strstr(out, "cookie") && !std::strstr(out, "token") && !std::strstr(out, "account"));
    assert(!d.encode(out, 8, 0, 0, 0, 0));
  }
  using A = WeReadTime::AckField;
  WeReadTime::AckObservation ack;
  const auto inspect = [&](const char* json) {
    return WeReadTime::acceptedTimeAck(json, std::strlen(json), false, &ack);
  };
  assert(inspect("{\"succ\":1}"));
  assert(ack.succ == A::One && ack.synckey == A::Missing);
  assert(inspect("{\"synckey\":0}")); // Preserve existing ACK semantics, NOT credit.
  assert(ack.succ == A::Missing && ack.synckey == A::Zero);
  assert(inspect("{\"succ\":true,\"synckey\":987654321}"));
  assert(ack.succ == A::One && ack.synckey == A::Other);
  assert(!inspect("{\"succ\":0}"));
  assert(ack.succ == A::Zero && ack.synckey == A::Missing);
  assert(!inspect("{\"succ\":1,\"errcode\":-1}"));
  assert(!inspect("{\"succ\":1} trailing"));
  assert(!inspect("{}"));
  assert(ack.succ == A::Missing && ack.synckey == A::Missing);
  D d{D::Stage::CloudStats, 1, 200, 1};
  d.network.stage = WeReadHttpClient::NetworkDiagnostic::Stage::Open;
  d.network.error = -123;
  d.network.tls = -456;
  d.network.verify = 7;
  d.report.seconds = 577;
  d.report.attempted = true;
  d.report.ack = {A::One, A::Missing};
  d.report.accepted = true;
  d.report.previousGapMs = UINT32_MAX;
  d.report.hasPrevious = true;
  d.report.http = 200;
  d.report.elapsedMs = UINT32_MAX;
  assert(d.encode(out, sizeof(out), 5, 11, 5, 650594));
  assert(std::strstr(out, "\"schema\":3"));
  assert(std::strstr(out, "\"tls\":-456"));
  assert(std::strstr(out, "\"verify\":7"));
  assert(std::strstr(out, "\"seconds\":577"));
  assert(std::strstr(out, "\"succ\":1,\"synckey\":-1"));
  assert(std::strstr(out, "\"stage\":\"cloud_stats\""));
  assert(!std::strstr(out, "987654321"));
  d.report.seconds = d.report.prepareAgeMs = d.report.entryGapMs = UINT32_MAX;
  d.report.responseBytes = UINT32_MAX;
  d.report.http = d.report.error = INT32_MIN;
  d.network.error = d.network.socket = d.network.tls = d.network.verify = INT32_MIN;
  d.network.elapsedMs = UINT32_MAX;
  d.preparationRetries = 3;
  assert(d.encode(out, sizeof(out), UINT32_MAX, UINT32_MAX, UINT32_MAX, 0xffffffffUL));
  std::cout << "Bounded diagnostics: exhaustive stages, numeric fields, no credential fields PASS\n";
}
