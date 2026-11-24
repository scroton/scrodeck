// mac/Sources/DeckCoreBridge/DeckBridge.cpp — see include/DeckBridge.h. The deck itself is core/deck (pure); this
// file is the wiring a shell needs around it, with a C ABI on the outside:
//   - a DBOptions becomes core::DeckOptions + core::ServerOptions in ONE place (buildOptions), resolved through
//     core/deck/platform.hpp exactly as lib/platform.js decides (the Mac answers come from Env::os() == "darwin");
//   - the host's inbox is drained into the core, then Deck::tick + Server::tick (the pump);
//   - every window is a client with its own queue of frames, delivered when the outermost call returns;
//   - the frames a shell sends are built by core/proto/frames.hpp, and go to the deck directly (local) or out as
//     text (remote) — the shell's code is the same in both modes;
//   - core/vt, core/model/layout, the key table and the mouse table as plain functions.
// Windows counterpart: windows/host/localdeck.cpp (buildDeckOptions, the pump), windows/src/deckhost.hpp (windows as
// clients), windows/src/keys.hpp (the key table), windows/src/mouse.hpp (included as it is).
// PURE C++17, no OS header. Built and tested on Windows with g++: mac/Tests/build.ps1.
#include "DeckBridge.h"
#include "DeckBridgeHost.hpp"

#include "core/deck/asks.hpp"
#include "core/deck/deck.hpp"
#include "core/deck/platform.hpp"
#include "core/deck/server.hpp"
#include "core/deck/util.hpp"
#include "core/deck/version.hpp"
#include "core/model/layout.hpp"
#include "core/proto/frames.hpp"
#include "core/vt/terminal.hpp"
#include "windows/src/mouse.hpp"        // pure (no windows.h): xterm's mouse service, shared with the Windows shell

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;
namespace core = deck::core;
namespace layout = deck::layout;
namespace proto = deck::proto;
namespace vt = deck::vt;

namespace {
inline std::string S(const char* s) { return s ? std::string(s) : std::string(); }
inline std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }
inline json parseOr(const char* text, json fallback) {
  if (!text || !*text) return fallback;
  json j = json::parse(text, nullptr, false);
  return j.is_discarded() ? fallback : j;
}
inline void setErr(char* err, size_t cap, const std::string& what) { if (err && cap) std::snprintf(err, cap, "%s", what.c_str()); }
const json& field(const json& o, const char* k) { static const json none; if (!o.is_object()) return none; auto it = o.find(k); return it == o.end() ? none : *it; }
const std::string& strField(const json& o, const char* k) { static const std::string e; const json& v = field(o, k); return v.is_string() ? v.get_ref<const std::string&>() : e; }
}  // namespace

// the header's literals are the core's own values (a change there must fail HERE, not on a Mac)
static_assert(DB_BOLD == vt::Bold && DB_DIM == vt::Dim && DB_ITALIC == vt::Italic && DB_UNDERLINE == vt::Underline && DB_BLINK == vt::Blink && DB_INVERSE == vt::Inverse
  && DB_INVISIBLE == vt::Invisible && DB_STRIKE == vt::Strike && DB_OVERLINE == vt::Overline && DB_UNDERLINE_STYLE_SHIFT == vt::UnderlineStyleShift
  && DB_UNDERLINE_STYLE_MASK == vt::UnderlineStyleMask && DB_PROTECTED == vt::Protected && DB_HYPERLINK == vt::Hyperlink, "DBCell.flags must be core/vt CellFlags");
static_assert(DB_BTN_LEFT == deck::mouse::LEFT && DB_BTN_MIDDLE == deck::mouse::MIDDLE && DB_BTN_RIGHT == deck::mouse::RIGHT && DB_BTN_NONE == deck::mouse::NONE && DB_BTN_WHEEL == deck::mouse::WHEEL
  && DB_ACT_UP == deck::mouse::UP && DB_ACT_DOWN == deck::mouse::DOWN && DB_ACT_LEFT == deck::mouse::WLEFT && DB_ACT_RIGHT == deck::mouse::WRIGHT && DB_ACT_MOVE == deck::mouse::MOVE, "mouse constants must be windows/src/mouse.hpp's");
static_assert(DB_MAX_CELLS == layout::kMax * layout::kMax, "DB_MAX_CELLS is the largest grid");
static_assert(sizeof(DBCell) == 16, "DBCell is 16 bytes (the Swift renderer reads rows of them)");

// ---------------------------------------------------------------------------------------------- structs
struct DBConn {
  DBDeck* deck = nullptr;                 // local: the deck it is a window of
  core::ClientId client = 0;
  DBSink sink{ nullptr, nullptr };
  std::deque<json> out;                   // local: frames waiting for the outermost call to return
  bool closed = false;
  void (*sendText)(void*, const char*, size_t) = nullptr; void* sendCtx = nullptr;   // remote
};

namespace {
struct ShellNotifier : core::Notifier {
  DBShellCallbacks* cb = nullptr;
  void toast(const core::Toast& t) override {
    if (!cb || !cb->toast) return;
    DBToast d{ t.title.c_str(), t.body.c_str(), t.silent ? 1 : 0, t.kind.c_str(), t.tileId.c_str(), t.sessionId.c_str() };
    cb->toast(cb->ctx, &d);
  }
  void flash(int w) override { if (cb && cb->flash) cb->flash(cb->ctx, w); }
};
struct ShellLifecycle : core::Lifecycle {
  DBShellCallbacks* cb = nullptr;
  bool restart(const std::string& why) override { return cb && cb->restart ? cb->restart(cb->ctx, why.c_str()) != 0 : false; }
  json fingerprint() override {
    if (!cb || !cb->fingerprint_json) return json();
    const char* t = cb->fingerprint_json(cb->ctx);
    return parseOr(t, json());
  }
  void armExitWatchdog(double ms) override { if (cb && cb->arm_exit_watchdog) cb->arm_exit_watchdog(cb->ctx, ms); }
  bool openExternal(const std::string& u) override { return cb && cb->open_external ? cb->open_external(cb->ctx, u.c_str()) != 0 : false; }
  bool openPath(const std::string& p) override { return cb && cb->open_path ? cb->open_path(cb->ctx, p.c_str()) != 0 : false; }
  bool showInFolder(const std::string& p) override { return cb && cb->show_in_folder ? cb->show_in_folder(cb->ctx, p.c_str()) != 0 : false; }
  bool openWindow(int client, const json& bounds, bool inactive) override {
    if (!cb || !cb->open_window) return false;
    const std::string b = dump(bounds);
    return cb->open_window(cb->ctx, client, b.c_str(), inactive ? 1 : 0) != 0;
  }
};
}  // namespace

