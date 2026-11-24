// core/test/deck/hosting_test.cpp — how EVERY shell holds a deck, on the fakes, in either path mode:
//   options   core/deck/options.hpp: the shipping defaults (pure functions of { is it the app, its arguments, its
//             environment }) and the one options builder (buildDeckOptions), win32 or darwin;
//   hosting   core/deck/hosting.hpp: the Inbox other threads post to, the pump, the shell's windows as clients
//             with their own sinks, EXACT routing by client id (the race the Mac draft found), the saved places a
//             window takes (R7) before start(), after it, and after the last window closed (main.js `activate`).
// No thread, no socket, no process: the Inbox is pushed from this thread, the listener is two strings per socket.
#include "t.hpp"
#include "fakes.hpp"
#include "../../deck/hosting.hpp"
#include "../../deck/options.hpp"
#include "../../deck/platform.hpp"
#include "../../deck/version.hpp"
#include "../../deck/edition.hpp"
#include "../../deck/ws.hpp"
#include "../../session/jsutil.hpp"

#include <stdexcept>

using namespace deck::core;
using fake::P;

namespace {

// an Env whose os() and variables a test sets (the options builder's three platforms)
struct OsEnv : fake::Env {
  std::string osName;
  explicit OsEnv(const std::string& os) : osName(os) {}
  std::string os() override { return osName; }
  void set(const std::string& k, const std::string& v) { for (auto& kv : env) if (kv.first == k) { kv.second = v; return; } env.emplace_back(k, v); }
};

// ---------------------------------------------------------------- options
void testOptions() {
  const std::string mode = fake::posix() ? "options (posix): " : "options: ";
  auto C = [&](const std::string& name, bool ok, const std::string& got = "") { check(mode + name, ok, got); };
  // ---- which process is the app (Windows: its file name - the edition's, core/deck/edition.hpp: ClaudeDeckNative.exe here)
  const std::string X = deck::edition::kExeName, XL = lowerAscii(X.substr(0, X.size() - 4)) + ".EXE", SD = deck::edition::kStateDirName;
  C("shippingExeName: " + X + " in any folder, any case, either slash", shippingExeName("C:\\Program Files\\x\\" + X) && shippingExeName(XL)
    && shippingExeName("/opt/deck/" + X) && shippingExeName(X));
  C("shippingExeName: a test exe, a tool, nothing, a name that only contains it", !shippingExeName("deck-host-test.exe") && !shippingExeName("") && !shippingExeName("C:\\x\\" + X + ".bak")
    && !shippingExeName("C:\\x\\My" + X));
  // ---- Claude sessions
  using V = std::vector<std::string>;
  C("allowClaude: the app on its own folder starts Claude; on a scratch --state-dir it does not; another process never",
    shippingAllowClaude(true, {}) && !shippingAllowClaude(true, { "--state-dir", "x" }) && !shippingAllowClaude(false, {}) && !shippingAllowClaude(false, { "--local" }));
  C("allowClaude: --allow-claude on, --no-claude off, wherever; the last of the two wins",
    shippingAllowClaude(false, { "--allow-claude" }) && shippingAllowClaude(true, { "--state-dir", "x", "--allow-claude" }) && !shippingAllowClaude(true, { "--no-claude" })
    && shippingAllowClaude(true, { "--no-claude", "--allow-claude" }) && !shippingAllowClaude(false, { "--allow-claude", "--no-claude" }));
  // ---- the ask feed
  C("asks: the app on its own folder reads the feed; a scratch deck or a test exe only when ASK_PAGE_STATE_DIR is set",
    shippingAsks(true, {}, false) && !shippingAsks(true, { "--state-dir", "x" }, false) && shippingAsks(true, { "--state-dir", "x" }, true) && !shippingAsks(false, {}, false) && shippingAsks(false, {}, true));
  C("asks: --no-asks off, --asks on, wherever; the last wins", !shippingAsks(true, { "--no-asks" }, true) && shippingAsks(false, { "--asks" }, false) && shippingAsks(false, { "--no-asks", "--asks" }, false)
    && !shippingAsks(true, V{ "--asks", "--no-asks" }, false));
  C("exit watchdog: the app only", shippingExitWatchdog(true) && !shippingExitWatchdog(false));
  { OsEnv e("win32");
    const ShippingDefaults a = shippingDefaults(true, {}, e), b = shippingDefaults("C:\\t\\deck-host-test.exe", { "--state-dir", "x" }, e);
    e.set("ASK_PAGE_STATE_DIR", P("C:\\tmp\\asks"));
    const ShippingDefaults c = shippingDefaults("C:\\t\\deck-host-test.exe", { "--state-dir", "x" }, e), d = shippingDefaults("D:\\" + X, {}, e);
    C("shippingDefaults: all three at once, the ask rule's fact read from the environment", a.allowClaude && a.asks && a.exitWatchdog && !b.allowClaude && !b.asks && !b.exitWatchdog
      && !c.allowClaude && c.asks && !c.exitWatchdog && d.allowClaude && d.asks && d.exitWatchdog); }

  // ---- the native deck's own state folder, per platform
  if (!fake::posix()) {
    OsEnv e("win32"); e.set("APPDATA", "C:\\home\\AppData\\Roaming");
    C("defaultStateDir win32: %APPDATA%\\" + SD + "\\local (never the Electron deck's)", defaultStateDir(e) == "C:\\home\\AppData\\Roaming\\" + SD + "\\local", defaultStateDir(e));
  } else {
    OsEnv mac("darwin"), linux_("linux"), xdg("linux"); xdg.set("XDG_CONFIG_HOME", "/home/cfg");
    C("defaultStateDir darwin: ~/Library/Application Support/" + SD + "/local", defaultStateDir(mac) == "/home/Library/Application Support/" + SD + "/local", defaultStateDir(mac));
    C("defaultStateDir elsewhere: $XDG_CONFIG_HOME, else ~/.config", defaultStateDir(xdg) == "/home/cfg/" + SD + "/local" && defaultStateDir(linux_) == "/home/.config/" + SD + "/local", defaultStateDir(linux_));
  }

  // ---- the builder: what a throwaway deck gets from an empty HostOptions
  { fake::Env env; fake::MemFiles files;
    if (!fake::posix()) env.env.emplace_back("APPDATA", P("C:\\home\\AppData\\Roaming"));
    const DeckBuild b = buildDeckOptions(HostOptions(), env, files);
    const DeckOptions& o = b.options;
    C("builder: an empty HostOptions is the platform's state folder, the env's home, the alias file platform.js names",
      b.stateDir == defaultStateDir(env) && o.stateDir == b.stateDir && o.home == P("C:\\home") && o.aliasFile == P("C:\\home\\.claude-code-account-aliases") && o.aliasFile == platform::aliasFiles(env, files).primary, o.aliasFile);
    C("builder: nothing of the owner's is on - no Claude, no shells kept, no alias edits, no polling, no feed, no Http", !o.allowClaude && !o.persistShells && !o.allowAccountEdit && !o.usagePolling && !o.cloudPolling
      && !o.allowCloudCreate && o.askStateDir.empty() && o.askDownloads.empty() && !b.wantsHttp && o.deskOpenInbox);
    if (!fake::posix()) C("builder: win32's New terminal is PowerShell -NoLogo; claude.exe from Path; Git Bash's fallback", o.shellFile == "powershell.exe" && o.shellArgs == std::vector<std::string>{ "-NoLogo" } && o.shellName == "PowerShell"
      && o.claudeExe == "claude.exe" && o.bash == "bash.exe", o.shellFile + " " + o.claudeExe + " " + o.bash);
    else C("builder: darwin's New terminal is the login $SHELL -l; claude from Path; bash", o.shellFile == "/bin/zsh" && o.shellArgs == std::vector<std::string>{ "-l" } && o.shellName == "zsh"
      && o.claudeExe == "claude" && o.bash == "bash", o.shellFile + " " + o.claudeExe + " " + o.bash);
    C("builder: the version is the one place it is written; the key help and font stack are the platform's", o.version == kNativeDeckVersion && o.keyHelp == platform::keyHelp(env.os())
      && o.fontStack == platform::defaultFontStack(env.os()) && (fake::posix() ? o.keyHelp["sidebar"] == "Cmd+B" : o.keyHelp["sidebar"] == "F9 / Ctrl+`"), o.keyHelp.dump());
    C("builder: the rulings are the core's defaults (a remote desk is not a window; the claim is a proposal: off)", o.remoteDeskIsAWindow == DeckOptions().remoteDeskIsAWindow && o.driverClaim == DeckOptions().driverClaim && !o.remoteDeskIsAWindow);
    C("builder: no asset root and no --serve: the server finds no pages and binds what the settings say", b.server.assetRoot.empty() && b.server.forcePort == 0); }

  // ---- the builder: what a shell names
  { fake::Env env; fake::MemFiles files;
    env.env.emplace_back("CLAUDE_DECK_CLAUDE", P("C:\\tools\\claude-from-env.exe")); env.env.emplace_back("CLAUDE_DECK_BASH", P("C:\\Git\\bin\\bash.exe")); env.env.emplace_back("CLAUDE_DECK_CWD", P("C:\\work"));
    env.env.emplace_back("ASK_PAGE_STATE_DIR", P("C:\\tmp\\ask-state")); env.env.emplace_back("ASK_PAGE_DOWNLOADS", P("C:\\tmp\\dl"));
    // a checkout: the exe two folders below the repo root, where renderer/phone/index.html is
    files.put(P("C:\\repo\\renderer\\phone\\index.html"), "<html>");
    files.put(P("C:\\home\\.local\\bin\\userswitch"), "#!/bin/sh");
    HostOptions h; h.stateDir = P("C:\\state"); h.home = P("C:\\home"); h.aliasFile = P("C:\\cfg\\aliases"); h.allowClaude = true; h.persistShells = true; h.allowAccountEdit = true;
    h.shellFile = "cmd.exe"; h.shellArgs = { "/d" }; h.shellName = "cmd"; h.servePort = 8811; h.exeDir = P("C:\\repo\\windows\\build"); h.usagePolling = true; h.asks = true;
    h.cloudPolling = true; h.allowCloudCreate = true; h.deskOpenInbox = false; h.version = "9.9.9"; h.remoteDeskIsAWindow = true; h.driverClaim = true;
    const DeckBuild b = buildDeckOptions(h, env, files);
    const DeckOptions& o = b.options;
    C("builder: every switch the shell names reaches the core", o.stateDir == P("C:\\state") && b.stateDir == o.stateDir && o.home == P("C:\\home") && o.aliasFile == P("C:\\cfg\\aliases") && o.allowClaude && o.persistShells
      && o.allowAccountEdit && o.usagePolling && o.cloudPolling && o.allowCloudCreate && !o.deskOpenInbox && o.version == "9.9.9" && o.remoteDeskIsAWindow && o.driverClaim && b.wantsHttp);
    C("builder: the shell's own New terminal, the env's claude / bash / cwd override", o.shellFile == "cmd.exe" && o.shellArgs == std::vector<std::string>{ "/d" } && o.shellName == "cmd"
      && o.claudeExe == P("C:\\tools\\claude-from-env.exe") && o.bash == P("C:\\Git\\bin\\bash.exe") && o.cwdOverride == P("C:\\work"), o.claudeExe);
    C("builder: the feed on with no folder named = lib/asks.js locations() (here the environment's)", o.askStateDir == P("C:\\tmp\\ask-state") && o.askDownloads == std::vector<std::string>{ P("C:\\tmp\\dl") }, o.askStateDir);
    C("builder: the asset root is found upwards from the exe's folder; --serve's port is forced", b.server.assetRoot == P("C:\\repo") && b.server.forcePort == 8811, b.server.assetRoot);
    C("builder: userswitch: ~/.local/bin first", o.userswitch == P("C:\\home\\.local\\bin\\userswitch"), o.userswitch);
    h.claudeExe = P("C:\\named\\claude.exe"); h.askStateDir = P("C:\\t\\asks"); h.askDownloads = { P("C:\\t\\dl1"), P("C:\\t\\dl2") }; h.asks = false; h.usagePolling = false; h.cloudPolling = false;
    h.assetRoot = P("C:\\pages"); h.shellFile.clear();
    const DeckBuild c = buildDeckOptions(h, env, files);
    C("builder: a named claude beats the environment; named ask folders turn the feed on whatever `asks` says; no polling = no Http",
      c.options.claudeExe == P("C:\\named\\claude.exe") && c.options.askStateDir == P("C:\\t\\asks") && c.options.askDownloads.size() == 2 && !c.wantsHttp);
    C("builder: a named asset root is kept as it is (no search); an empty shell file = the platform's", c.server.assetRoot == P("C:\\pages") && c.options.shellFile == platform::shellCommand(env).file);
    files.files.clear(); files.dirs.clear();
    h.assetRoot.clear();
    C("builder: no pages anywhere above the exe: no asset root (the pages answer 404)", buildDeckOptions(h, env, files).server.assetRoot.empty()); }
}

// ---------------------------------------------------------------- hosting
// One deck as a shell holds it, on the fakes. Each window's sink records what it was given, under a label.
struct HostRig {
  fake::Clock clock; fake::MemFiles files; fake::Ptys ptys; fake::Procs procs; fake::Random random; fake::Env env; fake::Listener listener; fake::Net net; fake::Life life;
  std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
  int wakes = 0;
  std::unique_ptr<Host> host;
  std::map<std::string, std::vector<json>> got;
  std::map<std::string, int> inSink;          // a sink running now (re-entry checks)
  std::function<void(const std::string&, const json&)> onFrame;   // a test's hook, called from inside a sink
  int rid = 1000;
  json oneDeck;                              // HostOptions::oneDeck for make()
  fake::Log log;                             // the deck's log lines

