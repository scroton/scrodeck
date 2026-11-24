// core/model/hibernate.cpp — the sleep policy, ported from lib/hibernate.js. PURE. Reason strings are
// reproduced exactly (they are part of the contract — a view may show them). Oracle:
// core/test/fixtures/hibernate.json.
#include "hibernate.hpp"

#include <algorithm>
#include <cmath>

namespace deck::hibernate {

Config config(const Config& p) {
  Config c = p;
  if (c.idleMinutes < 1) c.idleMinutes = 1; else if (c.idleMinutes > 24 * 60) c.idleMinutes = 24 * 60;
  if (c.maxPerTick < 1) c.maxPerTick = 1; else if (c.maxPerTick > 50) c.maxPerTick = 50;
  return c;
}

static std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; }
  return s;
}

std::string sleepBlocker(const TileView& t, int64_t now, const Config& cfgIn) {
  Config cfg = config(cfgIn);
  if (!cfg.enabled) return "hibernation is off";
  // (no null tile in C++: the shell passes a real TileView. The JS "no such tile" guard has no analogue.)
  if (t.kind != "claude" || t.sessionId.empty()) return "not a resumable session";
  // Note: the shell must set `resumable` truthfully. In the JS only an explicit `false` blocks; here the
  // bool defaults false, so an unset field would block — the shell is responsible for setting it.
  if (!t.resumable) return "nothing to resume yet";
  if (t.asleep) return "already asleep";
  if (!t.running) return "not running";
  if (cfg.keepPinned && t.pinned) return "pinned";
  if (!t.shownBy.empty()) return "on screen (" + join(t.shownBy, ", ") + ")";
  if (t.held > 0) return "holding a prompt";
  if (t.status != "idle") return "status: " + t.status;
  int64_t quiet = now - t.lastActivityAt;
  if (!(quiet >= (int64_t)cfg.idleMinutes * 60000)) {
    long secs = std::lround((double)quiet / 1000.0);
    if (secs < 0) secs = 0;
    return "active " + std::to_string(secs) + "s ago";
  }
  return "";   // may sleep
}

bool maySleep(const TileView& t, int64_t now, const Config& cfg) { return sleepBlocker(t, now, cfg).empty(); }

std::string wakeReason(const TileView& t, bool underPressure) {
  if (!t.asleep) return "";
  if (!underPressure && !t.shownBy.empty()) return "on screen (" + join(t.shownBy, ", ") + ")";
  if (t.held > 0) return "a prompt is waiting for it";
  return "";
}

std::vector<SleepPick> chooseSleepers(const std::vector<TileView>& tiles, int64_t now, const Config& cfgIn) {
  Config cfg = config(cfgIn);
  std::vector<SleepPick> out;
  if (!cfg.enabled) return out;
  std::vector<const TileView*> eligible;
  for (auto& t : tiles) if (maySleep(t, now, cfg)) eligible.push_back(&t);
  std::stable_sort(eligible.begin(), eligible.end(), [](const TileView* a, const TileView* b) { return a->lastActivityAt < b->lastActivityAt; });
  for (int i = 0; i < (int)eligible.size() && i < cfg.maxPerTick; ++i)
    out.push_back({ eligible[i]->id, now - eligible[i]->lastActivityAt });
  return out;
}

const double PRESSURE_THRESHOLD = 0.9;
const double PRESSURE_RELEASE = 0.85;
bool pressureMode(double pressure, bool wasOn) { return wasOn ? !(pressure < PRESSURE_RELEASE) : pressure >= PRESSURE_THRESHOLD; }
std::vector<SleepPick> pressureSleepers(const std::vector<TileView>& tiles, int64_t now, const Config& cfgIn, double pressure) {
  std::vector<SleepPick> out;
  if (!(pressure >= PRESSURE_THRESHOLD)) return out;
  Config cfg = config(cfgIn); cfg.enabled = true; cfg.idleMinutes = 0; cfg.keepPinned = false;   // under pressure pinned and on-screen give way too
  std::vector<TileView> relaxed = tiles; for (auto& t : relaxed) t.shownBy.clear();
  std::vector<const TileView*> eligible;
  for (auto& t : relaxed) if (maySleep(t, now, cfg)) eligible.push_back(&t);
  std::stable_sort(eligible.begin(), eligible.end(), [](const TileView* a, const TileView* b) { return a->lastActivityAt < b->lastActivityAt; });
  for (auto* t : eligible) out.push_back({ t->id, now - t->lastActivityAt });
  return out;
}

std::vector<WakePick> chooseWakers(const std::vector<TileView>& tiles, bool underPressure) {
  std::vector<WakePick> out;
  for (auto& t : tiles) { std::string r = wakeReason(t, underPressure); if (!r.empty()) out.push_back({ t.id, r }); }
  return out;
}

}  // namespace deck::hibernate
