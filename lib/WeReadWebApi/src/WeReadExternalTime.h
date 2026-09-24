#pragma once

#include "WeReadTimeLedger.h"

namespace WeReadTime {

// Independent, read-only accounting receipt. Never changes native WRTL frames.
// Confirmed + unknown equals the covered prefix: unknown time is NOT sendable.
struct ExternalTime {
  static constexpr size_t kSize = 240;
  Identity identity;
  uint64_t sourceMs = 0;
  uint64_t coveredSeconds = 0;
  uint64_t confirmedSeconds = 0;
  uint64_t unknownSeconds = 0;

  static uint64_t number(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value;
  }
  static uint32_t checksum(const uint8_t* bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
      crc ^= bytes[i];
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
  }
  bool decode(const uint8_t* bytes, size_t size) {
    if (!bytes || size != kSize || std::memcmp(bytes, "WRTX", 4) || bytes[4] != 1 ||
        number(bytes + 232) != checksum(bytes, 232))
      return false;
    ExternalTime candidate;
    std::memcpy(candidate.identity.account, bytes + 8, 32);
    std::memcpy(candidate.identity.book, bytes + 40, 64);
    std::memcpy(candidate.identity.source, bytes + 104, 64);
    const auto day = number(bytes + 168);
    if (!day || day > UINT32_MAX) return false;
    candidate.identity.day = static_cast<uint32_t>(day);
    Ledger validator;
    if (!validator.bind(candidate.identity.account, candidate.identity.book, candidate.identity.source,
                        candidate.identity.day))
      return false;
    candidate.sourceMs = number(bytes + 176);
    candidate.coveredSeconds = number(bytes + 184);
    candidate.confirmedSeconds = number(bytes + 192);
    candidate.unknownSeconds = number(bytes + 200);
    if (candidate.coveredSeconds > candidate.sourceMs / 1000 || candidate.confirmedSeconds > candidate.coveredSeconds ||
        candidate.unknownSeconds != candidate.coveredSeconds - candidate.confirmedSeconds)
      return false;
    *this = candidate;
    return true;
  }
  bool balance(const Ledger& ledger, uint64_t& sendableSeconds) const {
    sendableSeconds = 0;
    const auto& id = ledger.identity();
    // Until a shared-writer migration exists, mixed native/external accounting
    // is ambiguous. Never double-subtract or assume the records are disjoint.
    if (id.day != identity.day || std::strcmp(id.account, identity.account) || std::strcmp(id.book, identity.book) ||
        std::strcmp(id.source, identity.source) || ledger.measuredMs() < sourceMs || ledger.verifiedSeconds() != 0 ||
        ledger.status() != Status::Pending || coveredSeconds > sourceMs / 1000 || confirmedSeconds > coveredSeconds ||
        unknownSeconds != coveredSeconds - confirmedSeconds)
      return false;
    sendableSeconds = ledger.measuredMs() / 1000 - coveredSeconds;
    return true;
  }
};
static_assert(sizeof(ExternalTime) <= 256, "External receipt must remain bounded");
}  // namespace WeReadTime