  HostRig() { files.clock = &clock; ptys.alivePids = &procs.alivePids; inbox->setWake([this] { wakes++; }); }
  void make(bool inlineSinks = false, bool withServer = true, bool persist = true) {
    HostOptions o; o.stateDir = P("C:\\state"); o.home = P("C:\\home"); o.aliasFile = P("C:\\home\\aliases"); o.shellFile = "cmd.exe"; o.shellArgs = { "/d" }; o.shellName = "cmd";
    o.persistShells = persist; o.servePort = withServer ? 8811 : 0;
    o.oneDeck = oneDeck;                      // null = off (a test sets it for the shared layout)
    const DeckBuild b = buildDeckOptions(o, env, files);
    HostSeams s; s.clock = &clock; s.files = &files; s.ptys = &ptys; s.procs = &procs; s.random = &random; s.env = &env; s.lifecycle = &life; s.log = &log;
    if (withServer) { s.listener = &listener; s.net = &net; }
    host.reset(new Host(b, s, inbox, inlineSinks));
  }
  Host::WindowSink sink(const std::string& label) {
    return [this, label](const json& f) {
      inSink[label]++;
      got[label].push_back(f);
      if (onFrame) onFrame(label, f);
      inSink[label]--;
    };
  }
  // the pump as a shell runs it: pty exits come in through the inbox, then pump(); the clock moves to the next timer
  void advance(double ms) {
    const double end = clock.t + ms;
    for (;;) {
      while (!ptys.exits.empty()) { const auto e = ptys.exits.front(); ptys.exits.pop_front(); if (fake::Pty* p = ptys.byToken(e.first)) { p->gone = true; procs.alivePids.erase(p->pid_); } inbox->ptyExit(e.first, e.second); }
      host->pump();
      if (clock.t >= end) break;
      clock.t += std::min(end - clock.t, std::max(1.0, (double)host->nextDueMs()));
    }
  }
  json call(ClientId w, const std::string& label, const std::string& fn, json args = json::array()) {
    const int id = ++rid; const size_t from = got[label].size();
    host->handle(w, { { "t", "call" }, { "rid", id }, { "fn", fn }, { "args", args } });
    for (size_t i = from; i < got[label].size(); i++) { const json& f = got[label][i]; if ((f["t"] == "ok" || f["t"] == "err") && f.value("rid", json()) == id) return f; }
    return json();
  }
  json res(ClientId w, const std::string& label, const std::string& fn, json args = json::array()) { return call(w, label, fn, args).value("result", json()); }
  std::string shell(ClientId w, const std::string& label) { return str(res(w, label, "newTile", json::array({ { { "kind", "shell" } } }))); }
  size_t count(const std::string& label, const char* t) { size_t n = 0; for (const json& f : got[label]) if (f["t"] == t) n++; return n; }
  json tilesFile() { return files.getJson(P("C:\\state\\tiles.json")); }

