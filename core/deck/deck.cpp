// core/deck/deck.cpp — see deck.hpp. Each function names the main.js / lib/phone.js function it ports; the comments
// there say why a rule exists, so they are not repeated here.
#include "deck_impl.hpp"
#include "onedeck.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace deck { namespace core {

// main.js TERMINAL_REPLY: input a terminal makes on its own (focus, cursor / device reports, OSC answers, mouse)
bool terminalReply(const std::string& s) {
  if (s.empty()) return false;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '\x1b' || i + 1 >= s.size()) return false;
    if (s[i + 1] == ']') {                                   // ESC ] … (BEL | ESC \)
      size_t j = i + 2;
      while (j < s.size() && s[j] != '\x07' && s[j] != '\x1b') j++;
      if (j < s.size() && s[j] == '\x07') { i = j + 1; continue; }
      if (j + 1 < s.size() && s[j] == '\x1b' && s[j + 1] == '\\') { i = j + 2; continue; }
      return false;
    }
    if (s[i + 1] != '[' || i + 2 >= s.size()) return false;
    const char c = s[i + 2];
    if (c == 'I' || c == 'O') { i += 3; continue; }
    if (c == '<') {                                          // ESC [ < digits/; M|m
      size_t j = i + 3; while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) j++;
      if (j > i + 3 && j < s.size() && (s[j] == 'M' || s[j] == 'm')) { i = j + 1; continue; }
      return false;
    }
    if (c == 'M') {                                          // ESC [ M + three characters
      size_t j = i + 3; int n = 0;
      while (n < 3 && j < s.size()) { js::nextCodePoint(s, j); n++; }
      if (n == 3) { i = j; continue; }
      return false;
    }
    { size_t j = i + 2;                                      // ESC [ [?>]? [digits;]* (R|c|n)
      if (j < s.size() && (s[j] == '?' || s[j] == '>')) j++;
      while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) j++;
      if (j < s.size() && (s[j] == 'R' || s[j] == 'c' || s[j] == 'n')) { i = j + 1; continue; }
      return false; }
  }
  return true;
}

using D = Deck::Impl;

// ---------------------------------------------------------------- small things
layout::Grid D::grid() const {
  layout::Grid g; const json& v = js::get(settings, "grid");
  if (js::get(v, "rows").is_number()) g.rows = js::get(v, "rows").get<int>();
  if (js::get(v, "cols").is_number()) g.cols = js::get(v, "cols").get<int>();
  return g;
}
json D::gridJson() const { return js::truthy(js::get(settings, "grid")) ? js::get(settings, "grid") : json{ { "rows", 2 }, { "cols", 2 } }; }
std::string D::driver() const { return js::truthy(js::get(settings, "driver")) ? js::toString(js::get(settings, "driver")) : "auto"; }

int D::setTimer(double ms, std::function<void()> fn, double every) {
  timers.push_back({ nextTimer, now() + ms, every, std::move(fn) });
  return nextTimer++;
}
void D::clearTimer(int id) { timers.erase(std::remove_if(timers.begin(), timers.end(), [&](const Timer& t) { return t.id == id; }), timers.end()); }
void D::runTimers() {
  for (int guard = 0; guard < 1000; guard++) {
    const double at = now();
    int pick = -1; double best = 0;
    for (size_t i = 0; i < timers.size(); i++) if (timers[i].due <= at && (pick < 0 || timers[i].due < best || (timers[i].due == best && timers[i].id < timers[(size_t)pick].id))) { pick = (int)i; best = timers[i].due; }
    if (pick < 0) return;
    Timer t = timers[(size_t)pick];
    if (t.every > 0) timers[(size_t)pick].due = at + t.every; else timers.erase(timers.begin() + pick);
    t.fn();
    if (down) return;
  }
}

// ---------------------------------------------------------------- entries (main.js wins / winEntries / allEntries)
std::vector<Entry*> D::winEntries() { std::vector<Entry*> v; for (auto& e : entries) if (!e->remote) v.push_back(e.get()); return v; }
std::vector<Entry*> D::allEntries() { std::vector<Entry*> v = winEntries(); for (auto& e : entries) if (e->remote) v.push_back(e.get()); return v; }
static bool holds(const layout::LayoutMap& l, const std::string& id) { for (const auto& kv : l) if (kv.first == id) return true; return false; }
std::vector<remoterule::Entry> D::ruleEntries(const std::vector<Entry*>& list) const {
  std::vector<remoterule::Entry> out;
  for (const Entry* e : list) {
    remoterule::Entry r; r.index = e->index; r.remote = e->remote; r.shown = e->shown; r.hidden = e->hidden; r.visible = winVisible(*e);
    for (const auto& kv : e->layout) r.layout.push_back(kv.first);
    out.push_back(std::move(r));
  }
  return out;
}
// Which window's layout holds this tile (R5's mark, the toast's window to raise). Under the remote-desk rule: one of the
// deck's OWN windows, never a remote desk (remoterule::holder); with remoteDeskIsAWindow, any entry.
Entry* D::windowHolding(const std::string& id) {
  if (opt.remoteDeskIsAWindow) { for (Entry* e : allEntries()) if (holds(e->layout, id)) return e; return nullptr; }
  const std::vector<Entry*> own = winEntries();
  const int at = remoterule::holder(id, ruleEntries(own));
  return at < 0 ? nullptr : own[(size_t)at];
}
void D::sendToEntry(Entry* e, const std::string& name, const json& payload) { if (e) send(e->client, { { "t", "event" }, { "name", name }, { "payload", payload } }); }
void D::deskEvent(const std::string& name, const json& payload) { for (auto& c : clients) if (c->remote && c->desk) send(c->id, { { "t", "event" }, { "name", name }, { "payload", payload } }); }
void D::toWindows(const json& frame) { for (auto& c : clients) if (!c->remote) send(c->id, frame); }
void D::toAll(const json& frame) { for (auto& c : clients) send(c->id, frame); }
json D::layoutJson(const layout::LayoutMap& l) const { json o = json::object(); for (const auto& kv : l) o[kv.first] = kv.second; return o; }
json D::windowsState() {
  std::vector<Entry*> w = winEntries();
  std::stable_sort(w.begin(), w.end(), [](Entry* a, Entry* b) { return a->index < b->index; });
  json out = json::array();
  for (Entry* e : w) out.push_back({ { "bounds", e->bounds }, { "layout", layoutJson(e->layout) } });
  // a deck with no window open (a headless server run) keeps what the file had, as the Electron deck's last
  // window closing leaves the file as it was
  if (w.empty()) return savedWindows;
  return out;
}

// ---------------------------------------------------------------- persistence (main.js saveState / loadSettings)
void D::saveState(const json& extra) {
  if (restoring(now())) return;
  json data = tilesStore->read();
  if (!data.is_object()) data = json::object();
  for (auto it = extra.begin(); it != extra.end(); ++it) data[it.key()] = it.value();
  data["groups"] = groups;
  json list = json::array();
  for (const auto& tp : tiles) {
    const Tile& t = *tp;
    const bool claude = t.s.kind == "claude" && t.s.sessionId && !t.s.sessionId->empty();
    const bool cloudTile = t.s.kind == "cloud" && t.s.cloudId && !t.s.cloudId->empty();
    if (!claude && !cloudTile && !(opt.persistShells && t.s.kind == "shell")) continue;
    json o = { { "id", t.s.id }, { "kind", t.s.kind }, { "account", optJ(t.s.account) }, { "sessionId", optJ(t.s.sessionId) }, { "cwd", t.s.cwd },
      { "pinned", t.s.pinned }, { "group", optJ(t.s.group) }, { "createdAt", numJ(t.createdAt) }, { "title", optJ(t.s.knownTitle) }, { "userTitle", optJ(t.s.userTitle) },
      { "asleep", t.s.asleep }, { "sleptAt", t.s.sleptAt != 0 ? numJ(t.s.sleptAt) : json() }, { "browser", t.s.browser ? json(*t.s.browser) : json() } };
    if (t.s.desk && !t.s.desk->empty()) o["desk"] = *t.s.desk;
    { const json p = pauses.saved(t.s.id); if (!p.is_null()) o["pause"] = p; }   // Pause / Pause until restart (pause.hpp)
    { const json a = auditors.saved(t.s.id); if (a.is_object()) for (auto it = a.begin(); it != a.end(); ++it) o[it.key()] = it.value(); }   // { auditor } | { child } (auditors.hpp)
    if (t.s.kind == "cloud") o["cloudId"] = optJ(t.s.cloudId);
    list.push_back(o);
  }
  data["tiles"] = list;
  tilesStore->write(data);
  publishShared();                            // the shared layout follows this deck's tiles.json (onedeck.hpp)
}
void D::saveSettings() { sm.files->writeAtomic(settingsFile(), settings.dump(2, ' ', false, json::error_handler_t::replace)); publishShared(); }

void D::loadSettings() {
  json s = json::object();
  std::string text;
  const bool exists = sm.files->isFile(settingsFile());
  if (exists) {
    bool ok = sm.files->read(settingsFile(), text);
    if (ok) { if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3); json v = json::parse(text, nullptr, false); ok = !v.is_discarded(); if (ok) s = v; }
    if (!ok) sm.files->copy(settingsFile(), settingsFile() + ".unreadable-" + isoStamp(now()));     // keep what could not be read
  }
  settings = mergeSettings(defaults, s);
  // The driver's words. With the claim: 'desk' (the stored word before it) is 'office', anything else that is no
  // value is the default — in memory; the file takes the new word at the next save of any setting. Without it:
  // a file a deck WITH the claim wrote ('office', 'remote') is read back into this tree's three words, so this
  // deck never says a word its clients do not know.
  { const std::string m = driverrule::mode(js::get(settings, "driver"));
    if (opt.driverClaim) settings["driver"] = m.empty() ? js::get(defaults, "driver") : json(m);
    else settings["driver"] = m == "office" ? json("desk") : (m == "auto" || m == "phone") ? json(m) : js::get(defaults, "driver"); }
  if (!js::truthy(js::get(js::get(settings, "phone"), "token"))) { settings["phone"]["token"] = sm.random->hex(8); saveSettings(); }
  if (!sm.files->isFile(settingsFile())) saveSettings();
}

// ---------------------------------------------------------------- accounts (lib/accounts.js + main.js refreshAccounts)
void D::loadAccounts() {
  std::string text;
  sm.files->read(aliasFile, text);
  accounts = parseAccounts(text, home, sm.env->os() == "win32");
  syncCloudAccounts();                        // main.js refreshAccounts: `if (cloud) cloud.accounts = accounts`
}
void D::refreshAccounts() {
  for (Account& a : accounts) {
    a.email = nullptr;
    std::string text;
    if (sm.files->read(joinPath(a.dir, ".claude.json"), text)) {
      const json j = json::parse(text, nullptr, false);
      const json& e = js::get(js::get(j, "oauthAccount"), "emailAddress");
      if (js::truthy(e)) a.email = e;
    }
    a.contextWindow = 200000;
    if (sm.files->read(joinPath(a.dir, "settings.json"), text)) {
      const json j = json::parse(text, nullptr, false);
      const json& m = js::get(j, "model");
      if (m.is_string() && lowerAscii(m.get<std::string>()).find("[1m]") != std::string::npos) a.contextWindow = 1000000;
    }
    a.key = a.email.is_string() ? a.email.get<std::string>() : a.id;
    json ab = js::get(js::get(settings, "aliases"), a.key.c_str());
    if (!js::truthy(ab)) ab = js::get(opt.labelFallback, a.key.c_str());   // none of its own: the labels the shell found (the Electron deck's)
    a.abbr = js::truthy(ab) ? ab : json();
    a.label = js::truthy(a.abbr) ? js::toString(a.abbr) : (a.email.is_string() ? a.email.get<std::string>() : a.id);
  }
}

