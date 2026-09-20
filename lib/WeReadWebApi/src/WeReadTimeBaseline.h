#pragma once
#include "WeReadTimeCloud.h"
#include "WeReadPacedTime.h"

namespace WeReadTime {
// Shared by device transport and offline replay: absence is never numeric zero.
inline AccountSnapshot cloudBaseline(const WeReadTimeCloud::Snapshot& cloud,
                                     const Identity& identity, uint64_t sampledAt) {
  AccountSnapshot result{};
  std::memcpy(result.account, identity.account, sizeof(result.account));
  result.month = static_cast<uint32_t>(cloud.month);
  result.day = static_cast<uint32_t>(cloud.day);
  result.monthSeconds = cloud.monthSeconds;
  result.daySeconds = cloud.daySeconds;
  result.sampledAt = sampledAt;
  result.complete = cloud.hasDay;
  result.monthComplete = true; // Caller has already required Query::Ready.
  return result;
}
}