  // a phone page on the server's socket, through the inbox as a socket thread would hand it over
  uint64_t nextConn = 1;
  std::map<uint64_t, std::unique_ptr<WsDecoder>> dec;
  uint64_t phone() {
    const uint64_t c = nextConn++;
    const std::string token = str(host->deck().settings()["phone"]["token"]);
    const std::string up = "GET /?t=" + token + " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    inbox->netAccept(c); inbox->netBytes(c, up.data(), up.size());
    host->pump();
    std::string& sent = listener.sent[c];
    const size_t end = sent.find("\r\n\r\n");
    if (end != std::string::npos) sent.erase(0, end + 4);
    dec[c].reset(new WsDecoder(false));
    return c;
  }
  std::vector<json> heard(uint64_t c) {
    std::vector<WsEvent> ev; const std::string s = listener.take(c); dec[c]->feed(s.data(), s.size(), ev);
    std::vector<json> v; for (const WsEvent& e : ev) if (e.kind == WsEvent::Message) v.push_back(json::parse(e.data, nullptr, false));
    return v;
  }
};

json bounds(int x, int y, int w, int h) { return { { "x", x }, { "y", y }, { "width", w }, { "height", h } }; }

void testHosting() {
  const std::string mode = fake::posix() ? "hosting (posix): " : "hosting: ";
  auto C = [&](const std::string& name, bool ok, const std::string& gotText = "") { check(mode + name, ok, gotText); };

  // ---- the inbox (the one thread-safe thing)
  { Inbox in; int wakes = 0; in.setWake([&] { wakes++; });
    in.ptyData(7, "ab", 2); in.ptyData(7, "cd", 2); in.netBytes(7, "x", 1); in.ptyData(7, "e", 1); in.ptyExit(7, 3);
    C("inbox: the wake is called once until the owner takes", wakes == 1);
    std::deque<Inbox::Event> q; in.take(q);
    C("inbox: a burst from one pty is one event; another kind or source in between starts a new one", q.size() == 4 && q[0].kind == Inbox::PtyData && q[0].bytes == "abcd" && q[1].kind == Inbox::NetBytes
      && q[2].bytes == "e" && q[3].kind == Inbox::PtyExit && q[3].code == 3 && in.empty());
    in.ptyData(1, "z", 1);
    C("inbox: after a take, the next push wakes again", wakes == 2);
    in.take(q);
    const std::string big(200 * 1024, 'x');
    in.ptyData(1, big.data(), big.size()); in.ptyData(1, big.data(), big.size()); in.ptyData(1, big.data(), big.size());
    in.take(q);
    C("inbox: a burst is merged while it is under 256 KB", q.size() == 2 && q[0].bytes.size() == 400 * 1024 && q[1].bytes.size() == 200 * 1024);
    bool ran = false; in.task([&] { ran = true; }); in.take(q);
    C("inbox: a task is carried, not run, until the owner runs it", q.size() == 1 && q[0].kind == Inbox::Task && !ran && (q[0].task(), ran)); }

  // ---- seams
  { HostRig r; HostSeams s; s.clock = &r.clock; s.files = &r.files;
    bool threw = false; try { Host h(buildDeckOptions(HostOptions(), r.env, r.files), s, r.inbox); } catch (const std::invalid_argument&) { threw = true; }
    C("host: a missing required seam is refused (std::invalid_argument)", threw);
    r.make(false, false);
    C("host: without a listener and a network there is no server and no phone status", !r.host->server() && r.host->phoneStatus().is_null() && r.host->running() && r.host->stateDir() == P("C:\\state")); }
  { fake::Listener inner; TestListener tl(inner); std::string err;
    C("TestListener: 127.0.0.1:8811-8819 only; the tailnet, the LAN and the live deck's port are refused", !tl.listen("0.0.0.0", 8811, &err) && tl.refused == "0.0.0.0:8811" && !tl.listen("127.0.0.1", 8788, &err)
      && !inner.listening && tl.listen("127.0.0.1", 8811, &err) && inner.listening && inner.port == 8811, err);
    TestNet tn; C("TestNet: no address, Tailscale never asked", tn.ipv4Addresses().empty() && tn.tailscaleServeStatus().empty()); }

  // ---- R7: which saved entry a window takes. Deck one saves three windows; deck two reopens them.
  std::map<std::string, fake::MemFiles::Entry> saved;
  std::string s1, s2, s3;
  { HostRig a; a.make();
    const ClientId w0 = a.host->addWindow(bounds(10, 10, 900, 600), a.sink("w0"));
    a.host->start();
    a.call(w0, "w0", "deskInit"); a.advance(2100);
    s1 = a.shell(w0, "w0"); s2 = a.shell(w0, "w0"); s3 = a.shell(w0, "w0");
    a.host->handle(w0, { { "t", "layout" }, { "layout", { { s1, { 1, 2 } } } } });
    const ClientId w1 = a.host->openWindow(s2, bounds(20, 20, 800, 500), a.sink("w1"));
    const ClientId w2 = a.host->openWindow(s3, bounds(30, 30, 700, 400), a.sink("w2"));
    a.advance(3000);
    C("R7 (deck one): three windows, each holding one tile, saved in order", w1 && w2 && a.tilesFile()["windows"].size() == 3 && a.tilesFile()["windows"][2]["bounds"]["width"] == 700, a.tilesFile()["windows"].dump().substr(0, 300));
    a.host->shutdown();
    saved = a.files.files; }

  { HostRig b; b.clock.t = 1e6; b.files.files = saved;
    b.make();
    const ClientId v0 = b.host->addWindow(bounds(1, 1, 100, 100), b.sink("v0"));
    b.host->start();
    const ClientId v1 = b.host->addWindow(bounds(2, 2, 100, 100), b.sink("v1"));
    const ClientId v2 = b.host->addWindow(bounds(3, 3, 100, 100), b.sink("v2"));
    const ClientId v3 = b.host->addWindow(bounds(4, 4, 333, 222), b.sink("v3"));
    const json l0 = b.res(v0, "v0", "deskInit")["layout"], l1 = b.res(v1, "v1", "deskInit")["layout"], l2 = b.res(v2, "v2", "deskInit")["layout"], l3 = b.res(v3, "v3", "deskInit")["layout"];
    C("Q1: the window made before start() takes the first saved entry", l0 == json{ { s1, { 1, 2 } } } && b.host->deck().windowIndex(v0) == 0, l0.dump());
    C("Q1: windows made AFTER start() take the next saved entries in order (the second its second, the third its third)",
      l1.contains(s2) && l1.size() == 1 && l2.contains(s3) && l2.size() == 1 && b.host->deck().windowIndex(v1) == 1 && b.host->deck().windowIndex(v2) == 2, l1.dump() + " " + l2.dump());
    C("Q1: one more than was saved takes nothing: the bounds the shell gave, no layout, the next index", l3.empty() && b.host->deck().windowIndex(v3) == 3, l3.dump());
    b.advance(4000);
    b.host->handle(v0, { { "t", "layout" }, { "layout", l0 } });        // (a save, so the file says each window's bounds)
    const json ws = b.tilesFile()["windows"];
    C("Q1: …with the saved bounds, the shell's for the extra one", ws.size() == 4 && ws[0]["bounds"]["width"] == 900 && ws[1]["bounds"]["width"] == 800 && ws[2]["bounds"]["width"] == 700 && ws[3]["bounds"]["width"] == 333, ws.dump().substr(0, 400));

    // ---- Q2: every window closes (the Mac: Cmd+W until none is left; the deck lives on), then one opens (the Dock icon)
    b.host->removeWindow(v1);
    b.host->removeWindow(v0);
    b.host->removeWindow(v3);
    const json before = b.tilesFile()["windows"];
    const size_t v2had = b.got["v2"].size();
    C("Q2: each window closing while others stay open drops out of the file", before.size() == 1 && before[0]["bounds"]["width"] == 700, before.dump());
    b.host->removeWindow(v2);
    b.host->handle(v2, { { "t", "call" }, { "rid", 1 }, { "fn", "deskInit" }, { "args", json::array() } });
    const json after = b.tilesFile()["windows"];
    C("Q2: the last window closing writes back every window closed in the 2 minutes before it, in window order, layouts and bounds (main.js `closed`); its sink gets nothing more",
      after.size() == 4 && after[0]["layout"] == json{ { s1, { 1, 2 } } } && after[0]["bounds"]["width"] == 900 && after[1]["layout"].contains(s2) && after[1]["bounds"]["width"] == 800
      && after[2]["layout"].contains(s3) && after[2]["bounds"]["width"] == 700 && after[3]["layout"].empty() && after[3]["bounds"]["width"] == 333 && b.got["v2"].size() == v2had && b.host->windowCount() == 0, after.dump());
    C("Q2: the tiles keep running with no window", b.ptys.live.size() == 3 && b.host->deck().livePids().size() == 3);
    const ClientId n = b.host->addWindow(bounds(5, 5, 555, 444), b.sink("n"));
    const json ln = b.res(n, "n", "deskInit")["layout"];
    C("Q2: the new window is the state file's FIRST entry as it is now (main.js `activate`: window 0's layout) with a NEW index",
      ln.contains(s1) && ln.size() == 1 && b.host->deck().windowIndex(n) == 4, ln.dump() + " index " + std::to_string(b.host->deck().windowIndex(n)));
    b.host->handle(n, { { "t", "layout" }, { "layout", ln } });
    C("Q2: …and its bounds (not the ones the shell asked for)", b.tilesFile()["windows"].size() == 1 && b.tilesFile()["windows"][0]["bounds"]["width"] == 900, b.tilesFile()["windows"].dump());
    const ClientId n2 = b.host->addWindow(bounds(6, 6, 666, 444), b.sink("n2"));
    C("Q2: a second window then is a plain new one (the saved entries were all taken at boot): nothing saved, the next index",
      b.res(n2, "n2", "deskInit")["layout"].empty() && b.host->deck().windowIndex(n2) == 5);
    b.host->shutdown(); }

  // ---- QUIT KEEPS EVERY WINDOW (main.js `closed`, 11bf1ab): two windows, the second closed, then the main one (the
  // Windows shell's last window ends the deck: shutdown with that window still open); a restart brings both back, and
  // the shared layout file has both windows' cells too. A window closed more than 2 minutes before the last is dropped.
  { const std::string sharedDir = P("C:\\shared");
    const json od = { { "deck", "native" }, { "sharedDir", sharedDir }, { "exe", P("C:\\deck\\ClaudeDeckNative.exe") }, { "args", json::array({ "--local" }) }, { "defaultDeck", true }, { "publishAtStart", "write" } };
    std::map<std::string, fake::MemFiles::Entry> quitSaved;
    std::string q1, q2;
    { HostRig a; a.env.pidV = 4242; a.oneDeck = od; a.make();
      const ClientId w0 = a.host->addWindow(bounds(10, 10, 900, 600), a.sink("w0"));
      a.host->start();
      a.call(w0, "w0", "deskInit"); a.advance(2100);
      q1 = a.shell(w0, "w0"); q2 = a.shell(w0, "w0");
      a.host->handle(w0, { { "t", "layout" }, { "layout", { { q1, { 1, 2 } } } } });
      const ClientId w1 = a.host->openWindow(q2, bounds(50, 50, 800, 500), a.sink("w1"));
      a.advance(3000);
      a.host->removeWindow(w1);
      const json one = a.tilesFile()["windows"];
      C("quit: the second window closing drops it from the file (as before)", w1 && one.size() == 1 && one[0]["layout"].contains(q1), one.dump());
      a.advance(30000);
      a.host->shutdown();                          // the main window closing ends the deck
      const json ws = a.tilesFile()["windows"];
      C("quit: the main window closing last writes BOTH windows back, each with its layout and bounds", ws.size() == 2 && ws[0]["layout"] == json{ { q1, { 1, 2 } } } && ws[0]["bounds"]["width"] == 900
        && ws[1]["layout"].size() == 1 && ws[1]["layout"].contains(q2) && ws[1]["bounds"]["width"] == 800, ws.dump());
      const json sh = a.files.getJson(sharedDir + P("\\shared-layout.json"));
      const json sw = sh.is_object() ? sh["windows"] : json();
      C("quit: the shared layout file keeps both windows' cells too", sw.is_array() && sw.size() == 2 && sw[0]["layout"].contains(q1) && sw[1]["layout"].contains(q2), sh.dump().substr(0, 400));
      C("quit: the log says so", a.log.count("the quit keeps 2 windows' layouts") == 1, a.log.all().substr(0, 400));
      quitSaved = a.files.files; }
    { HostRig b; b.clock.t = 1e6; b.env.pidV = 4242; b.oneDeck = od; b.files.files = quitSaved; b.make();
      const ClientId v0 = b.host->addWindow(bounds(1, 1, 100, 100), b.sink("v0"));
      b.host->start();
      const ClientId v1 = b.host->addWindow(bounds(2, 2, 100, 100), b.sink("v1"));
      const json l0 = b.res(v0, "v0", "deskInit")["layout"], l1 = b.res(v1, "v1", "deskInit")["layout"];
      C("quit: after a restart both layouts are back (window 0 its cells, window 1 its tile)", l0 == json{ { q1, { 1, 2 } } } && l1.size() == 1 && l1.contains(q2), l0.dump() + " " + l1.dump());
      b.host->shutdown(); }
    { HostRig c; c.env.pidV = 4242; c.make();
      const ClientId w0 = c.host->addWindow(bounds(10, 10, 900, 600), c.sink("w0"));
      c.host->start(); c.call(w0, "w0", "deskInit"); c.advance(2100);
      const std::string x1 = c.shell(w0, "w0"), x2 = c.shell(w0, "w0");
      c.host->handle(w0, { { "t", "layout" }, { "layout", { { x1, { 1 } } } } });
      const ClientId w1 = c.host->openWindow(x2, bounds(50, 50, 800, 500), c.sink("w1"));
      c.advance(3000); c.host->removeWindow(w1);
      c.advance(121000);
      c.host->shutdown();
      const json ws = c.tilesFile()["windows"];
      C("quit: a window closed more than 2 minutes before the last one is not part of the quit (dropped, as before)", ws.size() == 1 && ws[0]["layout"].contains(x1) && !ws[0]["layout"].contains(x2), ws.dump()); }
    { HostRig d; d.env.pidV = 4242; d.make();
      const ClientId w0 = d.host->addWindow(bounds(10, 10, 900, 600), d.sink("w0"));
      d.host->start(); d.call(w0, "w0", "deskInit"); d.advance(2100);
      const std::string y1 = d.shell(w0, "w0"), y2 = d.shell(w0, "w0");
      d.host->handle(w0, { { "t", "layout" }, { "layout", { { y1, { 1 } } } } });
      const ClientId w1 = d.host->openWindow(y2, bounds(50, 50, 800, 500), d.sink("w1"));
      d.advance(3000);
      d.host->removeWindow(w1); d.advance(1000);
      d.host->handle(w0, { { "t", "layout" }, { "layout", { { y1, { 1 } }, { y2, { 2 } } } } });   // its tile taken into the main window
      d.advance(1000);
      d.host->shutdown();
      const json ws = d.tilesFile()["windows"];
      C("quit: a tile placed in a closed window AND the last one is kept in the window that closed last", ws.size() == 2 && ws[0]["layout"].contains(y2) && ws[1]["layout"].empty(), ws.dump()); }
  }

  // ---- THE RACE (the Mac draft's question): a frame raised for ANOTHER client while a window is being added
  { HostRig r; r.make(false, true, false);
    const ClientId a = r.host->addWindow(json(), r.sink("a"));
    r.host->start(); r.call(a, "a", "deskInit"); r.advance(2100);
    const uint64_t ph = r.phone();
    const std::vector<json> hello = r.heard(ph);
    C("race: a phone came in through the inbox (accept + bytes) and got its hello", r.host->deck().remoteCount() == 1 && !hello.empty() && hello[0]["t"] == "init", hello.empty() ? "" : hello[0].dump().substr(0, 200));
    const std::string t = r.shell(a, "a");
    r.host->handle(a, { { "t", "layout" }, { "layout", { { t, { 1 } } } } });
    r.advance(2000); r.heard(ph);
    // openWindow moves the tile into the new window and publishes - a meta for the PHONE is raised inside the call,
    // while the new window's id is not yet the shell's
    const ClientId w = r.host->openWindow(t, bounds(40, 40, 640, 480), r.sink("new"));
    const std::vector<json> during = r.heard(ph);
    size_t phoneMetas = 0; for (const json& f : during) if (f["t"] == "meta") phoneMetas++;
    size_t newMetas = 0, foreign = 0; for (const json& f : r.got["new"]) { if (f["t"] == "meta") newMetas++; if (f.contains("pageKey")) foreign++; }
    C("race: the phone's meta raised inside openWindow reached the PHONE (it went to the new window before the fix)", w != 0 && phoneMetas == 1 && during.back()["meta"]["tiles"][0]["id"] == t, std::to_string(phoneMetas) + " " + (during.empty() ? "" : during.back().dump().substr(0, 200)));
    C("race: the new window got its own frames only - its hello and ONE meta, nothing meant for the phone", r.got["new"].size() == 2 && r.count("new", "init") == 1 && newMetas == 1 && foreign == 0,
      std::to_string(r.got["new"].size()) + " frames, " + std::to_string(newMetas) + " metas");
    r.host->shutdown();
    C("shutdown: the server stopped listening, every pty killed, nothing runs after", !r.listener.listening && r.listener.stops >= 1 && !r.host->running() && r.host->nextDueMs() == 2000); }

  // ---- delivery: deferred (the default: a sink may call straight back into the host) and inline
  { HostRig r; r.make(false, false, false);
    const ClientId a = r.host->addWindow(json(), r.sink("a"));
    const size_t beforeStart = r.got["a"].size();
    r.host->start();
    C("deferred: a window made before start() gets its hello from start()", beforeStart == 0 && r.count("a", "init") == 1);
    // a sink that answers a frame by calling the host again: the nested reply waits for the sink to return
    bool nestedInsideSink = true, sawNested = false;
    r.onFrame = [&](const std::string& label, const json& f) {
      if (label != "a" || f["t"] != "ok") return;
      if (f["rid"] == 50) r.host->handle(a, { { "t", "call" }, { "rid", 51 }, { "fn", "getSettings" }, { "args", json::array() } });
      if (f["rid"] == 51) { sawNested = true; nestedInsideSink = r.inSink["a"] > 1; }
    };
    r.host->handle(a, { { "t", "call" }, { "rid", 50 }, { "fn", "getSettings" }, { "args", json::array() } });
    C("deferred: a sink that calls back into the host is not re-entered - the nested reply comes after it returned", sawNested && !nestedInsideSink);
    r.onFrame = nullptr;
    // frames the shell causes by calling deck() directly arrive at once, or - inside enter() - when it returns
    const size_t at = r.got["a"].size();
    size_t seenInside = 0;
    r.host->enter([&] { r.host->deck().handle(a, { { "t", "call" }, { "rid", 60 }, { "fn", "getSettings" }, { "args", json::array() } }); seenInside = r.got["a"].size() - at; });
    C("deferred: enter() holds what a direct deck() call raises until it returns", seenInside == 0 && r.got["a"].size() == at + 1 && r.got["a"].back()["rid"] == 60);
    C("adoptWindow: refuses 0 and a window it already has", !r.host->adoptWindow(0, r.sink("x")) && !r.host->adoptWindow(a, r.sink("x")) && r.host->hasWindow(a) && !r.host->hasWindow(a + 100));
    // a pty's bytes through the inbox reach the window that attached the tile
    const std::string t = r.shell(a, "a");
    r.host->handle(a, { { "t", "attach" }, { "id", t } });
    fake::Pty* p = r.ptys.last(); const uint64_t ptyToken = p->token;   // (shutdown deletes the Pty: ask by token then)
    const size_t before = r.got["a"].size();
    r.inbox->ptyData(p->token, "hello from the pty", 18);
    r.host->pump();
    std::string data; for (size_t i = before; i < r.got["a"].size(); i++) if (r.got["a"][i]["t"] == "data" && r.got["a"][i]["id"] == t) data += r.got["a"][i]["d"].get<std::string>();
    C("pump: a pty's bytes posted to the inbox reach the attached window", data == "hello from the pty", data);
    r.host->removeWindow(a);
    const size_t afterRemove = r.got["a"].size();
    r.inbox->ptyData(p->token, "more", 4); r.host->pump();
    C("removeWindow: nothing more is delivered to its sink; the tile keeps running", r.got["a"].size() == afterRemove && r.host->windowCount() == 0 && !p->killed);
    r.host->shutdown();
    C("shutdown: the pty is killed; handle and pump do nothing after", r.ptys.wasKilled(ptyToken) &&(r.host->pump(), r.host->handle(a, json::object()), true) && !r.host->running()); }
  { HostRig r; r.make(true, false, false);
    bool helloInside = false, returned = false;
    r.onFrame = [&](const std::string& label, const json& f) { if (label == "a" && f["t"] == "init" && !returned) helloInside = true; };
    r.host->start();
    const ClientId a = r.host->addWindow(json(), r.sink("a")); returned = true;
    C("inline (a shell whose sinks only post, Windows): the hello reaches the sink inside addWindow, by the exact id", a != 0 && helloInside && r.count("a", "init") == 1);
    r.host->shutdown(); }
}

}  // namespace

void testHostingAndOptions(const std::string&) {
  if (want("options")) testOptions();
  if (want("hosting")) testHosting();
}
