#pragma once

#include "WeReadServiceJournal.h"

namespace WeReadTime {
// One transient workspace per explicit handoff. Kept off the 8 KiB TLS stack;
// no history-sized buffer and no change to immutable WRS1 task identities.
struct ServiceBatch {
  static constexpr size_t kMaxItems = 16, kBodySize = 8192;
  enum class Action : uint8_t { Submit, Query };
  enum class Receipt : uint8_t { Queued, Running, Uncertain, Confirmed };
  struct Item {
    uint32_t day = 0;
    uint64_t start = 0, end = 0, credit = 0, receivedCredit = 0;
    Action action = Action::Query;
    Receipt receipt = Receipt::Queued;
  };
  Item items[kMaxItems]{};
  size_t count = 0;
  char body[kBodySize]{};
  bool add(uint32_t day, const ServiceJournal& journal) {
    if (count == kMaxItems || journal.state() == ServiceJournal::State::Empty ||
        journal.state() == ServiceJournal::State::Confirmed)
      return false;
    for (size_t i = 0; i < count; ++i)
      if (items[i].day == day && items[i].start == journal.start()) return false;
    auto& item = items[count++];
    item = {};
    item.day = day;
    item.start = journal.start();
    item.end = journal.end();
    item.credit = journal.credit();
    item.action = journal.state() == ServiceJournal::State::Reserved ? Action::Submit : Action::Query;
    return true;
  }
};
static_assert(sizeof(ServiceBatch) < 10 * 1024, "Fixed batch workspace exceeded");
}  // namespace WeReadTime