// ---------------------------------------------------------------- transcripts + live status (the file reads)
std::string D::findTranscript(const std::string& configDir, const std::string& sessionId) const {
  if (configDir.empty() || sessionId.empty()) return std::string();
  const std::string projects = joinPath(configDir, "projects");
  for (const std::string& slug : sm.files->list(projects)) {
    const std::string f = joinPath(joinPath(projects, slug), sessionId + ".jsonl");
    if (sm.files->isFile(f)) return f;
  }
  return std::string();
}
void D::pollTail(Tile& t, double at) {      // TranscriptTail.poll()
  const FileStat st = sm.files->stat(t.tailFile);
  if (!st.exists || st.isDir) return;
  const uint64_t size = st.size;
  if (size < t.tailOffset) { t.tailOffset = 0; t.tail->restart(); }
  if (size == t.tailOffset) return;
  std::string bytes; uint64_t sizeNow = 0;
  if (!sm.files->readFrom(t.tailFile, t.tailOffset, bytes, &sizeNow)) return;
  if (bytes.size() > size - t.tailOffset) bytes.resize((size_t)(size - t.tailOffset));
  t.tailOffset += bytes.size();
  t.tail->feed(bytes.data(), bytes.size(), at);
}
session::LiveMap D::readLive() const {
  session::LiveMap map;
  for (const Account& a : accounts) {
    const std::string dir = joinPath(a.dir, "sessions");
    for (const std::string& n : sm.files->list(dir)) {
      if (n.size() < 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
      std::string text;
      if (!sm.files->read(joinPath(dir, n), text)) continue;
      ProcessList* procs = sm.procs;
      map.add(a.id, text, [procs](double pid) { return pid > 0 && procs->alive(pid); });
    }
  }
  return map;
}
bool D::isResumable(Tile& t, double at) {
  if (!t.s.sessionId || t.s.sessionId->empty()) return false;
  if (t.resumable) return true;
  if (at - t.resumableAt < 10000) return false;
  t.resumableAt = at;
  const Account* a = acctById(t.s.account);
  t.resumable = a && !findTranscript(a->dir, *t.s.sessionId).empty();
  return t.resumable;
}

// ---------------------------------------------------------------- who is watching (lib/phone.js watching / phoneWatching)
bool D::watching(const std::string& id) const {
  for (const auto& c : clients) if (c->remote && ((c->hasAttached && c->attached == id) || (c->desk && !c->hidden && c->shownSet.count(id)))) return true;
  return false;
}
bool D::phoneWatching(const std::string& id) const {
  for (const auto& c : clients) if (c->remote && !c->desk && c->hasAttached && c->attached == id) return true;
  return false;
}
bool D::remoteShows(const std::string& id) const {
  for (const auto& c : clients) if (c->remote && c->desk && !c->hidden && c->shownSet.count(id)) return true;
  return false;
}
// hibernation's "on screen" set. Under the remote-desk rule a REMOTE DESK IS NOT IN IT: what sleeps and when is decided by
// the windows on this box; a phone attached to a tile still counts ('remote'), as it always has.
std::vector<std::string> D::shownBy(const Tile& t) {
  if (!opt.remoteDeskIsAWindow) return remoterule::shownBy(t.s.id, ruleEntries(winEntries()), phoneWatching(t.s.id));
  std::vector<std::string> out;
  for (Entry* w : allEntries()) if (winVisible(*w) && std::find(w->shown.begin(), w->shown.end(), t.s.id) != w->shown.end()) out.push_back("win:" + std::to_string(w->index));
  if (watching(t.s.id)) out.push_back("remote");
  return out;
}
// what one desk is sent: its own `mirror` per tile, and (the remote-desk rule) no window of the deck's box as a holder
json D::deskMetaFor(const json& desk, const Client& c) {
  return deskMeta(desk, c.shownSet, c.hidden, [this](const std::string& id) { return phoneWatching(id); }, deskRule());
}

// ---------------------------------------------------------------- notifications (main.js maybeNotify)
bool D::anyWindowFocused() { for (Entry* w : winEntries()) if (w->focused && w->visible) return true; return false; }
void D::maybeNotify(Tile& t, const std::string& status, double turnMs) {
  if (!sm.notifier) return;
  const json& n = js::get(settings, "notify");
  const bool isCloud = t.s.kind == "cloud";
  if (t.s.kind != "claude" && !isCloud) return;
  if (anyWindowFocused()) return;
  const bool waiting = status == "waiting" && !(js::get(n, "waiting").is_boolean() && !js::get(n, "waiting").get<bool>());
  const double minTurn = js::get(n, "minTurnSec").is_number() ? js::get(n, "minTurnSec").get<double>() : 20;
  const bool done = status == "idle" && !(js::get(n, "done").is_boolean() && !js::get(n, "done").get<bool>()) && turnMs >= minTurn * 1000;
  if (!waiting && !done) return;
  const Account* acct = acctById(t.s.account);
  const session::TailFacts* f = t.tail ? &t.tail->facts() : nullptr;
  Toast toast;
  toast.title = (acct ? acct->label : (t.s.account ? *t.s.account : std::string())) + (isCloud ? " \xE2\x98\x81" : "") + " \xC2\xB7 "
    + (t.s.userTitle && !t.s.userTitle->empty() ? *t.s.userTitle : t.s.knownTitle && !t.s.knownTitle->empty() ? *t.s.knownTitle : (f && f->title && !f->title->empty()) ? *f->title : std::string("Claude"));
  if (waiting) toast.body = "waiting for you";
  else {
    toast.body = "done after " + js::numberText(js::round(turnMs / 1000)) + " s";
    // the last thing it said: a cloud tile's is its session's summary (lib/cloud.js), a Claude tile's its transcript's
    std::string last;
    if (isCloud) { const json c = (cloud && t.s.cloudId) ? cloud->session(*t.s.cloudId) : json(); if (js::truthy(js::get(c, "summary"))) last = js::toString(js::get(c, "summary")); }
    else if (f && f->lastText) last = *f->lastText;
    if (!last.empty()) toast.body += ": " + js::sliceUnits(js::collapseSpaces(last), 140);
  }
  toast.silent = js::get(n, "sound").is_boolean() && !js::get(n, "sound").get<bool>();
  toast.kind = waiting ? "waiting" : "done"; toast.tileId = t.s.id;
  sm.notifier->toast(toast);
  Entry* w = windowHolding(t.s.id);
  if (!w || w->remote) { auto ws = winEntries(); w = ws.empty() ? nullptr : ws[0]; }
  if (w) sm.notifier->flash(w->index);
}

// ---------------------------------------------------------------- the meta (main.js collectMeta / pushMeta)
// ---------------------------------------------------------------- a tile follows its session (lib/tilefollow.js + main.js followTiles)
// /clear or /resume <id> inside a tile moves the same claude.exe to another session id; its sessions/<pid>.json says
// so. The tile's process: the pty's pid for a direct spawn, else learned from its own id's record (a userswitch move,
// a shell-hosted session); then only that pid + procStart counts. A record counts when alive, in the tile's account,
// not started before the tile's process (a reused pid's old file). An id another tile holds is refused (kept the old).
bool D::followTiles(const session::LiveMap& live) {
  constexpr double SLACK = 5000;
  bool moved = false;
  std::map<std::string, Tile*> taken;
  for (auto& tp : tiles) {
    Tile& t = *tp;
    if (t.s.kind != "claude" || !t.s.sessionId || t.s.sessionId->empty() || !t.pty) continue;
    auto ok = [&](const session::LiveRecord& r) {
      return r.alive && t.s.account && r.account == *t.s.account && !(r.startedAt > 0 && t.s.startedAt > 0 && r.startedAt < t.s.startedAt - SLACK);
    };
    auto pidOf = [](const session::LiveRecord& r) { return r.pid.is_number() ? r.pid.get<double>() : -1.0; };
    const std::pair<std::string, session::LiveRecord>* hit = nullptr;
    if (!t.claudeBound) {
      if (t.direct && t.pid) { for (const auto& kv : live.all()) if (pidOf(kv.second) == (double)t.pid && ok(kv.second)) { hit = &kv; break; } }
      else for (const auto& kv : live.entries()) if (kv.first == *t.s.sessionId && ok(kv.second)) { hit = &kv; break; }
      if (!hit) continue;
      t.claudeBound = true; t.claudePid = pidOf(hit->second); t.claudeProcStart = hit->second.procStart;
    } else {
      for (const auto& kv : live.all()) {
        const session::LiveRecord& r = kv.second;
        if (pidOf(r) != t.claudePid || !ok(r)) continue;
        if (js::truthy(t.claudeProcStart) && js::truthy(r.procStart) && js::toString(r.procStart) != js::toString(t.claudeProcStart)) continue;
        if (!js::truthy(t.claudeProcStart) && js::truthy(r.procStart)) t.claudeProcStart = r.procStart;
        hit = &kv; break;
      }
      if (!hit) continue;
    }
    const std::string to = hit->first;
    if (to == *t.s.sessionId) { t.followRefused.reset(); continue; }
    Tile* holder = nullptr;
    for (auto& x : tiles) if (x.get() != &t && x->s.sessionId && *x->s.sessionId == to) { holder = x.get(); break; }
    if (!holder && taken.count(to)) holder = taken[to];
    if (holder) {                                     // two tiles never share an id: the old one is kept
      if (!t.followRefused || *t.followRefused != to) logEvent("follow", "tile " + id8(t.s.id) + ": its process moved to session " + to + ", which tile " + id8(holder->s.id) + " holds: kept " + *t.s.sessionId);
      t.followRefused = to; continue;
    }
    taken[to] = &t;
    t.followRefused.reset();
    logEvent("follow", "tile " + id8(t.s.id) + ": session " + *t.s.sessionId + " -> " + to + " (/clear or /resume in the tile)");
    t.s.sessionId = to;
    t.tail.reset(); t.tailFile.clear(); t.tailOffset = 0;
    t.s.knownTitle.reset(); t.s.lastStoppedInit = false;
    t.resumable = false; t.resumableAt = 0;
    moved = true;
  }
  if (moved) saveState();
  return moved;
}

json D::collectMeta() {
  const double at = now();
  const session::LiveMap live = readLive();
  followTiles(live);   // before anything reads the tile's session id
  std::vector<std::pair<bool, json>> metas;
  bool titled = false;                       // a cloud tile learned its session's title: save it
  for (auto& tp : tiles) {
    Tile& t = *tp;
    const Account* acct = acctById(t.s.account);
    if (t.s.kind == "claude" && t.s.sessionId && !t.s.sessionId->empty()) {
      if (!t.tail) {
        const std::string f = acct ? findTranscript(acct->dir, *t.s.sessionId) : std::string();
        if (!f.empty()) { t.tail.reset(new session::TranscriptTail()); t.tailFile = f; t.tailOffset = 0; }
      }
      if (t.tail) pollTail(t, at);
    }
    t.s.running = !!t.pty;
    if (t.pty) { t.s.ptyCols = t.pty->cols(); t.s.ptyRows = t.pty->rows(); }
    session::TileTick in;
    in.live = t.s.sessionId ? live.get(*t.s.sessionId) : nullptr;
    in.tail = t.tail.get();
    in.shownBy = shownBy(t);
    if (Entry* w = windowHolding(t.s.id)) in.inWindow = w->index;
    in.held = held ? held->forTile(t.s.id) : json::array();
    in.contextWindow = acct ? acct->contextWindow : 200000;
    in.resumable = isResumable(t, at);
    in.remoteWatching = watching(t.s.id);
    if (t.startup && !t.startup->done) {
      in.startupOpen = true; in.startupSince = t.startup->since;
      const json w = t.startup->waitingOn(at);
      if (js::truthy(w)) in.startupWaitTitle = js::toString(js::get(w, "title"));
    }
    // a cloud tile's session as lib/cloud.js last read it (null until the first read lands)
    json c;
    if (t.s.kind == "cloud" && t.s.cloudId && !t.s.cloudId->empty() && cloud) c = cloud->session(*t.s.cloudId);
    if (c.is_object()) {
      in.cloud = &c;
      const json& ct = js::get(c, "title");
      if (js::truthy(ct) && (!t.s.knownTitle || *t.s.knownTitle != js::toString(ct))) { t.s.knownTitle = js::toString(ct); titled = true; }   // kept in tiles.json for the next boot
    }
    session::TickEffects fx;
    t.meta = session::tileMeta(t.s, in, at, settings, &fx);
    if (!t.elsewhere.empty()) t.meta["elsewhere"] = t.elsewhere;   // the same-session guard: "deck" | "terminal"
    if (t.s.kind == "cloud") { const json over = cloudMeta(t, c); for (auto it = over.begin(); it != over.end(); ++it) t.meta[it.key()] = it.value(); }
    if (opt.driverClaim) {
      // set while another KIND of screen drives this session's size: the size the pty is at — or is about to
      // be, mid-debounce — for the office's panes to render at; null: the office drives. And which kind that is.
      const json size = t.pty ? (t.hasPending ? json{ { "cols", t.pendCols }, { "rows", t.pendRows } } : json{ { "cols", t.pty->cols() }, { "rows", t.pty->rows() } }) : json();
      t.meta["mirror"] = driverrule::officeMirror(js::get(settings, "driver"), whoLooks(t.s.id), size);
      t.meta["sizeBy"] = sizeDriver(t.s.id);
    }
    if (fx.finishStartup && t.startup) t.startup->finish();
    if (fx.notify) maybeNotify(t, fx.notifyStatus, fx.turnMs);
    t.meta["pause"] = pauses.view(t.s.id);       // Pause / Resume: { state, untilRestart, want } or null
    { const json a = auditors.view(t.s.id);     // auditor: { string, children } | child: { parent, n, auditorName } (auditors.hpp)
      t.meta["auditor"] = a.is_object() && a.contains("auditor") ? a["auditor"] : json(); t.meta["child"] = a.is_object() && a.contains("child") ? a["child"] : json();
      t.meta["auditorMail"] = deliveries.forTile(t.s.id); }
    // who answers the session's terminal queries, exactly one (14b, remoterule::answerer): this deck's window that
    // shows it, else the remote desk, else the phone. Its windows have terminals only for what they show.
    t.meta["answerer"] = remoterule::answerer(remoterule::officeShows(t.s.id, ruleEntries(winEntries())), remoteShows(t.s.id), phoneWatching(t.s.id));
    metas.emplace_back(t.s.pinned, t.meta);
  }
  if (titled) saveState();
  session::DeckInputs in;
  in.now = at; in.restoring = restoring(at);
  in.accounts = publicAccounts(accounts);
  in.settings = settings;
  in.groups = groups;
  in.usage = usage && usageLive() ? usage->data() : json::object();
  in.triggers = triggersList();
  in.restart = restartView(at);
  // labels and viewer ids only: a remote never sees a configured page's path (lib/viewer.js)
  in.pagesShown = pagesShown();
  // every session's ask pages waiting on the user, and the last pages they answered (lib/asks.js)
  if (asks) { in.asks = asks->view(); in.askLog = asks->logView(); }
  json meta = session::deckMeta(in, metas);
  // who serves the ask pages: 'off' (no feed), 'writer' (this deck writes deck.json), 'guest' (another deck's
  // heartbeat is fresh: this one shows the asks read-only and writes nothing)
  meta["askFeed"] = !asks ? json{ { "state", "off" }, { "reason", nullptr } } : asks->guest() ? json{ { "state", "guest" }, { "reason", kAskGuestReason } } : json{ { "state", "writer" }, { "reason", nullptr } };
  // main.js says true always: it makes cloud sessions. Here only a deck that MAY make one says so, so a view
  // offers "new cloud session" exactly when the call would not be refused.
  meta["cloudOption"] = opt.allowClaude && opt.allowCloudCreate;
  // where the user was last active, as the hosted sessions read it from deck-state.json
  if (presence) { const nlohmann::ordered_json s = presence->state(); meta["presence"] = { { "where", json(s["where"]) }, { "since", json(s["since"]) }, { "lastInputFrom", json(s["lastInputFrom"]) } }; }
  else meta["presence"] = nullptr;
  // the connected remote desk, for the driver button's words on every screen — with the claim only (deck.hpp)
  if (opt.driverClaim) meta["remoteDesk"] = remoteDesk();
  // the PUBLIC build only (core/deck/edition.hpp): the pages it serves (the Electron deck's renderer) name it from here;
  // the owner's decks send no such key, so their pages are as before
  if (edition::kPublic) meta["edition"] = { { "name", edition::kProductName } };
  // ... and its pages show no "move every session" (2026-11-24 ask r4 Q6; the core refuses moveAll there anyway)
  if (edition::kPublic) meta["edition"]["moveAll"] = false;
  return meta;
}
json D::extras() {
  if (sm.phone) return sm.phone->extras();
  return { { "assets", "" }, { "build", nullptr }, { "pageKey", "" } };
}
// lib/phone.js sendMeta: a remote client is sent a meta when it differs from what that client last got, or
// META_KEEPALIVE_MS after it; a frame the sink refused (a busy socket) is not remembered, so the next tick heals it.
void D::sendRemoteMeta(const json& meta) {
  bool any = false; for (const auto& c : clients) if (c->remote) any = true;
  if (!any) return;
  const json ex = extras();
  const double at = now();
  bool hasPhone = false, hasDesk = false; json phone, desk; std::string phoneKey;
  // (index, not iterators: a refused send may drop a client from inside the sink)
  for (size_t i = 0; i < clients.size(); i++) {
    Client* c = clients[i].get();
    if (!c->remote) continue;
    json m; std::string key;
    if (c->desk) {
      if (!hasDesk) { desk = remoteMeta(meta, true); hasDesk = true; }
      m = deskMetaFor(desk, *c);
      key = metaKey(m, js::get(ex, "assets"), js::get(ex, "build"));
    } else {
      if (!hasPhone) { phone = remoteMeta(meta, false); phoneKey = metaKey(phone, js::get(ex, "assets"), js::get(ex, "build")); hasPhone = true; }
      m = phone; key = phoneKey;
    }
    if (c->hasMetaKey && key == c->metaKey && at - c->metaAt < kMetaKeepaliveMs) continue;
    json frame = { { "t", "meta" }, { "meta", m } };
    for (auto it = ex.begin(); it != ex.end(); ++it) frame[it.key()] = it.value();
    const ClientId id = c->id;
    if (send(id, frame)) { if (Client* again = clientById(id)) { again->hasMetaKey = true; again->metaKey = key; again->metaAt = at; } }
  }
}
void D::pushMeta() {
  if (!started || down) return;
  presenceTick();
  const json m = collectMeta();
  toWindows({ { "t", "meta" }, { "meta", m }, { "build", nullptr }, { "assets", "" } });
  sendRemoteMeta(m);
}
void D::pushMetaSoon() { if (metaSoonTimer) clearTimer(metaSoonTimer); metaSoonTimer = setTimer(120, [this] { metaSoonTimer = 0; pushMeta(); }); }

json D::publicPlatform() const {
  const std::string os = sm.env->os();
  // what the shell said, else lib/platform.js's answer for this OS (the Windows table on win32, the Cmd map on a Mac)
  json keyHelp = !opt.keyHelp.is_null() ? opt.keyHelp : platform::keyHelp(os);
  return { { "os", os }, { "isMac", os == "darwin" }, { "keyHelp", keyHelp },
    { "fontStack", !opt.fontStack.empty() ? opt.fontStack : platform::defaultFontStack(os) }, { "shellName", opt.shellName } };
}
json D::phoneStatus() {
  const json& p = js::get(settings, "phone");
  if (sm.phone) return sm.phone->status(p);
  return { { "enabled", js::truthy(js::get(p, "enabled")) }, { "running", false }, { "port", js::get(p, "port") }, { "bind", js::get(p, "bind") }, { "token", js::get(p, "token") },
    { "address", nullptr }, { "serve", nullptr }, { "url", nullptr }, { "clients", 0 }, { "desks", 0 }, { "tailscale", nullptr }, { "error", nullptr } };
}

// ---------------------------------------------------------------- spawning (main.js queueSpawn / spawnTile)
void D::queueSpawn(std::function<void()> fn) {
  spawnQueue.push_back(std::move(fn));
  if (!spawnTimerOn) { spawnTimerOn = true; setTimer(0, [this] { drainSpawns(); }); }
}
void D::drainSpawns() {
  if (spawnQueue.empty()) { spawnTimerOn = false; return; }
  auto fn = std::move(spawnQueue.front()); spawnQueue.pop_front();
  try { fn(); } catch (...) { /* a queued spawn failed: the next one still runs */ }
  setTimer(SPAWN_GAP_MS, [this] { drainSpawns(); });
}

static void envPut(EnvList& env, const std::string& k, const std::string& v) {
  for (auto& kv : env) if (kv.first == k) { kv.second = v; return; }
  env.emplace_back(k, v);
}

// Returns the tile's id. Throws for an unknown account, as main.js does.
std::string D::spawnTile(const SpawnSpec& spec, Tile* existing) {
  const bool withAcct = spec.kind == "claude" || spec.kind == "cloud";
  const Account* acct = withAcct ? acctById(spec.account) : nullptr;
  if (withAcct && !acct) throw std::runtime_error("unknown account: " + (spec.account ? *spec.account : std::string("undefined")));
  const double at = now();
  std::unique_ptr<Tile> fresh;
  Tile* tile = existing;
  if (!tile) { fresh.reset(new Tile()); tile = fresh.get(); tile->s.id = sm.random->uuid(); tile->createdAt = at; }
  TileState& s = tile->s;
  const std::optional<bool> hadBrowser = s.browser;
  const std::optional<std::string> hadDesk = s.desk;
  s.kind = spec.kind;
  s.account = acct ? std::optional<std::string>(acct->id) : std::nullopt;
  s.sessionId = spec.sessionId && !spec.sessionId->empty() ? spec.sessionId : std::nullopt;
  const std::string settingsCwd = js::truthy(js::get(settings, "cwd")) ? js::toString(js::get(settings, "cwd")) : std::string();
  s.cwd = spec.cwd && !spec.cwd->empty() ? *spec.cwd : (!settingsCwd.empty() ? settingsCwd : home);
  s.exited = false; s.exitCode.reset(); s.oscTitle.reset();
  tile->tail.reset(); tile->tailFile.clear(); tile->tailOffset = 0;
  tile->replay.reset();
  const std::optional<std::string> cloudId = spec.kind == "cloud" && spec.cloudId && !spec.cloudId->empty() ? spec.cloudId : std::nullopt;
  const bool watched = spec.kind == "claude" || (spec.kind == "cloud" && !cloudId);   // a Claude Code process starts in this pty
  tile->trust.reset(); tile->startup.reset();
  if (watched) {
    tile->trust.reset(new TrustWatch()); tile->trust->since = at;
    Files* files = sm.files;
    tile->startup.reset(new StartupWatch(at, [files](const std::string& p) { return files->stat(p).exists; }));
  }
  s.startedAt = at; s.lastStatus.reset(); s.lastStoppedInit = false;
  tile->hasLastSize = false;
  s.asleep = false; s.sleptAt = 0; s.waking = false;
  s.browser = spec.kind == "claude" ? (spec.hasBrowser ? spec.browser : hadBrowser) : std::nullopt;
  s.desk = spec.kind == "claude" ? (spec.hasDesk ? spec.desk : hadDesk) : std::nullopt;
  tile->inShell = false;
  tile->direct = false; tile->claudeBound = false; tile->claudePid = 0; tile->claudeProcStart = json(); tile->followRefused.reset();
  tile->elsewhere.clear(); tile->elsewherePid = 0;          // the same-session guard's mark: every start looks again
  s.cloudTask = spec.kind == "cloud" ? (spec.task && !spec.task->empty() ? spec.task : s.cloudTask) : std::nullopt;
  s.cloudId = cloudId;
  if (s.lastStoppedAt == 0) s.lastStoppedAt = at;

  auto adopt = [&] { if (fresh) tiles.push_back(std::move(fresh)); };
  auto failed = [&](const std::string& file, const std::string& why) {
    s.exited = true; s.exitCode = -1; s.running = false;
    tile->replay.pushUncounted("\r\n[claude-deck] failed to start " + file + ": " + why + "\r\n");
    logEvent("spawn", "tile " + id8(s.id) + " (" + s.kind + "): failed to start " + file + ": " + why);
    adopt(); saveState(); pushMeta();
    return s.id;
  };

  // A cloud tile that has its session runs nothing on this machine.
  if (spec.kind == "cloud" && cloudId) {
    tile->pty.reset(); tile->pid = 0; s.running = false; s.modes.reset();
    adopt(); saveState(); pushMeta();
    return s.id;
  }

  // THE SAME-SESSION GUARD (2026-11-24; onedeck.hpp holderOf): a session another process holds - the other Claude
  // Deck's tile, or a Claude process in a terminal - is never resumed here as well. The tile says "open elsewhere";
  // nothing is killed. In the first minute after the deck started a terminal holder is looked at again (5 times, 2 s
  // apart): after a deck restart the old deck's sessions can still be on their way out.
  if (spec.kind == "claude" && s.sessionId && !s.sessionId->empty()) {
    const onedeck::Holder h = sessionHolder(*s.sessionId);
    if (h.held) {
      const std::string where = h.where == "deck" ? std::string("in the other Claude Deck") : "in another window (pid " + js::numberText(h.pid) + ")";
      if (h.where == "terminal" && at - deckStartedAt < 60000 && tile->elsewhereTries < 5) {
        tile->elsewhereTries++;
        const std::string id = s.id; const SpawnSpec again = spec;
        setTimer(2000, [this, id, again] { queueSpawn([this, id, again] {
          Tile* t = tileById(id);
          if (!t || t->pty || t->elsewhere.empty()) return;
          try { spawnTile(again, t); } catch (...) {}
        }); });
      } else tile->elsewhereTries = 0;
      tile->elsewhere = h.where; tile->elsewherePid = h.pid;
      tile->pty.reset(); tile->pid = 0; s.running = false; s.exited = true; s.exitCode.reset(); s.modes.reset();
      tile->trust.reset(); tile->startup.reset();
      tile->replay.pushUncounted("\r\n[claude-deck] this session is open " + where + ": it is not resumed here as well.\r\n");
      logEvent("onedeck", "tile " + id8(s.id) + ": session " + *s.sessionId + " is open " + where + ": not resumed here");
      adopt(); saveState(); pushMeta();
      return s.id;
    }
  }
  PtySpawn ps;
  if (spec.kind == "shell") {
    ps.file = opt.shellFile; ps.args = opt.shellArgs;
    ps.env = baseEnv(sm.env->environment());
    // the deck's contract with what it hosts: which tile this is, and where the state file is (presence.hpp)
    for (const auto& kv : tileEnv(s.id, presenceFile())) envPut(ps.env, kv.first, kv.second);
    // ...and where the deck keeps its state, so the deck-triggers skill (and anything else) finds this deck's folder
    envPut(ps.env, "CLAUDE_DECK_USERDATA", stateDir);
  } else {
    // THE SEAM. Everything main.js's Claude branches decide is decided here…
    ClaudeLaunch launch;
    if (spec.moveFrom && !spec.moveFrom->empty()) {
      // userswitch copies the transcript into the target account and execs `claude --resume <id>` there
      const Account* from = acctById(spec.moveFrom);
      std::string us = opt.userswitch; for (char& c : us) if (c == '\\') c = '/';
      launch.file = opt.bash;
      launch.args = { us, "--start", acct->id, (from ? from->id : *spec.moveFrom) + " --resume " + (s.sessionId ? *s.sessionId : std::string("null")) };
      launch.env = envFor(sm.env->environment(), *acct);
      envPut(launch.env, "USERSWITCH_ZSHRC", aliasFile);
      if (s.browser) envPut(launch.env, "CLAUDE_CODE_ENABLE_CFC", *s.browser ? "1" : "0");
      if (s.desk && !s.desk->empty()) envPut(launch.env, "CLAUDE_ONCALL_KIND", *s.desk + "-desk");
      launch.cwd = s.cwd; launch.sessionId = s.sessionId ? *s.sessionId : std::string();
    } else if (spec.kind == "cloud") {
      // `claude --cloud "<task>"` makes the session, prints its link and exits 0 (cloudCreated reads the link off
      // the output). The run is on screen in the tile, so a refusal or a question on the way is answerable.
      launch.file = opt.claudeExe;
      launch.args = { "--cloud", s.cloudTask ? *s.cloudTask : std::string() };
      launch.env = envFor(sm.env->environment(), *acct);
      launch.cwd = s.cwd;
    } else {
      ClaudeLaunchInput in;
      in.parentEnv = sm.env->environment(); in.claudeExe = opt.claudeExe; in.account = *acct;
      in.sessionId = s.sessionId ? *s.sessionId : std::string();
      in.transcriptExists = !in.sessionId.empty() && !findTranscript(acct->dir, in.sessionId).empty();
      in.browser = s.browser; in.desk = s.desk; in.cwd = s.cwd; in.freshUuid = sm.random->uuid();
      in.model = spec.model; in.prompt = spec.prompt;
      launch = claudeLaunch(in);
      s.sessionId = launch.sessionId;
      tile->direct = spec.kind == "claude";   // the pty's pid is this claude.exe's (followTiles)
    }
    for (const auto& kv : tileEnv(s.id, presenceFile())) envPut(launch.env, kv.first, kv.second);
    envPut(launch.env, "CLAUDE_DECK_USERDATA", stateDir);
    tile->lastLaunch = launch;
    // making a session in Anthropic's cloud has its own switch, on top of allowClaude
    if (spec.kind == "cloud" && opt.allowClaude && !opt.allowCloudCreate) { s.modes.reset(); return failed(launch.file, "making cloud sessions is switched off in the native local deck (allowCloudCreate)"); }
    // …and the launch itself happens only when the deck was told it may start Claude sessions.
    if (!opt.allowClaude) { s.modes.reset(); return failed(launch.file, "starting Claude sessions is switched off in the native local deck (allowClaude)"); }
    ps.file = launch.file; ps.args = launch.args; ps.env = launch.env;
  }
  ps.cwd = sm.env->resolvePath(s.cwd);        // node-pty: cwd = path.resolve(cwd)
  ps.cols = 120; ps.rows = 30;

  s.modes.reset();                            // a new process starts with no modes
  tile->decoder.reset();
  const uint64_t gen = nextGen++;
  std::string err;
  std::unique_ptr<Pty> p = sm.ptys->spawn(ps, gen, &err);
  if (!p) return failed(ps.file, err);
  tile->pid = p->pid();
  ownPids.insert((double)tile->pid);          // this deck's own process, ever: never "elsewhere" (the same-session guard)
  tile->pty = std::move(p); tile->gen = gen; s.running = true; s.ptyCols = 120; s.ptyRows = 30;
  adopt(); saveState(); pushMeta();
  return s.id;
}

// ---------------------------------------------------------------- Claude Code's startup dialogs (spawnTile's onData)
void D::trustCycle(const std::string& id, uint64_t gen) {
  Tile* t = tileById(id);
  if (!t || !t->pty || t->gen != gen || !t->trust || t->trust->done) return;
  TrustWatch& w = *t->trust;
  if (++w.tries > 12) { w.done = true; return; }
  w.text.clear();
  t->pty->resize(t->pty->cols() > 100 ? t->pty->cols() - 1 : t->pty->cols() + 1, t->pty->rows());
  setTimer(800, [this, id, gen] {
    Tile* tile = tileById(id);
    if (!tile || !tile->pty || tile->gen != gen || !tile->trust || tile->trust->done) return;
    TrustWatch& tw = *tile->trust;
    if (!trustOptionSeen(tw.text)) { tw.done = true; return; }           // answered
    const std::string at = trustMarkerLine(tw.text);
    if (!at.empty() && tile->startup) tile->startup->keySent();          // what follows is a new paint
    if (at == "yes") { tile->pty->write("\r"); setTimer(1800, [this, id, gen] { trustCycle(id, gen); }); }
    else if (at == "no") { tile->pty->write("\x1b[B"); setTimer(800, [this, id, gen] { trustCycle(id, gen); }); }
    else setTimer(800, [this, id, gen] { trustCycle(id, gen); });        // no readable repaint yet
  });
}
void D::watchStartup(Tile& t, uint64_t gen, const std::string& d) {
  const double at = now();
  const bool autoTrust = js::truthy(js::get(settings, "autoTrust"));
  const std::string id = t.s.id;
  if (t.trust && !t.trust->done) {
    TrustWatch& w = *t.trust;
    w.text = trustAppend(w.text, d);
    if (at - w.since > 120000) w.done = true;
    else if (autoTrust && !w.answering && trustPromptSeen(w.text)) {
      w.answering = true; w.tries = 0;
      setTimer(700, [this, id, gen] { trustCycle(id, gen); });
    }
  }
  if (t.startup && !t.startup->done) {
    StartupWatch& sw = *t.startup;
    sw.feed(d, at);
    if (sw.wantsContinue(autoTrust, t.s.lastKeyAt)) {
      sw.pending = true;
      setTimer(700, [this, id, gen] {
        Tile* tile = tileById(id);
        if (!tile || !tile->startup) return;
        StartupWatch& w = *tile->startup;
        w.pending = false;
        if (!tile->pty || tile->gen != gen || !w.wantsContinue(js::truthy(js::get(settings, "autoTrust")), tile->s.lastKeyAt)) return;
        const json warning = w.prompt;                                    // the settled paint, not the first piece of it
        if (!tile->pty->write("\r")) return;
        w.answers++; w.keySent();
        // noteSettingsWarning: say what was skipped, once per distinct warning per deck run
        const std::string what = describeWarning(warning);
        if (what.empty() || settingsWarningsShown.count(what)) return;
        settingsWarningsShown.insert(what);
        if (sm.notifier) {
          Toast toast; toast.title = "Claude Code skipped part of a settings file";
          toast.body = what + ". The deck pressed Continue so the sessions start; fix the file to stop the warning.";
          toast.silent = js::get(js::get(settings, "notify"), "sound").is_boolean() && !js::get(js::get(settings, "notify"), "sound").get<bool>();
          toast.kind = "settings-warning"; toast.tileId = id;
          sm.notifier->toast(toast);
        }
      });
    }
  }
}

// ---------------------------------------------------------------- ending a process (main.js killPty)
// `then` runs once the process is gone (or the grace + 4 s has passed), as the JS awaits killPty.
void D::killPty(Tile& t, std::function<void()> then, double graceMs) {
  if (!t.pty) { if (then) then(); return; }
  const uint64_t gen = t.gen;
  const bool gentle = t.s.kind == "claude";
  dying.push_back({ std::move(t.pty), gen, false, std::move(then) });
  t.s.running = false;
  setTimer(graceMs + 4000, [this, gen] { finishKill(gen); });
  if (gentle) {
    // ask Claude Code to exit by itself first: Ctrl+C three times, spaced
    for (double delay : { 0.0, 180.0, 360.0 }) setTimer(delay, [this, gen] { for (auto& x : dying) if (x.gen == gen && !x.done && x.pty) x.pty->write("\x03"); });
    setTimer(graceMs, [this, gen] { for (auto& x : dying) if (x.gen == gen && !x.done && x.pty) x.pty->kill(); });
  } else {
    for (auto& x : dying) if (x.gen == gen && x.pty) x.pty->kill();
  }
}
void D::finishKill(uint64_t gen) {
  for (auto& x : dying) {
    if (x.gen != gen || x.done) continue;
    x.done = true;
    if (x.pty && !x.pty->exited()) x.pty->kill();            // the grace ran out: no longer waiting for it
    auto then = std::move(x.then); x.then = nullptr;
    if (then) then();
    return;
  }
}
void D::reapDying() {
  for (size_t i = 0; i < dying.size();) {
    if (dying[i].done && (!dying[i].pty || dying[i].pty->exited())) dying.erase(dying.begin() + (long long)i); else i++;
  }
}

// ---------------------------------------------------------------- tile actions
void D::closeTile(const std::string& id, std::function<void()> done) {
  if (held) held->forgetTile(id);                              // a closed session takes its held prompts with it
  pauses.forget(id);
  endAuditorTies(id);                                          // an auditor closed ends its children; a child closed leaves its auditor
  Tile* t = tileById(id);
  if (!t) { done(); return; }
  killPty(*t, [this, id, done] {
    for (size_t i = 0; i < tiles.size(); i++) if (tiles[i]->s.id == id) { if (tiles[i]->resizeTimer) clearTimer(tiles[i]->resizeTimer); tiles.erase(tiles.begin() + (long long)i); break; }
    saveState(); pushMeta();
    done();
  });
}
SpawnSpec D::respawnSpec(const Tile& t, const json& opts) const {
  SpawnSpec sp;
  sp.kind = t.s.kind == "shell" ? "shell" : "claude";
  sp.cwd = t.s.cwd;
  if (sp.kind == "claude") {
    sp.account = t.s.account; sp.sessionId = t.s.sessionId;
    // browserSpec(opts): named only as true, false or null
    if (opts.is_object() && opts.find("browser") != opts.end()) {
      const json& b = opts["browser"];
      if (b.is_boolean()) { sp.hasBrowser = true; sp.browser = b.get<bool>(); } else if (b.is_null()) { sp.hasBrowser = true; sp.browser.reset(); }
    }
  }
  return sp;
}
void D::refreshTile(const std::string& id, const json& opts, std::function<void()> done) {
  Tile* t = tileById(id);
  if (!t) { done(); return; }
  // a cloud session runs nothing here to restart: its page reloads in the view, and the status is read again at once
  if (t->s.kind == "cloud") { if (t->s.cloudId && !t->s.cloudId->empty() && cloud) cloud->soon(*t->s.cloudId); pushMeta(); done(); return; }
  killPty(*t, [this, id, opts, done] {
    Tile* tile = tileById(id);
    if (tile) {
      tile->tail.reset();
      resetTile(id);
      try { spawnTile(respawnSpec(*tile, opts), tile); } catch (...) { /* the account went away: the tile stays as it is */ }
    }
    done();
  });
}
void D::restartTile(const std::string& id) {
  Tile* t = tileById(id);
  if (!t || t->pty) return;
  if (t->s.kind == "cloud" && t->s.cloudId && !t->s.cloudId->empty()) return;      // nothing here to restart (refreshTile reads it again)
  resetTile(id);
  if (t->s.kind == "cloud") {                  // retry a run that made no session
    SpawnSpec sp; sp.kind = "cloud"; sp.account = t->s.account; sp.task = t->s.cloudTask; sp.cwd = t->s.cwd;
    spawnTile(sp, t);
    return;
  }
  spawnTile(respawnSpec(*t), t);
}
void D::sleepTile(const std::string& id, const std::string& /*why*/, std::function<void(bool)> done) {
  Tile* t = tileById(id);
  if (!t || t->s.asleep || !t->pty) { done(false); return; }
  t->s.asleep = true; t->s.sleptAt = now();
  pushMeta();
  killPty(*t, [this, id, done] {
    Tile* tile = tileById(id);
    if (!tile || !tile->s.asleep) { done(false); return; }
    saveState(); pushMeta();
    done(true);
  });
}
bool D::wakeTile(const std::string& id, const std::string& /*why*/) {
  Tile* t = tileById(id);
  if (!t || !t->s.asleep || t->pty) return false;
  t->s.asleep = false; t->s.sleptAt = 0; t->s.waking = true;
  pushMeta();
  queueSpawn([this, id] {
    Tile* tile = tileById(id);
    if (!tile || tile->pty || tile->s.asleep) return;
    tile->s.waking = false;
    resetTile(id);
    SpawnSpec sp; sp.kind = "claude"; sp.account = tile->s.account; sp.sessionId = tile->s.sessionId; sp.cwd = tile->s.cwd;
    try { spawnTile(sp, tile); }
    catch (...) { tile->s.asleep = true; tile->s.sleptAt = now(); pushMeta(); }
  });
  return true;
}
bool D::setPinned(const std::string& id, bool pinned) {
  size_t i = 0; for (; i < tiles.size(); i++) if (tiles[i]->s.id == id) break;
  if (i == tiles.size()) return false;
  std::unique_ptr<Tile> t = std::move(tiles[i]);
  tiles.erase(tiles.begin() + (long long)i);
  t->s.pinned = pinned;
  long long lastPinned = -1;
  for (size_t k = 0; k < tiles.size(); k++) if (tiles[k]->s.pinned) lastPinned = (long long)k;
  const bool nowPinned = t->s.pinned;
  tiles.insert(nowPinned ? tiles.begin() + (lastPinned + 1) : tiles.end(), std::move(t));
  saveState(); pushMeta();
  return nowPinned;
}
void D::reorderPinned(const json& ids) {
  std::vector<std::unique_ptr<Tile>> pinned, rest, ordered;
  for (auto& t : tiles) (t->s.pinned ? pinned : rest).push_back(std::move(t));
  if (ids.is_array()) for (const json& id : ids) {
    if (!id.is_string()) continue;
    for (auto& t : pinned) if (t && t->s.id == id.get<std::string>()) { ordered.push_back(std::move(t)); break; }
  }
  for (auto& t : pinned) if (t) ordered.push_back(std::move(t));
  tiles.clear();
  for (auto& t : ordered) tiles.push_back(std::move(t));
  for (auto& t : rest) tiles.push_back(std::move(t));
  saveState(); pushMeta();
}
void D::renameTile(const std::string& id, const json& title) {
  Tile* t = tileById(id); if (!t) return;
  const std::string v = js::trim(js::truthy(title) ? js::toString(title) : std::string());
  t->s.userTitle = v.empty() ? std::nullopt : std::optional<std::string>(v);
  saveState(); pushMeta();
}
void D::setTileGroup(const std::string& id, const json& group) {
  Tile* t = tileById(id); if (!t) return;
  std::optional<std::string> g;
  if (group.is_string() && !js::trim(group.get<std::string>()).empty()) g = js::sliceUnits(js::trim(group.get<std::string>()), 40);
  if (g && std::find(groups.begin(), groups.end(), *g) == groups.end()) groups.push_back(*g);
  t->s.group = g;
  saveState(); pushMeta();
}
json D::setGroups(const json& names) {
  std::vector<std::string> clean;
  if (names.is_array()) for (const json& n : names) {
    const std::string g = js::sliceUnits(js::trim(js::truthy(n) ? js::toString(n) : std::string()), 40);
    if (!g.empty() && std::find(clean.begin(), clean.end(), g) == clean.end()) clean.push_back(g);
  }
  for (auto& t : tiles) if (t->s.group && std::find(clean.begin(), clean.end(), *t->s.group) == clean.end()) t->s.group.reset();
  groups = clean;
  saveState(); pushMeta();
  return groups;
}

// ---------------------------------------------------------------- input / size (main.js writeTile, resizeTile, nudgeTile, R10)
bool D::remoteDrives(const std::string& id) const {
  const std::string mode = driver();
  return mode == "phone" || (mode == "auto" && watching(id));
}
// The claim: which KIND of screen drives this tile's size. The rule is driverrule.hpp; here is only what it needs
// to know about one session: is a phone attached to it, does a remote desk show it in a pane on a visible tab.
driverrule::Who D::whoLooks(const std::string& id) const { driverrule::Who w; w.phone = phoneWatching(id); w.remote = remoteShows(id); return w; }
std::string D::sizeDriver(const std::string& id) const { return driverrule::driverOf(js::get(settings, "driver"), whoLooks(id)); }
// R10: the office is read-only on a tile another screen drives
bool D::officeLocked(const std::string& id) const {
  return opt.driverClaim ? driverrule::inputLocked(js::get(settings, "driver"), whoLooks(id), "office") : remoteDrives(id);
}
std::string D::refusedReason(const std::string& id) const {
  if (!opt.driverClaim) return driver() == "phone" ? "phone" : "phone-attached";
  const std::string r = driverrule::refusedReason(js::get(settings, "driver"), whoLooks(id));   // 'phone' | 'phone-attached' | 'remote' | 'remote-showing'
  return r.empty() ? std::string("phone") : r;
}
// a size (a resize, a redraw's nudge) from a screen of kind `from`: does it reach the pty
bool D::sizeAccepted(const std::string& id, const char* from) const {
  if (opt.driverClaim) return driverrule::acceptsSize(js::get(settings, "driver"), whoLooks(id), from);
  const bool office = std::string(from) == "office";
  if (office && remoteDrives(id)) return false;
  if (!office && driver() == "desk") return false;
  return true;
}
// /^\x1b\[<(\d+);\d+;\d+M$/ with bit 32 of the button clear: one SGR mouse press or wheel notch, never motion
static bool mousePress(const std::string& s) {
  int b = 0, c = 0, r = 0, used = 0;
  if (s.compare(0, 3, "\x1b[<") != 0 || std::sscanf(s.c_str() + 3, "%d;%d;%dM%n", &b, &c, &r, &used) != 3 || (size_t)used + 3 != s.size()) return false;
  for (size_t i = 3; i + 1 < s.size(); i++) if (!((s[i] >= '0' && s[i] <= '9') || s[i] == ';')) return false;
  return (b & 32) == 0;
}
// /^(?:\x1b\[1;5[FH]|\x1b\[<[\d;]+[Mm])+$/: what the phone page sends by itself while it scrolls
static bool phoneScroll(const std::string& s) {
  if (s.empty()) return false;
  size_t i = 0;
  while (i < s.size()) {
    if (s.compare(i, 6, "\x1b[1;5F") == 0 || s.compare(i, 6, "\x1b[1;5H") == 0) { i += 6; continue; }
    if (s.compare(i, 3, "\x1b[<") != 0) return false;
    size_t j = i + 3; while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) j++;
    if (j == i + 3 || j >= s.size() || (s[j] != 'M' && s[j] != 'm')) return false;
    i = j + 1;
  }
  return true;
}
// `kind` ('office' | 'remote' | 'phone'): the screen it came from, for the state the hosted sessions read
void D::noteKeys(const std::string& id, const std::string& data, const char* kind) {
  const bool phone = std::string(kind) == "phone";
  if (terminalReply(data)) { if (!phone && mousePress(data)) sawInput(kind); return; }
  if (!(phone && phoneScroll(data))) sawInput(kind);
  Tile* t = tileById(id); if (!t) return;
  t->s.lastKeyAt = now();
  if (t->startup) t->startup->keySent();           // a person is answering a startup dialog: the next paint is theirs
}
void D::noteDriver(const std::string& id, const char* from, const char* kind) {
  if (Tile* t = tileById(id)) { t->s.hadInput = true; t->s.lastInputFrom = from; t->s.lastInputAt = now(); }
  sawInput(kind);
}

