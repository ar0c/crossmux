#pragma once
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include "WeReadTimeAck.h"
#include "WeReadNetworkDiagnostic.h"

namespace WeReadTime {
// Fixed-size evidence survives subsequent cloud-query diagnostics. Missing
// previous interval means no prior timed attempt in this activity, not zero.
struct ReportEvidence {
  enum class Guard { None, Phase, StaleContext, Body };
  bool attempted = false, accepted = false, hasPrevious = false;
  uint32_t seconds = 0, prepareAgeMs = 0, entryGapMs = 0, previousGapMs = 0;
  uint32_t elapsedMs = 0, responseBytes = 0;
  int http = 0, error = 0;
  Guard guard = Guard::None;
  AckObservation ack;
};
// No URL, response body, account, title or credential may enter this record.
struct Diagnostic {
  enum class Stage { None, StartupMemory, StartupSession, StartupAudit, Identity, Login,
    ReaderId, ReaderRequest, ReaderSignature, ProgressMemory, ProgressRequest, ProgressPayload,
    TocOpen, TocRead, ChapterMissing, ProgressRange, ProgressFraction, SessionSave, Clock,
    CloudKey, CloudStats, Enter, Report };
  Stage stage = Stage::None;
  int error = 0;
  int http = 0;
  int detail = 0;
  ReportEvidence report{};
  WeReadHttpClient::NetworkDiagnostic network{};
  uint8_t preparationRetries = 0;
  const char* name() const {
    switch (stage) {
      case Stage::None: return "none";
      case Stage::StartupMemory: return "startup_memory";
      case Stage::StartupSession: return "startup_session";
      case Stage::StartupAudit: return "startup_audit";
      case Stage::Identity: return "identity";
      case Stage::Login: return "login";
      case Stage::ReaderId: return "reader_id";
      case Stage::ReaderRequest: return "reader_request";
      case Stage::ReaderSignature: return "reader_signature";
      case Stage::ProgressMemory: return "progress_memory";
      case Stage::ProgressRequest: return "progress_request";
      case Stage::ProgressPayload: return "progress_payload";
      case Stage::TocOpen: return "toc_open";
      case Stage::TocRead: return "toc_read";
      case Stage::ChapterMissing: return "chapter_missing";
      case Stage::ProgressRange: return "progress_range";
      case Stage::ProgressFraction: return "progress_fraction";
      case Stage::SessionSave: return "session_save";
      case Stage::Clock: return "clock";
      case Stage::CloudKey: return "cloud_key";
      case Stage::CloudStats: return "cloud_stats";
      case Stage::Enter: return "enter";
      case Stage::Report: return "report";
    }
    return "invalid";
  }
  int encode(char* out, size_t capacity, unsigned queue, unsigned phase, unsigned issue, unsigned long tick) const {
    const int n = std::snprintf(out, capacity,
      "{\"schema\":3,\"stage\":\"%s\",\"error\":%d,\"http\":%d,\"detail\":%d,\"queue\":%u,\"phase\":%u,\"issue\":%u,\"uptime_ms\":%lu,"
      "\"retries\":%u,\"network\":{\"stage\":%u,\"error\":%d,\"socket\":%d,\"tls\":%d,\"verify\":%d,\"elapsed_ms\":%u},"
      "\"report\":{\"attempted\":%u,\"accepted\":%u,\"seconds\":%u,\"prepare_age_ms\":%u,\"entry_gap_ms\":%u,"
      "\"has_previous\":%u,\"previous_gap_ms\":%u,\"elapsed_ms\":%u,\"response_bytes\":%u,\"http\":%d,\"error\":%d,\"guard\":%u,\"succ\":%d,\"synckey\":%d}}\n",
      name(), error, http, detail, queue, phase, issue, tick,
      unsigned(preparationRetries), unsigned(network.stage), network.error, network.socket, network.tls, network.verify, unsigned(network.elapsedMs),
      unsigned(report.attempted), unsigned(report.accepted), unsigned(report.seconds), unsigned(report.prepareAgeMs),
      unsigned(report.entryGapMs), unsigned(report.hasPrevious), unsigned(report.previousGapMs),
      unsigned(report.elapsedMs), unsigned(report.responseBytes), report.http, report.error, unsigned(report.guard),
      int(report.ack.succ), int(report.ack.synckey));
    return n > 0 && static_cast<size_t>(n) < capacity ? n : 0;
  }
};
}
