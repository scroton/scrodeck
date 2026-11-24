// core/deck/deck_parity.cpp — the rest of main.js that the 2026-11-19 build brought over, on Deck::Impl:
//   ask pages        main.js doneAsk / notifyNewAsks / openAskLink + the AskFeed's wiring   (lib/asks.js is asks.cpp)
//   the page viewer  main.js viewHtml / sessionHtml / meta.settings.pages                   (lib/viewer.js is viewer.cpp)
//   the on-call's desks  main.js openDeskRequest / placeDeskTile / notifyDeskOpened         (lib/deskopen.js is deskopen.cpp)
//   cloud sessions   main.js cloudSpec / cloudCreated / cloudTick / openCloud / cloudMeta   (lib/cloud.js is cloud.cpp)
//   the exit watchdog's core half, a clicked toast (tiles:activate / asks:open)
// Each function names the main.js function it ports; the comments there say why a rule exists.
#include "deck_impl.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace deck { namespace core {

using D = Deck::Impl;

// s.length in UTF-16 units
static size_t units(const std::string& s) { size_t n = 0; for (size_t i = 0; i < s.size();) { n += js::nextCodePoint(s, i) > 0xFFFF ? 2 : 1; } return n; }
// s.slice(-n) in UTF-16 units (a cut inside a surrogate pair drops the whole character)
static std::string lastUnits(const std::string& s, size_t n) {
  const size_t total = units(s);
  if (total <= n) return s;
  size_t skip = total - n, i = 0;
  while (i < s.size() && skip > 0) { const uint32_t cp = js::nextCodePoint(s, i); const size_t u = cp > 0xFFFF ? 2 : 1; skip = skip > u ? skip - u : 0; }
  return s.substr(i);
}
// (24000).toLocaleString() as en-US prints it
static std::string grouped(size_t n) {
  std::string d = std::to_string(n), out;
  for (size_t i = 0; i < d.size(); i++) { if (i && (d.size() - i) % 3 == 0) out += ','; out += d[i]; }
  return out;
}
static bool startsWithNoCase(const std::string& s, const char* p) {
  const size_t n = std::char_traits<char>::length(p);
  return s.size() >= n && lowerAscii(s.substr(0, n)) == p;
}

Entry* D::primaryEntry() { const std::vector<Entry*> w = winEntries(); return w.empty() ? nullptr : w[0]; }
std::string D::tileTitle(const Tile& t) const {
  if (t.s.userTitle && !t.s.userTitle->empty()) return *t.s.userTitle;
  if (t.s.knownTitle && !t.s.knownTitle->empty()) return *t.s.knownTitle;
  if (t.tail && t.tail->facts().title && !t.tail->facts().title->empty()) return *t.tail->facts().title;
  return std::string();
}

// ---------------------------------------------------------------- start
void D::initParity() {
  const bool win = sm.env->os() == "win32";
  viewer = Viewer(win);

  // Every session's ask pages, from the skill's state folder (main.js whenReady: new Asks.AskFeed + asks.start())
  if (!opt.askStateDir.empty()) {
    AskDeps ad;
    ad.files = sm.files; ad.clock = sm.clock; ad.win = win;
    ad.home = home;
    ad.stateDir = opt.askStateDir; ad.downloads = opt.askDownloads;
    ad.draftsFile = joinPath(stateDir, "ask-drafts.json");
    ad.pid = sm.env->pid();
    ad.settings = [this] {                                     // { ...DEFAULT_SETTINGS.asks, ...(settings.asks || {}) }
      json v = js::get(defaults, "asks").is_object() ? js::get(defaults, "asks") : json::object();
      const json& mine = js::get(settings, "asks");
      if (mine.is_object()) for (auto it = mine.begin(); it != mine.end(); ++it) v[it.key()] = it.value();
      return v;
    };
    ad.onChange = [this] { pushMetaSoon(); notifyNewAsks(); };
    // the session read the answers file before Done's one line went in: drop the line, it would only repeat them
    ad.onRead = [this](const std::string& /*pageId*/, const json& sent) {
      const json& heldId = js::get(sent, "heldId");
      if (!held || !heldId.is_string()) return;
      const json list = held->list();
      for (const json& it : list) if (js::get(it, "id") == heldId) {
        const std::string st = js::str(js::get(it, "state"));
        if (st == "held" || st == "due") held->remove(heldId.get<std::string>());
        return;
      }
    };
    // who can still read answers: a Claude tile here (asleep too: it resumes), or a live Claude Code process
    // anywhere (its <config>/sessions/<pid>.json); any other session's page is marked "session ended"
    ad.liveSessions = [this](std::set<std::string>& out) {
      for (const auto& t : tiles) if (t->s.kind == "claude" && t->s.sessionId && !t->s.sessionId->empty() && !t->s.exited) out.insert(*t->s.sessionId);
      const session::LiveMap live = readLive();
      for (const auto& kv : live.entries()) if (kv.second.alive) out.insert(kv.first);
      return true;
    };
    ad.later = [this](double ms, std::function<void()> fn) { setTimer(ms, std::move(fn)); };
    asks.reset(new AskFeed(std::move(ad)));
    asks->writeDeckFile();                     // first: it finds out whether another deck is serving the pages (then this one is a guest)
    asks->scan();
    if (!asks->guest()) sm.files->makeDirs(opt.askStateDir);
    // main.js also watches the folder (fs.watch, 150 ms); here the poll alone carries it: a new page shows up to 4 s later
    setTimer(kAskScanMs, [this] { if (asks) asks->scan(); }, kAskScanMs);
    setTimer(kAskBeatSeconds * 1000.0, [this] { if (!asks) return; const bool was = asks->guest(); asks->writeDeckFile(); if (asks->guest() != was) { asks->scan(); pushMeta(); } }, kAskBeatSeconds * 1000.0);
  }

  // Cloud sessions' status (lib/cloud.js): each cloud tile's own session every 15 s, each account's live list every minute
  if (sm.http && sm.credentials && opt.cloudPolling) {
    cloud.reset(new CloudPoller(*sm.clock, sm.http, sm.credentials));
    syncCloudAccounts();
    setTimer(0, [this] { cloudTick(); });
    setTimer(kCloudTickMs, [this] { cloudTick(); }, kCloudTickMs);
  }
}