// ---------------------------------------------------------------- where the user is (deck-state.json; presence.hpp)
void D::startPresence() {
  json prev; std::string text;
  if (sm.files->read(presenceFile(), text)) { prev = json::parse(text, nullptr, false); if (prev.is_discarded()) prev = json(); }
  const json deckInfo = { { "pid", sm.env->pid() }, { "startedAt", now() }, { "version", opt.version.empty() ? json() : json(opt.version) } };
  // this PC's name, as os.hostname() gives it to lib/presence.js (a remote desk on this PC = the user at this PC)
  const std::string pcName = sm.env->os() == "win32" ? sm.env->get("COMPUTERNAME") : sm.env->get("HOSTNAME");
  presence.reset(new Presence(deckInfo, js::get(settings, "awayMinutes"), now(), prev, pcName));
  writePresence();
}
// written ONLY when the text differs from what was written last: never on a timer
void D::writePresence() {
  if (!presence) return;
  const std::string text = presence->text();
  if (presenceEverWritten && text == presenceWritten) return;
  if (sm.files->writeAtomic(presenceFile(), text)) { presenceEverWritten = true; presenceWritten = text; }
}
// the connected remote desk ({ name, kind }), or null
json D::remoteDesk() const {
  for (const auto& c : clients) if (c->remote && c->desk) return { { "name", c->deskName }, { "kind", c->deskKind } };
  return json();
}
// what is connected, and the one step time can take (into 'away'): on the meta tick, written only on a change
void D::presenceTick() {
  if (!presence) return;
  bool phone = false;
  for (const auto& c : clients) if (c->remote && !c->desk && !c->viaDesk) phone = true;
  const bool a = presence->connections(remoteDesk(), phone, now());
  const bool b = presence->tick(now());
  if (a || b) writePresence();
}
void D::writeTile(const std::string& id, const std::string& data) {
  Tile* t = tileById(id);
  if (!t) return;
  if (t->s.asleep) { wakeTile(id, "typed into"); return; }   // the keystroke itself is dropped
  if (t->pty) t->pty->write(data);
}
// ipcMain 'tile:input': a window's input, refused while a remote drives the tile (R10)
void D::windowInput(Client& c, const std::string& id, const std::string& data) {
  if (officeLocked(id)) {
    if (terminalReply(data)) return;
    sawInput("office");                              // refused, but a person typed it at this PC
    const double at = now();
    auto it = refusedNoteAt.find(id);
    if (it != refusedNoteAt.end() && at - it->second < 3000) return;
    refusedNoteAt[id] = at;
    send(c.id, { { "t", "event" }, { "name", "input:refused" }, { "payload", { { "id", id }, { "reason", refusedReason(id) } } } });
    return;
  }
  noteKeys(id, data);
  writeTile(id, data);
}
void D::resizeTile(const std::string& id, double cols, double rows, const char* from) {
  Tile* t = tileById(id);
  if (!t || !(cols > 1 && rows > 1)) return;
  if (!sizeAccepted(id, from)) return;
  t->hasPending = true; t->pendCols = (int)cols; t->pendRows = (int)rows;
  if (t->resizeTimer) clearTimer(t->resizeTimer);
  t->resizeTimer = setTimer(150, [this, id] {
    Tile* tile = tileById(id); if (!tile) return;
    tile->resizeTimer = 0;
    const bool had = tile->hasPending; tile->hasPending = false;
    if (!had || !tile->pty) return;
    if (tile->hasLastSize && tile->lastCols == tile->pendCols && tile->lastRows == tile->pendRows) return;
    tile->hasLastSize = true; tile->lastCols = tile->pendCols; tile->lastRows = tile->pendRows;
    tile->replay.markResize();
    tile->pty->resize(tile->pendCols, tile->pendRows);
  });
}
void D::nudgeTile(const std::string& id, double cols, double rows, const char* from) {
  Tile* t = tileById(id); if (!t || !t->pty) return;
  if (!sizeAccepted(id, from)) return;                 // a redraw from a screen that does not own the size: no kick
  if (!(cols > 2 && rows > 1)) return;
  if (t->resizeTimer) { clearTimer(t->resizeTimer); t->resizeTimer = 0; }
  t->hasPending = false;
  const int c = (int)cols, r = (int)rows;
  if (!t->pty->resize(c - 1, r)) return;
  setTimer(100, [this, id, c, r] {
    Tile* tile = tileById(id);
    if (!tile || !tile->pty) return;
    if (!tile->pty->resize(c, r)) return;
    tile->hasLastSize = true; tile->lastCols = c; tile->lastRows = r;
    tile->replay.markResize();
  });
}

