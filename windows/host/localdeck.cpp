// windows/host/localdeck.cpp — see localdeck.hpp. The deck itself is core/deck (pure), and so is how a shell
// holds one (core/deck/hosting.hpp: the inbox, the pump, the routing) and how its options are built
// (core/deck/options.hpp). This file is the Windows half: the seams (winseams.hpp, statefile.hpp, listener.hpp),
// the wake (PostMessage + a condition variable for headless callers), the process's own facts (its file name and
// command line) and the exit watchdog.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "localdeck.hpp"
#include "listener.hpp"
#include "statefile.hpp"
#include "winhttp.hpp"
#include "winseams.hpp"
#include "../../core/deck/deck.hpp"
#include "../../core/deck/hosting.hpp"
#include "../../core/deck/options.hpp"
#include "../../core/deck/server.hpp"

#include <windows.h>
#include <shellapi.h>                          // CommandLineToArgvW

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace deck { namespace host {

namespace {
// How the owner is woken: a message to its window when it gave one (at most one outstanding: the Inbox calls the
// wake once per drain), and a condition variable waitForWork() sleeps on (headless callers).
struct Waker {
  std::mutex m;
  std::condition_variable cv;
  HWND hwnd = nullptr; UINT msg = 0;
  void wake() {
    { std::lock_guard<std::mutex> g(m); }      // (a waiter is either before its check or already waiting)
    cv.notify_all();
    if (hwnd) PostMessageW(hwnd, msg, 0, 0);
  }
};

// What the host itself answers of core::Lifecycle (the exit watchdog); the rest is the shell's, when it gave one.
struct HostLifecycle : core::Lifecycle {
  core::Lifecycle* shell = nullptr; bool watchdog = false;
  bool restart(const std::string& why) override { return shell ? shell->restart(why) : false; }
  json fingerprint() override { return shell ? shell->fingerprint() : json(); }
  void armExitWatchdog(double ms) override { if (shell) shell->armExitWatchdog(ms); if (watchdog) armProcessExitWatchdog(ms); }
  bool openExternal(const std::string& url) override { return shell ? shell->openExternal(url) : false; }
  bool openPath(const std::string& path) override { return shell ? shell->openPath(path) : false; }
  bool showInFolder(const std::string& path) override { return shell ? shell->showInFolder(path) : false; }
  bool openWindow(int client, const json& bounds, bool inactive) override { return shell ? shell->openWindow(client, bounds, inactive) : false; }
};

std::vector<std::string> narrowAll(const std::vector<std::wstring>& v) { std::vector<std::string> o; for (const std::wstring& s : v) o.push_back(narrow(s)); return o; }
// this process's own file and arguments (without the program)
void processFacts(std::wstring& exe, std::vector<std::wstring>& args) {
  wchar_t buf[32768]; const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
  exe.assign(buf, n);
  int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; i++) args.push_back(argv[i]);
  if (argv) LocalFree(argv);
}
}  // namespace

struct LocalDeck::Impl {
  HostLifecycle life;
  LocalDeckOptions opt;
  Emit emit;
  std::shared_ptr<core::Inbox> inbox = std::make_shared<core::Inbox>();
  std::shared_ptr<Waker> waker = std::make_shared<Waker>();
  std::unique_ptr<WinEnv> env;
  WinRandom random;
  WinProcessList procs;
  std::unique_ptr<WinPtyHost> ptys;
  std::unique_ptr<WinListener> listener;
  std::unique_ptr<WinNet> net;
  std::unique_ptr<WinHttpGet> http;
  WinCredentials credentials;
  std::unique_ptr<core::Host> host;            // last: destroyed first (the deck, then its server)
  core::ClientId window = 0;
  bool down = false;
};

// ---- the shipping defaults: core/deck/options.hpp's rules, asked with this platform's "is it the app" (the exe's name)
bool shippingAllowClaude(const std::wstring& exe, const std::vector<std::wstring>& args) { return core::shippingAllowClaude(core::shippingExeName(narrow(exe)), narrowAll(args)); }
bool shippingAllowClaudeHere() {
  static const bool on = [] { std::wstring exe; std::vector<std::wstring> args; processFacts(exe, args); return shippingAllowClaude(exe, args); }();
  return on;
}
bool shippingAsks(const std::wstring& exe, const std::vector<std::wstring>& args, bool envNamesAskDir) { return core::shippingAsks(core::shippingExeName(narrow(exe)), narrowAll(args), envNamesAskDir); }
bool shippingAsksHere() {
  static const bool on = [] { std::wstring exe; std::vector<std::wstring> args; processFacts(exe, args); return shippingAsks(exe, args, !envVar(L"ASK_PAGE_STATE_DIR").empty()); }();
  return on;
}
bool shippingOwnerSwitchesHere() {
  static const bool on = [] {
    std::wstring exe; std::vector<std::wstring> args; processFacts(exe, args);
    for (const std::wstring& a : args) if (a == L"--state-dir") return false;
    return core::shippingExeName(narrow(exe));
  }();
  return on;
}
bool shippingExitWatchdog(const std::wstring& exe) { return core::shippingExitWatchdog(core::shippingExeName(narrow(exe))); }
bool shippingExitWatchdogHere() {
  static const bool on = [] { std::wstring exe; std::vector<std::wstring> args; processFacts(exe, args); return shippingExitWatchdog(exe); }();
  return on;
}
// main.js armExitWatchdog, both of its layers in one: a thread of this process (nothing the main thread is stuck
// in can stop it) that ends what this process started and then the process. No PowerShell, no window.
void armProcessExitWatchdog(double ms) {
  static std::atomic<bool> armed{ false };
  if (armed.exchange(true)) return;
  const DWORD wait = ms > 0 ? (DWORD)ms : 0;
  std::thread([wait] {
    Sleep(wait);
    const DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
      if (Process32FirstW(snap, &pe)) do {
        if (pe.th32ParentProcessID != me || pe.th32ProcessID == me) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (h) { TerminateProcess(h, 1); CloseHandle(h); }
      } while (Process32NextW(snap, &pe));
      CloseHandle(snap);
    }
    TerminateProcess(GetCurrentProcess(), 0);
  }).detach();
}

