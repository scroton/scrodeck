// core/model/prompts.cpp — the portable due-logic of the special-prompt schedule, ported from
// lib/queue.js (the dueReason method + inWindow/minutesOfDay). Delivery is shell I/O, not here.
// Oracle: core/test/fixtures/prompts.json.
#include "prompts.hpp"

#include <ctime>

namespace deck::prompts {

int minutesOfDay(int64_t nowMs) {
  std::time_t secs = (std::time_t)(nowMs / 1000);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &secs);
#else
  localtime_r(&secs, &tmv);
#endif
  return tmv.tm_hour * 60 + tmv.tm_min;
}

bool inWindow(int mins, int startMin, int endMin) {
  if (startMin == endMin) return true;                      // zero-length = any time
  return startMin < endMin ? (mins >= startMin && mins < endMin) : (mins >= startMin || mins < endMin);
}

bool oneShot(Kind k) { return k == Kind::Timed || k == Kind::Next; }

bool isDue(const Item& i, int64_t now, TriggerRisenAt risenAt, void* ctx) {
  if (i.kind == Kind::Recurring) {
    if (!inWindow(minutesOfDay(now), i.startMin, i.endMin)) return false;
    if (i.lastSentAt && now - i.lastSentAt < (int64_t)i.everyMin * 60000) return false;
    return true;
  }
  if (i.kind == Kind::Triggered) {
    int64_t risen = risenAt ? risenAt(i.trigger, ctx) : 0;
    if (!risen) return false;                               // trigger false
    if (i.firedAt >= risen) return false;                   // already fired for this edge
    return true;
  }
  return i.at <= now;                                       // timed / next
}

}  // namespace deck::prompts
