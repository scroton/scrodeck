// core/deck/onedeck.cpp — lib/onedeck.js, line for line where it can be (see onedeck.hpp).
#include "onedeck.hpp"
#include "util.hpp"
#include "rules.hpp"
#include "../session/jsutil.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace deck { namespace core { namespace onedeck {

namespace js = deck::session::js;

const char* const MARKER = "active.json";
const char* const SHARED = "shared-layout.json";
const char* const SYNC = "onedeck.json";
const char* const BACKUPS = "onedeck-backups";
const std::vector<std::string> OWN_FILES = { "tiles.json", "settings.json", "held-prompts.json", "triggers.json", "onedeck.json" };
const std::vector<std::string> LOCAL_SETTINGS = { "phone", "pages", "fontSize", "zoomFontSize", "fontFamily", "cwd" };
static bool localKey(const std::string& k) { return std::find(LOCAL_SETTINGS.begin(), LOCAL_SETTINGS.end(), k) != LOCAL_SETTINGS.end(); }
json sharedSettings(const json& settings) {
  json out = json::object();
  if (settings.is_object()) for (auto it = settings.begin(); it != settings.end(); ++it) if (!localKey(it.key())) out[it.key()] = it.value();
  return out;
}
json mergeSettings(const json& own, const json& shared) {
  json out = own.is_object() ? own : json::object();
  if (shared.is_object()) for (auto it = shared.begin(); it != shared.end(); ++it) if (!localKey(it.key())) out[it.key()] = it.value();
  return out;
}

std::string otherOf(const std::string& deck) { return deck == "native" ? "electron" : "native"; }
static bool knownDeck(const json& d) { return d.is_string() && (d.get<std::string>() == "electron" || d.get<std::string>() == "native"); }
static int intOf(const json& v) { if (!v.is_number()) return 0; const double d = v.get<double>(); return std::isfinite(d) ? (int)d : 0; }   // `x | 0`
static double numOf(const json& v) { return v.is_number() ? v.get<double>() : 0; }

double parseIso(const std::string& s) { return js::dateParse(s); }

json readJson(Files& files, const std::string& path) {
  std::string text;
  if (!files.read(path, text)) return json();
  const json v = parseState(text);
  return v.is_object() ? v : json();
}

// ---------------------------------------------------------------- the marker
json markerJson(const Marker& m) {
  json o = json::object();
  o["v"] = V; o["deck"] = m.deck; o["running"] = m.running; o["pid"] = m.pid; o["exe"] = m.exe;
  o["args"] = json::array(); for (const auto& a : m.args) o["args"].push_back(a);
  o["stateDir"] = m.stateDir; o["startedAt"] = m.startedAt; o["heartbeatAt"] = m.heartbeatAt; o["liveSessions"] = std::max(0, m.liveSessions);
  return o;
}
json parseMarker(const json& m) {
  if (!m.is_object() || !js::get(m, "v").is_number() || js::get(m, "v").get<double>() != V || !knownDeck(js::get(m, "deck"))) return json();
  if (!js::get(m, "stateDir").is_string() || js::str(js::get(m, "stateDir")).empty()) return json();
  return m;
}
bool sameExe(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  auto norm = [](std::string s) { for (char& c : s) { if (c == '/') c = '\\'; } s = lowerAscii(s); while (s.size() > 1 && s.back() == '\\') s.pop_back(); return s; };
  return norm(a) == norm(b);
}
bool heartbeatFresh(const json& m, double now) {
  const double t = parseIso(js::str(js::get(m, "heartbeatAt")));
  return std::isfinite(t) && now - t < FRESH_MS;
}
bool markerRunning(const json& m, const std::function<std::string(double)>& exeOf, double now) {
  if (!m.is_object() || js::get(m, "running") != json(true)) return false;
  const std::string exe = exeOf ? exeOf(numOf(js::get(m, "pid"))) : std::string();
  return sameExe(exe, js::str(js::get(m, "exe"))) && heartbeatFresh(m, now);
}
bool mayWriteMarker(const json& m, const std::string& deck, double pid, const std::function<std::string(double)>& exeOf, double now) {
  if (!m.is_object()) return true;
  if (js::str(js::get(m, "deck")) == deck && numOf(js::get(m, "pid")) == pid) return true;
  return !markerRunning(m, exeOf, now);
}

