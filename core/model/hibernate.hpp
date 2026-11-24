// core/model/hibernate.hpp — the tile hibernation policy, ported from lib/hibernate.js
// (docs/NATIVE-PORT.md §5 step 1). PURE: no pty, no timers, no I/O — it decides, the shell acts.
//
// STATUS: header only (no compiler on the box). Implementation is written against
// core/test/fixtures/hibernate.json, generated from lib/hibernate.js.
//
// The policy is THREE separable pieces, kept separate on purpose so a native shell can swap the DECISION
// for a real memory-pressure signal while the TEST stays untouched:
//   1. TEST      sleepBlocker(view, now, cfg) -> why this tile may NOT sleep, or empty if it may
//   2. TRIGGERS  wakeReason(view)             -> why a sleeping tile should wake, or empty
//   3. DECISION  chooseSleepers(views, ...)   -> of the eligible, which sleep this tick
#ifndef CLAUDE_DECK_CORE_HIBERNATE_HPP
#define CLAUDE_DECK_CORE_HIBERNATE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace deck::hibernate {

struct Config { bool enabled = false; int idleMinutes = 30; bool keepPinned = true; int maxPerTick = 3; };

// A plain snapshot of a tile the policy reasons over — the shell builds this from its own tile record.
// `shownBy` is the set of clients showing the tile ("win:0", "remote", …); the policy keys ONLY on
// whether it is empty, never on the token text (a key-typed-for-one-thing-accepting-another bug bit the
// JS once — see PORTING.md / the brief's F1 note). Times are ms epoch.
struct TileView {
  std::string id;
  std::string kind;          // "claude" | "shell"
  std::string sessionId;     // empty if none
  bool resumable = false;    // a transcript exists on disk (claude --resume would work)
  std::string status;        // "idle" | "busy" | "waiting" | "starting" | "asleep" | "off" | "shell"
  bool running = false;
  bool asleep = false;
  bool pinned = false;
  std::vector<std::string> shownBy;
  int  held = 0;             // number of special prompts queued for it
  int64_t lastActivityAt = 0;
};

struct SleepPick { std::string id; int64_t quietMs; };
struct WakePick  { std::string id; std::string reason; };

Config config(const Config& partial);                        // clamp / default (mirrors JS config())

// TEST: empty string = may sleep; otherwise the reason it may not (stable, human-readable, from the JS).
std::string sleepBlocker(const TileView& t, int64_t now, const Config& cfg);
bool        maySleep(const TileView& t, int64_t now, const Config& cfg);

// TRIGGERS: empty = leave asleep; otherwise the reason it should wake.
std::string wakeReason(const TileView& t, bool underPressure = false);   // under pressure, on screen does not wake

// DECISION: which of the eligible actually sleep now (longest-quiet first, capped by maxPerTick); and
// which sleeping tiles something now wants awake.
std::vector<SleepPick> chooseSleepers(const std::vector<TileView>& tiles, int64_t now, const Config& cfg);
// memory pressure (fraction of the commit limit in use, >= 0.9): every session that MAY sleep sleeps now —
// idle wait, per-tick cap and the on/off switch waived; the other blockers hold. Empty below the threshold.
extern const double PRESSURE_THRESHOLD;
extern const double PRESSURE_RELEASE;   // pressure mode ends only under this (hysteresis)
bool pressureMode(double pressure, bool wasOn);
std::vector<SleepPick> pressureSleepers(const std::vector<TileView>& tiles, int64_t now, const Config& cfg, double pressure);
std::vector<WakePick>  chooseWakers(const std::vector<TileView>& tiles, bool underPressure = false);

}  // namespace deck::hibernate

#endif  // CLAUDE_DECK_CORE_HIBERNATE_HPP