struct DBDeck {
  DBHost* host = nullptr;
  DBShellCallbacks shell{};
  ShellNotifier notifier; ShellLifecycle lifecycle;
  std::unique_ptr<core::Server> server;   // declared before the deck: the deck is destroyed first
  std::unique_ptr<core::Deck> deck;
  std::vector<DBConn*> conns, dead;
  DBConn* adopting = nullptr;             // a window being added: frames for a client not yet in `conns`
  core::ClientId adoptingClient = 0;
  int depth = 0; bool flushing = false, down = false, allowClaude = false;
  std::string stateDir, scratch;
  std::vector<std::string> askDownloads;  // the ask feed's Done folders, as buildOptions resolved them

  DBConn* byClient(core::ClientId c) { for (DBConn* k : conns) if (k->client == c && !k->closed) return k; return nullptr; }
  void flush();
  void drain();
};

namespace {
void deliver(DBConn* c, const json& f) {
  if (!c->sink.frame) return;
  DBFrame d; std::memset(&d, 0, sizeof d);
  static const std::string kNull = "null";
  d.id = ""; d.name = ""; d.json = kNull.c_str(); d.json_len = kNull.size(); d.bytes = "";
  const std::string& t = strField(f, "t");
  const std::string& id = strField(f, "id");
  d.id = id.c_str();
  const json& rid = field(f, "rid");
  d.rid = rid.is_number() ? rid.get<int>() : 0;
  std::string text;
  if (t == "data") {
    d.kind = DB_FRAME_DATA;
    const std::string& bytes = strField(f, "d");
    d.bytes = bytes.data(); d.bytes_len = bytes.size();
    const json& r = field(f, "replay"); d.replay = r.is_boolean() && r.get<bool>() ? 1 : 0;
  } else if (t == "meta" || t == "init") {
    d.kind = t == "meta" ? DB_FRAME_META : DB_FRAME_INIT;
    text = dump(f);
  } else if (t == "ok") {
    d.kind = DB_FRAME_OK; text = dump(field(f, "result"));
  } else if (t == "err") {
    d.kind = DB_FRAME_ERR; d.name = strField(f, "error").c_str();
  } else if (t == "event") {
    d.kind = DB_FRAME_EVENT; d.name = strField(f, "name").c_str(); text = dump(field(f, "payload"));
  } else if (t == "exit") {
    d.kind = DB_FRAME_EXIT; const json& e = field(f, "exitCode"); d.exit_code = e.is_number() ? e.get<int>() : 0;
  } else if (t == "reset") {
    d.kind = DB_FRAME_RESET;
  } else if (t == "open") {
    d.kind = DB_FRAME_OPEN;               // the bridge's own: never on a wire
  } else {
    d.kind = DB_FRAME_UNKNOWN; text = dump(f);
  }
  if (!text.empty()) { d.json = text.c_str(); d.json_len = text.size(); }
  c->sink.frame(c->sink.ctx, &d);
}

// Every public entry that can make the core speak holds one of these: frames queue while it lives and are
// delivered when the OUTERMOST one goes (never from inside a callback's own nested call).
struct Enter {
  DBDeck* d;
  explicit Enter(DBDeck* deck) : d(deck) { if (d) d->depth++; }
  ~Enter() { if (d && --d->depth == 0 && !d->flushing) d->flush(); }
};
}  // namespace

void DBDeck::flush() {
  flushing = true;
  for (bool progress = true; progress;) {
    progress = false;
    for (size_t i = 0; i < conns.size(); i++) {           // (a callback may add or close windows: index, re-read)
      DBConn* c = conns[i];
      while (!c->closed && !c->out.empty()) {
        const json f = std::move(c->out.front()); c->out.pop_front();
        try { deliver(c, f); } catch (...) {}
        progress = true;
      }
    }
  }
  flushing = false;
  for (DBConn* c : dead) delete c;
  dead.clear();
}

void DBDeck::drain() {
  std::deque<db::Inbox::Event> q;
  host->inbox->take(q);
  for (db::Inbox::Event& ev : q) {
    if (down) return;
    switch (ev.kind) {
      case db::Inbox::PtyData: deck->onPtyData(ev.id, ev.bytes.data(), ev.bytes.size()); break;
      case db::Inbox::PtyExit: deck->onPtyExit(ev.id, ev.code); break;
      case db::Inbox::NetAccept: if (server) server->onAccept(ev.id); break;
      case db::Inbox::NetBytes: if (server) server->onBytes(ev.id, ev.bytes.data(), ev.bytes.size()); break;
      case db::Inbox::NetClosed: if (server) server->onClosed(ev.id); break;
      case db::Inbox::Task: if (ev.task) ev.task(); break;
    }
  }
}

// ---------------------------------------------------------------------------------------------- misc
int db_abi_version(void) { return 1; }
const char* db_core_version(void) { return core::kNativeDeckVersion; }
void db_host_set_wake(DBHost* host, void (*wake)(void*), void* ctx) { if (host) host->inbox->setWake(wake, ctx); }
void db_host_destroy(DBHost* host) { delete host; }

