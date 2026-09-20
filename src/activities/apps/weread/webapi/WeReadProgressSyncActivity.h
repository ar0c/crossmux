#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/apps/weread/WeReadBackend.h"
#include "activities/apps/weread/WeReadProgressContext.h"
#include "WeReadTimeCloud.h"
#include "WeReadTimeTransaction.h"
#include "WeReadTimeQueue.h"
#include "WeReadTimeDiagnostic.h"
#include "WeReadTimeSync.h"

class Epub;
struct CrossPointPosition;

class WeReadProgressSyncActivity final : public Activity {
 public:
  WeReadProgressSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                             const char* bookId, const WeReadProgressContext& context);
  ~WeReadProgressSyncActivity() override;

  static WeReadProgressContext makeContext(const Epub& epub, const char* bookId, float localFraction,
                                           const CrossPointPosition& localPosition);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t {
    WifiSelection,
    Starting,
    CheckingTime,
    Syncing,
    ChoosingDirection,
    Success,
    Failed,
    LoginRequired,
    TimeConfirm,
    TimeUploading,
    TimeResult,
  };

  enum class DirectionOption : uint8_t {
    ApplyRemote,
    UploadLocal,
  };

  State state_ = State::WifiSelection;
  WeReadClient::Operation operation_;
  WeReadClient::Error error_ = WeReadClient::Error::Ok;
  WeReadClient::ProgressSyncMode syncMode_ = WeReadClient::ProgressSyncMode::Compare;
  WeReadClient::ProgressSyncOutcome outcome_ = WeReadClient::ProgressSyncOutcome::Pending;
  DirectionOption selectedDirection_ = DirectionOption::ApplyRemote;
  std::string epubPath_;
  char bookId_[64] = {};
  WeReadClient::ProgressSyncInput input_;
  uint16_t localSpineIndex_ = 0;
  uint16_t localPageNumber_ = 0;
  uint16_t localPageCount_ = 0;
  float remoteFraction_ = 0.0f;
  bool uploadConflict_ = false;
  bool wifiActivated_ = false;
  uint64_t pendingTimeSeconds_ = 0;
  uint64_t externalConfirmedSeconds_ = 0;
  uint64_t externalUnknownSeconds_ = 0;
  bool timeCollectionFailed_ = false;
  bool timeHostPaused_ = false;
  bool timeChecked_ = false;
  WeReadTimeCloud::Result cloudTimeResult_ = WeReadTimeCloud::Result::Pending;
  WeReadTimeCloud::Snapshot cloudTime_;
  std::unique_ptr<WeReadTimeCloud::Query> timeQuery_;
  WeReadTimeSync::Accounting timeAccounting_;
  bool backgroundView_ = false;
  bool resumeTimeAfterWifi_ = false;
  uint32_t timeStatusRevision_ = 0;
  WeReadTime::TimeTransaction::State timeResult_ = WeReadTime::TimeTransaction::State::Idle;
  uint32_t selectedTimeDay_ = 0;
  uint64_t deviceConfirmedSeconds_ = 0;
  uint64_t deviceUnknownSeconds_ = 0;
  uint64_t servicePendingSeconds_ = 0, serviceConfirmedSeconds_ = 0;
  bool serviceMode_ = false;
  bool timeBatchUsed_ = false;
  bool timeInputBarrier_ = false;
  WeReadTime::TimeQueue::State timeQueueState_ = WeReadTime::TimeQueue::State::Idle;
  uint64_t timeRunConfirmed_ = 0;
  uint32_t timeWaitSeconds_ = 0;
  uint8_t timePreparationRetries_ = 0;
  char timeDiagnosticText_[128] = {};
  WeReadTime::TimeTransaction::Issue timeIssue_ = WeReadTime::TimeTransaction::Issue::None;
  bool auditTime(const char* account, WeReadTime::ExternalTime* selected = nullptr, uint64_t* measured = nullptr);
  void startTimeUpload();
  void advanceTimeUpload();
  const char* timeMessage() const;

  void collectReadingTime(const char* account);
  void advanceTimeQuery();

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void startSync();
  void advanceSync();
  void beginSelectedDirection();
  void applyRemoteProgress(const WeReadProtocol::RemoteProgress& remote);
  void returnToReader();
  const char* resultMessage() const;
  const char* errorMessage() const;
};
