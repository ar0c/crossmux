#include "WeReadTimeCloud.h"

#include <cstring>

namespace WeReadTimeCloud {
namespace {
bool equal(const char* value, size_t len, const char* expected) {
  return strlen(expected) == len && memcmp(value, expected, len) == 0;
}
bool integer(const char* value, size_t len, uint64_t& out) {
  if (!len || (len > 1 && value[0] == '0')) return false;
  out = 0;
  for (size_t i = 0; i < len; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    const unsigned digit = value[i] - '0';
    if (out > (UINT64_MAX - digit) / 10) return false;
    out = out * 10 + digit;
  }
  return true;
}
}  // namespace

Response::Response()
    : parser_({this, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd,
               onArrayStart, onArrayEnd, onChunk}) {}

void Response::reset(Mode mode, uint64_t month, uint64_t day) {
  parser_.reset();
  snapshot_ = {};
  snapshot_.month = month;
  snapshot_.day = day;
  mode_ = mode;
  field_ = Field::Ignore;
  depth_ = daysDepth_ = 0;
  started_ = closed_ = failed_ = false;
  seenKey_ = seenMonth_ = seenTotal_ = seenDays_ = seenError_ = false;
  memset(key_, 0, sizeof(key_));
}

bool Response::feed(const uint8_t* data, size_t size) {
  // The shared parser has no EOF API. Require a closed root and reject data
  // after it, including a second JSON document, independently of transport.
  for (size_t i = 0; i < size && !failed_; ++i) {
    const char ch = static_cast<char>(data[i]);
    const bool space = ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t';
    if (closed_ || !started_) {
      if (space) continue;
      if (closed_ || ch != '{') { failed_ = true; break; }
    }
    parser_.feed(&ch, 1);
  }
  return !failed_ && !parser_.hasError();
}

bool Response::complete() const {
  if (failed_ || parser_.hasError() || !closed_ || depth_) return false;
  if (mode_ == Mode::Key) return seenKey_;
  return seenMonth_ && seenTotal_ && seenDays_ &&
         (!snapshot_.hasDay || snapshot_.daySeconds <= snapshot_.monthSeconds);
}

void Response::onKey(void* raw, const char* key, size_t len) {
  auto& self = *static_cast<Response*>(raw);
  self.field_ = Field::Ignore;
  if (self.depth_ == 1) {
    if (equal(key, len, "errcode")) self.field_ = Field::Error;
    else if (equal(key, len, "upgrade_info")) self.field_ = Field::Upgrade;
    else if (self.mode_ == Mode::Key && equal(key, len, "apikey")) self.field_ = Field::Key;
    else if (self.mode_ == Mode::Stats) {
      if (equal(key, len, "baseTime")) self.field_ = Field::Month;
      else if (equal(key, len, "totalReadTime")) self.field_ = Field::Total;
      else if (equal(key, len, "readTimes")) self.field_ = Field::Days;
    }
  } else if (self.daysDepth_ && self.depth_ == self.daysDepth_) {
    uint64_t day = 0;
    if (integer(key, len, day) && day == self.snapshot_.day) self.field_ = Field::Day;
  }
  if (self.field_ == Field::Upgrade) self.failed_ = true;
}

void Response::wrongType() {
  if (field_ != Field::Ignore) failed_ = true;
  field_ = Field::Ignore;
}

void Response::onString(void* raw, const char* value, size_t len) {
  auto& self = *static_cast<Response*>(raw);
  if (self.field_ != Field::Key) { self.wrongType(); return; }
  if (self.seenKey_ || len <= 4 || len >= sizeof(self.key_) || memcmp(value, "wrk-", 4)) {
    self.failed_ = true;
    return;
  }
  for (size_t i = 0; i < len; ++i) {
    const char ch = value[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) {
      self.failed_ = true;
      return;
    }
  }
  memcpy(self.key_, value, len);
  self.key_[len] = 0;
  self.seenKey_ = true;
  self.field_ = Field::Ignore;
}

void Response::onNumber(void* raw, const char* value, size_t len) {
  auto& self = *static_cast<Response*>(raw);
  if (self.field_ == Field::Ignore) return;
  uint64_t number = 0;
  if (!integer(value, len, number)) { self.failed_ = true; return; }
  switch (self.field_) {
    case Field::Month:
      if (self.seenMonth_ || number != self.snapshot_.month) self.failed_ = true;
      self.seenMonth_ = true;
      break;
    case Field::Total:
      if (self.seenTotal_) self.failed_ = true;
      self.seenTotal_ = true;
      self.snapshot_.monthSeconds = number;
      break;
    case Field::Day:
      if (self.snapshot_.hasDay) self.failed_ = true;
      self.snapshot_.hasDay = true;
      self.snapshot_.daySeconds = number;
      break;
    case Field::Error:
      if (self.seenError_ || number != 0) self.failed_ = true;
      self.seenError_ = true;
      break;
    default: self.failed_ = true; break;
  }
  self.field_ = Field::Ignore;
}

void Response::start(bool object) {
  if (!depth_) {
    if (started_ || !object) failed_ = true;
    started_ = true;
  } else if (field_ == Field::Days) {
    if (!object || seenDays_ || depth_ != 1) failed_ = true;
    seenDays_ = true;
    daysDepth_ = depth_ + 1;
    field_ = Field::Ignore;
  } else {
    wrongType();
  }
  if (depth_ >= sizeof(objects_)) { failed_ = true; return; }
  objects_[depth_++] = object;
}
void Response::end(bool object) {
  if (!depth_ || objects_[depth_ - 1] != object || field_ != Field::Ignore) { failed_ = true; return; }
  if (depth_ == daysDepth_) daysDepth_ = 0;
  if (--depth_ == 0) closed_ = true;
}
void Response::onBool(void* raw, bool) { static_cast<Response*>(raw)->wrongType(); }
void Response::onNull(void* raw) { static_cast<Response*>(raw)->wrongType(); }
void Response::onObjectStart(void* raw) { static_cast<Response*>(raw)->start(true); }
void Response::onObjectEnd(void* raw) { static_cast<Response*>(raw)->end(true); }
void Response::onArrayStart(void* raw) { static_cast<Response*>(raw)->start(false); }
void Response::onArrayEnd(void* raw) { static_cast<Response*>(raw)->end(false); }
void Response::onChunk(void* raw, const char*, size_t, bool) { static_cast<Response*>(raw)->wrongType(); }

}  // namespace WeReadTimeCloud