void db_options_init(DBOptions* o) {
  if (!o) return;
  std::memset(o, 0, sizeof *o);
  o->desk_open_inbox = -1; o->remote_desk_is_a_window = -1; o->driver_claim = -1;
}

// ---------------------------------------------------------------------------------------------- the deck
namespace {
// THE ONE PLACE a DBOptions becomes the core's options (windows/host/localdeck.cpp buildDeckOptions is the Windows
// twin and should come here: mac/README.md "What the Windows shell could share").
void buildOptions(const DBOptions& in, DBHost& h, core::DeckOptions& o, core::ServerOptions& so) {
  core::Env& env = *h.env; core::Files& files = *h.files;
  const std::string os = env.os();
  o.stateDir = S(in.state_dir);
  o.home = !S(in.home).empty() ? S(in.home) : env.home();
  o.aliasFile = !S(in.alias_file).empty() ? S(in.alias_file) : core::platform::aliasFiles(env, files).primary;
  o.allowClaude = in.allow_claude != 0; o.persistShells = in.persist_shells != 0; o.allowAccountEdit = in.allow_account_edit != 0;
  const core::platform::Shell sh = core::platform::shellCommand(env);
  o.shellFile = sh.file; o.shellArgs = sh.args; o.shellName = sh.name;
  o.claudeExe = core::platform::claudeExe(env, files, !S(in.claude_exe).empty() ? S(in.claude_exe) : env.get("CLAUDE_DECK_CLAUDE"));
  o.bash = core::platform::bashPath(env, files, env.get("CLAUDE_DECK_BASH"));
  o.cwdOverride = env.get("CLAUDE_DECK_CWD");
  o.version = !S(in.version).empty() ? S(in.version) : std::string(core::kNativeDeckVersion);
  if (in.remote_desk_is_a_window >= 0) o.remoteDeskIsAWindow = in.remote_desk_is_a_window != 0;
  if (in.driver_claim >= 0) o.driverClaim = in.driver_claim != 0;
  if (!S(in.ask_state_dir).empty()) {
    o.askStateDir = S(in.ask_state_dir);
    const std::string all = S(in.ask_downloads);
    for (size_t p = 0; p <= all.size();) { const size_t q = std::min(all.find('\n', p), all.size()); if (q > p) o.askDownloads.push_back(all.substr(p, q - p)); p = q + 1; }
  } else if (in.asks) {
    const core::AskLocations loc = core::askLocations(files, env);
    o.askStateDir = loc.stateDir; o.askDownloads = loc.downloads;
  }
  if (in.desk_open_inbox >= 0) o.deskOpenInbox = in.desk_open_inbox != 0;
  o.cloudPolling = in.cloud_polling != 0; o.allowCloudCreate = in.allow_cloud_create != 0;
  o.usagePolling = in.usage_polling != 0;
  o.keyHelp = core::platform::keyHelp(os);
  o.fontStack = core::platform::defaultFontStack(os);
  so.assetRoot = S(in.asset_root);
  so.forcePort = in.serve_port;
  o.userswitch = core::platform::userswitchPath(env, files, so.assetRoot, S(in.resources_dir));
}
}  // namespace

DBDeck* db_deck_create(const DBOptions* options, DBHost* host, const DBShellCallbacks* shell, char* err, size_t err_cap) {
  if (err && err_cap) err[0] = 0;
  if (!options || !host) { setErr(err, err_cap, "no options or no host"); return nullptr; }
  if (!host->clock || !host->files || !host->ptys || !host->procs || !host->random || !host->env) { setErr(err, err_cap, "the host is missing a required seam (clock, files, ptys, procs, random, env)"); return nullptr; }
  if (S(options->state_dir).empty()) { setErr(err, err_cap, "state_dir is required"); return nullptr; }
  try {
    // paths: '/' everywhere but Windows (core/deck/util.hpp: said once, before a deck is made)
    core::setPathSep(host->env->os() == "win32" ? '\\' : '/');
    std::unique_ptr<DBDeck> d(new DBDeck);
    d->host = host;
    if (shell) d->shell = *shell;
    d->notifier.cb = &d->shell; d->lifecycle.cb = &d->shell;
    core::DeckOptions o; core::ServerOptions so;
    buildOptions(*options, *host, o, so);
    d->stateDir = o.stateDir; d->allowClaude = o.allowClaude; d->askDownloads = o.askDownloads;

    core::Seams sm;
    sm.clock = host->clock; sm.files = host->files; sm.ptys = host->ptys; sm.procs = host->procs; sm.random = host->random; sm.env = host->env;
    if (shell) { sm.notifier = &d->notifier; sm.lifecycle = &d->lifecycle; }
    if (host->listener && host->net) {
      d->server.reset(new core::Server(so, *host->files, *host->clock, *host->listener, *host->net));
      sm.phone = d->server.get();
    }
    // Usage / cloud polling read the accounts' sign-in tokens: only when the shell asked AND the host has both seams
    if ((options->usage_polling || options->cloud_polling) && host->http && host->credentials) { sm.http = host->http; sm.credentials = host->credentials; }

    DBDeck* self = d.get();
    d->deck.reset(new core::Deck(o, sm, [self](core::ClientId c, const json& frame) {
      DBConn* k = self->byClient(c);
      if (!k && self->adopting && (self->adoptingClient == 0 || self->adoptingClient == c)) k = self->adopting;
      if (k) { k->out.push_back(frame); return true; }
      return self->server ? self->server->deliver(c, frame) : false;
    }));
    if (d->server) d->server->attach(d->deck.get());
    return d.release();
  } catch (const std::exception& e) { setErr(err, err_cap, e.what()); }
  catch (...) { setErr(err, err_cap, "unknown error"); }
  return nullptr;
}