// ---------------------------------------------------------------- windows (main.js cleanLayout / takeLayout / tiles:shown / desk*)
layout::LayoutMap D::cleanLayout(const json& in) const {
  layout::LayoutMap clean;
  const layout::Grid g = grid();
  if (!in.is_object()) return clean;
  for (auto it = in.begin(); it != in.end(); ++it) {
    if (!it.value().is_array()) continue;
    layout::Cells set;
    for (const json& c : it.value()) if (c.is_number()) set.push_back(c.get<int>());
    const layout::Cells cells = layout::norm(set, g);
    if (!cells.empty() && layout::isLegal(cells, g)) clean.emplace_back(it.key(), cells);
  }
  return clean;
}
void D::takeLayout(Entry* entry, const json& in) { takeLayoutMap(entry, cleanLayout(in)); }
// A tile lives in exactly one pane of exactly one window (F5) — unless Settings says a session may show in
// several windows (oneWindow off). The window that loses it is told (`layout:evict`).
// Under the remote-desk rule (remoterule.hpp) among the deck's OWN windows only: a remote desk's layout takes nothing from a
// window here, and a window here takes nothing from a remote desk.
void D::takeLayoutMap(Entry* entry, layout::LayoutMap l) {
  entry->layout = std::move(l);
  const json& ow = js::get(settings, "oneWindow");
  const bool oneWindow = !(ow.is_boolean() && !ow.get<bool>());
  if (!opt.remoteDeskIsAWindow) {
    const std::vector<Entry*> own = winEntries();
    size_t taker = own.size();
    for (size_t i = 0; i < own.size(); i++) if (own[i] == entry) taker = i;
    std::vector<std::string> ids; for (const auto& kv : entry->layout) ids.push_back(kv.first);
    for (const remoterule::Eviction& ev : remoterule::evictions(taker, entry->remote, ids, ruleEntries(own), oneWindow)) {
      Entry* other = own[ev.entry];
      other->layout.erase(std::remove_if(other->layout.begin(), other->layout.end(), [&](const std::pair<std::string, layout::Cells>& kv) { return holds(entry->layout, kv.first); }), other->layout.end());
      sendToEntry(other, "layout:evict", json(ev.ids));
    }
  } else if (oneWindow) for (Entry* other : allEntries()) {
    if (other == entry) continue;
    json gone = json::array();
    for (const auto& kv : other->layout) if (holds(entry->layout, kv.first)) gone.push_back(kv.first);
    if (gone.empty()) continue;
    other->layout.erase(std::remove_if(other->layout.begin(), other->layout.end(), [&](const std::pair<std::string, layout::Cells>& kv) { return holds(entry->layout, kv.first); }), other->layout.end());
    sendToEntry(other, "layout:evict", gone);
    if (other->remote) saveState({ { "deskLayout", layoutJson(other->layout) } });
  }
  // a remote desk's layout persists beside the windows' (tiles.json `deskLayout`), restored when a desk connects
  if (entry->remote) saveState({ { "deskLayout", layoutJson(entry->layout) } });
  saveWindows(); pushMeta();
}
// ipcMain 'tiles:shown' / deskShown
void D::entryShown(Entry* e, const std::vector<std::string>& ids, bool hidden) {
  std::vector<std::string> next;
  for (const std::string& id : ids) if (tileById(id)) next.push_back(id);
  const bool same = next == e->shown && e->hidden == hidden;
  remoterule::Shown prev; prev.has = true; prev.shown = e->shown; prev.hidden = e->hidden;
  e->shown = next; e->hidden = hidden;
  if (same) return;
  if (e->remote && !opt.remoteDeskIsAWindow) {
    // THE exception to "a remote does not touch sleep": a tile that ARRIVES on a remote's screen asleep is woken,
    // because there is nothing to stream otherwise. Arrivals only (new to its panes, or its tab back from
    // hidden): the remote's showing it is not "on screen" for hibernation, so the session sleeps again by the
    // office's rules, and a later change to another pane does not wake it a second time.
    remoterule::Shown cur; cur.has = true; cur.shown = next; cur.hidden = hidden;
    for (const std::string& id : remoterule::wakesForRemote(prev, cur, [this](const std::string& x) { Tile* t = tileById(x); return t && t->s.asleep; })) wakeTile(id, "shown on a remote");
    pushMeta();                                 // the driver rule and the mirrors follow what a remote shows
    return;
  }
  // a tile arriving on screen asleep is woken now — not for a hidden window
  if (!hidden) for (const std::string& id : next) { Tile* t = tileById(id); if (t && t->s.asleep) wakeTile(id, "on screen"); }
  pushMeta();
}
void D::removeEntry(Entry* e) {
  for (auto& c : clients) if (c->entry == e) c->entry = nullptr;
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::unique_ptr<Entry>& x) { return x.get() == e; }), entries.end());
}
// lib/phone.js deskOpen + main.js deskOpen: a remote announces itself as a desk and becomes a window entry.
// Two remote desks at once is unsupported: a second one is refused while the first is connected.
static const char* const kOneDesk = "one desk at a time: another browser desk is connected \xE2\x80\x94 close that tab first";
// The desk slot (lib/phone.js deskOpen, 2026-11-18): a second desk is refused while the first is THERE. One that
// is connected but has not been heard from for deskSilentMs is asked (a ping): no answer within deskProbeMs, and
// the new desk takes its place — a remote whose connection died behind a proxy must not hold the slot.
// True = the caller was answered, or will be when the probe ends.
bool D::deskSlot(Client& c, const json& rid, const json& opts) {
  Client* other = nullptr;
  for (auto& o : clients) if (o.get() != &c && o->remote && o->desk) other = o.get();
  if (!other) return false;
  const double heard = sm.phone ? sm.phone->heardAt(other->id) : -1;
  if (heard < 0 || now() - heard <= opt.deskSilentMs) throw std::runtime_error(kOneDesk);
  const ClientId otherId = other->id;
  if (!sm.phone->ping(otherId)) {                 // it cannot even be asked: gone
    sm.phone->drop(otherId);
    if (Client* still = clientById(otherId)) deskGone(*still);
    return false;
  }
  DeskProbe p; p.caller = c.id; p.other = otherId; p.rid = rid; p.opts = opts; p.before = heard; p.t0 = now();
  const ClientId caller = c.id;
  p.timer = setTimer(std::max(5.0, std::min(100.0, opt.deskProbeMs / 10)), [this, caller] { deskProbeStep(caller); }, std::max(5.0, std::min(100.0, opt.deskProbeMs / 10)));
  deskProbes.erase(std::remove_if(deskProbes.begin(), deskProbes.end(), [&](const DeskProbe& x) { if (x.caller != caller) return false; clearTimer(x.timer); return true; }), deskProbes.end());
  deskProbes.push_back(std::move(p));
  return true;
}
void D::deskProbeStep(ClientId caller) {
  auto it = std::find_if(deskProbes.begin(), deskProbes.end(), [&](const DeskProbe& x) { return x.caller == caller; });
  if (it == deskProbes.end()) return;
  Client* c = clientById(caller);
  Client* other = clientById(it->other);
  const bool alive = other && other->desk && sm.phone && sm.phone->heardAt(it->other) > it->before;
  if (c && !alive && other && other->desk && now() - it->t0 < opt.deskProbeMs) return;       // still waiting
  const DeskProbe p = *it;
  clearTimer(p.timer); deskProbes.erase(it);
  if (!c) return;                                  // the asker left while it waited
  if (alive) { fail(*c, p.rid, kOneDesk); return; }
  // the connected desk did not answer; the new one takes its place
  if (other && sm.phone) sm.phone->drop(p.other);
  if (Client* still = clientById(p.other)) deskGone(*still);
  c = clientById(caller); if (!c) return;
  try { reply(*c, p.rid, deskOpen(*c, p.opts)); }
  catch (const std::exception& e) { if (Client* again = clientById(caller)) fail(*again, p.rid, e.what()); }
}
json D::deskOpen(Client& c, const json& opts) {
  for (auto& o : clients) if (o.get() != &c && o->remote && o->desk) throw std::runtime_error("one desk at a time: another browser desk is connected \xE2\x80\x94 close that tab first");
  c.desk = true; c.attachedSet.clear(); c.shownSet.clear(); c.hidden = false;
  c.hasMetaKey = false;                         // what it was sent as a phone is not what a desk is sent
  c.deskName = presenceName(js::get(opts, "name")); c.deskKind = deskKind(js::get(opts, "kind"));   // the native exe says 'native'; anything else is a browser
  if (!hasDeskIndex) { hasDeskIndex = true; deskIndex = nextWindowIndex++; }
  if (c.entry) removeEntry(c.entry);            // (a second deskInit on one socket: the entry is made again)
  const json st = tilesStore->read();
  layout::LayoutMap saved = cleanLayout(js::get(st, "deskLayout"));
  saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const std::pair<std::string, layout::Cells>& kv) {
    if (!tileById(kv.first)) return true;
    // the remote-desk rule: it comes back WHOLE, also the sessions a real window holds (both show them). Before it: minus
    // any tile a real window holds, so connecting from afar never pulled a pane off the desktop.
    if (opt.remoteDeskIsAWindow) for (Entry* w : winEntries()) if (holds(w->layout, kv.first)) return true;
    return false; }), saved.end());
  std::unique_ptr<Entry> e(new Entry());
  e->index = deskIndex; e->client = c.id; e->remote = true;
  c.entry = e.get();
  entries.push_back(std::move(e));
  takeLayoutMap(c.entry, saved);                // persists, publishes
  const json meta = deskMetaFor(remoteMeta(collectMeta(), true), c);
  return { { "home", home }, { "meta", meta }, { "platform", publicPlatform() }, { "layout", layoutJson(c.entry->layout) }, { "windowIndex", c.entry->index }, { "grid", gridJson() } };
}
void D::deskGone(Client& c) {
  if (!c.desk) return;
  c.desk = false;
  if (c.entry) { removeEntry(c.entry); pushMeta(); }
}