// main.js armExitWatchdog: a restart and the last window closing both arm it; one is enough
void D::armExitWatchdog() {
  if (watchdogArmed) return;
  watchdogArmed = true;
  if (sm.lifecycle) sm.lifecycle->armExitWatchdog(EXIT_WATCHDOG_MS);
}

// ---------------------------------------------------------------- ask pages (main.js doneAsk / notifyNewAsks / openAskLink)
const char* const kAskGuestReason = "another deck is serving the ask pages";
// pages: [{ id, answers }] where answers is the page's own answers() JSON (kind 'ask-page-answers')
json D::doneAsk(const std::string& sessionId, const json& pages) {
  if (!asks) throw std::runtime_error("the ask pane is not running");
  if (asks->guest()) throw std::runtime_error(kAskGuestReason);
  std::vector<json> ours;
  if (pages.is_array()) for (const json& p : pages) {
    const json& id = js::get(p, "id"); const json& a = js::get(p, "answers");
    if (!js::truthy(p) || !id.is_string() || !askIdOk(id.get<std::string>()) || !js::truthy(a) || js::get(a, "id") != id) continue;
    const AskRound r = asks->round(id.get<std::string>());
    if (!r.found) continue;
    const json& sid = js::get(r.state, "session_id");
    if ((js::truthy(sid) ? js::toString(sid) : r.sid) != sessionId) continue;
    ours.push_back(p);
  }
  if (ours.empty()) throw std::runtime_error("none of these pages is that session's");
  json forText = json::array();
  for (const json& p : ours) { const json& lines = js::get(js::get(p, "answers"), "lines"); forText.push_back({ { "id", p["id"] }, { "lines", js::truthy(lines) ? lines : json::array() } }); }
  if (js::trim(composeDone(forText)).empty()) throw std::runtime_error("nothing answered yet");
  Tile* tile = nullptr;
  for (auto& t : tiles) if (t->s.kind == "claude" && t->s.sessionId && *t->s.sessionId == sessionId && !t->s.exited) { tile = t.get(); break; }
  // Every Done hands over a FILE first: the answers file, exactly as the page's Done saves it in a browser, where
  // the skill's `wait` looks. A session in this deck then gets ONE line naming it; nothing long is typed.
  json files = json::array(); std::vector<std::string> ids;
  for (const json& p : ours) {
    json answers = p["answers"];
    answers["saved_at"] = isoTime(now()); answers["via"] = "claude-deck";
    files.push_back(asks->writeAnswersFile(p["id"].get<std::string>(), answers));
    ids.push_back(p["id"].get<std::string>());
  }
  if (!tile) {
    // not in this deck: the file is all; its `ask_page.py wait` picks it up
    asks->markSent(ids, { { "how", "file" }, { "file", files[0] } });
    for (const json& p : ours) asks->logDone(p["id"].get<std::string>(), p["answers"], "file", json());
    return { { "how", "file" }, { "files", files } };
  }
  // Claude Code's own picker for these questions is on the screen: Esc closes it, then the line goes in as the
  // next message. Only when the session is WAITING and the picker's question is drawn on its screen — never an
  // Esc into a permission prompt.
  std::vector<std::string> pickerQs;
  for (const json& p : ours) {
    const AskRound r = asks->round(p["id"].get<std::string>());
    if (r.found && js::get(r.round, "source") == "picker" && r.items.is_array())
      for (const json& it : r.items) pickerQs.push_back(js::truthy(js::get(it, "question")) ? js::toString(js::get(it, "question")) : std::string());
  }
  const bool waiting = tile->s.lastStatus && *tile->s.lastStatus == "waiting";
  const bool esc = !pickerQs.empty() && waiting && tile->pty && pickerOnScreen(tile->replay.all(), pickerQs);
  if (esc) tile->pty->write("\x1b");
  json forLine = json::array();
  for (size_t i = 0; i < ours.size(); i++) {
    const json& a = ours[i]["answers"];
    forLine.push_back({ { "id", ours[i]["id"] }, { "file", files[i] }, { "answered", js::get(a, "answered") }, { "total", js::get(a, "total") }, { "later", js::get(a, "later") } });
  }
  const std::string tileId = tile->s.id;
  const json title = tileTitle(*tile).empty() ? json() : json(tileTitle(*tile));
  const json item = held->add(tileId, { { "kind", "next" }, { "text", composePointer(forLine) } });
  asks->markSent(ids, { { "how", "file" }, { "file", files[0] }, { "tileId", tileId }, { "heldId", js::get(item, "id") } });
  for (const json& p : ours) asks->logDone(p["id"].get<std::string>(), p["answers"],
    esc ? "saved to a file, named in one line after Esc on its picker" : "saved to a file, named in one line", title);
  setTimer(esc ? 1500 : 250, [this] { if (held) held->tick(); });
  pushMeta();
  return { { "how", "file" }, { "files", files }, { "tileId", tileId }, { "esc", esc }, { "waiting", !esc && waiting } };
}

