// core/session/meta.cpp — see meta.hpp. Ported from main.js collectMeta / hibernateView / quietViews /
// restartView and the parts of lib/hibernate.js and lib/quietrestart.js they call.
#include "meta.hpp"
#include "jsutil.hpp"

#include <algorithm>

namespace deck { namespace session {

using namespace js;

static json orNull(const std::optional<std::string>& s) { return s ? json(*s) : json(); }
static bool has(const std::optional<std::string>& s) { return s && !s->empty(); }             // JS truthiness of a string field
// a JS number as JSON: integers print as integers, NaN as null (JSON.stringify)
static json num(double d) {
  if (std::isnan(d) || std::isinf(d)) return json();
  if (d == std::floor(d) && std::fabs(d) < 9007199254740992.0) return json((int64_t)d);
  return json(d);
}
static json numOrNull(double d) { return d != 0 && !std::isnan(d) ? num(d) : json(); }       // `x || null`

std::optional<std::string> cleanOsc(const std::optional<std::string>& t) {
  if (!has(t)) return std::nullopt;
  const std::string& s = *t;
  // /\.exe\s*$/i
  { size_t i = 0, end = 0;
    while (i < s.size()) { if (!isSpace(nextCodePoint(s, i))) end = i; }
    if (end >= 4) {
      std::string tail = s.substr(end - 4, 4);
      for (char& c : tail) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (tail == ".exe") return std::nullopt;
    } }
  // /^[A-Za-z]:\\/
  if (s.size() >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && s[2] == '\\') return std::nullopt;
  // /^\W*\s*Claude Code\s*$/i  (\W: anything that is not [A-Za-z0-9_], so every non-ASCII glyph and space)
  { size_t i = 0;
    while (i < s.size() && !isWordChar((unsigned char)s[i])) i++;
    static const char kIdle[] = "claude code";
    bool match = s.size() >= i + 11;
    for (size_t k = 0; match && k < 11; k++) { char c = s[i + k]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); if (c != kIdle[k]) match = false; }
    if (match) {
      size_t j = i + 11; bool onlySpace = true;
      while (j < s.size()) { if (!isSpace(nextCodePoint(s, j))) { onlySpace = false; break; } }
      if (onlySpace) return std::nullopt;
    } }
  return t;
}

HibernateConfig hibernateConfig(const json& v) {
  HibernateConfig c;
  if (!v.is_object()) return c;
  auto clampInt = [&](const char* k, int lo, int hi, int dflt) {
    double d = 0;
    if (!finiteNumber(v, k, &d)) return dflt;
    return (int)std::max((double)lo, std::min((double)hi, js::round(d)));
  };
  if (get(v, "enabled").is_boolean()) c.enabled = get(v, "enabled").get<bool>();
  c.idleMinutes = clampInt("idleMinutes", 1, 24 * 60, 30);
  if (get(v, "keepPinned").is_boolean()) c.keepPinned = get(v, "keepPinned").get<bool>();
  c.maxPerTick = clampInt("maxPerTick", 1, 50, 3);
  return c;
}

static std::string joinComma(const std::vector<std::string>& v) { std::string s; for (size_t i = 0; i < v.size(); i++) { if (i) s += ", "; s += v[i]; } return s; }

// lib/hibernate.js sleepBlocker over main.js hibernateView(t): null (may sleep) or the reason it may not
static json sleepBlocker(const TileState& t, const TileTick& in, const std::string& viewStatus, int background, double now, const HibernateConfig& cfg) {
  if (!cfg.enabled) return "hibernation is off";
  if (t.kind != "claude" || !has(t.sessionId)) return "not a resumable session";
  if (!in.resumable) return "nothing to resume yet";
  if (t.asleep) return "already asleep";
  if (!t.running) return "not running";
  if (cfg.keepPinned && t.pinned) return "pinned";
  if (!in.shownBy.empty()) return "on screen (" + joinComma(in.shownBy) + ")";
  if (in.held.is_array() && !in.held.empty()) return "holding a prompt";
  if (viewStatus != "idle") return "status: " + viewStatus;
  if (background > 0) return "background work running (" + std::to_string(background) + ")";
  const double lastActivityAt = std::max(std::max(t.startedAt, t.lastStoppedAt), std::max(t.lastInputAt, t.busySince));
  const double quiet = now - lastActivityAt;
  if (!(quiet >= cfg.idleMinutes * 60000.0)) return "active " + numberText(std::max(0.0, js::round(quiet / 1000))) + "s ago";
  return json();
}

json tileMeta(TileState& t, const TileTick& in, double now, const json& settings, TickEffects* fx) {
  const LiveRecord* l = has(t.sessionId) ? in.live : nullptr;
  const TranscriptTail* tail = (t.kind == "claude" && has(t.sessionId)) ? in.tail : nullptr;
  const TailFacts* f = tail ? &tail->facts() : nullptr;
  if (f) {
    if (has(f->title)) t.knownTitle = f->title;
    if (!t.lastStoppedInit && has(f->lastAssistantAt)) {
      const double at = dateParse(*f->lastAssistantAt);
      t.lastStoppedAt = std::max(t.lastStoppedAt, std::isnan(at) ? 0.0 : at);
      t.lastStoppedInit = true;
    }
  }
  const bool live = l && l->alive;
  std::string status = "off";
  if (t.asleep) status = "asleep";
  else if (t.waking) status = "starting";
  else if (t.running) {
    if (t.kind == "shell") status = "shell";
    else status = live ? (l->status.is_string() ? l->statusText() : toString(l->status)) : "starting";
  }
  else if (t.kind == "cloud" && has(t.cloudId)) status = (in.cloud && in.cloud->is_object()) ? toString(get(*in.cloud, "status")) : "idle";   // busy / waiting / idle / archived
  std::optional<std::string> startupWait;
  if (in.startupOpen) {
    if (live || now - in.startupSince > 30 * 60000.0) { if (fx) fx->finishStartup = true; }
    else if (status == "starting" && !t.waking) startupWait = in.startupWaitTitle;
  }
  if (startupWait) status = "waiting";
  const bool wasBusy = t.lastStatus && *t.lastStatus == "busy";
  if (status == "busy" && !wasBusy) t.busySince = now;
  t.waitingFor = status != "waiting" ? json() : startupWait ? json("startup: " + *startupWait)
    : (in.cloud && in.cloud->is_object()) ? get(*in.cloud, "waitingFor") : (l ? l->waitingFor : json());
  if (startupWait && !(t.lastStatus && *t.lastStatus == "waiting")) { if (fx) { fx->notify = true; fx->notifyStatus = "waiting"; fx->turnMs = 0; } }
  if (status != "asleep" && !(t.lastStatus && *t.lastStatus == "asleep") && wasBusy && status != "busy" && status != "starting") {
    t.lastStoppedAt = now;
    if (fx) { fx->notify = true; fx->notifyStatus = status; fx->turnMs = now - (t.busySince != 0 ? t.busySince : now); }
  }
  t.lastStatus = status;
  if (l && truthy(l->bridgeSessionId)) t.bridge = l->bridgeSessionId;

  const std::string driver = truthy(get(settings, "driver")) ? toString(get(settings, "driver")) : "auto";
  const std::optional<std::string> osc = cleanOsc(t.oscTitle);
  json title;
  if (has(t.userTitle)) title = *t.userTitle;
  else if (f && has(f->title)) title = *f->title;
  else if (has(t.knownTitle)) title = *t.knownTitle;
  else if (osc) title = *osc;
  else if (l && truthy(l->name)) title = l->name;

  json lastAt;
  if (live && l->statusUpdatedAt != 0) lastAt = num(l->statusUpdatedAt);
  else if (f && has(f->lastAt)) lastAt = num(dateParse(*f->lastAt));
  else lastAt = num(t.startedAt);

  // hibernateView(t).status, after lastStatus was stamped above
  const std::string viewStatus = t.asleep ? "asleep" : t.waking ? "starting" : (t.running ? (t.kind == "shell" ? "shell" : (has(t.lastStatus) ? *t.lastStatus : "starting")) : "off");
  const int background = (t.running && t.kind == "claude" && tail) ? (int)tail->background(t.startedAt, now).size() : 0;

  json m = json::object();
  m["id"] = t.id;
  m["kind"] = t.kind;
  m["account"] = orNull(t.account);
  m["sessionId"] = orNull(t.sessionId);
  m["pinned"] = t.pinned;
  m["group"] = has(t.group) ? json(*t.group) : json();
  m["title"] = title;
  m["lastText"] = (f && has(f->lastText)) ? json(*f->lastText) : json();
  m["model"] = (f && has(f->lastModel)) ? json(*f->lastModel) : json();
  m["effort"] = (f && has(f->lastEffort)) ? json(*f->lastEffort) : json();
  m["ultracode"] = !!(f && t.running && f->ultracodeSince(std::max(t.startedAt, live ? l->startedAt : 0.0)));
  m["browser"] = t.browser ? json(*t.browser) : json();
  m["desk"] = has(t.desk) ? json(*t.desk) : json();
  m["attention"] = t.attentionAt != 0 && now - t.attentionAt < 10 * 60000.0 && !(t.lastInputAt > t.attentionAt);
  m["context"] = (f && f->lastContextTokens)
    ? num(std::min(100.0, js::round(100 * *f->lastContextTokens / (in.contextWindow != 0 ? in.contextWindow : 200000.0))))
    : json();
  m["lastAt"] = lastAt;
  m["bridge"] = (l && truthy(l->bridgeSessionId)) ? l->bridgeSessionId : (truthy(t.bridge) ? t.bridge : json());
  m["mirror"] = (t.phoneSize && (driver == "phone" || (driver != "desk" && in.remoteWatching)))
    ? json{ { "cols", t.phoneSize->first }, { "rows", t.phoneSize->second } } : json();
  m["ptySize"] = t.running ? json{ { "cols", t.ptyCols }, { "rows", t.ptyRows } } : json();
  m["altScreen"] = t.modes.altScreen();
  m["mouseOn"] = !t.asleep && t.modes.mouseOn();
  m["sgrMouse"] = !t.asleep && t.modes.sgrMouse();
  m["asleep"] = t.asleep;
  m["sleptAt"] = numOrNull(t.sleptAt);
  m["shownBy"] = in.shownBy;
  m["inWindow"] = in.inWindow ? json(*in.inWindow) : json();
  m["sleepBlocker"] = sleepBlocker(t, in, viewStatus, background, now, hibernateConfig(get(settings, "hibernate")));
  m["driver"] = has(t.lastInputFrom) ? json(*t.lastInputFrom) : json();
  m["driverAt"] = numOrNull(t.lastInputAt);
  m["lastStoppedAt"] = num(t.lastStoppedAt);
  m["status"] = status;
  m["exited"] = t.exited;
  m["exitCode"] = t.exitCode ? json(*t.exitCode) : json();
  m["cwd"] = t.cwd;
  m["fresh"] = !t.hadInput && (t.kind == "shell" || !f || (!has(f->lastRole) && !has(f->lastText)));
  m["held"] = in.held.is_array() ? in.held : json::array();
  return m;
}

json settingsMeta(const json& s, const json& pagesShown) {
  json o = json::object();
  auto copy = [&](const char* k) { if (s.is_object() && s.find(k) != s.end()) o[k] = s[k]; };
  copy("fontSize");
  o["zoomFontSize"] = truthy(get(s, "zoomFontSize")) ? get(s, "zoomFontSize") : json(0);
  o["fontFamily"] = truthy(get(s, "fontFamily")) ? get(s, "fontFamily") : json("");
  copy("cwd"); copy("effortStyle"); copy("sidebarSide"); copy("pinSide"); copy("bars"); copy("barLabels"); copy("barLabelMode"); copy("msgLines"); copy("notify");
  o["animations"] = !(get(s, "animations") == false);
  o["driver"] = truthy(get(s, "driver")) ? get(s, "driver") : json("auto");
  o["grid"] = truthy(get(s, "grid")) ? get(s, "grid") : json{ { "rows", 2 }, { "cols", 2 } };
  o["oneWindow"] = !(get(s, "oneWindow") == false);
  o["stepEscape"] = !(get(s, "stepEscape") == false);
  json asks = { { "deckThreshold", nullptr }, { "picker", "alongside" }, { "usePages", true }, { "background", true },
    { "showTile", true }, { "showMarks", true }, { "closeOnDone", true } };   // DEFAULT_SETTINGS.asks
  const json& mine = get(s, "asks");
  if (mine.is_object()) for (auto it = mine.begin(); it != mine.end(); ++it) asks[it.key()] = it.value();
  o["asks"] = asks;
  o["pages"] = pagesShown.is_array() ? pagesShown : json::array();
  return o;
}

static std::string mins(double ms) {
  const double m = js::round(ms / 60000);
  if (m < 1) return "<1m";
  if (m < 60) return numberText(m) + "m";
  const int mm = (int)std::fmod(m, 60);
  return numberText(std::floor(m / 60)) + "h" + (mm < 10 ? "0" : "") + std::to_string(mm);
}
static std::string bgReason(const BackgroundWork& b) {
  std::string what;
  if (b.kind == "cron") what = b.recurring ? "recurring cron" : "scheduled check-in";
  else if (b.kind == "shell") what = "background shell";
  else if (b.kind == "agent") what = "background agent";
  else if (b.kind == "workflow") what = "workflow";
  else if (b.kind == "task") what = "background task";
  else if (b.kind == "monitor") what = "monitor";
  else if (b.kind == "wakeup") what = "loop wakeup";
  else what = "background work";
  return has(b.label) ? what + ": " + *b.label : what;
}

json restartBlockers(const std::vector<QuietView>& views, double now) {
  json out = json::array();
  for (const QuietView& v : views) {
    if (v.kind == "shell" || v.asleep || !v.running) continue;
    auto row = [&](const std::string& reason) { out.push_back({ { "id", v.id }, { "title", has(v.title) ? *v.title : std::string("untitled session") }, { "reason", reason } }); };
    if (v.status == "busy") row(v.busySince != 0 ? "mid-turn (" + mins(now - v.busySince) + ")" : "mid-turn");
    else if (v.status == "waiting") row(truthy(v.waitingFor) ? "waiting for you (" + toString(v.waitingFor) + ")" : "waiting for you");
    else if (v.status == "starting") row("starting up");
    for (const BackgroundWork& b : v.background) row(bgReason(b));
    if (v.lastKeyAt != 0 && now - v.lastKeyAt < 60000) row("typed " + numberText(std::max(1.0, js::round((now - v.lastKeyAt) / 1000))) + "s ago");
  }
  return out;
}
int shellsLost(const std::vector<QuietView>& views) { int n = 0; for (const QuietView& v : views) if (v.kind == "shell" && v.running) n++; return n; }

json restartViewUnarmed(const json& blockers, int shells) {
  return { { "armed", false }, { "why", nullptr }, { "armedAt", nullptr }, { "quietSince", nullptr }, { "settleLeftMs", nullptr },
    { "blockers", blockers.is_array() ? blockers : json::array() }, { "shells", shells }, { "update", nullptr } };
}

QuietView quietView(const TileState& t, const json& meta, const TranscriptTail* tail, double now) {
  QuietView v;
  v.id = t.id;
  const json& title = get(meta, "title");
  if (truthy(title)) v.title = toString(title);
  v.kind = t.kind;
  v.status = t.asleep ? "asleep" : t.waking ? "starting" : (t.running ? (t.kind == "shell" ? "shell" : (has(t.lastStatus) ? *t.lastStatus : "starting")) : "off");
  v.running = t.running; v.asleep = t.asleep; v.busySince = t.busySince; v.waitingFor = t.waitingFor; v.lastKeyAt = t.lastKeyAt;
  if (t.running && t.kind == "claude" && tail) v.background = tail->background(t.startedAt, now);
  return v;
}

json deckMeta(const DeckInputs& in, const std::vector<std::pair<bool, json>>& tiles) {
  json pinned = json::array();
  std::vector<const json*> rest;
  for (const auto& kv : tiles) { if (kv.first) pinned.push_back(kv.second); else rest.push_back(&kv.second); }
  auto stamp = [](const json* m) { const json& v = get(*m, "lastStoppedAt"); return v.is_number() ? v.get<double>() : 0.0; };
  std::stable_sort(rest.begin(), rest.end(), [&](const json* a, const json* b) { return stamp(a) > stamp(b); });
  for (const json* m : rest) pinned.push_back(*m);
  json o = json::object();
  o["now"] = num(in.now);
  o["restoring"] = in.restoring;
  o["accounts"] = in.accounts;
  o["settings"] = settingsMeta(in.settings, in.pagesShown);
  o["groups"] = in.groups;
  o["usage"] = in.usage;
  o["triggers"] = in.triggers;
  o["restart"] = in.restart;
  o["browserOption"] = true;
  o["cloudOption"] = true;
  o["asks"] = in.asks;
  o["askLog"] = in.askLog;
  o["tiles"] = pinned;
  return o;
}

}}  // namespace deck::session