void db_deck_start(DBDeck* d) { if (!d || d->down) return; Enter e(d); try { d->deck->start(); } catch (...) {} }
void db_deck_pump(DBDeck* d) {
  if (!d || d->down) return;
  Enter e(d);
  try {
    d->drain(); d->deck->tick();
    if (d->server) d->server->tick();
    d->drain();
  } catch (...) {}
}
int db_deck_next_due_ms(const DBDeck* d) {
  if (!d || d->down) return 2000;
  int ms = d->deck->nextDueMs();
  if (d->server && d->server->running()) ms = std::min(ms, 1000);       // the heartbeat's clock
  return ms;
}
void db_deck_shutdown(DBDeck* d) {
  if (!d || d->down) return;
  d->down = true;
  try {
    if (d->server) d->server->stop();
    if (d->host->listener) d->host->listener->stop();
    d->deck->shutdown();
    d->host->quiesce();
  } catch (...) {}
}
void db_deck_destroy(DBDeck* d) {
  if (!d) return;
  db_deck_shutdown(d);
  d->deck.reset(); d->server.reset();
  for (DBConn* c : d->conns) delete c;
  for (DBConn* c : d->dead) delete c;
  delete d;
}

namespace {
DBConn* newLocal(DBDeck* d, const DBSink* sink) {
  DBConn* c = new DBConn; c->deck = d; if (sink) c->sink = *sink;
  c->out.push_back(json{ { "t", "open" } });
  return c;
}
}  // namespace
DBConn* db_deck_add_window(DBDeck* d, const char* bounds_json, const DBSink* sink) {
  if (!d || d->down) return nullptr;
  Enter e(d);
  DBConn* c = newLocal(d, sink);
  try {
    d->adopting = c; d->adoptingClient = 0;
    c->client = d->deck->addWindow(parseOr(bounds_json, json()));
    d->adopting = nullptr;
  } catch (...) { d->adopting = nullptr; delete c; return nullptr; }
  d->conns.push_back(c);
  return c;
}
DBConn* db_deck_open_window(DBDeck* d, const char* tile_id, const char* bounds_json, const DBSink* sink) {
  if (!d || d->down) return nullptr;
  Enter e(d);
  DBConn* c = newLocal(d, sink);
  try {
    d->adopting = c; d->adoptingClient = 0;
    c->client = d->deck->openWindow(S(tile_id), parseOr(bounds_json, json()));
    d->adopting = nullptr;
  } catch (...) { d->adopting = nullptr; c->client = 0; }
  if (!c->client) { delete c; return nullptr; }
  d->conns.push_back(c);
  return c;
}
DBConn* db_deck_adopt_window(DBDeck* d, int client, const DBSink* sink) {
  if (!d || d->down || client <= 0 || d->byClient(client)) return nullptr;
  Enter e(d);
  DBConn* c = newLocal(d, sink);
  c->client = client;
  d->conns.push_back(c);
  return c;
}
int db_deck_toast_clicked(DBDeck* d, const DBToast* t) {
  if (!d || d->down || !t) return 0;
  Enter e(d);
  core::Toast toast; toast.title = S(t->title); toast.body = S(t->body); toast.silent = t->silent != 0; toast.kind = S(t->kind); toast.tileId = S(t->tile_id); toast.sessionId = S(t->session_id);
  try { return d->deck->toastClicked(toast); } catch (...) { return 0; }
}
const char* db_deck_phone_status_json(DBDeck* d) {
  if (!d) return "null";
  try { d->scratch = d->server && !d->down ? dump(d->server->status(field(d->deck->settings(), "phone"))) : std::string("null"); } catch (...) { d->scratch = "null"; }
  return d->scratch.c_str();
}
const char* db_deck_saved_windows_json(DBDeck* d) { if (!d) return "[]"; try { d->scratch = dump(d->deck->savedWindows()); } catch (...) { d->scratch = "[]"; } return d->scratch.c_str(); }
const char* db_deck_settings_json(DBDeck* d) { if (!d) return "null"; try { d->scratch = dump(d->deck->settings()); } catch (...) { d->scratch = "null"; } return d->scratch.c_str(); }
const char* db_deck_relaunch_spec_json(DBDeck* d) { if (!d) return "null"; try { d->scratch = dump(d->deck->relaunchSpec()); } catch (...) { d->scratch = "null"; } return d->scratch.c_str(); }
const char* db_deck_state_dir(DBDeck* d) { return d ? d->stateDir.c_str() : ""; }
const char* db_deck_ask_downloads_json(DBDeck* d) {
  if (!d) return "[]";
  try { d->scratch = dump(json(d->askDownloads)); } catch (...) { d->scratch = "[]"; }
  return d->scratch.c_str();
}
int db_deck_live_count(DBDeck* d) { if (!d || d->down) return 0; try { return (int)d->deck->livePids().size(); } catch (...) { return 0; } }
int db_deck_allow_claude(const DBDeck* d) { return d && d->allowClaude ? 1 : 0; }
void db_deck_pty_size(DBDeck* d, const char* tile_id, int* cols, int* rows) {
  std::pair<int, int> s{ 0, 0 };
  if (d && !d->down) { try { s = d->deck->ptySize(S(tile_id)); } catch (...) {} }
  if (cols) *cols = s.first;
  if (rows) *rows = s.second;
}