// ---------------------------------------------------------------- settings (main.js applySettings / settings:get)
static layout::LayoutMap rawLayout(const json& in) {
  layout::LayoutMap out;
  if (in.is_object()) for (auto it = in.begin(); it != in.end(); ++it) {
    if (!it.value().is_array()) continue;
    layout::Cells set; for (const json& c : it.value()) if (c.is_number()) set.push_back(c.get<int>());
    out.emplace_back(it.key(), set);
  }
  return out;
}
json D::applySettingsCall(const json& s) {
  const layout::Grid before = grid();
  const json phoneBefore = js::get(settings, "phone");
  // The driver's words in (setDriver, saveSettings). With the claim: the four, and 'desk' as 'office'. Without:
  // this tree's three, and 'office' taken as 'desk' (a client that already speaks the new words changes what it
  // meant to); 'remote' is no value here and changes nothing.
  const std::string driverIn = s.is_object() ? driverrule::mode(js::get(s, "driver")) : std::string();
  if (!opt.driverClaim && driverIn == "office") { json s2 = s; s2["driver"] = "desk"; settings = applySettings(defaults, settings, s2, sm.random->hex(8)); }
  else settings = applySettings(defaults, settings, s, sm.random->hex(8));
  if (opt.driverClaim && !driverIn.empty()) settings["driver"] = driverIn;
  // settings.awayMinutes (the review branch's setting; DEFAULT_SETTINGS on this tree does not have it yet)
  { const json& am = js::get(s, "awayMinutes");
    if (s.is_object() && s.contains("awayMinutes") && !(am.is_string() && am.get<std::string>().empty()) && !am.is_null() && std::isfinite(js::toNumber(am))) settings["awayMinutes"] = awayMinutes(am);
    if (presence && presence->setAwayMinutes(js::get(settings, "awayMinutes"), now())) writePresence(); }
  const layout::Grid after = grid();
  if (before.rows != after.rows || before.cols != after.cols) {
    for (Entry* w : allEntries()) w->layout = layout::regrid(w->layout, before, after);
    const json st = tilesStore->read();
    saveWindows();
    saveState({ { "deskLayout", layoutJson(layout::regrid(rawLayout(js::get(st, "deskLayout")), before, after)) } });
    for (Entry* w : winEntries()) { json payload = gridJson(); payload["layout"] = layoutJson(w->layout); sendToEntry(w, "grid:changed", payload); }
    deskEvent("grid:changed", gridJson());       // the remote desk is a window too (it re-reads its layout on init)
  }
  saveSettings(); refreshAccounts(); pushMeta();
  if (asks) asks->writeDeckFile();             // sessions read the new threshold / picker mode at their next ask (a guest writes nothing)
  const json& phoneAfter = js::get(settings, "phone");
  bool phoneChanged = false;
  for (const char* k : { "enabled", "port", "bind", "token" }) if (js::get(phoneBefore, k) != js::get(phoneAfter, k)) phoneChanged = true;
  if (phoneChanged && sm.phone) { sm.phone->sync(phoneAfter); pushMeta(); }
  json out = settings; out["accounts"] = publicAccounts(accounts); out["phoneStatus"] = phoneStatus();
  return out;
}
json D::getSettings(bool desktopShape) {
  json out = settings;
  out["accounts"] = publicAccounts(accounts); out["accountRows"] = accountRows(accounts);
  if (!desktopShape) {
    // the phone may read settings, but never the token: regenerating it from the page would lock the page out
    const json& p = js::get(settings, "phone");
    out["phone"] = { { "enabled", js::get(p, "enabled") }, { "port", js::get(p, "port") }, { "bind", js::get(p, "bind") } };
    return out;
  }
  out["aliasFile"] = aliasFile; out["userswitch"] = opt.userswitch; out["bash"] = opt.bash;
  out["phoneStatus"] = phoneStatus();
  return out;
}

// main.js saveAccountRows / writeAliasFile: the account editor writes the alias file userswitch parses
static std::string toHomeRelative(const std::string& dir, const std::string& home, bool win) {
  const std::string norm = normalizePath(dir, win), h = normalizePath(home, win);
  // .split(path.sep).join('/'): only Windows has a separator to turn (a '\\' in a POSIX path is a character)
  auto fwd = [win](std::string s) { if (win) for (char& c : s) if (c == '\\') c = '/'; return s; };
  if (lowerAscii(norm).compare(0, h.size(), lowerAscii(h)) == 0) return "$HOME" + fwd(norm.substr(h.size()));
  return fwd(norm);
}
json D::saveAccountRows(const json& rows) {
  if (!opt.allowAccountEdit) throw std::runtime_error("not in the native local deck yet: saveAccounts");
  std::string text =
    "# Claude Code account aliases. Read by userswitch (session moves) and " DECK_EDITION_PRODUCT " (the sidebar).\n"
    "# Edited by " DECK_EDITION_PRODUCT " > Settings > Accounts; hand edits are fine too. One line per keyword:\n"
    "#   alias <keyword>='CLAUDE_CONFIG_DIR=\"$HOME/.claude-<name>\" claude'\n"
    "# Keywords are [A-Za-z0-9_]. The bare `claude` keyword is the default account (~/.claude) and needs no line.\n"
    "\n";
  if (rows.is_array()) for (const json& r : rows) {
    const std::string kws = js::truthy(js::get(r, "keywords")) ? js::toString(js::get(r, "keywords")) : std::string();
    std::vector<std::string> keywords; std::string cur;
    for (size_t i = 0; i <= kws.size();) {                      // split(/[\s,]+/)
      if (i == kws.size()) { if (!cur.empty()) keywords.push_back(cur); break; }
      size_t j = i; const uint32_t cp = js::nextCodePoint(kws, j);
      if (cp == ',' || js::isSpace(cp)) { if (!cur.empty()) keywords.push_back(cur); cur.clear(); } else cur.append(kws, i, j - i);
      i = j;
    }
    std::string dir = js::trim(js::truthy(js::get(r, "dir")) ? js::toString(js::get(r, "dir")) : std::string());
    if (dir.empty() || keywords.empty()) continue;
    auto sepAt = [&](size_t n) { return dir.size() == n || dir[n] == '\\' || dir[n] == '/'; };
    if (dir[0] == '~' && sepAt(1)) dir = home + dir.substr(1);
    else if (dir.compare(0, 5, "$HOME") == 0 && sepAt(5)) dir = home + dir.substr(5);
    else if (lowerAscii(dir).compare(0, 13, "%userprofile%") == 0) dir = home + dir.substr(13);
    // path.isAbsolute: path.win32's (a drive and a separator, or a leading separator) or path.posix's (a leading '/')
    const bool win = sm.env->os() == "win32";
    const bool absolute = win ? ((dir.size() > 2 && dir[1] == ':' && (dir[2] == '\\' || dir[2] == '/')) || dir[0] == '\\' || dir[0] == '/') : dir[0] == '/';
    if (!absolute) dir = joinPath(home, dir);
    sm.files->makeDirs(dir);
    const std::string rel = toHomeRelative(dir, home, win);
    for (const std::string& kw : keywords) {
      bool ok = !kw.empty() && kw != "claude";
      for (char ch : kw) if (!js::isWordChar((unsigned char)ch)) ok = false;
      if (ok) text += "alias " + kw + "='CLAUDE_CONFIG_DIR=\"" + rel + "\" claude'\n";
    }
  }
  if (sm.files->isFile(aliasFile)) sm.files->copy(aliasFile, aliasFile + ".bak-" + isoStamp(now()));
  sm.files->write(aliasFile, text);
  loadAccounts(); refreshAccounts();          // reloadAccounts
  return accountRows(accounts);
}