// A toast when a session puts up a new ask page while no deck window is focused. One per session; clicking it
// brings the window up on that session's questions (Deck::toastClicked).
void D::notifyNewAsks() {
  if (!asks) return;
  const json got = newAskRows(asksSeenInit ? asksSeen : json(), asks->view());
  asksSeen = js::get(got, "seen"); asksSeenInit = true;
  const json& n = js::get(settings, "notify");
  const json& fresh = js::get(got, "fresh");
  if (asks->guest()) return;                   // the deck that serves the pages toasts for them
  if (!fresh.is_array() || fresh.empty() || (js::get(n, "asks").is_boolean() && !js::get(n, "asks").get<bool>())) return;
  if (anyWindowFocused() || !sm.notifier) return;
  for (size_t k = 0; k < fresh.size() && k < 3; k++) {
    const json& s = js::get(fresh[k], "session"); const json& rounds = js::get(fresh[k], "rounds");
    const std::string sid = js::str(js::get(s, "sessionId"));
    Tile* t = nullptr;
    for (auto& x : tiles) if (x->s.kind == "claude" && x->s.sessionId && !x->s.sessionId->empty() && *x->s.sessionId == sid) { t = x.get(); break; }
    const std::string sAccount = js::truthy(js::get(s, "account")) ? js::toString(js::get(s, "account")) : std::string();
    const Account* acct = (t && t->s.account && !t->s.account->empty()) ? acctById(t->s.account) : accountById(accounts, sAccount);
    std::string who = t ? tileTitle(*t) : std::string();
    if (who.empty()) who = js::truthy(js::get(s, "name")) ? js::toString(js::get(s, "name")) : std::string("a session");
    double q = 0; bool replaces = false;
    if (rounds.is_array()) for (const json& r : rounds) {
      if (js::get(r, "total").is_number()) q += js::get(r, "total").get<double>();
      if (js::get(r, "replaces").is_array() && !js::get(r, "replaces").empty()) replaces = true;
    }
    Toast toast;
    toast.title = (acct ? acct->label : (!sAccount.empty() ? sAccount : std::string("Claude"))) + " \xC2\xB7 " + who;
    toast.body = "asks you: " + js::numberText(q) + " question" + (q == 1 ? "" : "s") + " on a page" + (replaces ? " (it replaces an earlier page)" : "");
    toast.silent = js::get(n, "sound").is_boolean() && !js::get(n, "sound").get<bool>();
    toast.kind = "ask"; toast.sessionId = sid; toast.tileId = t ? t->s.id : std::string();
    sm.notifier->toast(toast);
    if (t) {
      Entry* w = windowHolding(t->s.id);
      if (!w || w->remote) w = primaryEntry();
      if (w) sm.notifier->flash(w->index);
    }
  }
}