// ---------------------------------------------------------------------------------------------- connections
DBConn* db_conn_remote(const DBSink* sink, void (*send_text)(void*, const char*, size_t), void* send_ctx) {
  DBConn* c = new DBConn; if (sink) c->sink = *sink;
  c->sendText = send_text; c->sendCtx = send_ctx;
  return c;
}
void db_conn_receive_text(DBConn* c, const char* text, size_t n) {
  if (!c || c->deck || c->closed || !text) return;
  try {
    const json f = json::parse(text, text + n, nullptr, false);
    if (f.is_object()) deliver(c, f);
  } catch (...) {}
}
void db_conn_close(DBConn* c) {
  if (!c || c->closed) return;
  c->closed = true;
  DBDeck* d = c->deck;
  if (!d) { delete c; return; }
  d->conns.erase(std::remove(d->conns.begin(), d->conns.end(), c), d->conns.end());
  d->dead.push_back(c);                                   // freed after the flush that may be walking it
  Enter e(d);
  if (!d->down && c->client) { try { d->deck->removeClient(c->client); } catch (...) {} }
}
int db_conn_is_local(const DBConn* c) { return c && c->deck ? 1 : 0; }
int db_conn_client(const DBConn* c) { return c && c->deck ? c->client : 0; }
int db_conn_window_index(const DBConn* c) { if (!c || !c->deck || c->deck->down) return -1; try { return c->deck->deck->windowIndex(c->client); } catch (...) { return -1; } }
void db_conn_window_state(DBConn* c, int visible, int focused) { if (!c || !c->deck || c->deck->down || c->closed) return; Enter e(c->deck); try { c->deck->deck->windowState(c->client, visible != 0, focused != 0); } catch (...) {} }
void db_conn_window_bounds(DBConn* c, const char* bounds_json) { if (!c || !c->deck || c->deck->down || c->closed) return; Enter e(c->deck); try { c->deck->deck->windowBounds(c->client, parseOr(bounds_json, json())); } catch (...) {} }
void db_conn_window_key(DBConn* c) { if (!c || !c->deck || c->deck->down || c->closed) return; Enter e(c->deck); try { c->deck->deck->windowKey(c->client); } catch (...) {} }

namespace {
void sendFrame(DBConn* c, const json& f) {
  if (!c || c->closed || f.is_null()) return;
  if (c->deck) {
    if (c->deck->down) return;
    Enter e(c->deck);
    try { c->deck->deck->handle(c->client, f); } catch (...) {}
  } else if (c->sendText) {
    const std::string t = proto::encode(f);
    c->sendText(c->sendCtx, t.data(), t.size());
  }
}
}  // namespace
void db_conn_call(DBConn* c, int rid, const char* fn, const char* args_json) {
  json args = parseOr(args_json, json::array());
  sendFrame(c, proto::call(rid, S(fn), std::move(args)));
}
void db_conn_attach(DBConn* c, const char* id, int since) { sendFrame(c, proto::attach(S(id), since != 0)); }
void db_conn_detach(DBConn* c, const char* id) { sendFrame(c, id ? proto::detach(S(id)) : proto::detachAll()); }
void db_conn_input(DBConn* c, const char* id, const char* bytes, size_t n) { if (bytes && n) sendFrame(c, proto::input(S(id), std::string(bytes, n))); }
int db_conn_wheel(DBConn* c, const char* id, const char* report, size_t n) {
  if (!report) return 0;
  const json f = proto::wheel(S(id), std::string(report, n));
  if (f.is_null()) return 0;
  sendFrame(c, f); return 1;
}
void db_conn_resize(DBConn* c, const char* id, int cols, int rows) { sendFrame(c, proto::resize(S(id), cols, rows)); }
void db_conn_shown(DBConn* c, const char* const* ids, int count, int hidden) {
  std::vector<std::string> v;
  for (int i = 0; ids && i < count; i++) v.push_back(S(ids[i]));
  sendFrame(c, proto::shown(v, hidden != 0));
}
void db_conn_leaving(DBConn* c, const char* id, int on) { sendFrame(c, proto::leaving(S(id), on != 0)); }
int db_frame_synthesised(const char* bytes, size_t n) { return bytes && proto::synthesised(std::string(bytes, n)) ? 1 : 0; }

// ---------------------------------------------------------------------------------------------- terminals
struct DBTerm { vt::Terminal t; std::string scratch; DBTerm(int c, int r, int sb) : t(c, r, sb) {} };

