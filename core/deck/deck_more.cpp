// core/deck/deck_more.cpp — the rest of main.js on Deck::Impl: triggers (lib/triggers.js), special prompts' wiring
// (lib/queue.js is core/deck/held.cpp), hibernation + the memory guard, restart when quiet + the update offer,
// sessions elsewhere / past, adopt, account moves, and the shell-host scan (lib/shellhost.js).
#include "deck_impl.hpp"

#include <algorithm>
#include <stdexcept>

namespace deck { namespace core {

using D = Deck::Impl;

// ---------------------------------------------------------------- triggers (lib/triggers.js)
std::string cleanTriggerName(const std::string& name) {
  const std::string n = js::trim(name);
  if (n.empty() || n.size() > 64) return std::string();
  for (char c : n) if (!(js::isWordChar((unsigned char)c) || c == '.' || c == '-')) return std::string();
  return n;
}
void D::loadTriggers() {
  triggers.clear();
  std::string text;
  if (!sm.files->read(joinPath(stateDir, "triggers.json"), text)) return;
  const json j = json::parse(text, nullptr, false);
  const json& m = js::get(j, "triggers");
  if (!m.is_object()) return;
  for (auto it = m.begin(); it != m.end(); ++it) {
    const std::string n = cleanTriggerName(it.key());
    if (n.empty() || !it.value().is_object()) continue;
    const double at = js::toNumber(js::get(it.value(), "at"));
    triggers[n] = { js::truthy(js::get(it.value(), "value")), std::isnan(at) ? 0 : at, js::truthy(js::get(it.value(), "by")) ? js::toString(js::get(it.value(), "by")) : std::string() };
  }
}
void D::saveTriggers() {
  json m = json::object();
  for (const auto& kv : triggers) m[kv.first] = { { "value", kv.second.value }, { "at", numJ(kv.second.at) }, { "by", kv.second.by } };
  sm.files->write(joinPath(stateDir, "triggers.json"), json{ { "triggers", m } }.dump(2, ' ', false, json::error_handler_t::replace));
}
void D::triggersChanged() { saveTriggers(); pushMeta(); }
json D::triggerPub(const std::string& name) const {
  auto it = triggers.find(name);
  if (it == triggers.end()) return json();
  return { { "name", name }, { "value", it->second.value }, { "at", numJ(it->second.at) }, { "by", it->second.by } };
}
json D::triggersList() const { json out = json::array(); for (const auto& kv : triggers) out.push_back(triggerPub(kv.first)); return out; }   // std::map: sorted by name
json D::triggerSet(const std::string& name, bool value, const std::string& by) {
  const std::string n = cleanTriggerName(name);
  if (n.empty()) throw std::runtime_error("bad trigger name (letters, digits, . _ - only; 64 max)");
  const auto it = triggers.find(n);
  const TriggerRow prev = it == triggers.end() ? TriggerRow() : it->second;
  const bool rising = value && !prev.value;
  triggers[n] = { value, rising ? now() : prev.at, !by.empty() ? by : prev.by };
  triggersChanged();
  return triggerPub(n);
}
json D::triggerDeclare(const std::string& name, const std::string& by) {
  const std::string n = cleanTriggerName(name);
  if (n.empty()) throw std::runtime_error("bad trigger name");
  if (triggers.find(n) == triggers.end()) { triggers[n] = { false, 0, by }; triggersChanged(); }
  return triggerPub(n);
}
bool D::triggerRemove(const std::string& name) {
  const std::string n = cleanTriggerName(name);
  if (n.empty() || triggers.find(n) == triggers.end()) return false;
  triggers.erase(n); triggersChanged();
  return true;
}
double D::triggerRisenAt(const std::string& name) const {
  const auto it = triggers.find(cleanTriggerName(name));
  return it != triggers.end() && it->second.value ? it->second.at : 0;
}
// The inbox: a session (the deck-triggers skill) drops a file naming a trigger; the deck sets it and deletes
// the file. Polled every 4 s (main.js also watches the folder; the poll is its own belt-and-braces).
void D::drainTriggerInbox() {
  const std::string dir = joinPath(stateDir, "trigger-inbox");
  for (const std::string& f : sm.files->list(dir)) {
    const std::string p = joinPath(dir, f);
    std::string raw;
    if (sm.files->read(p, raw)) {
      raw = js::trim(raw);
      json name; bool value = true; std::string by = "session"; bool ok = true;
      if (!raw.empty() && raw[0] == '{') {
        const json j = json::parse(raw, nullptr, false);
        if (j.is_discarded()) ok = false;
        else { name = js::get(j, "name"); if (j.is_object() && j.contains("value")) value = js::truthy(j["value"]); if (js::truthy(js::get(j, "by"))) by = js::toString(js::get(j, "by")); }
      } else name = raw;
      if (ok && js::truthy(name) && !cleanTriggerName(js::toString(name)).empty()) { try { triggerSet(js::toString(name), value, by); } catch (...) { /* malformed drop */ } }
    }
    sm.files->remove(p);
  }
}

// ---------------------------------------------------------------- the periodic machinery (main.js whenReady)
void D::initMore() {
  loadTriggers();
  sm.files->makeDirs(joinPath(stateDir, "trigger-inbox"));
  HeldDeps hd;
  hd.now = [this] { return now(); };
  hd.minutesOfDay = [this](double at) {
    const double local = at + sm.clock->localOffsetMs(at);
    const double dayMs = std::fmod(std::fmod(local, 86400000.0) + 86400000.0, 86400000.0);
    return (int)(dayMs / 60000.0);
  };
  hd.uuid = [this] { return sm.random->uuid(); };
  const std::string heldFile = joinPath(stateDir, "held-prompts.json");
  hd.load = [this, heldFile](std::string& out) { return sm.files->read(heldFile, out); };
  hd.save = [this, heldFile](const std::string& text) { sm.files->write(heldFile, text); };
  hd.tile = [this](const std::string& id) {
    HeldTile h; Tile* t = tileById(id);
    if (!t) return h;
    h.exists = true; h.kind = t->s.kind; h.sessionId = optJ(t->s.sessionId); h.exited = t->s.exited; h.hasPty = !!t->pty;
    h.status = t->pty ? (t->s.asleep ? "asleep" : (t->s.lastStatus && !t->s.lastStatus->empty() ? *t->s.lastStatus : "starting")) : "off";
    return h;
  };
  hd.recentOutput = [this](const std::string& id) { Tile* t = tileById(id); return t ? t->replay.all() : std::string(); };
  hd.write = [this](const std::string& id, const std::string& data) { Tile* t = tileById(id); if (t && t->pty) t->pty->write(data); };
  hd.noteInput = [this](const std::string& id) { if (Tile* t = tileById(id)) { t->s.hadInput = true; t->s.lastInputFrom = "deck"; t->s.lastInputAt = now(); } };
  hd.triggerRisenAt = [this](const std::string& name) { return triggerRisenAt(name); };
  hd.onChange = [this] { pushMeta(); };
  hd.later = [this](double ms, std::function<void()> fn) { setTimer(ms, std::move(fn)); };
  held.reset(new HeldPrompts(std::move(hd)));

  if (sm.http && sm.credentials && opt.usagePolling) {
    usage.reset(new UsagePoller(*sm.clock, *sm.files, sm.http, sm.credentials, joinPath(stateDir, "usage-cache.json")));
    auto poll = [this] { if (usageLive()) usage->pollAll(accounts, false, [this] { pushMeta(); }); };
    setTimer(0, poll);
    setTimer(USAGE_TICK_MS, poll, USAGE_TICK_MS);
  }
  if (sm.lifecycle) bootPrint = sm.lifecycle->fingerprint();
  setTimer(0, [this] { drainTriggerInbox(); });                 // anything dropped while the deck was down
  setTimer(TRIGGER_INBOX_MS, [this] { drainTriggerInbox(); }, TRIGGER_INBOX_MS);
  setTimer(HELD_TICK_MS, [this] { held->tick(); }, HELD_TICK_MS);
  setTimer(HIBERNATE_TICK_MS, [this] { hibernateTick(); }, HIBERNATE_TICK_MS);
  setTimer(QUIET_TICK_MS, [this] { quietTick(); }, QUIET_TICK_MS);
  setTimer(PAUSE_TICK_MS, [this] { pauseTick(); }, PAUSE_TICK_MS);
  setTimer(AUDITOR_TICK_MS, [this] { auditorTick(); }, AUDITOR_TICK_MS);
  setTimer(SHELL_HOST_TICK_MS, [this] { scanShellHosted(); }, SHELL_HOST_TICK_MS);
  setTimer(UPDATE_CHECK_MS, [this] { checkUpdate(); }, UPDATE_CHECK_MS);
}

// ---------------------------------------------------------------- hibernation (main.js hibernateView / hibernateTick / memoryGuard)
D::HView D::hibernateView(Tile& t, double at) {
  HView v;
  v.id = t.s.id; v.kind = t.s.kind; v.hasSession = t.s.sessionId && !t.s.sessionId->empty();
  v.status = t.s.asleep ? "asleep" : t.s.waking ? "starting" : (t.pty ? (t.s.kind == "shell" ? "shell" : (t.s.lastStatus && !t.s.lastStatus->empty() ? *t.s.lastStatus : "starting")) : "off");
  v.running = !!t.pty; v.resumable = isResumable(t, at); v.asleep = t.s.asleep; v.pinned = t.s.pinned;
  v.shownBy = shownBy(t);
  v.held = held ? (int)held->countFor(t.s.id) : 0;
  v.background = (t.pty && t.s.kind == "claude" && t.tail) ? (int)t.tail->background(t.s.startedAt, at).size() : 0;
  v.lastActivityAt = std::max(std::max(t.s.startedAt, t.s.lastStoppedAt), std::max(t.s.lastInputAt, t.s.busySince));
  return v;
}
// lib/hibernate.js sleepBlocker: may this tile sleep? (the reason strings are core/session/meta.cpp's; here only yes / no)
static bool maySleep(const D::HView& t, double now, const session::HibernateConfig& cfg) {
  if (!cfg.enabled) return false;
  if (t.kind != "claude" || !t.hasSession) return false;
  if (!t.resumable) return false;
  if (t.asleep || !t.running) return false;
  if (cfg.keepPinned && t.pinned) return false;
  if (!t.shownBy.empty()) return false;
  if (t.held > 0) return false;
  if (t.status != "idle") return false;
  if (t.background > 0) return false;
  return (now - t.lastActivityAt) >= cfg.idleMinutes * 60000.0;
}
void D::hibernateTick() {
  const double at = now();
  if (restoring(at)) return;
  std::vector<HView> views;
  for (auto& t : tiles) views.push_back(hibernateView(*t, at));
  // wakes go first: a tile that is wanted must never wait a tick behind a sleep decision
  for (const HView& v : views) {
    if (!v.asleep) continue;
    if (!pressureOn && !v.shownBy.empty()) wakeTile(v.id, "on screen");
    else if (v.held > 0) wakeTile(v.id, "a prompt is waiting for it");
  }
  const session::HibernateConfig cfg = session::hibernateConfig(js::get(settings, "hibernate"));
  if (cfg.enabled) {
    std::vector<const HView*> cand;
    for (const HView& v : views) if (maySleep(v, at, cfg)) cand.push_back(&v);
    std::stable_sort(cand.begin(), cand.end(), [](const HView* a, const HView* b) { return a->lastActivityAt < b->lastActivityAt; });
    for (size_t i = 0; i < cand.size() && (int)i < cfg.maxPerTick; i++) sleepTile(cand[i]->id, "idle", [](bool) {});
  }
  // the memory guard: at 90 % of the commit limit every session that MAY sleep sleeps now
  double used = 0, limit = 0;
  if (!sm.procs->commit(used, limit) || !(limit > 0)) return;
  const double pressure = used / limit;
  pressureOn = pressureOn ? !(pressure < 0.85) : pressure >= 0.9;
  if (!(pressure >= 0.9)) return;
  session::HibernateConfig c = cfg; c.enabled = true; c.idleMinutes = 0; c.keepPinned = false;
  std::vector<const HView*> picks;
  std::vector<HView> bare = views;
  for (HView& v : bare) v.shownBy.clear();
  for (const HView& v : bare) if (maySleep(v, at, c)) picks.push_back(&v);
  if (picks.empty()) return;
  std::stable_sort(picks.begin(), picks.end(), [](const HView* a, const HView* b) { return a->lastActivityAt < b->lastActivityAt; });
  for (const HView* p : picks) sleepTile(p->id, "memory", [](bool) {});
  if (at - pressureNotifiedAt > 5 * 60000.0 && sm.notifier) {
    pressureNotifiedAt = at;
    const int pct = (int)js::round(pressure * 100);
    auto gb = [](double x) { char b[32]; std::snprintf(b, sizeof b, "%.1f", x / 1073741824.0); return std::string(b); };
    Toast toast; toast.kind = "memory"; toast.title = std::string(edition::kProductName) + ": memory is nearly full";
    toast.body = "Committed memory is at " + std::to_string(pct) + "% (" + gb(used) + " of " + gb(limit) + " GB). Put " + std::to_string(picks.size()) + " idle session" + (picks.size() == 1 ? "" : "s")
      + " to sleep; they resume when you open them. Close something big (Chrome?) if it keeps happening.";
    sm.notifier->toast(toast);
  }
}

// ---------------------------------------------------------------- restart when quiet (main.js quietState / quietTick / restartDeck / checkUpdate)
std::vector<session::QuietView> D::quietViews(double at) {
  std::vector<session::QuietView> out;
  for (auto& t : tiles) { t->s.running = !!t->pty; out.push_back(session::quietView(t->s, t->meta, t->tail.get(), at)); }
  return out;
}
json D::restartView(double at) {
  const std::vector<session::QuietView> views = quietViews(at);
  json v = quiet.view(session::restartBlockers(views, at), at);
  v["shells"] = session::shellsLost(views);
  // offered until it is dismissed ("Later") or taken — a later change to the files offers again
  v["update"] = hasUpdate && updateSig != dismissedUpdate ? json{ { "files", updateFiles }, { "since", numJ(updateSince) } } : json();
  return v;
}
bool D::setQuiet(bool on, const std::string& why) {
  if (on) quiet.arm(why, now()); else quiet.cancel();
  pushMeta();
  return quiet.armed;
}
void D::quietTick() {
  if (!quiet.armed || restarting) return;
  const double at = now();
  if (restoring(at)) return;                   // the restore is still spawning: never restart into it
  if (quiet.step(!session::restartBlockers(quietViews(at), at).empty(), at) != "go") return;
  // one fresh look before acting: re-read every live status and transcript, then decide again
  pushMeta();
  const double again = now();
  if (!session::restartBlockers(quietViews(again), again).empty()) { quiet.step(true, again); return; }
  const std::string why = quiet.why == "update" ? "quiet, for the update" : "quiet";
  if (!restartDeck(why)) quiet.cancel();       // a shell that cannot relaunch: stop asking every two seconds
}
bool D::restartDeck(const std::string& why) {
  if (restarting) return false;
  if (!sm.lifecycle) return false;
  restarting = true;
  quiet.cancel();
  saveState({ { "windows", windowsState() } });
  if (!sm.lifecycle->restart(why)) { restarting = false; return false; }
  armExitWatchdog();                           // main.js restartDeck: exitWatchdog() before app.quit()
  return true;
}
void D::checkUpdate() {
  if (!sm.lifecycle || !bootPrint.is_object()) return;
  const json cur = sm.lifecycle->fingerprint();
  if (!cur.is_object()) return;
  std::set<std::string> names;
  for (auto it = bootPrint.begin(); it != bootPrint.end(); ++it) names.insert(it.key());
  for (auto it = cur.begin(); it != cur.end(); ++it) names.insert(it.key());
  json changed = json::array(); std::string sig;
  for (const std::string& f : names) {
    if (js::get(bootPrint, f.c_str()) == js::get(cur, f.c_str())) continue;
    changed.push_back(f);
    if (!sig.empty()) sig += "|";
    sig += f + ":" + js::toString(js::get(cur, f.c_str()));
  }
  const std::string before = hasUpdate ? updateSig : std::string();
  if (!changed.empty()) { if (!(hasUpdate && updateSig == sig)) updateSince = now(); hasUpdate = true; updateFiles = changed; updateSig = sig; }
  else { hasUpdate = false; updateSig.clear(); }
  if ((hasUpdate ? updateSig : std::string()) != before) pushMeta();
}
void D::dismissUpdate() { if (hasUpdate) dismissedUpdate = updateSig; pushMeta(); }

// ---------------------------------------------------------------- sessions elsewhere / past (main.js externalSessions / pastSessions)
// the last `"<key>":"<json string>"` after `prefix` in text; null when none. A value that is not a JSON string
// literal makes the whole lookup null, as the JS's JSON.parse throws out of quickTitle.
static bool stringAfter(const std::string& text, const std::string& prefix, bool last, json& out, bool& bad) {
  size_t at = last ? text.rfind(prefix) : text.find(prefix);
  // the regex needs the closing quote too: walk back / on until a match that closes
  while (at != std::string::npos) {
    size_t j = at + prefix.size(); bool closed = false;
    while (j < text.size()) { if (text[j] == '\\') { if (j + 1 >= text.size() || text[j + 1] == '\n') break; j += 2; continue; } if (text[j] == '"') { closed = true; break; } j++; }
    if (closed) {
      const json v = json::parse("\"" + text.substr(at + prefix.size(), j - at - prefix.size()) + "\"", nullptr, false);
      if (v.is_discarded()) { bad = true; return false; }
      out = v; return true;
    }
    if (last) { if (at == 0) break; at = text.rfind(prefix, at - 1); } else at = text.find(prefix, at + 1);
  }
  return false;
}
static json quickTitle(Files& files, const std::string& file) {
  const FileStat st = files.stat(file);
  if (!st.exists || st.isDir) return json();
  const uint64_t tailLen = std::min<uint64_t>(st.size, 262144);
  std::string tail, head;
  if (!files.readFrom(file, st.size - tailLen, tail, nullptr)) return json();
  if (tail.size() > tailLen) tail.resize((size_t)tailLen);
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) { if (!files.readRange(file, 0, std::min<uint64_t>(st.size, 262144), head)) return json(); }
    const std::string& text = pass == 0 ? tail : head;
    json v; bool bad = false;
    if (stringAfter(text, "\"type\":\"custom-title\",\"customTitle\":\"", true, v, bad)) return v;
    if (bad) return json();
    if (stringAfter(text, "\"type\":\"ai-title\",\"aiTitle\":\"", true, v, bad)) return v;
    if (bad) return json();
  }
  return json();
}
static json quickCwd(Files& files, const std::string& file) {
  std::string head;
  if (!files.readRange(file, 0, 8192, head)) return json();
  json v; bool bad = false;
  return stringAfter(head, "\"cwd\":\"", false, v, bad) ? v : json();
}