// A LocalDeckOptions as the core's builder reads it: the same wishes, UTF-8, plus where this exe is (the asset
// root is looked for upwards from there when none was named).
core::HostOptions toHostOptions(const LocalDeckOptions& opt) {
  core::HostOptions h;
  h.stateDir = opt.stateDir; h.home = opt.home; h.aliasFile = opt.aliasFile;
  h.allowClaude = opt.allowClaude; h.persistShells = opt.persistShells; h.allowAccountEdit = opt.allowAccountEdit;
  h.shellFile = narrow(opt.shellFile); h.shellArgs = narrowAll(opt.shellArgs); h.shellName = opt.shellName;
  h.claudeExe = opt.claudeExe;
  h.servePort = opt.servePort; h.assetRoot = opt.assetRoot;
  wchar_t exe[32768]; const DWORD n = GetModuleFileNameW(nullptr, exe, 32768);
  h.exeDir = dirName(narrow(std::wstring(exe, n)));
  h.usagePolling = opt.usagePolling;
  h.asks = opt.asks; h.askStateDir = opt.askStateDir; h.askDownloads = opt.askDownloads;
  h.deskOpenInbox = opt.deskOpenInbox;
  h.cloudPolling = opt.cloudPolling; h.allowCloudCreate = opt.allowCloudCreate;
  h.version = opt.version; h.labelFallback = opt.labelFallback;
  h.remoteDeskIsAWindow = opt.remoteDeskIsAWindow; h.driverClaim = opt.driverClaim;
  h.oneDeck = opt.oneDeck;
  return h;
}
DeckBuild buildDeckOptions(const LocalDeckOptions& opt, core::Env& env) { return core::buildDeckOptions(toHostOptions(opt), env, winFiles()); }

core::onedeck::Busy oneDeckGuard(const LocalDeckOptions& o) {
  if (!o.oneDeck.is_object()) return core::onedeck::Busy();
  WinEnv env(o.home);
  const DeckBuild b = buildDeckOptions(o, env);
  WinProcessList procs;
  return core::onedeck::guardAtStart(winFiles(), procs, env, winClock().nowMs(), o.oneDeck, b.options.aliasFile, b.options.home);
}

static std::string oneDeckState(const LocalDeckOptions& o) { WinEnv env(o.home); return buildDeckOptions(o, env).stateDir; }
static std::string oneDeckShared(const LocalDeckOptions& o) { return o.oneDeck.is_object() && o.oneDeck.contains("sharedDir") && o.oneDeck["sharedDir"].is_string() ? o.oneDeck["sharedDir"].get<std::string>() : std::string(); }
nlohmann::json oneDeckPreflight(const LocalDeckOptions& o) {
  if (!o.oneDeck.is_object() || oneDeckShared(o).empty()) return { { "decision", { { "action", "none" }, { "why", "off" } } } };
  return core::onedeck::preflight(winFiles(), oneDeckState(o), oneDeckShared(o), winClock().nowMs());
}
std::string oneDeckImport(const LocalDeckOptions& o, const nlohmann::json& shared, const std::string& why) {
  if (!o.oneDeck.is_object() || !shared.is_object()) return std::string();
  const std::string st = oneDeckState(o);
  winFiles().makeDirs(st);
  return core::onedeck::importShared(winFiles(), st, shared, winClock().nowMs(), why);
}
std::string oneDeckKeepOwn(const LocalDeckOptions& o, const nlohmann::json& shared) {
  if (!o.oneDeck.is_object()) return std::string();
  const std::string st = oneDeckState(o);
  winFiles().makeDirs(st);
  return core::onedeck::keepOwn(winFiles(), st, oneDeckShared(o), shared, winClock().nowMs());
}

