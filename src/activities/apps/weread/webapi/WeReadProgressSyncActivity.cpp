#include "WeReadProgressSyncActivity.h"

#ifdef ENABLE_CHINESE_VERSION

#include <Epub.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "NetworkStartup.h"
#include "ProgressMapper.h"
#include "ReadingStatsStore.h"
#include "SilentRestart.h"
#include "WeReadTimeStorage.h"
#include "WeReadXhtmlCodec.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
Rect timeActionRect(const Rect& content, const int height, const int index) {
  constexpr int gap = 6;
  const int width = (content.width - gap) / 2;
  return Rect{content.x + index * (width + gap), content.y + content.height - height, width, height};
}
}  // namespace

WeReadProgressSyncActivity::~WeReadProgressSyncActivity() = default;

WeReadProgressSyncActivity::WeReadProgressSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string epubPath, const char* bookId,
                                                       const WeReadProgressContext& context)
    : Activity("WeReadProgressSync", renderer, mappedInput), epubPath_(std::move(epubPath)) {
  if (bookId) strncpy(bookId_, bookId, sizeof(bookId_) - 1);
  input_.localFraction = context.localFraction;
  input_.localTocIndex = context.localTocIndex;
  input_.localOffset = context.localOffset;
  input_.localOffsetBasis = context.localOffsetBasis;
  localSpineIndex_ = context.localSpineIndex;
  localPageNumber_ = context.localPageNumber;
  localPageCount_ = context.localPageCount;
}

WeReadProgressContext WeReadProgressSyncActivity::makeContext(const Epub& epub, const char* bookId,
                                                              const float localFraction,
                                                              const CrossPointPosition& localPosition) {
  WeReadProgressContext context;
  context.localFraction = localFraction;
  context.localSpineIndex = static_cast<uint16_t>(localPosition.spineIndex);
  context.localPageNumber = static_cast<uint16_t>(localPosition.pageNumber);
  context.localPageCount = static_cast<uint16_t>(localPosition.totalPages);
  if (!localPosition.hasVisibleTextOffset) return context;

  const std::string bookDir = WeReadStore::bookDirectory(bookId);
  WeReadStore::BookOptions options;
  if (!WeReadStore::loadBookOptions(bookDir, options) ||
      !WeReadStore::parseGeneratedChapterHref(epub.getSpineItem(localPosition.spineIndex).href,
                                              context.localTocIndex)) {
    return context;
  }

  context.localOffset = localPosition.visibleTextOffset;
  context.localOffsetBasis = WeReadClient::LocalOffsetBasis::VisibleText;
  uint32_t nativeOffset = 0;
  if (WeReadXhtmlCodec::visibleToNativeOffset(WeReadStore::chapterPath(bookDir, context.localTocIndex),
                                              context.localOffset, nativeOffset)) {
    context.localOffset = nativeOffset;
    context.localOffsetBasis = WeReadClient::LocalOffsetBasis::RawXhtmlUtf16;
  }
  return context;
}

void WeReadProgressSyncActivity::onEnter() {
  Activity::onEnter();
  timeInputBarrier_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  WeReadTimeSync::Status status;
  if (WeReadTimeSync::active()) {
    if (WeReadTimeSync::status(bookId_, status)) {
      backgroundView_ = true;
      timeBatchUsed_ = true;
      state_ = State::TimeUploading;
      advanceTimeUpload();
    } else {
      state_ = State::TimeResult;
      timeCollectionFailed_ = true;
      timeQueueState_ = WeReadTime::TimeQueue::State::Paused;
      std::snprintf(timeDiagnosticText_, sizeof(timeDiagnosticText_), "%s", tr(STR_WEREAD_TIME_BACKGROUND_BUSY));
      requestUpdate();
    }
    return;
  }

  WeReadStore::Session session;
  const bool loggedIn = WeReadStore::loadSession(session) && session.valid();
  if (loggedIn && WeReadTimeSync::status(bookId_, status, session.vid)) {
    session.clear();
    backgroundView_ = true;
    timeBatchUsed_ = true;
    state_ = State::TimeUploading;
    advanceTimeUpload();
    return;
  }
  WeReadTimeSync::dismiss(bookId_);  // Never show another account's old result.
  if (loggedIn) collectReadingTime(session.vid);
  session.clear();
  if (!loggedIn) {
    state_ = State::LoginRequired;
    requestUpdate();
    return;
  }

  NetworkStartup::prepare(renderer);
  wifiActivated_ = true;
  if (WiFi.status() == WL_CONNECTED) {
    state_ = State::Starting;
    requestUpdate();
    return;
  }
  launchWifiSelection();
}

void WeReadProgressSyncActivity::collectReadingTime(const char* account) {
  pendingTimeSeconds_ = 0;
  externalConfirmedSeconds_ = externalUnknownSeconds_ = 0;
  timeCollectionFailed_ = false;
  timeHostPaused_ = false;
  // The reader ends the measured session before opening this activity. Save
  // its source first; the time journal never rewrites the original statistics.
  if (!READING_STATS.saveToFile()) {
    timeCollectionFailed_ = true;
    return;
  }
  if (Storage.exists(WeReadTime::kLegacyTimeManifest) || Storage.exists(WeReadTime::kTimeManifest)) {
    timeCollectionFailed_ = !auditTime(account);
    return;
  }
  const auto* book = READING_STATS.findBook(epubPath_);
  if (!book) return;
  struct Workspace {
    WeReadTime::SdByteLog log;
    WeReadTime::Journal journal{log};
    WeReadTime::ExternalTimeStorage external;
  };
  // ~1 KiB, once per sync entry; fixed buffers must not live on the task stack.
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) {
    LOG_ERR("WRTime", "OOM: journal workspace");
    timeCollectionFailed_ = true;
    return;
  }
  uint64_t assignedMs = 0;
  for (const auto& day : book->readingDays) {
    if (day.readingMs > UINT64_MAX - assignedMs) {
      timeCollectionFailed_ = true;
      return;
    }
    assignedMs += day.readingMs;
    if (!work->log.configure(account, bookId_, book->bookId.c_str(), day.dayOrdinal) ||
        !work->journal.open(account, bookId_, book->bookId.c_str(), day.dayOrdinal) ||
        !work->journal.collect(day.readingMs)) {
      timeCollectionFailed_ = true;
      LOG_ERR("WRTime", "History import blocked: day=%lu", static_cast<unsigned long>(day.dayOrdinal));
      continue;
    }
    uint64_t pending = 0, confirmed = 0, unknown = 0;
    if (!work->external.reconcile(work->journal.ledger(), pending, confirmed, unknown)) {
      timeCollectionFailed_ = true;
      LOG_ERR("WRTime", "External accounting requires reconciliation; no sendable balance exposed");
      continue;
    }
    pendingTimeSeconds_ += pending;
    externalConfirmedSeconds_ += confirmed;
    externalUnknownSeconds_ += unknown;
  }
  // Undated legacy time cannot silently become today's measured time.
  if (assignedMs != book->totalReadingMs) timeCollectionFailed_ = true;
  LOG_INF("WRTime", "History pending=%llu seconds blocked=%u; no timed request sent",
          static_cast<unsigned long long>(pendingTimeSeconds_), static_cast<unsigned>(timeCollectionFailed_));
}