// ---------------------------------------------------------------- boot (main.js whenReady: the restore)
void D::restore() {
  const json st = tilesStore->loadAtBoot().data;
  groups.clear();
  const json& gs = js::get(st, "groups");
  if (gs.is_array()) for (const json& g : gs) if (g.is_string() && !js::trim(g.get<std::string>()).empty()) groups.push_back(g);
  // R7: every window comes back with its layout. Older state files have one `bounds` and one `layout`.
  const json& ws = js::get(st, "windows");
  json saved = (ws.is_array() && !ws.empty()) ? ws
    : json::array({ { { "bounds", js::get(st, "bounds") }, { "layout", js::truthy(js::get(st, "layout")) ? js::get(st, "layout") : json::object() } } });
  // R13: an auxiliary window that had nothing in any of its cells does not come back; the primary always does
  savedWindows = json::array();
  for (size_t i = 0; i < saved.size(); i++) {
    const layout::LayoutMap lay = cleanLayout(js::get(saved[i], "layout"));
    if (i > 0 && lay.empty()) continue;
    savedWindows.push_back({ { "bounds", js::truthy(js::get(saved[i], "bounds")) ? js::get(saved[i], "bounds") : json() }, { "layout", layoutJson(lay) } });
  }
  // windows the shell made before start() take their saved place now
  for (Entry* e : winEntries()) if (savedTaken < savedWindows.size()) {
    const json& sw = savedWindows[savedTaken++];
    if (js::truthy(js::get(sw, "bounds"))) e->bounds = js::get(sw, "bounds");
    e->layout = cleanLayout(js::get(sw, "layout"));
  }
  const json& list = js::get(st, "tiles");
  const size_t count = list.is_array() ? list.size() : 0;
  restoringUntil = now() + count * SPAWN_GAP_MS + 2000;
  int queued = 0;
  std::vector<std::string> restoredClaude;     // restorerefresh.hpp: the sessions refreshed once the restore is over
  if (list.is_array()) for (const json& s : list) {
    const bool shell = opt.persistShells && js::get(s, "kind") == "shell";
    const std::optional<std::string> account = js::get(s, "account").is_string() ? std::optional<std::string>(js::get(s, "account").get<std::string>()) : std::nullopt;
    // a cloud tile comes back as its session (no process; through the queue all the same, so the tiles keep their order)
    const bool isCloud = js::get(s, "kind") == "cloud";
    if (!shell && (!acctById(account) || !(isCloud ? isCloudId(js::str(js::get(s, "cloudId"))) : js::truthy(js::get(s, "sessionId"))))) continue;
    if (!js::get(s, "id").is_string()) continue;
    std::shared_ptr<std::unique_ptr<Tile>> base = std::make_shared<std::unique_ptr<Tile>>(new Tile());
    Tile& b = **base;
    b.s.id = js::get(s, "id"); b.createdAt = js::get(s, "createdAt").is_number() ? js::get(s, "createdAt").get<double>() : now();
    b.s.pinned = js::truthy(js::get(s, "pinned"));
    if (js::truthy(js::get(s, "group"))) b.s.group = js::toString(js::get(s, "group"));
    if (js::truthy(js::get(s, "title"))) b.s.knownTitle = js::toString(js::get(s, "title"));
    if (js::truthy(js::get(s, "userTitle"))) b.s.userTitle = js::toString(js::get(s, "userTitle"));
    if (js::get(s, "browser").is_boolean()) b.s.browser = js::get(s, "browser").get<bool>();
    if (js::get(s, "desk").is_string() && !js::get(s, "desk").get<std::string>().empty()) b.s.desk = js::get(s, "desk").get<std::string>();
    // paused before the restart: a mark "until restart" resumes by itself; with settings.resumeAfterRestart (default on) every one does
    if (!shell && !isCloud) pauses.load(b.s.id, js::get(s, "pause"), js::get(settings, "resumeAfterRestart") != json(false));
    if (!shell && !isCloud) auditors.load(b.s.id, s);                     // an auditor or a child (auditors.hpp); settled below
    SpawnSpec sp;
    sp.kind = shell ? "shell" : isCloud ? "cloud" : "claude";
    if (isCloud) { sp.account = account; sp.cloudId = js::str(js::get(s, "cloudId")); }
    else if (!shell) { sp.account = account; sp.sessionId = js::toString(js::get(s, "sessionId")); }
    if (js::truthy(js::get(s, "cwd"))) sp.cwd = js::toString(js::get(s, "cwd"));
    if (!shell && !isCloud && js::truthy(js::get(s, "asleep")) && js::truthy(js::get(js::get(settings, "hibernate"), "enabled"))) {
      // restoreSleeping: a tile the deck last closed asleep comes back asleep
      const double slept = js::get(s, "sleptAt").is_number() ? js::get(s, "sleptAt").get<double>() : 0;
      b.s.kind = "claude"; b.s.account = acctById(account)->id; b.s.sessionId = sp.sessionId; b.s.cwd = sp.cwd ? *sp.cwd : home;
      b.s.startedAt = slept != 0 ? slept : b.createdAt; b.s.lastStoppedAt = b.s.startedAt;
      b.s.asleep = true; b.s.sleptAt = slept != 0 ? slept : now();
      tiles.push_back(std::move(*base));
      continue;
    }
    queued++; restorePending++;
    if (sp.kind == "claude") restoredClaude.push_back(b.s.id);
    queueSpawn([this, base, sp] {
      Tile* raw = base->get();
      try { tiles.push_back(std::move(*base)); spawnTile(sp, raw); }
      catch (...) { for (size_t i = 0; i < tiles.size(); i++) if (tiles[i].get() == raw) { tiles.erase(tiles.begin() + (long long)i); break; } }
      if (--restorePending == 0) restoringUntil = now() + 2000;
    });
  }
  auditors.settle();                          // a child whose auditor did not come back is a child no more
  restoreCapAt = now() + queued * SPAWN_GAP_MS + 120000;
  restoringUntil = now() + 2000;
  startRestoreRefresh(restoredClaude);
}

// ---------------------------------------------------------------- the call table (main.js PHONE_ACTIONS + the window's IPC handlers)
// A window: `rid` 0 sends no reply at all (PROTOCOL.md). A remote: lib/phone.js answers every call, whatever its rid.
void D::reply(const Client& c, const json& rid, const json& result) {
  if (c.remote) { send(c.id, { { "t", "ok" }, { "rid", rid }, { "result", result } }); return; }
  if (rid.is_number() && rid.get<double>() != 0) send(c.id, { { "t", "ok" }, { "rid", rid }, { "result", result } });
}
void D::fail(const Client& c, const json& rid, const std::string& error) {
  if (c.remote || (rid.is_number() && rid.get<double>() != 0)) send(c.id, { { "t", "err" }, { "rid", rid }, { "error", error } });
}

static const char* const REMOTE_ACTIONS[] = { "input", "askDraftGet", "askLogState", "askDraft", "askClear", "askDone", "userInput", "resize", "newTile", "closeTile",
  "moveTile", "restartTile", "refreshTile", "renameTile", "pin", "setGroup", "setGroups", "viewHtml", "sessionHtml", "transcript", "externalSessions", "pastSessions",
  "adoptSession", "bringHere", "moveAll", "refreshUsage", "setDriver", "holdAdd", "triggersList", "triggerSet", "holdRemove", "getSettings", "saveSettings", "sleepTile", "wakeTile",
  "reorder", "saveAccounts", "nudge", "screenProbeReply", "triggerDeclare", "triggerRemove", "restartWhenQuiet", "restartNow", "updateLater", "pasteImage",
  "pauseTile", "resumeTile", "pauseAll", "resumeAll", "makeAuditor", "unmakeAuditor", "setAuditorChild", "unchildTile", "auditorSend", "auditorDrop" };

// main.js savePastedImage: png / jpeg / gif / webp, 20 MB at most, in <stateDir>/paste (the deck's own folder: the
// core has no temp folder of its own). "" + why when refused.
std::string D::savePastedImage(const std::string& id, const json& b64, const json& mime, std::string& why) {
  if (!tileById(id)) { why = "no such session"; return ""; }
  const std::string m = mime.is_string() ? mime.get<std::string>() : std::string();
  const char* ext = m == "image/png" ? ".png" : m == "image/jpeg" ? ".jpg" : m == "image/gif" ? ".gif" : m == "image/webp" ? ".webp" : nullptr;
  if (!ext) { why = "not a picture this can take (png, jpeg, gif, webp)"; return ""; }
  const std::string bytes = unbase64(b64.is_string() ? b64.get<std::string>() : std::string());
  if (bytes.empty()) { why = "an empty picture"; return ""; }
  if (bytes.size() > 20u * 1024 * 1024) { why = "a picture over 20 MB"; return ""; }
  const std::string dir = joinPath(stateDir, "paste");
  sm.files->makeDirs(dir);
  std::string stamp = isoStamp(now()); for (char& ch : stamp) if (ch == ':' || ch == '.') ch = '-';
  const std::string file = joinPath(dir, stamp + "-" + sm.random->hex(3) + ext);
  if (!sm.files->write(file, bytes)) { why = "the picture could not be saved"; return ""; }
  return file;
}

void D::call(Client& c, const json& rid, const std::string& fn, const json& args) {
  auto arg = [&](size_t i) -> const json& { return args.is_array() && i < args.size() ? args[i] : js::null(); };
  auto idArg = [&](size_t i) { return arg(i).is_string() ? arg(i).get<std::string>() : std::string(); };
  auto num = [&](size_t i) { return arg(i).is_number() ? arg(i).get<double>() : std::nan(""); };
  const ClientId cid = c.id; const bool remote = c.remote;
  // an answer that arrives later (a kill, a move): to the caller if it is still there
  auto later = [this, cid, rid](const json& result) { if (Client* cl = clientById(cid)) reply(*cl, rid, result); };
  auto laterErr = [this, cid, rid](const std::string& e) { if (Client* cl = clientById(cid)) fail(*cl, rid, e); };

  if (fn == "deskInit") {
    if (remote) { if (deskSlot(c, rid, arg(0))) return; reply(c, rid, deskOpen(c, arg(0))); return; }
    c.inited = true;
    reply(c, rid, { { "home", home }, { "meta", collectMeta() }, { "platform", publicPlatform() }, { "layout", c.entry ? layoutJson(c.entry->layout) : json::object() },
      { "windowIndex", c.entry ? c.entry->index : 0 }, { "grid", gridJson() } });
    return;
  }
  if (remote) {
    bool known = false; for (const char* a : REMOTE_ACTIONS) if (fn == a) known = true;
    if (!known) throw std::runtime_error("unknown action: " + fn);
  }
  if (fn == "newTile") {
    // kind 'cloud' with a task makes a cloud session (main.js cloudSpec): checked before a tile exists
    if (js::get(arg(0), "kind") == "cloud") { reply(c, rid, spawnTile(cloudSpec(arg(0)), nullptr)); return; }
    const TileSpec ts = newTileSpec(arg(0));
    SpawnSpec sp; sp.kind = ts.kind;
    if (js::truthy(ts.account)) sp.account = js::toString(ts.account);
    if (js::truthy(ts.sessionId)) sp.sessionId = js::toString(ts.sessionId);
    if (js::truthy(ts.cwd)) sp.cwd = js::toString(ts.cwd);
    if (ts.hasBrowser) { sp.hasBrowser = true; if (ts.browser.is_boolean()) sp.browser = ts.browser.get<bool>(); }
    reply(c, rid, spawnTile(sp, nullptr));
  } else if (fn == "closeTile") { closeTile(idArg(0), [later] { later(nullptr); });
  } else if (fn == "restartTile") { restartTile(idArg(0)); reply(c, rid, nullptr);
  } else if (fn == "bringHere") { bringHere(idArg(0), [later](bool ok) { later(ok); });
  } else if (fn == "refreshTile") { refreshTile(idArg(0), arg(1), [later] { later(nullptr); });
  } else if (fn == "pauseTile") { std::string why; if (pauseTile(idArg(0), js::truthy(js::get(arg(1), "untilRestart")), why)) reply(c, rid, true); else fail(c, rid, why);
  } else if (fn == "resumeTile") { std::string why; if (resumeTile(idArg(0), why)) reply(c, rid, true); else fail(c, rid, why);
  } else if (fn == "pauseAll") { reply(c, rid, pauseAll(js::truthy(js::get(arg(0), "untilRestart"))));
  } else if (fn == "resumeAll") { reply(c, rid, resumeAll());
  } else if (fn == "makeAuditor") { std::string why; const json r = makeAuditor(idArg(0), arg(1), why); if (why.empty()) reply(c, rid, r); else fail(c, rid, why);
  } else if (fn == "unmakeAuditor") { std::string why; if (unmakeAuditor(idArg(0), why)) reply(c, rid, true); else fail(c, rid, why);
  } else if (fn == "setAuditorChild") { std::string why; const json r = setAuditorChild(idArg(0), idArg(1), why); if (why.empty()) reply(c, rid, r); else fail(c, rid, why);
  } else if (fn == "unchildTile") { std::string why; if (unchildTile(idArg(0), why)) reply(c, rid, true); else fail(c, rid, why);
  } else if (fn == "auditorSend") { std::string why; if (auditorSend(idArg(0), why)) reply(c, rid, true); else fail(c, rid, why);
  } else if (fn == "auditorDrop") { std::string why; if (auditorDrop(idArg(0), why)) reply(c, rid, true); else fail(c, rid, why);
  } else if (fn == "sleepTile") { sleepTile(idArg(0), "asked", [later](bool ok) { later(ok); });
  } else if (fn == "wakeTile") { reply(c, rid, wakeTile(idArg(0), "asked"));
  } else if (fn == "renameTile") { renameTile(idArg(0), arg(1)); reply(c, rid, nullptr);
  } else if (fn == "pin") { reply(c, rid, setPinned(idArg(0), js::truthy(arg(1))));
  } else if (fn == "reorder") { reorderPinned(arg(0)); reply(c, rid, nullptr);
  } else if (fn == "setGroup") { setTileGroup(idArg(0), arg(1)); reply(c, rid, nullptr);
  } else if (fn == "setGroups") { reply(c, rid, setGroups(arg(0)));
  } else if (fn == "pasteImage") {
    // a picture pasted on a remote (main.js savePastedImage): saved here, its path answered for the remote to paste
    std::string err; const std::string file = savePastedImage(idArg(0), arg(1), arg(2), err);
    if (file.empty()) fail(c, rid, err); else reply(c, rid, file);
  } else if (fn == "userInput") { noteDriver(idArg(0), remote ? (opt.driverClaim && c.desk ? "remote" : "phone") : "desktop", kindOf(c)); reply(c, rid, nullptr);
  } else if (fn == "input") {
    const std::string d = arg(1).is_string() ? arg(1).get<std::string>() : (arg(1).is_null() ? std::string() : js::toString(arg(1)));
    if (remote) { noteKeys(idArg(0), d, c.desk ? "remote" : "phone"); writeTile(idArg(0), d); } else windowInput(c, idArg(0), d);
    reply(c, rid, nullptr);
  } else if (fn == "resize") {
    if (remote && !opt.driverClaim) {
      // remember what the remote is showing: the desktop view mirrors it while a remote has the tile
      // (with the claim nothing is remembered: the office mirrors the pty's own size, collectMeta)
      Tile* t = tileById(idArg(0));
      if (t && num(1) > 1 && num(2) > 1) t->s.phoneSize = std::make_pair((int)num(1), (int)num(2));
    }
    resizeTile(idArg(0), num(1), num(2), kindOf(c)); reply(c, rid, nullptr);
  } else if (fn == "nudge") { nudgeTile(idArg(0), num(1), num(2), kindOf(c)); reply(c, rid, nullptr);
  } else if (fn == "setOscTitle") {                             // tile:osc-title: a window read a title off the stream
    if (Tile* t = tileById(idArg(0))) t->s.oscTitle = js::truthy(arg(1)) ? std::optional<std::string>(js::toString(arg(1))) : std::nullopt;
    reply(c, rid, nullptr);
  } else if (fn == "screenProbeReply") { reply(c, rid, nullptr);  // no probe is ever sent (main's probeTrust is not called today)
  } else if (fn == "getSettings") { reply(c, rid, getSettings(!remote || c.desk));
  } else if (fn == "saveSettings") {
    json s = arg(0).is_object() ? arg(0) : json::object();
    if (remote) s.erase("phone");                               // a remote never changes the phone block
    reply(c, rid, applySettingsCall(s));
  } else if (fn == "setDriver") { applySettingsCall({ { "driver", arg(0) } }); reply(c, rid, js::get(settings, "driver"));   // old and new words (applySettingsCall)
  } else if (fn == "holdAdd") {
    // main.js holdAddDeclaring: a triggered prompt's trigger is created (off) when it is new
    const json spec = arg(1).is_object() ? arg(1) : json::object();
    json added = held->add(idArg(0), spec);
    if (spec.value("kind", std::string()) == "triggered" && spec.contains("trigger") && spec["trigger"].is_string()) {
      try { triggerDeclare(spec["trigger"].get<std::string>(), remote ? "remote" : "desk"); } catch (...) { /* the prompt stays */ }
    }
    reply(c, rid, added);
  } else if (fn == "holdRemove") { reply(c, rid, held->remove(idArg(0)));
  } else if (fn == "triggersList") { reply(c, rid, triggersList());
  } else if (fn == "triggerDeclare") { reply(c, rid, triggerDeclare(js::truthy(arg(0)) ? js::toString(arg(0)) : std::string(), remote ? "remote" : "desk"));
  } else if (fn == "triggerSet") { reply(c, rid, triggerSet(js::truthy(arg(0)) ? js::toString(arg(0)) : std::string(), js::truthy(arg(1)), remote ? "remote" : "desk"));
  } else if (fn == "triggerRemove") { reply(c, rid, triggerRemove(js::truthy(arg(0)) ? js::toString(arg(0)) : std::string()));
  } else if (fn == "restartWhenQuiet") { reply(c, rid, setQuiet(js::truthy(arg(0)), arg(1) == "update" ? "update" : "manual"));
  } else if (fn == "restartNow") { reply(c, rid, restartDeck(remote ? "asked, from a remote" : "asked"));
  } else if (fn == "updateLater") { dismissUpdate(); reply(c, rid, nullptr);
  } else if (fn == "externalSessions") { reply(c, rid, externalSessions());
  } else if (fn == "pastSessions") { reply(c, rid, pastSessions());
  } else if (fn == "adoptSession") { adoptSession(arg(0), later, laterErr);
  } else if (fn == "moveTile") { moveTile(idArg(0), idArg(1), arg(2), [later] { later(nullptr); }, laterErr);
  } else if (fn == "moveAll") { moveAll(idArg(0), idArg(1), js::truthy(arg(2)), arg(3), later, laterErr);
  } else if (fn == "saveAccounts") { const json out = saveAccountRows(arg(0)); pushMeta(); reply(c, rid, out);
  } else if (fn == "transcript") {
    // the conversation itself, straight off disk, paged, for reading (the phone's text view)
    Tile* t = tileById(idArg(0));
    if (!t || !t->s.sessionId || t->s.sessionId->empty()) throw std::runtime_error("no such session");
    const Account* a = acctById(t->s.account);
    const std::string file = a ? findTranscript(a->dir, *t->s.sessionId) : std::string();
    std::string text;
    if (file.empty() || !sm.files->read(file, text)) throw std::runtime_error("no transcript on disk for this session yet");
    reply(c, rid, transcriptPage(text, arg(1).is_object() ? arg(1) : json::object()));
  // ---- ask pages (lib/asks.js): the same drafts and Done for a window, the phone's ask screen and a desk's ask pane
  } else if (fn == "askDraftGet") { reply(c, rid, asks ? asks->draft(idArg(0)) : json());
  } else if (fn == "askLogState") { reply(c, rid, asks ? asks->logState(idArg(0)) : json());
  } else if (fn == "askDraft") { if (asks && asks->guest()) throw std::runtime_error(kAskGuestReason); if (asks) asks->setDraft(idArg(0), arg(1)); reply(c, rid, true);
  } else if (fn == "askClear") { if (asks && asks->guest()) throw std::runtime_error(kAskGuestReason); if (asks) asks->clearDraft(idArg(0)); reply(c, rid, true);
  } else if (fn == "askDone") { reply(c, rid, doneAsk(idArg(0), arg(1)));
  } else if (fn == "askOpenLink") { openAskLink(idArg(0)); reply(c, rid, true);        // ask:open-link (a window's; a browser desk opens links itself)
  // ---- the page viewer (lib/viewer.js)
  } else if (fn == "viewHtml") { reply(c, rid, viewHtml(arg(0)));
  } else if (fn == "sessionHtml") { reply(c, rid, sessionHtml(idArg(0)));
  // ---- a cloud session's page in the default browser. claude.ai links only: no page can open anything else through here.
  } else if (fn == "openExternal") {
    const std::string u = arg(0).is_null() ? std::string() : js::toString(arg(0));
    if (u.compare(0, 18, "https://claude.ai/") != 0) throw std::runtime_error("only claude.ai links open from here");
    reply(c, rid, sm.lifecycle ? json(sm.lifecycle->openExternal(u)) : json(false));
  } else if (fn == "refreshUsage") {
    if (!usage) throw std::runtime_error("not in the native local deck yet: refreshUsage");
    if (!usageLive()) throw std::runtime_error("the live usage bars are off (Settings: Live usage bars)");
    usage->pollAll(accounts, true, [this, later] { pushMeta(); later(nullptr); });
  } else throw std::runtime_error("not in the native local deck yet: " + fn);
}