// url.fileURLToPath for the links an ask page carries: file:///C:/a%20b -> C:\a b, file://host/share/x -> \\host\share\x
static std::string fileUrlToPath(const std::string& url, bool win) {
  std::string rest = url.substr(5);                                  // after "file:"
  const size_t cut = rest.find_first_of("?#"); if (cut != std::string::npos) rest.resize(cut);
  std::string host;
  if (rest.compare(0, 2, "//") == 0) { const size_t slash = rest.find('/', 2); host = rest.substr(2, slash == std::string::npos ? std::string::npos : slash - 2); rest = slash == std::string::npos ? std::string() : rest.substr(slash); }
  std::string path;
  for (size_t i = 0; i < rest.size(); i++) {
    if (rest[i] != '%') { path += rest[i]; continue; }
    if (i + 2 >= rest.size() || !std::isxdigit((unsigned char)rest[i + 1]) || !std::isxdigit((unsigned char)rest[i + 2])) return std::string();
    const char ch = (char)std::stoi(rest.substr(i + 1, 2), nullptr, 16);
    if (ch == '/' || (win && ch == '\\')) return std::string();      // an encoded separator: Node throws
    path += ch; i += 2;
  }
  if (!win) return host.empty() || host == "localhost" ? path : std::string();
  for (char& c : path) if (c == '/') c = '\\';
  if (!host.empty() && host != "localhost") return "\\\\" + host + path;
  // \C:\… -> C:\…; anything else is not an absolute Windows path
  if (path.size() >= 3 && path[0] == '\\' && std::isalpha((unsigned char)path[1]) && path[2] == ':') return path.substr(1);
  return std::string();
}
// main.js openAskLink: a web address opens in the default browser; a file a page links to opens in its own
// program; something that would RUN is shown in its folder instead.
void D::openAskLink(const std::string& u) {
  if (!sm.lifecycle) return;
  if (startsWithNoCase(u, "http://") || startsWithNoCase(u, "https://")) { sm.lifecycle->openExternal(u); return; }
  if (!startsWithNoCase(u, "file:")) return;
  const std::string p = fileUrlToPath(u, sm.env->os() == "win32");
  if (p.empty()) return;
  static const char* const RUNNABLE[] = { ".exe", ".com", ".bat", ".cmd", ".ps1", ".psm1", ".vbs", ".vbe", ".js", ".jse", ".wsf", ".wsh", ".msi", ".msp", ".scr", ".lnk", ".url", ".reg", ".hta", ".cpl", ".jar", ".app", ".command", ".sh" };
  const std::string low = lowerAscii(p);
  for (const char* e : RUNNABLE) { const size_t n = std::char_traits<char>::length(e); if (low.size() >= n && low.compare(low.size() - n, n, e) == 0) { sm.lifecycle->showInFolder(p); return; } }
  sm.lifecycle->openPath(p);
}

// ---------------------------------------------------------------- the page viewer (main.js viewHtml / sessionHtml, meta.settings.pages)
json D::pagesShown() {
  json out = json::array();
  const json& pages = js::get(settings, "pages");
  if (pages.is_array()) for (const json& p : pages) {
    if (!js::get(p, "file").is_string()) continue;
    json row = viewer.show(js::get(p, "file").get<std::string>());   // { dir, name }: its folder becomes servable
    row["label"] = js::get(p, "label");
    out.push_back(row);
  }
  return out;
}
// an .html file the phone was given the path of (a session's message, or the session's own list) opens there
json D::viewHtml(const json& p) {
  const std::string raw = p.is_string() ? p.get<std::string>() : (js::truthy(p) ? js::toString(p) : std::string());
  const std::string f = viewerHtmlFile(*sm.files, raw, home, sm.env->os() == "win32");
  json out = viewer.show(f);
  out["label"] = baseName(f); out["path"] = f;
  return out;
}
// the .html files this session wrote or named that are on disk now, newest mention first (cached 30 s per transcript)
json D::sessionHtml(const std::string& tileId) {
  Tile* t = tileById(tileId);
  if (!t || !t->s.sessionId || t->s.sessionId->empty()) throw std::runtime_error("no such session");
  const Account* a = acctById(t->s.account);
  const std::string file = a ? findTranscript(a->dir, *t->s.sessionId) : std::string();
  if (file.empty()) return json::array();
  const FileStat st = sm.files->stat(file);
  if (!st.exists || st.isDir) throw std::runtime_error("no transcript on disk for this session yet");   // (fs.statSync throws in the JS)
  auto hit = sessionHtmlCache.find(file);
  if (hit != sessionHtmlCache.end() && hit->second.size == st.size && hit->second.mtime == st.mtimeMs && now() - hit->second.at < 30000) return hit->second.list;
  std::string text;
  if (!sm.files->read(file, text)) throw std::runtime_error("no transcript on disk for this session yet");
  const json list = viewerSessionHtml(*sm.files, text, home, sm.env->os() == "win32");
  if (hit == sessionHtmlCache.end()) sessionHtmlOrder.push_back(file);
  sessionHtmlCache[file] = { st.size, st.mtimeMs, now(), list };
  while (sessionHtmlCache.size() > 50 && !sessionHtmlOrder.empty()) { sessionHtmlCache.erase(sessionHtmlOrder.front()); sessionHtmlOrder.pop_front(); }
  return list;
}