void WeReadProgressSyncActivity::onExit() {
  if (backgroundView_) WeReadTimeSync::dismiss(bookId_);
  timeAccounting_.clear();
  timeQuery_.reset();
  operation_.reset();
  Activity::onExit();
  if (WeReadTimeSync::ownsWifi() || !wifiActivated_) return;
  if (backgroundView_) {
    // A cancelled/failed reconnect has no worker to release its Wi-Fi session.
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    return;
  }
  WiFi.disconnect(false);
  delay(30);
  silentRestartToReader();
}

bool WeReadProgressSyncActivity::preventAutoSleep() {
  return state_ == State::Starting || state_ == State::CheckingTime || state_ == State::Syncing ||
         state_ == State::TimeUploading;
}

bool WeReadProgressSyncActivity::auditTime(const char* account, WeReadTime::ExternalTime* selected,
                                           uint64_t* measured) {
  const auto* book = READING_STATS.findBook(epubPath_);
  if (!book) return false;
  WeReadTimeSync::Totals totals;
  const WeReadTimeSync::Source source{bookId_, book->bookId.c_str(), book->readingDays.data(), book->readingDays.size(),
                                      book->totalReadingMs};
  const bool ok = timeAccounting_.audit(source, account, totals, selected, measured);
  pendingTimeSeconds_ = totals.pending;
  externalConfirmedSeconds_ = totals.externalConfirmed;
  externalUnknownSeconds_ = totals.externalUnknown;
  deviceConfirmedSeconds_ = totals.deviceConfirmed;
  deviceUnknownSeconds_ = totals.deviceUnknown;
  servicePendingSeconds_ = totals.servicePending;
  serviceConfirmedSeconds_ = totals.serviceConfirmed;
  serviceCheckedAt_ = totals.serviceCheckedAt;
  serviceMode_ = totals.serviceMode;
  selectedTimeDay_ = totals.selectedDay;
  timeHostPaused_ = totals.hostPaused;
  return ok;
}

void WeReadProgressSyncActivity::startTimeUpload() {
  if (!selectedTimeDay_ || timeBatchUsed_ || timeCollectionFailed_) return;
  if (!READING_STATS.saveToFile()) {
    timeCollectionFailed_ = true;
    timeQueueState_ = WeReadTime::TimeQueue::State::StorageError;
    state_ = State::TimeResult;
    requestUpdate();
    return;
  }
  operation_.reset();
  timeQuery_.reset();
  timeAccounting_.clear();
  timeDiagnosticText_[0] = '\0';
  timeRunConfirmed_ = 0;
  timeWaitSeconds_ = 0;
  timePreparationRetries_ = 0;
  timeIssue_ = WeReadTime::TimeTransaction::Issue::None;
  // The service copies only stable identity/day counters before the reader can
  // resume. No Activity, renderer or mutable statistics pointer escapes.
  auto session = makeUniqueNoThrow<WeReadStore::Session>();
  const auto* book = READING_STATS.findBook(epubPath_);
  bool started = false;
  const char* startError = session ? tr(STR_WEREAD_LOGIN_REQUIRED) : tr(STR_WEREAD_TIME_START_MEMORY);
  if (!book) startError = tr(STR_WEREAD_TIME_START_SOURCE);
  if (session && book && WeReadStore::loadSession(*session) && session->valid()) {
    const WeReadTimeSync::Source source{bookId_, book->bookId.c_str(), book->readingDays.data(),
                                        book->readingDays.size(), book->totalReadingMs};
    started = WeReadTimeSync::start(source, session->vid);
    using Failure = WeReadTimeSync::StartFailure;
    switch (WeReadTimeSync::lastStartFailure()) {
      case Failure::Headroom:
      case Failure::JobMemory:
      case Failure::SourceMemory:
      case Failure::TaskMemory:
        startError = tr(STR_WEREAD_TIME_START_MEMORY);
        break;
      case Failure::Network:
        startError = tr(STR_WEREAD_TIME_START_NETWORK);
        break;
      case Failure::InvalidSource:
        startError = tr(STR_WEREAD_TIME_START_SOURCE);
        break;
      case Failure::Busy:
        startError = tr(STR_WEREAD_TIME_BACKGROUND_BUSY);
        break;
      case Failure::None:
        break;
    }
  }
  if (session)
    session->clear();
  else
    LOG_ERR("WRTime", "OOM: startup session");
  if (!started) {
    timeResult_ = WeReadTime::TimeTransaction::State::NotSent;
    timeQueueState_ = WeReadTime::TimeQueue::State::Paused;
    state_ = State::TimeResult;
    std::snprintf(timeDiagnosticText_, sizeof(timeDiagnosticText_), "%s", startError);
    requestUpdate();
    return;
  }
  timeBatchUsed_ = true;
  backgroundView_ = true;
  timeInputBarrier_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  timeStatusRevision_ = 0;
  state_ = State::TimeUploading;
  advanceTimeUpload();
}