LocalDeck::LocalDeck(LocalDeckOptions options, Emit emit) : d(new Impl) {
  d->opt = std::move(options);
  d->emit = std::move(emit);
  d->waker->hwnd = (HWND)d->opt.notifyHwnd; d->waker->msg = d->opt.notifyMsg;
  std::shared_ptr<Waker> wk = d->waker;
  d->inbox->setWake([wk] { wk->wake(); });
  d->env.reset(new WinEnv(d->opt.home));

  const DeckBuild built = buildDeckOptions(d->opt, *d->env);
  std::shared_ptr<core::Inbox> in = d->inbox;
  d->ptys.reset(new WinPtyHost([in](uint64_t token, bool exit, uint32_t code, const char* b, size_t n) {
    if (exit) in->ptyExit(token, code); else in->ptyData(token, b, n);
  }, d->opt.ptyTrace));

  core::HostSeams sm;
  sm.clock = &winClock(); sm.files = &winFiles(); sm.ptys = d->ptys.get(); sm.procs = &d->procs; sm.random = &d->random; sm.env = d->env.get();

  // The phone / desk server (lib/phone.js): the core speaks HTTP and the websocket; the sockets are WinListener's.
  d->listener.reset(new WinListener([in](uint64_t conn, int what, const char* b, size_t n) {
    if (what == 0) in->netAccept(conn); else if (what == 1) in->netBytes(conn, b, n); else in->netClosed(conn);
  }));
  d->net.reset(new WinNet());
  sm.listener = d->listener.get(); sm.net = d->net.get();
  d->life.shell = d->opt.lifecycle; d->life.watchdog = d->opt.exitWatchdog;
  // (no shell lifecycle and no watchdog: the core is handed none, and behaves as it did before there was one)
  if (d->opt.lifecycle || d->opt.exitWatchdog) sm.lifecycle = &d->life;
  sm.notifier = d->opt.notifier;
  // Usage polling reads each account's sign-in token (read-only) and asks Anthropic's usage endpoint, as the
  // Electron deck does. Off unless the caller says so: see winhttp.hpp.
  if (built.wantsHttp) {
    d->http.reset(new WinHttpGet([in](std::function<void()> task) { in->task(std::move(task)); }));
    sm.http = d->http.get(); sm.credentials = &d->credentials;
  }

  // deliverInline: emit is called from inside start(), handle() or pump(), as each frame is raised (localdeck.hpp)
  d->host.reset(new core::Host(built, sm, d->inbox, true));
  Impl* self = d.get();
  d->window = d->host->addWindow(json(), [self](const json& frame) { if (self->emit) self->emit(frame); });
}
LocalDeck::~LocalDeck() { shutdown(); }

void LocalDeck::start() { d->host->start(); }
void LocalDeck::handle(const json& frame) { if (!d->down) d->host->handle(d->window, frame); }
void LocalDeck::pump() { if (!d->down) d->host->pump(); }
int LocalDeck::nextDueMs() const { return d->host->nextDueMs(); }      // (the server's heartbeat clock included)
bool LocalDeck::waitForWork(int ms) {
  std::unique_lock<std::mutex> g(d->waker->m);
  if (d->inbox->empty()) d->waker->cv.wait_for(g, std::chrono::milliseconds(ms), [&] { return !d->inbox->empty(); });
  return !d->inbox->empty();
}
void LocalDeck::shutdown() {
  if (d->down) return;
  d->down = true;
  d->host->shutdown();                         // the server, the listener (joins the socket thread: nothing is listening after this), every pty
  d->http.reset();                             // joins a request in flight (15 s at most)
}
ClaudeLaunch LocalDeck::lastClaudeLaunch(const std::string& tileId) const { return d->host->deck().lastClaudeLaunch(tileId); }
std::vector<uint32_t> LocalDeck::livePids() const { return d->host->deck().livePids(); }
std::pair<int, int> LocalDeck::ptySize(const std::string& tileId) const { return d->host->deck().ptySize(tileId); }
const std::string& LocalDeck::stateDir() const { return d->host->stateDir(); }
json LocalDeck::serveStatus() const { return d->host->phoneStatus(); }
core::Deck& LocalDeck::core() { return d->host->deck(); }
core::ClientId LocalDeck::windowClient() const { return d->window; }

}}  // namespace deck::host