// ---------------------------------------------------------------- the guard
static std::string plural(int n) { return n == 1 ? "" : "s"; }
Busy otherBusy(const std::string& me, const json& m, const std::function<std::string(double)>& exeOf, double now,
               const std::vector<uint32_t>& otherPids, const std::function<bool(std::vector<std::string>&)>& otherTileIds,
               const std::set<std::string>& liveIds) {
  Busy b; b.other = otherOf(me);
  const std::string name = b.other == "native" ? "the native Claude Deck" : "the Electron Claude Deck";
  if (m.is_object() && js::str(js::get(m, "deck")) == b.other && markerRunning(m, exeOf, now)) {
    b.n = std::max(0, intOf(js::get(m, "liveSessions")));
    b.busy = b.n > 0; b.source = "marker";
    b.why = name + " runs (pid " + js::toString(js::get(m, "pid")) + ") with " + std::to_string(b.n) + " live session" + plural(b.n) + " (its marker)";
    return b;
  }
  if (!otherPids.empty()) {
    b.source = "process";
    std::vector<std::string> ids;
    if (!otherTileIds || !otherTileIds(ids)) { b.busy = true; b.known = false; b.why = name + " runs and its tiles.json does not read: taken as busy"; return b; }
    for (const auto& id : ids) if (liveIds.count(id)) b.n++;
    b.busy = b.n > 0;
    b.why = name + " runs with " + std::to_string(b.n) + " live session" + plural(b.n) + " (its tiles.json)";
    return b;
  }
  b.source = "none"; b.why = name + " does not run";
  return b;
}
std::string blockedText(int n) { return "The other Claude Deck is running with " + (n > 0 ? std::to_string(n) : std::string("its")) + " session" + plural(n) + " - quit it first"; }

static std::string lowerName(const std::string& s) { return lowerAscii(s); }
std::vector<uint32_t> findDeckPids(const std::vector<Proc>& procs, const std::string& deck, const std::string& exe, uint32_t self) {
  std::map<uint32_t, const Proc*> byPid;
  for (const Proc& p : procs) byPid[p.pid] = &p;
  std::vector<uint32_t> out;
  for (const Proc& p : procs) {
    if (p.pid == self) continue;
    if (!exe.empty()) { if (sameExe(p.path, exe)) out.push_back(p.pid); continue; }
    const std::string name = lowerName(p.name);
    if (deck == "native") { if (name == "claudedecknative.exe" || name == "claudedecknative") out.push_back(p.pid); continue; }
    if (name != "electron.exe" && name != "electron") continue;
    if (lowerAscii(p.path).find("claude-deck") == std::string::npos) continue;
    auto parent = byPid.find(p.ppid);
    if (parent != byPid.end()) { const std::string pn = lowerName(parent->second->name); if (pn == "electron.exe" || pn == "electron") continue; }
    out.push_back(p.pid);
  }
  return out;
}
bool tileSessionIds(Files& files, const std::string& stateDir, std::vector<std::string>& out) {
  const std::string f = joinPath(stateDir, "tiles.json");
  if (!files.isFile(f)) return true;
  const json j = readJson(files, f);
  if (!j.is_object()) return false;
  const json& t = js::get(j, "tiles");
  if (t.is_array()) for (const json& x : t) if (js::truthy(js::get(x, "sessionId"))) out.push_back(js::toString(js::get(x, "sessionId")));
  return true;
}