void D::handle(ClientId cid, const json& m) {
  if (!m.is_object() || down) return;
  Client* cp = clientById(cid);
  if (!cp) return;
  Client& c = *cp;
  const std::string t = js::str(js::get(m, "t"));
  const std::string id = js::str(js::get(m, "id"));
  const bool windowLike = c.remote ? c.desk : true;          // carries several panes (attachedSet)
  if (t == "call") {
    const json rid = js::get(m, "rid");
    const std::string fn = js::str(js::get(m, "fn"));
    { const std::string what = realChangeOf(fn); if (!what.empty()) noteReal(what); }   // a user action (onedeck.hpp realChange)
    try { call(c, rid, fn, js::get(m, "args")); }
    catch (const std::exception& e) { if (Client* again = clientById(cid)) fail(*again, rid, e.what()); }
  } else if (t == "attach") {
    // A desk (and a window) attaches to every pane it shows; a phone to one. Either way the attach carries the
    // replay: mode prefix + the window (`since`: only what was written since the pty last changed size).
    if (windowLike) {
      if (id.empty()) return;
      c.attachedSet.insert(id);                // before the tile is looked up: a tile still in the restore queue is carried once it starts
      Tile* tile = tileById(id); if (!tile) return;
      bool any = false;
      const std::string d = session::replayFor(tile->replay, tile->s.modes, js::truthy(js::get(m, "since")), &any);
      if (any) send(cid, { { "t", "data" }, { "id", id }, { "d", d }, { "replay", true } });
      return;
    }
    c.hasAttached = !id.empty(); c.attached = id;
    if (c.hasAttached) if (Tile* tile = tileById(id)) {
      bool any = false;
      const std::string d = session::replayFor(tile->replay, tile->s.modes, false, &any);
      if (any) send(cid, { { "t", "data" }, { "id", id }, { "d", d }, { "replay", true } });
    }
  } else if (t == "detach") {
    if (windowLike) { if (!id.empty()) c.attachedSet.erase(id); else c.attachedSet.clear(); }
    else { c.hasAttached = false; c.attached.clear(); }
  } else if (t == "input") {
    const json& dj = js::get(m, "d");
    const std::string d = dj.is_string() ? dj.get<std::string>() : (dj.is_null() ? std::string() : js::toString(dj));
    if (c.remote) { noteKeys(id, d, c.desk ? "remote" : "phone"); writeTile(id, d); } else windowInput(c, id, d);
  } else if (t == "resize") {
    const json& cj = js::get(m, "cols"); const json& rj = js::get(m, "rows");
    if (!cj.is_number() || !rj.is_number()) return;
    if (c.remote && !opt.driverClaim) { Tile* tile = tileById(id); if (tile && cj.get<double>() > 1 && rj.get<double>() > 1) tile->s.phoneSize = std::make_pair(cj.get<int>(), rj.get<int>()); }
    resizeTile(id, cj.get<double>(), rj.get<double>(), kindOf(c));
  } else if (t == "shown") {
    if (c.remote ? !c.desk : !c.inited) return;
    std::vector<std::string> ids;
    const json& list = js::get(m, "ids");
    if (list.is_array()) for (const json& x : list) if (x.is_string()) ids.push_back(x.get<std::string>());
    const bool hidden = js::truthy(js::get(m, "hidden"));
    if (c.remote) { c.shownSet = std::set<std::string>(ids.begin(), ids.end()); c.hidden = hidden; }
    if (c.entry) entryShown(c.entry, ids, hidden);
  } else if (t == "layout") {
    if ((c.remote ? !c.desk : !c.inited) || !c.entry) return;
    const json before = layoutJson(c.entry->layout);
    takeLayout(c.entry, js::get(m, "layout").is_object() ? js::get(m, "layout") : json::object());
    if (!c.remote && layoutJson(c.entry->layout) != before) noteReal("changed the layout");   // a remote desk's layout is not shared
  } else if (t == "leaving") {
    // a stepping walk has a session another window holds shown here, uncommitted: that window wears the mark
    if ((c.remote ? !c.desk : !c.inited) || !c.entry) return;
    // The remote-desk rule: among the deck's own windows only. A walk on a remote takes nothing from a window here, so there
    // is no pane here to mark as being left (an older page may still send it; it tells nobody), and a window's
    // walk marks nothing on a remote.
    if (!opt.remoteDeskIsAWindow && c.remote) return;
    for (Entry* other : (opt.remoteDeskIsAWindow ? allEntries() : winEntries())) if (other != c.entry) sendToEntry(other, "layout:leaving", { { "id", js::get(m, "id") }, { "on", js::truthy(js::get(m, "on")) } });
  }
}

// ---------------------------------------------------------------- one deck at a time: the marker (onedeck.hpp)
// tiles with a session this deck runs or will run: running, starting, waking, asleep, or still queued for the restore
int D::liveSessionCount() const {
  int n = restorePending;
  for (const auto& t : tiles) if (t->s.kind == "claude" && t->s.sessionId && !t->s.sessionId->empty() && (t->pty || t->s.asleep || t->s.waking)) n++;
  return n;
}
// the marker as this deck is now, written (writeDeckMarker decides whether it may)
static void putDeckMarker(D& d, bool running) {
  const json& od = d.opt.oneDeck;
  const std::string dir = js::str(js::get(od, "sharedDir"));
  const double at = d.now();
  onedeck::Marker m;
  m.deck = "native"; m.running = running; m.pid = (double)d.sm.env->pid(); m.exe = js::str(js::get(od, "exe"));
  if (js::get(od, "args").is_array()) for (const json& a : js::get(od, "args")) if (a.is_string()) m.args.push_back(a.get<std::string>());
  m.stateDir = d.stateDir; m.startedAt = d.oneDeckStartedAt; m.heartbeatAt = isoTime(at); m.liveSessions = running ? d.liveSessionCount() : 0;
  d.sm.files->makeDirs(dir);
  if (!d.sm.files->writeAtomic(joinPath(dir, onedeck::MARKER), onedeck::markerJson(m).dump(2))) d.logEvent("onedeck", "the marker could not be written in " + dir);
}
// The heartbeat and the quit: only while no other live deck holds the marker (a deck that started later, beside this
// one while it had no sessions, is the active one). At start the deck has just passed the guard: putDeckMarker outright.
void D::writeDeckMarker(bool running) {
  if (!oneDeckOn) return;
  const std::string dir = js::str(js::get(opt.oneDeck, "sharedDir"));
  const json cur = onedeck::parseMarker(onedeck::readJson(*sm.files, joinPath(dir, onedeck::MARKER)));
  ProcessList* procs = sm.procs;
  const std::string curExe = js::str(js::get(cur, "exe"));
  if (!onedeck::mayWriteMarker(cur, "native", (double)sm.env->pid(), [procs, curExe](double pid) { return pid > 0 && procs && procs->alive(pid) ? curExe : std::string(); }, now())) return;
  putDeckMarker(*this, running);
}

// ---------------------------------------------------------------- the shared layout (onedeck.hpp Sharer)
json D::ownLayoutFiles() {
  return { { "tilesJson", tilesStore->read() }, { "held", onedeck::readJson(*sm.files, joinPath(stateDir, "held-prompts.json")) },
    { "triggers", onedeck::readJson(*sm.files, joinPath(stateDir, "triggers.json")) }, { "settings", onedeck::readJson(*sm.files, settingsFile()) } };
}
void D::publishShared() { if (sharer && !down) sharer->publish(); }
// a user action: the next write counts it, soon (a change saved elsewhere - held prompts, triggers - is on disk first)
void D::noteReal(const std::string& what) {
  if (!sharer || down) return;
  sharer->noteReal(what);
  setTimer(300, [this] { publishShared(); });
}
std::string D::realChangeOf(const std::string& fn) {
  static const std::map<std::string, std::string> real = {
    { "newTile", "a new tile" }, { "closeTile", "closed a tile" }, { "moveTile", "moved a tile to another account" }, { "renameTile", "renamed a tile" },
    { "pin", "pinned a tile" }, { "reorder", "reordered the tiles" }, { "setGroup", "changed a tile's group" }, { "setGroups", "changed the groups" },
    { "saveSettings", "changed the settings" }, { "setDriver", "changed the settings" }, { "holdAdd", "a held prompt" }, { "holdRemove", "removed a held prompt" },
    { "triggerDeclare", "a trigger" }, { "triggerSet", "set a trigger" }, { "triggerRemove", "removed a trigger" },
    { "pauseTile", "paused a tile" }, { "resumeTile", "resumed a tile" }, { "pauseAll", "paused every session" }, { "resumeAll", "resumed every session" },
    { "makeAuditor", "made an auditor" }, { "unmakeAuditor", "unmade an auditor" }, { "setAuditorChild", "set an auditor's child" }, { "unchildTile", "ended an auditor's child" },
    { "adoptSession", "brought a session here" }, { "moveAll", "moved every session" }, { "bringHere", "brought a session here" }, { "openWindow", "opened a window" } };
  auto it = real.find(fn);
  return it == real.end() ? std::string() : it->second;
}

// ---------------------------------------------------------------- the same-session guard (onedeck.hpp holderOf)
// Who else holds this session right now: a live Claude process that is not one of this deck's, or the other deck's tile
// while that deck runs with sessions (its tiles.json: a tile there that is not asleep).
onedeck::Holder D::sessionHolder(const std::string& sessionId) {
  std::set<double> own = ownPids;
  for (const auto& t : tiles) { if (t->pid) own.insert((double)t->pid); if (t->claudePid) own.insert(t->claudePid); }
  std::vector<onedeck::Record> recs;
  const session::LiveMap live = readLive();
  for (const auto& kv : live.all()) recs.push_back({ kv.first, kv.second.alive, kv.second.pid.is_number() ? kv.second.pid.get<double>() : 0 });
  json otherTiles = json::array();
  if (opt.oneDeck.is_object()) {
    const json m = onedeck::parseMarker(onedeck::readJson(*sm.files, joinPath(js::str(js::get(opt.oneDeck, "sharedDir")), onedeck::MARKER)));
    ProcessList* procs = sm.procs; const std::string exe = js::str(js::get(m, "exe"));
    if (js::str(js::get(m, "deck")) == "electron" && js::get(m, "liveSessions").is_number() && js::get(m, "liveSessions").get<double>() > 0
        && onedeck::markerRunning(m, [procs, exe](double pid) { return pid > 0 && procs->alive(pid) ? exe : std::string(); }, now())) {
      const json j = onedeck::readJson(*sm.files, joinPath(js::str(js::get(m, "stateDir")), "tiles.json"));
      if (js::get(j, "tiles").is_array()) otherTiles = js::get(j, "tiles");
    }
  }
  return onedeck::holderOf(sessionId, recs, own, otherTiles);
}

// The last window closing (removeClient), or the deck ending with its windows still open (the Windows shell's last
// window, a quit with several open): the file gets every window open now AND every one closed in the 2 minutes before,
// each with its layout and bounds, in window order; a tile placed in more than one is kept in the window that closed
// last (main.js `closed`). Only when that is more than one window: one is what the file already says.
void D::keepWindowsAtQuit() {
  static constexpr double QUIT_SEQUENCE_MS = 2 * 60000.0;
  const double t = now();
  std::vector<ClosedWin> kept;
  for (const ClosedWin& c : closedWins) if (t - c.at <= QUIT_SEQUENCE_MS) kept.push_back(c);
  for (Entry* e : winEntries()) kept.push_back({ e->index, e->bounds, e->layout, t });
  if (kept.size() < 2) return;
  std::vector<size_t> newestFirst(kept.size());
  for (size_t i = 0; i < kept.size(); i++) newestFirst[i] = i;
  std::stable_sort(newestFirst.begin(), newestFirst.end(), [&](size_t a, size_t b) { return kept[a].at > kept[b].at; });
  std::set<std::string> placed;
  for (size_t i : newestFirst) {
    layout::LayoutMap l;
    for (const auto& kv : kept[i].layout) if (placed.insert(kv.first).second) l.push_back(kv);
    kept[i].layout = l;
  }
  std::stable_sort(kept.begin(), kept.end(), [](const ClosedWin& a, const ClosedWin& b) { return a.index < b.index; });
  json ws = json::array();
  for (const ClosedWin& c : kept) ws.push_back({ { "bounds", c.bounds }, { "layout", layoutJson(c.layout) } });
  saveState({ { "windows", ws } });
  logEvent("windows", "the quit keeps " + std::to_string(ws.size()) + " windows' layouts");
}

