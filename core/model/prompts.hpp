// core/model/prompts.hpp — the portable heart of the special-prompt schedule, ported from lib/queue.js
// (docs/NATIVE-PORT.md, R17). PURE: this is the DUE-LOGIC only. Delivery (bracketed paste, the Enter, the
// busy-confirm) is shell I/O and stays in the shell — the core only answers "is this item due now?".
//
// STATUS: header only (no compiler on the box). Implementation is written against
// core/test/fixtures/prompts.json, generated from lib/queue.js.
//
// Four kinds (R17): timed (deliver at a time), next (deliver at the next idle-at-prompt), recurring (a
// daily clock window + interval, re-arms), triggered (gated on a named trigger's rising edge). The trigger
// STORE lives in triggers.hpp; here the item just carries the trigger name and the port asks the store for
// its rising-edge time.
#ifndef CLAUDE_DECK_CORE_PROMPTS_HPP
#define CLAUDE_DECK_CORE_PROMPTS_HPP

#include <cstdint>
#include <string>

namespace deck::prompts {

// minutes since local midnight for an epoch-ms instant (shell provides the local-time breakdown)
int minutesOfDay(int64_t nowMs);
// is `mins` inside a daily window [start, end]? Windows may wrap past midnight (start > end). A
// zero-length window (start == end) means "any time". (Pure; port directly.)
bool inWindow(int mins, int startMin, int endMin);

enum class Kind { Timed, Next, Recurring, Triggered };

// The schedulable fields of one special prompt. `text`, ids, delivery state etc. live in the shell's own
// item record; the core needs only what gates delivery.
struct Item {
  Kind kind = Kind::Timed;
  int64_t at = 0;            // timed/next: due time (next is "now" at creation)
  int everyMin = 0;          // recurring: interval
  int startMin = 0;          // recurring: window (minutes of day)
  int endMin = 0;
  int64_t lastSentAt = 0;    // recurring: last delivery (0 = never)
  std::string trigger;       // triggered: the gating trigger's name
  int64_t firedAt = 0;       // triggered: the edge time we last fired for (0 = never)
};

// The rising-edge time of a named trigger (0 if false/never) — supplied by the shell from the trigger
// store (triggers.hpp), so this module stays free of the store's state.
using TriggerRisenAt = int64_t (*)(const std::string& name, void* ctx);

// Is the item due to be delivered at `now`? true = due. Kind-specific, mirroring lib/queue.js dueReason:
//   timed/next   at <= now
//   recurring    now's time-of-day in [start,end]  AND  (never sent OR now - lastSentAt >= everyMin*60000)
//   triggered    the trigger has risen (risenAt > 0)  AND  firedAt < risenAt  (once per rising edge)
bool isDue(const Item& item, int64_t now, TriggerRisenAt risenAt, void* ctx);

// true for the one-shot kinds (timed, next); the standing kinds (recurring, triggered) re-arm.
bool oneShot(Kind k);

}  // namespace deck::prompts

#endif  // CLAUDE_DECK_CORE_PROMPTS_HPP