DBTerm* db_term_create(int cols, int rows, int scrollback) { try { return new DBTerm(cols, rows, scrollback); } catch (...) { return nullptr; } }
void db_term_destroy(DBTerm* t) { delete t; }
void db_term_feed(DBTerm* t, const char* bytes, size_t n) { if (t && bytes && n) { try { t->t.feed(bytes, n); } catch (...) {} } }
void db_term_resize(DBTerm* t, int cols, int rows) { if (t) { try { t->t.resize(cols, rows); } catch (...) {} } }
void db_term_reset(DBTerm* t) { if (t) { try { t->t.reset(); } catch (...) {} } }
void db_term_state(const DBTerm* t, DBTermState* o) {
  if (!o) return;
  std::memset(o, 0, sizeof *o);
  if (!t) return;
  const vt::Terminal& v = t->t;
  o->cols = v.cols(); o->rows = v.rows(); o->scrollback = v.scrollbackSize();
  o->cursor_x = v.cursorX(); o->cursor_y = v.cursorY(); o->cursor_visible = v.cursorVisible(); o->cursor_shape = v.cursorShape(); o->cursor_blink = v.cursorBlink();
  o->alt_screen = v.altScreen(); o->app_cursor_keys = v.appCursorKeys(); o->app_keypad = v.appKeypad(); o->bracketed_paste = v.bracketedPaste();
  o->mouse_mode = v.mouseMode(); o->mouse_encoding = v.mouseEncoding(); o->focus_reporting = v.focusReporting(); o->synchronized_output = v.synchronizedOutput();
}
int db_term_take_damage(DBTerm* t, int* first, int* last) {
  int a = 0, b = 0;
  if (!t || !t->t.takeDamage(a, b)) return 0;
  if (first) *first = a;
  if (last) *last = b;
  return 1;
}
int db_term_take_bell(DBTerm* t) { return t && t->t.takeBell() ? 1 : 0; }
const char* db_term_title(DBTerm* t) { return t ? t->t.title().c_str() : ""; }
const char* db_term_take_replies(DBTerm* t, size_t* n) {
  if (!t) { if (n) *n = 0; return ""; }
  t->scratch = t->t.takeReplies();
  if (n) *n = t->scratch.size();
  return t->scratch.data();
}
namespace {
const vt::Line* lineAt(const vt::Terminal& v, int abs) {
  const int sb = v.scrollbackSize();
  if (abs < 0 || abs >= sb + v.rows()) return nullptr;
  return abs < sb ? &v.scrollbackLine(abs) : &v.line(abs - sb);
}
inline uint32_t packColor(const vt::Color& c) {
  return c.kind == vt::Color::Default ? DB_COLOR_DEFAULT : c.kind == vt::Color::Palette ? (DB_COLOR_PALETTE | (c.value & 0xFF)) : (c.value & 0xFFFFFF);
}
void appendUtf8(std::string& s, char32_t c) {
  if (c < 0x80) s.push_back((char)c);
  else if (c < 0x800) { s.push_back((char)(0xC0 | (c >> 6))); s.push_back((char)(0x80 | (c & 0x3F))); }
  else if (c < 0x10000) { s.push_back((char)(0xE0 | (c >> 12))); s.push_back((char)(0x80 | ((c >> 6) & 0x3F))); s.push_back((char)(0x80 | (c & 0x3F))); }
  else { s.push_back((char)(0xF0 | (c >> 18))); s.push_back((char)(0x80 | ((c >> 12) & 0x3F))); s.push_back((char)(0x80 | ((c >> 6) & 0x3F))); s.push_back((char)(0x80 | (c & 0x3F))); }
}
}  // namespace
int db_term_read_line(const DBTerm* t, int abs_line, DBCell* out, int cap, int* wrapped) {
  if (wrapped) *wrapped = 0;
  if (!t || !out || cap <= 0) return 0;
  const vt::Line* l = lineAt(t->t, abs_line);
  if (!l) return 0;
  if (wrapped) *wrapped = l->wrapped ? 1 : 0;
  const int n = std::min(std::min(t->t.cols(), cap), (int)l->cells.size());
  for (int i = 0; i < n; i++) {
    const vt::Cell& c = l->cells[(size_t)i];
    out[i].ch = (uint32_t)c.ch; out[i].fg = packColor(c.fg); out[i].bg = packColor(c.bg);
    out[i].flags = c.flags; out[i].width = c.width; out[i].cluster = c.extra ? 1 : 0;
  }
  return n;
}
int db_term_cell_cluster(const DBTerm* t, int abs_line, int col, uint32_t* out, int cap) {
  if (!t || !out || cap <= 0) return 0;
  const vt::Line* l = lineAt(t->t, abs_line);
  if (!l || col < 0 || col >= (int)l->cells.size()) return 0;
  const std::u32string s = t->t.cellText(l->cells[(size_t)col]);
  const int n = std::min((int)s.size(), cap);
  for (int i = 0; i < n; i++) out[i] = (uint32_t)s[(size_t)i];
  return n;
}
const char* db_term_text(DBTerm* t, int x0, int y0, int x1, int y1, size_t* n) {
  if (n) *n = 0;
  if (!t) return "";
  std::string& s = t->scratch; s.clear();
  const vt::Terminal& v = t->t;
  const int cols = v.cols();
  for (int y = y0; y <= y1; y++) {
    const vt::Line* l = lineAt(v, y);
    if (!l) continue;
    if (y > y0 && !l->wrapped) s.push_back('\n');
    const int from = y == y0 ? std::max(0, x0) : 0, to = y == y1 ? std::min(cols - 1, x1) : cols - 1;
    std::string row; size_t keep = 0;
    for (int x = from; x <= to && x < (int)l->cells.size(); x++) {
      const vt::Cell& c = l->cells[(size_t)x];
      if (c.width == 0) continue;                           // the right half of a wide character
      if (c.ch == 0) { row.push_back(' '); continue; }
      for (char32_t u : v.cellText(c)) appendUtf8(row, u);
      if (c.ch != U' ') keep = row.size();
    }
    // a line that the next one continues keeps its blanks (they are inside the text); any other loses its tail
    const vt::Line* next = lineAt(v, y + 1);
    if (!(next && next->wrapped && y < y1)) row.resize(keep);
    s += row;
  }
  if (n) *n = s.size();
  return s.c_str();
}
void db_term_set_theme_color(DBTerm* t, int index, uint32_t rgb) { if (t) t->t.setThemeColor(index, rgb); }