// ---------------------------------------------------------------- a desk the Claude on-call opens (main.js openDeskRequest and friends)
void D::startDeskInbox() {
  if (!opt.deskOpenInbox || down) return;
  deskInbox.reset(new DeskOpenInbox(*sm.files, *sm.clock, joinPath(stateDir, kDeskOpenDir), [this](const json& req) { return openDeskRequest(req); }));
  // a request left from before this start is opened only if the on-call is still waiting for it (its expiry)
  deskInbox->start();
  // main.js also watches the folder; the 1.5 s poll is its own belt-and-braces and is all there is here
  setTimer(DESK_INBOX_MS, [this] { if (deskInbox) deskInbox->drain(); }, DESK_INBOX_MS);
}
json D::deskWindows() {
  json out = json::array();
  Entry* primary = primaryEntry();
  for (Entry* w : winEntries())
    out.push_back({ { "index", w->index }, { "primary", w == primary }, { "layout", layoutJson(w->layout) }, { "showing", w->layout.empty() && !w->shown.empty() } });
  return out;
}
// Where the desk's tile goes: where it already is; else the first free cell in reading order of the main window,
// then of each auxiliary window; else a new auxiliary window with the tile alone in its top-left cell.
json D::placeDeskTile(Tile& t, Entry** entryOut) {
  Entry* primary = primaryEntry();
  const std::string id = t.s.id;
  Entry* holder = windowHolding(id);
  if (holder && !holder->remote) {
    json cells = json::array();
    for (const auto& kv : holder->layout) if (kv.first == id) cells = kv.second;
    *entryOut = holder;
    return { { "window", holder->index }, { "primary", holder == primary }, { "cells", cells }, { "newWindow", false } };
  }
  const json spot = deskPickSpot(deskWindows(), gridJson());
  const json cells = js::get(spot, "cells");
  if (js::truthy(js::get(spot, "newWindow"))) {
    auto num = [](const json& b, const char* k, double d) { return js::get(b, k).is_number() ? js::get(b, k).get<double>() : d; };
    const json base = primary && primary->bounds.is_object() ? primary->bounds : json{ { "x", 80 }, { "y", 80 }, { "width", 1200 }, { "height", 800 } };
    const double bw = num(base, "width", 1200), bh = num(base, "height", 800), bx = num(base, "x", 80), by = num(base, "y", 80);
    const json bounds = { { "width", numJ(std::max(800.0, js::round(bw * 0.8))) }, { "height", numJ(std::max(500.0, js::round(bh * 0.8))) },
      { "x", numJ(js::round(bx + bw * 0.1)) }, { "y", numJ(js::round(by + bh * 0.1)) } };
    // the window exists for the core first (an entry and a client), then the shell is asked to show it
    std::unique_ptr<Entry> e(new Entry());
    e->index = nextWindowIndex++; e->bounds = bounds;
    std::unique_ptr<Client> c(new Client());
    c->id = nextClient++; c->entry = e.get(); e->client = c->id;
    const ClientId cid = c->id; Entry* raw = e.get();
    entries.push_back(std::move(e)); clients.push_back(std::move(c));
    hadWindow = true;
    if (!sm.lifecycle || !sm.lifecycle->openWindow(cid, bounds, true)) {
      // NOT in main.js (Electron can always make a window): this shell cannot open another one. The tile stays
      // in the main window's sidebar — started, toasted and pulsing, in no cell.
      removeEntry(raw);
      clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const std::unique_ptr<Client>& x) { return x->id == cid; }), clients.end());
      nextWindowIndex--; nextClient--;
      *entryOut = primary;
      return { { "window", primary ? json(primary->index) : json() }, { "primary", true }, { "cells", json::array() }, { "newWindow", false } };
    }
    takeLayout(raw, { { id, cells } });          // persists the windows (under the remote-desk rule a remote desk that also shows it keeps it)
    Entry* still = nullptr; for (auto& x : entries) if (x.get() == raw) still = raw;
    if (still && !down) send(cid, { { "t", "init" }, { "meta", collectMeta() }, { "home", home }, { "platform", publicPlatform() }, { "build", nullptr }, { "assets", "" } });
    *entryOut = still;
    return { { "window", still ? json(still->index) : json() }, { "primary", false }, { "cells", cells }, { "newWindow", true } };
  }
  Entry* entry = nullptr;
  for (Entry* w : winEntries()) if (json(w->index) == js::get(spot, "window")) entry = w;
  if (!entry) { *entryOut = primary; return { { "window", nullptr }, { "primary", false }, { "cells", json::array() }, { "newWindow", false } }; }
  json lay = layoutJson(entry->layout); lay[id] = cells;
  takeLayout(entry, lay);
  sendToEntry(entry, "layout:place", { { "id", id }, { "cells", cells } });   // the window's view places it without selecting it
  *entryOut = entry;
  return { { "window", entry->index }, { "primary", entry == primary }, { "cells", cells }, { "newWindow", false } };
}
// Noticeable: the taskbar flash on its window, and a toast while no deck window has the focus (the pulse on the
// tile — meta.attention — says it otherwise).
void D::notifyDeskOpened(Tile& t, Entry* entry, const json& req, const json& place, const std::string& how) {
  const bool focused = anyWindowFocused();
  if (!sm.notifier) return;
  if (entry && !entry->remote && !entry->focused) sm.notifier->flash(entry->index);
  if (focused) return;
  const Account* acct = acctById(t.s.account);
  const json& bells = js::get(req, "bells");
  const size_t nb = bells.is_array() ? bells.size() : 0;
  const std::string title = js::str(js::get(req, "title"));
  const bool chrome = js::truthy(js::get(req, "chrome"));
  Toast toast;
  toast.title = (acct ? acct->label : (t.s.account ? *t.s.account : std::string())) + " \xC2\xB7 " + (t.s.userTitle && !t.s.userTitle->empty() ? *t.s.userTitle : title);
  toast.body = (how == "typed" ? std::string("the on-call has new bells for it")
      : std::string("the on-call ") + (how == "woken" ? "reopened" : "opened") + " the " + title + (chrome ? " with the browser" : ""))
    + ": " + deskPlacedWords(place) + (nb ? " - " + std::to_string(nb) + " bell" + (nb == 1 ? "" : "s") : std::string());
  toast.silent = js::get(js::get(settings, "notify"), "sound").is_boolean() && !js::get(js::get(settings, "notify"), "sound").get<bool>();
  toast.kind = "desk"; toast.tileId = t.s.id; toast.sessionId = t.s.sessionId ? *t.s.sessionId : std::string();
  sm.notifier->toast(toast);
}
// req: lib/deskopen.js checkRequest's. -> the answer's fields ({ ok, sessionId, tileId, pid, window, primary, cells,
// newWindow, how } or { ok: false, error }); a throw is a refusal too (the inbox answers it)
json D::openDeskRequest(const json& req) {
  const json& on = js::get(settings, "deskOpen");
  if (on.is_boolean() && !on.get<bool>()) return { { "ok", false }, { "error", "desk requests are switched off in the deck (settings.json deskOpen)" } };
  if (winEntries().empty()) return { { "ok", false }, { "error", "the deck has no window open" } };
  json accts = json::array(), rows = json::array();
  for (const Account& a : accounts) accts.push_back({ { "id", a.id }, { "dir", a.dir } });
  for (const auto& x : tiles) rows.push_back({ { "id", x->s.id }, { "kind", x->s.kind }, { "account", optJ(x->s.account) }, { "sessionId", optJ(x->s.sessionId) }, { "cwd", x->s.cwd },
    { "running", !!(x->pty && !x->s.exited) }, { "userTitle", optJ(x->s.userTitle) } });
  Files* files = sm.files;
  const json plan = deskOpenPlan(req, rows, accts, [files](const std::string& d) { return files->isDir(d); });
  if (!js::truthy(js::get(plan, "ok"))) return plan;
  const std::string action = js::str(js::get(plan, "action"));
  Tile* t = js::get(plan, "tileId").is_string() ? tileById(js::get(plan, "tileId").get<std::string>()) : nullptr;
  std::string how;
  if (action == "type") {
    if (!t) return { { "ok", false }, { "error", "the session did not start" } };
    held->add(t->s.id, { { "kind", "next" }, { "text", js::get(plan, "text") } });   // typed at its next idle prompt; never a second process for one session
    how = "typed";
  } else {
    // NOT in main.js: a deck that may not start Claude refuses before it makes a tile that could only read
    // "failed to start" (the on-call then starts its desk headless, as it does with no deck at all)
    if (!opt.allowClaude) return { { "ok", false }, { "error", "starting Claude sessions is switched off in the native local deck (allowClaude)" } };
    const json& ps = js::get(plan, "spec");
    SpawnSpec sp; sp.kind = "claude";
    if (js::truthy(js::get(ps, "account"))) sp.account = js::toString(js::get(ps, "account"));
    if (js::truthy(js::get(ps, "sessionId"))) sp.sessionId = js::toString(js::get(ps, "sessionId"));
    if (js::truthy(js::get(ps, "cwd"))) sp.cwd = js::toString(js::get(ps, "cwd"));
    sp.hasDesk = true; if (js::truthy(js::get(ps, "desk"))) sp.desk = js::toString(js::get(ps, "desk"));
    if (js::truthy(js::get(ps, "prompt"))) sp.prompt = js::toString(js::get(ps, "prompt"));
    if (js::truthy(js::get(ps, "model"))) sp.model = js::toString(js::get(ps, "model"));
    if (ps.is_object() && ps.contains("browser")) { sp.hasBrowser = true; if (ps["browser"].is_boolean()) sp.browser = ps["browser"].get<bool>(); }
    if (action == "wake") {
      if (!t) return { { "ok", false }, { "error", "the session did not start" } };
      // as wakeTile does: the frozen screen goes first (a queued wake of it, if any, finds it running and stands down)
      t->s.sleptAt = 0; t->s.waking = false;
      const std::string id = t->s.id;
      resetTile(id);
      spawnTile(sp, t); how = "woken";
      t = tileById(id);
    } else {
      t = tileById(spawnTile(sp, nullptr)); how = "opened";
      if (t && !(t->s.userTitle && !t->s.userTitle->empty()) && js::truthy(js::get(plan, "title"))) t->s.userTitle = js::toString(js::get(plan, "title"));
    }
    if (!t || !t->pty || !t->pid) {
      std::string why;
      if (t) { const std::string tail = lastUnits(js::trim(js::collapseSpaces(t->replay.all())), 160); if (!tail.empty()) why = " (" + tail + ")"; }
      return { { "ok", false }, { "error", "the session did not start" + why } };
    }
  }
  Entry* entry = nullptr;
  const std::string tid = t->s.id;
  json place = placeDeskTile(*t, &entry);
  t = tileById(tid);
  if (!t) return { { "ok", false }, { "error", "the session did not start" } };
  t->s.attentionAt = now();
  saveState();
  notifyDeskOpened(*t, entry, req, place, how);
  pushMeta();
  json out = { { "ok", true }, { "sessionId", optJ(t->s.sessionId) }, { "tileId", t->s.id }, { "pid", t->pid ? json(t->pid) : json() }, { "how", how } };
  for (auto it = place.begin(); it != place.end(); ++it) out[it.key()] = it.value();
  return out;
}