void D::shutdown() {
  if (down) return;
  if (started && !restarting && !winEntries().empty()) keepWindowsAtQuit();   // before `down`: the shared file follows
  down = true;
  if (oneDeckOn && !oneDeckStopped) { oneDeckStopped = true; writeDeckMarker(false); }   // active.json: running:false (never deleted)
  if (started) sm.files->remove(joinPath(stateDir, "running.json"));     // markRunning(false): this is a quit, not a crash
  // main.js window-all-closed: no desk is opened by a deck on its way out; the skill opens its browser window
  // again once the deck is gone (deck.json removed, when it is ours); then the watchdog, then killAll
  deskInbox.reset();
  if (asks) { asks->flushDrafts(); asks->removeDeckFile(); }
  if (started) armExitWatchdog();
  timers.clear(); spawnQueue.clear();
  for (auto& t : tiles) if (t->pty) t->pty->kill();            // killAll
  for (auto& x : dying) if (x.pty) x.pty->kill();
  for (auto& t : tiles) t->pty.reset();                        // a shell's Pty joins here: nothing of ours is running after this
  dying.clear();
}

// ---------------------------------------------------------------- the public face
Deck::Deck(DeckOptions options, Seams seams, Sink sink) : d(new Impl) {
  d->opt = std::move(options); d->sm = seams; d->sink = std::move(sink);
  d->home = d->opt.home; d->stateDir = d->opt.stateDir; d->aliasFile = d->opt.aliasFile;
  d->defaults = defaultSettings(d->home, d->opt.cwdOverride);
  d->settings = d->defaults;
  d->tilesStore.reset(new StateFile(*d->sm.files, *d->sm.clock, joinPath(d->stateDir, "tiles.json"),
    [](const json& v) { const json& t = js::get(v, "tiles"); return t.is_array() ? t.size() : (size_t)0; }, json{ { "tiles", json::array() } }));
}
Deck::~Deck() { shutdown(); }

void Deck::start() {
  if (d->started) return;
  d->sm.files->makeDirs(d->stateDir);
  d->loadAccounts();
  d->loadSettings();
  d->refreshAccounts();
  d->initMore();                               // triggers and held prompts exist before the first meta
  d->initParity();                             // the ask feed (it needs the held prompts) and the cloud poller
  d->startPresence();                          // deck-state.json, before any tile is spawned: their environment names it
  d->started = true;
  d->deckStartedAt = d->now();                 // RESUME's {restartedAt}
  // running.json: present while the deck runs, removed on a normal quit — a supervisor outside the deck
  // relaunches it when the file is there and no deck process is (main.js markRunning)
  d->sm.files->write(joinPath(d->stateDir, "running.json"), json{ { "pid", d->sm.env->pid() }, { "at", isoTime(d->now()) } }.dump());
  d->restore();
  // the marker (onedeck.hpp), with the sessions queued above counted as live; its heartbeat every 30 s
  if (d->opt.oneDeck.is_object() && !js::str(js::get(d->opt.oneDeck, "sharedDir")).empty()) {
    d->oneDeckOn = true; d->oneDeckStartedAt = isoTime(d->now());
    putDeckMarker(*d, true);
    // the shared layout's writer; this deck's first write when it is the newest (the shell decided before start():
    // publishAtStart "write", or the start-up question's "kept this deck's own layout" as a real change)
    Impl* impl = d.get();
    d->sharer.reset(new onedeck::Sharer(*d->sm.files, "native", d->stateDir, js::str(js::get(d->opt.oneDeck, "sharedDir")),
      [impl] { return impl->ownLayoutFiles(); }, [impl] { return impl->now(); }, [impl](const std::string& l) { impl->logEvent("onedeck", l.compare(0, 10, "[onedeck] ") == 0 ? l.substr(10) : l); }));
    const std::string pub = js::str(js::get(d->opt.oneDeck, "publishAtStart"));
    if (pub == "write") d->sharer->publish(true);
    else if (!pub.empty()) { d->sharer->noteReal(pub); d->sharer->publish(true); }
    d->setTimer(onedeck::HEARTBEAT_MS, [this] { d->writeDeckMarker(true); d->publishShared(); }, onedeck::HEARTBEAT_MS);
  }
  d->startDeskInbox();                         // after the windows exist: a desk is placed in one
  if (d->sm.phone) d->sm.phone->sync(js::get(d->settings, "phone"));      // syncPhone()
  d->setTimer(META_INTERVAL_MS, [this] { d->pushMeta(); }, META_INTERVAL_MS);
  d->setTimer(ACCOUNT_REFRESH_MS, [this] { d->refreshAccounts(); }, ACCOUNT_REFRESH_MS);
  // the hello, to the windows the shell already made
  const json hello = { { "t", "init" }, { "meta", d->collectMeta() }, { "home", d->home }, { "platform", d->publicPlatform() }, { "build", nullptr }, { "assets", "" } };
  d->toWindows(hello);
}

ClientId Deck::addWindow(const json& bounds) {
  std::unique_ptr<Entry> e(new Entry());
  e->index = d->nextWindowIndex++;
  e->bounds = bounds;
  if (d->started && !d->down && d->hadWindow && d->winEntries().empty()) {
    // main.js `activate` (the Mac: every window was closed, the deck lives on, the Dock icon is clicked): the new
    // window is the state file's first entry as it is now — what the last window to close looked like.
    const json st = d->tilesStore->read();
    const json& ws = js::get(st, "windows");
    const json first = (ws.is_array() && !ws.empty() && js::truthy(ws[0])) ? ws[0] : json{ { "bounds", js::get(st, "bounds") }, { "layout", js::get(st, "layout") } };
    if (js::truthy(js::get(first, "bounds"))) e->bounds = js::get(first, "bounds");
    e->layout = d->cleanLayout(js::get(first, "layout"));
  } else if (d->started && d->savedTaken < d->savedWindows.size()) {
    const json& sw = d->savedWindows[d->savedTaken++];
    if (js::truthy(js::get(sw, "bounds"))) e->bounds = js::get(sw, "bounds");
    e->layout = d->cleanLayout(js::get(sw, "layout"));
  }
  d->hadWindow = true;
  std::unique_ptr<Client> c(new Client());
  c->id = d->nextClient++; c->entry = e.get(); e->client = c->id;
  const ClientId id = c->id;
  d->entries.push_back(std::move(e));
  d->clients.push_back(std::move(c));
  if (d->started && !d->down)
    d->send(id, { { "t", "init" }, { "meta", d->collectMeta() }, { "home", d->home }, { "platform", d->publicPlatform() }, { "build", nullptr }, { "assets", "" } });
  return id;
}
ClientId Deck::nextClientId() const { return d->nextClient; }
ClientId Deck::openWindow(const std::string& tileId, const json& bounds) {
  if (!d->tileById(tileId)) return 0;
  std::unique_ptr<Entry> e(new Entry());
  e->index = d->nextWindowIndex++; e->bounds = bounds;
  d->hadWindow = true;
  std::unique_ptr<Client> c(new Client());
  c->id = d->nextClient++; c->entry = e.get(); e->client = c->id;
  const ClientId id = c->id; Entry* raw = e.get();
  d->entries.push_back(std::move(e));
  d->clients.push_back(std::move(c));
  layout::LayoutMap l; l.emplace_back(tileId, layout::fullSet(d->grid()));
  d->takeLayoutMap(raw, l);                    // evicts the tile from wherever it was, persists, publishes
  if (!d->down) d->send(id, { { "t", "init" }, { "meta", d->collectMeta() }, { "home", d->home }, { "platform", d->publicPlatform() }, { "build", nullptr }, { "assets", "" } });
  return id;
}
void Deck::windowState(ClientId window, bool visible, bool focused) {
  Client* c = d->clientById(window); if (!c || c->remote || !c->entry) return;
  const bool was = c->entry->visible;
  c->entry->visible = visible; c->entry->focused = focused;
  if (visible && !was) { for (const std::string& id : c->entry->shown) { Tile* t = d->tileById(id); if (t && t->s.asleep) d->wakeTile(id, "on screen"); } }
  if (visible != was) d->pushMeta();
}
void Deck::windowBounds(ClientId window, const json& bounds) {
  Client* c = d->clientById(window); if (!c || c->remote || !c->entry) return;
  c->entry->bounds = bounds;
  d->saveWindows();
}
void Deck::windowKey(ClientId window) { Client* c = d->clientById(window); if (c && !c->remote && !d->down) d->sawInput("office"); }
ClientId Deck::addRemote(bool viaDesk) {
  std::unique_ptr<Client> c(new Client());
  c->id = d->nextClient++; c->remote = true; c->viaDesk = viaDesk;
  const ClientId id = c->id;
  d->clients.push_back(std::move(c));
  // lib/phone.js onSocket: the hello carries a whole meta, so the tick right after it need not repeat it
  const json ex = d->extras();
  const json meta = remoteMeta(d->collectMeta(), false);
  json hello = { { "t", "init" }, { "home", d->home }, { "meta", meta }, { "platform", d->publicPlatform() } };
  for (auto it = ex.begin(); it != ex.end(); ++it) hello[it.key()] = it.value();
  if (d->send(id, hello)) if (Client* again = d->clientById(id)) { again->hasMetaKey = true; again->metaKey = metaKey(meta, js::get(ex, "assets"), js::get(ex, "build")); again->metaAt = d->now(); }
  return id;
}
void Deck::removeClient(ClientId client) {
  Client* c = d->clientById(client); if (!c) return;
  const bool remote = c->remote;
  if (remote) d->deskGone(*c);
  else if (c->entry) {
    Entry* e = c->entry;
    const std::vector<Entry*> open = d->winEntries();
    const bool last = open.size() == 1 && open[0] == e;
    // Closing ONE window of several drops it from the file, but it is remembered for a moment: the usual way to quit is
    // to close the windows one by one, and the last one closing would otherwise leave only ITS layout in the file
    // (main.js `closed`, 11bf1ab). A restart closes them all and has saved them already.
    if (!d->restarting && !last && !e->remote) d->closedWins.push_back({ e->index, e->bounds, e->layout, d->now() });
    if (!d->restarting && last) d->keepWindowsAtQuit();
    d->removeEntry(e);
    if (!d->winEntries().empty() && !d->restarting) d->saveWindows();
  }
  d->clients.erase(std::remove_if(d->clients.begin(), d->clients.end(), [&](const std::unique_ptr<Client>& x) { return x->id == client; }), d->clients.end());
  if (!remote) d->pushMeta();
}
int Deck::windowIndex(ClientId client) const { Client* c = d->clientById(client); return c && c->entry ? c->entry->index : -1; }
json Deck::savedWindows() const { return d->savedWindows; }

void Deck::handle(ClientId client, const json& frame) { d->handle(client, frame); }
void Deck::onPtyData(uint64_t token, const char* bytes, size_t n) {
  if (d->down) return;
  Tile* t = d->tileByGen(token);
  if (!t) return;                                              // `if (tile.pty !== p) return`
  const std::string s = t->decoder.write(bytes, n);
  if (s.empty()) return;
  t->replay.push(s);
  t->s.modes.track(s);
  const std::string id = t->s.id;
  const json frame = { { "t", "data" }, { "id", id }, { "d", s } };
  for (size_t i = 0; i < d->clients.size(); i++) {
    Client* c = d->clients[i].get();
    if (c->attachedSet.count(id) || (c->remote && c->hasAttached && c->attached == id)) d->send(c->id, frame);
  }
  if (Tile* again = d->tileByGen(token)) d->watchStartup(*again, token, s);
}
void Deck::onPtyExit(uint64_t token, uint32_t exitCode) {
  if (d->down) return;
  Tile* t = d->tileByGen(token);
  if (t) {
    d->dying.push_back({ std::move(t->pty), token, true, nullptr });
    t->s.running = false; t->s.exited = true; t->s.exitCode = (int)(int32_t)exitCode;
    // main.js onExit: a move that failed at once (userswitch refused) - the tile goes back to the account its session is in
    const std::optional<std::string> mvFrom = t->movePendingFrom; const double mvAt = t->movePendingAt;
    t->movePendingFrom.reset(); t->movePendingAt = 0;
    if (mvFrom && *t->s.exitCode != 0 && d->now() - mvAt < 120000 && accountById(d->accounts, *mvFrom)) { t->s.account = mvFrom; d->saveState(); }
    d->toAll({ { "t", "exit" }, { "id", t->s.id }, { "exitCode", *t->s.exitCode } });
    if (t->s.kind == "cloud") d->cloudCreated(*t);            // `claude --cloud "<task>"` has exited: did it make a session?
    d->pushMeta();
  } else d->finishKill(token);                                 // a process the deck was ending: whoever waited continues
  d->reapDying();
}
void Deck::tick() { if (d->down) return; d->runTimers(); d->reapDying(); }
int Deck::nextDueMs() const {
  double best = 2000; const double at = d->now();
  for (const auto& t : d->timers) best = std::min(best, t.due - at);
  return (int)std::max(0.0, best);
}
void Deck::shutdown() { d->shutdown(); }

int Deck::remoteCount() const { int n = 0; for (const auto& c : d->clients) if (c->remote) n++; return n; }
int Deck::deskCount() const { int n = 0; for (const auto& c : d->clients) if (c->remote && c->desk) n++; return n; }
const json& Deck::settings() const { return d->settings; }
json Deck::collectMeta() { return d->collectMeta(); }
ClaudeLaunch Deck::lastClaudeLaunch(const std::string& tileId) const { Tile* t = d->tileById(tileId); return t ? t->lastLaunch : ClaudeLaunch(); }
std::vector<uint32_t> Deck::livePids() const {
  std::vector<uint32_t> out;
  for (const auto& t : d->tiles) if (t->pty) for (uint32_t p : t->pty->processTree()) out.push_back(p);
  for (const auto& x : d->dying) if (x.pty) for (uint32_t p : x.pty->processTree()) out.push_back(p);
  return out;
}
std::pair<int, int> Deck::ptySize(const std::string& tileId) const { Tile* t = d->tileById(tileId); return t && t->pty ? std::make_pair(t->pty->cols(), t->pty->rows()) : std::make_pair(0, 0); }
const std::string& Deck::stateDir() const { return d->stateDir; }
bool Deck::restoring() const { return d->restoring(d->now()); }

// main.js saveUpload: kept under the deck's data folder, dated, the name made safe; a cap on the folder
std::string Deck::saveUpload(const std::string& name, const std::string& bytes) {
  const std::string dir = joinPath(d->stateDir, "uploads");
  d->sm.files->makeDirs(dir);
  // String(name).replace(/[^\w.\-]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 80) || 'file'
  std::string safe; bool run = false;
  for (unsigned char ch : name) { const bool ok = js::isWordChar(ch) || ch == '.' || ch == '-'; if (ok) { safe += (char)ch; run = false; } else if (!run) { safe += '_'; run = true; } }
  while (!safe.empty() && safe.front() == '_') safe.erase(0, 1);
  while (!safe.empty() && safe.back() == '_') safe.pop_back();
  if (safe.size() > 80) safe.resize(80);
  if (safe.empty()) safe = "file";
  // new Date().toISOString() -> 20261118-031502
  const std::string iso = isoTime(d->now());
  std::string stamp;
  for (char ch : iso.substr(0, 19)) { if (ch == '-' || ch == ':') continue; stamp += ch == 'T' ? '-' : ch; }
  const std::string p = joinPath(dir, stamp + "-" + safe);
  if (!d->sm.files->write(p, bytes)) throw std::runtime_error("could not save the file");
  // the newest 200 stay, and nothing older than 30 days
  std::vector<std::string> names;
  for (const std::string& n : d->sm.files->list(dir)) {
    bool dated = n.size() > 16 && n[8] == '-' && n[15] == '-';
    for (size_t k : { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14 }) if (dated && !(n[k] >= '0' && n[k] <= '9')) dated = false;
    if (dated) names.push_back(n);
  }
  std::sort(names.begin(), names.end());
  const double cutoff = d->now() - 30 * 86400000.0;
  for (size_t i = 0; i < names.size(); i++) {
    const std::string f = joinPath(dir, names[i]);
    const bool old = (names.size() > 200 && i < names.size() - 200) || d->sm.files->stat(f).mtimeMs < cutoff;
    if (old && f != p) d->sm.files->remove(f);
  }
  return p;
}

}}  // namespace deck::core