void WeReadProgressSyncActivity::advanceTimeUpload() {
  WeReadTimeSync::Status status;
  if (!WeReadTimeSync::status(bookId_, status) || status.revision == timeStatusRevision_) return;
  RenderLock lock(*this);
  timeStatusRevision_ = status.revision;
  pendingTimeSeconds_ = status.totals.pending;
  externalConfirmedSeconds_ = status.totals.externalConfirmed;
  externalUnknownSeconds_ = status.totals.externalUnknown;
  deviceConfirmedSeconds_ = status.totals.deviceConfirmed;
  deviceUnknownSeconds_ = status.totals.deviceUnknown;
  servicePendingSeconds_ = status.totals.servicePending;
  serviceConfirmedSeconds_ = status.totals.serviceConfirmed;
  serviceCheckedAt_ = status.totals.serviceCheckedAt;
  serviceQueueFull_ = status.serviceQueueFull;
  serviceMode_ = status.totals.serviceMode;
  selectedTimeDay_ = status.totals.selectedDay;
  timeHostPaused_ = status.totals.hostPaused;
  timeCollectionFailed_ = status.auditFailed;
  timeQueueState_ = status.queue;
  timeResult_ = status.phase;
  timeIssue_ = status.issue;
  timeRunConfirmed_ = status.confirmed;
  timeWaitSeconds_ = status.waitSeconds;
  timePreparationRetries_ = status.retries;
  if (status.diagnostic.stage != WeReadTime::Diagnostic::Stage::None) {
    std::snprintf(timeDiagnosticText_, sizeof(timeDiagnosticText_), tr(STR_WEREAD_TIME_DIAGNOSTIC),
                  status.diagnostic.name(), status.diagnostic.error, status.diagnostic.http);
  } else
    timeDiagnosticText_[0] = '\0';
  state_ = status.running ? State::TimeUploading : State::TimeResult;
  if (!status.running) timeInputBarrier_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

const char* WeReadProgressSyncActivity::timeMessage() const {
  if (serviceMode_) {
    using Q = WeReadTime::TimeQueue::State;
    if (serviceQueueFull_) return tr(STR_WEREAD_SERVICE_FULL);
    if (timeQueueState_ == Q::Complete)
      return !pendingTimeSeconds_ && !timeCollectionFailed_ ? tr(STR_WEREAD_SERVICE_ACCEPTED)
                                                            : tr(STR_WEREAD_SERVICE_RETRY);
    if (timeQueueState_ == Q::Paused) return tr(STR_WEREAD_SERVICE_RETRY);
    if (timeQueueState_ == Q::Uncertain) return tr(STR_WEREAD_SERVICE_REVIEW);
  }
  if (timeIssue_ == WeReadTime::TimeTransaction::Issue::LowSpace) return tr(STR_WEREAD_TIME_LOW_SPACE);
  if (timeIssue_ == WeReadTime::TimeTransaction::Issue::BaselineIncomplete) return tr(STR_WEREAD_TIME_BASELINE_MISSING);
  using Q = WeReadTime::TimeQueue::State;
  switch (timeQueueState_) {
    case Q::Complete:
      return tr(STR_WEREAD_TIME_QUEUE_DONE);
    case Q::Paused:
      return timeDiagnosticText_[0] ? timeDiagnosticText_ : tr(STR_WEREAD_TIME_QUEUE_PAUSED);
    case Q::Uncertain:
      switch (timeIssue_) {
        case WeReadTime::TimeTransaction::Issue::Expired:
          return tr(STR_WEREAD_TIME_EXPIRED);
        case WeReadTime::TimeTransaction::Issue::ClockInvalid:
          return tr(STR_WEREAD_TIME_CLOCK_INVALID);
        case WeReadTime::TimeTransaction::Issue::ReadbackFailed:
          return tr(STR_WEREAD_TIME_READ_FAILED);
        case WeReadTime::TimeTransaction::Issue::Mismatch:
          return tr(STR_WEREAD_TIME_MISMATCH);
        case WeReadTime::TimeTransaction::Issue::None:
        case WeReadTime::TimeTransaction::Issue::BaselineIncomplete:
        case WeReadTime::TimeTransaction::Issue::LowSpace:
        case WeReadTime::TimeTransaction::Issue::UnknownWrite:
          return tr(STR_WEREAD_TIME_UNCERTAIN);
      }
      return tr(STR_WEREAD_TIME_UNCERTAIN);
    case Q::StorageError:
      return tr(STR_WEREAD_TIME_STORAGE_ERROR);
    case Q::Selecting:
      return tr(STR_WEREAD_TIME_QUEUE_AUDIT);
    case Q::Idle:
    case Q::Running:
      break;
  }
  using T = WeReadTime::TimeTransaction::State;
  const auto phase = timeResult_;
  switch (phase) {
    case T::Idle:
    case T::Preparing:
      return tr(STR_WEREAD_TIME_PREPARING);
    case T::Waiting:
      return tr(STR_WEREAD_TIME_WAITING);
    case T::RetryWait:
      return tr(STR_WEREAD_TIME_PREPARING);
    case T::Baseline:
      return tr(STR_WEREAD_TIME_FETCH_STATS);
    case T::Reserving:
      return tr(STR_WEREAD_TIME_RESERVING);
    case T::Entering:
    case T::Sending:
      return tr(STR_WEREAD_TIME_SENDING);
    case T::ReadbackWait:
    case T::ReadingBack:
      return tr(STR_WEREAD_TIME_VERIFYING);
    case T::Confirmed:
      return tr(STR_WEREAD_TIME_CONFIRMED);
    case T::NotSent:
    case T::Cancelled:
      return tr(STR_WEREAD_TIME_NO_REQUEST);
    case T::Uncertain:
      return tr(STR_WEREAD_TIME_UNCERTAIN);
    case T::StorageError:
      return tr(STR_WEREAD_TIME_STORAGE_ERROR);
  }
  return tr(STR_WEREAD_TIME_REVIEW);
}

void WeReadProgressSyncActivity::launchWifiSelection() {
  state_ = State::WifiSelection;
  // ActivityManager owns this fixed-size activity until its result returns.
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!wifi) {
    LOG_ERR("WRSync", "OOM: WifiSelectionActivity (%u bytes)", static_cast<unsigned>(sizeof(WifiSelectionActivity)));
    error_ = WeReadClient::Error::OutOfMemory;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WeReadProgressSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (resumeTimeAfterWifi_) {
    resumeTimeAfterWifi_ = false;
    state_ = connected ? State::TimeConfirm : State::TimeResult;
    timeInputBarrier_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    requestUpdate();
    return;
  }

  if (!connected || WiFi.status() != WL_CONNECTED) {
    returnToReader();
    return;
  }
  state_ = State::Starting;
  requestUpdate();
}

void WeReadProgressSyncActivity::startSync() {
  requestUpdateAndWait();
  if (!TimeUtils::isClockValid() && !halClock.syncNow()) {
    LOG_ERR("WRSync", "Clock sync failed");
    error_ = WeReadClient::Error::Clock;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (!timeChecked_) {
    timeChecked_ = true;
    // Fixed <7 KiB scratch: heap-scoped so TLS does not share it with the task
    // stack. Released before position synchronization; no history-sized data.
    timeQuery_ = makeUniqueNoThrow<WeReadTimeCloud::Query>();
    if (!timeQuery_)
      LOG_ERR("WRTime", "OOM: cloud query (%u bytes)", static_cast<unsigned>(sizeof(WeReadTimeCloud::Query)));
    struct LoginScratch {
      WeReadStore::Session session;
      char cookie[896] = {};
      ~LoginScratch() {
        session.clear();
        auto* p = static_cast<volatile char*>(cookie);
        for (size_t i = 0; i < sizeof(cookie); ++i) p[i] = 0;
      }
    };
    // 1728 bytes, once per entry, freed before any TLS call. Avoids a large
    // frame on the small main-task stack; no persistent duplicate credentials.
    auto login = makeUniqueNoThrow<LoginScratch>();
    if (!login) LOG_ERR("WRTime", "OOM: login scratch (%u bytes)", static_cast<unsigned>(sizeof(LoginScratch)));
    if (timeQuery_ && login && WeReadStore::loadSession(login->session) && login->session.valid() &&
        login->session.cookieHeader(login->cookie, sizeof(login->cookie)) &&
        timeQuery_->begin(login->cookie, time(nullptr))) {
      state_ = State::CheckingTime;
      requestUpdate();
      return;
    }
    cloudTimeResult_ = timeQuery_ ? timeQuery_->result() : WeReadTimeCloud::Result::Unavailable;
    timeQuery_.reset();
  }
  if (!operation_.beginProgressSync(bookId_, input_, syncMode_)) {
    error_ = operation_.error();
    state_ = error_ == WeReadClient::Error::SessionExpired ? State::LoginRequired : State::Failed;
    requestUpdate();
    return;
  }
  state_ = State::Syncing;
  requestUpdate();
}

void WeReadProgressSyncActivity::advanceTimeQuery() {
  // Paint feedback before synchronous HTTPS (20 s timeout per read). Back is
  // handled between requests, not claimed to interrupt a blocking TLS call.
  requestUpdateAndWait();
  {
    RenderLock renderBarrier(*this);
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
    cloudTimeResult_ = timeQuery_->step();
  }
  if (cloudTimeResult_ == WeReadTimeCloud::Result::Pending) {
    requestUpdate();
    return;
  }
  if (cloudTimeResult_ == WeReadTimeCloud::Result::Ready) cloudTime_ = timeQuery_->snapshot();
  LOG_INF("WRTime", "Cloud preflight result=%u month=%llu today=%llu present=%u; no timed request sent",
          static_cast<unsigned>(cloudTimeResult_), static_cast<unsigned long long>(cloudTime_.monthSeconds),
          static_cast<unsigned long long>(cloudTime_.daySeconds), static_cast<unsigned>(cloudTime_.hasDay));
  timeQuery_.reset();
  state_ = State::Starting;
  requestUpdate();
}

void WeReadProgressSyncActivity::advanceSync() {
  // Serialize synchronous TLS work with the renderer task and release glyph
  // caches before the handshake, matching the main WeRead activity.
  RenderLock renderBarrier(*this);
  if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
  const auto event = operation_.step();
  switch (event) {
    case WeReadClient::Operation::Event::None:
    case WeReadClient::Operation::Event::Authenticated:
    case WeReadClient::Operation::Event::DetailReady:
    case WeReadClient::Operation::Event::ChapterComplete:
    case WeReadClient::Operation::Event::QrReady:
      return;
    case WeReadClient::Operation::Event::Cancelled:
      returnToReader();
      return;
    case WeReadClient::Operation::Event::Failed:
      error_ = operation_.error();
      state_ = error_ == WeReadClient::Error::SessionExpired ? State::LoginRequired : State::Failed;
      requestUpdate();
      return;
    case WeReadClient::Operation::Event::Complete:
      break;
  }

  const auto result = operation_.progressSyncResult();
  outcome_ = result.outcome;
  LOG_INF("WRSync", "sync complete: outcome=%u", static_cast<unsigned>(outcome_));
  operation_.reset();
  if (outcome_ == WeReadClient::ProgressSyncOutcome::SelectionRequired) {
    remoteFraction_ = result.remote.percent / 100.0f;
    uploadConflict_ = syncMode_ == WeReadClient::ProgressSyncMode::UploadLocal;
    selectedDirection_ = DirectionOption::ApplyRemote;
    syncMode_ = WeReadClient::ProgressSyncMode::Compare;
    state_ = State::ChoosingDirection;
    requestUpdate();
    return;
  }
  if (outcome_ == WeReadClient::ProgressSyncOutcome::ApplyRemote) {
    applyRemoteProgress(result.remote);
    return;
  }
  state_ = State::Success;
  requestUpdate();
}

void WeReadProgressSyncActivity::beginSelectedDirection() {
  switch (selectedDirection_) {
    case DirectionOption::ApplyRemote:
      syncMode_ = WeReadClient::ProgressSyncMode::ApplyRemote;
      break;
    case DirectionOption::UploadLocal:
      syncMode_ = WeReadClient::ProgressSyncMode::UploadLocal;
      break;
  }
  state_ = State::Starting;
  requestUpdate();
}

void WeReadProgressSyncActivity::applyRemoteProgress(const WeReadProtocol::RemoteProgress& remoteProgress) {
  // Mapping needs EPUB metadata after TLS has been released. The object is
  // fallible and activity-scoped; a task-stack Epub is too large.
  auto uniqueEpub = makeUniqueNoThrow<Epub>(epubPath_, "/.crosspoint");
  if (!uniqueEpub) {
    LOG_ERR("WRSync", "OOM: Epub (%u bytes)", static_cast<unsigned>(sizeof(Epub)));
    error_ = WeReadClient::Error::OutOfMemory;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  uniqueEpub->setupCacheDir();
  if (!uniqueEpub->load(false, true)) {
    LOG_ERR("WRSync", "Failed to load EPUB metadata");
    error_ = WeReadClient::Error::Integrity;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  // ProgressMapper currently accepts shared_ptr, but ownership stays here. The
  // aliasing view has no control block and therefore performs no allocation.
  const std::shared_ptr<Epub> epubView(std::shared_ptr<Epub>(), uniqueEpub.get());
  SavedProgressPosition fallback{"", std::max(0.0f, std::min(100.0f, remoteProgress.percent)) / 100.0f};
  bool preciseRemote = false;
  uint32_t remoteTocIndex = 0;
  float chapterFraction = 0.0f;
  float remoteBookFraction = 0.0f;
  int exactSpineIndex = -1;
  WeReadStore::BookOptions options;
  const bool generatedBook = WeReadStore::loadBookOptions(WeReadStore::bookDirectory(bookId_), options);
  const bool canonicalRemote = generatedBook && remoteProgress.hasChapterOffset &&
                               WeReadStore::mapChapterToPosition(
                                   WeReadStore::tocPath(bookId_), remoteProgress.chapterUid,
                                   remoteProgress.chapterOffset, remoteTocIndex, chapterFraction, remoteBookFraction);
  if (canonicalRemote) {
    for (int spine = 0; spine < uniqueEpub->getSpineItemsCount(); ++spine) {
      uint32_t tocIndex = 0;
      if (!WeReadStore::parseGeneratedChapterHref(uniqueEpub->getSpineItem(spine).href, tocIndex) ||
          tocIndex != remoteTocIndex) {
        continue;
      }
      exactSpineIndex = spine;
      preciseRemote = true;
      break;
    }
    if (exactSpineIndex < 0) {
      LOG_ERR("WRSync", "Remote chapter %u is not present in the downloaded range",
              static_cast<unsigned>(remoteTocIndex));
      error_ = WeReadClient::Error::Unavailable;
      state_ = State::Failed;
      requestUpdate();
      return;
    }
  }
  LOG_INF("WRSync", "remote progress mapping: precise=%u toc=%u chapter=%s offset=%u fraction=%lu",
          static_cast<unsigned>(preciseRemote), static_cast<unsigned>(remoteTocIndex), remoteProgress.chapterUid,
          static_cast<unsigned>(remoteProgress.chapterOffset),
          static_cast<unsigned long>(
              (preciseRemote ? uniqueEpub->calculateProgress(exactSpineIndex, chapterFraction) : fallback.percentage) *
                  1000000.0f +
              0.5f));
  const CrossPointPosition target =
      preciseRemote ? ProgressMapper::fromSpineProgress(epubView, exactSpineIndex, chapterFraction, renderer,
                                                        localSpineIndex_, localPageCount_)
                    : ProgressMapper::toCrossPoint(epubView, fallback, renderer, localSpineIndex_, localPageCount_);
  if (target.totalPages <= 0) {
    error_ = WeReadClient::Error::Unavailable;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  std::optional<uint32_t> visibleTextOffset;
  uint32_t resolvedVisibleOffset = 0;
  if (preciseRemote && WeReadXhtmlCodec::nativeToVisibleOffset(
                           WeReadStore::chapterPath(WeReadStore::bookDirectory(bookId_), remoteTocIndex),
                           remoteProgress.chapterOffset, resolvedVisibleOffset)) {
    visibleTextOffset = resolvedVisibleOffset;
  }
  if (!EpubReaderUtils::saveProgress(*uniqueEpub, target.spineIndex, target.pageNumber, target.totalPages,
                                     visibleTextOffset)) {
    error_ = WeReadClient::Error::SdCard;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  uniqueEpub.reset();
  state_ = State::Success;
  requestUpdate();
}

void WeReadProgressSyncActivity::returnToReader() { activityManager.goToReader(epubPath_); }

const char* WeReadProgressSyncActivity::resultMessage() const {
  switch (outcome_) {
    case WeReadClient::ProgressSyncOutcome::Pending:
    case WeReadClient::ProgressSyncOutcome::AlreadySynced:
      return tr(STR_ALREADY_SYNCED);
    case WeReadClient::ProgressSyncOutcome::SelectionRequired:
      return tr(STR_WEREAD_PROGRESS_CHANGED);
    case WeReadClient::ProgressSyncOutcome::ApplyRemote:
      return tr(STR_WEREAD_REMOTE_PROGRESS_APPLIED);
    case WeReadClient::ProgressSyncOutcome::LocalUploaded:
      return tr(STR_WEREAD_LOCAL_PROGRESS_UPLOADED);
  }
  return tr(STR_ALREADY_SYNCED);
}

const char* WeReadProgressSyncActivity::errorMessage() const {
  switch (error_) {
    case WeReadClient::Error::Unavailable:
      return tr(STR_WEREAD_PROGRESS_UNAVAILABLE);
    case WeReadClient::Error::Network:
      return tr(STR_WEREAD_HTTP_ERROR);
    case WeReadClient::Error::SdCard:
      return tr(STR_SAVE_PROGRESS_FAILED);
    case WeReadClient::Error::Clock:
      return tr(STR_CLOCK_SYNC_FAIL);
    case WeReadClient::Error::SessionExpired:
      return tr(STR_WEREAD_LOGIN_REQUIRED);
    case WeReadClient::Error::Ok:
    case WeReadClient::Error::Cancelled:
    case WeReadClient::Error::LoginFailed:
    case WeReadClient::Error::Protocol:
    case WeReadClient::Error::Integrity:
    case WeReadClient::Error::OutOfMemory:
      return tr(STR_SYNC_FAILED_MSG);
  }
  return tr(STR_SYNC_FAILED_MSG);
}

void WeReadProgressSyncActivity::loop() {
  if (timeInputBarrier_) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) timeInputBarrier_ = false;
    return;
  }
  switch (state_) {
    case State::TimeConfirm: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_ = State::Success;
        requestUpdate();
        return;
      }
      const auto& metrics = UITheme::getInstance().getMetrics();
      const auto content =
          SubpageLayout::contentRect(UITheme::getInstance().getScreenSafeArea(renderer, true, false), metrics);
      int row = -1;
      const bool tapped = mappedInput.rowTouch(row, content.y + content.height - metrics.menuRowHeight,
                                               metrics.menuRowHeight, 1, content.x, content.x + content.width,
                                               metrics.menuRowHeight) == MappedInputManager::RowTouch::Tap;
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || tapped) startTimeUpload();
      return;
    }
    case State::TimeUploading: {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const auto content =
          SubpageLayout::contentRect(UITheme::getInstance().getScreenSafeArea(renderer, true, false), metrics);
      for (int action = 0; action < 2; ++action) {
        const auto rect = timeActionRect(content, metrics.menuRowHeight, action);
        int row = -1;
        if (mappedInput.rowTouch(row, rect.y, rect.height, 1, rect.x, rect.x + rect.width, rect.height) ==
            MappedInputManager::RowTouch::Tap) {
          if (action == 0)
            returnToReader();
          else
            WeReadTimeSync::pause();
          return;
        }
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        returnToReader();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) WeReadTimeSync::pause();
      advanceTimeUpload();
      return;
    }
    case State::TimeResult: {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const auto content =
          SubpageLayout::contentRect(UITheme::getInstance().getScreenSafeArea(renderer, true, false), metrics);
      int row = -1;
      const bool tapped = mappedInput.rowTouch(row, content.y + content.height - metrics.menuRowHeight,
                                               metrics.menuRowHeight, 1, content.x, content.x + content.width,
                                               metrics.menuRowHeight) == MappedInputManager::RowTouch::Tap;
      if (!timeCollectionFailed_ && selectedTimeDay_ &&
          (mappedInput.wasReleased(MappedInputManager::Button::NavNext) || tapped)) {
        timeBatchUsed_ = false;
        if (WiFi.status() != WL_CONNECTED) {
          resumeTimeAfterWifi_ = true;
          wifiActivated_ = true;
          launchWifiSelection();
          return;
        }
        state_ = State::TimeConfirm;
        timeInputBarrier_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        requestUpdate();
        return;
      }
      int x = 0, y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y))
        returnToReader();
      return;
    }
    case State::WifiSelection:
      return;
    case State::Starting:
      startSync();
      return;
    case State::CheckingTime:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        timeQuery_.reset();
        returnToReader();
        return;
      }
      advanceTimeQuery();
      return;
    case State::Syncing:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        operation_.cancel();
      }
      advanceSync();
      return;
    case State::ChoosingDirection: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        returnToReader();
        return;
      }
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const Rect content = SubpageLayout::contentRect(screen, metrics);
      const int optionStep = metrics.menuRowHeight + metrics.menuSpacing;
      const int optionTop = content.y + content.height - optionStep * 2;
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(touchedOption, optionTop, optionStep, 2, content.x,
                                              content.x + content.width, metrics.menuRowHeight);
      if (touch != MappedInputManager::RowTouch::None) {
        selectedDirection_ = touchedOption == 0 ? DirectionOption::ApplyRemote : DirectionOption::UploadLocal;
        if (touch == MappedInputManager::RowTouch::Tap) {
          beginSelectedDirection();
        } else {
          requestUpdate();
        }
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
          mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
        selectedDirection_ = selectedDirection_ == DirectionOption::ApplyRemote ? DirectionOption::UploadLocal
                                                                                : DirectionOption::ApplyRemote;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        beginSelectedDirection();
      }
      return;
    }
    case State::Success: {
      if (!timeCollectionFailed_ && selectedTimeDay_ && !timeBatchUsed_) {
        const auto& metrics = UITheme::getInstance().getMetrics();
        const auto content =
            SubpageLayout::contentRect(UITheme::getInstance().getScreenSafeArea(renderer, true, false), metrics);
        int row = -1;
        const bool tapped = mappedInput.rowTouch(row, content.y + content.height - metrics.menuRowHeight,
                                                 metrics.menuRowHeight, 1, content.x, content.x + content.width,
                                                 metrics.menuRowHeight) == MappedInputManager::RowTouch::Tap;
        if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) || tapped) {
          state_ = State::TimeConfirm;
          timeInputBarrier_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
          requestUpdate();
          return;
        }
      }
      int x = 0, y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y))
        returnToReader();
      return;
    }
    case State::LoginRequired: {
      int x = 0;
      int y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        returnToReader();
      }
      return;
    }
    case State::Failed:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        returnToReader();
        return;
      }
      {
        int x = 0;
        int y = 0;
        if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) &&
            (error_ == WeReadClient::Error::Network || error_ == WeReadClient::Error::Clock ||
             error_ == WeReadClient::Error::Unavailable)) {
          state_ = State::Starting;
          requestUpdate();
        }
        return;
      }
  }
}

void WeReadProgressSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WEREAD_SYNC_PROGRESS));
  const Rect content = SubpageLayout::contentRect(screen, metrics);
  const Rect textBounds = SubpageLayout::insetHorizontal(content, metrics.contentSidePadding);
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);

  switch (state_) {
    case State::TimeConfirm:
    case State::TimeUploading:
    case State::TimeResult: {
      const int top = content.y + SubpageLayout::sectionGap(metrics);
      const int rowTop = top + titleHeight + 12;
      const int rowStep = renderer.getLineHeight(UI_10_FONT_ID) + 6;
      // Fixed stack text; existing UI layout and font metrics remain unchanged.
      char waitingTitle[96];
      const char* title = state_ == State::TimeConfirm
                              ? (serviceMode_ ? tr(STR_WEREAD_SERVICE_CONFIRM) : tr(STR_WEREAD_TIME_CONFIRM_ALL))
                              : timeMessage();
      if (state_ == State::TimeUploading && timeResult_ == WeReadTime::TimeTransaction::State::Waiting) {
        snprintf(waitingTitle, sizeof(waitingTitle), tr(STR_WEREAD_TIME_WAITING_FMT), unsigned(timeWaitSeconds_));
        title = waitingTitle;
      }
      if (state_ == State::TimeUploading && timeResult_ == WeReadTime::TimeTransaction::State::RetryWait) {
        snprintf(waitingTitle, sizeof(waitingTitle), tr(STR_WEREAD_TIME_READ_RETRY_FMT),
                 unsigned(timePreparationRetries_), unsigned(timeWaitSeconds_));
        title = waitingTitle;
      }
      UITheme::drawCenteredText(renderer, textBounds, UI_12_FONT_ID, top, title, true, EpdFontFamily::BOLD);
      char line[112];
      snprintf(line, sizeof(line), serviceMode_ ? tr(STR_WEREAD_SERVICE_LOCAL) : tr(STR_WEREAD_TIME_PENDING_FMT),
               static_cast<unsigned long long>(pendingTimeSeconds_ / 60), unsigned(pendingTimeSeconds_ % 60));
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop, line);
      snprintf(line, sizeof(line), tr(STR_WEREAD_TIME_DEVICE_FMT),
               static_cast<unsigned long long>(deviceConfirmedSeconds_),
               static_cast<unsigned long long>(deviceUnknownSeconds_));
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep, line);
      snprintf(line, sizeof(line), tr(STR_WEREAD_TIME_EXTERNAL_FMT),
               static_cast<unsigned long long>(externalConfirmedSeconds_),
               static_cast<unsigned long long>(externalUnknownSeconds_));
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep * 2, line);
      snprintf(line, sizeof(line), tr(STR_WEREAD_TIME_RUN_FMT), static_cast<unsigned long long>(timeRunConfirmed_ / 60),
               unsigned(timeRunConfirmed_ % 60));
      if (serviceMode_)
        snprintf(line, sizeof(line), tr(STR_WEREAD_SERVICE_TOTALS),
                 static_cast<unsigned long long>(servicePendingSeconds_),
                 static_cast<unsigned long long>(serviceConfirmedSeconds_));
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep * 3, line);
      const char* notice = tr(STR_WEREAD_TIME_TODAY_NOTICE);
      if (serviceMode_) {
        notice = tr(STR_WEREAD_SERVICE_UNCHECKED);
        if (serviceCheckedAt_) {
          const std::time_t at = static_cast<std::time_t>(serviceCheckedAt_);
          std::tm value{};
          char when[24]{};
          if (localtime_r(&at, &value) && std::strftime(when, sizeof(when), "%m-%d %H:%M", &value)) {
            snprintf(line, sizeof(line), tr(STR_WEREAD_SERVICE_CHECKED), when);
            notice = line;
          }
        }
      }
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep * 4, notice);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep * 5,
                                tr(STR_WEREAD_TIME_OTHER_CLIENTS));
      if (state_ == State::TimeConfirm) {
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID,
                                  content.y + content.height - metrics.menuRowHeight, tr(STR_WEREAD_TIME_UPLOAD_30),
                                  true, EpdFontFamily::BOLD);
      } else if (state_ == State::TimeUploading) {
        for (int action = 0; action < 2; ++action) {
          const auto rect = timeActionRect(content, metrics.menuRowHeight, action);
          UITheme::drawCenteredText(
              renderer, rect, UI_10_FONT_ID, rect.y,
              action == 0 ? tr(STR_WEREAD_TIME_BACKGROUND_READ) : tr(STR_WEREAD_TIME_BACKGROUND_PAUSE), true,
              EpdFontFamily::BOLD);
        }
      } else if (state_ == State::TimeResult && !timeCollectionFailed_ && selectedTimeDay_) {
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID,
                                  content.y + content.height - metrics.menuRowHeight, tr(STR_WEREAD_TIME_RESUME), true,
                                  EpdFontFamily::BOLD);
      }
      break;
    }
    case State::CheckingTime:
      UITheme::drawCenteredText(
          renderer, textBounds, UI_12_FONT_ID, SubpageLayout::centeredTop(content, titleHeight),
          timeQuery_ && timeQuery_->readingStats() ? tr(STR_WEREAD_TIME_FETCH_STATS) : tr(STR_WEREAD_TIME_FETCH_KEY),
          true, EpdFontFamily::BOLD);
      break;
    case State::WifiSelection:
    case State::Starting:
    case State::Syncing:
      UITheme::drawCenteredText(renderer, textBounds, UI_12_FONT_ID, SubpageLayout::centeredTop(content, titleHeight),
                                tr(STR_WEREAD_SYNCING_PROGRESS), true, EpdFontFamily::BOLD);
      break;
    case State::ChoosingDirection: {
      const int contentTop = content.y + SubpageLayout::sectionGap(metrics);
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const char* comparisonTitle = uploadConflict_ ? tr(STR_WEREAD_PROGRESS_CHANGED) : tr(STR_PROGRESS_FOUND);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, contentTop, comparisonTitle, true,
                                EpdFontFamily::BOLD);

      char remoteValue[24];
      snprintf(remoteValue, sizeof(remoteValue), "%.2f%%", remoteFraction_ * 100.0f);
      char localValue[64];
      snprintf(localValue, sizeof(localValue), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), localPageNumber_ + 1, localPageCount_,
               input_.localFraction * 100.0f);
      const int remoteLabelY = contentTop + lineHeight * 2;
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, remoteLabelY, tr(STR_REMOTE_LABEL), true,
                                EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, remoteLabelY + lineHeight, remoteValue);
      const int localLabelY = remoteLabelY + lineHeight * 3;
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, localLabelY, tr(STR_LOCAL_LABEL), true,
                                EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, localLabelY + lineHeight, localValue);

      const int optionStep = metrics.menuRowHeight + metrics.menuSpacing;
      const int optionTop = content.y + content.height - optionStep * 2;
      const int optionTextOffset = (metrics.menuRowHeight - lineHeight) / 2;
      const DirectionOption options[] = {DirectionOption::ApplyRemote, DirectionOption::UploadLocal};
      for (int index = 0; index < 2; ++index) {
        const int optionY = optionTop + optionStep * index;
        const bool selected = selectedDirection_ == options[index];
        if (selected) {
          renderer.fillRect(textBounds.x, optionY, textBounds.width, metrics.menuRowHeight);
        }
        const char* label =
            options[index] == DirectionOption::ApplyRemote ? tr(STR_APPLY_REMOTE) : tr(STR_UPLOAD_LOCAL);
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, optionY + optionTextOffset, label, !selected);
      }
      break;
    }
    case State::Success: {
      const int resultY = content.y + SubpageLayout::sectionGap(metrics);
      const int rowTop = resultY + titleHeight + 12;
      const int rowStep = renderer.getLineHeight(UI_10_FONT_ID) + 6;
      UITheme::drawCenteredText(renderer, textBounds, UI_12_FONT_ID, resultY, resultMessage(), true,
                                EpdFontFamily::BOLD);
      char pending[96];
      if (timeCollectionFailed_) {
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop,
                                  timeHostPaused_ ? tr(STR_WEREAD_TIME_HOST_PAUSED) : tr(STR_WEREAD_TIME_REVIEW), true);
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep,
                                  tr(STR_WEREAD_TIME_COUNTERS_UNAVAILABLE), true);
      } else {
        snprintf(pending, sizeof(pending), tr(STR_WEREAD_TIME_PENDING_FMT),
                 static_cast<unsigned long long>(pendingTimeSeconds_ / 60),
                 static_cast<unsigned>(pendingTimeSeconds_ % 60));
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep, pending, true);
        snprintf(pending, sizeof(pending), tr(STR_WEREAD_TIME_EXTERNAL_FMT),
                 static_cast<unsigned long long>(externalConfirmedSeconds_),
                 static_cast<unsigned long long>(externalUnknownSeconds_));
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop, pending, true);
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep * 2,
                                  tr(STR_WEREAD_TIME_NOT_SENT), true);
        snprintf(pending, sizeof(pending), tr(STR_WEREAD_TIME_DEVICE_FMT),
                 static_cast<unsigned long long>(deviceConfirmedSeconds_),
                 static_cast<unsigned long long>(deviceUnknownSeconds_));
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep * 4, pending);
      }
      if (!timeCollectionFailed_ && selectedTimeDay_ && !timeBatchUsed_) {
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID,
                                  content.y + content.height - metrics.menuRowHeight, tr(STR_WEREAD_TIME_UPLOAD_30),
                                  true, EpdFontFamily::BOLD);
      }
      const char* cloudStatus = tr(STR_WEREAD_TIME_CLOUD_UNAVAILABLE);
      if (cloudTimeResult_ == WeReadTimeCloud::Result::Ready) {
        if (cloudTime_.hasDay) {
          snprintf(pending, sizeof(pending), tr(STR_WEREAD_TIME_CLOUD_TODAY_FMT),
                   static_cast<unsigned long long>(cloudTime_.daySeconds / 60),
                   static_cast<unsigned>(cloudTime_.daySeconds % 60));
          cloudStatus = pending;
        } else
          cloudStatus = tr(STR_WEREAD_TIME_CLOUD_NO_DAY);
      } else if (cloudTimeResult_ == WeReadTimeCloud::Result::LoginRequired) {
        cloudStatus = tr(STR_WEREAD_TIME_CLOUD_AUTH);
      }
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, rowTop + rowStep * 3, cloudStatus, true);
      break;
    }
    case State::LoginRequired:
      UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, tr(STR_WEREAD_LOGIN_REQUIRED), 2, true,
                                       EpdFontFamily::BOLD);
      break;
    case State::Failed:
      UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, errorMessage(), 2, true,
                                       EpdFontFamily::BOLD);
      break;
  }

  if (state_ == State::TimeConfirm) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::TimeUploading || state_ == State::TimeResult) {
    const bool resumable = state_ == State::TimeResult && !timeCollectionFailed_ && selectedTimeDay_;
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), state_ == State::TimeUploading ? tr(STR_WEREAD_TIME_BACKGROUND_PAUSE) : "",
                              "", resumable ? tr(STR_WEREAD_TIME_RESUME) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::ChoosingDirection) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::Success || state_ == State::LoginRequired || state_ == State::Failed) {
    const bool retryable =
        state_ == State::Failed && (error_ == WeReadClient::Error::Network || error_ == WeReadClient::Error::Clock ||
                                    error_ == WeReadClient::Error::Unavailable);
    const bool timeAvailable =
        state_ == State::Success && !timeCollectionFailed_ && selectedTimeDay_ && !timeBatchUsed_;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "",
                                              timeAvailable ? tr(STR_SELECT)
                                              : retryable   ? tr(STR_RETRY)
                                                            : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

#endif