// ---------------------------------------------------------------- cloud sessions (main.js cloudSpec / cloudCreated / cloudTick / openCloud / cloudMeta)
// A new cloud session: its task, and the folder `claude --cloud` runs in. Checked here so the dialog can say what
// is wrong before a tile exists.
SpawnSpec D::cloudSpec(const json& s) {
  const std::string task = cloudCleanTask(js::get(s, "task"));
  if (task.empty()) throw std::runtime_error("a cloud session starts from a task: say what it should do");
  if (units(task) > CLOUD_TASK_MAX) throw std::runtime_error("the task is " + grouped(units(task)) + " characters; a command line takes at most " + grouped(CLOUD_TASK_MAX)
    + ". Put the detail in a file in the repo and point the task at it.");
  std::string cwd = js::trim(js::truthy(js::get(s, "cwd")) ? js::toString(js::get(s, "cwd")) : std::string());
  if (cwd.empty()) cwd = js::truthy(js::get(settings, "cwd")) ? js::toString(js::get(settings, "cwd")) : home;
  if (!sm.files->isDir(cwd)) throw std::runtime_error("no such folder: " + cwd);
  // THE SWITCH for making a session in Anthropic's cloud (not in main.js, which always may)
  if (!opt.allowClaude || !opt.allowCloudCreate) throw std::runtime_error("making cloud sessions is switched off in the native local deck (allowCloudCreate)");
  SpawnSpec sp; sp.kind = "cloud"; sp.task = task; sp.cwd = cwd;
  if (js::truthy(js::get(s, "account"))) sp.account = js::toString(js::get(s, "account"));
  return sp;
}
// `claude --cloud "<task>"` has exited. Made: the tile becomes that session (its pane turns into the page).
// Refused or stopped before it made one: the tile stays exited with the run on screen, and `retry` runs it again.
void D::cloudCreated(Tile& t) {
  if (t.startup) t.startup->finish();
  const json made = cloudParseCreated(t.replay.all());
  if (!made.is_object()) { logEvent("cloud", "tile " + id8(t.s.id) + ": no session made (exit " + (t.s.exitCode ? std::to_string(*t.s.exitCode) : std::string("?")) + ")"); return; }
  t.s.cloudId = js::str(js::get(made, "id"));
  logEvent("cloud", "tile " + id8(t.s.id) + " made " + (t.s.cloudId ? *t.s.cloudId : std::string()));
  if (js::truthy(js::get(made, "title"))) t.s.knownTitle = js::toString(js::get(made, "title"));
  t.s.exited = false; t.s.exitCode.reset(); t.s.lastStoppedAt = now();
  saveState();
  setTimer(300, [this] { cloudTick(); });        // its first status now, not at the next tick
}
void D::cloudTick() {
  if (!cloud || down || !cloudLive()) return;   // the public build's opt-in (core/deck/edition.hpp): off, no token is read
  std::vector<CloudTile> mine;
  for (const auto& t : tiles) if (t->s.kind == "cloud" && t->s.cloudId && !t->s.cloudId->empty()) mine.push_back({ *t->s.cloudId, t->s.account ? *t->s.account : std::string() });
  cloud->poll(mine, [this](bool changed) { if (changed) pushMeta(); });
}
void D::syncCloudAccounts() {
  if (!cloud) return;
  std::vector<CloudAccount> list;
  for (const Account& a : accounts) list.push_back({ a.id, a.dir });
  cloud->setAccounts(std::move(list));
}
// A cloud session (from Elsewhere) as a tile of its own account. Nothing is killed or moved.
std::string D::openCloud(const json& s) {
  const std::string account = js::truthy(js::get(s, "account")) ? js::toString(js::get(s, "account")) : std::string("undefined");
  const Account* acct = accountById(accounts, account);
  if (!acct) throw std::runtime_error("unknown account: " + account);
  const std::string cloudId = js::truthy(js::get(s, "cloudId")) ? js::toString(js::get(s, "cloudId")) : std::string();
  if (!isCloudId(cloudId)) throw std::runtime_error("not a cloud session id");
  for (const auto& t : tiles) if (t->s.cloudId && !t->s.cloudId->empty() && cloudKey(*t->s.cloudId) == cloudKey(cloudId)) throw std::runtime_error("already open here");
  SpawnSpec sp; sp.kind = "cloud"; sp.account = acct->id; sp.cloudId = cloudWebId(cloudId);
  sp.cwd = js::truthy(js::get(settings, "cwd")) ? js::toString(js::get(settings, "cwd")) : home;
  return spawnTile(sp, nullptr);
}
// The meta of a cloud tile, over the Claude fields it has no transcript for. `cloudId` is what a view keys its
// page on; `bridge` is the same id under the name the phone already opens in the Claude app.
json D::cloudMeta(const Tile& t, const json& c) const {
  const std::string id = t.s.cloudId && !t.s.cloudId->empty() ? cloudWebId(*t.s.cloudId) : std::string();
  json title;
  if (t.s.userTitle && !t.s.userTitle->empty()) title = *t.s.userTitle;
  else if (js::truthy(js::get(c, "title"))) title = js::get(c, "title");
  else if (t.s.knownTitle && !t.s.knownTitle->empty()) title = *t.s.knownTitle;
  else if (t.s.cloudTask && !t.s.cloudTask->empty()) title = js::sliceUnits(t.s.cloudTask->substr(0, t.s.cloudTask->find('\n')), 80);
  const json& ctx = js::get(c, "context");
  return { { "title", title }, { "lastText", js::truthy(js::get(c, "summary")) ? js::get(c, "summary") : json() }, { "model", js::truthy(js::get(c, "model")) ? js::get(c, "model") : json() },
    { "effort", nullptr }, { "ultracode", false }, { "context", ctx.is_null() ? json() : ctx },
    { "lastAt", js::truthy(js::get(c, "lastAt")) ? js::get(c, "lastAt") : numJ(t.s.startedAt) },
    { "bridge", id.empty() ? json() : json(id) }, { "cloudId", id.empty() ? json() : json(id) }, { "cloudUrl", id.empty() ? json() : json(cloudWebUrl(id)) },
    { "cloudError", js::truthy(js::get(c, "error")) ? js::get(c, "error") : json() },
    // closing the tile loses nothing: the session keeps running in the cloud, and Elsewhere lists it
    { "fresh", true } };
}