// ---------------------------------------------------------------------------------------------- keys
// windows/src/keys.hpp keyToBytes, with DB_KEY_* in place of VK_*. Kept identical case for case: the bridge test
// checks every row both files answer.
int db_key_bytes(int key, int mods, int app_cursor, char* out, int cap) {
  const bool shift = (mods & DB_MOD_SHIFT) != 0, alt = (mods & DB_MOD_ALT) != 0, ctrl = (mods & DB_MOD_CTRL) != 0;
  const bool any = shift || alt || ctrl;
  const int code = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
  std::string o; const std::string esc = "\x1b";
  auto csiMod = [&](const char* final1, const char* plain) { if (any) o = esc + "[1;" + std::to_string(code) + final1; else o = plain; };
  auto tilde = [&](int n) { if (any) o = esc + "[" + std::to_string(n) + ";" + std::to_string(code) + "~"; else o = esc + "[" + std::to_string(n) + "~"; };
  switch (key) {
    case DB_KEY_UP:    csiMod("A", app_cursor ? "\x1bOA" : "\x1b[A"); break;
    case DB_KEY_DOWN:  csiMod("B", app_cursor ? "\x1bOB" : "\x1b[B"); break;
    case DB_KEY_RIGHT: csiMod("C", app_cursor ? "\x1bOC" : "\x1b[C"); break;
    case DB_KEY_LEFT:  csiMod("D", app_cursor ? "\x1bOD" : "\x1b[D"); break;
    case DB_KEY_HOME:  csiMod("H", app_cursor ? "\x1bOH" : "\x1b[H"); break;
    case DB_KEY_END:   csiMod("F", app_cursor ? "\x1bOF" : "\x1b[F"); break;
    case DB_KEY_INSERT: if (shift || ctrl) return -1; tilde(2); break;
    case DB_KEY_DELETE: tilde(3); break;
    case DB_KEY_PAGE_UP: tilde(5); break;
    case DB_KEY_PAGE_DOWN: tilde(6); break;
    case DB_KEY_F1: csiMod("P", "\x1bOP"); break;
    case DB_KEY_F2: csiMod("Q", "\x1bOQ"); break;
    case DB_KEY_F3: csiMod("R", "\x1bOR"); break;
    case DB_KEY_F4: csiMod("S", "\x1bOS"); break;
    case DB_KEY_F5: tilde(15); break;
    case DB_KEY_F6: tilde(17); break;
    case DB_KEY_F7: tilde(18); break;
    case DB_KEY_F8: tilde(19); break;
    case DB_KEY_F9: tilde(20); break;
    case DB_KEY_F10: tilde(21); break;
    case DB_KEY_F11: tilde(23); break;
    case DB_KEY_F12: tilde(24); break;
    case DB_KEY_ENTER:
      if (shift && !ctrl && !alt) o = "\x1b\r";              // the deck: a newline in the prompt
      else o = alt ? "\x1b\r" : "\r";
      break;
    case DB_KEY_BACKSPACE: o = ctrl ? "\x08" : "\x7f"; if (alt) o = esc + o; break;
    case DB_KEY_TAB: if (ctrl) return -1; o = shift ? "\x1b[Z" : "\t"; break;      // Ctrl+Tab is the window's
    case DB_KEY_ESCAPE: o = alt ? "\x1b\x1b" : "\x1b"; break;
    case DB_KEY_SPACE: if (!ctrl) return -1; o = std::string(1, '\0'); break;
    default: return -1;
  }
  if (!out || (int)o.size() > cap) return -1;
  std::memcpy(out, o.data(), o.size());
  return (int)o.size();
}
int db_meta_text(const char* utf8, size_t n, char* out, int cap) {
  if (!utf8 || !out || (int)n + 1 > cap) return -1;
  out[0] = 0x1b; std::memcpy(out + 1, utf8, n);
  return (int)n + 1;
}
const char* db_paste_bytes(const char* utf8, size_t n, int bracketed, size_t* out_n) {
  static thread_local std::string r;
  std::string t; t.reserve(n);
  for (size_t i = 0; utf8 && i < n; i++) {
    const char c = utf8[i];
    if (c == '\r') { t.push_back('\r'); if (i + 1 < n && utf8[i + 1] == '\n') i++; }
    else if (c == '\n') t.push_back('\r');
    else t.push_back(c);
  }
  if (!bracketed) r = t;
  else {
    std::string clean; size_t p = 0;                          // a paste must not be able to end its own bracket
    for (;;) { const size_t q = t.find("\x1b[201~", p); if (q == std::string::npos) { clean += t.substr(p); break; } clean += t.substr(p, q - p); p = q + 6; }
    r = "\x1b[200~" + clean + "\x1b[201~";
  }
  if (out_n) *out_n = r.size();
  return r.data();
}
const char* db_key_action(const char* os, const char* key, const char* code, int mods) {
  static thread_local std::string r;
  try {
    r = core::platform::keyAction(S(os), json{ { "key", S(key) }, { "code", S(code) }, { "control", (mods & DB_MOD_CTRL) != 0 },
      { "shift", (mods & DB_MOD_SHIFT) != 0 }, { "alt", (mods & DB_MOD_ALT) != 0 }, { "meta", (mods & DB_MOD_META) != 0 } });
  } catch (...) { r.clear(); }
  return r.c_str();
}
const char* db_key_help_json(const char* os) { static thread_local std::string r; try { r = dump(core::platform::keyHelp(S(os))); } catch (...) { r = "null"; } return r.c_str(); }
const char* db_default_font_stack(const char* os) { static thread_local std::string r; r = core::platform::defaultFontStack(S(os)); return r.c_str(); }

// ---------------------------------------------------------------------------------------------- mouse
struct DBMouse { deck::mouse::Encoder enc; deck::mouse::WheelSum wheel; };
DBMouse* db_mouse_create(void) { return new DBMouse; }
void db_mouse_destroy(DBMouse* m) { delete m; }
void db_mouse_reset(DBMouse* m) { if (m) { m->enc.reset(); m->wheel.partial = 0; } }
int db_mouse_report(DBMouse* m, const DBMouseEvent* e, int mode, int encoding, int cols, int rows, char* out, int cap) {
  if (!m || !e) return -1;
  deck::mouse::Event ev; ev.col = e->col; ev.row = e->row; ev.x = e->x; ev.y = e->y; ev.button = e->button; ev.action = e->action;
  ev.ctrl = e->ctrl != 0; ev.alt = e->alt != 0; ev.shift = e->shift != 0;
  std::string units;
  if (!m->enc.trigger(ev, mode, encoding, cols, rows, units)) return -1;
  const std::string w = deck::mouse::wire(units);
  if (!out || (int)w.size() > cap) return -1;
  std::memcpy(out, w.data(), w.size());
  return (int)w.size();
}
int db_mouse_wheel_lines(DBMouse* m, double dy, double cell_h, int shift) { return m ? m->wheel.consume(dy, (float)cell_h, shift != 0) : 0; }
void db_mouse_coords(double px, double py, double cw, double ch, int cols, int rows, DBMouseEvent* e) {
  if (!e) return;
  deck::mouse::Event ev; deck::mouse::reportCoords((float)px, (float)py, (float)cw, (float)ch, cols, rows, ev);
  e->col = ev.col; e->row = ev.row; e->x = ev.x; e->y = ev.y;
}