std::set<std::string> liveSessionIds(Files& files, ProcessList& procs, const std::string& aliasFile, const std::string& home, bool win) {
  std::set<std::string> out;
  std::string text; files.read(aliasFile, text);
  for (const Account& a : parseAccounts(text, home, win)) {
    const std::string dir = joinPath(a.dir, "sessions");
    for (const std::string& n : files.list(dir)) {
      if (n.size() < 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
      const json j = readJson(files, joinPath(dir, n));
      const double pid = js::get(j, "pid").is_number() ? js::get(j, "pid").get<double>() : 0;
      if (js::truthy(js::get(j, "sessionId")) && pid > 0 && procs.alive(pid)) out.insert(js::toString(js::get(j, "sessionId")));
    }
  }
  return out;
}
Busy guardAtStart(Files& files, ProcessList& procs, Env& env, double now, const json& od, const std::string& aliasFile, const std::string& home) {
  const std::string me = js::str(js::get(od, "deck")).empty() ? std::string("native") : js::str(js::get(od, "deck"));
  const json m = parseMarker(readJson(files, joinPath(js::str(js::get(od, "sharedDir")), MARKER)));
  std::vector<Proc> snap;
  const bool haveSnap = procs.snapshot(snap);
  std::map<double, std::string> pathOf;
  for (const Proc& p : snap) pathOf[(double)p.pid] = p.path;
  const std::string markerExe = js::str(js::get(m, "exe"));
  // the marker's pid -> its file; a process Windows will not describe (another user's, elevated) counts as the marker's
  // exe (taken as running: the guard errs on the side of blocking)
  auto exeOf = [&](double pid) -> std::string {
    if (haveSnap) { auto it = pathOf.find(pid); if (it == pathOf.end()) return std::string(); return it->second.empty() ? markerExe : it->second; }
    return pid > 0 && procs.alive(pid) ? markerExe : std::string();
  };
  const bool def = js::truthy(js::get(od, "defaultDeck"));
  const std::string otherExe = def ? std::string() : js::str(js::get(od, "otherExe"));
  std::vector<uint32_t> otherPids;
  if (haveSnap && (def || !otherExe.empty())) otherPids = findDeckPids(snap, otherOf(me), otherExe, env.pid());
  std::string otherState = (m.is_object() && js::str(js::get(m, "deck")) == otherOf(me)) ? js::str(js::get(m, "stateDir")) : std::string();
  if (otherState.empty()) otherState = js::str(js::get(od, "otherState"));
  const std::set<std::string> live = otherPids.empty() ? std::set<std::string>() : liveSessionIds(files, procs, aliasFile, home, env.os() == "win32");
  return otherBusy(me, m, exeOf, now, otherPids, [&](std::vector<std::string>& ids) { return otherState.empty() ? true : tileSessionIds(files, otherState, ids); }, live);
}

// ---------------------------------------------------------------- the same-session guard
Holder holderOf(const std::string& sessionId, const std::vector<Record>& records, const std::set<double>& ownPids, const json& otherDeckTiles) {
  Holder h;
  if (sessionId.empty()) return h;
  bool inOther = false;
  if (otherDeckTiles.is_array()) for (const json& t : otherDeckTiles) if (js::str(js::get(t, "sessionId")) == sessionId && !js::truthy(js::get(t, "asleep"))) inOther = true;
  for (const Record& r : records) {
    if (r.sessionId != sessionId || !r.alive || ownPids.count(r.pid)) continue;
    h.held = true; h.where = inOther ? "deck" : "terminal"; h.pid = r.pid;
    return h;
  }
  if (inOther) { h.held = true; h.where = "deck"; h.pid = 0; }
  return h;
}

// ---------------------------------------------------------------- translators
json toShared(const json& tilesJson, const json& held, const json& triggers, const json& settings, const std::string& deck, double atMs, int realChange, const json& lastChange) {
  const json st = tilesJson.is_object() ? tilesJson : json::object();
  const json& ws = js::get(st, "windows");
  json wins = (ws.is_array() && !ws.empty()) ? ws : json::array({ { { "layout", js::get(st, "layout").is_object() ? js::get(st, "layout") : json::object() } } });
  json out = json::object();
  out["v"] = V; out["writtenBy"] = deck; out["writtenAt"] = isoTime(atMs); out["realChange"] = realChange;
  out["lastChange"] = lastChange.is_object() ? lastChange : json();
  out["tiles"] = js::get(st, "tiles").is_array() ? js::get(st, "tiles") : json::array();
  out["windows"] = json::array();
  for (size_t i = 0; i < wins.size(); i++) {
    const json& l = js::get(wins[i], "layout");
    out["windows"].push_back({ { "index", (int)i }, { "layout", l.is_object() ? l : json::object() } });
  }
  out["groups"] = js::get(st, "groups").is_array() ? js::get(st, "groups") : json::array();
  out["heldPrompts"] = js::get(held, "items").is_array() ? js::get(held, "items") : json::array();
  out["triggers"] = js::get(triggers, "triggers").is_object() ? js::get(triggers, "triggers") : json::object();
  out["settings"] = sharedSettings(settings);
  return out;
}
Own fromShared(const json& shared, const json& ownIn, const json& ownSettings) {
  const json own = ownIn.is_object() ? ownIn : json::object();
  const json& ownWins = js::get(own, "windows");
  json wins = js::get(shared, "windows").is_array() && !js::get(shared, "windows").empty() ? js::get(shared, "windows") : json::array({ { { "index", 0 }, { "layout", json::object() } } });
  std::vector<json> sorted(wins.begin(), wins.end());
  std::stable_sort(sorted.begin(), sorted.end(), [](const json& a, const json& b) { return intOf(js::get(a, "index")) < intOf(js::get(b, "index")); });
  Own o;
  o.tilesJson = own;
  o.tilesJson.erase("layout"); o.tilesJson.erase("bounds");
  o.tilesJson["groups"] = js::get(shared, "groups").is_array() ? js::get(shared, "groups") : json::array();
  o.tilesJson["windows"] = json::array();
  for (size_t i = 0; i < sorted.size(); i++) {
    json b = (ownWins.is_array() && i < ownWins.size() && js::truthy(js::get(ownWins[i], "bounds"))) ? js::get(ownWins[i], "bounds") : json();
    const json& l = js::get(sorted[i], "layout");
    o.tilesJson["windows"].push_back({ { "bounds", b }, { "layout", js::truthy(l) ? l : json::object() } });
  }
  o.tilesJson["tiles"] = js::get(shared, "tiles").is_array() ? js::get(shared, "tiles") : json::array();
  o.held = { { "items", js::get(shared, "heldPrompts").is_array() ? js::get(shared, "heldPrompts") : json::array() } };
  o.triggers = { { "triggers", js::get(shared, "triggers").is_object() ? js::get(shared, "triggers") : json::object() } };
  o.settings = js::get(shared, "settings").is_object() ? mergeSettings(ownSettings, js::get(shared, "settings")) : ownSettings;
  return o;
}
json parseShared(const json& s) {
  if (!s.is_object() || !js::get(s, "v").is_number() || js::get(s, "v").get<double>() != V || !js::get(s, "tiles").is_array() || !knownDeck(js::get(s, "writtenBy"))) return json();
  return s;
}
std::string contentKey(const json& s) {
  return json::array({ js::get(s, "tiles"), js::get(s, "windows"), js::get(s, "groups"), js::get(s, "heldPrompts"), js::get(s, "triggers"), js::get(s, "settings"), js::get(s, "realChange"), js::get(s, "lastChange") }).dump();
}

// ---------------------------------------------------------------- the sync record and the decision
json emptySync() { return { { "v", V }, { "local", { { "n", 0 }, { "last", nullptr } } }, { "seen", nullptr } }; }
json readSync(Files& files, const std::string& stateDir) {
  const json j = readJson(files, joinPath(stateDir, SYNC));
  if (!j.is_object() || !js::get(j, "v").is_number() || js::get(j, "v").get<double>() != V) return emptySync();
  json s = emptySync();
  s["local"]["n"] = intOf(js::get(js::get(j, "local"), "n"));
  if (js::truthy(js::get(js::get(j, "local"), "last"))) s["local"]["last"] = js::get(js::get(j, "local"), "last");
  if (js::get(j, "seen").is_object()) s["seen"] = js::get(j, "seen");
  return s;
}
bool writeSync(Files& files, const std::string& stateDir, const json& sync) { return files.writeAtomic(joinPath(stateDir, SYNC), sync.dump(2)); }
json seenOf(const json& shared, int localN) {
  return { { "realChange", intOf(js::get(shared, "realChange")) }, { "writtenAt", js::get(shared, "writtenAt") }, { "writtenBy", js::get(shared, "writtenBy") }, { "local", localN } };
}
bool sharedChanged(const json& shared, const json& seen) {
  if (!shared.is_object()) return false;
  if (!seen.is_object()) return true;
  return js::get(shared, "writtenAt") != js::get(seen, "writtenAt") || intOf(js::get(shared, "realChange")) != intOf(js::get(seen, "realChange")) || js::get(shared, "writtenBy") != js::get(seen, "writtenBy");
}
json decide(const json& shared, const json& syncIn, bool ownHasState, double savedAtMs) {
  const json sync = syncIn.is_object() ? syncIn : emptySync();
  const json& seen = js::get(sync, "seen");
  const json& local = js::get(sync, "local");
  json mine = js::truthy(js::get(local, "last")) ? js::get(local, "last") : (savedAtMs > 0 ? json{ { "what", "its own files" }, { "at", isoTime(savedAtMs) } } : json());
  json theirs;
  if (shared.is_object()) theirs = js::truthy(js::get(shared, "lastChange")) ? js::get(shared, "lastChange") : json{ { "what", "its layout" }, { "at", js::get(shared, "writtenAt") }, { "by", js::get(shared, "writtenBy") } };
  const bool ownUnsynced = seen.is_object() ? intOf(js::get(local, "n")) > intOf(js::get(seen, "local")) : ownHasState;
  auto out = [&](const char* action, const std::string& why) { return json{ { "action", action }, { "why", why }, { "mine", mine }, { "theirs", theirs } }; };
  if (!shared.is_object()) return out("write", "no shared layout yet: this deck writes the first");
  if (!sharedChanged(shared, seen)) return ownUnsynced ? out("write", "only this deck changed since it last wrote the shared layout") : out("none", "the shared layout is what this deck last saw");
  if (!ownUnsynced && (!seen.is_object() || intOf(js::get(shared, "realChange")) >= intOf(js::get(seen, "realChange"))))
    return out("import", "the shared layout is newer (written by the " + js::str(js::get(shared, "writtenBy")) + " deck at " + js::str(js::get(shared, "writtenAt")) + ")");
  const std::string why = !seen.is_object() ? "this deck has tiles of its own and has never read the shared layout"
    : !ownUnsynced ? "the shared layout went back (an older copy than this deck last saw)"
    : "both decks changed since this one last saw the shared layout";
  return out("ask", why);
}

json preflight(Files& files, const std::string& stateDir, const std::string& sharedDir, double /*nowMs*/) {
  const json shared = parseShared(readJson(files, joinPath(sharedDir, SHARED)));
  const json sync = readSync(files, stateDir);
  const json tj = readJson(files, joinPath(stateDir, "tiles.json"));
  const json held = readJson(files, joinPath(stateDir, "held-prompts.json"));
  const json trig = readJson(files, joinPath(stateDir, "triggers.json"));
  const bool hasState = (js::get(tj, "tiles").is_array() && !js::get(tj, "tiles").empty()) || (js::get(held, "items").is_array() && !js::get(held, "items").empty())
    || (js::get(trig, "triggers").is_object() && !js::get(trig, "triggers").empty());
  const FileStat st = files.stat(joinPath(stateDir, "tiles.json"));
  return { { "shared", shared }, { "decision", decide(shared, sync, hasState, st.exists ? st.mtimeMs : 0) } };
}
std::string keepOwn(Files& files, const std::string& stateDir, const std::string& sharedDir, const json& shared, double atMs) {
  const std::string dir = backup(files, stateDir, {}, "kept-own", atMs, joinPath(sharedDir, SHARED));
  json sync = readSync(files, stateDir);
  if (shared.is_object()) sync["seen"] = seenOf(shared, intOf(sync["local"]["n"]));
  writeSync(files, stateDir, sync);
  return dir;
}

// ---------------------------------------------------------------- backups, the import, the writer
std::string backup(Files& files, const std::string& stateDir, const std::vector<std::string>& names, const std::string& why, double atMs, const std::string& sharedFile) {
  const std::string dir = joinPath(joinPath(stateDir, BACKUPS), isoStamp(atMs) + "-" + why);
  files.makeDirs(dir);
  for (const std::string& n : names) { const std::string f = joinPath(stateDir, n); if (files.isFile(f)) files.copy(f, joinPath(dir, n)); }
  if (!sharedFile.empty() && files.isFile(sharedFile)) files.copy(sharedFile, joinPath(dir, SHARED));
  return dir;
}
std::string importShared(Files& files, const std::string& stateDir, const json& shared, double atMs, const std::string& why) {
  const std::string dir = backup(files, stateDir, OWN_FILES, why, atMs, "");
  const json own = readJson(files, joinPath(stateDir, "tiles.json"));
  const Own o = fromShared(shared, own, readJson(files, joinPath(stateDir, "settings.json")));
  files.writeAtomic(joinPath(stateDir, "tiles.json"), o.tilesJson.dump(2));
  if (o.settings.is_object() && js::get(shared, "settings").is_object()) files.writeAtomic(joinPath(stateDir, "settings.json"), o.settings.dump(2));
  files.writeAtomic(joinPath(stateDir, "held-prompts.json"), o.held.dump(2));
  files.writeAtomic(joinPath(stateDir, "triggers.json"), o.triggers.dump(2));
  json sync = readSync(files, stateDir);
  sync["seen"] = seenOf(shared, intOf(sync["local"]["n"]));
  writeSync(files, stateDir, sync);
  return dir;
}

Sharer::Sharer(Files& files, std::string deck, std::string stateDir, std::string dir, std::function<json()> read, std::function<double()> now, std::function<void(const std::string&)> log)
  : files_(files), deck_(std::move(deck)), stateDir_(std::move(stateDir)), dir_(std::move(dir)), read_(std::move(read)), now_(std::move(now)), log_(std::move(log)) {
  file_ = joinPath(dir_, SHARED);
  sync_ = readSync(files_, stateDir_);
}
void Sharer::noteReal(const std::string& what) { pending_ = { { "what", what.empty() ? std::string("a change") : what }, { "at", isoTime(now_()) }, { "by", deck_ } }; }
bool Sharer::publish(bool force) {
  const double at = now_();
  const json cur = parseShared(readJson(files_, file_));
  if (cur.is_object() && sharedChanged(cur, sync_["seen"]) && !force) {
    if (!pending_.is_null()) { sync_["local"]["n"] = intOf(sync_["local"]["n"]) + 1; sync_["local"]["last"] = pending_; pending_ = json(); writeSync(files_, stateDir_, sync_); }
    if (!behind_) { behind_ = true; if (log_) log_("[onedeck] the shared layout was written by the " + js::str(js::get(cur, "writtenBy")) + " deck at " + js::str(js::get(cur, "writtenAt")) + ", after this deck last saw it: not overwriting it (asked at the next start)"); }
    return false;
  }
  const json real = pending_;
  const int base = cur.is_object() ? intOf(js::get(cur, "realChange")) : (sync_["seen"].is_object() ? intOf(js::get(sync_["seen"], "realChange")) : 0);
  const json r = read_ ? read_() : json::object();
  const json last = !real.is_null() ? real : (cur.is_object() && js::truthy(js::get(cur, "lastChange")) ? js::get(cur, "lastChange") : js::get(sync_["local"], "last"));
  const json next = toShared(js::get(r, "tilesJson"), js::get(r, "held"), js::get(r, "triggers"), js::get(r, "settings"), deck_, at, base + (real.is_null() ? 0 : 1), last);
  const std::string key = contentKey(next);
  if (real.is_null() && !force && cur.is_object() && key == lastKey_) return false;
  if (real.is_null() && !force && cur.is_object() && key == contentKey(cur)) { lastKey_ = key; return false; }
  if (!files_.makeDirs(dir_) && !files_.isDir(dir_)) return false;
  if (!files_.writeAtomic(file_, next.dump(2))) return false;
  lastKey_ = key; pending_ = json(); behind_ = false;
  if (!real.is_null()) { sync_["local"]["n"] = intOf(sync_["local"]["n"]) + 1; sync_["local"]["last"] = real; }
  sync_["seen"] = seenOf(next, intOf(sync_["local"]["n"]));
  writeSync(files_, stateDir_, sync_);
  return true;
}

}}}  // namespace deck::core::onedeck