// ---------------------------------------------------------------- the public face's new parts
std::string Deck::askPage(const std::string& pageId) { return d->asks && !d->down ? d->asks->pagePath(pageId) : std::string(); }
std::string Deck::viewFile(const std::string& folderId, const std::vector<std::string>& parts) { return d->down ? std::string() : d->viewer.fileFor(*d->sm.files, folderId, parts); }
std::vector<json> Deck::drainDeskInbox() { return d->deskInbox && !d->down ? d->deskInbox->drain() : std::vector<json>(); }
json Deck::relaunchSpec() const {
  std::string text;
  if (!d->sm.files->read(joinPath(d->stateDir, "relaunch.json"), text)) return json();
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
  const json r = json::parse(text, nullptr, false);
  const json& cmd = js::get(r, "command");
  if (!cmd.is_string() || js::trim(cmd.get<std::string>()).empty()) return json();
  json args = json::array();
  if (js::get(r, "args").is_array()) for (const json& a : js::get(r, "args")) args.push_back(a.is_string() ? a.get<std::string>() : js::toString(a));
  return { { "command", js::trim(cmd.get<std::string>()) }, { "args", args } };
}
// main.js note.on('click'): the window that holds the tile (else the first) comes up — the shell's part — and is
// told which tile to activate, or whose questions to open
ClientId Deck::toastClicked(const Toast& toast) {
  if (d->down) return 0;
  Entry* w = toast.tileId.empty() ? nullptr : d->windowHolding(toast.tileId);
  if (!w || w->remote) w = d->primaryEntry();
  if (!w) return 0;
  if (toast.kind == "ask") d->sendToEntry(w, "asks:open", { { "sessionId", toast.sessionId }, { "tileId", toast.tileId.empty() ? json() : json(toast.tileId) } });
  else if (!toast.tileId.empty()) d->sendToEntry(w, "tiles:activate", toast.tileId);
  return w->client;
}

}}  // namespace deck::core