// ---------------------------------------------------------------------------------------------- layout
struct DBLayout { layout::Grid g; layout::LayoutMap m; std::vector<std::string> evicted; std::string scratch; };
namespace {
layout::Cells cellsOf(const int* cells, int count) { layout::Cells c; for (int i = 0; cells && i < count; i++) c.push_back(cells[i]); return c; }
int putCells(const layout::Cells& c, int* out, int cap) { const int n = std::min((int)c.size(), cap); for (int i = 0; out && i < n; i++) out[i] = c[(size_t)i]; return out ? n : 0; }
}  // namespace
DBLayout* db_layout_create(int rows, int cols) { DBLayout* l = new DBLayout; l->g.rows = rows; l->g.cols = cols; const layout::Dims d = layout::dims(l->g); l->g.rows = d.R; l->g.cols = d.C; return l; }
void db_layout_destroy(DBLayout* l) { delete l; }
void db_layout_grid(const DBLayout* l, int* rows, int* cols) { if (rows) *rows = l ? l->g.rows : 0; if (cols) *cols = l ? l->g.cols : 0; }
void db_layout_regrid(DBLayout* l, int rows, int cols) {
  if (!l) return;
  layout::Grid to; to.rows = rows; to.cols = cols; const layout::Dims d = layout::dims(to); to.rows = d.R; to.cols = d.C;
  l->m = layout::regrid(l->m, l->g, to); l->g = to; l->evicted.clear();
}
void db_layout_clear(DBLayout* l) { if (l) { l->m.clear(); l->evicted.clear(); } }
int db_layout_load_json(DBLayout* l, const char* text) {
  if (!l) return 0;
  const json j = parseOr(text, json());
  if (!j.is_object()) return 0;
  l->m.clear(); l->evicted.clear();
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it.value().is_array()) continue;
    layout::Cells c;
    for (const json& v : it.value()) if (v.is_number_integer()) c.push_back(v.get<int>());
    c = layout::norm(c, l->g);
    if (!c.empty() && layout::isLegal(c, l->g)) l->m.emplace_back(it.key(), c);
  }
  return 1;
}
const char* db_layout_json(DBLayout* l) {
  if (!l) return "{}";
  json o = json::object();
  for (const auto& kv : l->m) o[kv.first] = kv.second;
  l->scratch = dump(o);
  return l->scratch.c_str();
}
int db_layout_count(const DBLayout* l) { return l ? (int)l->m.size() : 0; }
const char* db_layout_id_at(DBLayout* l, int i) { return l && i >= 0 && i < (int)l->m.size() ? l->m[(size_t)i].first.c_str() : ""; }
int db_layout_cells(const DBLayout* l, const char* id, int* out, int cap) { return l ? putCells(layout::occupancy(l->m, S(id), l->g), out, cap) : 0; }
int db_layout_rect_of_cells(const DBLayout* l, const int* cells, int count, double* x, double* y, double* w, double* h) {
  if (!l) return 0;
  const layout::Cells c = layout::legalize(layout::norm(cellsOf(cells, count), l->g), l->g);
  if (c.empty()) return 0;
  const layout::Rect r = layout::rect(c, l->g);
  if (x) *x = r.x;
  if (y) *y = r.y;
  if (w) *w = r.w;
  if (h) *h = r.h;
  return 1;
}
int db_layout_rect(const DBLayout* l, const char* id, double* x, double* y, double* w, double* h) {
  if (!l) return 0;
  const layout::Cells c = layout::occupancy(l->m, S(id), l->g);
  return c.empty() ? 0 : db_layout_rect_of_cells(l, c.data(), (int)c.size(), x, y, w, h);
}
int db_layout_place(DBLayout* l, const char* id, const int* cells, int count) {
  if (!l) return 0;
  const layout::Cells c = layout::legalize(layout::norm(cellsOf(cells, count), l->g), l->g);
  if (c.empty() || S(id).empty()) return 0;
  layout::PlaceResult r = layout::place(l->m, S(id), c, l->g);
  l->m = std::move(r.layout); l->evicted = std::move(r.evicted);
  return 1;
}
int db_layout_evicted_count(const DBLayout* l) { return l ? (int)l->evicted.size() : 0; }
const char* db_layout_evicted_at(DBLayout* l, int i) { return l && i >= 0 && i < (int)l->evicted.size() ? l->evicted[(size_t)i].c_str() : ""; }
void db_layout_unplace(DBLayout* l, const char* id) { if (l) l->m = layout::unplace(l->m, S(id)); }
void db_layout_prune(DBLayout* l, const char* const* ids, int count) {
  if (!l) return;
  std::vector<std::string> live;
  for (int i = 0; ids && i < count; i++) live.push_back(S(ids[i]));
  l->m = layout::prune(l->m, live, l->g);
}
int db_layout_drop_target(const DBLayout* l, double px, double py, int* out, int cap) { return l ? putCells(layout::dropTarget(l->m, px, py, l->g), out, cap) : 0; }
int db_layout_resolve_drop(const DBLayout* l, const char* id, const int* cells, int count, int* out, int cap) {
  return l ? putCells(layout::resolveDrop(l->m, S(id), layout::norm(cellsOf(cells, count), l->g), l->g), out, cap) : 0;
}
void db_conn_layout(DBConn* c, const DBLayout* l) { if (c && l) sendFrame(c, proto::layout(l->m)); }