json D::externalSessions() {
  std::set<std::string> hosted;
  for (const auto& t : tiles) if (t->s.sessionId && !t->s.sessionId->empty()) hosted.insert(*t->s.sessionId);
  std::vector<json> out;
  for (const Account& a : accounts) {
    const std::string dir = joinPath(a.dir, "sessions");
    for (const std::string& n : sm.files->list(dir)) {
      if (n.size() < 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
      std::string text;
      if (!sm.files->read(joinPath(dir, n), text)) continue;
      const json j = json::parse(text, nullptr, false);
      if (!js::truthy(j) || !js::truthy(js::get(j, "sessionId")) || !js::truthy(js::get(j, "pid"))) continue;
      const std::string sid = js::toString(js::get(j, "sessionId"));
      if (hosted.count(sid)) continue;
      if (!js::get(j, "pid").is_number() || !sm.procs->alive(js::get(j, "pid").get<double>())) continue;
      if (js::truthy(js::get(j, "kind")) && js::get(j, "kind") != "interactive") continue;
      const std::string f = findTranscript(a.dir, sid);
      const json& su = js::get(j, "statusUpdatedAt"); const json& up = js::get(j, "updatedAt");
      out.push_back({ { "sessionId", js::get(j, "sessionId") }, { "account", a.id }, { "label", a.label }, { "pid", js::get(j, "pid") },
        { "status", js::truthy(js::get(j, "status")) ? js::get(j, "status") : json("idle") }, { "statusUpdatedAt", js::truthy(su) ? su : (js::truthy(up) ? up : json(0)) },
        { "cwd", js::truthy(js::get(j, "cwd")) ? js::get(j, "cwd") : json() }, { "name", js::truthy(js::get(j, "name")) ? js::get(j, "name") : json() },
        { "title", f.empty() ? json() : quickTitle(*sm.files, f) } });
    }
  }
  // Cloud sessions of every account that no tile holds (lib/cloud.js lists them once a minute). Opening one here
  // touches nothing: it keeps running in the cloud, and the tile is its page.
  if (cloud && cloudLive()) {   // (the public build: only with its opt-in on; off, nothing was fetched to list)
    std::set<std::string> hostedCloud;
    for (const auto& t : tiles) if (t->s.cloudId && !t->s.cloudId->empty()) hostedCloud.insert(cloudKey(*t->s.cloudId));
    const json rows = cloud->elsewhere(hostedCloud);
    if (rows.is_array()) for (const json& v : rows) {
      const Account* a = accountById(accounts, js::str(js::get(v, "account"))); if (!a) continue;
      out.push_back({ { "cloud", true }, { "cloudId", js::get(v, "id") }, { "account", a->id }, { "label", a->label }, { "pid", "cloud" }, { "status", js::get(v, "status") },
        { "statusUpdatedAt", js::truthy(js::get(v, "lastAt")) ? js::get(v, "lastAt") : json(0) }, { "cwd", nullptr }, { "name", "cloud session" }, { "title", js::get(v, "title") } });
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const json& x, const json& y) { return js::toNumber(js::get(y, "statusUpdatedAt")) < js::toNumber(js::get(x, "statusUpdatedAt")); });
  return out;
}
json D::pastSessions(size_t limit) {
  std::set<std::string> hosted;
  for (const auto& t : tiles) if (t->s.sessionId && !t->s.sessionId->empty()) hosted.insert(*t->s.sessionId);
  const session::LiveMap live = readLive();
  struct Row { json o; std::string file; double mtime; };
  std::vector<Row> out;
  for (const Account& a : accounts) {
    const std::string projects = joinPath(a.dir, "projects");
    for (const std::string& slug : sm.files->list(projects)) {
      const std::string d = joinPath(projects, slug);
      for (const std::string& f : sm.files->list(d)) {
        if (f.size() < 6 || f.compare(f.size() - 6, 6, ".jsonl") != 0) continue;
        const std::string sid = f.substr(0, f.size() - 6);
        if (hosted.count(sid)) continue;
        const session::LiveRecord* l = live.get(sid);
        if (l && l->alive) continue;
        const FileStat st = sm.files->stat(joinPath(d, f));
        if (!st.exists || st.isDir) continue;
        if (st.size < 2000) continue;                       // never got a real message
        out.push_back({ { { "sessionId", sid }, { "account", a.id }, { "label", a.label }, { "mtime", st.mtimeMs }, { "size", st.size } }, joinPath(d, f), st.mtimeMs });
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const Row& x, const Row& y) { return y.mtime < x.mtime; });
  json top = json::array();
  for (size_t i = 0; i < out.size() && i < limit; i++) {
    json o = out[i].o;
    o["title"] = quickTitle(*sm.files, out[i].file); o["cwd"] = quickCwd(*sm.files, out[i].file);
    top.push_back(o);
  }
  return top;
}

// main.js adoptSession: a Claude session running elsewhere is ended there and resumed in a tile here.
void D::adoptSession(const json& s, std::function<void(const json&)> ok, std::function<void(const std::string&)> err) {
  if (js::truthy(js::get(s, "cloudId"))) { ok(openCloud(s)); return; }   // a cloud session from Elsewhere: nothing is killed or moved
  const std::string account = js::truthy(js::get(s, "account")) ? js::toString(js::get(s, "account")) : std::string("undefined");
  const Account* acct = accountById(accounts, account);
  if (!acct) throw std::runtime_error("unknown account: " + account);
  const std::string sessionId = js::truthy(js::get(s, "sessionId")) ? js::toString(js::get(s, "sessionId")) : std::string();
  for (const auto& t : tiles) if (t->s.sessionId && *t->s.sessionId == sessionId) throw std::runtime_error("already open here");
  // NOT in main.js: adopting ENDS the session's process where it runs. A deck that may not start Claude would
  // end it and then fail to bring it back, so it refuses before touching anything.
  if (!opt.allowClaude) throw std::runtime_error("starting Claude sessions is switched off in the native local deck (allowClaude)");
  const double pid = js::get(s, "pid").is_number() ? js::get(s, "pid").get<double>() : 0;
  if (pid > 0) sm.procs->killTree((uint32_t)pid);
  SpawnSpec sp; sp.kind = "claude"; sp.account = acct->id; sp.sessionId = sessionId;
  if (js::truthy(js::get(s, "cwd"))) sp.cwd = js::toString(js::get(s, "cwd"));
  // wait for the process to be gone so the resume does not race the dying copy (25 looks, 200 ms apart)
  std::shared_ptr<std::function<void(int)>> step = std::make_shared<std::function<void(int)>>();
  std::weak_ptr<std::function<void(int)>> weak = step;
  *step = [this, pid, sp, ok, err, weak](int i) {
    if (i < 25 && pid > 0 && sm.procs->alive(pid)) { auto keep = weak.lock(); setTimer(200, [keep, i] { if (keep) (*keep)(i + 1); }); return; }
    try { ok(spawnTile(sp, nullptr)); } catch (const std::exception& e) { err(e.what()); }
  };
  (*step)(0);
}

// "Bring it here" on a tile the same-session guard left open in a terminal (main.js bringHere): Elsewhere's adopt for
// that tile - the other process is ended (the user's click asked for exactly that), then the session resumes here.
// Never for the other deck's sessions.
void D::bringHere(const std::string& id, std::function<void(bool)> done) {
  Tile* t = tileById(id);
  if (!t || t->pty || t->elsewhere != "terminal" || !(t->elsewherePid > 0)) { done(false); return; }
  const double pid = t->elsewherePid;
  sm.procs->killTree((uint32_t)pid);
  std::shared_ptr<std::function<void(int)>> step = std::make_shared<std::function<void(int)>>();
  std::weak_ptr<std::function<void(int)>> weak = step;
  *step = [this, pid, id, done, weak](int i) {
    if (i < 25 && sm.procs->alive(pid)) { auto keep = weak.lock(); setTimer(200, [keep, i] { if (keep) (*keep)(i + 1); }); return; }
    restartTile(id); done(true);
  };
  (*step)(0);
}

// ---------------------------------------------------------------- account moves (main.js moveTile / moveAll)
void D::moveTile(const std::string& id, const std::string& targetId, const json& opts, std::function<void()> ok, std::function<void(const std::string&)> err) {
  Tile* tile = tileById(id);
  if (!tile) throw std::runtime_error("no such tile");
  if (tile->s.kind != "claude" || !tile->s.sessionId || tile->s.sessionId->empty()) throw std::runtime_error("only Claude sessions can be moved");
  const Account* target = accountById(accounts, targetId);
  if (!target) throw std::runtime_error("unknown account: " + targetId);
  if (tile->s.account && target->id == *tile->s.account) throw std::runtime_error("the session is already in that account");
  const std::optional<std::string> from = tile->s.account;
  const std::string targetAcct = target->id;
  // main.js moveTile: a session that never got a message has no transcript in ANY account - nothing to move
  // (userswitch refuses it); it starts afresh on the new account under the same id, as the plain spawn does
  bool conversation = false;
  for (const Account& a : accounts) if (!findTranscript(a.dir, *tile->s.sessionId).empty()) { conversation = true; break; }
  killPty(*tile, [this, id, from, targetAcct, opts, ok, err, conversation] {
    Tile* t = tileById(id);
    if (!t) { ok(); return; }
    t->tail.reset();
    resetTile(id);
    SpawnSpec sp = respawnSpec(*t, opts);
    sp.kind = "claude"; sp.account = targetAcct; sp.sessionId = t->s.sessionId; sp.cwd = t->s.cwd;
    if (conversation) { sp.moveFrom = from; t->movePendingFrom = from; t->movePendingAt = now(); }
    else sp.moveFrom.reset();
    try { spawnTile(sp, t); ok(); } catch (const std::exception& e) { err(e.what()); }
  });
}
// /spend limit|usage limit|rate limit|limit resets|\/usage-credits|hit your (org'?s )?(monthly|weekly|session)|out of (usage|credits)|usage.*reset/i
bool usageNotice(const std::string& textIn) {
  const std::string s = lowerAscii(textIn);
  for (const char* w : { "spend limit", "usage limit", "rate limit", "limit resets", "/usage-credits", "out of usage", "out of credits" }) if (s.find(w) != std::string::npos) return true;
  for (size_t p = s.find("hit your "); p != std::string::npos; p = s.find("hit your ", p + 1)) {
    size_t i = p + 9;
    if (s.compare(i, 6, "org's ") == 0) i += 6; else if (s.compare(i, 5, "orgs ") == 0) i += 5;
    if (s.compare(i, 7, "monthly") == 0 || s.compare(i, 6, "weekly") == 0 || s.compare(i, 7, "session") == 0) return true;
  }
  for (size_t p = s.find("usage"); p != std::string::npos; p = s.find("usage", p + 1)) {
    const size_t nl = s.find_first_of("\n\r", p);
    const size_t r = s.find("reset", p + 5);
    if (r != std::string::npos && (nl == std::string::npos || r < nl)) return true;
  }
  return false;
}
void D::moveAll(const std::string& fromId, const std::string& toId, bool continueIfUsage, const json& onlyIds, std::function<void(const json&)> ok, std::function<void(const std::string&)> /*err*/) {
  // the public build has no move-all and no "continue" after a usage notice (core/deck/edition.hpp): one tile at a time, by hand
  if (edition::kPublic) throw std::runtime_error("moving every session at once is not in this build: move each tile from its menu");
  const Account* from = accountById(accounts, fromId); const Account* to = accountById(accounts, toId);
  if (!from || !to) throw std::runtime_error("unknown account");
  if (from->id == to->id) throw std::runtime_error("same account");
  struct Target { std::string id, sessionId; bool usage; };
  std::shared_ptr<std::vector<Target>> targets = std::make_shared<std::vector<Target>>();
  for (const auto& t : tiles) {
    if (t->s.kind != "claude" || !t->s.sessionId || t->s.sessionId->empty() || !t->s.account || *t->s.account != from->id) continue;
    if (onlyIds.is_array() && std::find(onlyIds.begin(), onlyIds.end(), json(t->s.id)) == onlyIds.end()) continue;
    const session::TailFacts* f = t->tail ? &t->tail->facts() : nullptr;
    targets->push_back({ t->s.id, *t->s.sessionId, f && f->lastText && !f->lastText->empty() && usageNotice(*f->lastText) });
  }
  const std::string toAcct = to->id;
  std::shared_ptr<json> errors = std::make_shared<json>(json::array());
  std::shared_ptr<std::function<void(size_t)>> step = std::make_shared<std::function<void(size_t)>>();
  std::weak_ptr<std::function<void(size_t)>> weak = step;
  *step = [this, targets, errors, toAcct, continueIfUsage, ok, weak](size_t i) {
    if (i >= targets->size()) {
      int usageSessions = 0; for (const Target& t : *targets) if (t.usage) usageSessions++;
      ok({ { "moved", (int)targets->size() - (int)errors->size() }, { "errors", *errors }, { "usageSessions", usageSessions } });
      return;
    }
    const Target tg = (*targets)[i];
    std::shared_ptr<std::function<void(size_t)>> self = weak.lock();
    auto next = [this, self, i] { setTimer(300, [self, i] { if (self) (*self)(i + 1); }); };
    auto bad = [errors, tg, next](const std::string& e) { errors->push_back(tg.id.substr(0, 8) + ": " + e); next(); };
    try {
      moveTile(tg.id, toAcct, json(), [this, tg, continueIfUsage, next] { if (continueIfUsage && tg.usage) sayContinueWhenReady(tg.id, tg.sessionId, 0); next(); }, bad);
    } catch (const std::exception& e) { bad(e.what()); }
  };
  // the chain is kept alive by the timers and callbacks it hands itself to
  std::shared_ptr<std::function<void(size_t)>> keep = step;
  setTimer(0, [keep] { (*keep)(0); });
}
// Wait (up to 90 s) for the tile's new process to register as a live idle session, then type "continue".
void D::sayContinueWhenReady(const std::string& tileId, const std::string& sessionId, int i) {
  if (i >= 90) return;
  setTimer(1000, [this, tileId, sessionId, i] {
    Tile* t = tileById(tileId);
    if (!t || !t->pty || !t->s.sessionId || *t->s.sessionId != sessionId) return;
    const session::LiveMap live = readLive();
    const session::LiveRecord* l = live.get(sessionId);
    if (l && l->alive && l->statusText() == "idle" && l->statusUpdatedAt > t->s.startedAt) {
      setTimer(1200, [this, tileId] {
        Tile* tile = tileById(tileId);
        if (!tile || !tile->pty) return;
        tile->pty->write("continue");
        setTimer(250, [this, tileId] { Tile* again = tileById(tileId); if (again && again->pty) again->pty->write("\r"); });
      });
      return;
    }
    sayContinueWhenReady(tileId, sessionId, i + 1);
  });
}

// ---------------------------------------------------------------- refresh every session once the restore is done (main.js)
// A resumed Claude Code draws over the screen the deck restored for it until the tile is refreshed; restorerefresh.hpp
// decides when each restored tile gets its one refresh (refreshTile), one at a time, never mid-turn.
bool D::readyAfterStart(const Tile& t, const session::LiveMap& live) const {
  if (!t.pty || !t.s.sessionId) return false;
  const session::LiveRecord* l = live.get(*t.s.sessionId);
  return l && l->alive && l->statusText() == "idle" && l->statusUpdatedAt > t.s.startedAt;
}
void D::startRestoreRefresh(const std::vector<std::string>& ids) {
  if (ids.empty() || js::get(settings, "refreshAfterRestore") == json(false)) { restoreRefresh.start({}, now()); return; }
  restoreRefresh.start(ids, now());
  restoreRefreshTimer = setTimer(RESTORE_REFRESH_TICK_MS, [this] { restoreRefreshTick(); }, RESTORE_REFRESH_TICK_MS);
}
void D::restoreRefreshTick() {
  if (restarting) return;
  const double at = now();
  const session::LiveMap live = readLive();
  std::vector<RestoreRefresh::View> views;
  for (auto& t : tiles) {
    RestoreRefresh::View v;
    v.id = t->s.id; v.kind = t->s.kind; v.asleep = t->s.asleep; v.exited = t->s.exited; v.running = !!t->pty; v.startedAt = t->s.startedAt;
    v.ready = readyAfterStart(*t, live) && !pauses.talking(t->s.id); v.lastKeyAt = t->s.lastKeyAt;
    v.background = (t->pty && t->s.kind == "claude" && t->tail) ? (int)t->tail->background(t->s.startedAt, at).size() : 0;
    views.push_back(v);
  }
  const std::string id = restoreRefresh.step(views, at, restoring(at));
  if (!id.empty()) { logEvent("restore-refresh", "refreshing " + id8(id) + " (" + std::to_string(restoreRefresh.pending.size()) + " to go)"); refreshTile(id, json(), [] {}); }
  if (restoreRefresh.phase == "done" && restoreRefreshTimer) { logEvent("restore-refresh", "done: " + std::to_string(restoreRefresh.refreshed()) + " refreshed"); clearTimer(restoreRefreshTimer); restoreRefreshTimer = 0; }
}

// ---------------------------------------------------------------- Pause / Resume / Pause until restart (main.js; pause.hpp)
// The pause file is <stateDir>/pause/<session id>.md: the deck only names it, the session writes it. Resume never
// interrupts a turn and waits for the restore refresh (restorerefresh.hpp settled).
void D::typePrompt(const std::string& id, const std::string& text, bool esc) {
  Tile* t = tileById(id);
  if (!t || !t->pty) return;
  const uint64_t gen = t->gen;
  auto same = [this, id, gen]() -> Tile* { Tile* x = tileById(id); return x && x->pty && x->gen == gen ? x : nullptr; };
  auto paste = [this, id, text, same] {
    Tile* a = same(); if (!a) return;
    a->pty->write("\x1b[200~" + text + "\x1b[201~");
    setTimer(150, [this, id, same] {
      Tile* b = same(); if (!b) return;
      b->pty->write("\r");
      b->s.lastInputFrom = "deck"; b->s.lastInputAt = now();
      // Enter once more if it did not turn busy (a paste still being read)
      setTimer(2500, [this, same] {
        Tile* c = same(); if (!c || !c->s.sessionId) return;
        const session::LiveMap live = readLive();
        const session::LiveRecord* l = live.get(*c->s.sessionId);
        const std::string st = l && l->alive ? l->statusText() : std::string();
        if (st != "busy" && st != "waiting") c->pty->write("\r");
      });
    });
  };
  if (esc) { t->pty->write("\x1b"); setTimer(800, paste); } else paste();
}
bool D::pauseTile(const std::string& id, bool untilRestart, std::string& why) {
  Tile* t = tileById(id);
  if (!t || !pausable(*t)) { why = "only a running Claude session can be paused"; return false; }
  sm.files->makeDirs(joinPath(stateDir, "pause"));
  const bool hasText = t->tail && t->tail->facts().lastText;
  const std::string file = pauseFileOf(*t->s.sessionId);   // kept with the pause (Resume names it); a /clear ends the pause
  pauses.pause(id, now(), untilRestart, hasText, hasText ? *t->tail->facts().lastText : std::string(), file, *t->s.sessionId);   // a /clear (a new session id) ends it
  saveState(); pushMeta();
  logEvent("pause", id8(id) + (untilRestart ? " until restart" : "") + ": PAUSE sent (" + file + ")");
  typePrompt(id, pauseFill(pausePromptText(), file, ""), true);
  return true;
}
bool D::resumeTile(const std::string& id, std::string& why) {
  if (!pauses.resume(id)) { why = "the deck did not pause this session"; return false; }
  logEvent("pause", id8(id) + ": Resume asked (sent once the session is idle)");
  Tile* t = tileById(id);
  if (t && t->s.asleep) wakeTile(id, "resume");
  saveState(); pushMeta(); pauseTick();
  return true;
}
// every running Claude session not paused already (each its own Esc + prompt; the timers keep them apart)
int D::pauseAll(bool untilRestart) {
  std::vector<std::string> ids;
  for (auto& t : tiles) if (pausable(*t) && !pauses.canResume(t->s.id)) ids.push_back(t->s.id);
  for (const std::string& id : ids) { std::string why; pauseTile(id, untilRestart, why); }
  return (int)ids.size();
}
int D::resumeAll() {
  int n = 0;
  std::vector<std::string> ids; for (auto& t : tiles) if (pauses.canResume(t->s.id)) ids.push_back(t->s.id);
  for (const std::string& id : ids) { std::string why; if (resumeTile(id, why)) n++; }
  return n;
}
void D::pauseTick() {
  // a pause change made while the restore was running (saveState skips those): saved once it is over
  if (pauseUnsaved && !restoring(now()) && !restarting) { pauseUnsaved = false; saveState(); }
  // nothing paused and no session whose newest prompt is a PAUSE (pasted or typed by hand: pause.hpp): nothing to do
  if (restarting) return;
  if (pauses.m.empty()) { bool any = false; for (auto& t : tiles) if (t->tail && t->tail->facts().lastPromptKind && *t->tail->facts().lastPromptKind == "pause") any = true; if (!any) return; }
  // a Resume waiting for a session that is asleep (hibernated while paused): wake it, so the Resume can go
  if (!restoring(now())) {
    std::vector<std::string> wake;
    for (auto& t : tiles) { auto it = pauses.m.find(t->s.id); if (it != pauses.m.end() && it->second.want && t->s.asleep && !t->s.waking) wake.push_back(t->s.id); }
    for (const std::string& id : wake) wakeTile(id, "resume");
  }
  const session::LiveMap live = readLive();
  std::vector<Pauses::View> views;
  for (auto& t : tiles) {
    Pauses::View v; v.id = t->s.id; v.running = !!t->pty && !t->s.asleep; v.ready = readyAfterStart(*t, live);
    const session::LiveRecord* l = t->s.sessionId ? live.get(*t->s.sessionId) : nullptr;
    v.status = l && l->alive ? l->statusText() : "starting"; v.statusAt = l ? l->statusUpdatedAt : 0;
    if (t->tail && t->tail->facts().lastText) { v.hasText = true; v.lastText = *t->tail->facts().lastText; }
    if (t->tail && t->tail->facts().lastAssistantAt) { const double at = js::dateParse(*t->tail->facts().lastAssistantAt); v.lastAssistantAt = std::isnan(at) ? 0.0 : at; }
    if (t->tail && t->tail->facts().lastPromptKind) { v.hasPrompt = true; v.promptKind = *t->tail->facts().lastPromptKind; v.promptAt = t->tail->facts().lastPromptAt.value_or(0); }
    if (t->s.sessionId) v.sid = *t->s.sessionId;
    views.push_back(v);
  }
  bool changed = false;
  std::map<std::string, std::string> was; for (const auto& kv : pauses.m) was[kv.first] = kv.second.state;
  const std::vector<std::string> resume = pauses.step(views, now(), [this](const std::string& id) { return restoreRefresh.settled(id); }, changed);
  for (const std::string& id : pauses.seen) { logEvent("pause", id8(id) + ": a PAUSE prompt in its transcript (not typed by the deck): pausing"); was.erase(id); }
  for (const std::string& id : pauses.cleared) { logEvent("pause", id8(id) + ": another prompt after the pause, or a /clear (a new session id): no longer paused"); was.erase(id); }
  for (const std::string& id : resume) {
    Tile* t = tileById(id);
    if (!t || !t->s.sessionId) continue;
    const std::string file = pauses.fileOf(id);
    logEvent("pause", "resuming " + id8(id) + " (" + (file.empty() ? pauseFileOf(*t->s.sessionId) : file) + ")");
    typePrompt(id, pauseFill(resumePromptText(), file.empty() ? pauseFileOf(*t->s.sessionId) : file, pauseLocalStamp(deckStartedAt, sm.clock->localOffsetMs(deckStartedAt))), false);
  }
  for (const auto& kv : was) {                    // pausing -> paused (its PAUSED line); resuming -> done; a closed tile
    auto it = pauses.m.find(kv.first);
    const std::string now_ = it == pauses.m.end() ? std::string(kv.second == "resuming" ? "resumed" : "gone") : it->second.state;
    if (now_ != kv.second && now_ != "resuming") logEvent("pause", id8(kv.first) + ": " + kv.second + " -> " + now_);
  }
  if (changed) { if (restoring(now())) pauseUnsaved = true; saveState(); pushMeta(); }
}

// ---------------------------------------------------------------- auditor sessions and their children (main.js; auditors.hpp)
// An auditor's inbox is <stateDir>/auditors/<tile id>/; a file to-<n>.md there is typed into its child n - the deck
// typing it makes it the user's own turn - once that child is idle (never into a running turn, never while paused),
// then moved to sent/. Making a child is the authorisation; un-childing, un-making or closing either ends it.
static bool auditable(const Tile* t) { return t && t->s.kind == "claude" && t->s.sessionId && !t->s.sessionId->empty() && !t->s.exited; }
static std::optional<std::string> optTitle(const std::string& s) { return s.empty() ? std::nullopt : std::optional<std::string>(s); }
// the tile's own name, before the deck renamed it a child (main.js ownTitle)
static std::string ownTitle(const Auditors& auditors, const Tile& t) {
  const Auditors::Kid* k = auditors.child(t.s.id);
  return k && k->hasPrev ? k->prevTitle : t.s.knownTitle && !t.s.knownTitle->empty() ? *t.s.knownTitle : (t.tail && t.tail->facts().title) ? *t.tail->facts().title : std::string();
}
json D::makeAuditor(const std::string& id, const json& string, std::string& why) {
  Tile* t = tileById(id);
  if (!auditable(t)) { why = "only a Claude session can be an auditor"; return nullptr; }
  const Auditors::Result r = auditors.make(id, string.is_string() ? string.get<std::string>() : std::string(), !!t->s.userTitle, t->s.userTitle ? *t->s.userTitle : std::string());
  if (!r.ok) { why = r.why; return nullptr; }
  const std::string inbox = auditorInbox(id);
  sm.files->makeDirs(inbox); sm.files->makeDirs(joinPath(inbox, "sent")); sm.files->makeDirs(joinPath(inbox, "refused"));
  t->s.userTitle = r.string;
  for (const auto& c : auditors.children(id)) if (Tile* ct = tileById(c.first)) ct->s.userTitle = auditors.nameOf(c.first);
  if (!r.again) {
    const json tmpl = js::get(settings, "auditorPrompt");
    std::string tp = tmpl.is_string() ? tmpl.get<std::string>() : std::string();
    if (tp.find_first_not_of(" \t\r\n") == std::string::npos) tp.clear();
    Deliveries::Item it; it.kind = "auditor"; it.target = id; it.text = auditorPrompt(tp, r.string, inbox, std::string(1, pathSep()));
    deliveries.add(it, false, now());
  }
  logEvent("auditor", id8(id) + " " + (r.again ? "renamed" : "made") + " \"" + r.string + "\" (" + inbox + ")");
  saveState(); pushMeta(); auditorTick();
  return { { "string", r.string }, { "inbox", inbox } };
}
bool D::unmakeAuditor(const std::string& id, std::string& why) {
  Auditors::Ended prev; std::vector<Auditors::Ended> kids;
  if (!auditors.unmake(id, prev, kids)) { why = "not an auditor"; return false; }
  if (Tile* t = tileById(id)) t->s.userTitle = optTitle(prev.prevTitle);
  for (const auto& c : kids) { if (Tile* ct = tileById(c.id)) ct->s.userTitle = optTitle(c.prevTitle); deliveries.dropFor(c.id); }
  deliveries.dropFor(id); deliveries.dropFrom(id);
  logEvent("auditor", id8(id) + " un-made (" + std::to_string(kids.size()) + " children ended)");
  saveState(); pushMeta();
  return true;
}
json D::setAuditorChild(const std::string& id, const std::string& parent, std::string& why) {
  Tile* t = tileById(id);
  if (!auditable(t)) { why = "only a Claude session can be a child"; return nullptr; }
  const bool hadTitle = !!t->s.userTitle; const std::string before = hadTitle ? *t->s.userTitle : std::string();
  const Auditors::Result r = auditors.setChild(id, parent);
  if (!r.ok) { why = r.why; return nullptr; }
  if (r.again) return { { "n", r.n } };
  if (!r.left.empty()) { const std::string left = r.left; deliveries.drop([&](const Deliveries::Item& i) { return i.target == id && i.from == left; }); tellChildLeft(left, id, r.leftN, ownTitle(auditors, *t), "un-childed"); }
  else auditors.setPrevTitle(id, hadTitle, before);
  AuditorChildFacts f; f.sessionId = *t->s.sessionId; f.account = t->s.account ? *t->s.account : std::string(); f.cwd = t->s.cwd;
  f.browser = t->s.browser ? (*t->s.browser ? 1 : 0) : -1;
  f.title = ownTitle(auditors, *t);
  t->s.userTitle = auditors.nameOf(id);
  Deliveries::Item it; it.kind = "line"; it.target = parent; it.about = id; it.text = auditorChildLine(r.n, f);
  deliveries.add(it, false, now());
  logEvent("auditor", id8(parent) + ": " + it.text);
  saveState(); pushMeta(); auditorTick();
  return { { "n", r.n }, { "name", *t->s.userTitle } };
}
// the auditor is told a child left it (2026-11-23) - unless the deck had not yet typed that child's CHILD line
// (then the line is dropped and the auditor never heard of it)
void D::tellChildLeft(const std::string& parent, const std::string& id, int n, const std::string& title, const std::string& leftWhy) {
  if (deliveries.drop([&](const Deliveries::Item& i) { return i.about == id && i.target == parent; })) return;
  if (!auditors.isAuditor(parent)) return;
  Deliveries::Item it; it.kind = "line"; it.target = parent; it.text = auditorLeftLine(n, title, leftWhy);
  deliveries.add(it, false, now());
  logEvent("auditor", id8(parent) + ": " + it.text);
}
// leftWhy: "un-childed" (the menu) | "closed" (its tile closed)
bool D::unchildTile(const std::string& id, std::string& why, const std::string& leftWhy) {
  Auditors::Ended e; const Auditors::Kid* k = auditors.child(id); const std::string parent = k ? k->parent : std::string();
  Tile* t0 = tileById(id); const std::string title = t0 && k ? ownTitle(auditors, *t0) : std::string();
  if (!auditors.unchild(id, &e)) { why = "not a child"; return false; }
  if (Tile* t = tileById(id)) t->s.userTitle = optTitle(e.prevTitle);
  deliveries.dropFor(id);
  tellChildLeft(parent, id, e.n, title, leftWhy == "closed" ? "closed" : "un-childed");
  deliveries.drop([&](const Deliveries::Item& i) { return i.about == id; });   // its CHILD line, if not typed yet
  logEvent("auditor", id8(id) + " un-childed (was child " + std::to_string(e.n) + " of " + id8(parent) + ")");
  saveState(); pushMeta();
  return true;
}
void D::endAuditorTies(const std::string& id) {
  std::string why;
  if (auditors.isAuditor(id)) unmakeAuditor(id, why); else if (auditors.child(id)) unchildTile(id, why, "closed");
  deliveries.dropFor(id);
}
bool D::auditorSend(const std::string& itemId, std::string& why) {
  if (!deliveries.send(itemId)) { why = "nothing waiting to send"; return false; }
  pushMeta(); auditorTick(); return true;
}
bool D::auditorDrop(const std::string& itemId, std::string& why) {
  Deliveries::Item* p = deliveries.get(itemId);
  if (!p || (p->state != "hold" && p->state != "queued")) { why = "nothing waiting to drop"; return false; }
  const Deliveries::Item it = *p;
  deliveries.remove(itemId);
  if (!it.file.empty()) refuseAuditorFile(it.from, it.name, "not sent: the user dropped it on the child tile");
  pushMeta(); return true;
}
// <inbox>/<name> -> <inbox>/<sub>/<stamp> <name>, a note line on top for refused/; "" when it could not be moved
std::string D::moveAuditorFile(const std::string& from, const std::string& name, const std::string& sub, const std::string& note) {
  const std::string inbox = auditorInbox(from), src = joinPath(inbox, name), dir = joinPath(inbox, sub);
  const double at = now(); const std::string stamp = auditorFileStamp(at, sm.clock->localOffsetMs(at));
  sm.files->makeDirs(dir);
  std::string dst = joinPath(dir, stamp + " " + name);
  for (int k = 2; sm.files->stat(dst).exists; k++) dst = joinPath(dir, stamp + " (" + std::to_string(k) + ") " + name);
  bool ok = false;
  if (!note.empty()) { std::string raw; ok = sm.files->read(src, raw) && sm.files->write(dst, note + "\n\n" + raw) && sm.files->remove(src); }
  else ok = sm.files->rename(src, dst);
  if (!ok) { logEvent("auditor", "could not move " + src + " to " + sub); return std::string(); }
  return dst;
}
// one line in <inbox>/log.md per delivery or refusal: the back-and-forth kept on file
void D::logAuditorFile(const std::string& from, int n, const std::string& name, const std::string& outcome) {
  const double at = now(); const std::string file = joinPath(auditorInbox(from), "log.md");
  std::string cur; sm.files->read(file, cur);
  if (!sm.files->write(file, cur + auditorLogLine(pauseLocalStamp(at, sm.clock->localOffsetMs(at)), n, name, outcome) + "\n")) logEvent("auditor", "could not write " + file);
}
void D::refuseAuditorFile(const std::string& from, const std::string& name, const std::string& why) {
  const double at = now(); const Auditors::Aud* a = auditors.auditor(from); const int n = auditorToIndex(name);
  const std::string who = a ? a->string : from;
  const std::string note = auditorRefusedNote(pauseLocalStamp(at, sm.clock->localOffsetMs(at)), who, n, why);
  if (!moveAuditorFile(from, name, "refused", note).empty()) {
    logEvent("auditor", id8(from) + ": " + name + " refused (" + (why.empty() ? "no child " + std::to_string(n) : why) + ")");
    logAuditorFile(from, n, name, "refused: " + (why.empty() ? who + " has no child " + std::to_string(n) : why));
  }
}
void D::drainAuditorInboxes(double at) {
  std::vector<std::pair<std::string, std::string>> list; for (const auto& kv : auditors.a) list.push_back({ kv.first, kv.second.string });
  for (const auto& aud : list) {
    const std::string& aid = aud.first; const std::string inbox = auditorInbox(aid);
    if (!auditors.isAuditor(aid)) continue;
    std::vector<int> kids; for (const auto& c : auditors.children(aid)) kids.push_back(c.second);
    const AuditorInboxPlan plan = auditorInboxPlan(sm.files->list(inbox), kids, deliveries.pendingFiles(aid));
    std::vector<std::pair<std::string, int>> all = plan.refuse; all.insert(all.end(), plan.take.begin(), plan.take.end());
    for (const auto& f : all) {
      const std::string file = joinPath(inbox, f.first);
      const FileStat st = sm.files->stat(file);
      if (!st.exists || st.isDir) continue;
      const double age = at - st.mtimeMs;
      if (age < AUDITOR_SETTLE_MS) continue;                       // still being written
      const std::string child = auditors.childAt(aid, f.second);
      if (child.empty()) { refuseAuditorFile(aid, f.first); continue; }
      if (st.size == 0) { if (age >= AUDITOR_EMPTY_MAX_MS) refuseAuditorFile(aid, f.first, "an empty file"); continue; }
      if (st.size > AUDITOR_TEXT_MAX * 4) { refuseAuditorFile(aid, f.first, "over " + std::to_string(AUDITOR_TEXT_MAX) + " characters"); continue; }
      std::string raw; if (!sm.files->read(file, raw)) continue;
      std::string first = auditorPromptText(raw); first = first.substr(0, first.find('\n')); if (first.size() > 120) first.resize(120);
      const bool hold = !auditorAutoSendOn(settings);
      Deliveries::Item it; it.kind = "file"; it.target = child; it.from = aid; it.fromName = aud.second; it.n = f.second; it.name = f.first; it.file = file; it.firstLine = first; it.urgent = auditorIsNow(f.first);
      deliveries.add(it, hold, at);
      logEvent("auditor", id8(aid) + ": " + f.first + " for " + auditors.nameOf(child) + (it.urgent ? " (URGENT: mid-turn)" : "") + (hold ? " (waits for Send)" : ""));
      pushMetaSoon();
    }
  }
}
void D::auditorTick() {
  const double at = now();
  if (restarting || restoring(at) || (auditors.a.empty() && deliveries.items.empty())) return;
  drainAuditorInboxes(at);
  bool any = false; for (const auto& i : deliveries.items) if (i.state == "queued") any = true;
  if (!any) return;
  const session::LiveMap live = readLive();
  std::vector<Deliveries::View> views;
  for (auto& t : tiles) {
    Deliveries::View v; v.id = t->s.id; v.running = !!t->pty && !t->s.asleep; v.asleep = t->s.asleep && !t->s.waking; v.paused = pauses.has(t->s.id);
    const session::LiveRecord* l = t->s.sessionId ? live.get(*t->s.sessionId) : nullptr;
    v.statusAt = l ? l->statusUpdatedAt : 0;
    const bool free = !pauses.talking(t->s.id) && restoreRefresh.settled(t->s.id);
    v.ready = readyAfterStart(*t, live) && free;
    v.live = !!t->pty && l && l->alive && l->statusUpdatedAt > t->s.startedAt && free;   // up after its start, idle or not (an urgent file)
    views.push_back(v);
  }
  std::vector<std::string> wake;
  const std::vector<Deliveries::Item> type = deliveries.step(views, at, wake);
  for (const std::string& id : wake) wakeTile(id, "auditor");
  for (const Deliveries::Item& it : type) {
    Tile* t = tileById(it.target);
    if (!t) continue;
    const std::string who = t->s.userTitle ? *t->s.userTitle : id8(it.target);
    std::string text = it.text;
    if (it.kind == "file") {
      std::string raw;
      if (!sm.files->read(it.file, raw)) { logEvent("auditor", it.name + " for " + who + ": the file is gone, nothing typed"); deliveries.remove(it.id); logAuditorFile(it.from, it.n, it.name, "refused: the file was gone when it was due, nothing typed"); continue; }
      text = auditorPromptText(raw);
      if (text.empty()) { deliveries.remove(it.id); refuseAuditorFile(it.from, it.name, "an empty file"); continue; }
      if (text.size() > AUDITOR_TEXT_MAX) { deliveries.remove(it.id); refuseAuditorFile(it.from, it.name, "over " + std::to_string(AUDITOR_TEXT_MAX) + " characters"); continue; }
      if (Deliveries::Item* kept = deliveries.get(it.id)) { std::string first = text.substr(0, text.find('\n')); if (first.size() > 120) first.resize(120); kept->firstLine = first; }
      const std::string sent = moveAuditorFile(it.from, it.name, "sent", std::string());
      logEvent("auditor", it.name + " from " + it.fromName + " typed into " + who + (it.esc ? " mid-turn (Esc)" : "") + (sent.empty() ? std::string() : " (" + sent + ")"));
      logAuditorFile(it.from, it.n, it.name, it.esc ? "delivered mid-turn (Esc)" : "delivered");
    } else logEvent("auditor", std::string(it.kind == "auditor" ? "the AUDITOR prompt" : "a CHILD line") + " typed into " + who);
    typePrompt(it.target, text, it.esc);
  }
  if (!type.empty()) pushMeta();
}

// ---------------------------------------------------------------- a Claude session started inside a shell tile (lib/shellhost.js + main.js scanShellHosted)
void D::scanShellHosted() {
  bool anyShell = false, anyInShell = false;
  for (const auto& t : tiles) { if (t->s.kind == "shell" && t->pty && t->pid) anyShell = true; if (t->inShell) anyInShell = true; }
  if (!anyShell && !anyInShell) return;
  const session::LiveMap live = readLive();
  // a /clear inside a shell-hosted session is a move, not the session ending (followTiles before leftShell)
  if (followTiles(live)) pushMeta();
  // leftShell: a tile that took on a session in its shell and no longer runs it is a shell again
  for (auto& tp : tiles) {
    Tile& t = *tp;
    if (!t.inShell || !t.pty) continue;
    const session::LiveRecord* l = t.s.sessionId ? live.get(*t.s.sessionId) : nullptr;
    if (l && l->alive && t.s.account && l->account == *t.s.account) continue;
    t.s.kind = "shell"; t.s.account.reset(); t.s.sessionId.reset(); t.inShell = false; t.tail.reset(); t.s.knownTitle.reset(); t.s.lastStatus.reset(); t.s.browser.reset();
    t.claudeBound = false; t.claudePid = 0; t.claudeProcStart = json(); t.followRefused.reset();
    saveState(); pushMeta();
  }
  auto runningShells = [this] { std::vector<Tile*> v; for (auto& t : tiles) if (t->s.kind == "shell" && t->pty && t->pid) v.push_back(t.get()); return v; };
  std::vector<Tile*> shells = runningShells();
  if (shells.empty()) return;
  std::set<std::string> hosted;
  for (const auto& t : tiles) if (t->s.sessionId && !t->s.sessionId->empty()) hosted.insert(*t->s.sessionId);
  double since = shells[0]->s.startedAt; for (Tile* t : shells) since = std::min(since, t->s.startedAt);
  struct Cand { std::string sessionId, key; const session::LiveRecord* live; uint32_t pid; };
  std::vector<Cand> cands;
  for (const auto& kv : live.entries()) {
    const session::LiveRecord& l = kv.second;
    if (!l.alive || hosted.count(kv.first)) continue;
    if (js::truthy(l.kind) && l.kind != "interactive") continue;
    if (l.startedAt != 0 && l.startedAt < since - 5000) continue;
    const std::string key = js::toString(l.pid) + ":" + (js::truthy(l.procStart) ? js::toString(l.procStart) : std::string());
    if (shellHostSeen.count(key)) continue;
    cands.push_back({ kv.first, key, &l, l.pid.is_number() ? (uint32_t)l.pid.get<double>() : 0 });
  }
  if (cands.empty()) return;
  std::map<uint32_t, uint32_t> parents;
  if (!sm.procs->parents(parents) || parents.empty()) return;        // the snapshot failed: try again next tick
  auto place = [this](Tile* t, const Cand& c) {
    if (!t || t->s.kind != "shell") return;
    for (const auto& x : tiles) if (x->s.sessionId && *x->s.sessionId == c.sessionId) return;
    // shellTookSession: the tile takes the session on in place; the running process is left alone
    t->s.kind = "claude"; t->s.account = c.live->account; t->s.sessionId = c.sessionId;
    if (js::truthy(c.live->cwd)) t->s.cwd = js::toString(c.live->cwd);
    t->inShell = true; t->tail.reset(); t->s.knownTitle.reset(); t->s.lastStatus.reset(); t->s.lastStoppedInit = false; t->s.browser.reset(); t->trust.reset();
    t->direct = false; t->claudeBound = true; t->claudePid = c.live->pid.is_number() ? c.live->pid.get<double>() : 0; t->claudeProcStart = c.live->procStart; t->followRefused.reset();
    saveState(); pushMeta();
  };
  std::vector<Cand> unplaced;
  for (const Cand& c : cands) {
    if (!c.pid || parents.find(c.pid) == parents.end()) continue;    // gone, or too new for the snapshot
    // ownerOf: walk up from the session's pid until a shell tile's pty pid turns up
    Tile* owner = nullptr;
    uint32_t p = parents[c.pid];
    for (int i = 0; i < 12 && p; i++) {
      for (Tile* t : shells) if (t->pid == p) owner = t;
      if (owner) break;
      const auto nx = parents.find(p);
      if (nx == parents.end() || nx->second == p) break;
      p = nx->second;
    }
    if (owner) { shellHostSeen.insert(c.key); place(owner, c); } else unplaced.push_back(c);
  }
  shells = runningShells();
  if (unplaced.empty() || shells.empty()) { for (const Cand& c : unplaced) shellHostSeen.insert(c.key); return; }
  // The walk found no tile: the session runs in some other window, or its chain broke on the way up
  // (`usw --start`). The console it is attached to says which.
  std::vector<uint32_t> ask; for (const Cand& c : unplaced) ask.push_back(c.pid); for (Tile* t : shells) ask.push_back(t->pid);
  std::map<uint32_t, int64_t> hosts;
  if (!sm.procs->consoleHosts(ask, hosts) || hosts.empty()) {        // could not ask: try again next tick, three times at most
    for (const Cand& c : unplaced) { const int n = ++shellConsoleFails[c.key]; if (n >= 3) { shellHostSeen.insert(c.key); shellConsoleFails.erase(c.key); } }
    return;
  }
  for (const Cand& c : unplaced) {
    shellHostSeen.insert(c.key);
    const auto h = hosts.find(c.pid);
    if (h == hosts.end() || !(h->second > 0)) continue;
    Tile* one = nullptr; int n = 0;
    for (Tile* t : runningShells()) { const auto th = hosts.find(t->pid); if (t->pid != c.pid && th != hosts.end() && th->second == h->second) { one = t; n++; } }
    if (n == 1) place(one, c);
  }
}

}}  // namespace deck::core
