#pragma once

#include "WeReadProtocol.h"
#include "WeReadStore.h"
#include "WeReadTimeBaseline.h"
#include "WeReadTimeCloud.h"
#include "WeReadTimeDiagnostic.h"
#include "WeReadTimeTransaction.h"

namespace WeReadClient {
// Worker-scoped fixed workspace. No write retries, no login fallback, no persistent
// credential copy. Use only through TimeTransaction after handover activation.
class DeviceTimeTransport final : public WeReadTime::TimeTransport {
 public:
  DeviceTimeTransport() = default;
  ~DeviceTimeTransport() override;
  void reset() override;
  bool retryablePreparation() const override;
  Read prepare(const WeReadTime::Identity& identity) override;
  Read snapshot(WeReadTime::AccountSnapshot& result) override;
  Write enter() override;
  Write report(uint32_t seconds) override;
  uint64_t epochSeconds() const override;
  uint32_t monotonicMs() const override;
  WeReadTime::Diagnostic diagnostic() const {
    auto result = diagnostic_;
    result.report = reportEvidence_;
    return result;
  }

 private:
  enum class Phase { Login, Reader, Progress, Ready, Entered, Reported, Failed };
  Write post(bool timed, uint32_t seconds = 0);
  bool fresh() const;
  Phase phase_ = Phase::Login;
  WeReadTime::Diagnostic diagnostic_;
  WeReadTime::ReportEvidence reportEvidence_;
  // Worker-scoped only. reset() clears credentials between batches but keeps
  // the last attempted-request timestamp for interval evidence; no persistence.
  uint32_t lastReportStartMs_ = 0, enteredAtMs_ = 0;
  bool hasLastReport_ = false;
  WeReadTime::Identity identity_;
  WeReadStore::Session session_;
  WeReadStore::TocRecord chapter_;
  WeReadProtocol::RemoteProgress remote_;
  WeReadTimeCloud::Query query_;
  bool querying_ = false;
  uint64_t preparedAt_ = 0;
  uint32_t preparedMs_ = 0;
  std::string referer_;
  char cookie_[896] = {}, url_[512] = {}, ps_[256] = {}, pc_[256] = {}, token_[128] = {};
  char body_[2048] = {}, acknowledgement_[513] = {};
  uint8_t io_[4096] = {};
};
static_assert(sizeof(DeviceTimeTransport) < 17 * 1024, "Time transport has a fixed activity-scoped budget");
}  // namespace WeReadClient
