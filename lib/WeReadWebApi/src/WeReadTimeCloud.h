#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

namespace WeReadTimeCloud {

// Account aggregates, including listening. Never label these as book counters.
struct Snapshot {
  uint64_t month = 0;
  uint64_t day = 0;
  uint64_t monthSeconds = 0;
  uint64_t daySeconds = 0;
  bool hasDay = false;
};

// Incremental, bounded-memory response decoder. Missing day != zero.
class Response {
 public:
  enum class Mode { Key, Stats };
  Response();
  void reset(Mode mode, uint64_t month = 0, uint64_t day = 0);
  bool feed(const uint8_t* data, size_t size);
  bool complete() const;
  const char* key() const { return key_; }
  const Snapshot& snapshot() const { return snapshot_; }

 private:
  enum class Field { Ignore, Key, Month, Total, Days, Day, Error, Upgrade };
  static void onKey(void*, const char*, size_t);
  static void onString(void*, const char*, size_t);
  static void onNumber(void*, const char*, size_t);
  static void onBool(void*, bool);
  static void onNull(void*);
  static void onObjectStart(void*);
  static void onObjectEnd(void*);
  static void onArrayStart(void*);
  static void onArrayEnd(void*);
  static void onChunk(void*, const char*, size_t, bool);
  void start(bool object);
  void end(bool object);
  void wrongType();
  StreamingJsonParser parser_;
  Snapshot snapshot_;
  Mode mode_ = Mode::Key;
  Field field_ = Field::Ignore;
  uint8_t depth_ = 0;
  bool objects_[32] = {};
  bool started_ = false;
  bool closed_ = false;
  bool failed_ = false;
  bool seenKey_ = false;
  bool seenMonth_ = false;
  bool seenTotal_ = false;
  bool seenDays_ = false;
  bool seenError_ = false;
  uint8_t daysDepth_ = 0;
  char key_[128] = {};
};

enum class Result { Pending, Ready, Network, LoginRequired, Unavailable, Protocol, Clock };

// Two read-only requests. Key is fetched from the existing Web login, retained
// only in this workspace, and never written to SD or logged.
class Query {
 public:
  ~Query();
  void clear();
  bool begin(const char* cookie, uint64_t now);
  Result step();
  Result result() const { return result_; }
  const Snapshot& snapshot() const { return response_.snapshot(); }
  bool readingStats() const { return phase_ == Phase::Stats; }
  int httpStatus() const { return httpStatus_; }

 private:
  enum class Phase { Key, Stats, Done };
  Response response_;
  Snapshot scope_;
  char cookie_[896] = {};
  char account_[64] = {};
  char skey_[384] = {};
  char authorization_[144] = {};
  uint8_t io_[4096] = {};
  Phase phase_ = Phase::Done;
  Result result_ = Result::Protocol;
  int httpStatus_ = 0;
};
static_assert(sizeof(Query) < 7 * 1024, "Time query workspace must remain bounded");

}  // namespace WeReadTimeCloud
