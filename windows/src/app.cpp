// windows/src/app.cpp — Claude Deck, native Windows shell. One exe, two modes, the same frames (core/PROTOCOL.md):
// REMOTE drives a Claude Deck running on another machine over the socket the browser desk uses (lib/phone.js,
// ws.hpp); LOCAL (--local) runs on a deck of its own in this process (windows/host, localtransport.hpp) and is
// then that deck's desktop window. `App::localMode` marks the few places where the two differ.
//
// This file is the glue the Electron renderer spreads over renderer/js/20-terminals.js (panes, fit,
// redraw), 25-layout.js (the grid, placement, the digit chord, zoom), 80-sidebar.js (steps, toggles),
// 85-focus.js (who has the keys) and renderer/desk/bridge.js (the socket). Each block names its source.
// The rules themselves are not here: the layout model is core/model/layout, the terminal is core/vt.
//
// What is drawn lives beside it, one file a thing, and this file only fills their models and answers their
// clicks and keys: sidebar.cpp (the sidebar and the context menu), overlays.hpp (the palette, Elsewhere / Past,
// move-all, the Special prompt form), settings.hpp (the Settings panel), termview.hpp (a pane), edit.hpp (every
// text box). script.hpp is the test script of --do. PARITY.md says, feature by feature, what matches Electron.
#include "gfx.hpp"
#include "ui.hpp"
#include "ws.hpp"
#include "wsraw.hpp"                            // the exe's own WebSocket client (permessage-deflate) + DeckSocket: which one
#include "termview.hpp"
#include "keys.hpp"
#include "mouse.hpp"
#include "config.hpp"
#include "script.hpp"
#include "deckhost.hpp"
#include "notify.hpp"
#include "lifecycle.hpp"
#include "asks.hpp"
#include "cloud.hpp"
#include "../../core/deck/version.hpp"
#include "../../core/deck/hash.hpp"
#include "update.hpp"
#include "../../core/model/layout.hpp"
#include "../../core/proto/frames.hpp"
#include <dwmapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>
#include <commdlg.h>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <cmath>

using nlohmann::json;
namespace L = deck::layout;
using namespace deck;

static const UINT WM_WS = WM_APP + 1;
static const UINT WM_LOCALDECK = WM_APP + 2;        // local mode: the deck has pty output or an exit waiting for pump() (to the hub)
static const UINT WM_TOASTCLICK = WM_APP + 3;       // to the hub: a toast was clicked (lParam = a heap std::string, the tile id)
static const UINT WM_SHELLTASK = WM_APP + 4;        // to the hub: run what was queued for after the current message (a restart, a window to open)
// the native deck's version (package.json's is the Electron app's): deck-state.json `deck.version`, the hello
// ONE source: core/deck/version.hpp (windows/res/app.rc reads the same header for the exe's version resource)
static const char* const kDeckVersion = deck::core::kNativeDeckVersion;
static const UINT_PTR TIMER_TIP = 8;
static const UINT_PTR TIMER_BOUNDS = 106;           // local mode: the window's place, told to the deck a moment after it stopped changing
static const UINT_PTR TIMER_ASKWARM = 105;          // the ask pane's web view, made ahead of an open: the pointer has rested on the asks tile (asks.hpp ASK_WARM_HOVER_MS)
static const UINT_PTR TIMER_UPDATE = 109;           // the first update check, a while after the start (update.hpp)
static const UINT_PTR TIMER_QUIETREDRAW = 108;     // a pane whose size changed takes the replay since the change (queueQuietRedraw)
static const UINT_PTR TIMER_WEBIDLE = 107;          // a cloud pane is off screen and still has its page: looked at again until its grace runs out (cloud.hpp)
static const UINT_PTR TIMER_BLINK = 1, TIMER_SHOT = 2, TIMER_DESKINIT = 3, TIMER_CLOCK = 4, TIMER_PROBE = 5, TIMER_SCRIPT = 6, TIMER_ANIM = 7, TIMER_LOCALDECK = 240;

struct Options {
  std::wstring link;            // --link <url>: use (and remember) this deck link
  std::wstring shot;            // --shot <png>: run hidden, save one frame, exit (tests)
  std::wstring windowShot;      // --window-shot <png>: (tests) the REAL window, parked off every screen, captured and closed
  std::wstring probe;           // --probe <png>: read-only look at a deck: list, attach one tile, save, exit
  std::wstring select;          // --select <tile id or title prefix>: the tile a shot / probe shows
  std::wstring deskName;        // --desk-name <name>: (tests) the name deskInit gives instead of this computer's
  std::wstring type;            // --type <text>: (tests, scratch decks only) type this into the selected tile
  std::wstring doScript;        // --do "<steps>": (tests) act out a script through the window's own handlers, then shoot (script.hpp)
  int after = 2500;             // --after <ms>: how long a shot / probe waits for the screen to settle
  int hang = 0;                 // --hang <ms>: (tests) how long a scripted test run may last before it is ended as hung (0 = 30 s, 120 s with a script; windows/test/soak.ps1)
  int width = 1400, height = 900;
  bool noSave = false;          // --no-save: remember nothing (tests)
  bool forget = false;          // --forget: drop the remembered link and ask again
  bool place = false;           // --place: (tests) place the selected tile in the grid
  bool local = false;           // --local: run on the in-process deck (windows/host) instead of a socket; skips the link prompt
  std::wstring stateDir;        // --state-dir <folder>: keep config.json and the local deck's state HERE (tests: a temp folder)
  std::wstring shell;           // --shell <exe>: (tests) the program a "New terminal" tile runs in local mode (default powershell.exe -NoLogo)
  bool persistShells = false;   // --persist-shells: local mode saves terminal tiles and brings them back (HOST-NOTES.md difference 1; default off = parity)
  std::wstring bench;           // --bench <log>: (tests) no deck at all: draw --frames frames of a fixture view on the bitmap target and on a parked window, write ms/frame
  std::wstring fixture;         // --fixture <meta.json>: the meta a --bench run shows (windows/test/fixtures/meta-sixty.json)
  int frames = 200;             // --frames <n>
  bool verifyScene = false;     // --verify-scene: (tests) every shot is also drawn whole and compared with the scene's frame, pixel by pixel (the log says)
  bool noScene = false;         // --no-scene: (tests) draw every frame whole, straight to the target: the picture the scene must equal
  std::string askPane;          // --ask-pane native | web: the ask pane drawn by the shell or in WebView2 (default: config.json askPane, else native)
  bool testAsks = false;        // --test-asks: (tests, a deck on 127.0.0.1 only) askDone / askClear are really sent: the ask pane's Done against a fake deck
  std::wstring webRuntime;      // --web-runtime <folder>: (tests) a fixed-version WebView2 runtime folder; one that is not there = a machine without the runtime
  int webWarm = -1;             // --web-warm 0|1: the ask pane may make its web view ahead of an open, when the pointer rests on the asks tile (default: 1 in a real run, 0 in a test)
  int askGrace = -1, cloudGrace = -1;   // --web-grace <ask ms>,<cloud ms>: (tests) how long a web view nobody can see lives (asks.hpp ASK_WEB_GRACE_MS, cloud.hpp CLOUD_OFFSCREEN_CLOSE_MS)
  std::wstring webFlags; bool webFlagsSet = false;   // --web-flags "<arguments>": (tests) the browser's command line instead of the built-in one (webpane.cpp kDefaultArgs), to measure a flag
  std::wstring fontsDir;        // --fonts-dir <folder>: where the bundled fonts' files are (default: assets\\fonts beside the exe, or config.json fontsDir); `none` = load none
  int servePort = 0;            // --serve <port>: local mode also serves a phone / a remote desk on 127.0.0.1:<port> for this run (HOST-NOTES.md "The server")
  std::wstring assets;          // --assets <folder>: where the pages the server serves live (default: looked for upwards from the exe)
  std::wstring home;            // --home <folder>: (tests) the local deck's home: accounts, the alias file, sessions and transcripts are looked for THERE, never in the user's own
  int allowClaude = -1;         // --allow-claude / --no-claude: the local deck may start Claude sessions (1) or not (0); -1 = the host's own
                                // default (windows/host/localdeck.hpp: on in a real run, off under --state-dir). The last of the two wins.
  bool driverClaim = false;     // --driver-claim: (tests) the local deck runs with the driver as a claim (core DeckOptions::driverClaim, a proposal that is
                                // OFF by default: its default lives in ONE line of core/deck/deck.hpp and the app follows it when this is not given)
  int registerMode = 0;         // --register (1) / --unregister (2): write / remove the Start-menu shortcut that carries the AppUserModelID, and exit.
                                // No window, no deck, no lock, no state (PACKAGING.md request 1). Exit 0 = done (or nothing of ours was there), 1 = failed,
                                // 2 = --unregister left a shortcut that points at something else
  std::wstring shortcutDir;     // --shortcut-dir <folder>: where (default: the user's Start menu Programs). TESTS ALWAYS NAME A TEMP FOLDER
  std::wstring shortcutArgs; bool shortcutArgsSet = false;   // --shortcut-args "<args>": the shortcut's arguments (the installer's --local)
  DWORD waitPid = 0;            // --wait-pid <pid>: a deck that was restarted waits for the old one to be gone before anything else (lifecycle.hpp)
  std::wstring cloudBase;       // --cloud-base http://127.0.0.1:<port>: (tests) the local page that stands in for claude.ai in a cloud pane (cloud.hpp)
  int forceDpi = 0;             // --dpi <n>: (tests) draw as if the monitor were at this DPI (144 = 150 %, 192 = 200 %)
};

// ---------------------------------------------------------------- the link prompt
// First run: where is the deck? One box for the link Settings -> Phone shows on the deck's own PC.
namespace linkdlg {
static HWND edit = nullptr; static bool done = false, ok = false, useLocal = false; static std::wstring text; static std::wstring errorLine;
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
      const wchar_t* lines = L"Paste the deck's link.\r\n\r\nOn the PC that runs " DECK_EDITION_PRODUCT L": Settings \x2192 Phone shows it "
        L"(it looks like https://your-pc.tailnet.ts.net/?t=\x2026). Tailscale must be on, on both machines.";
      HWND s = CreateWindowW(L"STATIC", lines, WS_CHILD | WS_VISIBLE, 16, 14, 468, 64, h, nullptr, nullptr, nullptr);
      SendMessageW(s, WM_SETFONT, (WPARAM)f, TRUE);
      edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 84, 468, 24, h, (HMENU)100, nullptr, nullptr);
      SendMessageW(edit, WM_SETFONT, (WPARAM)f, TRUE);
      HWND e = CreateWindowW(L"STATIC", errorLine.c_str(), WS_CHILD | WS_VISIBLE, 16, 114, 468, 18, h, (HMENU)101, nullptr, nullptr);
      SendMessageW(e, WM_SETFONT, (WPARAM)f, TRUE);
      HWND b1 = CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 312, 140, 82, 26, h, (HMENU)IDOK, nullptr, nullptr);
      HWND b2 = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 402, 140, 82, 26, h, (HMENU)IDCANCEL, nullptr, nullptr);
      // no deck to connect to: the same window on a deck of its own, in this process (local mode)
      HWND b3 = CreateWindowW(L"BUTTON", L"Use this PC (local preview)", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 16, 140, 190, 26, h, (HMENU)102, nullptr, nullptr);
      SendMessageW(b1, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(b2, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(b3, WM_SETFONT, (WPARAM)f, TRUE);
      SetFocus(edit);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(w) == IDOK) {
        wchar_t buf[2048]; GetWindowTextW(edit, buf, 2047); text = buf;
        DeckLink probe;
        if (!parseDeckLink(text, probe)) { SetWindowTextW(GetDlgItem(h, 101), L"That is not a deck link: it starts with http and ends with ?t= and the token."); return 0; }
        ok = true; done = true; DestroyWindow(h); return 0;
      }
      if (LOWORD(w) == IDCANCEL) { done = true; DestroyWindow(h); return 0; }
      if (LOWORD(w) == 102) { useLocal = true; done = true; DestroyWindow(h); return 0; }
      break;
    case WM_CLOSE: done = true; DestroyWindow(h); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}
// 0 = cancelled, 1 = a link was given (out), 2 = "Use this PC": local mode
static int ask(HINSTANCE inst, std::wstring& out, const std::wstring& why) {
  WNDCLASSW wc = {}; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"ClaudeDeckNativeLink";
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);
  done = ok = useLocal = false; text = out; errorLine = why;
  RECT r = { 0, 0, 500, 180 }; AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  HWND h = CreateWindowW(wc.lpszClassName, L"" DECK_EDITION_PRODUCT L" \x2014 connect to a deck", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
    (sw - (r.right - r.left)) / 2, (sh - (r.bottom - r.top)) / 2, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  MSG msg;
  while (!done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  }
  out = text;
  return useLocal ? 2 : ok ? 1 : 0;
}
}  // namespace linkdlg

// ---------------------------------------------------------------- the process
// What every window of this process shares. In LOCAL mode the process IS the deck (deckhost.hpp), and each of its
// top-level windows is one client of it — main.js's `wins`. The hub is a message-only window that outlives any one
// of them: the deck's wake-ups and its 50 ms timer, a clicked toast, a second launch and what must happen "after
// this message" all arrive there, so no window has to stay open for the others to keep working.
class App;
static void removeStaleOld();
static bool& updateAppliedAtStart();
struct Shell {
  DeckHost host;                                // local mode's deck
  ShellNotifier notifier;                       // its toasts and taskbar flash (notify.hpp)
  ShellLifecycle life;                          // restart, the update fingerprint (lifecycle.hpp)
  SingleInstance single;
  HWND hub = nullptr;
  std::vector<App*> apps;                       // every window there is, in the order they were made
  int exitCode = 0;
  bool restarting = false;                      // the deck asked for a restart: every window goes, the file keeps them all
  std::vector<std::function<void()>> tasks;     // WM_SHELLTASK
  void later(std::function<void()> f) { tasks.push_back(std::move(f)); if (hub) PostMessageW(hub, WM_SHELLTASK, 0, 0); }
  // REMOTE mode with more than one window (2026-11-19, PARITY "A second window"): the deck lets in ONE remote desk at
  // a time, so every window of this process shares the first window's socket (`remoteOwner`). What each window sends
  // is merged here (App::shareSend) and what the socket brings is handed to the windows (App::shareReceive).
  App* remoteOwner = nullptr;
  nlohmann::json remoteInit;                    // the owner's last deskInit answer: a window opened later starts from it
  std::map<std::string, std::set<App*>> remoteAttached;   // tile -> the windows that hold it attached
  std::map<std::string, std::set<App*>> remoteReplayTo;   // tile -> the windows its next replay is for
  std::map<App*, std::pair<std::set<std::string>, bool>> remoteShown;   // each window's shown set, and minimised
  std::string remoteShownKey;
  void remoteForget() { remoteAttached.clear(); remoteReplayTo.clear(); remoteShown.clear(); remoteShownKey.clear(); }
  int nextRid = 0;                              // a call's rid, unique across the windows (a shared socket's replies are routed by it)
};
static Shell& shell() { static Shell s; return s; }
// (defined after App)
static App* shellOpenWindow(App& from, const std::string& tileId, bool at, int sx, int sy);
static void shellRestoreWindows(App& first);
static void shellQuit(App* asking);
static void shellCloseOthers(App* keep);
static void shellToastClick(const core::Toast& toast);

// ---------------------------------------------------------------- the app (one window)
class App {
 public:
  HINSTANCE inst = nullptr;
  HWND hwnd = nullptr;
  Options opt;
  // config.json is the process's: the first window owns it, every other window reads and writes the same one
  Config cfgOwn; Config& cfg = cfgOwn;
  bool primary = true;                          // the window wWinMain made; the others are opened from it (shellOpenWindow)
  bool heapOwned = false;                       // made by shellNewWindow: deleted when its HWND goes
  bool showInactive = false;                    // a window the deck opened by itself: shown without taking the focus
  core::ClientId client = 0;                    // LOCAL: this window as the deck knows it (Deck::addWindow / openWindow)
  App() {}
  explicit App(App& first) : cfg(first.cfg), primary(false) {}
  Gfx gfx;
  DeckSocket ws;                                // config.json `wsClient`: auto (the exe's own, WinHTTP after a fallback) | winhttp | raw
  DeckLink link;
  // Local mode (--local, or "Use this PC" on the first run): the same App on a deck of its own in this process
  // (windows/host/localdeck.hpp) — same frames, no socket. This window is then THE DESKTOP WINDOW of that deck,
  // not a remote desk: where the two differ (the terminal's own replies, the OSC title) `localMode` says so.
  bool localMode = false;
  // Leaving the deck: the socket's full goodbye (ws.hpp: close frame, the deck's answer, the connection closed)
  // before anything else happens — the process exiting, another deck being connected to.
  // `quitting` false = a SWITCH to another deck (switchDeck): this process lives on, so the local deck's stop arms
  // only the watchdog for the processes it started, never the exit watchdog - which ended the whole exe 8 s after
  // "Connect to the remembered deck" (exit code 0, the office check of 2026-11-20).
  void stopTransport(bool quitting = true) {
    if (localMode) {
      Shell& sh = shell();
      // one window of several closing touches no session: its tiles drop out of that layout and keep running
      // (main.js `closed`). The last one is window-all-closed: every process ended, the watchdog armed first.
      if (sh.host.windowCount() > 1) { sh.host.removeWindow(client); client = 0; return; }
      if (sh.host.running()) {
        if (testRun()) logLine(std::string("local deck stopped: ") + (quitting ? "quit (exit watchdog)" : "switch (child watchdog only)"));
        else if (quitting) armExitWatchdog(sh.host.core().livePids());
        else armChildWatchdog(sh.host.core().livePids());
        sh.life.switching = !quitting;              // the core's shutdown arms the exit watchdog too: not for a switch
        sh.host.stop();
        sh.life.switching = false;
      }
      if (sh.hub) KillTimer(sh.hub, TIMER_LOCALDECK);
      client = 0; return;
    }
    if (shell().remoteOwner && shell().remoteOwner != this) { shareLeave(); return; }   // a window of several: the socket is the owner's
    if (shell().remoteOwner == this) { shell().remoteOwner = nullptr; shell().remoteForget(); }
    ws.stop();
    if (testRun()) { const WsClient::StopInfo i = ws.lastStop(); logLine("ws stop: close frame sent " + std::to_string(i.closeSent) + ", closed by the reader after the deck's answer " + std::to_string(i.readerDone && !i.closedHard) + ", closed hard " + std::to_string(i.closedHard) + ", " + std::to_string(i.ms) + " ms"); }
  }

  // what the deck told us
  json meta = json::object();
  json platform = json::object();
  std::string home;
  bool inited = false;          // deskInit answered (desk mode) / the hello arrived (probe)
  bool everOpened = false;
  bool wsOpenNoted = false;             // this run's first `ws open` went to ws.log (onWs WsOpen)
  int deskInitTries = 0;

  // the view (renderer/js/00-state.js)
  ui::State st;
  ui::Sidebar sidebar;
  std::map<std::string, std::unique_ptr<TermView>> terms;
  L::LayoutMap layout;
  L::Grid grid;
  std::string zoom;
  L::Cells emptySlot;
  std::set<int> chordPressed; std::set<UINT> chordHeld;
  std::set<std::string> attached;       // ids the socket carries (bridge.js)
  std::string shownKey;                 // the last `shown` message, so it is sent on change only
  std::map<std::string, std::pair<int, int>> sentSize;
  Theme theme;
  std::wstring monoFamily = L"Cascadia Mono";
  std::map<int, CellMetrics> metricsBySize;
  bool blinkOn = true, caretOn = true, fullscreen = false, windowFocused = true;
  ULONGLONG blinkEpoch = 0;             // when the cursor last became solid (a key restarts the blink)
  void blinkReset() { blinkEpoch = GetTickCount64(); blinkOn = caretOn = true; }
  WINDOWPLACEMENT beforeFullscreen = { sizeof(WINDOWPLACEMENT) };
  float viewW = 1400, viewH = 900;      // the client area in DIPs
  bool resizingSidebar = false, selecting = false;
  std::string selectingId;
  std::string flashName;                // a pane whose name flashes green: the redraw tap was taken
  ULONGLONG flashUntil = 0;
  HWND tip = nullptr; std::wstring tipText; ui::Hit tipHit;
  // the text boxes and small overlays the Electron renderer keeps in the DOM
  EditBox renameEdit, searchEdit;
  ui::CtxMenu ctx;
  ui::HoldForm hold;
  // the ask pane (49-asks.js): asks.hpp is its model and its web view; this file tells it the meta, where it is,
  // the keys and the clicks (WEB-NOTES.md "Wiring it in")
  asks::AskPane askPane;
  // the cloud panes (48-cloud.js): cloud.hpp holds one web view per cloud pane on show; this file tells it the meta,
  // where the panes are, what is drawn over them, and hands the keyboard over
  cloud::CloudPanes cloudPanes;
  std::string cloudWantFocus;           // a cloud tile was selected before its pane existed: its page takes the keys once it does
  std::string cloudHadFocus;            // the cloud page that had the keyboard when the window was deactivated
  ui::CloudDlg cdlg;                    // the New cloud session dialog (48-cloud.js openCloudDialog)
  ui::ColorPick cpick;                  // Settings' colour box picker (ui.hpp ColorPick)
  bool closing = false;                 // the window is going: nothing queued is acted on any more
  bool webHadFocus = false;             // the page had the keyboard when the window was deactivated: it gets it back
  // --do: a scripted run holds its own modifier keys (script.hpp)
  Mods fakeMods; bool scripting = false;
  Mods mods() const { return scripting ? fakeMods : currentMods(); }

  // calls waiting for ok / err (bridge.js `pending`)
  int rid = 0;
  std::map<int, std::function<void(bool, const json&)>> pending;

  bool headless() const { return !opt.shot.empty() || !opt.probe.empty(); }
  bool probing() const { return !opt.probe.empty(); }
  // any run a test drives (the off-screen ones, and the parked real window of --window-shot / --bench): the calls a
  // test must never make are refused in all of them
  bool testRun() const { return headless() || !opt.windowShot.empty() || !opt.bench.empty(); }

  // ------------------------------------------------------------ helpers over meta
  const json* tile(const std::string& id) const {
    if (!meta.contains("tiles") || !meta["tiles"].is_array()) return nullptr;
    for (const auto& t : meta["tiles"]) if (t.is_object() && t.value("id", std::string()) == id) return &t;
    return nullptr;
  }
  std::vector<std::string> tileIds() const {
    std::vector<std::string> out;
    if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"]) if (t.is_object()) out.push_back(t.value("id", std::string()));
    return out;
  }
  static std::string str(const json& o, const char* k) { auto it = o.find(k); return it != o.end() && it->is_string() ? it->get<std::string>() : std::string(); }
  json setting(const char* k) const { if (meta.contains("settings") && meta["settings"].is_object() && meta["settings"].contains(k)) return meta["settings"][k]; return json(); }
  float baseFontSize() const { json v = setting("fontSize"); return v.is_number() ? v.get<float>() : 14.f; }
  bool oneWindow() const { json v = setting("oneWindow"); return !(v.is_boolean() && v.get<bool>() == false); }
  bool sidebarRight() const { json v = setting("sidebarSide"); return v.is_string() && v.get<std::string>() == "right"; }
  // 30-tiles.js tileTitle
  std::string tileTitle(const json& t) const {
    std::string title = str(t, "title");
    if (!title.empty()) return title;
    std::string kind = str(t, "kind");
    if (kind == "shell") { std::string s = platform.is_object() ? str(platform, "shellName") : ""; return s.empty() ? "PowerShell" : s; }
    return kind == "cloud" ? "New cloud session" : "New session";
  }
  std::string acctAbbr(const json& t) const {
    std::string kind = str(t, "kind"); if (kind != "claude" && kind != "cloud") return "";
    std::string a = str(t, "account");
    if (meta.contains("accounts") && meta["accounts"].is_array()) for (const auto& ac : meta["accounts"]) if (ac.is_object() && str(ac, "id") == a) return str(ac, "abbr");
    return "";
  }
  // 20-terminals.js mirrorOf: the size somebody else owns, or none
  bool mirrorOf(const std::string& id, int& cols, int& rows) const {
    const json* t = tile(id); if (!t) return false;
    auto it = t->find("mirror"); if (it == t->end() || !it->is_object()) return false;
    cols = it->value("cols", 0); rows = it->value("rows", 0);
    return cols > 1 && rows > 1;
  }
  // REMOTE: never (the remote-desk rule, 2026-11-18: "'it only shows one place' should only be on the same box, it
  // shouldn't care about others on other boxes"). A session the deck's own windows show is, for a remote, a
  // session like any other: a click shows it, and showing it here takes it from nobody (the layout below).
  bool heldElsewhere(const std::string& id) const {
    if (!localMode) return false;
    const json* t = tile(id); if (!t) return false;
    auto it = t->find("inWindow"); return it != t->end() && it->is_number() && it->get<int>() != st.windowIndex;
  }

  // ------------------------------------------------------------ the socket (renderer/desk/bridge.js)
  // bridge.js queues what is sent before the socket first opens and loses everything across a reconnect (the page
  // reloads): nothing said while the deck is away may arrive later as a stale rename, input or layout.
  void send(const json& m) {
    if (!opt.bench.empty()) return;                     // a bench has no deck
    if (localMode) { if (client) shell().host.send(client, proto::encode(m)); return; }
    // a remote keeps its layout and its walks to itself (persistLayout): neither frame ever leaves this machine
    { auto t = m.find("t"); if (t != m.end() && t->is_string() && (*t == "layout" || *t == "leaving")) return; }
    if (shareSend(m)) return;
    if (everOpened && !st.connected) return;
    ws.send(proto::encode(m));
  }
  // ---- REMOTE, several windows on one socket (Shell::remoteOwner). True = handled here (sent through the owner's
  // socket, merged or dropped); false = this is the only window: as before.
  //   attach   always sent (the deck answers every attach with the session's replay), and the replay is meant for
  //            this window only (shareReceive); the window is counted as holding the tile
  //   detach   sent only when no other window still holds the tile
  //   shown    the union of every window's set (minimised only when every window is)
  //   resize   from the focused window, or when no other window holds the tile (two windows never fight over a size)
  std::vector<App*> remoteWindows() const { std::vector<App*> v; for (App* a : shell().apps) if (!a->localMode && !a->closing && a->hwnd) v.push_back(a); return v; }
  bool shareSend(const json& m) {
    Shell& s = shell();
    if (localMode || !s.remoteOwner || probing()) return false;
    App* o = s.remoteOwner;
    const std::string t = str(m, "t"), id = str(m, "id");
    const bool multi = remoteWindows().size() > 1;
    if (t == "attach" && !id.empty()) { s.remoteAttached[id].insert(this); s.remoteReplayTo[id].insert(this); }
    else if (t == "detach" && !id.empty()) {
      auto& w = s.remoteAttached[id]; w.erase(this); s.remoteReplayTo[id].erase(this);
      if (!w.empty()) { if (testRun()) logLine("share: detach " + id + " kept (another window holds it)"); return true; }
    } else if (t == "shown") {
      std::set<std::string> ids; if (m.contains("ids") && m["ids"].is_array()) for (const auto& x : m["ids"]) if (x.is_string()) ids.insert(x.get<std::string>());
      s.remoteShown[this] = { ids, m.value("hidden", false) };
      if (multi || o != this) {
        std::set<std::string> all; bool hidden = true;
        for (App* a : remoteWindows()) { auto it = s.remoteShown.find(a); if (it == s.remoteShown.end()) continue; all.insert(it->second.first.begin(), it->second.first.end()); hidden = hidden && it->second.second; }
        const std::vector<std::string> v(all.begin(), all.end());
        const std::string key = json(v).dump() + (hidden ? "h" : "v");
        if (key == s.remoteShownKey) return true;
        s.remoteShownKey = key;
        if (testRun()) logLine("share: shown " + json(v).dump() + (hidden ? " hidden" : ""));
        if (!(o->everOpened && !o->st.connected)) o->ws.send(proto::encode(proto::shown(v, hidden)));
        return true;
      }
      s.remoteShownKey.clear();
    } else if (t == "resize" && multi && !windowFocused) {
      for (App* a : remoteWindows()) if (a != this && a->attached.count(id)) { if (testRun()) logLine("share: resize " + id + " not sent (an unfocused window; another holds it)"); return true; }
    }
    if (o == this) return false;                        // the owner's own frames go out its own socket, as before
    if (o->everOpened && !o->st.connected) return true;
    if (testRun() && (t == "attach" || t == "detach")) logLine("share: " + t + " " + id + " from window " + std::to_string(windowNo()));
    o->ws.send(proto::encode(m));
    return true;
  }
  int windowNo() const { int i = 0; for (App* a : shell().apps) { if (a == this) return i; if (!a->localMode) i++; } return -1; }
  // What the owner's socket brought: a reply to the window that asked; a replay to the window(s) that attached for it;
  // everything else to every window (a frame for a tile a window does not show finds no pane there and is dropped).
  void shareReceive(const json& m) {
    Shell& s = shell();
    const std::string t = str(m, "t");
    const bool replay = t == "data" && m.value("replay", false);
    std::set<App*> replayTo;
    if (replay) { auto it = s.remoteReplayTo.find(str(m, "id")); if (it != s.remoteReplayTo.end()) { replayTo = it->second; s.remoteReplayTo.erase(it); } }
    const std::vector<App*> wins = remoteWindows();
    if (s.remoteOwner != this || wins.size() <= 1) { onMessage(m); return; }
    if (t == "ok" || t == "err") { const int rid = m.value("rid", 0); for (App* a : wins) if (a->pending.count(rid)) { a->onMessage(m); return; } return; }
    if (replay) { for (App* a : wins) if (replayTo.count(a)) { if (testRun()) logLine("share: replay " + str(m, "id") + " -> window " + std::to_string(a->windowNo())); a->onMessage(m); } return; }
    for (App* a : wins) a->onMessage(m);
  }
  // a window of several leaves the shared socket: what only it held is let go
  void shareLeave() {
    Shell& s = shell();
    if (localMode || !s.remoteOwner || s.remoteOwner == this) return;
    App* o = s.remoteOwner;
    for (auto& kv : s.remoteAttached) if (kv.second.erase(this) && kv.second.empty() && o->st.connected) o->ws.send(proto::encode(proto::detach(kv.first)));
    for (auto& kv : s.remoteReplayTo) kv.second.erase(this);
    s.remoteShown.erase(this); s.remoteShownKey.clear();
    if (o->st.connected && o->inited) { o->shownKey.clear(); o->syncShown(); }   // the union again, without this window
    if (testRun()) logLine("share: window " + std::to_string(windowNo()) + " left");
  }
  // A reconnect is a page reload in the browser desk: every overlay shut, nothing half-done kept (a rename in
  // progress is dropped, not saved), no pane, zoom, walk or drag carried over. deskInit then restores the layout.
  void resetView() {
    if (hwnd && GetCapture() == hwnd) { releasing = true; ReleaseCapture(); releasing = false; }
    ghostHide();
    dragSrc.clear(); st.dragging.clear(); st.dropMark.clear(); previewSet.clear(); underPreview.clear(); growingId.clear(); leaving.clear();
    press = Press(); resizingSidebar = false; selecting = false; selectingId.clear(); remoteClick = RemoteClick(); editDrag = nullptr; st.pressed.clear();
    thumbDrag = false; dragScrollAmt = 0; mousePane.clear(); mouseButtons = 0; focusPane.clear();
    chordPressed.clear(); chordHeld.clear();
    stepSnap = StepSnap(); stepLeavingElsewhere.clear();
    closeCtxMenu(); closePalette(); closePull(); closeMoveMenu(); closeCloudDialog();
    askPane.asksBlur(); askPane.closePane();            // the page reloads: the ask pane is shut with everything else
    if (sset.open || widthBeforeSettings >= 0) closeSettings();
    st.renaming.clear(); st.renameEdit = nullptr;
    closeSearch(); closeHoldForm(); st.confirm.clear(); st.focusEdit = nullptr;
    nowConfirmUntil = 0; st.restartConfirming = false;
    zoom.clear(); emptySlot.clear(); layout.clear(); st.shown.clear(); beforeClick.clear(); nameClickId.clear(); pendingNew.clear();
    paneRedrawQueue.clear(); redrawOwed.clear(); startPending.clear(); altPending.clear(); knownTiles.clear(); metaSeen = false; paneRectKey.clear();
    terms.clear(); nameRects.clear();
    if (hwnd) { KillTimer(hwnd, 100); KillTimer(hwnd, 101); KillTimer(hwnd, 102); }
    paneNoteText.clear(); paneNoteUntil = 0;
  }
  bool metaSeen = false;                                // a meta has been applied since the (re)connect
  std::map<std::string, json> fakeTile;                 // (tests) tile id -> fields laid over every meta
  json fakeMeta = json::object();                       // (tests) `fakeset <path> <json>`: fields laid over every meta (settings.effortStyle, restart …)
  // A headless run (a test) never sends a call that would start, move or adopt a Claude session, restart the deck
  // or rewrite its accounts or settings: the button can be pressed and the log read, nothing more happens.
  static bool blockedInTests(const std::string& fn, const json& args) {
    static const char* never[] = { "adoptSession", "moveAll", "moveTile", "restartNow", "restartWhenQuiet", "updateLater", "saveAccounts", "saveSettings", "askDone", "askClear" };
    for (const char* n : never) if (fn == n) return true;
    if (fn == "newTile") { const json a = args.is_array() && !args.empty() ? args[0] : json(); return !(a.is_object() && a.value("kind", std::string()) == "shell"); }
    return false;
  }
  // A headless LOCAL run is a throwaway deck in a temp folder (--state-dir): saving its settings or renaming its
  // tiles touches nothing else, so only the one thing no test may ever do stays blocked: a tile that is not a shell.
  bool blockedHere(const std::string& fn, const json& args) const {
    // --test-asks: the ask pane's Done and Discard against a test deck on loopback (the fake deck of windows/web-test)
    if (opt.testAsks && !localMode && link.host == L"127.0.0.1" && (fn == "askDone" || fn == "askClear")) return false;
    if (!localMode) return blockedInTests(fn, args);
    // (… and with --allow-claude a test's deck COULD start Claude: nothing that starts, adopts or moves a session,
    // wakes one, or rewrites the alias file is ever sent from a test)
    for (const char* n : { "adoptSession", "moveAll", "moveTile", "saveAccounts", "wakeTile", "restartTile", "refreshTile" }) if (fn == n) { if (fn == "wakeTile" || fn == "restartTile" || fn == "refreshTile") { const json* t = args.is_array() && !args.empty() && args[0].is_string() ? tile(args[0].get<std::string>()) : nullptr; return !(t && str(*t, "kind") == "shell"); } return true; }
    return fn == "newTile" && blockedInTests(fn, args);
  }
  void call(const std::string& fn, json args, std::function<void(bool, const json&)> cb = nullptr) {
    if (testRun()) {
      const bool blocked = blockedHere(fn, args);
      if (fn != "deskInit") logLine(std::string(blocked ? "BLOCKED " : "call ") + fn + " " + args.dump());
      if (blocked) return;
    }
    int id = cb ? ++shell().nextRid : 0;
    if (cb) pending[id] = cb;
    send(proto::call(id, fn, args));
  }
  void fire(const std::string& fn, json args) { send(proto::call(0, fn, args)); }
  // A call the deck refused (`err`). Electron writes it into the session's pane in red (30-tiles.js `report`,
  // `reportToActive`) or into the panel that asked; a refusal with no pane to write into is the pane note.
  // Nothing a person asked for fails silently — the local deck refuses a good part of the table (HOST-NOTES.md).
  static std::string errText(const json& e) { return e.is_string() ? e.get<std::string>() : e.dump(); }
  void refused(const std::string& id, const json& e) {
    if (!id.empty() && terms.count(id) && st.shown.count(id)) { report(id, e); return; }
    if (!st.selected.empty() && terms.count(st.selected) && st.shown.count(st.selected)) { report(st.selected, e); return; }
    note(errText(e), 4000);
  }
  std::function<void(bool, const json&)> orSay(const std::string& id = "") { return [this, id](bool ok, const json& e) { if (!ok) refused(id, e); }; }

  void onWs(WPARAM ev, const std::string& text) {
    switch (ev) {
      case WsOpen:
        // a reconnect is a fresh window as far as the deck is concerned: start over, as the page reloads
        attached.clear(); shownKey.clear(); sentSize.clear(); pending.clear(); inited = false; deskInitTries = 0;
        mirroredIds.clear(); reassertAt.clear(); resizeGen.clear();
        for (auto& kv : terms) { kv.second->attached = false; }
        if (everOpened) resetView();                    // bridge.js: `if (everOpened) { location.reload(); return; }`
        if (shell().remoteOwner == this) { shell().remoteForget(); for (App* a : remoteWindows()) if (a != this) a->ownerReconnected(); }
        st.connected = true; st.notice.clear(); st.noticeDetail.clear();
        everOpened = true;
        // REMOTE: the first open of a run (of each deck switchDeck goes to) is said in ws.log beside config.json as well (a real run too, not only a
        // test's log): which client carries the socket and whether the deck's frames come compressed - the office
        // deck logs nothing per client (2026-11-24, the home box's first compressed run). Later opens: the test log only.
        if (!localMode && !wsOpenNoted) { wsOpenNoted = true; wsNote("ws open: " + ws.describe()); }
        else if (testRun() && !localMode) logLine("ws open: " + ws.describe());
        if (!probing()) deskInit();
        invalidate();
        break;
      case WsFallback:
        // the exe's own client gave up on its own path (TLS, the upgrade, a frame it cannot read: `text`); it has
        // ended, a WsClosed went before when it had been open. The rest of this run is WinHTTP's, uncompressed - said
        // once, in the test log and in ws.log beside config.json.
        if (localMode || !ws.fallBack()) break;
        wsNote("ws: the exe's own socket failed (" + text + "); WinHTTP for the rest of this run, without compression");
        break;
      case WsMessage: {
        json m = json::parse(text, nullptr, false);
        if (m.is_object()) shareReceive(m);
        break;
      }
      case WsClosed:
      case WsFailed:
        ws.clearQueue();
        st.connected = false; inited = false;
        st.notice = everOpened ? "The deck is away \xE2\x80\x94 reconnecting\xE2\x80\xA6" : "Connecting to the deck\xE2\x80\xA6";
        st.noticeDetail = ev == WsFailed ? text : "It comes back by itself when the deck is reachable.";
        if (headless() && ev == WsFailed && !everOpened) { failHeadless(text); return; }
        pending.clear();                                // bridge.js: the page reloads on reconnect, so no promise of the old socket ever settles
        if (shell().remoteOwner == this) for (App* a : remoteWindows()) if (a != this) { a->st.connected = false; a->inited = false; a->st.notice = st.notice; a->st.noticeDetail = st.noticeDetail; a->pending.clear(); a->invalidate(); }
        invalidate();
        break;
    }
  }
  // (a window of several) the owner's socket came back: a reload, as the owner's own - its layout is kept and comes
  // back when the owner's deskInit has answered (rejoinRemote)
  json keptLayout; std::string keptSelected;
  void ownerReconnected() {
    keptLayout = json::object(); for (const auto& kv : layout) keptLayout[kv.first] = kv.second; keptSelected = st.selected;
    attached.clear(); shownKey.clear(); sentSize.clear(); pending.clear(); inited = false;
    mirroredIds.clear(); reassertAt.clear(); resizeGen.clear();
    for (auto& kv : terms) kv.second->attached = false;
    resetView(); st.connected = true; st.notice.clear(); st.noticeDetail.clear(); invalidate();
  }

  // ---- what the deck is told about this window (LOCAL; HOST-NOTES.md edits 1b, 3 and the bounds of R7)
  // On screen (not minimised) and focused: hibernation's "on screen", the driver's "watching", and a toast only
  // while no deck window has the focus (main.js maybeNotify, winVisible).
  int lastWinState = -1;
  void reportWindowState() {
    if (!localMode || !client || !shell().host.running()) return;
    const bool visible = !minimised() && (testRun() || !hwnd || IsWindowVisible(hwnd));
    const int k = (visible ? 2 : 0) | (windowFocused ? 1 : 0);
    if (k == lastWinState) return;
    if (testRun() && lastWinState >= 0) logLine("windowState w" + std::to_string(st.windowIndex) + " visible " + std::to_string(visible) + " focused " + std::to_string(windowFocused));
    lastWinState = k;
    shell().host.core().windowState(client, visible, windowFocused);
  }
  // the window's NORMAL rectangle as main.js keeps it: { x, y, width, height, maximized } (a test's window has no place)
  json boundsJson() const {
    if (!hwnd || testRun()) return json{ { "width", opt.width }, { "height", opt.height } };
    WINDOWPLACEMENT wp = { sizeof(wp) }; if (!GetWindowPlacement(hwnd, &wp)) return json();
    const RECT& r = wp.rcNormalPosition;
    return json{ { "x", (int)r.left }, { "y", (int)r.top }, { "width", (int)(r.right - r.left) }, { "height", (int)(r.bottom - r.top) },
      { "maximized", wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED)) } };
  }
  void reportBounds() { if (localMode && client && !testRun() && !fullscreen && shell().host.running()) shell().host.core().windowBounds(client, boundsJson()); }
  // a key went down anywhere in this window (main.js windowKeys): the user is at this PC — deck-state.json `where`
  void noteKey() { if (localMode && client && shell().host.running()) shell().host.core().windowKey(client); }

  // Where the pages the server serves are looked for when --assets names no folder (PACKAGING.md request 4: "do not
  // look above a packaged exe"): the exe's own folder — a package ships renderer\, lib\ and node_modules\@xterm
  // beside it — and, ONLY for an exe that sits in a checkout's windows\build, that checkout's root (as
  // loadBundledFonts does for the fonts). Never any other folder above: an installed exe whose renderer\ went
  // missing serves nothing and says so (phoneStatus.assets = 'missing' with this folder as the one tried), instead
  // of quietly serving whatever renderer\phone\index.html sits somewhere above it. The host's own five-levels
  // climb only runs when it is handed no folder, so it never runs for the app.
  static std::wstring ownAssetRoot() {
    std::wstring dir = ownExePath(); { const size_t sl = dir.find_last_of(L"\\/"); dir = sl == std::wstring::npos ? std::wstring() : dir.substr(0, sl); }
    auto has = [](const std::wstring& d) { const DWORD a = GetFileAttributesW((d + L"\\renderer\\phone\\index.html").c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); };
    if (has(dir)) return dir;
    static const wchar_t tail[] = L"\\windows\\build";
    const size_t tn = wcslen(tail);
    if (dir.size() > tn && CompareStringOrdinal(dir.c_str() + dir.size() - tn, (int)tn, tail, (int)tn, TRUE) == CSTR_EQUAL) { const std::wstring up = dir.substr(0, dir.size() - tn); if (has(up)) return up; }
    return dir;
  }

  // One deck at a time + the shared layout (core/deck/onedeck.hpp; lib/onedeck.js): what a LOCAL deck tells the core.
  // The shared folder is %APPDATA%\claude-deck-shared (the edition's DECK_EDITION_SHARED_DIR) for the deck on its own folder; a --state-dir deck (every test)
  // keeps it under its own folder unless CLAUDE_DECK_SHARED_DIR names one, and only such a deck reads the test's
  // stand-ins for the other deck (CLAUDE_DECK_OTHER_EXE, CLAUDE_DECK_OTHER_STATE).
  json oneDeckOptions() const {
    const bool def = opt.stateDir.empty();
    std::wstring shared = envW(L"CLAUDE_DECK_SHARED_DIR");
    if (shared.empty()) {
      if (def) { const std::wstring c = configDir(); const size_t sl = c.find_last_of(L"\\/"); shared = (sl == std::wstring::npos ? c : c.substr(0, sl)) + L"\\" DECK_EDITION_SHARED_DIR; }
      else shared = opt.stateDir + L"\\local\\onedeck-shared";
    }
    json args = json::array({ "--local" });
    if (!def) { args.push_back("--state-dir"); args.push_back(narrow(opt.stateDir)); }
    json o = { { "deck", "native" }, { "sharedDir", narrow(shared) }, { "exe", narrow(ownExePath()) }, { "args", args }, { "defaultDeck", def } };
    if (!def) { const std::wstring ox = envW(L"CLAUDE_DECK_OTHER_EXE"), os = envW(L"CLAUDE_DECK_OTHER_STATE"); if (!ox.empty()) o["otherExe"] = narrow(ox); if (!os.empty()) o["otherState"] = narrow(os); }
    return o;
  }

  // ---- one deck at a time: the strip a blocked deck shows (the other Claude Deck runs with sessions). The deck was
  // never started: it resumed no session and wrote nothing (no marker, no shared layout, not even its own files).
  // mode 1 = blocked; mode 2 = the start-up question (both decks changed the layout): which wins. `resolved` / `publish`:
  // the answer, carried into the start that follows it
  struct OneDeckUi { int mode = 0; std::string text, detail; std::vector<std::pair<RectF, std::string>> buttons; json shared, mine, theirs; bool resolved = false; std::string publish; std::shared_ptr<host::LocalDeckOptions> lo; };
  OneDeckUi od;
  void oneDeckBlocked(const core::onedeck::Busy& b) {
    od.mode = 1; od.text = core::onedeck::blockedText(b.n); od.detail = b.why + ". This deck resumed no session and wrote nothing.";
    if (testRun()) logLine("onedeck: blocked - " + od.text + " (" + b.why + ")");
    if (primary && (headless() || !opt.windowShot.empty()) && !scriptStarted) SetTimer(hwnd, TIMER_SHOT, opt.after, nullptr);
    updateTitle(); invalidate();
  }
  static std::string oneDeckWhen(const json& c) {
    if (!c.is_object() || !c.contains("at") || !c["at"].is_string()) return "no change recorded";
    std::string at = c["at"].get<std::string>();                  // ISO, UTC: shown as "2026-11-24 15:20 UTC"
    if (at.size() >= 16) at = at.substr(0, 10) + " " + at.substr(11, 5) + " UTC";
    std::string s = (c.contains("what") && c["what"].is_string() ? c["what"].get<std::string>() : std::string("a change")) + ", " + at;
    if (c.contains("by") && c["by"].is_string()) s += " (the " + c["by"].get<std::string>() + " deck)";
    return s;
  }
  void oneDeckAsk(const json& pre, const host::LocalDeckOptions& lo) {
    const json& d = pre["decision"];
    od.lo = std::make_shared<host::LocalDeckOptions>(lo);
    od.mode = 2; od.shared = pre.value("shared", json()); od.mine = d.value("mine", json()); od.theirs = d.value("theirs", json());
    od.text = "Which layout should this " DECK_EDITION_PRODUCT " start with?"; od.detail = d.value("why", std::string());
    if (testRun()) logLine("onedeck: ask - " + od.detail + " | shared: " + oneDeckWhen(od.theirs) + " | this deck: " + oneDeckWhen(od.mine));
    if (primary && (headless() || !opt.windowShot.empty()) && !scriptStarted) SetTimer(hwnd, TIMER_SHOT, opt.after, nullptr);
    invalidate();
  }
  // the question's answer: the side that loses is backed up, then the deck starts
  void oneDeckAnswer(const std::string& a, host::LocalDeckOptions& lo) {
    if (a == "shared") { const std::string b = host::oneDeckImport(lo, od.shared, "chose-shared"); od.publish.clear(); if (testRun()) logLine("onedeck: the shared layout wins; this deck's files backed up in " + b); }
    else { const std::string b = host::oneDeckKeepOwn(lo, od.shared); od.publish = "kept this deck's own layout at start"; if (testRun()) logLine("onedeck: this deck's own layout wins; the shared one backed up in " + b); }
  }
  // a plain strip across the top (blocked: the words, then Quit), or the question in the middle of the window (two rows,
  // each side's most recent change and its button)
  void drawOneDeck() {
    od.buttons.clear();
    if (!od.mode) return;
    if (od.mode == 2) {
      Font h; h.family = L"Segoe UI"; h.size = 17; Font f = h; f.size = 14; Font fd = f; fd.size = 12;
      gfx.fill(RectF(0, 0, viewW, viewH), Rgba::hex(0x0c0c0c, 0.92f));
      const float w = std::min(720.f, viewW - 32), x = std::round((viewW - w) / 2), y = 60;
      gfx.text(widen(od.text), RectF(x, y, w, 26), h, Rgba::hex(0xf0d9a0));
      gfx.text(widen(od.detail), RectF(x, y + 28, w, 18), fd, Rgba::hex(0xa89a7a));
      const struct { const wchar_t* name; std::string last; const wchar_t* btn; const char* id; } rows[2] = {
        { L"The shared layout", "Most recent change: " + oneDeckWhen(od.theirs), L"Use it", "shared" },
        { L"This deck's own layout", "Most recent change: " + oneDeckWhen(od.mine), L"Keep it", "own" } };
      for (int i = 0; i < 2; i++) {
        const RectF r(x, y + 64 + i * 74, w, 62);
        gfx.strokeRound(r, 6, Rgba::hex(0x333333), 1);
        gfx.text(rows[i].name, RectF(r.x + 14, r.y + 10, r.w - 140, 20), f, Rgba::hex(0xdddddd));
        gfx.text(widen(rows[i].last), RectF(r.x + 14, r.y + 33, r.w - 140, 18), fd, Rgba::hex(0x9a9a9a));
        const RectF b(r.r() - 110, r.y + 15, 96, 32);
        gfx.fillRound(b, 5, Rgba::hex(0x3a3a3a)); gfx.strokeRound(b, 5, Rgba::hex(0x666666), 1);
        gfx.text(rows[i].btn, b, f, Rgba::hex(0xeeeeee), Align::Center);
        od.buttons.push_back({ b, rows[i].id });
      }
      gfx.text(L"The other one is backed up either way (in this deck's onedeck-backups folder).", RectF(x, y + 64 + 2 * 74 + 4, w, 18), fd, Rgba::hex(0x7a7a7a));
      return;
    }
    Font f; f.family = L"Segoe UI"; f.size = 14; Font fd = f; fd.size = 12;
    const RectF strip(0, 0, viewW, 58);
    gfx.fill(strip, Rgba::hex(0x2b2414)); gfx.fill(RectF(0, strip.b() - 1, viewW, 1), Rgba::hex(0x6b5523));
    const RectF quit(viewW - 96, 13, 80, 32);
    gfx.fillRound(quit, 5, Rgba::hex(0x3a3a3a)); gfx.strokeRound(quit, 5, Rgba::hex(0x666666), 1);
    gfx.text(L"Quit", quit, f, Rgba::hex(0xeeeeee), Align::Center);
    od.buttons.push_back({ quit, "quit" });
    gfx.text(widen(od.text), RectF(16, 8, viewW - 128, 22), f, Rgba::hex(0xf0d9a0));
    gfx.text(widen(od.detail), RectF(16, 31, viewW - 128, 18), fd, Rgba::hex(0xa89a7a));
  }
  // a press on the strip; while it is up every other press is swallowed
  bool oneDeckClick(float x, float y) {
    if (!od.mode) return false;
    for (const auto& b : od.buttons) if (b.first.has(x, y)) {
      if (testRun()) logLine("onedeck: " + b.second);
      if (b.second == "quit") shellQuit(this);
      else if (od.mode == 2 && (b.second == "shared" || b.second == "own")) {
        if (od.lo) oneDeckAnswer(b.second, *od.lo);
        od.mode = 0; od.resolved = true; od.buttons.clear();
        KillTimer(hwnd, TIMER_SHOT);
        startTransport();                               // the deck starts now, on the winning layout
        invalidate();
      }
      return true;
    }
    return true;
  }

  // The deck this window is on, started: the one in this process (LOCAL) or the socket to `link` (REMOTE).
  void startTransport() {
    if (localMode) {
      // the deck in this process: its hello and every later frame arrive as posted WM_WS messages, pty output wakes
      // WM_LOCALDECK, and its timers (resize debounce, spawn queue, meta tick) run from pump() on a 50 ms timer
      host::LocalDeckOptions lo;
      if (!opt.stateDir.empty()) lo.stateDir = narrow(opt.stateDir) + "\\local";
      lo.persistShells = opt.persistShells || cfg.j.value("persistShells", false);
      if (!opt.shell.empty()) { lo.shellFile = opt.shell; lo.shellArgs.clear(); size_t sl = opt.shell.find_last_of(L"\\/"); std::wstring n = sl == std::wstring::npos ? opt.shell : opt.shell.substr(sl + 1); size_t dot = n.rfind(L'.'); if (dot != std::wstring::npos) n = n.substr(0, dot); lo.shellName = narrow(n); }
      // `--serve <port>` / `--assets <folder>`: the phone / desk server on loopback for this run (HOST-NOTES.md edit 1)
      lo.servePort = opt.servePort; lo.assetRoot = narrow(opt.assets.empty() ? ownAssetRoot() : opt.assets);
      if (!opt.home.empty()) { lo.home = narrow(opt.home); lo.aliasFile = lo.home + "\\.claude-code-account-aliases"; }
      // lo.allowClaude and lo.usagePolling are what windows/host/localdeck.hpp defaults them to (the core/host
      // side's switches, on the owner's word: Claude on in a real run, off under --state-dir; `--allow-claude` /
      // `--no-claude` override — the host reads the two off the command line itself, and the parser's copy says
      // the same). The shell only READS the result: st.claudeOff is what dims the account buttons and Move.
      if (opt.allowClaude >= 0) lo.allowClaude = opt.allowClaude == 1;
      if (opt.driverClaim) lo.driverClaim = true;
      lo.oneDeck = oneDeckOptions();
      st.connected = false; st.claudeOff = !lo.allowClaude; st.ownLayoutOnly = false; st.officeScreen = true; st.notice.clear(); st.noticeDetail.clear();
      // ONE DECK AT A TIME (2026-11-24): the other deck runs with live sessions = this one is not started at all
      { const core::onedeck::Busy guard = host::oneDeckGuard(lo); if (guard.busy) { oneDeckBlocked(guard); return; } }
      // THE SHARED LAYOUT, before this deck writes anything shared: import it, ask, or note that this deck writes it
      if (!od.resolved) {
        const json pre = host::oneDeckPreflight(lo);
        const std::string action = pre["decision"].value("action", std::string("none"));
        if (testRun()) logLine("onedeck: at start: " + action + " - " + pre["decision"].value("why", std::string()));
        if (action == "ask") { oneDeckAsk(pre, lo); return; }
        if (action == "import") { const std::string b = host::oneDeckImport(lo, pre["shared"], "import"); if (testRun()) logLine("onedeck: imported the shared layout; this deck's files backed up in " + b); }
        else if (action == "write") od.publish = "write";
        od.resolved = true;
      }
      if (!od.publish.empty()) lo.oneDeck["publishAtStart"] = od.publish;
      Shell& sh = shell();
      DeckHost::Extra x; x.version = kDeckVersion; x.notifier = &sh.notifier; x.lifecycle = &sh.life; x.test = testRun();
      sh.restarting = false;
      sh.host.create(lo, x, sh.hub, WM_LOCALDECK);
      lastWinState = -1;
      client = sh.host.addWindow(hwnd, WM_WS, boundsJson());   // before start(): the first window takes the first saved place (R7)
      sh.host.start();                          // the hello follows; a window ignores it and calls deskInit
      if (sh.hub) SetTimer(sh.hub, TIMER_LOCALDECK, 50, nullptr);
      reportWindowState();
      shellRestoreWindows(*this);               // R7: the other windows the state file holds come back (R13: not an empty one)
    } else {
      st.notice = "Connecting to the deck\xE2\x80\xA6"; st.noticeDetail.clear(); st.connected = false; st.claudeOff = false; st.ownLayoutOnly = true; st.officeScreen = false;
      shell().remoteOwner = this; shell().remoteForget();
      const std::string wc = cfg.j.contains("wsClient") && cfg.j["wsClient"].is_string() ? cfg.j["wsClient"].get<std::string>() : std::string("auto");
      ws.setMode(DeckSocket::modeOf(wc));
      ws.start(link, hwnd, WM_WS);
    }
  }
  // a line about the socket that a person may want to find later: the test log, and ws.log beside config.json
  // (this machine's folder, or the test's --state-dir; kept under 64 KB)
  void wsNote(const std::string& line) {
    logLine(line);
    if (cfg.path.empty()) return;
    const std::wstring f = cfg.path.substr(0, cfg.path.find_last_of(L"\\/") + 1) + L"ws.log";
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    const bool big = GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 65536;
    std::ofstream o(f.c_str(), std::ios::binary | (big ? std::ios::trunc : std::ios::app));
    SYSTEMTIME t; GetLocalTime(&t); char ts[32]; snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    o << ts << line << "\n";
  }
  // Another deck, without leaving the window: the palette's "Connect to another deck…" / "Use this PC (local
  // preview)" (the first run's link box asks the same two things; --forget is no longer the only way back to
  // them). Leaving is complete before arriving starts: the socket's goodbye (ws.hpp) or the local deck's shutdown
  // (its terminals end), every frame of the old deck still in the queue thrown away, the view emptied as a page
  // reload empties it. Then the new transport starts and deskInit fills the window again. Remembered like the
  // first run's answer (config.json `mode`, `link`).
  void switchDeck(bool toLocal, const DeckLink* to) {
    if (probing() || !opt.bench.empty() || !hwnd) return;
    if (testRun()) {
      // a test's local deck lives in its --state-dir, and a test's socket goes to loopback only
      if (toLocal && opt.stateDir.empty()) { logLine("BLOCKED switch to local: no --state-dir"); return; }
      if (!toLocal && to && to->host != L"127.0.0.1") { logLine("BLOCKED switch: a test connects to 127.0.0.1 only"); return; }
      logLine(std::string("switch deck -> ") + (toLocal ? "local" : "remote " + narrow(to ? to->host : link.host) + ":" + std::to_string(to ? to->port : link.port)));
    }
    shellCloseOthers(this);                             // another deck: this window alone goes there (a remote has one window)
    stopTransport(false);                               // a switch, not a quit: the process lives on
    KillTimer(hwnd, TIMER_DESKINIT);
    { MSG m; while (PeekMessageW(&m, hwnd, WM_WS, WM_LOCALDECK, PM_REMOVE)) if (m.message == WM_WS) delete (std::string*)m.lParam; }
    resetView();
    meta = json::object(); metaGen++; paneMetaSig = 0; platform = json::object(); home.clear();
    cloudPanes.setMeta(meta); cloudWantFocus.clear(); cloudHadFocus.clear();   // another deck: its cloud panes go (the sign-ins stay, per account)
    inited = false; everOpened = false; wsOpenNoted = false; deskInitTries = 0; pending.clear(); attached.clear(); shownKey.clear(); sentSize.clear();
    mirroredIds.clear(); reassertAt.clear(); resizeGen.clear();
    st.selected.clear(); st.shown.clear(); st.confirm.clear(); st.windowIndex = 0; st.shellName.clear(); st.scroll = 0;
    st.elsewhereCount = st.pastCount = -1; pullBadgeAt = pastBadgeAt = 0; pullBadgeOff = pastBadgeOff = false;
    localMode = toLocal; if (to) link = *to;
    if (!headless()) {                                  // the window moves to the other taskbar group, with its icon
      setWindowIdentity(hwnd, inst, !localMode, opt.stateDir);
      if (testRun()) logLine(std::string("taskbar identity: ") + (localMode ? "local" : "remote") + " -> " + narrow(windowIdentity(hwnd)));
    }
    askPane.setLink(localMode ? DeckLink() : link);     // another deck: another origin, another cookie (the web view is made again)
    if (!opt.noSave) { cfg.j["mode"] = localMode ? "local" : "remote"; if (to) cfg.setLink(link); cfg.save(); }
    startTransport();
    updateTitle(); invalidate();
  }
  // the link box: the menu's own inline prompt (35-groups.js promptInline), where the palette was
  void askDeckLink() {
    promptInline(std::max(8.f, viewW / 2 - 95), 72, L"the deck's link: https://\x2026/?t=\x2026", [this](const std::wstring& text) {
      DeckLink l;
      if (!parseDeckLink(text, l)) { note("That is not a deck link: it starts with http and ends with ?t= and the token (Settings \xE2\x86\x92 Phone on the deck's PC shows it).", 5000); return; }
      switchDeck(false, &l);
    });
  }

  // deskInit registers this window. Refused while another desk is connected ("one desk at a time"):
  // retried for a few seconds, because a reload's old socket may still be closing, then said out loud.
  // It says what this machine is called: `deskInit({ name })` (core/PROTOCOL.md; branch native-remote-server shows
  // it on the other screens' driver button as "the remote desk (NAME)"). A deck from before that takes no
  // argument and ignores this one (origin/main lib/phone.js: `m.fn === 'deskInit' ? await this.deskOpen(ws)`).
  std::string thisComputer() const {
    if (!opt.deskName.empty()) return narrow(opt.deskName);
    wchar_t b[MAX_COMPUTERNAME_LENGTH + 1]; DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(b, &n) ? narrow(std::wstring(b, n)) : std::string();
  }
  void deskInit() {
    json who = json::object(); { const std::string n = thisComputer(); if (!n.empty()) who["name"] = n; }
    // … and what kind of remote desk it is: branch native-remote-server's lib/phone.js reads an optional `kind`
    // ('native' | 'browser', lib/presence.js; seen 2026-11-18 late) and calls a desk that says nothing a browser. A deck
    // from before it ignores the word, as it ignores the name.
    who["kind"] = "native";
    json initArgs = json::array(); if (!localMode) initArgs.push_back(who);
    if (testRun() && !localMode) logLine("deskInit " + who.dump());
    call("deskInit", initArgs, [this](bool ok, const json& r) {
      if (ok && !localMode && shell().remoteOwner == this) {
        shell().remoteInit = r;
        for (App* a : remoteWindows()) if (a != this) a->rejoinRemote();
      }
      if (!ok) {
        std::string err = r.is_string() ? r.get<std::string>() : "";
        // (12 tries, 1.2 s apart: a deck with the desk slot's takeover — the native core, branch native-remote-server —
        // frees a DEAD desk's slot once it has been silent 10 s and then failed a 3 s ping; this window's own old
        // socket after a reload goes sooner. The deck's answer may itself come up to 3 s late while it probes:
        // nothing here times a call out, the socket's 60 s silence watchdog is the only limit.)
        if (err.find("one desk at a time") != std::string::npos && deskInitTries++ < 12) { SetTimer(hwnd, TIMER_DESKINIT, 1200, nullptr); return; }
        st.notice = "This deck already has a remote desk open.";
        st.noticeDetail = "Close the other one (a browser tab on /desk/, or another " DECK_EDITION_PRODUCT " remote), then start this again. " + err;
        if (headless()) { failHeadless(st.notice + " " + err); return; }
        invalidate(); return;
      }
      inited = true;
      home = str(r, "home");
      if (r.contains("platform")) platform = r["platform"];
      st.shellName = platform.is_object() ? str(platform, "shellName") : "";
      askPane.setKeyHelp(keyHelp("zoom", "F6"), keyHelp("asks", "F7"));
      st.windowIndex = r.value("windowIndex", 0);
      if (r.contains("grid") && r["grid"].is_object()) { grid.rows = r["grid"].value("rows", 2); grid.cols = r["grid"].value("cols", 2); }
      layout.clear();
      if (localMode) { if (r.contains("layout")) readLayoutMap(r["layout"], false); }
      if (r.contains("meta")) { if (!localMode) meta = r["meta"]; }   // (the tiles, so the remote's own layout below can drop what is gone)
      // REMOTE: this machine's own layout for this deck, not the one the deck answers (persistLayout); a session
      // that no longer exists is dropped
      if (!localMode && primary) { const json own = cfg.j.contains("layouts") && cfg.j["layouts"].is_object() ? cfg.j["layouts"].value(layoutKey(), json()) : json(); readLayoutMap(own, true); if (testRun()) logLine("own layout restored " + (own.is_object() ? own.dump() : std::string("(none)")) + " -> " + std::to_string(layout.size()) + " placed"); }
      if (r.contains("meta")) applyMeta(r["meta"]);
      // 90-boot.js: the tile that was selected last time, else the first
      std::string last = cfg.j.value("active", std::string());
      if (tile(st.selected)) last = st.selected;        // a reconnect: what was selected stays selected
      if (!opt.select.empty()) last = findTile(narrow(opt.select));
      // another window of the deck (R5): a pane it holds, never "the top tile" (70-meta.js: the primary window only)
      if (!primary) { auto ids = L::placedIds(layout); last = ids.empty() ? "" : ids[0]; }
      else if (!tile(last)) { auto ids = tileIds(); last = ids.empty() ? "" : ids[0]; }
      reportWindowState();
      if (!last.empty()) activate(last, false);
      if (opt.place && !last.empty()) placeTile(last, L::fullSet(grid));
      applyLayout();
      // (a reconnect in the middle of a script must not end the run: the script does)
      if (primary && (headless() || !opt.windowShot.empty()) && !scriptStarted) SetTimer(hwnd, TIMER_SHOT, opt.after, nullptr);
      if (primary && autoUpdateHere()) SetTimer(hwnd, TIMER_UPDATE, 30000, nullptr);
    });
  }

  // (a window of several, REMOTE) start from the owner's deskInit answer and its latest meta, as if this window had
  // asked itself: the deck sees one desk, this process shows it in several windows
  void joinRemote(const json& placeIt, const std::string& selected) {
    Shell& s = shell(); App* o = s.remoteOwner; if (!o || !s.remoteInit.is_object()) return;
    const json r = s.remoteInit;
    inited = true; home = str(r, "home");
    if (r.contains("platform")) platform = r["platform"];
    st.shellName = platform.is_object() ? str(platform, "shellName") : "";
    askPane.setKeyHelp(keyHelp("zoom", "F6"), keyHelp("asks", "F7"));
    st.windowIndex = r.value("windowIndex", 0);
    if (r.contains("grid") && r["grid"].is_object()) { grid.rows = r["grid"].value("rows", 2); grid.cols = r["grid"].value("cols", 2); }
    meta = o->meta;
    layout.clear(); readLayoutMap(placeIt, true);
    applyMeta(o->meta);
    std::string last = tile(selected) && isPlaced(selected) ? selected : std::string();
    if (last.empty()) { auto ids = L::placedIds(layout); last = ids.empty() ? "" : ids[0]; }
    if (!last.empty()) activate(last, false);
    applyLayout();
    if (testRun()) logLine("share: window " + std::to_string(windowNo()) + " joined the shared socket, layout " + placeIt.dump());
  }
  void rejoinRemote() { if (keptLayout.is_object()) joinRemote(keptLayout, keptSelected); }

  std::string findTile(const std::string& want) const {
    if (tile(want)) return want;
    if (meta.contains("tiles")) for (const auto& t : meta["tiles"]) { std::string ti = tileTitle(t); if (!want.empty() && ti.compare(0, want.size(), want) == 0) return str(t, "id"); }
    return "";
  }

  // Exactly one screen answers a session's terminal queries (2026-11-19, item 14b: never two - junk in Claude's
  // prompt - and never none - a stall). The C++ deck names it per tile in meta `answerer` (core/deck/remoterule
  // answerer: the office window that shows it, else the remote desk that shows it, else the phone). The Electron
  // deck names none: its window has a terminal for every session and answers them all, so a remote answers nothing.
  //   LOCAL (this is the office window): answers unless the deck names "remote" or "phone".
  //   REMOTE: answers only when the deck names "remote" (the one remote desk; this one shows the tile, or it would
  //   have no pane for it).
  bool answersQueries(const std::string& id) const {
    const json* t = tile(id);
    const std::string a = (t && t->contains("answerer") && (*t)["answerer"].is_string()) ? (*t)["answerer"].get<std::string>() : std::string();
    return localMode ? (a != "remote" && a != "phone") : a == "remote";
  }

  // Bytes of a tile's pty reach its pane (20-terminals.js deck.onData -> term.write), and what the terminal does
  // by itself in answer.
    // What the terminal answers by itself (cursor position, device attributes: term.takeReplies()).
    // REMOTE: dropped, unless the office deck names this remote the answerer (answersQueries: a C++ office whose
    //   window does not show the tile). The Electron office has a terminal for every tile and answers the same
    //   query; a second answer from here would reach the session as typed text (R8: a remote sends what was typed).
    // LOCAL: written back to the pty as input. This window IS the deck's own window (xterm's term.onData ->
    //   deck.input in 20-terminals.js); no other terminal exists to answer, and a program that asks and gets
    //   nothing waits (ConPTY itself asks for the device attributes when a shell starts).
    //   Not for a replay, though: a pane here is re-attached every time it comes back on screen (the desk's
    //   attach / detach, HOST-NOTES.md difference 15) and on every redraw, and answering a query the program
    //   asked long ago types the answer into whatever runs now. The one replay that is answered is the first
    //   screen of a process that has only just started (output it wrote before its pane was attached).
  void feedPane(const std::string& did, const std::string& d, bool replay) {
    auto it = terms.find(did); if (it == terms.end()) return;
    TermView& v = *it->second;
    const bool fresh = startPending.count(did) != 0;   // a process that has just come up and has shown nothing yet
    const bool focusWas = v.term.focusReporting();
    // a drag on the alt screen (DragSpill): the selection as it stood before this output is what a cut copies
    const bool dragging = selecting && selectingId == did && v.drag.active();
    const std::wstring before = dragging ? v.selectionText() : std::wstring();
    v.term.feed(d.data(), d.size());
    if (dragging) {
      const vt::DragSpill::Result r = v.drag.afterFeed(v.term);
      if (r == vt::DragSpill::Moved) {
        v.dragAnchorToSel(); v.repaintAll = true;
        if (testRun()) logLine("drag follows: anchor " + std::string(v.drag.where() < 0 ? "kept above, junction row " : v.drag.where() > 0 ? "kept below, junction row " : "on screen, row ") + std::to_string(v.drag.anchorLine()) + ", " + std::to_string(v.drag.kept()) + " rows kept");
      } else if (r == vt::DragSpill::Cut) {
        v.cutText = before; v.sel = Selection(); v.repaintAll = true;   // no highlight over text that was not selected
        if (testRun()) logLine("drag cut at the redraw: " + std::to_string(before.size()) + " chars as it stood");
      }
    }
    // the selection follows its text when the output moved it (termview.hpp followSelection); not while it is being dragged
    else if (v.sel.active && !(selecting && selectingId == did)) {
      const int moved = v.followSelection();
      if (moved) { v.repaintAll = true; if (testRun()) logLine(std::string("selection ") + (moved == 1 ? "followed its text" : "cleared: its text is gone") + " in " + did); }
    }
    std::string replies = v.term.takeReplies();
    // … and never for a SLEEPING tile (its frozen screen replayed, a late byte): any input wakes a sleeping tile in
    // the core deck, and a terminal's own answer is not somebody asking for the session.
    // WHO answers (14b, 2026-11-23; answersQueries above): LOCAL, unless the deck names a remote desk or the phone;
    // REMOTE, only when the office deck names this remote - a C++ office whose window does not show the tile.
    if (!replies.empty() && inited && (!replay || fresh) && !answersQueries(did) && testRun()) {
      const json* tj = tile(did);
      logLine("reply NOT sent for " + did + ": the deck names " + (tj && tj->contains("answerer") && (*tj)["answerer"].is_string() ? (*tj)["answerer"].get<std::string>() : std::string("no answerer")));
    }
    if (!replies.empty() && inited && (!replay || fresh) && answersQueries(did)) {
      if (tileAsleep(did)) { if (testRun()) logLine("reply NOT sent for " + did + ": the tile is asleep"); }
      else { if (testRun()) logInput(did, replies, "reply"); send(proto::input(did, replies)); }
    }
    // 20-terminals.js `term.onTitleChange(t => deck.setOscTitle(id, t))`. LOCAL only: bridge.js makes it a no-op
    // for a remote desk, whose deck reads the title in its own window. Without it a shell tile has no title.
    if (localMode && inited && !v.term.title().empty() && v.term.title() != v.oscTitle) { v.oscTitle = v.term.title(); fire("setOscTitle", json::array({ did, v.oscTitle })); }
    // xterm: DECSET 1004 answers at once with the focus the terminal has now (InputHandler onRequestSendFocus ->
    // _reportFocus). LOCAL only, and not for a replay (the program that asked may be long gone).
    if (localMode && inited && !replay && !focusWas && v.term.focusReporting()) sendReport(did, paneFocused(did) ? "\x1b[I" : "\x1b[O");
    // Synchronized output (DEC 2026): the pane's picture is held while the application is mid-frame (paintScene),
    // for one second at most (xterm 6 RenderService SynchronizedOutputHandler, SYNCHRONIZED_OUTPUT_TIMEOUT_MS)
    if (v.term.synchronizedOutput()) { if (!v.syncSince) { v.syncSince = GetTickCount64(); if (hwnd) SetTimer(hwnd, 103, 1000 + 20, nullptr); } }
    else v.syncSince = 0;
    // a session that has just come up gets the redraw once its first screen is drawn: 2.5 s after its first
    // output, and 1.5 s after it enters the alternate screen (Claude Code's first frame)
    if (startPending.erase(did)) queuePaneRedraw({ did }, 2500);
    if (altPending.count(did) && v.term.altScreen()) { altPending.erase(did); queuePaneRedraw({ did }, 1500); }
    invalidate();
  }
  bool tileAsleep(const std::string& id) const { const json* t = tile(id); return t && ((t->contains("asleep") && (*t)["asleep"].is_boolean() && (*t)["asleep"].get<bool>()) || str(*t, "status") == "asleep"); }
  // A report the terminal makes by itself in LOCAL mode (mouse, focus): xterm's onData / onBinary -> deck.input.
  // Never through sendInput: it is not a key (the tile stays fresh, the view does not jump to the bottom), and
  // never in remote mode (R8) — the callers check, and this checks again.
  void sendReport(const std::string& id, const std::string& bytes) {
    if (!localMode || !inited || probing() || bytes.empty()) return;
    if (tileAsleep(id)) return;                         // a focus or mouse report is not a key: it must not wake a sleeping tile
    if (testRun()) logInput(id, bytes, "report");
    send(proto::input(id, bytes));
  }
  // the 2026 hold ran out (the end sequence never came): xterm switches the mode off and paints
  void syncTimeout() {
    const ULONGLONG now = GetTickCount64(); ULONGLONG next = 0;
    for (auto& kv : terms) { TermView& v = *kv.second; if (!v.syncSince) continue;
      if (now - v.syncSince >= 1000) { v.term.feed("\x1b[?2026l", 8); v.syncSince = 0; v.repaintAll = true; }
      else { const ULONGLONG left = 1000 - (now - v.syncSince); if (!next || left < next) next = left; } }
    if (next && hwnd) SetTimer(hwnd, 103, (UINT)next + 20, nullptr);
    invalidate();
  }

  void onMessage(const json& m) {
    const std::string t = str(m, "t");
    if (t == "ok" || t == "err") {
      int id = m.value("rid", 0);
      auto it = pending.find(id); if (it == pending.end()) return;
      auto cb = it->second; pending.erase(it);
      if (t == "ok") cb(true, m.contains("result") ? m["result"] : json()); else cb(false, m.contains("error") ? m["error"] : json());
    } else if (t == "data") {
      feedPane(str(m, "id"), str(m, "d"), m.value("replay", false));
    } else if (t == "meta") {
      if (m.contains("meta")) applyMeta(m["meta"]);
    } else if (t == "init") {
      if (probing()) { if (m.contains("meta")) applyMeta(m["meta"]); if (m.contains("platform")) platform = m["platform"]; inited = true; startProbe(); }
    } else if (t == "exit") {
      auto it = terms.find(str(m, "id")); if (it == terms.end()) return;
      std::string s = "\r\n\x1b[90m[process exited with code " + std::to_string(m.value("exitCode", 0)) + "]\x1b[0m\r\n";
      it->second->term.feed(s.data(), s.size()); invalidate();
    } else if (t == "reset") {
      auto it = terms.find(str(m, "id")); if (it == terms.end()) return;
      it->second->term.reset(); it->second->scrollOff = 0; it->second->sel = Selection(); it->second->repaintAll = true; markStarting(str(m, "id")); invalidate();
    } else if (t == "event") {
      const std::string name = str(m, "name");
      const json payload = m.contains("payload") ? m["payload"] : json();
      if (name == "layout:evict" && !localMode) {
        // REMOTE: ignored. What the deck's own windows show takes nothing from a remote (2026-11-18).
      } else if (name == "layout:evict" && payload.is_array()) {
        // another window took these: they leave this layout (not saved back: main already knows)
        bool goneActive = false, any = false;
        for (const auto& id : payload) if (id.is_string() && isPlaced(id.get<std::string>())) { any = true; if (id.get<std::string>() == st.selected) goneActive = true; layout = L::unplace(layout, id.get<std::string>()); }
        leaving.clear();                                   // whatever was marked as leaving has now left
        if (!any) { invalidate(); return; }
        // a tile taken by another window is no longer this window's selection: a pane that remains, or nothing
        if (goneActive) { auto ids = L::placedIds(layout); st.selected = ids.empty() ? "" : ids[0]; }
        if (!st.selected.empty() && !L::isEmpty(layout) && !isPlaced(st.selected)) activate(L::placedIds(layout)[0], true); else applyLayout();
      } else if (name == "grid:changed" && payload.is_object()) {
        // main sends this window's layout remapped onto the new shape; a payload without one means "cleared"
        grid.rows = payload.value("rows", 2); grid.cols = payload.value("cols", 2);
        emptySlot.clear();
        if (localMode) { layout.clear(); if (payload.contains("layout")) readLayoutMap(payload["layout"], true); }
        else {
          // REMOTE: the grid's shape is the deck's; the layout is this machine's own — what still fits the new
          // shape stays where it is, the rest leaves the grid
          L::LayoutMap kept; for (const auto& kv : layout) { L::Cells c = L::norm(kv.second, grid); if (!c.empty() && tile(kv.first)) kept.push_back({ kv.first, c }); }
          layout = kept; persistLayout();
        }
        applyLayout();
      } else if (name == "tiles:activate" && payload.is_string()) {
        if (tile(payload.get<std::string>())) activate(payload.get<std::string>(), true);
      } else if (name == "asks:open" && payload.is_object()) {
        // a new-ask toast was clicked (main.js notifyNewAsks; the core's Deck::toastClicked): that session's
        // tile, then its questions. tileId is null for a session that runs outside the deck.
        askSync(); askPane.onAskOpen(str(payload, "sessionId"), str(payload, "tileId"));
      } else if (name == "layout:place" && payload.is_object()) {
        // main placed a tile in this window (a desk the on-call opened): placed WITHOUT selecting it
        std::string id = str(payload, "id"); L::Cells cells;
        if (payload.contains("cells") && payload["cells"].is_array()) for (const auto& c : payload["cells"]) if (c.is_number()) cells.push_back(c.get<int>());
        if (!id.empty() && !cells.empty()) { ensureTerm(id); layout = L::place(layout, id, cells, grid).layout; applyLayout(); persistLayout(); }
      } else if (name == "layout:leaving" && payload.is_object()) {
        // another window's walk would take a session this window holds: its pane wears the leaving mark
        std::string id = str(payload, "id");
        if (payload.value("on", false) && isPlaced(id)) leaving = L::occupancy(layout, id, grid); else leaving.clear();
        invalidate();
      } else if (name == "input:refused") {
        inputRefused(payload.is_object() ? str(payload, "reason") : std::string());
      } else if (name == "palette:open") togglePalette();
      else if (name == "settings:toggle") toggleSettings();
      else if (name == "sidebar:toggle") setCollapsed(st.sidebarOpen);
      else if (name == "tiles:step" && payload.is_number()) stepTile(payload.get<int>());
      else if (name == "view:redraw") { if (!st.selected.empty()) redrawTile(st.selected); }
      else if (name == "view:zoom") { if (!askPane.asksZoom() && !st.selected.empty()) zoomToggle(st.selected); }
      else if (name == "view:asks") { askSync(); askPane.asksKey(st.selected); }
    }
  }

  // 25-layout.js `deck.onInputRefused` (main.js noteInputRefused): typing was refused because another screen is
  // driving. LOCAL only in practice: the core sends it to a WINDOW (bridge.js gives a desk no such event). Two
  // sets of sentences, word for word: a deck that speaks the claim's words (ui::newDriverWords; branch
  // native-remote-server: reasons `phone` / `remote` = the setting says that screen, `phone-attached` /
  // `remote-showing` = auto and it is looking at this session), and `main`'s (`phone` / anything else).
  void inputRefused(const std::string& reason) {
    std::string t;
    if (ui::newDriverWords(meta)) {
      t = reason == "phone" ? "The phone is driving, so typing here is ignored."
        : reason == "remote" ? "The remote desk is driving this session, so typing here is ignored."
        : reason == "remote-showing" ? "The remote desk is driving this session (it is on screen there), so typing here is ignored. Hide it there, or:"
        : "The phone is driving this session (it is open on the phone), so typing here is ignored. Close it on the phone, or:";
      t += " Click the driver button at the bottom of the sidebar to drive from this PC.";
    } else {
      t = reason == "phone" ? "The phone is driving, so typing here is ignored. Click the green phone button at the bottom of the sidebar to switch to desk."
        : "The phone is driving this session (it is open on the phone), so typing here is ignored. Close it on the phone, or set the driver to desk with the phone button at the bottom of the sidebar.";
    }
    if (testRun()) logLine("input refused (" + reason + "): note '" + t + "'");
    note(t, 6000);
  }
  // the kind of screen this window is, in the claim's words (80-sidebar.js DRIVE_ME): the deck's own window = the
  // office, the exe as a remote desk = remote
  const char* driveMe() const { return localMode ? "office" : "remote"; }

  // ------------------------------------------------------------ meta (renderer/js/70-meta.js applyMeta)
  void applyMeta(const json& m) {
    if (!m.is_object()) return;
    meta = m;
    // (tests) `faketile <field> <json>`: a field of a tile as a deck in another state would send it
    if (!fakeTile.empty() && meta.contains("tiles") && meta["tiles"].is_array()) for (auto& t : meta["tiles"]) { auto f = fakeTile.find(str(t, "id")); if (f != fakeTile.end()) for (auto it = f->second.begin(); it != f->second.end(); ++it) t[it.key()] = it.value(); }
    for (auto it = fakeMeta.begin(); it != fakeMeta.end(); ++it) { const size_t dot = it.key().find('.'); if (dot == std::string::npos) meta[it.key()] = it.value(); else meta[it.key().substr(0, dot)][it.key().substr(dot + 1)] = it.value(); }
    metaGen++;
    askPane.setMeta(meta);
    cloudPanes.setMeta(meta);
    // (No web view is made because a session is asking: on a deck that always has asks waiting that kept a whole
    // browser process tree alive for good. It is made when the pane opens, or when the pointer rests on the asks
    // tile: onMouseMove, TIMER_ASKWARM.)
    { std::string s; for (const char* k : { "asks", "askLog", "settings", "accounts" }) if (meta.contains(k)) s += meta[k].dump(); paneMetaSig = std::hash<std::string>()(s); }
    long long now = m.value("now", (long long)0);
    if (now) st.clockSkew = nowMs() - now;
    bool restoring = m.value("restoring", false);
    std::set<std::string> live; for (const auto& id : tileIds()) live.insert(id);
    // a tile created while the deck runs is a fresh process (20-terminals.js createTerm -> markStarting)
    // (told by `metaSeen`, not by the list being empty: a deck with no tiles at all is the local deck's first run)
    if (metaSeen && !restoring) for (const auto& id : live) if (!knownTiles.count(id)) markStarting(id);
    knownTiles = live; metaSeen = true;
    leftStarting();
    if (!restoring) {
      for (auto it = terms.begin(); it != terms.end();) { if (!live.count(it->first)) { attached.erase(it->first); it = terms.erase(it); } else ++it; }
      // 25-layout.js pruneLayout: closed tiles leave the layout
      L::LayoutMap kept; for (const auto& kv : layout) if (live.count(kv.first)) kept.push_back(kv);
      if (kept.size() != layout.size()) { layout = kept; persistLayout(); }
      // 30-tiles.js renderTiles: a closed tile takes its question, its rename box and its form with it
      for (auto it = st.confirm.begin(); it != st.confirm.end();) { if (!live.count(it->first)) it = st.confirm.erase(it); else ++it; }
      if (!st.renaming.empty() && !live.count(st.renaming)) finishRename(false);
      // … and a drag of it ends, a press on it is forgotten, a walk that started from it has no way back to it
      if (!dragSrc.empty() && !live.count(dragSrc)) { if (hwnd && GetCapture() == hwnd) { releasing = true; ReleaseCapture(); releasing = false; } dragSrc.clear(); st.dragging.clear(); st.dropMark.clear(); previewSet.clear(); underPreview.clear(); growingId.clear(); leaving.clear(); press.on = false; }
      if (press.on && !press.hit.id.empty() && inTile(press.hit.kind) && !live.count(press.hit.id)) { press.on = false; st.pressed.clear(); }
      if (stepSnap.on && !stepSnap.active.empty() && !live.count(stepSnap.active)) stepSnap.active.clear();
      if (!nameClickId.empty() && !live.count(nameClickId)) nameClickId.clear();
      if (!hold.tile.empty() && !live.count(hold.tile)) closeHoldForm();
    }
    // 70-meta.js: `if (!state.active || !ids.has(state.active))` — nothing selected, or the selected tile is gone: a
    // pane that remains, else (the primary window only) the top tile. Restoring or not: a tile the staggered
    // restore brings back after the window opened is selected the same way.
    if (inited && (st.selected.empty() || !live.count(st.selected))) {
      st.selected.clear();
      auto placed = L::placedIds(layout); std::string first;
      for (const auto& id : placed) if (live.count(id)) { first = id; break; }
      if (first.empty() && L::isEmpty(layout) && st.windowIndex == 0) { auto ids = tileIds(); if (!ids.empty()) first = ids[0]; }
      if (!first.empty()) { st.selected = first; st.focusInTerminal = st.focusInTerminal && !st.focusEdit; }
    }
    panelMeta();
    json g = setting("grid");
    if (g.is_object() && inited) { int r = g.value("rows", grid.rows), c = g.value("cols", grid.cols); if (r != grid.rows || c != grid.cols) { grid.rows = r; grid.cols = c; } }
    std::wstring fam = widen(setting("fontFamily").is_string() ? setting("fontFamily").get<std::string>() : std::string());
    std::wstring resolved = resolveMonoFamily(gfx, fam);
    if (resolved != monoFamily) { monoFamily = resolved; metricsBySize.clear(); }
    applyLayout();
    driverPush();
    updateTitle();
    adoptPendingNew();                                  // a tile just asked for (newTile answered before this meta listed it)
  }

  // LOCAL: a session that has just come up may have had its pty narrowed behind this window's back — the deck's
  // trust-prompt answerer nudges it 119 / 118 / 117 columns to force a repaint and leaves it at 117 (HOST-NOTES.md
  // edit 6). fitVisible only says a size that differs from the last one IT said, so the pane would stay 120 wide
  // over a 117-column program. When a tile this window shows and sizes leaves `starting`, the pane's size is said
  // again (the Electron renderer's redraw on a new session's first screen does the same: 20-terminals.js
  // markStarting / queuePaneRedraw).
  std::map<std::string, std::string> statusSeen;
  void leftStarting() {
    std::map<std::string, std::string> now;
    if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"]) if (t.is_object()) now[str(t, "id")] = str(t, "status");
    if (localMode && inited && !probing()) for (const auto& kv : now) {
      auto was = statusSeen.find(kv.first);
      if (was == statusSeen.end() || was->second != "starting" || kv.second == "starting") continue;
      auto it = terms.find(kv.first); int mc, mr;
      if (it == terms.end() || !st.shown.count(kv.first) || mirrorOf(kv.first, mc, mr)) continue;
      const int c = it->second->term.cols(), r = it->second->term.rows(); if (!(c > 1 && r > 1)) continue;
      sentSize[kv.first] = { c, r };
      if (testRun()) logLine("resize " + kv.first + " " + std::to_string(c) + "x" + std::to_string(r) + " (left starting)");
      send(proto::resize(kv.first, c, r));
    }
    statusSeen = now;
  }

  // ---- becoming the driver (core/PROTOCOL.md "A client that becomes the driver must say its size"; the branch's
  // 70-meta.js applyMeta / reassertDriven). Only with a deck that speaks the claim's words (ui::newDriverWords);
  // with a deck from before it nothing here sends anything, as before.
  //  * a shown tile whose `mirror` went from set to null is one this screen now drives: its size is said even when
  //    the pane did not change (fitVisible only sends a size that differs from the last one IT sent, and the pty
  //    is still at the other screen's size);
  //  * a shown tile the deck says this kind of screen sizes (`sizeBy: remote`, no mirror) whose `ptySize` is not
  //    what the pane draws is told again, at most once per tile every 2.5 s.
  std::set<std::string> mirroredIds;                    // the tiles the last meta told this screen to mirror
  std::map<std::string, long long> reassertAt;          // tile -> when its size was last re-said
  std::map<std::string, unsigned long long> resizeGen;  // tile -> the metaGen in which fitVisible said its size
  void driverPush() {
    std::set<std::string> now; int mc, mr;
    for (const auto& id : tileIds()) if (mirrorOf(id, mc, mr)) now.insert(id);
    std::vector<std::string> taken; for (const auto& id : mirroredIds) if (!now.count(id)) taken.push_back(id);
    mirroredIds = now;
    if (!inited || probing() || !ui::newDriverWords(meta)) return;
    if (minimised()) return;                            // hidden: this screen shows nothing, so it drives nothing
    // LOCAL (the office): 70-meta.js says its size only as term.onResize does — `oneWindow() || document.hasFocus()`:
    // with several windows showing one session, the focused one sizes it
    if (localMode && !(oneWindow() || windowFocused)) return;
    const long long t = nowMs();
    auto say = [&](const std::string& id, const std::string& why) {
      auto it = terms.find(id); if (it == terms.end()) return;
      const int c = it->second->term.cols(), r = it->second->term.rows(); if (!(c > 1 && r > 1)) return;
      reassertAt[id] = t; sentSize[id] = { c, r };
      if (testRun()) logLine("resize " + id + " " + std::to_string(c) + "x" + std::to_string(r) + " (" + why + ")");
      send(proto::resize(id, c, r));
    };
    for (const auto& id : taken) if (st.shown.count(id) && tile(id) && !(resizeGen.count(id) && resizeGen[id] == metaGen)) say(id, "became the driver");
    for (const auto& id : st.shown) {
      const json* tl = tile(id); if (!tl || now.count(id) || str(*tl, "sizeBy") != driveMe()) continue;
      auto ps = tl->find("ptySize"); auto it = terms.find(id); if (ps == tl->end() || !ps->is_object() || it == terms.end()) continue;
      if (ps->value("cols", 0) == it->second->term.cols() && ps->value("rows", 0) == it->second->term.rows()) continue;
      auto at = reassertAt.find(id); if (at != reassertAt.end() && t - at->second < 2500) continue;
      say(id, "reassert: the pty is " + std::to_string(ps->value("cols", 0)) + "x" + std::to_string(ps->value("rows", 0)));
      if (!localMode) queueQuietRedraw(id, 900);         // what was drawn at the other size goes once it is back at ours
    }
  }

  // ---- the bundled console fonts (assets/fonts: the Electron renderer loads them by @font-face). This exe is one
  // static file and carries none of them: they are read from their FILES when a folder of them is found —
  // --fonts-dir, config.json `fontsDir`, `assets\\fonts` beside the exe, or the checkout's own when the exe sits in
  // windows\\build — into a private DirectWrite collection (gfx.hpp loadFontFolder). Not found: only installed fonts
  // work, the stack falls through to Cascadia Mono as the css stack does, and Settings says so.
  void loadBundledFonts() {
    if (opt.fontsDir == L"none") return;
    std::vector<std::wstring> tries;
    if (!opt.fontsDir.empty()) tries.push_back(opt.fontsDir);
    else {
      const std::string c = cfg.j.value("fontsDir", std::string()); if (!c.empty()) tries.push_back(widen(c));
      wchar_t exe[MAX_PATH * 2] = L""; GetModuleFileNameW(nullptr, exe, MAX_PATH * 2 - 1);
      std::wstring dir = exe; const size_t sl = dir.find_last_of(L"\\/"); if (sl != std::wstring::npos) dir = dir.substr(0, sl);
      tries.push_back(dir + L"\\assets\\fonts"); tries.push_back(dir + L"\\fonts");
      if (!testRun()) tries.push_back(dir + L"\\..\\..\\assets\\fonts");     // windows\build inside a checkout (a test says its folder itself)
    }
    // (no folder named outright: the fonts inside the exe, windows/res/fonts.rc - the folders below are the fallback)
    if (opt.fontsDir.empty() && cfg.j.value("fontsDir", std::string()).empty()) {
      const int n = gfx.loadFontResources(GetModuleHandleW(nullptr));
      if (n) { if (testRun()) { std::string fams; for (const auto& f : gfx.privateFamilies) fams += (fams.empty() ? "" : ", ") + narrow(f); fontsNote = "bundled fonts: " + std::to_string(n) + " families from inside the exe: " + fams; } return; }
    }
    for (const auto& t : tries) {
      const DWORD a = GetFileAttributesW(t.c_str()); if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
      const int n = gfx.loadFontFolder(t);
      if (n) { if (testRun()) { std::string fams; for (const auto& f : gfx.privateFamilies) fams += (fams.empty() ? "" : ", ") + narrow(f); fontsNote = "bundled fonts: " + std::to_string(n) + " families from " + narrow(t) + ": " + fams; } return; }
    }
    if (testRun()) fontsNote = "bundled fonts: none loaded (no folder of them found)";
  }
  std::string fontsNote;
  // 20-terminals.js rememberFont: the fonts used before, on this machine (localStorage deck.fonthistory), newest first, twelve at most
  std::vector<std::string> fontHistory() const {
    std::vector<std::string> out;
    if (cfg.j.contains("fonthistory") && cfg.j["fonthistory"].is_array()) for (const auto& x : cfg.j["fonthistory"]) if (x.is_string() && !narrow(trim(widen(x.get<std::string>()))).empty()) out.push_back(x.get<std::string>());
    return out;
  }
  void rememberFont(const std::string& name) {
    const std::string n = narrow(trim(widen(name))); if (n.empty()) return;
    std::vector<std::string> h = { n };
    for (const auto& x : fontHistory()) if (lowerA(x) != lowerA(n) && h.size() < 12) h.push_back(x);
    cfg.j["fonthistory"] = h;
    if (!opt.noSave) cfg.save();
  }

  // ------------------------------------------------------------ panes (renderer/js/25-layout.js)
  bool isPlaced(const std::string& id) const { for (const auto& kv : layout) if (kv.first == id) return true; return false; }
  std::vector<std::string> visibleIds() const {
    if (!L::isEmpty(layout)) return L::placedIds(layout);
    if (st.selected.empty()) return {};
    if (heldElsewhere(st.selected) && oneWindow()) return {};
    return { st.selected };
  }
  // The sidebar's width on device pixels (a whole number of DIPs is one already at 100 % and 200 %): the line
  // between the sidebar and the terminal area is never a half-covered pixel, so each side repaints by itself.
  // css #sidebar: width var(--sb-width), min-width var(--sb-min), max-width 70vw (the minimum wins when they disagree):
  // a window made narrower than the remembered width still has a terminal area.
  float sidebarW() const { return gfx.snap(st.sidebarOpen ? std::max(sbMin(), std::min(st.sidebarWidth, viewW * 0.7f)) : 14); }
  // --sb-min: the sidebar's narrowest, the widest tile strip at its natural size (ui::Sidebar::minWidth; 160 at least;
  // a fixed 220 until 2026-11-23), measured again only when the accounts' ids / labels or the scale change
  mutable unsigned sbMinGen = ~0u; mutable float sbMinScale = 0, sbMinV = 160; mutable std::string sbMinKey;
  float sbMin() const {
    if (sbMinGen == metaGen && sbMinScale == gfx.scale) return sbMinV;
    std::string k; const json& as = meta.is_object() && meta.contains("accounts") ? meta["accounts"] : json();
    if (as.is_array()) for (const auto& a : as) { k += str(a, "id"); k += '\x1'; k += str(a, "label"); k += '\x2'; }
    if (k != sbMinKey || sbMinScale != gfx.scale) {
      const float v = ui::Sidebar::minWidth(const_cast<Gfx&>(gfx), meta, st);
      if (v <= 0) return sbMinV;                        // no target yet: asked again next time
      sbMinV = v; sbMinKey = k;
    }
    sbMinGen = metaGen; sbMinScale = gfx.scale;
    return sbMinV;
  }
  RectF mainRect() const {
    float sb = sidebarW();
    return sidebarRight() ? RectF(0, 0, viewW - sb, viewH) : RectF(sb, 0, viewW - sb, viewH);
  }
  RectF sidebarRect() const {
    float sb = sidebarW();
    return sidebarRight() ? RectF(viewW - sb, 0, sb, viewH) : RectF(0, 0, sb, viewH);
  }
  // where a tile's pane is, or false when it shows nowhere (unplaced, or hidden behind a zoom)
  bool paneRect(const std::string& id, RectF& out) const {
    RectF mr = mainRect();
    if (L::isEmpty(layout)) { if (id != st.selected || (heldElsewhere(id) && oneWindow())) return false; out = mr; return true; }
    L::Cells set = L::occupancy(layout, id, grid); if (set.empty()) return false;
    if (!zoom.empty()) { if (zoom != id) return false; out = mr; return true; }
    L::Rect r = L::rect(set, grid);
    // on device pixels (whole DIPs at 100 % and 200 %, as before): a pane's edge is never a half-covered pixel, and
    // the pane can be repainted alone under its own rectangle (the scene)
    float x0 = gfx.snap(mr.x + (float)r.x * mr.w), y0 = gfx.snap(mr.y + (float)r.y * mr.h);
    float x1 = gfx.snap(mr.x + (float)(r.x + r.w) * mr.w), y1 = gfx.snap(mr.y + (float)(r.y + r.h) * mr.h);
    out = RectF(x0, y0, x1 - x0, y1 - y0);
    return true;
  }
  TermView* ensureTerm(const std::string& id) {
    auto it = terms.find(id); if (it != terms.end()) return it->second.get();
    auto v = std::make_unique<TermView>();
    // a view nobody has fitted yet starts at the shown terminal's size, so its first replay is not re-wrapped
    auto sel = terms.find(st.selected);
    if (sel != terms.end() && sel->second->term.cols() > 1) v->term.resize(sel->second->term.cols(), sel->second->term.rows());
    TermView* p = v.get(); terms[id] = std::move(v); return p;
  }
  const CellMetrics& metrics(float size) {
    int key = (int)std::round(size * 4);
    auto it = metricsBySize.find(key); if (it != metricsBySize.end()) return it->second;
    return metricsBySize[key] = measureCells(gfx, monoFamily, size, 1.1f);
  }
  float paneNameSize() const { return std::max(8.f, baseFontSize() - 1); }

  // applyLayout: what shows, told to the deck (hibernation's on-screen set), attached, fitted
  void applyLayout() {
    if (!zoom.empty() && (L::isEmpty(layout) || !isPlaced(zoom))) zoom.clear();
    { json o = json::object(); for (const auto& kv : layout) o[kv.first] = kv.second; if (!meta.contains("layout") || meta["layout"] != o) { meta["layout"] = o; metaGen++; } }   // the tiles' occupancy glyph (sidebar.cpp)
    // A pane whose rectangle CHANGED gets the F5 redraw on its own, 400 ms out (past the deck's resize debounce);
    // never for the first paint, never while the deck is still restoring (25-layout.js paneRectKey)
    {
      const bool empty = L::isEmpty(layout); std::vector<std::string> changed;
      for (const auto& id : tileIds()) {
        std::string key;
        const L::Cells set = empty ? L::Cells() : L::occupancy(layout, id, grid);
        if (!set.empty()) { if (zoom == id) key = "0,0,1,1"; else { L::Rect r = L::rect(set, grid); char b[96]; snprintf(b, sizeof b, "%g,%g,%g,%g", r.x, r.y, r.w, r.h); key = b; } }
        auto it = paneRectKey.find(id);
        if (!key.empty() && it != paneRectKey.end() && it->second != key && inited && !meta.value("restoring", false)) changed.push_back(id);
        paneRectKey[id] = key;
      }
      if (!changed.empty()) queuePaneRedraw(changed, 400);
    }
    st.shown.clear();
    std::vector<std::string> vis = visibleIds();
    for (const auto& id : vis) if (tile(id)) { ensureTerm(id); st.shown.insert(id); }
    syncShown();
    fitVisible();
    invalidate();
  }
  std::map<std::string, std::string> paneRectKey;
  std::set<std::string> paneRedrawQueue, redrawOwed;    // owed: fell due while the pane was hidden behind a zoom
  std::set<std::string> startPending, altPending, knownTiles;   // 20-terminals.js markStarting: a session that has just come up
  int paneRedrawDelay = 0;
  // A QUIET redraw after a size change (2026-11-19, the remote: Claude Code's status line drew garbled until a
  // refresh). For a moment the pty was another size than this pane - the office or the phone sized it and this
  // remote re-asserted its own, or this pane followed the pty to a new size - and what Claude Code drew in that
  // moment stays in the terminal at the wrong width (it redraws only its live rows). Once the size has settled the
  // pane takes the replay of what was written since the pty last changed size (attach `since`), which is that
  // redraw at the right width. No flash, no nudge: nobody asked for it, it only puts the picture right.
  std::set<std::string> quietRedrawQueue;
  void queueQuietRedraw(const std::string& id, int delay) {
    if (!inited || probing() || !terms.count(id)) return;
    quietRedrawQueue.insert(id);
    if (hwnd) SetTimer(hwnd, TIMER_QUIETREDRAW, (UINT)delay, nullptr);
  }
  void runQuietRedraws() {
    std::set<std::string> due; due.swap(quietRedrawQueue);
    for (const auto& id : due) {
      auto it = terms.find(id); if (it == terms.end() || !st.shown.count(id) || !attached.count(id) || !inited) continue;
      TermView& v = *it->second;
      if (v.sel.active && selecting && selectingId == id) { quietRedrawQueue.insert(id); continue; }   // not under a drag
      v.term.reset(); v.scrollOff = 0; v.sel = Selection(); v.repaintAll = true;
      if (testRun()) logLine("quiet redraw " + id + " (the size changed: the replay since it)");
      send(proto::attach(id, true));
    }
    if (!quietRedrawQueue.empty() && hwnd) SetTimer(hwnd, TIMER_QUIETREDRAW, 300, nullptr);
    invalidate();
  }
  void queuePaneRedraw(const std::vector<std::string>& ids, int delay) {
    for (const auto& id : ids) paneRedrawQueue.insert(id);
    paneRedrawDelay = std::max(paneRedrawDelay, delay);
    if (hwnd) SetTimer(hwnd, 102, paneRedrawDelay, nullptr);
  }
  void runPaneRedraws() {
    std::set<std::string> due; due.swap(paneRedrawQueue); paneRedrawDelay = 0;
    std::vector<std::string> vis = visibleIds();
    for (const auto& id : due) {
      if (!terms.count(id) || std::find(vis.begin(), vis.end(), id) == vis.end()) continue;
      if (!zoom.empty() && id != zoom && isPlaced(id)) redrawOwed.insert(id); else redrawTile(id);
    }
  }
  void markStarting(const std::string& id) { startPending.insert(id); altPending.insert(id); }
  void syncShown() {
    if (!inited || probing()) return;
    std::vector<std::string> ids(st.shown.begin(), st.shown.end());
    bool hidden = hwnd && IsIconic(hwnd);
    std::string key = json(ids).dump() + (hidden ? "h" : "v");
    if (key != shownKey) { shownKey = key; send(proto::shown(ids, hidden)); }
    for (const auto& id : st.shown) if (!attached.count(id)) attachNow(id, false);
    for (auto it = attached.begin(); it != attached.end();) { if (!st.shown.count(*it)) { send(proto::detach(*it)); auto t = terms.find(*it); if (t != terms.end()) t->second->attached = false; it = attached.erase(it); } else ++it; }
  }
  void attachNow(const std::string& id, bool since) {
    TermView* v = ensureTerm(id);
    // a pane coming back on screen holds whatever it showed last: reset, and the replay fills it
    if (v->everAttached && !v->attached) { v->term.reset(); v->scrollOff = 0; v->sel = Selection(); v->drag.clear(); v->cutText.clear(); v->repaintAll = true; }
    v->attached = v->everAttached = true; attached.insert(id);
    send(proto::attach(id, since));
  }
  // REMOTE: the deck says ANOTHER kind of screen sizes this tile (`sizeBy` set and not this remote's word), so the pty
  // is at that screen's size, `ptySize` - whether or not a `mirror` came with it. lib/phone.js deskMeta sends a remote
  // its `mirror` only for a tile the deck counts as shown there; a tile just brought on screen (the deck has not had
  // the new `shown` yet) arrives with sizeBy 'office' and no mirror. Fitted to its own pane over a narrower pty, the
  // part of a line the program let wrap lands far to the right of the row (2026-11-23, the home remote, a long
  // code block). So: drawn at the pty's size, and no size sent (the office session's word, 2026-11-24). A deck that
  // says neither word (no sizeBy / no ptySize) is fitted as before.
  bool sizedElsewhere(const std::string& id, int& cols, int& rows) const {
    if (localMode) return false;
    const json* t = tile(id); if (!t) return false;
    auto sb = t->find("sizeBy"); if (sb == t->end() || !sb->is_string() || sb->get<std::string>() == driveMe()) return false;
    auto ps = t->find("ptySize"); if (ps == t->end() || !ps->is_object()) return false;
    cols = ps->value("cols", 0); rows = ps->value("rows", 0);
    return cols > 1 && rows > 1;
  }
  // the largest font size, at most `base` (in half pixels, 6 at the least), at which cols x rows fit the pane's text box
  float fontToShow(const RectF& pr, int cols, int rows, float base) {
    const RectF box = TermView::contentBox(pr, paneNameSize());
    for (float px = base; px > 6.f; px -= 0.5f) { int c, r; TermView::fit(box, metrics(px), c, r); if (c >= cols && r >= rows) return px; }
    return 6.f;
  }
  // 20-terminals.js fitOne / fitActive
  void fitVisible() {
    for (const auto& id : st.shown) {
      RectF pr; if (!paneRect(id, pr)) continue;
      TermView* v = ensureTerm(id);
      int mc, mr;
      if (mirrorOf(id, mc, mr)) {                         // somebody else owns the size: match it exactly
        float base = baseFontSize();
        float fit = pr.h > 0 ? std::floor(pr.h / (mr * 1.1f)) : base;
        v->fontSize = std::max(6.f, std::min(base, fit > 0 ? fit : base));
        if (v->term.cols() != mc || v->term.rows() != mr) { v->term.resize(mc, mr); if (!localMode) queueQuietRedraw(id, 600); }
        continue;
      }
      if (sizedElsewhere(id, mc, mr)) {                   // REMOTE, another kind of screen drives and no mirror came: the pty's own size
        v->fontSize = fontToShow(pr, mc, mr, baseFontSize());
        if (v->term.cols() != mc || v->term.rows() != mr) {
          if (testRun()) logLine("pty size " + id + " " + std::to_string(mc) + "x" + std::to_string(mr) + " (sized by " + str(*tile(id), "sizeBy") + "; drawn at the pty's size, none sent)");
          v->term.resize(mc, mr); queueQuietRedraw(id, 600);
        }
        continue;
      }
      json zf = setting("zoomFontSize");
      float px = (zoom == id && zf.is_number() && zf.get<float>() > 0) ? zf.get<float>() : baseFontSize();
      v->fontSize = px;
      int cols, rows;
      TermView::fit(TermView::contentBox(pr, paneNameSize()), metrics(px), cols, rows);
      if (v->term.cols() != cols || v->term.rows() != rows) v->term.resize(cols, rows);
      // only a visible pane this window drives pushes its size (term.onResize in 20-terminals.js)
      auto& last = sentSize[id];
      if (inited && !probing() && (last.first != cols || last.second != rows)) { last = { cols, rows }; resizeGen[id] = metaGen; reassertAt[id] = nowMs(); if (testRun()) logLine("resize " + id + " " + std::to_string(cols) + "x" + std::to_string(rows)); send(proto::resize(id, cols, rows)); }
    }
  }
  // LOCAL: the layout is told to the deck (25-layout.js saveLayout): it keeps one home per tile across its windows.
  // REMOTE (decided 2026-11-18): a remote keeps its layout to ITSELF. No `layout` frame goes to the deck —
  // the deck's takeLayout would take the session out of the pane it has in the office window ("a session shows
  // in one window at a time" is a rule of one box) — and no `leaving` either (App::send drops both, whoever asks).
  // It is kept on THIS machine instead: config.json `layouts[<host:port>] = { tileId: [cells] }`, restored after
  // deskInit. A deliberate difference from the browser desk (PARITY.md).
  std::string layoutKey() const { return narrow(link.host) + ":" + std::to_string(link.port); }
  void persistLayout() {
    if (!inited || probing() || meta.value("restoring", false) || stepSnap.on) return;
    if (localMode) { send(proto::layout(layout)); return; }
    if (!primary) return;                               // (a remote's other windows: their layouts are not kept)
    json o = json::object(); for (const auto& kv : layout) o[kv.first] = kv.second;
    if (testRun()) logLine("own layout kept (not sent) " + o.dump());
    if (opt.noSave || opt.bench.empty() == false) return;
    if (!cfg.j.contains("layouts") || !cfg.j["layouts"].is_object()) cfg.j["layouts"] = json::object();
    if (cfg.j["layouts"].value(layoutKey(), json()) == o) return;
    cfg.j["layouts"][layoutKey()] = o; cfg.save();
  }
  void readLayoutMap(const json& o, bool onlyLive) {
    layout.clear();
    if (o.is_object()) for (auto it = o.begin(); it != o.end(); ++it) {
      L::Cells cells; if (it.value().is_array()) for (const auto& c : it.value()) if (c.is_number()) cells.push_back(c.get<int>());
      cells = L::norm(cells, grid);
      if (!cells.empty() && (!onlyLive || tile(it.key()))) layout.push_back({ it.key(), cells });
    }
  }

  // 25-layout.js paneNote: one short note at the top of the terminal area, for "you did something and the deck
  // did not do it". A newer note replaces the one showing.
  bool zoomBlocks() { if (zoom.empty()) return false; note("Zoomed \xE2\x80\x94 double-click the pane name or press F6 to return to the grid before changing it."); return true; }
  void note(const std::string& text, int ms = 2500) { paneNoteText = text; paneNoteUntil = GetTickCount64() + ms; invalidate(); }
  // Copy on select, REMOTE mode (2026-11-19: "it looks like I would have to ctrl+C, it doesn't behave like the
  // normal terminal"): over a full-screen Claude Code session their own terminal lets the application select and copy
  // on release; a remote sends no mouse reports (R8), so the selection is ours - and it is copied the moment it is
  // made. On by default; config.json `copyOnSelect` (the palette toggles it). Ctrl+C and right-click are unchanged.
  // The local deck keeps the Electron deck's behaviour (select, then Ctrl+C or right-click).
  bool copyOnSelectOn() const { return !localMode && cfg.j.value("copyOnSelect", true); }
  // The end of a drag on the alt screen (the release, or the system taking the mouse away: `copy` false). True = it
  // was one of DragSpill's and is handled here: it kept rows (its text is held: termview.hpp heldText) or it was cut
  // (what was selected before the redraw is copied, and the note says so). False = the usual release.
  bool finishDrag(TermView& v, bool copy) {
    if (v.drag.cut()) {
      const std::wstring t = v.cutText; v.drag.clear(); v.cutText.clear();
      if (copy && !t.empty()) { setClipboard(t); note("Copied up to the redraw: the selection was cut there", 3000); }
      else note("Selection cut at the redraw", 3000);
      if (testRun()) logLine("drag ended cut: " + std::string(copy && !t.empty() ? "copied " + std::to_string(t.size()) + " chars" : "nothing copied"));
      return true;
    }
    if (!v.drag.active() || v.drag.where() == 0) { v.drag.clear(); return false; }
    const size_t kept = v.drag.kept();
    v.heldText = v.selectionText(); v.drag.clear(); v.heldSel = v.sel;   // (the text while the drag still knows its rows)
    if (v.sel.empty()) v.heldSel.active = v.sel.active = true;          // (the head on the junction: the kept rows are still the selection)
    v.snapSelection();
    if (testRun()) logLine("drag ended past the screen: " + std::to_string(kept) + " rows kept, " + std::to_string(v.heldText.size()) + " chars held");
    if (copy) copyOnSelect(v);
    return true;
  }
  void copyOnSelect(TermView& v) {
    if (!copyOnSelectOn() || !v.sel.active) return;
    const std::wstring s = v.selectionText();
    if (s.empty()) return;
    setClipboard(s);
    note("Copied", 900);
    if (testRun()) logLine("copy on select: " + std::to_string(s.size()) + " chars");
  }
  std::string paneNoteText; ULONGLONG paneNoteUntil = 0;
  ULONGLONG noticeUntil = 0;

  // ---------------------------------------------------------------- stepping is a PREVIEW (25-layout.js)
  // F11 / F12 (and Up / Down) walk the sessions; the selected pane shows each one as you pass, but nothing is
  // committed until something draws focus elsewhere: a press, a key that is not a step, the window losing
  // focus. Stepping again first restores the layout the walk started from. Escape puts everything back.
  struct StepSnap { bool on = false; L::LayoutMap layout; std::string active; L::Cells emptySlot; } stepSnap;
  std::string stepLeavingElsewhere;   // a tile another window holds, shown here by the walk: that window marks its pane
  void stepPreviewBegin() {
    if (!stepSnap.on) { stepSnap.on = true; stepSnap.layout = layout; stepSnap.active = st.selected; stepSnap.emptySlot = emptySlot; return; }
    layout = stepSnap.layout; emptySlot = stepSnap.emptySlot;
  }
  void stepPreviewMark() {
    if (!stepLeavingElsewhere.empty() && stepLeavingElsewhere != st.selected) { send(proto::leaving(stepLeavingElsewhere, false)); stepLeavingElsewhere.clear(); }
    if (!stepSnap.on || st.selected.empty()) { leaving.clear(); invalidate(); return; }
    const L::Cells was = L::occupancy(stepSnap.layout, st.selected, grid), now = L::occupancy(layout, st.selected, grid);
    // it left a pane of this window for another one: mark the pane it is leaving
    if (!was.empty() && !now.empty() && !L::same(was, now, grid)) leaving = was; else leaving.clear();
    // it is held in another window and one window per session: that window's pane is the one it would leave
    if (heldElsewhere(st.selected) && oneWindow() && stepLeavingElsewhere != st.selected) { stepLeavingElsewhere = st.selected; send(proto::leaving(st.selected, true)); }
    invalidate();
  }
  void stepPreviewEnd() {
    stepSnap = StepSnap(); leaving.clear();
    if (!stepLeavingElsewhere.empty()) { send(proto::leaving(stepLeavingElsewhere, false)); stepLeavingElsewhere.clear(); }
    invalidate();
  }
  void stepPreviewCommit() { if (!stepSnap.on) return; stepPreviewEnd(); persistLayout(); }
  bool stepPreviewCancel() {
    if (!stepSnap.on) return false;
    StepSnap s = stepSnap; stepPreviewEnd();
    layout = s.layout; emptySlot = s.emptySlot;
    activate(s.active, false);
    return true;
  }

  // activate (20-terminals.js): select; adopt = a selection may place the tile (double-click, F11/F12)
  void activate(const std::string& id, bool adopt) {
    std::string prev = st.selected;
    st.selected = id;
    if (!opt.noSave) cfg.j["active"] = id;
    if (!zoom.empty() && !id.empty() && id != zoom && isPlaced(id)) zoom = id;
    if (!id.empty() && id != prev && adopt) layoutAdopt(id, prev);
    else if (!id.empty() && isPlaced(id)) emptySlot.clear();
    askPane.asksBlur(); st.asksFocus = false;           // 20-terminals.js activate: a session selected, the asks tile is no longer the selection
    if (!id.empty() && st.focusEdit) focusEdit(nullptr);   // the terminal takes the focus (r.term.focus()): an open text box loses it
    st.focusInTerminal = !id.empty();
    applyLayout();
    // push the size on activate even when the view did not change size: a phone may have resized the pty
    auto it = terms.find(id); int mc, mr;
    if (it != terms.end() && inited && !probing() && st.shown.count(id) && !mirrorOf(id, mc, mr)) { if (testRun()) logLine("resize " + id + " " + std::to_string(it->second->term.cols()) + "x" + std::to_string(it->second->term.rows()) + " (activate)"); send(proto::resize(id, it->second->term.cols(), it->second->term.rows())); }
    // 20-terminals.js activate: `if (!focusCloud(id)) r.term.focus()` — a cloud pane's page takes the keys, any other
    // selection takes them back from a page that had them
    if (hwnd && !headless()) {
      if (cloudPanes.isCloud(id)) { cloudWantFocus = id; cloudSync(); }
      else { cloudWantFocus.clear(); if (cloudPanes.webFocused()) SetFocus(hwnd); }
    }
    scrollTileIntoView(id);
    updateTitle();
  }
  void layoutAdopt(const std::string& id, const std::string& prev) {
    if (isPlaced(id) || !tile(id)) return;
    if (zoomBlocks()) return;
    if (!emptySlot.empty() && !L::isEmpty(layout)) { L::Cells slot = emptySlot; emptySlot.clear(); layout = L::place(layout, id, slot, grid).layout; persistLayout(); return; }
    if (L::isEmpty(layout)) {
      if (heldElsewhere(id) && oneWindow()) { layout = L::place({}, id, L::fullSet(grid), grid).layout; persistLayout(); }
      return;
    }
    L::Cells slot = L::occupancy(layout, prev, grid);
    if (slot.empty()) { auto ids = L::placedIds(layout); if (!ids.empty()) slot = L::occupancy(layout, ids[0], grid); }
    if (slot.empty()) return;
    layout = L::place(L::unplace(layout, prev), id, slot, grid).layout;
    persistLayout();
  }
  void placeTile(const std::string& id, const L::Cells& set) {
    if (id.empty() || !tile(id)) return;
    if (zoomBlocks()) return;
    layout = L::place(layout, id, L::legalize(L::norm(set, grid), grid), grid).layout;
    emptySlot.clear();
    activate(id, true);
    persistLayout();
  }
  void unplaceTile(const std::string& id) {
    if (!isPlaced(id) || zoomBlocks()) return;
    layout = L::unplace(layout, id);
    if (st.selected == id && !L::isEmpty(layout)) activate(L::placedIds(layout)[0], true); else applyLayout();
    persistLayout();
  }
  // a NEW session never takes the selected pane: the first empty region, else selected only
  void placeNew(const std::string& id) {
    if (L::isEmpty(layout)) { activate(id, true); return; }
    std::set<int> taken; for (const auto& kv : layout) for (int k : kv.second) taken.insert(k);
    L::Cells free; for (int k : L::fullSet(grid)) if (!taken.count(k)) free.push_back(k);
    if (free.empty()) { activate(id, false); return; }
    // the first empty region in reading order: all the free cells when they form a rectangle; else the largest
    // rectangle of free cells that starts at the first one (widen along the row, then deepen)
    L::Cells region = free;
    if (!L::isLegal(free, grid)) {
      const L::Dims d = L::dims(grid);
      const int r0 = (free[0] - 1) / d.C, c0 = (free[0] - 1) % d.C;
      auto isFree = [&](int r, int c) { return r < d.R && c < d.C && !taken.count(r * d.C + c + 1); };
      int w = 1; while (isFree(r0, c0 + w)) w++;
      int h = 1; for (;;) { bool all = true; for (int i = 0; i < w; i++) if (!isFree(r0 + h, c0 + i)) all = false; if (!all) break; h++; }
      region.clear(); for (int r = r0; r < r0 + h; r++) for (int c = c0; c < c0 + w; c++) region.push_back(r * d.C + c + 1);
    }
    if (!zoom.empty()) { layout = L::place(layout, id, region, grid).layout; applyLayout(); persistLayout(); return; }
    placeTile(id, region);
  }
  void zoomToggle(const std::string& id) {
    if (!zoom.empty()) {
      zoom.clear(); applyLayout();
      if (!redrawOwed.empty()) { std::vector<std::string> owed(redrawOwed.begin(), redrawOwed.end()); redrawOwed.clear(); queuePaneRedraw(owed, 400); }   // redraws that fell due behind the zoom
      return;
    }
    if (isPlaced(id)) zoom = id; else return;
    applyLayout();
  }
  // 80-sidebar.js stepTile: previous / next tile in sidebar order, wrapping; shown, not committed
  void stepTile(int delta) {
    if (zoomBlocks()) return;
    const std::vector<std::string> ids = tileIds();
    if (ids.empty()) return;
    int i = 0; for (size_t k = 0; k < ids.size(); k++) if (ids[k] == st.selected) i = (int)k;
    const int n = (int)ids.size();
    const std::string next = ids[((i + delta) % n + n) % n];
    stepPreviewBegin();
    // The walk's HOME is the pane (or empty slot) that was selected when it began: a session that is not on
    // screen previews there, whatever was stepped past on the way. activate() adopts against the selection.
    st.selected = stepSnap.active;
    activate(next, true);
    stepPreviewMark();
  }
  // 20-terminals.js redrawTile: reset, re-attach with only what was written at this size, nudge the pty
  void redrawTile(const std::string& id) {
    // a cloud pane: its page loads again (48-cloud.js cloudRedraw), the name flashes all the same
    if (cloudPanes.redraw(id)) { flashName = id; flashUntil = GetTickCount64() + 600; invalidate(); return; }
    auto it = terms.find(id); if (it == terms.end()) return;
    TermView& v = *it->second;
    v.term.reset(); v.scrollOff = 0; v.sel = Selection(); v.repaintAll = true;
    if (inited) send(proto::attach(id, true));
    flashName = id; flashUntil = GetTickCount64() + 600;
    fitVisible();
    int mc, mr;
    if (st.shown.count(id) && !mirrorOf(id, mc, mr) && v.term.cols() > 2 && v.term.rows() > 1) { if (testRun()) logLine("nudge " + id); fire("nudge", json::array({ id, v.term.cols(), v.term.rows() })); }
    invalidate();
  }

  void scrollTileIntoView(const std::string& id) {
    float top, bottom; if (!sidebar.tileSpan(id, top, bottom)) return;
    float vh = sidebar.viewHeight(); if (vh <= 0) return;
    if (top < st.scroll) st.scroll = top; else if (bottom > st.scroll + vh) st.scroll = bottom - vh;
    clampScroll();
  }
  void clampScroll() { float mx = std::max(0.f, sidebar.contentHeight() - sidebar.viewHeight()); st.scroll = std::max(0.f, std::min(mx, st.scroll)); }

  std::string windowTitle() const { if (!hwnd) return ""; wchar_t cur[512] = L""; GetWindowTextW(hwnd, cur, 511); return narrow(cur); }
  void updateTitle() {
    if (!hwnd) return;
    std::vector<std::string> vis = visibleIds();
    bool fills = vis.size() == 1;
    if (fills && !L::isEmpty(layout)) { L::Rect r = L::rect(L::occupancy(layout, vis[0], grid), grid); fills = r.w == 1 && r.h == 1; }
    const json* t = fills ? tile(vis[0]) : nullptr;
    int n = 0; if (meta.contains("tiles")) for (const auto& x : meta["tiles"]) if (!(x.contains("exited") && x["exited"].is_boolean() && x["exited"].get<bool>())) n++;
    std::string want = t ? tileTitle(*t) : DECK_EDITION_PRODUCT " [" + std::to_string(n) + "]";
    // a title bar holds a line, not a paragraph (a program may set any length of window title): cut on a character
    if (want.size() > 200) { size_t cut = 200; while (cut > 0 && ((unsigned char)want[cut] & 0xC0) == 0x80) cut--; want = want.substr(0, cut) + "\xE2\x80\xA6"; }
    want += localMode ? " \xE2\x80\x94 local" : " \xE2\x80\x94 remote";
    std::wstring w = widen(want);
    wchar_t cur[512]; GetWindowTextW(hwnd, cur, 511);
    if (w != cur) SetWindowTextW(hwnd, w.c_str());
  }

  // ------------------------------------------------------------ drawing
  void invalidate() { syncFocus(); if (hwnd && !headless()) InvalidateRect(hwnd, nullptr, FALSE); }
  // LOCAL mode: focus in / out reports (DECSET 1004), as xterm sends them when its textarea gains or loses the
  // focus (CoreBrowserTerminal _handleTextAreaFocus / _handleTextAreaBlur): ESC [ I to the pane that now has the
  // keys, ESC [ O to the one that had them. Checked wherever the view changes; never from a remote (R8).
  std::string focusPane;
  void syncFocus() {
    if (!localMode || !inited) return;
    std::string cur; if (!st.selected.empty() && st.shown.count(st.selected) && paneFocused(st.selected)) cur = st.selected;
    if (cur == focusPane) return;
    const std::string was = focusPane; focusPane = cur;
    auto a = terms.find(was); if (a != terms.end() && a->second->term.focusReporting()) sendReport(was, "\x1b[O");
    auto b = terms.find(cur); if (b != terms.end() && b->second->term.focusReporting()) sendReport(cur, "\x1b[I");
  }
  void dropCaches() { gfx.dropScene(); gfx.runCacheDrop(); }

  // One frame. The panes and the sidebar live in the scene (gfx.hpp): only what changed is painted into it, it is
  // copied to the target in one blit, and the overlays (the drop preview, notes, menus, panels, the drag picture)
  // are drawn over it, every frame they are up. Without a scene (it could not be made, or --no-scene) the same
  // painters draw the whole frame straight to the target, as before; the picture is the same either way.
  void drawFrame() {
    st.blinkOn = caretOn;
    st.animOn = animOn() && (!headless() || animForced);
    // css transitions (ui.hpp State::transOn): their clock is read HERE, once a frame — a transition starts in the
    // frame that first draws its new target, so that frame needs "now". A test's frames never read the time: its
    // transitions are off unless a script sets the clock (`trans <ms>`), so every scripted shot stays what it was.
    st.transOn = animOn() && opt.bench.empty() && (!testRun() || transForced);
    if (st.transOn && !transForced) { if (!animEpoch) animEpoch = GetTickCount64(); st.transMs = (double)(GetTickCount64() - animEpoch); }
    askSync();
    cloudSync();
    if (gfx.sceneBegin()) { paintScene(gfx.sceneFresh); gfx.sceneEnd(); gfx.sceneBlit(); }
    else paintScene(true);
    drawMainOver();
    drawPanels();                                       // the menus and panels that sit over the sidebar
    if (ctx.open) ui::drawCtxMenu(gfx, meta, st, ctx, viewW, viewH);
    drawPanelsTop();                                    // the palette: over everything
    if (!dragSrc.empty() && ghostW > 0) ui::drawTileGhost(gfx, meta, st, dragSrc, st.mouseX - grabDX, st.mouseY - grabDY, ghostW);   // the drag image
    drawNotice();
    drawOneDeck();                                      // one deck at a time: the blocked strip
    // an overlay whose box is only known once it is drawn (a menu, a panel) may have come over the pane this frame
    if (askPane.isOpen() && overAskPane() != askPane.covered()) invalidate();
    if (cloudPanes.count() && !cloudPanes.settled(cloudViews())) invalidate();
    animArm();
  }
  // ---- the cloud panes' place in the frame (cloud.hpp). Each one's page is a child window on its pane, below the
  // pane's name. Every frame says where each is, whether anything of the app's is drawn over it (then it shows as a
  // still picture, as the ask pane does), and whether it is on screen at all.
  bool paneNoteBox(const RectF& mr, RectF& out) {
    if (paneNoteText.empty()) return false;
    Font f; f.family = monoFamily; f.size = 12;
    const std::wstring t = widen(paneNoteText);
    const float maxW = mr.w * 0.9f, tw = gfx.textWidth(t, f);
    const float w = std::min(maxW, std::ceil(tw) + 26);
    float h = gfx.textWrapped(t, RectF(0, 0, w - 26, 1000), f, Rgba(), 0, 0, false);
    if (h <= 0) h = 16;
    out = RectF(std::round(mr.x + (mr.w - w) / 2), mr.y + 8, w, h + 14);
    return true;
  }
  bool webIdleTimer = false, askHoverArmed = false;
  bool testMin = false;                                 // (tests) `wm min`: as if minimised (a parked window is never really minimised)
  bool minimised() const { return testMin || (hwnd && !headless() && IsIconic(hwnd)); }
  bool overCloudPane(const RectF& pr) {
    if (pal.open || ctx.open || !st.notice.empty() || !dragSrc.empty() || cdlg.open) return true;
    const RectF mr = mainRect(); RectF nb;
    if ((mv.open && meets(mv.box, pr)) || (pull.open && meets(pull.box, pr)) || (sset.open && meets(sset.box, pr))) return true;
    if (paneNoteBox(mr, nb) && meets(nb, pr)) return true;
    if (zoom.empty() && !leaving.empty() && L::isLegal(leaving, grid) && meets(setRect(leaving, mr), pr)) return true;
    return false;
  }
  float cloudInset(const std::string& id, bool inGrid) const {
    const json* t = tile(id);
    if (t && t->contains("attention") && (*t)["attention"].is_boolean() && (*t)["attention"].get<bool>()) return 2;   // css .term.desk-attn::before: 2px
    return inGrid ? 1.f : 0.f;                                                                                        // css .term.grid.placed::after: 1px
  }
  std::vector<cloud::View> cloudViews() {
    std::vector<cloud::View> out;
    const bool gone = askPane.isOpen() || minimised();    // the ask pane is over the whole area (css z 20); minimised
    const bool inGrid = !L::isEmpty(layout);
    for (const auto& id : st.shown) {
      if (!cloudPanes.isCloud(id)) continue;
      cloud::View v; v.id = id;
      if (!paneRect(id, v.pane)) continue;               // behind a zoom: not on show (the pane keeps its page, hidden)
      v.inset = cloudInset(id, inGrid); v.hidden = gone; v.covered = !gone && overCloudPane(v.pane);
      out.push_back(v);
    }
    return out;
  }
  void cloudSync() {
    if (closing) return;
    cloudPanes.sync(gfx, cloudViews(), paneNameSize());
    // a page nobody can see is closed when its grace runs out, frames or no frames (a minimised window draws none)
    if (hwnd && !headless()) { const bool want = cloudPanes.anyOffscreen(); if (want != webIdleTimer) { webIdleTimer = want; if (want) SetTimer(hwnd, TIMER_WEBIDLE, cloudPanes.tickMs(), nullptr); else KillTimer(hwnd, TIMER_WEBIDLE); } }
    if (!cloudWantFocus.empty()) {
      if (cloudWantFocus != st.selected || !cloudPanes.isCloud(cloudWantFocus)) cloudWantFocus.clear();
      else if (cloudPanes.ready(cloudWantFocus)) { if (windowFocused && !st.focusEdit && !anyPanelOpen() && !cdlg.open) cloudPanes.focus(cloudWantFocus); cloudWantFocus.clear(); }
    }
  }
  // The ONE WebView2 user-data folder of this deck: the cloud panes' sign-ins live in it (a named profile per
  // account), and the ask pane runs in it too, InPrivate (asks.hpp): one folder = one browser process tree for
  // every web view of every window. It keeps the name it had when only the cloud panes used it, because renaming
  // it would sign everybody out. '' = no folder (a test without --state-dir): no cloud page, and the ask pane
  // falls back to a private temp folder of its own.
  std::wstring webDir() const {
    if (!opt.stateDir.empty()) return opt.stateDir + L"\\web-cloud";
    return testRun() ? std::wstring() : configDir() + L"\\web-cloud";
  }
  void initCloud() {
    cloud::Deps d;
    // a click in the page is a click in the pane (48-cloud.js: wv 'focus' -> activate); a press anywhere ends a walk
    d.focused = [this](const std::string& id, bool byUser) {
      if (closing) return;
      if (testRun()) logLine("cloud page " + id + " has the keyboard" + (byUser ? " (a click in the page)" : " (handed over)"));
      // our own hand-off of the keyboard, arriving after the selection has moved on (the page came up late): it is
      // not a click, so it selects nothing; the keys go back to the window unless another page is to have them
      if (!byUser && st.selected != id) { if (hwnd && !cloudPanes.isCloud(st.selected)) SetFocus(hwnd); return; }
      if (byUser) { stepPreviewCommit(); closeCtxMenu(); closePull(); closeMoveMenu(); }
      windowFocused = true;
      if (st.selected != id && tile(id)) activate(id, true);
      st.focusInTerminal = true; invalidate();
    };
    d.changed = [this] { invalidate(); };
    // a window key pressed while a cloud page has the keyboard (main.js: the guest's before-input-event -> windowKeys)
    d.accelerator = [this](unsigned vk, bool c, bool s, bool a) {
      const Mods m = modsFrom(c, s, a, GetAsyncKeyState(VK_RMENU) < 0);
      const json se = setting("stepEscape");
      if (vk == VK_ESCAPE && !m.any() && stepSnap.on && !(se.is_boolean() && !se.get<bool>())) { stepPreviewCancel(); return true; }   // Esc undoes a walk, wherever the keys are
      if (windowKeyOf(vk, m) == WinKey::None) return false;                                                      // the page's own key
      if (testRun()) logLine("accelerator from a cloud page: vk " + std::to_string(vk) + " mods " + std::to_string(m.code()));
      hideTip();
      if (vk != VK_F11 && vk != VK_F12) stepPreviewCommit();   // a key that is not a step ends the walk (25-layout.js)
      windowKey(vk, m);
      return true;
    };
    d.openOutside = [this](const std::string& url) { openOutside(url); };
    d.log = [this](const std::string& line) { if (testRun()) logLine(line); };
    if (!opt.webRuntime.empty()) cloudPanes.webOptions.fixedRuntimeDir = opt.webRuntime;
    // The sign-ins live in <state>\web-cloud (the folder config.json is in). A test has its own state folder or no
    // web view at all, and never loads claude.ai: only the local stand-in --cloud-base names (cloud.hpp).
    const std::wstring dir = webDir();
    if (testRun() && opt.cloudGrace > 0) cloudPanes.offscreenMs = (unsigned)opt.cloudGrace;
    if (!opt.windowShot.empty() && opt.forceDpi > 0) cloudPanes.rasterScale = opt.forceDpi / 96.f;
    cloudPanes.init(headless() || !opt.bench.empty() ? nullptr : hwnd, d, dir, testRun(), narrow(opt.cloudBase));
  }
  // main.js openOutside / open:external: an http(s) address in the default browser. A test opens nothing: it logs.
  void openOutside(const std::string& url) {
    if (testRun()) { logLine("OPEN " + url); return; }
    web::WebPane::openExternal(widen(url));
  }
  void beginWebShutdown() { askPane.beginShutdown(); cloudPanes.beginShutdown(); }
  // ---- the ask pane's place in the frame. Its page is a CHILD WINDOW over the terminal area: nothing Direct2D
  // draws shows over it. So while something of the app's is over the terminal area the pane is told (cover): the
  // page is photographed and hidden, and the pane draws the photograph, the overlay over it (asks.hpp cover()).
  static bool meets(const RectF& a, const RectF& b) { return a.w > 0 && a.h > 0 && a.x < b.r() && b.x < a.r() && a.y < b.b() && b.y < a.b(); }
  bool overAskPane() {
    if (!askPane.isOpen()) return false;
    if (pal.open || ctx.open || !st.notice.empty() || !dragSrc.empty() || cdlg.open) return true;
    const RectF mr = mainRect();
    return (mv.open && meets(mv.box, mr)) || (pull.open && meets(pull.box, mr)) || (sset.open && meets(sset.box, mr));
  }
  void askSync() { askPane.layout(mainRect(), gfx.scale); askPane.cover(overAskPane()); askPane.minimised(minimised()); }
  void openAsks(const std::string& mode, const std::string& scrollTo = "") { askSync(); askPane.openPane(mode, scrollTo); }
  // the keys are a terminal's only while no ask pane is over the terminals (49-asks.js: a terminal that takes the
  // focus behind the pane gives it back)
  // … and never a cloud tile's: the terminal under its page is not shown (css .term.cloud .xterm), its page has the keys
  bool termKeys() const { return st.focusInTerminal && !askPane.isOpen() && !cloudPanes.isCloud(st.selected); }
  bool askWarm() const { return opt.webWarm >= 0 ? opt.webWarm != 0 : !testRun(); }
  void endAsks() {
    // The browser process is waited for only when this window's web views were the last ones: another window of
    // this process that still has a page keeps the (one, shared) browser alive, and waiting for it would only stall.
    bool othersHaveWeb = false;
    for (App* a : shell().apps) if (a != this && !a->closing && (a->askPane.webMade() || a->cloudPanes.webCount())) othersHaveWeb = true;
    const unsigned waitMs = othersHaveWeb ? 0 : 3000;
    cloudPanes.beginShutdown();                         // (both kinds closed before either wait: they share the process)
    const ULONGLONG t0 = GetTickCount64(); const bool made = askPane.webMade(); const bool clean = waitMs ? askPane.shutdown(waitMs) : (askPane.beginShutdown(), true);
    if (made && testRun()) logLine("ask web view shut down: clean " + std::to_string(clean) + ", " + std::to_string((unsigned)(GetTickCount64() - t0)) + " ms");
    const ULONGLONG t1 = GetTickCount64(); const bool cmade = cloudPanes.anyMade(); const bool cclean = cloudPanes.shutdown(waitMs);
    if (cmade && testRun()) logLine("cloud web views shut down: browser process gone " + std::to_string(cclean) + ", " + std::to_string((unsigned)(GetTickCount64() - t1)) + " ms (the profile folder stays: it holds the sign-ins)");
  }
  void initAsks() {
    asks::Deps d;
    d.call = [this](const std::string& fn, const json& a, std::function<void(bool, const json&, const std::string&)> done) {
      call(fn, a, [done](bool ok, const json& r) { done(ok, ok ? r : json(), ok ? std::string() : errText(r)); });
    };
    d.fire = [this](const std::string& fn, const json& a) { if (testRun()) logLine("fire " + fn + " " + a.dump().substr(0, 200)); fire(fn, a); };
    d.activate = [this](const std::string& id) { activate(id, false); if (hwnd && !headless()) SetFocus(hwnd); };
    d.focusTerminal = [this] { if (hwnd && !headless()) { HWND f = GetFocus(); if (f && f != hwnd && IsChild(hwnd, f)) SetFocus(hwnd); } st.focusInTerminal = !st.selected.empty(); invalidate(); };
    d.note = [this](const std::string& t, int ms) { note(t, ms); };
    // A link on an ask page (main.js openAskLink). LOCAL: the DECK decides, as Electron's main process does - http(s)
    // goes to the default browser, a file opens in its own program, something that would RUN is only shown in its
    // folder (the core's `askOpenLink`; the opening is ShellLifecycle's, which a test only logs). A REMOTE: http(s)
    // in this machine's browser and nothing else - a file: link names a file on the deck's PC.
    d.openLink = [this](const std::wstring& url) {
      if (localMode) { call("askOpenLink", json::array({ narrow(url) }), [this](bool ok, const json& e) { if (!ok) refused("", e); }); return; }
      if (testRun()) { logLine("OPEN (remote: http(s) only) " + narrow(url)); return; }
      web::WebPane::openExternal(url);
    };
    d.writeClipboard = [this](const std::string& t) { setClipboard(widen(t)); };
    // the native pane's answer boxes are text fields like the app's others (focusEdit: the caret, the keys, the IME)
    d.focusEdit = [this](EditBox* e) { focusEdit(e); };
    d.focusedEdit = [this]() -> EditBox* { return st.focusEdit; };
    // An OLDER ask page's own Done downloads `ask_<id>.answers.json` (or .txt). main.js hookAskDownloads saves exactly
    // that name, with no dialog, in the first of the ask feed's Downloads folders that exists - where the ask-page
    // skill's `wait` looks. LOCAL only: on a remote the file would land on the wrong machine (cancelled; the page's
    // Copy all works). Any other download from an ask page is cancelled (Electron would show a Save dialog).
    d.downloadTo = [this](const std::wstring& name) -> std::wstring {
      const std::string n = narrow(name);
      static const std::regex answers("^ask_[A-Za-z0-9_-]+\\.answers(\\.json|\\.txt)$");
      std::wstring to;
      if (localMode && std::regex_match(n, answers))
        for (const std::string& dir : shell().host.askDownloads()) {
          const DWORD at = GetFileAttributesW(widen(dir).c_str());
          if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) { to = widen(dir) + L"\\" + name; break; }
        }
      if (testRun()) logLine("ask page download " + n + (to.empty() ? std::string(" -> cancelled") + (localMode ? "" : " (remote)") : " -> " + narrow(to)));
      if (!to.empty()) note("Answers saved: " + n, 2500);
      return to;
    };
    d.changed = [this] { st.asksFocus = askPane.asksFocused(); st.asksPick = askPane.asksPick(); invalidate(); };
    // a window key pressed while the PAGE has the keyboard (asks.hpp has already decided it is the app's: keys.hpp
    // webKeyOf): the same windowKey a press in the window reaches
    d.accelerator = [this](unsigned vk, bool c, bool s, bool a) {
      const Mods m = modsFrom(c, s, a, GetAsyncKeyState(VK_RMENU) < 0);
      if (testRun()) logLine("accelerator from the web view: vk " + std::to_string(vk) + " mods " + std::to_string(m.code()));
      hideTip();
      const bool step = vk == VK_F11 || vk == VK_F12;
      if (!step) stepPreviewCommit();
      const bool took = windowKey(vk, m);
      if (step) stepPreviewCommit();                    // no later key of the page's reaches the app to end the walk: each step is final
      return took;
    };
    // --shot / --probe / --bench have no window on any screen and never a web view: the pane draws its bar alone
    if (!opt.webRuntime.empty()) askPane.webOptions.fixedRuntimeDir = opt.webRuntime;
    // The ask pane's profile: InPrivate (cookies and storage in memory only: the deck's cookie never reaches disk),
    // inside the deck's one web folder, so it shares the cloud panes' browser process tree instead of starting a
    // second one. It is the InPrivate side of the folder's DEFAULT profile, which nothing else uses (every cloud
    // pane is in a named profile of its account). Not a named profile of its own: the first view ever made in a
    // new named profile takes about five seconds to come up (measured: WEB-NOTES.md), the default one 0.3 s.
    { const std::wstring dir = webDir();
      if (!dir.empty()) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        askPane.webOptions.userDataDir = dir; askPane.webOptions.removeUserDataOnClose = false; askPane.webOptions.inPrivate = true; askPane.webOptions.profileName.clear();
      } }
    if (testRun() && opt.askGrace > 0) { askPane.graceMs = (unsigned)opt.askGrace; askPane.offscreenMs = (unsigned)(opt.cloudGrace > 0 ? opt.cloudGrace : opt.askGrace); }
    if (!opt.windowShot.empty() && opt.forceDpi > 0) askPane.web().setRasterScale(opt.forceDpi / 96.f);   // (tests, --dpi on the parked window: the page at that scale too)
    askPane.init(headless() || !opt.bench.empty() ? nullptr : hwnd, d);
    // config.json askPane: "native" (the default: the shell draws the cards, no web view) | "web" (WebView2, the
    // fallback); --ask-pane overrides it (tests)
    { std::string ap = opt.askPane;
      if (ap.empty() && cfg.j.contains("askPane") && cfg.j["askPane"].is_string()) ap = cfg.j["askPane"].get<std::string>();
      askPane.setNative(ap != "web");
      if (testRun()) logLine(std::string("ask pane: ") + (askPane.native() ? "native (askcards.hpp)" : "web (WebView2)")); }
    askPane.setLink(localMode ? DeckLink() : link);
  }
  // style.css #rail: collapsed, the whole thin strip is the button
  void drawRail() {
    RectF r = sidebarRect();
    gfx.fill(r, Rgba::hex(st.hoverPart == "rail" ? 0x1a1a1a : 0x0f0f0f));
    float cx = r.x + r.w / 2, cy = r.b() - 8 - 11;
    Rgba c = Rgba::hex(0x9a9a9a);
    bool right = sidebarRight();
    float d = right ? -1.f : 1.f;
    gfx.line(cx - 2 * d, cy - 4, cx + 2 * d, cy, c, 1.4f); gfx.line(cx + 2 * d, cy, cx - 2 * d, cy + 4, c, 1.4f);
  }
  // ---- the scene's bookkeeping: what each part was painted FROM. A part is painted again when that differs.
  struct PaneSeen { std::string key, cursor; int cursorRow = -1; };
  std::string sceneAnim;                                // the animation clock the sidebar's moving parts were last painted at
  static unsigned rgbKey(const Rgba& c) { return ((unsigned)std::lround(c.r * 255) << 16) | ((unsigned)std::lround(c.g * 255) << 8) | (unsigned)std::lround(c.b * 255); }
  std::map<std::string, PaneSeen> sceneSeen;            // the panes the scene holds
  std::string sceneBase, sceneSidebar;                  // the frame's skeleton, and everything the sidebar is drawn from
  unsigned metaGen = 0;                                 // bumped whenever `meta` changes (a new snapshot, or a field set here)
  size_t paneMetaSig = 0;                               // the parts of meta a pane's NAME is drawn from besides its own tile
  struct SceneStats { unsigned frames = 0, full = 0, paneFull = 0, paneRows = 0, sidebar = 0, held = 0, animParts = 0; } sceneStats;
  static bool paneHeld(const TermView& v) { return v.term.synchronizedOutput() && v.syncSince && GetTickCount64() - v.syncSince < 1000; }

  static void sigEdit(std::string& o, const EditBox* e) {
    if (!e) { o += "-|"; return; }
    o += narrow(e->text); o += '\x1'; o += narrow(e->placeholder);
    char b[96]; snprintf(b, sizeof b, "\x1%d,%d,%g,%g|", e->caret, e->anchor, e->scrollX, e->scrollY); o += b;
  }
  // Everything Sidebar::draw(g, area, meta, st) reads: the area, the snapshot (by generation) and every field of
  // ui::State, text boxes included. A field added to ui::State that the sidebar draws from MUST be added here.
  std::string sidebarSig(const RectF& sr) const {
    std::string o; o.reserve(600);
    char b[320];
    snprintf(b, sizeof b, "%u|%g,%g,%g,%g|%d%d%d%d%d%d%d%d%d|%g|%g|%g|%d|%lld|%d,%d|", metaGen, sr.x, sr.y, sr.w, sr.h, st.sidebarOpen, st.showBars, st.showMsgs, st.grouped,
      st.searchOpen, st.focusInTerminal, st.connected, st.restartConfirming, st.asksFocus, st.sidebarWidth, st.scroll, st.toolsWidth, st.windowIndex, st.clockSkew, st.elsewhereCount, st.pastCount);
    o += b; o += st.claudeOff ? "L|" : "R|"; o += st.ownLayoutOnly ? "o|" : "-|"; o += st.officeScreen ? "O|" : "-|"; o += st.animOn ? "A|" : "a|"; o += st.shellName; o += '|';
    // a css transition under way (the last draw said so): every frame is another step of it, until it has settled
    if (st.transOn && sidebar.transActive()) { snprintf(b, sizeof b, "T%.0f|", st.transMs); o += b; } else o += st.transOn ? "T|" : "t|";
    for (const std::string* s : { &st.selected, &st.hover, &st.hoverPart, &st.hoverArg, &st.pressed, &st.search, &st.renaming, &st.dragging, &st.dropMark, &st.notice, &st.noticeDetail, &st.asksPick }) { o += *s; o += '|'; }
    o += st.dropBefore ? "b|" : "a|";
    for (const auto& s : st.shown) { o += s; o += ','; }
    o += '|';
    for (const auto& s : st.collapsedGroups) { o += s; o += ','; }
    o += '|';
    for (const auto& kv : st.confirm) { o += kv.first + ":" + kv.second.kind + ":" + kv.second.account + ":" + std::to_string(kv.second.browser) + ","; }
    o += '|';
    // the pointer matters to the sidebar only while it is over it (the pin's corner zone follows it)
    if (st.sidebarOpen && sr.has(st.mouseX, st.mouseY)) { snprintf(b, sizeof b, "%g,%g|", st.mouseX, st.mouseY); o += b; } else o += "-|";
    sigEdit(o, st.renameEdit); sigEdit(o, st.searchEdit);
    if (st.hold) {
      const ui::HoldForm& h = *st.hold;
      o += h.tile + ":" + h.kind + ":" + narrow(h.err) + ":";
      for (const auto& p : h.picks) { o += narrow(p.label); o += ','; }
      for (const EditBox* e : { &h.text, &h.time, &h.every, &h.from, &h.to, &h.trig }) sigEdit(o, e);
    }
    // a caret is showing somewhere: which box, and its blink
    if (st.focusEdit) { snprintf(b, sizeof b, "%p,%d|", (void*)st.focusEdit, st.blinkOn); o += b; }
    return o;
  }
  bool paneFocused(const std::string& id) const { return id == st.selected && termKeys() && windowFocused && !st.focusEdit; }
  // everything drawPane draws from, except the grid's own cells and the cursor (those repaint by rows)
  std::string paneKey(const std::string& id, const TermView& v, const RectF& pr, bool inGrid) {
    const json* t = tile(id);
    const bool flashing = flashName == id && GetTickCount64() < flashUntil;
    const int growPhase = growingId != id ? -1 : st.animOn ? (int)(std::fmod(st.animMs, 800.0) / 200) : 4;
    const Selection& s = v.sel;
    char b[420];
    snprintf(b, sizeof b, "%g,%g,%g,%g|%d%d%d%d%d%d|%g,%g|%d,%d,%d,%d,%d|%d:%d,%d,%d,%d|%d|%zu|%d|", pr.x, pr.y, pr.w, pr.h, inGrid, id == st.selected, zoom == id,
      st.hoverPart == "pane-name" && st.hover == id, flashing, (int)underPreview.count(id), v.fontSize, paneNameSize(), v.term.cols(), v.term.rows(), v.term.scrollbackSize(), v.scrollOff,
      v.term.altScreen(), s.active, s.ax, s.ay, s.bx, s.by, growPhase, paneMetaSig, v.customBox);
    std::string o = b; o += narrow(monoFamily); o += '|';
    o += cloudPanes.sig(id); o += (st.hoverPart == "cloud-link" && st.hover == id) ? "h|" : "|";
    if (t) {
      o += tileTitle(*t); o += '\x1'; o += acctAbbr(*t); o += '\x1'; o += str(*t, "status"); o += '\x1'; o += str(*t, "kind"); o += '\x1'; o += str(*t, "sessionId");
      // (the attention frame pulses with the animation clock, desk-attn-pulse: its colour is part of what the pane is
      // drawn from, so a pane under attention is drawn again as the colour moves; no other pane is)
      if (t->contains("attention") && (*t)["attention"].is_boolean() && (*t)["attention"].get<bool>()) { o += "\x1!"; o += std::to_string(rgbKey(attnColor())); } else o += "\x1.";
    } else o += "(no tile)";
    return o;
  }
  // the cursor as TermView::draw would draw it now
  std::string cursorKey(const std::string& id, const TermView& v, int& row) const {
    row = -1;
    if (!v.term.cursorVisible() || v.scrollOff != 0) return "none";
    const int cx = std::min(v.term.cursorX(), v.term.cols() - 1); row = v.term.cursorY();
    char b[64];
    if (!paneFocused(id)) snprintf(b, sizeof b, "o%d,%d", cx, row);
    else snprintf(b, sizeof b, "f%d,%d,%d,%d", cx, row, v.term.cursorShape(), (blinkOn || v.term.cursorBlink() == 0) ? 1 : 0);
    return b;
  }
  // #main.has-grid: the grid's cell lines, so empty cells are bounded too
  void paintGridLines(const RectF& mr) {
    L::Dims d = L::dims(grid);
    for (int c = 0; c <= d.C; c++) { float x = gfx.snap(mr.x + mr.w * c / d.C) - 1; gfx.fill(RectF(x, mr.y, 1, mr.h), Rgba::hex(0x262626)); }
    for (int r = 0; r <= d.R; r++) { float y = gfx.snap(mr.y + mr.h * r / d.R) - 1; gfx.fill(RectF(mr.x, y, mr.w, 1), Rgba::hex(0x262626)); }
  }
  // Rows a..b of a pane's grid, painted again in place. A glyph may reach into the row above or below its own, so
  // the band that is cleared and painted is rows a-1..b+1 (cut on device pixels) and the rows drawn into it are
  // a-2..b+2: inside the band that is every draw call a whole repaint of the pane would have made there, in the
  // same order (background, the rows, the cursor, the scrollbar), so the pixels are the same.
  void paintPaneRows(const std::string& id, TermView& v, const RectF& pr, bool inGrid, int a, int b) {
    const CellMetrics& m = metrics(v.fontSize);
    const RectF box = TermView::contentBox(pr, paneNameSize());
    const float oy = v.gridTop(box, m);
    const float y0 = std::max(pr.y, gfx.snapDown(oy + (a - 1) * m.h)), y1 = std::min(pr.b(), gfx.snapUp(oy + (b + 2) * m.h));
    if (y1 <= y0) return;
    gfx.pushClip(RectF(pr.x, y0, pr.w, y1 - y0));
    const Rgba bg = Rgba::hex(theme.background);
    gfx.pushClip(box); gfx.fill(RectF(pr.x - 2, y0 - 2, pr.w + 4, y1 - y0 + 4), bg); gfx.popClip();
    // the scrollbar's column (it straddles the box's right edge), on whole pixels
    const float sx0 = gfx.snapDown(pr.r() - 9), sx1 = gfx.snapUp(pr.r() - 3);
    gfx.fill(RectF(sx0, y0, sx1 - sx0, y1 - y0), bg);
    v.draw(gfx, box, monoFamily, m, theme, paneFocused(id), blinkOn, &pr, a - 2, b + 2);
    // that column was cleared from the pane's top to its foot: where the pane's frame crosses it, the frame again
    gfx.pushClip(RectF(sx0, y0, sx1 - sx0, y1 - y0)); paintPaneFrame(id, pr, inGrid); gfx.popClip();
    gfx.popClip();
    sceneStats.paneRows++;
  }
  // The scene: the terminal area's lines and panes, and the sidebar. all = paint every part (a new scene, or no
  // scene at all: straight to the target). Otherwise a part is painted only when what it is drawn from changed.
  void paintScene(bool all) {
    const RectF mr = mainRect(), sr = sidebarRect();
    const bool empty = L::isEmpty(layout);
    const bool lines = !empty && zoom.empty();
    std::vector<std::pair<std::string, RectF>> panes;
    for (const auto& id : st.shown) { RectF pr; if (!paneRect(id, pr)) continue; if (!terms.count(id)) continue; panes.push_back({ id, pr }); }
    // the skeleton: sizes, the grid's lines, which panes are where, the "no session" line
    std::string base;
    { char b[200]; snprintf(b, sizeof b, "%g,%g,%g|%g,%g,%g,%g|%g,%g,%g,%g|%d,%d,%d,%d|%d|", viewW, viewH, gfx.scale, mr.x, mr.y, mr.w, mr.h, sr.x, sr.y, sr.w, sr.h, lines, grid.rows, grid.cols, st.sidebarOpen, inited); base = b;
      for (const auto& p : panes) { snprintf(b, sizeof b, "%g,%g,%g,%g;", p.second.x, p.second.y, p.second.w, p.second.h); base += p.first; base += b; } }
    if (base != sceneBase) all = true;
    sceneStats.frames++;
    if (all) {
      sceneStats.full++;
      sceneBase = base; sceneSeen.clear(); sceneSidebar.clear();
      gfx.clear(Rgba::hex(0x0c0c0c));
      gfx.pushClip(mr);
      if (lines) paintGridLines(mr);
      if (panes.empty()) {
        Font f; f.family = L"Segoe UI"; f.size = 13;
        std::wstring msg = !inited ? L"" : L"No session selected. Use the tile at the bottom of the sidebar to start one.";
        gfx.text(msg, mr.inset(20), f, Rgba::hex(0x6a6a6a), Align::Center, false);
      }
      gfx.popClip();
    }
    gfx.pushClip(mr);
    for (const auto& p : panes) {
      const std::string& id = p.first; const RectF& pr = p.second;
      TermView& v = *terms[id];
      PaneSeen& seen = sceneSeen[id];
      // Synchronized output (DEC 2026): the application is mid-frame, so the pane keeps the picture it has (xterm 6
      // buffers the row refreshes until the mode goes off, one second at most: feedPane / syncTimeout). The damage
      // stays in the terminal and is painted with the frame that ends the hold. A pane the scene holds nothing
      // of (a new scene, a resize) is painted as it stands: there is no older picture to keep.
      if (paneHeld(v) && !all && !v.repaintAll && !seen.key.empty()) { sceneStats.held++; continue; }
      const std::string key = paneKey(id, v, pr, !empty);
      int crow = -1; const std::string ckey = cursorKey(id, v, crow);
      int d0 = 0, d1 = -1; const bool damaged = v.term.takeDamage(d0, d1);
      if (damaged && v.scrollOff != 0) { d0 = 0; d1 = v.term.rows() - 1; }   // scrolled back: the rows shown are not the rows that changed
      if (all || v.repaintAll || seen.key != key) { drawPane(id, v, pr, !empty); sceneStats.paneFull++; }
      else if (cloudPanes.isCloud(id)) {}                 // the terminal under a cloud pane's page is not drawn: nothing of it repaints
      else {
        // the damaged rows, and the rows the cursor left and entered (or changed its look on)
        std::vector<std::pair<int, int>> spans;
        if (damaged) spans.push_back({ d0, d1 });
        if (ckey != seen.cursor) { if (seen.cursorRow >= 0) spans.push_back({ seen.cursorRow, seen.cursorRow }); if (crow >= 0) spans.push_back({ crow, crow }); }
        std::sort(spans.begin(), spans.end());
        for (size_t i = 0; i < spans.size(); i++) {
          int a = spans[i].first, b = spans[i].second;
          while (i + 1 < spans.size() && spans[i + 1].first <= b + 4) { b = std::max(b, spans[i + 1].second); i++; }   // bands that would overlap are one band
          paintPaneRows(id, v, pr, !empty, a, b);
        }
      }
      seen.key = key; seen.cursor = ckey; seen.cursorRow = crow; v.repaintAll = false;
    }
    gfx.popClip();
    // a pane that left the screen is forgotten: it is painted whole when it comes back
    for (auto it = sceneSeen.begin(); it != sceneSeen.end();) { bool on = false; for (const auto& p : panes) if (p.first == it->first) on = true; if (on) ++it; else it = sceneSeen.erase(it); }
    // the sidebar (or the collapsed rail): whole, when anything it is drawn from changed
    const std::string sig = sidebarSig(sr);
    char ab[32]; snprintf(ab, sizeof ab, "%.0f", st.animOn ? st.animMs : -1.0);
    if (all || sig != sceneSidebar) {
      sceneSidebar = sig; sceneAnim = ab; sceneStats.sidebar++;
      gfx.pushClip(sr);
      if (!all) gfx.fill(RectF(sr.x - 1, sr.y - 1, sr.w + 2, sr.h + 2), Rgba::hex(0x0c0c0c));
      if (st.sidebarOpen) sidebar.draw(gfx, sr, meta, st); else drawRail();
      gfx.popClip();
    } else if (sceneAnim != ab) {
      // only the animation clock moved: the parts that move with it, each under its own pixel-aligned clip
      sceneAnim = ab;
      if (st.sidebarOpen && sidebar.animCount()) { gfx.pushClip(sr); sceneStats.animParts += (unsigned)sidebar.drawAnim(gfx, sr, meta, st); gfx.popClip(); }
    }
  }
  // what sits over the terminal area but is not a pane: drawn on the target, over the scene, while it is up
  void drawMainOver() {
    RectF mr = mainRect();
    gfx.pushClip(mr);
    const bool empty = L::isEmpty(layout);
    cloudPanes.drawOver(gfx);                           // the cloud panes that are still pictures just now, under every overlay
    if (!emptySlot.empty() && !empty && zoom.empty()) {
      L::Rect r = L::rect(emptySlot, grid);
      RectF sr(mr.x + (float)r.x * mr.w, mr.y + (float)r.y * mr.h, (float)r.w * mr.w, (float)r.h * mr.h);
      dashedRect(sr, Rgba::hex(0x8a8a8a), 1, 3, 2);
    }
    if (zoom.empty()) {                                 // #main.zoomed hides the preview and the leaving mark
      if (!previewSet.empty()) drawDropPreview(mr);
      if (!leaving.empty()) drawLeaving(mr);
    }
    drawPaneNote(mr);
    askPane.blink(st.blinkOn);                          // (the native pane's answer box: its caret blinks as the others do)
    askPane.draw(gfx);                                  // css #ask-pane z-index 20: over the panes and over the note (z 6)
    gfx.popClip();
  }
  // a css dashed border (Blink: `on` px on, `off` px off)
  void dashedRect(const RectF& r, const Rgba& c, float wdt, float on, float off) {
    for (float x = r.x; x < r.r(); x += on + off) { float n = std::min(on, r.r() - x); gfx.fill(RectF(x, r.y, n, wdt), c); gfx.fill(RectF(x, r.b() - wdt, n, wdt), c); }
    for (float y = r.y; y < r.b(); y += on + off) { float n = std::min(on, r.b() - y); gfx.fill(RectF(r.x, y, wdt, n), c); gfx.fill(RectF(r.r() - wdt, y, wdt, n), c); }
  }
  RectF setRect(const L::Cells& set, const RectF& mr) const {
    L::Rect r = L::rect(set, grid);
    float x0 = gfx.snap(mr.x + (float)r.x * mr.w), y0 = gfx.snap(mr.y + (float)r.y * mr.h);
    float x1 = gfx.snap(mr.x + (float)(r.x + r.w) * mr.w), y1 = gfx.snap(mr.y + (float)(r.y + r.h) * mr.h);
    return RectF(x0, y0, x1 - x0, y1 - y0);
  }
  bool animOn() const { json v = setting("animations"); return !(v.is_boolean() && !v.get<bool>()); }
  // ---- animations (css body.anim-on): ONE clock, st.animMs, ticked by ONE timer (TIMER_ANIM, 33 ms) that runs
  // only while something that animates is on screen: a tile that pulses or drifts, the asks tile asking, the armed
  // restart icon (the sidebar says which: Sidebar::animRegions), the drop preview, a growing label, a pane's
  // attention frame. Nothing animating = no timer, no frame. A tick repaints those parts only (paintScene). A frame
  // never reads the time itself. Headless runs have no timer: a script sets the clock (`anim <ms>`), else the
  // static look is drawn, as before.
  ULONGLONG animEpoch = 0; bool animTimerOn = false, animForced = false, transForced = false;
  // the Settings panel's rise (css .panel { animation: slide-up 160ms ease-out }: 10px up, opacity 0 -> 1), on the
  // transition clock: -1 = not rising. Started by the first frame that draws the open panel.
  double settingsRiseAt = -1; bool settingsWasOpen = false;
  float settingsRise() const {                          // 0..1 while it rises, 1 = there
    if (!st.transOn || settingsRiseAt < 0) return 1;
    const double q = (st.transMs - settingsRiseAt) / 160.0;
    return q >= 1 ? 1.f : ui::cubicBezier(0, 0, 0.58f, 1, (float)std::max(0.0, q));
  }
  bool transVisible() const { return st.transOn && ((st.sidebarOpen && sidebar.transActive()) || (sset.open && settingsRise() < 1)); }
  bool attnShown() const { for (const auto& id : st.shown) { const json* t = tile(id); if (t && t->contains("attention") && (*t)["attention"].is_boolean() && (*t)["attention"].get<bool>()) return true; } return false; }
  bool animVisible() const {
    if (!st.animOn) return false;
    return (st.sidebarOpen && sidebar.animCount() > 0) || (zoom.empty() && !previewSet.empty()) || !growingId.empty() || attnShown();
  }
  void animArm() {
    if (!hwnd || headless() || !opt.bench.empty() || closing) return;
    const bool want = (animVisible() || transVisible()) && !minimised();
    if (want && !animTimerOn) { if (!animEpoch) animEpoch = GetTickCount64(); SetTimer(hwnd, TIMER_ANIM, 33, nullptr); animTimerOn = true; }
    else if (!want && animTimerOn) { KillTimer(hwnd, TIMER_ANIM); animTimerOn = false; }
  }
  // 0 at the ends of a period, 1 in the middle, eased as css ease-in-out eases each half (sidebar.cpp pulse())
  static float easeIO(float x) {
    if (x <= 0) return 0; if (x >= 1) return 1;
    float t = x;
    for (int i = 0; i < 8; i++) { const float u = 1 - t, bx = 3 * u * u * t * 0.42f + 3 * u * t * t * 0.58f + t * t * t - x, dx = 3 * u * u * 0.42f + 6 * u * t * 0.16f + 3 * t * t * 0.42f; if (std::fabs(bx) < 1e-5f || dx == 0) break; t = std::max(0.f, std::min(1.f, t - bx / dx)); }
    return 3 * (1 - t) * t * t + t * t * t;
  }
  float animPulse(double periodMs) const { const double q = std::fmod(st.animMs, periodMs) / periodMs; return easeIO((float)(q < 0.5 ? q * 2 : (1 - q) * 2)); }
  // css .term.desk-attn::before: 2px #f0b429; body.anim-on: desk-attn-pulse, #8a6a1c <-> #f0b429, 1.6 s
  Rgba attnColor() const { return st.animOn ? Rgba::mix(Rgba::hex(0x8a6a1c), Rgba::hex(0xf0b429), animPulse(1600)) : Rgba::hex(0xf0b429); }
  // css #drop-preview: the rectangle the dragged session WILL take, with the rest of the area shaded
  void drawDropPreview(const RectF& mr) {
    if (!L::isLegal(previewSet, grid)) return;
    const RectF r = setRect(previewSet, mr);
    const Rgba shade(0, 0, 0, 0.45f);                    // box-shadow: 0 0 0 4000px rgba(0,0,0,.45), clipped by #main
    gfx.fill(RectF(mr.x, mr.y, mr.w, r.y - mr.y), shade); gfx.fill(RectF(mr.x, r.b(), mr.w, mr.b() - r.b()), shade);
    gfx.fill(RectF(mr.x, r.y, r.x - mr.x, r.h), shade); gfx.fill(RectF(r.r(), r.y, mr.r() - r.r(), r.h), shade);
    gfx.fill(r, Rgba::hex(0xbdbdbd, 0.08f));
    float bw = 3;                                        // animations on: the frame pulses 2px -> 5px, 0.8 s (css drop-pulse, ease-in-out; a border is whole device pixels)
    if (st.animOn) bw = std::floor((2 + 3 * animPulse(800)) * gfx.scale + 0.001f) / gfx.scale;
    gfx.stroke(r, Rgba::hex(0xbdbdbd), bw);
    Font f; f.family = L"Segoe UI"; f.size = 11;         // ::after { content: attr(data-zone); right 6, bottom 4 }
    std::wstring zone = widen(L::key(previewSet, grid));
    float tw = gfx.textWidth(zone, f);
    gfx.text(zone, RectF(r.r() - bw - 6 - tw - 1, r.b() - bw - 4 - 15, tw + 2, 15), f, Rgba::hex(0xbdbdbd, 0.7f), Align::Left, false);
  }
  // css #leaving: over the pane a session is about to be removed from: a dashed amber frame, faint stripes, a tag
  void drawLeaving(const RectF& mr) {
    if (!L::isLegal(leaving, grid)) return;
    const RectF r = setRect(leaving, mr);
    gfx.pushClip(r);
    const Rgba stripe = Rgba::hex(0xd9a441, 0.10f);      // repeating-linear-gradient(135deg, … 0 8px, transparent 8px 18px)
    const float s2 = 1.41421356f;
    for (float t = 4; t < (r.w + r.h) / s2 + 18; t += 18) { float c = t * s2; gfx.line(r.x + c + 20, r.y - 20, r.x - 20, r.y + c + 20, stripe, 8); }
    gfx.popClip();
    dashedRect(r, Rgba::hex(0xd9a441), 2, 6, 4);
    Font f; f.family = L"Segoe UI"; f.size = 11;
    const std::wstring tag = L"moving away";
    float tw = gfx.textWidth(tag, f);
    RectF tr(r.r() - 2 - 6 - tw - 12, r.y + 2 + 4, tw + 12, 17);
    gfx.fillRound(tr, 3, Rgba::hex(0x0c0c0c, 0.85f));
    gfx.text(tag, RectF(tr.x + 6, tr.y, tw + 2, tr.h), f, Rgba::hex(0xd9a441), Align::Left, false);
  }
  // css #zoom-note
  void drawPaneNote(const RectF& mr) {
    if (paneNoteText.empty()) return;
    if (paneNoteUntil && GetTickCount64() > paneNoteUntil && !headless()) { paneNoteText.clear(); paneNoteUntil = 0; return; }
    Font f; f.family = monoFamily; f.size = 12;
    std::wstring t = widen(paneNoteText);
    RectF r; if (!paneNoteBox(mr, r)) return;
    const float w = r.w;
    gfx.fillRound(r, 4, Rgba::hex(0x1e1e1e, 0.96f)); gfx.strokeRound(r, 4, Rgba::hex(0x8a8a8a), 1);
    Com<IDWriteTextLayout> lay = gfx.layout(t, f, w - 26, 1000, true, false, 0);
    if (lay) { lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); gfx.rt->DrawTextLayout(D2D1::Point2F(r.x + 13, r.y + 7), lay.p, gfx.b(Rgba::hex(0xd4d4d4)), (D2D1_DRAW_TEXT_OPTIONS)4); }
  }
  std::map<std::string, RectF> nameRects;   // each pane's name label, for clicks
  void drawPane(const std::string& id, TermView& v, const RectF& pr, bool inGrid) {
    gfx.pushClip(pr);
    gfx.fill(pr, Rgba::hex(theme.background));
    const bool active = id == st.selected;
    const CellMetrics& m = metrics(v.fontSize);
    float pn = paneNameSize();
    // a cloud tile that has its session: the page, not the terminal (css .term.cloud .xterm { visibility: hidden });
    // what is under the page's own window, and all of it when there is no page here, is cloud.hpp's
    if (cloudPanes.isCloud(id)) cloudPanes.drawBody(gfx, id, pr, pn, cloudInset(id, inGrid), st.hoverPart == "cloud-link" && st.hover == id);
    else v.draw(gfx, TermView::contentBox(pr, pn), monoFamily, m, theme, active && st.focusInTerminal && windowFocused && !st.focusEdit, blinkOn, &pr);
    // the pane's name: top-left, console font one size smaller (20-terminals.js paintPaneNames)
    const json* t = tile(id);
    if (t) {
      std::wstring abbr = widen(acctAbbr(*t)), title = widen(tileTitle(*t));
      bool busy = str(*t, "status") == "busy";
      bool attn = t->contains("attention") && (*t)["attention"].is_boolean() && (*t)["attention"].get<bool>();
      Font f; f.family = monoFamily; f.size = pn;
      if (growingId == id) {                              // css .term.growing .pane-name: bold, stepping up to the console size and back
        f.weight = 700;
        if (st.animOn) { int ph = (int)(std::fmod(st.animMs, 800.0) / 200); float big = v.fontSize; f.size = ph == 0 ? pn : ph == 2 ? big : (pn + big) / 2; }
      }
      Font fb = f; fb.weight = std::max(600, f.weight);
      std::wstring zoomTag = zoom == id ? L" \x00B7 zoomed" : L"";
      const bool cloudy = str(*t, "kind") == "cloud";
      float wc = cloudy ? 3 + pn : 0;                       // .cloud-mark: 1em square, 3px after the account
      float wa = (abbr.empty() ? 0 : gfx.textWidth(abbr, fb)) + wc + ((abbr.empty() && !cloudy) ? 0 : gfx.textWidth(L" ", fb));
      float wt = gfx.textWidth(title, f), wz = gfx.textWidth(zoomTag, f);
      float maxW = pr.w * 0.7f;
      // the session's ask mark rides after its name (49-asks.js askMarkInfo; css .pane-name .pane-ask: 6px to the left)
      const std::string sid = str(*t, "kind") == "claude" ? str(*t, "sessionId") : "";
      const float wm = sid.empty() ? 0 : ui::paneAskMark(gfx, meta, st, sid, 0, 0, false);
      float w = std::min(maxW, 8 + wa + wt + wz + (wm ? 6 + wm : 0) + 7), h = std::round(pn * 1.2f) + 3;
      RectF lr(pr.x + 1, pr.y + 1, w, h);
      nameRects[id] = lr;
      bool hover = st.hoverPart == "pane-name" && st.hover == id;
      gfx.fill(lr, hover ? Rgba::hex(0x282828, 0.92f) : Rgba::hex(0x0c0c0c, 0.82f));
      Rgba col = (flashName == id && GetTickCount64() < flashUntil) ? Rgba::hex(0x3ecf5a) : attn ? Rgba::hex(0xf0b429) : hover ? Rgba::hex(0xececec) : Rgba::hex(0xd4d4d4);
      float x = lr.x + 8;
      RectF tr(x, lr.y + 1, w - 15, h - 3);
      if (!abbr.empty()) gfx.text(abbr, tr, fb, busy ? Rgba::hex(0x3ecf5a) : Rgba::hex(0x9a9a9a), Align::Left, false);
      if (cloudy) ui::drawCloudMark(gfx, std::round(tr.x + (abbr.empty() ? 0 : gfx.textWidth(abbr, fb)) + 3), std::round(lr.y + (h - pn) / 2), pn, busy ? Rgba::hex(0x3ecf5a) : Rgba::hex(0x8fb8e8));
      tr.x += wa; tr.w -= wa;
      gfx.text(title, RectF(tr.x, tr.y, std::max(0.f, tr.w - wz), tr.h), f, col);
      if (!zoomTag.empty()) gfx.text(zoomTag, RectF(tr.x + std::min(wt, std::max(0.f, tr.w - wz)), tr.y, wz + 2, tr.h), f, Rgba::hex(0x6a6a6a), Align::Left, false);
      if (wm && tr.x + wt + wz + 6 + wm <= lr.r() - 6) ui::paneAskMark(gfx, meta, st, sid, tr.x + wt + wz + 6, lr.y + (h - 13) / 2 - 1, true);
    }
    paintPaneFrame(id, pr, inGrid);
    gfx.popClip();
  }
  // the lines around a pane, drawn last: the attention frame, and in a grid the pane's border
  void paintPaneFrame(const std::string& id, const RectF& pr, bool inGrid) {
    const json* t = tile(id);
    if (t && t->contains("attention") && (*t)["attention"].is_boolean() && (*t)["attention"].get<bool>()) gfx.stroke(pr, attnColor(), 2);
    if (inGrid && !underPreview.count(id)) gfx.stroke(pr, Rgba::hex(id == st.selected ? 0x8a8a8a : 0x1e1e1e), 1);
  }
  // bridge.js `say`: the banner while the deck is away, or refused
  void drawNotice() {
    if (st.notice.empty()) return;
    if (noticeUntil && GetTickCount64() > noticeUntil && st.connected) { st.notice.clear(); noticeUntil = 0; return; }
    Font f; f.family = L"Segoe UI"; f.size = 14; Font fd = f; fd.size = 12;
    std::wstring a = widen(st.notice), b = widen(st.noticeDetail);
    float w = std::min(viewW * 0.8f, std::max(gfx.textWidth(a, f), gfx.textWidth(b, fd)) + 34);
    float h = b.empty() ? 40 : 58;
    RectF r(std::round((viewW - w) / 2), 14, w, h);
    gfx.fillRound(r, 8, Rgba::hex(0x1a1a1a)); gfx.strokeRound(r, 8, Rgba::hex(0x444444), 1);
    gfx.text(a, RectF(r.x + 16, r.y + 9, r.w - 32, 22), f, Rgba::hex(0xdddddd));
    if (!b.empty()) gfx.text(b, RectF(r.x + 16, r.y + 31, r.w - 32, 18), fd, Rgba::hex(0x999999));
  }

  // ------------------------------------------------------------ headless: --shot and --probe
  void saveShot(const std::wstring& path) {
    if (!gfx.rt || !gfx.wicBitmap) { exitCode = 3; return; }
    gfx.begin(); drawFrame(); gfx.end();
    // a pane holding its picture (DEC 2026) is the one case where the scene's frame is NOT the whole frame, by design
    bool held = false; for (const auto& id : st.shown) { auto it = terms.find(id); if (it != terms.end() && paneHeld(*it->second)) held = true; }
    if (opt.verifyScene) { if (held) logLine("scene holds a pane (synchronized output): not compared"); else verifyScene(path); }
    exitCode = gfx.savePng(path) ? 0 : 4;
  }
  int exitCode = 0;
  // --verify-scene: the frame just drawn came out of the scene as the run left it (parts painted at different
  // moments, rows repainted in place). Draw the same frame again from nothing and compare every pixel: the scene
  // is only allowed to be faster, never different. The result goes to the run's log.
  std::vector<uint32_t> grabPixels() {
    std::vector<uint32_t> px; UINT w = 0, h = 0; gfx.wicBitmap->GetSize(&w, &h);
    WICRect r = { 0, 0, (INT)w, (INT)h }; Com<IWICBitmapLock> lock;
    if (FAILED(gfx.wicBitmap->Lock(&r, WICBitmapLockRead, lock.put())) || !lock) return px;
    UINT stride = 0, size = 0; BYTE* data = nullptr; lock->GetStride(&stride); lock->GetDataPointer(&size, &data);
    px.resize((size_t)w * h);
    for (UINT y = 0; y < h; y++) memcpy(&px[(size_t)y * w], data + (size_t)y * stride, (size_t)w * 4);
    return px;
  }
  void verifyScene(const std::wstring& path) {
    const std::vector<uint32_t> a = grabPixels();
    const bool was = gfx.sceneOff; gfx.sceneOff = true;                 // the whole frame, straight to the target
    gfx.begin(); drawFrame(); gfx.end();
    gfx.sceneOff = was;
    const std::vector<uint32_t> b = grabPixels();
    UINT w = 0, h = 0; gfx.wicBitmap->GetSize(&w, &h);
    size_t diff = 0, first = 0;
    if (a.size() != b.size()) diff = (size_t)-1;
    else for (size_t i = 0; i < a.size(); i++) if ((a[i] & 0xffffff) != (b[i] & 0xffffff)) { if (!diff) first = i; diff++; }
    const size_t sl = path.find_last_of(L"\\/"); const std::string name = narrow(sl == std::wstring::npos ? path : path.substr(sl + 1));
    if (!diff) logLine("scene == whole frame: " + name);
    else logLine("SCENE MISMATCH " + name + ": " + std::to_string(diff) + " px differ, the first at " + std::to_string(w ? first % w : 0) + "," + std::to_string(w ? first / w : 0));
  }
  // --dpi <n> on an off-screen run: the bitmap is drawn at that scale (144 = 150 %, 192 = 200 %); DIPs stay DIPs
  float shotScale() const { return opt.forceDpi > 0 ? opt.forceDpi / 96.f : 1.f; }
  // the window as Windows composes it (WM_PAINT through the HWND render target), for the one test that
  // must exercise the on-screen path without putting anything on a screen
  void captureWindow(const std::wstring& path) {
    UpdateWindow(hwnd);
    RECT rc; GetClientRect(hwnd, &rc);
    HDC wdc = GetDC(hwnd); HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL ok = PrintWindow(hwnd, mem, 1 /*PW_CLIENTONLY*/ | 2 /*PW_RENDERFULLCONTENT*/);
    SelectObject(mem, old);
    Com<IWICBitmap> wb; Com<IWICStream> stream; Com<IWICBitmapEncoder> enc; Com<IWICBitmapFrameEncode> frame;
    exitCode = 4;
    if (ok && SUCCEEDED(gfx.wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, wb.put())) &&
        SUCCEEDED(gfx.wic->CreateStream(stream.put())) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(gfx.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put()))) {
      enc->Initialize(stream.p, WICBitmapEncoderNoCache);
      if (SUCCEEDED(enc->CreateNewFrame(frame.put(), nullptr))) {
        frame->Initialize(nullptr); frame->SetSize(rc.right, rc.bottom);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR; frame->SetPixelFormat(&fmt);
        if (SUCCEEDED(frame->WriteSource(wb.p, nullptr))) { frame->Commit(); enc->Commit(); exitCode = 0; }
      }
    }
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(hwnd, wdc);
  }
  void failHeadless(const std::string& why) {
    std::wstring p = (probing() ? opt.probe : opt.shot) + L".error.txt";
    std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc); f << why << "\n";
    exitCode = 5; shellQuit(this);
  }
  // The read-only look (the one contact with a live deck this build is allowed): the phone-shaped hello
  // already carries the whole deck; attach ONE tile as a phone does, draw it, detach, go. No deskInit
  // (that would register a window and take the desk's saved layout), no shown, no layout, no resize,
  // no input, no call of any kind.
  std::string probeId;
  void startProbe() {
    if (!probeId.empty()) return;
    std::string want = opt.select.empty() ? "" : findTile(narrow(opt.select));
    if (want.empty() && meta.contains("tiles")) {
      // a tile nobody is showing and that is awake: attaching to it disturbs no view
      for (const auto& t : meta["tiles"]) {
        bool shownSomewhere = t.contains("shownBy") && t["shownBy"].is_array() && !t["shownBy"].empty();
        bool asleep = t.value("asleep", false);
        if (!shownSomewhere && !asleep && str(t, "status") != "off") { want = str(t, "id"); break; }
      }
    }
    probeId = want.empty() ? "-" : want;
    if (!want.empty()) {
      st.selected = want; st.shown.insert(want);
      TermView* v = ensureTerm(want);
      const json* t = tile(want);
      if (t && t->contains("ptySize") && (*t)["ptySize"].is_object()) { int c = (*t)["ptySize"].value("cols", 80), r = (*t)["ptySize"].value("rows", 24); if (c > 1 && r > 1) v->term.resize(c, r); }
      send(proto::attach(want));
    }
    SetTimer(hwnd, TIMER_PROBE, opt.after, nullptr);
  }
  void finishProbe() {
    if (probeId != "-" && !probeId.empty()) send(proto::detachAll());
    // the listing, beside the picture: what a remote sees of the deck (titles and states only)
    std::ofstream f((opt.probe + L".tiles.txt").c_str(), std::ios::binary | std::ios::trunc);
    if (meta.contains("tiles")) for (const auto& t : meta["tiles"]) f << str(t, "status") << "\t" << str(t, "kind") << "\t" << str(t, "account") << "\t" << tileTitle(t) << "\n";
    f.close();
    auto it = terms.find(st.selected);
    if (it != terms.end()) {   // show the pane at the pty's own size, the way a mirror does
      RectF pr; if (paneRect(st.selected, pr)) { float base = baseFontSize(); float fit = std::floor(pr.h / (it->second->term.rows() * 1.1f)); it->second->fontSize = std::max(6.f, std::min(base, fit > 0 ? fit : base)); }
    }
    saveShot(opt.probe);
    Sleep(150);
    stopTransport(); DestroyWindow(hwnd);
  }

  // ------------------------------------------------------------ panels: the palette, Elsewhere / Past, move-all,
  // the Special prompt form, Settings. Each is a model in ui.hpp drawn by sidebar.cpp (overlays.hpp); this is the
  // part the Electron renderer does in 88-palette.js, 45-elsewhere.js, 47-past.js, 60-move.js, 30-tiles.js
  // renderHoldForm and 50-settings.js: filling the model and answering its clicks and keys.
  ui::Palette pal;
  ui::PullMenu pull; json pullList = json::array(); std::vector<int> pullShown;
  ui::MoveAll mv;
  ULONGLONG pullBadgeAt = 0, pastBadgeAt = 0; bool pullBadgeOff = false, pastBadgeOff = false;

  static std::string lowerA(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
  static std::vector<std::string> wordsOf(const std::string& s) { std::vector<std::string> out; std::istringstream is(lowerA(s)); std::string w; while (is >> w) out.push_back(w); return out; }
  std::string shellName() const { std::string s = platform.is_object() ? str(platform, "shellName") : ""; return s.empty() ? "terminal" : s; }
  std::string acctLabelOf(const std::string& id) const {
    if (meta.contains("accounts") && meta["accounts"].is_array()) for (const auto& a : meta["accounts"]) if (a.is_object() && str(a, "id") == id) return str(a, "label");
    return id;
  }
  std::string keyHelp(const char* k, const char* dflt) const {
    if (platform.is_object() && platform.contains("keyHelp") && platform["keyHelp"].is_object()) { std::string v = str(platform["keyHelp"], k); if (!v.empty()) return v; }
    return dflt;
  }
  void reportToActive(const json& e) { if (!st.selected.empty()) report(st.selected, e); }

  // ---- the palette (88-palette.js): Ctrl+Shift+P; type to filter, arrows, Enter runs, Esc closes
  static ui::PalItem pi(const char* group, const std::wstring& label, const std::wstring& hint, std::function<void()> run) { ui::PalItem i; i.group = group; i.label = label; i.hint = hint; i.run = run; return i; }
  std::vector<ui::PalItem> paletteCommands() {
    std::vector<ui::PalItem> items;
    auto none = orSay();
    const json tiles = meta.contains("tiles") && meta["tiles"].is_array() ? meta["tiles"] : json::array();
    const json accounts = meta.contains("accounts") && meta["accounts"].is_array() ? meta["accounts"] : json::array();
    for (const auto& t : tiles) {
      const std::string id = str(t, "id"), status = str(t, "status");
      const std::string label = str(t, "kind") == "shell" ? shellName() : acctLabelOf(str(t, "account")) + " " + (str(t, "title").empty() ? "New session" : str(t, "title"));
      items.push_back(pi("sessions", widen(label), status == "busy" ? L"busy" : status == "waiting" ? L"waiting for you" : L"", [this, id] { activate(id, true); }));
    }
    for (const auto& a : accounts) { const std::string aid = str(a, "id"); items.push_back(pi("new", widen("New Claude session: " + str(a, "label")), widen(str(a, "email").empty() ? aid : str(a, "email")), [this, aid] { newTile({ { "kind", "claude" }, { "account", aid } }); })); }
    if (meta.value("browserOption", false)) for (const auto& a : accounts) { const std::string aid = str(a, "id"); items.push_back(pi("new", widen("New Claude session with the browser: " + str(a, "label")), widen(str(a, "email").empty() ? aid : str(a, "email")), [this, aid] { newTile({ { "kind", "claude" }, { "account", aid }, { "browser", true } }); })); }
    if (meta.value("cloudOption", false)) for (const auto& a : accounts) { const std::string aid = str(a, "id"); items.push_back(pi("new", widen("New cloud session: " + str(a, "label")), L"runs on claude.ai", [this, aid] { openCloudDialog(aid); })); }
    items.push_back(pi("new", widen("New " + shellName()), L"", [this] { newTile({ { "kind", "shell" } }); }));
    const json* curp = tile(st.selected);
    if (curp) {
      const json cur = *curp; const std::string cid = st.selected, ckind = str(cur, "kind");
      if (ckind == "claude") for (const auto& a : accounts) if (str(a, "id") != str(cur, "account")) { const std::string aid = str(a, "id"); items.push_back(pi("this session", widen("Open in " + str(a, "label")), L"confirm on the tile", [this, cid, aid] { st.confirm[cid] = { "move", aid, -1 }; invalidate(); })); }
      if (ckind == "cloud") {
        if (!str(cur, "cloudId").empty()) items.push_back(pi("this session", L"Reload the cloud session's page", L"", [this, cid, none] { redrawCloud(cid); call("refreshTile", json::array({ cid }), none); }));
        if (!str(cur, "cloudUrl").empty()) items.push_back(pi("this session", L"Open the cloud session in the browser", L"", [this, cid] { const json* t = tile(cid); if (t) openCloudOutside(*t); }));
        items.push_back(pi("this session", L"Remove the tile", L"the session keeps running", [this, cid, none] { call("closeTile", json::array({ cid }), none); }));
      } else {
        items.push_back(pi("this session", L"Refresh session", L"confirm on the tile", [this, cid] { st.confirm[cid] = { "refresh", "", -1 }; invalidate(); }));
        if (ckind == "claude" && cur.contains("browser")) {
          const bool isOn = cur["browser"].is_boolean() && cur["browser"].get<bool>();
          items.push_back(pi("this session", isOn ? L"Refresh session without the browser" : L"Refresh session with the browser", L"confirm on the tile", [this, cid, isOn] { st.confirm[cid] = { "refresh", "", isOn ? 0 : 1 }; invalidate(); }));
        }
        items.push_back(pi("this session", L"Close session", L"confirm on the tile", [this, cid] { st.confirm[cid] = { "close", "", -1 }; invalidate(); }));
        for (const PauseItem& p : pauseItems(cur)) { const std::string fn = p.fn; const json args = p.args; items.push_back(pi("this session", p.label + L" session", p.hint, [this, fn, args, none] { call(fn, args, none); })); }
        for (const AuditorItem& a : auditorItems(cur, viewW / 2 - 100, viewH / 3, true)) items.push_back(pi("this session", a.label, a.hint, a.run));   // auditors (88-palette.js)
      }
      // "Rename tile" is the tile's right-click menu (the palette sends the tile a contextmenu event)
      items.push_back(pi("this session", L"Rename tile", L"", [this, cid] { RectF r; if (sidebar.tileRect(cid, r)) openTileMenu(r.x, r.y, cid); else startRename(cid); }));
    }
    // the ask pane (88-palette.js, the four `asks` entries)
    const json askList = meta.contains("asks") && meta["asks"].is_array() ? meta["asks"] : json::array();
    if (!askList.empty()) items.push_back(pi("asks", widen("Answer every session's questions (" + std::to_string(askList.size()) + " waiting)"), L"the asks tile", [this] { openAsks("all"); }));
    const std::string curSid = curp ? str(*curp, "sessionId") : "";
    if (curp && str(*curp, "kind") == "claude" && askPane.hasAsk(curSid)) items.push_back(pi("asks", L"Answer this session's questions", widen(keyHelp("asks", "F7")), [this, curSid] { openAsks(curSid); }));
    for (const auto& a : askList) {
      const std::string sid = str(a, "sessionId");
      if (curp && sid == curSid) continue;
      items.push_back(pi("asks", widen("Answer " + askPane.sessionName(sid)), widen(std::to_string(askPane.openCount(sid)) + " waiting"), [this, sid] { openAsks(sid); }));
    }
    const size_t askLog = meta.contains("askLog") && meta["askLog"].is_array() ? meta["askLog"].size() : 0;
    if (askLog) items.push_back(pi("asks", L"Recently answered", widen("the last " + std::to_string(askLog) + ", read-only"), [this] { openAsks("all", asks::AskPane::RECENT); }));
    if (!deck::edition::kPublic) items.push_back(pi("deck", L"Move every session\x2026", L"", [this] { openMoveMenu(); }));   // the public build has no move-all (core/deck/edition.hpp)
    // Pause / Resume every session (main.js pauseAll / resumeAll; 88-palette.js)
    { bool canPause = false; int paused = 0;
      for (const auto& t : tiles) {
        if (str(t, "kind") != "claude" || (t.contains("exited") && t["exited"].is_boolean() && t["exited"].get<bool>())) continue;
        const json p = t.contains("pause") && t["pause"].is_object() ? t["pause"] : json();
        const std::string ps = p.is_object() ? str(p, "state") : std::string(), status = str(t, "status");
        const bool want = p.is_object() && p.contains("want") && p["want"].is_boolean() && p["want"].get<bool>();
        if (status != "asleep" && status != "off" && (ps.empty() || ps == "resuming")) canPause = true;
        if ((ps == "pausing" || ps == "paused") && !want) paused++;
      }
      if (canPause) {
        items.push_back(pi("deck", L"Pause every session", L"Esc, then the PAUSE prompt, in each", [this, none] { call("pauseAll", json::array({ json::object() }), none); }));
        items.push_back(pi("deck", L"Pause every session until restart", L"each resumed by itself at the next deck start", [this, none] { call("pauseAll", json::array({ json{ { "untilRestart", true } } }), none); }));
      }
      if (paused) items.push_back(pi("deck", widen("Resume every paused session (" + std::to_string(paused) + ")"), L"each when its turn ends", [this, none] { call("resumeAll", json::array(), none); }));
    }
    items.push_back(pi("deck", L"Search sessions", L"local", [this] { openSearch(); }));
    items.push_back(pi("deck", L"Settings", L"", [this] { openSettings(); }));
    // which deck this window is on (not in the Electron palette: its window IS its deck)
    if (localMode) {
      DeckLink known;
      if (cfg.link(known)) items.push_back(pi("deck", L"Connect to the remembered deck", widen(narrow(known.host) + " \xE2\x80\x94 this PC's terminals close"), [this, known] { switchDeck(false, &known); }));
      items.push_back(pi("deck", L"Connect to a deck\x2026", L"paste its link; this PC's terminals close", [this] { askDeckLink(); }));
    } else {
      items.push_back(pi("deck", L"Connect to another deck\x2026", L"paste its link", [this] { askDeckLink(); }));
      items.push_back(pi("deck", L"Use this PC (local preview)", L"terminal tiles only; leaves this deck", [this] { switchDeck(true, nullptr); }));
      // this PC's own choice (config.json), not the deck's settings
      if (copyOnSelectOn()) items.push_back(pi("deck", L"Copy on select: turn off", L"on: a selection is copied as you make it (this PC)", [this] { cfg.j["copyOnSelect"] = false; if (!opt.noSave) cfg.save(); note("Copy on select is off: Ctrl+C or right-click copies", 2500); }));
      else items.push_back(pi("deck", L"Copy on select: turn on", L"off: Ctrl+C or right-click copies (this PC)", [this] { cfg.j["copyOnSelect"] = true; if (!opt.noSave) cfg.save(); note("Copy on select is on", 2000); }));
    }
    // the exe's own update (update.hpp): from the public releases repo, only what the project's release key signed
    if (updateReady()) items.push_back(pi("deck", L"Restart to finish the update", L"the new version is in place; this starts it", [this] { restartForUpdate(); }));
    items.push_back(pi("deck", L"Check for updates", L"the signed releases (" DECK_EDITION_RELEASES_REPO L")", [this] { checkForUpdates(true); }));
    {   // 82-restart.js restartPaletteItems
      const json r = meta.contains("restart") ? meta["restart"] : json();
      if (r.is_object()) {
        const size_t n = r.contains("blockers") && r["blockers"].is_array() ? r["blockers"].size() : 0;
        const bool armed = r.value("armed", false), upd = r.contains("update") && !r["update"].is_null();
        if (armed) items.push_back(pi("deck", L"Cancel quiet restart", n ? widen("waiting on " + std::to_string(n)) : L"about to go", [this] { restartCancel(); }));
        else items.push_back(pi("deck", L"Restart deck when quiet", upd ? L"picks up the update" : L"", [this, upd, none] { call("restartWhenQuiet", json::array({ true, upd ? "update" : "manual" }), none); }));
        // Now past a blocker is confirmed on the strip, never straight from a keystroke
        items.push_back(pi("deck", n ? L"Restart deck now\x2026" : L"Restart deck now", n ? widen("stops " + std::to_string(n) + "; confirm on the strip") : L"nothing running", [this, n, armed, none] {
          if (!n) { call("restartNow", json::array(), none); return; }
          if (!armed) call("restartWhenQuiet", json::array({ true, "manual" }), none);
          nowConfirmUntil = GetTickCount64() + 6000; st.restartConfirming = true; invalidate();
        }));
      }
    }
    if (!L::isEmpty(layout) && zoom.empty()) items.push_back(pi("deck", L"Reset layout", L"one pane again", [this] { clearLayout(); }));
    items.push_back(pi("deck", st.sidebarOpen ? L"Hide sidebar" : L"Show sidebar", widen(keyHelp("sidebar", "F9")), [this] { setCollapsed(st.sidebarOpen); }));
    items.push_back(pi("deck", L"Toggle fullscreen", widen(keyHelp("fullscreen", "F10")), [this] { toggleFullscreen(); }));
    if (!zoom.empty()) items.push_back(pi("deck", L"Back to the grid", widen(keyHelp("zoom", "F6")), [this] { zoom.clear(); applyLayout(); }));
    else if (!st.selected.empty() && isPlaced(st.selected)) items.push_back(pi("deck", L"Zoom the selected pane", widen(keyHelp("zoom", "F6")), [this] { zoomToggle(st.selected); }));
    if (!st.selected.empty()) items.push_back(pi("deck", L"Redraw the selected pane", widen(keyHelp("redraw", "F5")), [this] { redrawTile(st.selected); }));
    items.push_back(pi("deck", st.showMsgs ? L"Hide recent messages" : L"Show recent messages", L"", [this] { st.showMsgs = !st.showMsgs; savePrefs(); clampScroll(); invalidate(); }));
    items.push_back(pi("deck", st.showBars ? L"Hide usage bars" : L"Show usage bars", L"", [this] { st.showBars = !st.showBars; savePrefs(); clampScroll(); invalidate(); }));
    return items;
  }
  void clearLayout() { if (zoomBlocks()) return; layout.clear(); applyLayout(); persistLayout(); }
  void renderPalette() {
    const std::vector<std::string> words = wordsOf(narrow(pal.input.text));
    pal.items.clear();
    for (auto& it : paletteCommands()) {
      const std::string hay = lowerA(narrow(it.label) + " " + narrow(it.hint) + " " + it.group);
      bool ok = true; for (const auto& w : words) if (hay.find(w) == std::string::npos) { ok = false; break; }
      if (ok) pal.items.push_back(it);
    }
    pal.index = std::min(pal.index, std::max(0, (int)pal.items.size() - 1));
    invalidate();
  }
  void openPalette() { pal.input.set(L""); pal.index = 0; pal.scroll = 0; pal.open = true; renderPalette(); focusEdit(&pal.input); }
  void closePalette() { if (!pal.open) return; pal.open = false; if (st.focusEdit == &pal.input) st.focusEdit = nullptr; focusTerminal(); }
  void runPalette(int i) { std::function<void()> run = i >= 0 && i < (int)pal.items.size() ? pal.items[i].run : nullptr; closePalette(); if (run) run(); }
  void togglePalette() { if (pal.open) closePalette(); else openPalette(); }

  // ---- "New cloud session" (48-cloud.js openCloudDialog): the task is the session's first prompt; the folder is
  // where `claude --cloud` runs on the DECK's machine. Opened from an account button's right-click menu and from the
  // palette. Start is `newTile({ kind: 'cloud', account, task, cwd })`: the deck checks it (main.js cloudSpec) and
  // what it refuses is written under the buttons. A test run never sends it (App::call: BLOCKED): no test may make a
  // cloud session.
  void openCloudDialog(const std::string& acctId) {
    if (notInLocal("Cloud sessions")) return;
    closeCloudDialog(); closeCtxMenu();
    cdlg = ui::CloudDlg();
    cdlg.account = acctId; cdlg.label = acctLabelOf(acctId);
    if (meta.contains("accounts") && meta["accounts"].is_array()) for (const auto& a : meta["accounts"]) if (a.is_object() && str(a, "id") == acctId) cdlg.email = str(a, "email");
    // the folder used last time on this machine (localStorage deck.cloud.cwd), else the deck's working folder
    std::string folder = cfg.j.value("cloudCwd", std::string());
    if (folder.empty()) { const json c = setting("cwd"); if (c.is_string()) folder = c.get<std::string>(); }
    cdlg.folder.set(widen(folder));
    cdlg.open = true;
    focusEdit(&cdlg.task);
    if (hwnd && !headless() && cloudPanes.webFocused()) SetFocus(hwnd);
    invalidate();
  }
  void closeCloudDialog() {
    if (!cdlg.open) return;
    if (st.focusEdit == &cdlg.task || st.focusEdit == &cdlg.folder) st.focusEdit = nullptr;
    cdlg.open = false; invalidate();
  }
  void cloudStart() {
    if (!cdlg.open || cdlg.starting) return;
    const std::wstring task = trim(cdlg.task.text);
    if (task.empty()) { cdlg.err = L"Say what it should do."; focusEdit(&cdlg.task); invalidate(); return; }
    cdlg.starting = true; cdlg.err.clear();
    const std::string folder = narrow(trim(cdlg.folder.text));
    const json spec = { { "kind", "cloud" }, { "account", cdlg.account }, { "task", narrow(task) }, { "cwd", folder } };
    call("newTile", json::array({ spec }), [this, folder](bool ok, const json& r) {
      if (!ok) { cloudFailed(errText(r)); return; }
      if (!opt.noSave) { cfg.j["cloudCwd"] = folder; cfg.save(); }
      closeCloudDialog();
      if (r.is_string()) { pendingNew = r.get<std::string>(); adoptPendingNew(); }   // createTerm(id); placeNew(id)
    });
    invalidate();
  }
  // the deck refused: its words under the buttons, and Start works again
  void cloudFailed(std::string why) {
    // ipcRenderer wraps a main-process throw: "Error invoking remote method 'tile:new': Error: <ours>"
    if (why.rfind("Error invoking remote method '", 0) == 0) { size_t q = why.find("': "); if (q != std::string::npos) { why = why.substr(q + 3); if (why.rfind("Error: ", 0) == 0) why = why.substr(7); } }
    if (!cdlg.open) { note(why, 5000); return; }
    cdlg.err = widen(why); cdlg.starting = false; invalidate();
  }
  // 40-new-tile.js: an account button's right click
  void openAcctMenu(float x, float y, const std::string& acctId) {
    if (claudeOffSays("Starting a Claude session")) return;
    const std::wstring label = widen(acctLabelOf(acctId));
    std::vector<ui::MenuItem> items;
    if (meta.value("browserOption", false)) items.push_back(mi(L"New " + label + L" session with the browser", [this, acctId] { newTile({ { "kind", "claude" }, { "account", acctId }, { "browser", true } }); }));
    // a new deck window is a window on the deck's own box: where none can open (bridge.js answers null) the session is placed here
    // LOCAL: 40-new-tile.js newSessionInNewWindow — the window 10 % in from this one's corner, on the same screen
    items.push_back(mi(L"New " + label + L" session in a new window", [this, acctId] { newTile({ { "kind", "claude" }, { "account", acctId } }, true); }));
    if (meta.value("cloudOption", false)) items.push_back(mi(L"New " + label + L" cloud session\x2026", [this, acctId] { openCloudDialog(acctId); }));
    showCtxMenu(x, y, items);
  }

  // ---- Elsewhere and Past (45-elsewhere.js, 47-past.js): one menu that grows up from the New terminal tile
  void closePull() { if (!pull.open) return; pull.open = false; pull.confirm = -1; if (st.focusEdit == &pull.filter) st.focusEdit = nullptr; invalidate(); }
  void togglePull(const std::string& which) {
    if (pull.open && pull.which == which) { closePull(); return; }
    closePull();
    if (which == "past") openPast(); else openElsewhere();
  }
  void openElsewhere() {
    call("externalSessions", json::array(), [this](bool ok, const json& list) {
      if (!ok) { refused("", list); return; }
      pullList = list.is_array() ? list : json::array();
      pull.which = "elsewhere"; pull.rows.clear(); pull.confirm = -1; pull.hover = -1; pull.scroll = 0; pullShown.clear();
      const long long now = nowMs();
      for (size_t i = 0; i < pullList.size(); i++) {
        const json& s = pullList[i]; ui::PullRow r;
        const std::string status = str(s, "status"), sid = str(s, "sessionId");
        r.label = widen(str(s, "label")); r.busy = status == "busy"; r.cloud = s.is_object() && s.value("cloud", false);
        r.title = widen(!str(s, "title").empty() ? str(s, "title") : !str(s, "name").empty() ? str(s, "name") : sid.substr(0, 8));
        const long long at = s.is_object() && s.contains("statusUpdatedAt") && s["statusUpdatedAt"].is_number() ? s["statusUpdatedAt"].get<long long>() : now;
        r.status = status == "busy" ? L"busy" : status == "waiting" ? L"waiting" : ui::fmtAgeMs((double)(now - at));
        pull.rows.push_back(r); pullShown.push_back((int)i);
      }
      pull.empty = pull.rows.empty() ? L"No Claude sessions running outside this window." : L"";
      st.elsewhereCount = (int)pullList.size();
      pull.open = true; invalidate();
    });
  }
  void renderPastList() {
    const std::vector<std::string> words = wordsOf(narrow(pull.filter.text));
    pull.rows.clear(); pullShown.clear(); pull.confirm = -1; pull.hover = -1; pull.scroll = 0;
    const long long now = nowMs();
    for (size_t i = 0; i < pullList.size() && pull.rows.size() < 60; i++) {
      const json& s = pullList[i];
      const std::string hay = lowerA(str(s, "label") + " " + str(s, "title") + " " + str(s, "account"));
      bool ok = true; for (const auto& w : words) if (hay.find(w) == std::string::npos) { ok = false; break; }
      if (!ok) continue;
      ui::PullRow r; r.label = widen(str(s, "label"));
      r.title = widen(!str(s, "title").empty() ? str(s, "title") : str(s, "sessionId").substr(0, 8));
      const long long mt = s.is_object() && s.contains("mtime") && s["mtime"].is_number() ? (long long)s["mtime"].get<double>() : now;
      r.status = ui::fmtAgeMs((double)(now - mt));
      pull.rows.push_back(r); pullShown.push_back((int)i);
    }
    pull.empty = pull.rows.empty() ? (pullList.empty() ? L"No past sessions found." : L"No match.") : L"";
    invalidate();
  }
  void openPast() {
    pullList = json::array(); pull.which = "past"; pull.rows.clear(); pullShown.clear(); pull.confirm = -1; pull.scroll = 0; pull.empty.clear();
    pull.filter.set(L""); pull.note = L"loading\x2026"; pull.open = true;
    focusEdit(&pull.filter);
    call("pastSessions", json::array(), [this](bool ok, const json& list) {
      if (pull.which != "past") return;
      if (!ok) { closePull(); refused("", list); return; }
      pullList = ok && list.is_array() ? list : json::array();
      st.pastCount = (int)pullList.size();
      pull.note = widen(std::to_string(pullList.size()) + " on disk");
      renderPastList();
    });
  }
  void pullAsk(int i) {
    if (i < 0 || i >= (int)pull.rows.size()) return;
    const json& s = pullList[pullShown[i]];
    pull.confirm = i;
    if (pull.which == "past") { pull.question = widen("Open this " + str(s, "label") + " session here?"); pull.yesLabel = L"Open here"; }
    else if (pull.rows[i].cloud) { pull.question = L"Open this cloud session here? It keeps running in the cloud."; pull.yesLabel = L"Open it here"; }
    else { pull.question = std::wstring(pull.rows[i].busy ? L"It is busy: the turn in progress will be lost. " : L"") + L"Close the copy in the other window and open it here?"; pull.yesLabel = L"Bring it here"; }
    invalidate();
  }
  void pullYes(int i) {
    if (i < 0 || i >= (int)pullShown.size()) return;
    const json s = pullList[pullShown[i]];
    const bool past = pull.which == "past";
    // (both start a Claude session here: with the launch off the deck would refuse — said before it is asked)
    if (claudeOffSays(past ? "Resuming a past session" : "Taking a session over")) { closePull(); return; }
    closePull();
    auto done = [this](bool ok, const json& r) { if (!ok) { refused("", r); return; } if (r.is_string()) { pendingNew = r.get<std::string>(); adoptPendingNew(); } };
    if (past) {
      json spec = { { "kind", "claude" }, { "account", str(s, "account") }, { "sessionId", str(s, "sessionId") } };
      if (!str(s, "cwd").empty()) spec["cwd"] = str(s, "cwd");
      call("newTile", json::array({ spec }), done);
    } else call("adoptSession", json::array({ s }), done);
  }
  // the buttons wear their count: "Elsewhere [2]" (checked every 10 s), "Past [40]" (the disk: every 60 s)
  void refreshBadges() {
    // (a test's local deck is not asked by itself: the counts would be whatever the disk holds when the shot is taken)
    if (!inited || probing() || (localMode && testRun())) return;
    const ULONGLONG now = GetTickCount64();
    // a count nobody asked for: a refusal here is not said out loud, and a deck that refuses the listing (the local
    // deck: HOST-NOTES.md) is not asked again every ten seconds. The button itself still says why when pressed.
    if (!pullBadgeOff && (!pullBadgeAt || now - pullBadgeAt >= 10000)) { pullBadgeAt = now; call("externalSessions", json::array(), [this](bool ok, const json& l) { if (!ok) pullBadgeOff = true; if (ok && l.is_array()) { st.elsewhereCount = (int)l.size(); invalidate(); } }); }
    if (!pastBadgeOff && (!pastBadgeAt || now - pastBadgeAt >= 60000)) { pastBadgeAt = now; call("pastSessions", json::array(), [this](bool ok, const json& l) { if (!ok) pastBadgeOff = true; if (ok && l.is_array()) { st.pastCount = (int)l.size(); invalidate(); } }); }
  }

  // ---- move every session (60-move.js): the button left of the gear
  std::vector<std::string> accountIds() const { std::vector<std::string> out; if (meta.contains("accounts") && meta["accounts"].is_array()) for (const auto& a : meta["accounts"]) if (a.is_object()) out.push_back(str(a, "id")); return out; }
  std::vector<std::string> claudeTilesOf(const std::string& acct) const { std::vector<std::string> out; if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"]) if (t.is_object() && str(t, "kind") == "claude" && str(t, "account") == acct) out.push_back(str(t, "id")); return out; }
  void openMoveMenu() {
    const std::vector<std::string> ids = accountIds();
    // default: the account with the most open tiles -> the first other account
    std::vector<std::pair<std::string, int>> counts;
    if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"]) if (t.is_object() && str(t, "kind") == "claude") {
      const std::string a = str(t, "account"); bool found = false;
      for (auto& c : counts) if (c.first == a) { c.second++; found = true; }
      if (!found) counts.push_back({ a, 1 });
    }
    std::stable_sort(counts.begin(), counts.end(), [](const std::pair<std::string, int>& x, const std::pair<std::string, int>& y) { return x.second > y.second; });
    mv = ui::MoveAll();
    mv.from = !counts.empty() && std::find(ids.begin(), ids.end(), counts[0].first) != ids.end() ? counts[0].first : ids.empty() ? "" : ids[0];
    mv.to = ids.empty() ? "" : ids[0];
    for (const auto& id : ids) if (id != mv.from) { mv.to = id; break; }
    mv.cont = cfg.j.value("movecontinue", false);       // ships off: it types into sessions
    mv.advanced = cfg.j.value("moveadvanced", false);
    mv.open = true; invalidate();
  }
  void closeMoveMenu() { if (mv.open) { mv.open = false; invalidate(); } }
  void toggleMoveAll() { if (mv.open) closeMoveMenu(); else openMoveMenu(); }
  void moveSelect(const std::string& which, const RectF& r) {
    std::vector<ui::MenuItem> items;
    for (const auto& id : accountIds()) {
      const bool from = which == "from";
      std::wstring label = widen(acctLabelOf(id)) + (from ? L" (" + std::to_wstring(claudeTilesOf(id).size()) + L")" : L"");
      items.push_back(mi(label, [this, id, from] { if (from) { if (mv.from != id) mv.picked.clear(); mv.from = id; } else mv.to = id; invalidate(); }, (from ? mv.from : mv.to) == id));
    }
    showCtxMenu(r.x, r.b(), items, "select");
  }
  void moveGo() {
    std::vector<std::string> all = claudeTilesOf(mv.from), only;
    if (mv.advanced) for (const auto& id : all) { auto it = mv.picked.find(id); if (it == mv.picked.end() || it->second) only.push_back(id); }
    const size_t n = mv.advanced ? only.size() : all.size();
    mv.asking = false;
    if (mv.from == mv.to) { mv.status = L"Pick two different accounts."; return; }
    if (!n) { mv.status = mv.advanced ? L"No sessions ticked." : widen("No " + acctLabelOf(mv.from) + " sessions are open here."); return; }
    mv.question = widen("Move " + std::to_string(n) + " " + acctLabelOf(mv.from) + " session" + (n == 1 ? "" : "s") + " to " + acctLabelOf(mv.to) + "? Busy ones lose the turn in progress. ");
    mv.asking = true; mv.status.clear();
  }
  void moveYes() {
    std::vector<std::string> all = claudeTilesOf(mv.from), only;
    if (mv.advanced) for (const auto& id : all) { auto it = mv.picked.find(id); if (it == mv.picked.end() || it->second) only.push_back(id); }
    const bool cont = mv.cont;
    mv.asking = false; mv.status = L"Moving\x2026";
    call("moveAll", json::array({ mv.from, mv.to, cont, mv.advanced ? json(only) : json() }), [this, cont](bool ok, const json& r) {
      if (!ok) { mv.status = widen("Failed: " + (r.is_string() ? r.get<std::string>() : r.dump())); invalidate(); return; }
      std::string s = "Moved " + std::to_string(r.is_object() ? r.value("moved", 0) : 0) + ".";
      const int us = r.is_object() ? r.value("usageSessions", 0) : 0;
      if (cont && us) s += " \"continue\" queued for " + std::to_string(us) + ".";
      if (r.is_object() && r.contains("errors") && r["errors"].is_array() && !r["errors"].empty()) { s += " Failed: "; bool first = true; for (const auto& e : r["errors"]) { if (!first) s += "; "; first = false; s += e.is_string() ? e.get<std::string>() : e.dump(); } }
      mv.status = widen(s); invalidate();
    });
    if (headless()) mv.status = L"Moving\x2026";
  }

  // ---- the Special prompt form (30-tiles.js renderHoldForm)
  // The Electron form's time box is <input type="datetime-local">: its VALUE is `YYYY-MM-DDTHH:MM` (local time, to
  // the minute: the default step of 60 s, no min, no max), which Chromium on an en-US box SHOWS as
  // `MM/DD/YYYY, hh:mm AM`; an entry that is not a whole, real date and time has the value '' and Date.parse('')
  // is NaN, so Add answers "that time has passed". The two <input type="time"> hold `HH:MM` (24 h) and show
  // `hh:mm AM`. Here they are text boxes: they show what Chromium shows, take that or the value format, and an
  // entry the browser's box could not hold is "no value", exactly as there.
  static std::wstring localStamp(long long ms, bool withDate) {
    time_t t = (time_t)(ms / 1000); struct tm tmv{}; localtime_s(&tmv, &t);
    wchar_t b[48]; const int h12 = tmv.tm_hour % 12 == 0 ? 12 : tmv.tm_hour % 12; const wchar_t* ap = tmv.tm_hour < 12 ? L"AM" : L"PM";
    if (withDate) swprintf(b, 48, L"%02d/%02d/%04d, %02d:%02d %ls", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900, h12, tmv.tm_min, ap);
    else swprintf(b, 48, L"%02d:%02d %ls", h12, tmv.tm_min, ap);
    return b;
  }
  // "hh:mm AM" / "hh:mm PM" (1-12) or "HH:MM" (0-23) -> minutes of the day; -1 = not a time the box could hold
  static int clockOf(const std::wstring& v0) {
    const std::wstring v = trim(v0);
    int h = 0, m2 = 0, used = 0; wchar_t ap[8] = L"";
    if (swscanf(v.c_str(), L"%2d:%2d%n", &h, &m2, &used) < 2 || used < 3) return -1;
    { const size_t colon = v.find(L':'); if (colon == std::wstring::npos || (size_t)used - colon - 1 != 2) return -1; }   // minutes are two digits
    std::wstring rest = trim(v.substr((size_t)used));
    if (m2 < 0 || m2 > 59 || h < 0) return -1;
    if (rest.empty()) return h <= 23 ? h * 60 + m2 : -1;
    for (auto& c : rest) c = (wchar_t)towupper(c);
    if (rest != L"AM" && rest != L"PM" && rest != L"A.M." && rest != L"P.M.") return -1;
    if (h < 1 || h > 12) return -1;
    (void)ap;
    return ((h % 12) + (rest[0] == L'P' ? 12 : 0)) * 60 + m2;
  }
  // `MM/DD/YYYY, hh:mm AM` (the comma may be left out) or `YYYY-MM-DDTHH:MM` (a space for the T is fine), local
  // -> ms; 0 = no value (not a whole real date and time: 02/30, 13 o'clock PM, seconds, anything else)
  static long long parseStamp(const std::wstring& s0) {
    const std::wstring s = trim(s0);
    int y = 0, mo = 0, d = 0, used = 0; std::wstring clock;
    if (swscanf(s.c_str(), L"%2d/%2d/%6d%n", &mo, &d, &y, &used) == 3 && used > 0) {
      clock = trim(s.substr((size_t)used)); if (!clock.empty() && clock[0] == L',') clock = trim(clock.substr(1));
      if (clock.find_first_of(L"aApP") == std::wstring::npos) return 0;      // the shown format always has its AM / PM field
    } else if (swscanf(s.c_str(), L"%6d-%2d-%2d%n", &y, &mo, &d, &used) == 3 && used > 0 && (size_t)used < s.size() && (s[(size_t)used] == L'T' || s[(size_t)used] == L't' || s[(size_t)used] == L' ')) {
      clock = trim(s.substr((size_t)used + 1));
      if (clock.find_first_of(L"aApP") != std::wstring::npos) return 0;      // the value format is 24 h
    } else return 0;
    const int mins = clockOf(clock);
    if (mins < 0 || y < 1 || y > 275760 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    struct tm tmv{}; tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d; tmv.tm_hour = mins / 60; tmv.tm_min = mins % 60; tmv.tm_isdst = -1;
    const time_t t = mktime(&tmv);
    if (t == (time_t)-1 || tmv.tm_mon != mo - 1 || tmv.tm_mday != d) return 0;   // (mktime rolled a day that does not exist into the next month)
    return (long long)t * 1000;
  }
  // 30-tiles.js minutesOf(input.value): the time of day, 0 when the box holds no time
  static int minutesOf(const std::wstring& v) { const int m2 = clockOf(v); return m2 < 0 ? 0 : m2; }
  void openHoldForm(const std::string& id) {
    const json* t = tile(id); if (!t) return;
    st.confirm[id] = { "hold", "", -1 };
    hold.tile = id; hold.kind = "next"; hold.err.clear(); hold.picks.clear();
    hold.text.set(L""); hold.text.multiline = true; hold.every.set(L"30"); hold.from.set(L"09:00 AM"); hold.to.set(L"05:00 PM"); hold.trig.set(L"");
    // the timed pane's picks: the account's reset times + 5 minutes, then "in 1 h"
    const long long now = nowMs(), AFTER = 5 * 60 * 1000;
    const json u = meta.contains("usage") && meta["usage"].is_object() && meta["usage"].contains(str(*t, "account")) ? meta["usage"][str(*t, "account")] : json::object();
    auto addPick = [&](const std::string& label, const json& seg) {
      double ms = 0; if (!seg.is_object() || !seg.contains("resetsAt") || !ui::parseTimeMs(seg["resetsAt"], ms) || ms <= (double)now) return;
      hold.picks.push_back({ widen(label), (long long)ms + AFTER });
    };
    if (u.is_object()) {
      if (u.contains("session")) addPick("session reset +5m", u["session"]);
      if (u.contains("week")) addPick("week reset +5m", u["week"]);
      if (u.contains("scoped") && u["scoped"].is_array()) for (const auto& sc : u["scoped"]) addPick((str(sc, "model").empty() ? "model" : str(sc, "model")) + " reset +5m", sc);
    }
    hold.picks.push_back({ L"in 1 h", now + 3600 * 1000 });
    for (auto& p : hold.picks) p.label += L" " + ui::fmtHeldTimeMs((double)p.ms, (double)now);
    hold.time.set(localStamp(hold.picks[0].ms, true));
    hold.pop = 0; hold.trigIndex = -1; hold.trigHidden = false;
    hold.trig.onChange = [this] { hold.trigHidden = false; hold.trigIndex = -1; invalidate(); };
    st.hold = &hold;
    focusEdit(&hold.text);
  }
  void closeHoldForm() {
    for (EditBox* e : { &hold.text, &hold.time, &hold.every, &hold.from, &hold.to, &hold.trig }) if (st.focusEdit == e) st.focusEdit = nullptr;
    hold.tile.clear(); hold.pop = 0; st.hold = nullptr; invalidate();
  }
  // which of the form's popups shows now (ui.hpp HoldForm::pop): 1 the calendar (opened), 2 the trigger list (while the
  // trigger box has the focus and some known trigger matches, unless Esc put it away)
  int holdPopNow() const {
    if (!st.hold || hold.tile.empty()) return 0;
    if (hold.pop == 1 && hold.kind == "timed") return 1;
    if (hold.kind == "triggered" && st.focusEdit == &hold.trig && !hold.trigHidden && !hold.trigList.empty()) return 2;
    return 0;
  }
  void openHoldCalendar() {
    long long ms = parseStamp(hold.time.text); if (ms <= 0) ms = nowMs();
    time_t t = (time_t)(ms / 1000); struct tm tmv{}; localtime_s(&tmv, &t);
    hold.calY = tmv.tm_year + 1900; hold.calM = tmv.tm_mon; hold.pop = 1;
    if (testRun()) logLine("hold calendar open " + std::to_string(hold.calY) + "-" + std::to_string(hold.calM + 1));
    invalidate();
  }
  // a day picked on the calendar: that date in the box, its time of day kept (none in the box: the time is left
  // blank, "--:-- --", as Chromium's box shows a date with no time)
  void holdPopClick(const std::string& id) {
    if (id == "prev" || id == "next") { hold.calM += id == "next" ? 1 : -1; if (hold.calM < 0) { hold.calM = 11; hold.calY--; } else if (hold.calM > 11) { hold.calM = 0; hold.calY++; } invalidate(); return; }
    int y = 0, mo = 0, d = 0; if (sscanf(id.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return;
    const long long had = parseStamp(hold.time.text);
    wchar_t b[48];
    if (had > 0) {
      time_t t = (time_t)(had / 1000); struct tm tmv{}; localtime_s(&tmv, &t);
      const int h12 = tmv.tm_hour % 12 == 0 ? 12 : tmv.tm_hour % 12;
      swprintf(b, 48, L"%02d/%02d/%04d, %02d:%02d %ls", mo, d, y, h12, tmv.tm_min, tmv.tm_hour < 12 ? L"AM" : L"PM");
    } else swprintf(b, 48, L"%02d/%02d/%04d, --:-- --", mo, d, y);
    hold.time.set(b); hold.err.clear(); hold.pop = 0;
    if (testRun()) logLine("hold calendar picked " + id + " -> " + narrow(hold.time.text));
    invalidate();
  }
  void pickHoldTrigger(const std::string& name) {
    hold.trig.set(widen(name)); hold.err.clear(); hold.trigHidden = true; hold.trigIndex = -1;
    if (testRun()) logLine("hold trigger picked " + name);
    invalidate();
  }
  void holdClick(const ui::Hit& h) {
    const std::string id = h.id, a = h.arg;
    if (hold.tile != id) return;
    if (a.rfind("kind:", 0) == 0) { hold.kind = a.substr(5); hold.pop = 0; invalidate(); return; }
    if (a == "cal") { if (hold.pop == 1) { hold.pop = 0; invalidate(); } else openHoldCalendar(); return; }
    if (a.rfind("trig:", 0) == 0) { pickHoldTrigger(a.substr(5)); return; }   // a name from the list of known triggers
    if (a.rfind("pick:", 0) == 0) { size_t i = (size_t)atoi(a.c_str() + 5); if (i < hold.picks.size()) hold.time.set(localStamp(hold.picks[i].ms, true)); invalidate(); return; }
    if (a == "cancel") { confirmNo(id); return; }
    if (a != "add") return;
    if (trim(hold.text.text).empty()) { hold.err = L"nothing to send"; invalidate(); return; }
    json spec = { { "kind", hold.kind }, { "text", narrow(hold.text.text) } };
    if (hold.kind == "timed") { const long long at = parseStamp(hold.time.text); if (!(at > nowMs())) { hold.err = L"that time has passed"; invalidate(); return; } spec["at"] = at; }
    else if (hold.kind == "recurring") { { const double ev = wcstod(hold.every.text.c_str(), nullptr); if (ev == std::floor(ev)) spec["everyMin"] = (long long)ev; else spec["everyMin"] = ev; } /* `+every.value` */ spec["startMin"] = minutesOf(hold.from.text); spec["endMin"] = minutesOf(hold.to.text); }
    else if (hold.kind == "triggered") { if (trim(hold.trig.text).empty()) { hold.err = L"name a trigger"; invalidate(); return; } spec["trigger"] = narrow(trim(hold.trig.text)); }
    call("holdAdd", json::array({ id, spec }), [this, id](bool ok, const json& e) {
      if (ok) { st.confirm.erase(id); if (hold.tile == id) closeHoldForm(); }
      else { hold.err = widen(e.is_string() ? e.get<std::string>() : e.dump()); }
      invalidate();
    });
  }

  // ---- what the app asks of every panel
  void initPanels() {
    pal.input.onChange = [this] { pal.index = 0; pal.scroll = 0; renderPalette(); };
    pull.filter.onChange = [this] { if (pull.which == "past") renderPastList(); };
    initSettings();
  }
  EditBox* panelEdit(const std::string& id) {
    if (id == "palette") return &pal.input;
    if (id == "past-filter") return &pull.filter;
    if (id == "hold-text") return &hold.text;
    if (id == "hold-time") return &hold.time;
    if (id == "hold-every") return &hold.every;
    if (id == "hold-from") return &hold.from;
    if (id == "hold-to") return &hold.to;
    if (id == "hold-trig") return &hold.trig;
    if (id == "cdlg-task") return &cdlg.task;
    if (id == "cdlg-folder") return &cdlg.folder;
    if (id == "cp-hex") return &cpick.hex;
    return settingsEdit(id);
  }
  bool panelEditKey(EditBox* e, UINT vk, const Mods& m) {
    if (e == &hold.trig && holdPopNow() == 2) {         // the <datalist> drop-down: arrows, Enter, Esc
      const int n = (int)hold.trigList.size();
      if (vk == VK_DOWN) { hold.trigIndex = std::min(n - 1, hold.trigIndex + 1); invalidate(); return true; }
      if (vk == VK_UP) { hold.trigIndex = std::max(0, hold.trigIndex - 1); invalidate(); return true; }
      if (vk == VK_RETURN && hold.trigIndex >= 0 && hold.trigIndex < n) { pickHoldTrigger(hold.trigList[(size_t)hold.trigIndex]); return true; }
      if (vk == VK_ESCAPE) { hold.trigHidden = true; hold.trigIndex = -1; invalidate(); return true; }
    }
    if (e == &hold.time) {                              // Alt+Down opens the calendar (Chromium's key for a date picker); Esc shuts it
      if (vk == VK_DOWN && m.alt) { openHoldCalendar(); return true; }
      if (vk == VK_ESCAPE && hold.pop == 1) { hold.pop = 0; invalidate(); return true; }
    }
    if (e == &cpick.hex) {                              // Enter keeps the colour, Esc puts it back
      if (vk == VK_ESCAPE) { closeColorPick(true); return true; }
      if (vk == VK_RETURN) { closeColorPick(false); return true; }
      return false;
    }
    if (e == &cdlg.task || e == &cdlg.folder) {          // 48-cloud.js: the box's own keydown
      if (vk == VK_ESCAPE) { closeCloudDialog(); return true; }
      if (vk == VK_RETURN && m.ctrl) { cloudStart(); return true; }
      if (vk == VK_TAB && !m.ctrl && !m.alt) { focusEdit(e == &cdlg.task ? &cdlg.folder : &cdlg.task); return true; }   // the browser's own Tab order, between the two boxes
      return false;
    }
    if (e == &pal.input) {
      if (vk == VK_ESCAPE) { closePalette(); return true; }
      if (vk == VK_DOWN) { pal.index = std::min((int)pal.items.size() - 1, pal.index + 1); if (pal.index < 0) pal.index = 0; invalidate(); return true; }
      if (vk == VK_UP) { pal.index = std::max(0, pal.index - 1); invalidate(); return true; }
      if (vk == VK_RETURN) { runPalette(pal.index); return true; }
      return false;
    }
    if (e == &pull.filter) { if (vk == VK_ESCAPE) { closePull(); return true; } return false; }
    return settingsEditKey(e, vk, m);
  }
  bool anyPanelOpen() const { return pal.open || pull.open || mv.open || settingsOpen() || cdlg.open; }
  bool panelKey(UINT vk, const Mods& m) {
    if (vk == VK_ESCAPE && hold.pop == 1 && st.hold) { hold.pop = 0; invalidate(); return true; }   // the calendar first
    if (cpick.open) {                                   // the picker first: Esc puts the colour back, Enter keeps it
      if (vk == VK_ESCAPE) { closeColorPick(true); return true; }
      if (vk == VK_RETURN) { closeColorPick(false); return true; }
    }
    if (cdlg.open) {                                    // the dialog is modal: its keys with no box focused, and nothing else
      if (vk == VK_ESCAPE) closeCloudDialog();
      else if (vk == VK_RETURN && m.ctrl) cloudStart();
      else if (vk == VK_TAB) focusEdit(&cdlg.task);
      return true;
    }
    if (pal.open) { if (vk == VK_ESCAPE) closePalette(); return true; }     // the palette is modal
    if (vk == VK_ESCAPE) {                              // 50-settings.js: Esc closes the settings panel, else the move menu
      if (sset.open) { closeSettings(); return true; }
      if (mv.open) { closeMoveMenu(); return true; }
    }
    return false;
  }
  static const ui::OZone* ozAt(const std::vector<ui::OZone>& v, float x, float y) { for (auto it = v.rbegin(); it != v.rend(); ++it) if (it->rect.has(x, y)) return &*it; return nullptr; }
  static void ozHit(const ui::OZone& z, ui::Hit& h) { h.kind = z.kind; h.id = z.id; h.arg = z.arg; h.rect = z.rect; }
  // above the context menu: the palette (it covers the window)
  bool panelHit(float x, float y, ui::Hit& h) {
    if (holdPopNow() && hold.popBox.has(x, y)) { const ui::OZone* z = ozAt(hold.popZones, x, y); if (z) ozHit(*z, h); else h.kind = "panel"; return true; }
    if (cpick.open) {                                   // a popup over everything: a click outside closes it (and does no more)
      if (cpick.box.has(x, y)) { const ui::OZone* z = ozAt(cpick.zones, x, y); if (z) ozHit(*z, h); else h.kind = "panel"; return true; }
      h.kind = "cp-back"; return true;
    }
    if (cdlg.open) {                                    // css z-index 300: over the palette too, and over the whole window
      if (cdlg.box.has(x, y)) { const ui::OZone* z = ozAt(cdlg.zones, x, y); if (z) ozHit(*z, h); else h.kind = "panel"; return true; }
      h.kind = "cdlg-back"; return true;
    }
    if (!pal.open) return false;
    for (size_t i = 0; i < pal.rows.size(); i++) if (pal.rows[i].w > 0 && pal.rows[i].has(x, y)) { h.kind = "pal"; h.id = std::to_string(i); return true; }
    if (pal.box.has(x, y)) { if (y < pal.box.y + 40) { h.kind = "edit"; h.id = "palette"; } else h.kind = "panel"; return true; }
    h.kind = "pal-back"; return true;
  }
  // under the context menu, over the sidebar: the menus that hang off the bottom block, and Settings
  bool panelHitUnder(float x, float y, ui::Hit& h) {
    if (mv.open && mv.box.has(x, y)) { const ui::OZone* z = ozAt(mv.zones, x, y); if (z) ozHit(*z, h); else h.kind = "panel"; return true; }
    if (pull.open && pull.box.has(x, y)) { const ui::OZone* z = ozAt(pull.zones, x, y); if (z) ozHit(*z, h); else h.kind = "panel"; return true; }
    return settingsHit(x, y, h);
  }
  void panelHover(const ui::Hit& h) {
    { const std::string hv = cdlg.open && (h.kind == "cdlg-go" || h.kind == "cdlg-cancel") ? h.kind : std::string(); if (hv != cdlg.hover) { cdlg.hover = hv; invalidate(); } }
    if (pal.open && h.kind == "pal") { int i = atoi(h.id.c_str()); if (i != pal.index) { pal.index = i; invalidate(); } }
    int ph = h.kind == "pull-row" ? atoi(h.id.c_str()) : -1;
    if (ph != pull.hover) { pull.hover = ph; invalidate(); }
  }
  bool panelCovers(float x, float y) { return cpick.open || (holdPopNow() && hold.popBox.has(x, y)) || cdlg.open || pal.open || (mv.open && mv.box.has(x, y)) || (pull.open && pull.box.has(x, y)) || settingsCovers(x, y); }
  std::wstring panelTip(const ui::Hit& h, float x, float y) { return settingsTip(h, x, y); }
  // true = the click was a panel's. A click outside an open menu closes it (and is still a click on what is there).
  bool panelClick(const ui::Hit& h, float x, float y, int count) {
    const std::string& k = h.kind;
    if (cpick.open) { if (k == "cp-back") closeColorPick(false); return true; }
    if (k == "holdpop") { holdPopClick(h.id); return true; }
    if (k == "holdtrig") { pickHoldTrigger(h.id); return true; }
    if (cdlg.open) {
      if (k == "cdlg-go") cloudStart();
      else if (k == "cdlg-cancel" || k == "cdlg-back") closeCloudDialog();     // (the js closes on the backdrop's mousedown)
      return true;
    }
    if (pal.open) {
      if (k == "pal") runPalette(atoi(h.id.c_str()));
      else if (k == "pal-back") closePalette();
      return true;
    }
    if (pull.open) {
      if (k == "pull-row") { pullAsk(atoi(h.id.c_str())); return true; }
      if (k == "pull-yes") { pullYes(atoi(h.id.c_str())); return true; }
      if (k == "pull-no") { pull.confirm = -1; invalidate(); return true; }
      if (pull.box.has(x, y)) return true;
      if (k != "elsewhere" && k != "past") closePull();
    }
    if (mv.open) {
      if (k == "move-mode") { mv.advanced = h.id == "these"; if (!opt.noSave) { cfg.j["moveadvanced"] = mv.advanced; cfg.save(); } invalidate(); return true; }
      if (k == "move-select") { moveSelect(h.id, h.rect); return true; }
      if (k == "move-pick") { auto it = mv.picked.find(h.id); const bool cur = it == mv.picked.end() ? true : it->second; mv.picked[h.id] = !cur; invalidate(); return true; }
      if (k == "move-continue") { mv.cont = !mv.cont; if (!opt.noSave) { cfg.j["movecontinue"] = mv.cont; cfg.save(); } invalidate(); return true; }
      if (k == "move-go") { moveGo(); invalidate(); return true; }
      if (k == "move-yes") { moveYes(); invalidate(); return true; }
      if (k == "move-no") { mv.asking = false; mv.status.clear(); invalidate(); return true; }
      if (mv.box.has(x, y)) return true;
      if (!(k == "tool" && h.id == "moveall-btn")) closeMoveMenu();
    }
    return settingsClick(h, x, y, count);
  }
  bool panelRightClick(const ui::Hit& h, float, float) { return cdlg.open || pal.open || h.kind == "panel" || h.kind.rfind("move-", 0) == 0 || h.kind.rfind("pull-", 0) == 0 || h.kind.rfind("set-", 0) == 0; }
  bool panelWheel(const ui::Hit&, float x, float y, int delta) {
    const float d = delta / 120.f * 60;
    if (cdlg.open) { if (cdlg.task.rect.has(x, y) || (y >= cdlg.task.rect.y - 7 && y < cdlg.task.rect.b() + 7 && cdlg.box.has(x, y))) { cdlg.task.scrollBy(-d); invalidate(); } return true; }
    if (pal.open) { if (pal.box.has(x, y)) { pal.scroll -= d; invalidate(); } return true; }
    if (pull.open && pull.box.has(x, y)) { pull.scroll -= d; if (pull.scroll < 0) pull.scroll = 0; invalidate(); return true; }
    if (mv.open && mv.box.has(x, y)) { mv.pickScroll -= d; if (mv.pickScroll < 0) mv.pickScroll = 0; invalidate(); return true; }
    return settingsWheel(x, y, d);
  }
  bool panelZone(const std::string& kind, const std::string& id, RectF& r) {
    if (cpick.open) { for (const auto& z : cpick.zones) if (z.kind == kind && (id == "-" || z.id == id)) { r = z.rect; return true; } }
    if (holdPopNow()) { for (const auto& z : hold.popZones) if (z.kind == kind && (id == "-" || z.id == id)) { r = z.rect; return true; } }
    if (cdlg.open) { for (const auto& z : cdlg.zones) if (z.kind == kind && (id == "-" || z.id == id)) { r = z.rect; return true; } return false; }
    if (pal.open) {
      if (kind == "pal") { const std::wstring want = widen(id); for (size_t i = 0; i < pal.items.size() && i < pal.rows.size(); i++) if (pal.rows[i].w > 0 && pal.items[i].label.compare(0, want.size(), want) == 0) { r = pal.rows[i]; return true; } }
      return false;
    }
    const std::vector<ui::OZone>* lists[3] = { &mv.zones, &pull.zones, settingsZones() };
    for (const std::vector<ui::OZone>* v : lists) {
      if (!v || (v == &mv.zones && !mv.open) || (v == &pull.zones && !pull.open)) continue;
      for (const auto& z : *v) if (z.kind == kind && (id == "-" || z.id == id)) { r = z.rect; return true; }
    }
    return false;
  }
  void panelDump(std::ostringstream& o) {
    auto rc = [](const RectF& r) { char b[96]; snprintf(b, sizeof b, "[%.0f,%.0f %.0fx%.0f]", r.x, r.y, r.w, r.h); return std::string(b); };
    if (st.hold && !hold.tile.empty()) {
      o << "hold form " << hold.kind << " time '" << narrow(hold.time.text) << "' trigger '" << narrow(hold.trig.text) << "' popup " << holdPopNow();
      if (holdPopNow() == 1) o << " month " << hold.calY << "-" << (hold.calM + 1);
      o << "\n"; if (holdPopNow()) for (auto& z : hold.popZones) o << "  " << z.kind << " " << z.id << " " << rc(z.rect) << "\n";
    }
    if (cpick.open) { char b[96]; snprintf(b, sizeof b, " h %.0f s %.2f v %.2f", cpick.h, cpick.s, cpick.v); o << "color picker " << cpick.key << " " << rc(cpick.box) << b << " value " << sset.colors[cpick.key] << " hex '" << narrow(cpick.hex.text) << "'\n"; for (auto& z : cpick.zones) o << "  " << z.kind << " " << z.id << " " << rc(z.rect) << "\n"; }
    if (cdlg.open) {
      std::string task = narrow(cdlg.task.text); for (size_t at; (at = task.find('\n')) != std::string::npos;) task.replace(at, 1, "\\n");
      o << "cloud dialog " << rc(cdlg.box) << " account " << cdlg.account << " '" << cdlg.label << "' starting " << cdlg.starting << " err '" << narrow(cdlg.err) << "' focus " << (st.focusEdit == &cdlg.task ? "task" : st.focusEdit == &cdlg.folder ? "folder" : "-")
        << "\n  task '" << task << "' caret " << cdlg.task.caret << " scrollY " << cdlg.task.scrollY << "\n  folder '" << narrow(cdlg.folder.text) << "'\n";
      for (auto& z : cdlg.zones) o << "  " << z.kind << " " << z.id << " " << rc(z.rect) << "\n";
    }
    if (pal.open) { o << "palette " << rc(pal.box) << " index " << pal.index << "\n"; for (auto& it : pal.items) o << "  " << it.group << " | " << narrow(it.label) << " | " << narrow(it.hint) << "\n"; }
    if (pull.open) { o << "pull " << pull.which << " " << rc(pull.box) << " rows " << pull.rows.size() << " note '" << narrow(pull.note) << "' empty '" << narrow(pull.empty) << "'\n"; for (auto& z : pull.zones) o << "  " << z.kind << " " << z.id << " " << rc(z.rect) << "\n"; }
    if (mv.open) { o << "moveall " << rc(mv.box) << " from " << mv.from << " to " << mv.to << " adv " << mv.advanced << " status '" << narrow(mv.status) << "' asking " << mv.asking << " '" << narrow(mv.question) << "'\n"; for (auto& z : mv.zones) o << "  " << z.kind << " " << z.id << " " << rc(z.rect) << "\n"; }
    settingsDump(o);
  }
  void panelTick() { refreshBadges(); }
  void panelMeta() { if (pal.open) renderPalette(); settingsMeta(); }
  void drawPanels() {
    if (!st.sidebarOpen) return;
    const RectF sr = sidebarRect();
    drawSettings(sr);
    if (pull.open) ui::drawPullMenu(gfx, meta, st, pull, sr, sidebar.bottomTop());
    if (mv.open) ui::drawMoveAll(gfx, meta, st, mv, sr);
  }
  void drawPanelsTop() {
    if (pal.open) ui::drawPalette(gfx, meta, st, pal, viewW, viewH);
    if (cdlg.open) ui::drawCloudDlg(gfx, meta, st, cdlg, viewW, viewH);   // css z-index 300
    if (cpick.open) ui::drawColorPick(gfx, meta, st, cpick, viewW, viewH);   // a page popup: over the page
    if (const int hp = holdPopNow()) { const int was = hold.pop; hold.pop = hp; ui::drawHoldPop(gfx, meta, st, hold, viewW, viewH); hold.pop = was; }
    else { hold.popZones.clear(); hold.popBox = RectF(); }
  }

  // ---- Settings (50-settings.js). The panel stands over the sidebar, above the tool row; the sidebar widens to
  // 600px for it and the tool row keeps the width it had. A remote may save everything but the phone block
  // (main.js: saveSettings from a socket never changes it) — those controls show the deck's values.
  ui::Settings sset; float widthBeforeSettings = -1;
  void initSettings() {}
  bool settingsOpen() const { return sset.open; }
  void openSettings() {
    if (widthBeforeSettings < 0) {                      // widenForSettings
      widthBeforeSettings = st.sidebarWidth; st.toolsWidth = st.sidebarWidth - 8;
      st.sidebarWidth = std::min(std::max(600.f, widthBeforeSettings), viewW * 0.7f);
      fitVisible();
    }
    call("getSettings", json::array(), [this](bool ok, const json& s) {
      if (!ok) { closeSettings(); refused("", s); return; }
      sset.newDriver = ui::newDriverWords(meta);        // "Size is driven by": the four names, or the three of a deck from before the claim
      sset.fontHistory = fontHistory(); sset.bundledFonts = !gfx.privateFamilies.empty();
      ui::settingsFill(sset, s);
      sset.open = true; invalidate();
    });
    invalidate();
  }
  void closeSettings() {
    closeColorPick(false);                              // the popup goes with the panel
    for (auto& kv : sset.edits) if (st.focusEdit == &kv.second) st.focusEdit = nullptr;
    for (auto& a : sset.accts) if (st.focusEdit == &a->kw || st.focusEdit == &a->dir || st.focusEdit == &a->label) st.focusEdit = nullptr;
    sset.open = false;
    if (widthBeforeSettings >= 0) { st.sidebarWidth = widthBeforeSettings; widthBeforeSettings = -1; st.toolsWidth = 0; }
    KillTimer(hwnd, 101);
    applyLayout();
  }
  void toggleSettings() { if (sset.open || widthBeforeSettings >= 0) closeSettings(); else openSettings(); }
  EditBox* settingsEdit(const std::string& id) {
    if (!sset.open || id.rfind("set:", 0) != 0) return nullptr;
    const std::string key = id.substr(4);
    if (key.rfind("acct:", 0) == 0) {
      const size_t c = key.find(':', 5); const size_t i = (size_t)atoi(key.c_str() + 5);
      if (c == std::string::npos || i >= sset.accts.size()) return nullptr;
      const std::string f = key.substr(c + 1);
      return f == "kw" ? &sset.accts[i]->kw : f == "dir" ? &sset.accts[i]->dir : &sset.accts[i]->label;
    }
    auto it = sset.edits.find(key);
    return it == sset.edits.end() ? nullptr : &it->second;
  }
  // a press on a box that does something besides taking the caret
  void editPressed(const std::string& id) {
    if (id == "set:set-fontfamily") sset.fontSource = "box";          // the box was touched last: it is the one that counts
    if (id == "set:set-phone-url") copyPhoneUrl();                    // clicking the link copies it too
  }
  void copyPhoneUrl() {
    EditBox& e = sset.edits["set-phone-url"]; if (e.text.empty()) return;
    setClipboard(e.text); e.selectAll(); st.focusEdit = &e;
    sset.status = L"Link copied."; invalidate();
  }
  bool settingsEditKey(EditBox* e, UINT vk, const Mods&) {
    if (!sset.open || vk != VK_ESCAPE) return false;
    bool mine = false;
    for (auto& kv : sset.edits) if (e == &kv.second) mine = true;
    for (auto& a : sset.accts) if (e == &a->kw || e == &a->dir || e == &a->label) mine = true;
    if (mine) closeSettings();
    return mine;
  }
  bool settingsHit(float x, float y, ui::Hit& h) {
    if (!sset.open || !sset.box.has(x, y)) return false;
    const ui::OZone* z = ozAt(sset.zones, x, y);
    if (z) ozHit(*z, h); else h.kind = "panel";
    return true;
  }
  bool settingsCovers(float x, float y) { return sset.open && sset.box.has(x, y); }
  // the control's own title, else its row's (index.html: the Asks settings carry theirs on the row)
  std::wstring settingsTip(const ui::Hit&, float x, float y) {
    if (!settingsCovers(x, y)) return L"";
    const ui::OZone* z = ozAt(sset.zones, x, y); if (z && !z->tip.empty()) return z->tip;
    const ui::OZone* t = ozAt(sset.tips, x, y); return t ? t->tip : L"";
  }
  bool settingsWheel(float x, float y, float d) { if (!settingsCovers(x, y)) return false; sset.scroll = std::max(0.f, sset.scroll - d); invalidate(); return true; }
  const std::vector<ui::OZone>* settingsZones() { return sset.open ? &sset.zones : nullptr; }
  void settingsDump(std::ostringstream& o) {
    if (!sset.open) return;
    o << "settings [" << sset.box.x << "," << sset.box.y << " " << sset.box.w << "x" << sset.box.h << "] content " << sset.contentH << " scroll " << sset.scroll << " status '" << narrow(sset.status) << "'\n";
    for (auto& kv : sset.checks) o << "  check " << kv.first << " " << kv.second << "\n";
    for (auto& kv : sset.selects) o << "  select " << kv.first << " " << kv.second << "\n";
    for (auto& kv : sset.edits) if (kv.first != "set-phone-url") o << "  edit " << kv.first << " '" << narrow(kv.second.text) << "'\n";   // never the link: it carries the token
    for (auto& z : sset.boxes) { char b[128]; snprintf(b, sizeof b, "[%.1f,%.1f %.1fx%.1f]", z.rect.x, z.rect.y, z.rect.w, z.rect.h); o << "  box " << z.kind << " " << (z.id.empty() ? std::string("-") : z.id) << " " << b << "\n"; }
  }
  void settingsMeta() {}
  void drawSettings(const RectF& sr) {
    if (!sset.open) { settingsWasOpen = false; settingsRiseAt = -1; return; }
    if (!settingsWasOpen) { settingsWasOpen = true; settingsRiseAt = st.transOn ? st.transMs : -1; }
    const float k = settingsRise();
    if (k >= 1) { settingsRiseAt = -1; ui::drawSettings(gfx, meta, st, sset, sr); return; }
    // mid-rise: the panel 10px lower at the start and transparent, as the keyframes say. Its zones are recorded
    // where it is drawn for these few frames; the frame that ends the rise records them where they stay.
    const float dy = gfx.snap(10 * (1 - k));
    gfx.rt->PushLayer(D2D1::LayerParameters(D2D1::RectF(sr.x, sr.y, sr.x + sr.w, sr.y + sr.h + 12), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), k), nullptr);
    ui::drawSettings(gfx, meta, st, sset, RectF(sr.x, sr.y + dy, sr.w, sr.h));
    gfx.rt->PopLayer();
  }
  // Settings' colour box: the picker Chromium opens for `<input type=color>` (ui.hpp ColorPick), in place of the
  // Windows colour dialog. Each change is the colour at once (sset.colors, as the input event sets the box's value).
  void pickColor(const std::string& key, const RectF& anchor) {
    cpick = ui::ColorPick(); cpick.open = true; cpick.key = key; cpick.anchor = anchor; cpick.before = sset.colors[key];
    const unsigned rgb = cpick.before.size() == 7 ? (unsigned)strtoul(cpick.before.c_str() + 1, nullptr, 16) : 0;
    ui::rgbToHsv(rgb, cpick.h, cpick.s, cpick.v);
    cpick.hex.owner = "cp-hex";
    cpick.hex.set(widen(cpick.before.empty() ? std::string("#000000") : cpick.before));
    cpick.hex.onChange = [this] {                       // a whole #rrggbb (or rrggbb) typed: the colour follows
      std::string t = narrow(cpick.hex.text); if (!t.empty() && t[0] == '#') t.erase(0, 1);
      if (t.size() != 6 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return;
      ui::rgbToHsv((unsigned)strtoul(t.c_str(), nullptr, 16), cpick.h, cpick.s, cpick.v);
      for (char& c : t) c = (char)tolower((unsigned char)c);
      sset.colors[cpick.key] = "#" + t; invalidate();
    };
    if (testRun()) logLine("color picker open " + key + " " + cpick.before);
    invalidate();
  }
  void colorPickSet() {                                 // the square or the strip moved: the colour, the hex box
    char b[16]; snprintf(b, sizeof b, "#%06x", ui::hsvToRgb(cpick.h, cpick.s, cpick.v));
    sset.colors[cpick.key] = b;
    cpick.hex.set(widen(b));
    invalidate();
  }
  void colorPickDrag(float x, float y) {
    if (cpick.drag == 1 && cpick.sv.w > 0) {
      cpick.s = std::min(1.f, std::max(0.f, (x - cpick.sv.x) / cpick.sv.w));
      cpick.v = 1 - std::min(1.f, std::max(0.f, (y - cpick.sv.y) / cpick.sv.h));
    } else if (cpick.drag == 2 && cpick.hue.w > 0) cpick.h = 360 * std::min(1.f, std::max(0.f, (x - cpick.hue.x) / cpick.hue.w));
    colorPickSet();
  }
  void closeColorPick(bool revert) {
    if (!cpick.open) return;
    if (revert) sset.colors[cpick.key] = cpick.before;
    if (testRun()) logLine(std::string("color picker ") + (revert ? "cancelled" : "closed") + " " + cpick.key + " = " + sset.colors[cpick.key]);
    if (st.focusEdit == &cpick.hex) st.focusEdit = nullptr;
    if (cpick.drag && GetCapture() == hwnd) ReleaseCapture();
    cpick.open = false; cpick.drag = 0; invalidate();
  }
  static std::vector<std::string> splitKw(const std::string& s) { std::vector<std::string> out; std::string cur; for (char c : s) { if (c == ' ' || c == ',' || c == '\t') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur.push_back(c); } if (!cur.empty()) out.push_back(cur); return out; }
  void saveSettingsNow() {
    // 1. accounts -> the alias file (keywords + folders), then what main resolved; 2. labels, keyed by email when
    // known, else by the account's keyword; 3. everything else
    struct RowIn { std::string keywords, dir, label, key; };
    std::vector<RowIn> rows; json send = json::array();
    for (auto& a : sset.accts) { rows.push_back({ narrow(a->kw.text), narrow(a->dir.text), narrow(trim(a->label.text)), a->key }); send.push_back({ { "keywords", rows.back().keywords }, { "dir", rows.back().dir } }); }
    sset.status = L"";
    call("saveAccounts", json::array({ send }), [this, rows](bool ok, const json& resolved) {
      if (!ok) { sset.status = widen("Not saved: " + (resolved.is_string() ? resolved.get<std::string>() : resolved.dump())); invalidate(); return; }
      json aliases = json::object();
      for (const auto& r : rows) {
        const std::vector<std::string> kws = splitKw(r.keywords);
        std::string key;
        if (resolved.is_array()) for (const auto& a : resolved) {
          bool hit = false;
          if (a.is_object() && a.contains("keywords") && a["keywords"].is_array()) for (const auto& k : a["keywords"]) if (k.is_string() && std::find(kws.begin(), kws.end(), k.get<std::string>()) != kws.end()) hit = true;
          if (hit) { key = !str(a, "email").empty() ? str(a, "email") : str(a, "id"); break; }
        }
        if (key.empty()) key = !r.key.empty() ? r.key : kws.empty() ? "" : kws[0];
        if (!key.empty() && !r.label.empty()) aliases[key] = r.label;
      }
      json payload = ui::settingsPayload(sset); payload["aliases"] = aliases;
      call("saveSettings", json::array({ payload }), [this](bool ok2, const json& saved) {
        if (!ok2) { sset.status = widen("Not saved: " + (saved.is_string() ? saved.get<std::string>() : saved.dump())); invalidate(); return; }
        settingsSaved(saved);
      });
    });
    invalidate();
  }
  // 50-settings.js, after `deck.saveSettings` answered: the font is remembered on this machine, and the panel says
  // "Saved" and closes, or says that the chosen font is not on this machine and stays
  void settingsSaved(const json& saved) {
    if (saved.is_object() && saved.contains("phoneStatus")) ui::settingsPhone(sset, saved["phoneStatus"]);
    const std::string fam = saved.is_object() ? narrow(trim(widen(str(saved, "fontFamily")))) : "";
    rememberFont(fam);                                  // rememberFont(saved.fontFamily)
    // (document.fonts.check: a bundled font read from its file counts, as an @font-face family does)
    if (!fam.empty() && lowerA(narrow(resolveMonoFamily(gfx, widen(fam)))) != lowerA(fam)) { sset.status = widen("Saved. \"" + fam + "\" is not installed on this machine, so Cascadia Mono is used until it is."); invalidate(); return; }
    sset.status = L"Saved"; invalidate();
    if (hwnd) SetTimer(hwnd, 101, 400, nullptr);        // 'Saved' shows for a moment, then the panel closes
  }
  ULONGLONG newTokenUntil = 0;                          // "new token" was clicked once: a second click before this issues it
  bool settingsClick(const ui::Hit& h, float, float, int) {
    if (!sset.open) return false;
    const std::string& k = h.kind;
    auto blurAccts = [this] { for (auto& a : sset.accts) if (st.focusEdit == &a->kw || st.focusEdit == &a->dir || st.focusEdit == &a->label) st.focusEdit = nullptr; };
    if (k == "set-check") { sset.checks[h.id] = !sset.checks[h.id]; invalidate(); return true; }
    if (k == "set-select") {
      std::vector<ui::MenuItem> items; const std::string key = h.id;
      if (key == "set-fontsel") sset.fontSource = "select";              // sel.onfocus: touching the list makes it the one that counts
      for (const auto& o : ui::settingsOptions(sset, key)) { const std::string val = o.first; items.push_back(mi(o.second, [this, key, val] {
        sset.selects[key] = val;
        if (key == "set-fontsel") { sset.fontSource = "select"; sset.edits["set-fontfamily"].set(widen(val)); }
        invalidate();
      }, sset.selects[key] == val)); }
      showCtxMenu(h.rect.x, h.rect.b(), items, "select");
      return true;
    }
    if (k == "set-color") { pickColor(h.id, h.rect); return true; }
    if (k == "set-btn") {
      if (h.id == "save") saveSettingsNow();
      else if (h.id == "cancel") closeSettings();
      else if (h.id == "acct-add") sset.accts.push_back(std::unique_ptr<ui::SettingsAcct>(new ui::SettingsAcct()));
      else if (h.id.rfind("acct-rm:", 0) == 0) { size_t i = (size_t)atoi(h.id.c_str() + 8); blurAccts(); if (i < sset.accts.size()) sset.accts.erase(sset.accts.begin() + i); }
      else if (h.id.rfind("acct-login:", 0) == 0) { size_t i = (size_t)atoi(h.id.c_str() + 11); if (i < sset.accts.size()) { const std::string aid = sset.accts[i]->id; closeSettings(); newTile({ { "kind", "claude" }, { "account", aid } }); } }
      else if (h.id == "phone-copy") copyPhoneUrl();
      else if (h.id == "phone-newtoken") {
        // 50-settings.js: confirm('Issue a new token? …'), then saveSettings({ phone: { …, newToken: true } }). No
        // message box here: the button asks in the panel's own status line and takes a second click within 6 s.
        if (!localMode) sset.status = L"The phone link is the deck's own: a new token is issued on the deck's PC.";
        else if (GetTickCount64() >= newTokenUntil) { newTokenUntil = GetTickCount64() + 6000; sset.status = L"Click again: old links stop working"; }   // (the status line is short; 50-settings.js asks 'Issue a new token? Any phone using the old link stops working until it opens the new one.')
        else {
          newTokenUntil = 0; sset.status = L"";
          const json ph = { { "enabled", sset.checks["set-phone-on"] }, { "port", narrow(sset.edits["set-phone-port"].text) }, { "bind", sset.selects["set-phone-bind"] }, { "newToken", true } };
          call("saveSettings", json::array({ json{ { "phone", ph } } }), [this](bool ok, const json& saved) {
            if (!ok) { sset.status = widen("Not saved: " + (saved.is_string() ? saved.get<std::string>() : saved.dump())); invalidate(); return; }
            if (saved.is_object() && saved.contains("phoneStatus")) ui::settingsPhone(sset, saved["phoneStatus"]);
            sset.status = L"New token issued."; invalidate();
          });
        }
      }
      invalidate(); return true;
    }
    return sset.box.has(press.x, press.y);
  }

  // ------------------------------------------------------------ input
  void sendInput(const std::string& id, const std::string& bytes) {
    if (bytes.empty() || !inited || probing()) return;
    auto it = terms.find(id); if (it == terms.end()) return;
    TermView& v = *it->second;
    if (!v.touched) { v.touched = true; fire("userInput", json::array({ id })); }    // a real key: the tile is no longer fresh
    v.scrollOff = 0; v.sel = Selection();
    if (proto::synthesised(bytes)) return;             // R8, bridge.js: never a mouse or focus report from a remote
    if (testRun()) logInput(id, bytes);
    send(proto::input(id, bytes));
    blinkReset(); invalidate();
  }
  // a test's log names every input frame sent (bytes escaped), so a script can prove what did NOT go to the pty
  void logInput(const std::string& id, const std::string& bytes, const char* kind = "input") {
    std::string e; char b[8];
    for (unsigned char c : bytes) { if (c == 0x1b) e += "\\e"; else if (c < 0x20 || c == 0x7f) { snprintf(b, sizeof b, "\\x%02x", c); e += b; } else e.push_back((char)c); }
    logLine(std::string(kind) + " " + id + " " + e);
  }
  std::wstring fakeClipboard;                           // what a test run's copies land in (edit.hpp testClipboard)
  // ---- the exe's own update (update.hpp; 2026-11-19: the public releases repo). Checked once, 30 s after a
  // real start of the first window (config.json `autoUpdate`: false turns it off), and from the palette. Never from a
  // checkout's build (windows\build) or a test, except a test that names a local folder (CLAUDE_DECK_UPDATE_FROM).
  static bool& updateReadyFlag() { static bool b = false; return b; }
  bool updateReady() const { return updateReadyFlag(); }
  static std::wstring envW(const wchar_t* n) { wchar_t b[2048] = {}; const DWORD k = GetEnvironmentVariableW(n, b, 2048); return k && k < 2048 ? std::wstring(b, k) : std::wstring(); }
  bool autoUpdateHere() const {
    if (testRun() || !opt.bench.empty() || !opt.probe.empty() || !cfg.j.value("autoUpdate", true)) return false;
    std::wstring p = ownExePath(); for (auto& ch : p) ch = (wchar_t)towlower(ch);
    return p.find(L"\\windows\\build\\") == std::wstring::npos;
  }
  static std::string keysResource() {
    HMODULE m = GetModuleHandleW(nullptr); HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(2100), MAKEINTRESOURCEW(10));
    HGLOBAL g = r ? LoadResource(m, r) : nullptr; const char* p = g ? (const char*)LockResource(g) : nullptr;
    return p ? std::string(p, SizeofResource(m, r)) : std::string();
  }
  void checkForUpdates(bool byHand) {
    static std::atomic<bool> busy{ false };
    const std::wstring from = envW(L"CLAUDE_DECK_UPDATE_FROM");
    if (testRun() && from.empty()) { if (byHand) note("A test checks only a folder it names (CLAUDE_DECK_UPDATE_FROM).", 3000); return; }
    if (busy.exchange(true)) { if (byHand) note("Already checking for an update\xE2\x80\xA6", 2500); return; }
    if (byHand) note("Checking for an update\xE2\x80\xA6", 3000);
    const std::wstring exe = ownExePath(); const std::string keys = keysResource();
    std::thread([exe, keys, from, byHand] {
      std::vector<std::string> lines;
      const update::Result r = update::check(exe, keys, from, [&lines](const std::string& l) { lines.push_back(l); });
      shell().later([r, lines, byHand] {
        busy = false;
        App* a = nullptr; for (App* x : shell().apps) if (!x->closing && x->hwnd) { a = x; break; }
        if (!a) return;
        for (const auto& l : lines) a->appLog(l);   // every step, in the exe's own log (not only in a test)
        if (r.state == 2) { updateReadyFlag() = true; a->note("Updated" + (r.version.empty() ? std::string() : " to " + r.version) + ": restart " DECK_EDITION_PRODUCT " to use it (palette: Restart to finish the update).", 9000); }
        else if (byHand) a->note(r.state == 1 ? std::string(DECK_EDITION_PRODUCT " is up to date.") : "No update: " + r.why, 6000);
      });
    }).detach();
  }
  // The exe's own log for what is not the deck's (the self-update's steps): a LOCAL deck's <state>\local\deck.log (the
  // deck's event log, opened per line), a remote's ws.log beside config.json. A test's log gets the line too.
  void appLog(const std::string& line) {
    if (testRun()) logLine(line);
    std::wstring f;
    if (localMode) f = (opt.stateDir.empty() ? configDir() : opt.stateDir) + L"\\local\\deck.log";
    else if (!cfg.path.empty()) f = cfg.path.substr(0, cfg.path.find_last_of(L"\\/") + 1) + L"ws.log";
    if (f.empty()) return;
    SHCreateDirectoryExW(nullptr, f.substr(0, f.find_last_of(L"\\/")).c_str(), nullptr);   // a first start: the folder is not there yet
    SYSTEMTIME t; GetLocalTime(&t); char ts[32]; snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    host::WinFileLog(narrow(f)).line(std::string(ts) + "[update] " + (line.compare(0, 8, "update: ") == 0 ? line.substr(8) : line));
  }
  void restartForUpdate() {
    Shell& s = shell();
    if (testRun()) { logLine("restart for the update (a test only logs it)"); return; }
    if (s.restarting || !s.life.relaunch()) { note("Could not start the new version: close " DECK_EDITION_PRODUCT " and open it again.", 6000); return; }
    s.restarting = true; s.later([] { shellQuit(nullptr); });
  }
  std::string fakeClipImage;                            // (tests) a picture on the test's clipboard: `clipimg <n>` (n bytes of PNG)
  // A picture on THIS machine's clipboard, as PNG bytes: the "PNG" format when a program put one there (a browser,
  // the Snipping Tool), else the bitmap (CF_DIB) encoded through WIC. "" = no picture. A test reads its own double.
  std::string clipboardPng() {
    if (testRun()) return fakeClipImage;
    std::string out;
    const UINT pngFmt = RegisterClipboardFormatW(L"PNG");
    const bool hasPng = IsClipboardFormatAvailable(pngFmt), hasDib = IsClipboardFormatAvailable(CF_DIB);
    if (!hasPng && !hasDib) return out;
    if (!OpenClipboard(hwnd)) return out;
    std::string dib;
    if (hasPng) { HANDLE h = GetClipboardData(pngFmt); const char* p = h ? (const char*)GlobalLock(h) : nullptr; if (p) { out.assign(p, GlobalSize(h)); GlobalUnlock(h); } }
    else { HANDLE h = GetClipboardData(CF_DIB); const char* p = h ? (const char*)GlobalLock(h) : nullptr; if (p) { dib.assign(p, GlobalSize(h)); GlobalUnlock(h); } }
    CloseClipboard();
    if (!out.empty() || dib.size() < sizeof(BITMAPINFOHEADER) || !gfx.wic) return out;
    // the DIB as a .bmp in memory -> WIC decodes it -> a PNG in memory
    const BITMAPINFOHEADER* bi = (const BITMAPINFOHEADER*)dib.data();
    DWORD colors = bi->biClrUsed ? bi->biClrUsed : (bi->biBitCount <= 8 ? (1u << bi->biBitCount) : 0);
    DWORD masks = (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
    BITMAPFILEHEADER fh = {}; fh.bfType = 0x4D42; fh.bfSize = (DWORD)(sizeof(fh) + dib.size()); fh.bfOffBits = (DWORD)(sizeof(fh) + bi->biSize + masks + colors * 4);
    std::string bmp((const char*)&fh, sizeof(fh)); bmp += dib;
    Com<IWICStream> in; Com<IWICBitmapDecoder> dec; Com<IWICBitmapFrameDecode> frame;
    if (FAILED(gfx.wic->CreateStream(in.put())) || FAILED(in->InitializeFromMemory((BYTE*)&bmp[0], (DWORD)bmp.size()))) return out;
    if (FAILED(gfx.wic->CreateDecoderFromStream(in.p, nullptr, WICDecodeMetadataCacheOnDemand, dec.put())) || FAILED(dec->GetFrame(0, frame.put()))) return out;
    IStream* mem = nullptr; if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) || !mem) return out;
    Com<IWICBitmapEncoder> enc; Com<IWICBitmapFrameEncode> fe;
    bool ok = SUCCEEDED(gfx.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) && SUCCEEDED(enc->Initialize(mem, WICBitmapEncoderNoCache))
      && SUCCEEDED(enc->CreateNewFrame(fe.put(), nullptr)) && SUCCEEDED(fe->Initialize(nullptr)) && SUCCEEDED(fe->WriteSource(frame.p, nullptr))
      && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
    HGLOBAL hg = nullptr;
    if (ok && SUCCEEDED(GetHGlobalFromStream(mem, &hg)) && hg) { STATSTG stt = {}; mem->Stat(&stt, STATFLAG_NONAME); const char* p = (const char*)GlobalLock(hg); if (p) { out.assign(p, (size_t)stt.cbSize.QuadPart); GlobalUnlock(hg); } }
    mem->Release();
    return out;
  }
  // REMOTE: a picture pasted (the owner's list, 2026-11-19, item 6). The session's Claude Code cannot read THIS machine's
  // clipboard, so the picture goes to the deck (`pasteImage`: main.js savePastedImage / the core's), which saves it
  // and answers its path; that path is pasted, and Claude Code takes a pasted image path as the picture. A deck from
  // before the action says "unknown action": the note says it needs the deck's next restart.
  void pastePicture(const std::string& id, const std::string& png, const char* mime = "image/png") {
    if (png.size() > 20u * 1024 * 1024) { note("The picture is over 20 MB: not sent.", 3500); return; }
    note("Sending the picture\xE2\x80\xA6", 2500);
    if (testRun()) logLine("paste: a picture (" + std::to_string(png.size()) + " bytes, " + mime + ") sent to the deck");
    call("pasteImage", json::array({ id, deck::core::base64(png), mime }), [this, id](bool ok, const json& r) {
      if (!ok) {
        std::string e = errText(r);
        if (e.find("unknown action") != std::string::npos) e = "This deck cannot take pictures yet: it needs its next restart.";
        if (testRun()) logLine("paste: the picture was refused: " + e);
        note(e, 4500); return;
      }
      auto it = terms.find(id); if (it == terms.end() || !r.is_string()) return;
      if (testRun()) logLine("paste: the deck saved it as " + r.get<std::string>() + "; that path pasted");
      sendInput(id, pasteBytes(r.get<std::string>(), it->second->term.bracketedPaste()));
    });
  }
  // REMOTE: text that is only the path of a picture file on THIS machine (2026-11-24: his mouse button's
  // AutoHotkey macro pastes the newest screenshot's path, which Claude Code on the same PC takes as the picture). The
  // session's Claude Code is on the deck's PC, where that path is nothing, so the FILE goes the way a pasted picture
  // does and the deck's saved path is pasted instead. One line, absolute, quotes allowed, png / jpeg / gif / webp (what
  // the deck takes), a file that is there; anything else stays text. LOCAL never: the session can open the path.
  bool pastePicturePath(const std::string& id, const std::wstring& text, bool copiedFile = false) {
    if (localMode) return false;
    std::wstring p = text;
    while (!p.empty() && iswspace(p.back())) p.pop_back();
    size_t b = 0; while (b < p.size() && iswspace(p[b])) b++;
    p = p.substr(b);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
    if (p.size() < 4 || p.size() > 1024 || p.find_first_of(L"\r\n\"") != std::wstring::npos) return false;
    const bool drive = iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
    if (!drive && p.rfind(L"\\\\", 0) != 0) return false;
    const size_t dot = p.find_last_of(L'.'), sep = p.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) return false;
    std::wstring ext = p.substr(dot + 1); for (auto& c : ext) c = (wchar_t)towlower(c);
    const char* mime = ext == L"png" ? "image/png" : (ext == L"jpg" || ext == L"jpeg") ? "image/jpeg" : ext == L"gif" ? "image/gif" : ext == L"webp" ? "image/webp" : nullptr;
    if (!mime) return false;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (!size) return false;
    if (testRun()) logLine(std::string(copiedFile ? "paste: the copied file is a picture on this machine (" : "paste: the text is a picture's path on this machine (") + narrow(p.substr(sep + 1)) + "): its file goes, not the path");
    if (size > 20ull * 1024 * 1024) { note("The picture is over 20 MB: not sent.", 3500); return true; }
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string bytes((size_t)size, '\0'); DWORD got = 0;
    const bool ok = ReadFile(h, &bytes[0], (DWORD)size, &got, nullptr) && got == size;
    CloseHandle(h);
    if (!ok) return false;
    pastePicture(id, bytes, mime);
    return true;
  }
  // Files copied as in Explorer (CF_HDROP), full paths; a test reads its own (`clipfile <path>|<path>`).
  std::vector<std::wstring> fakeClipFiles;
  std::vector<std::wstring> clipboardFiles() {
    if (testRun()) return fakeClipFiles;
    std::vector<std::wstring> out;
    if (!IsClipboardFormatAvailable(CF_HDROP) || !clipboardOpen(hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
      HDROP d = (HDROP)h; const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n && i < 64; i++) { const UINT len = DragQueryFileW(d, i, nullptr, 0); std::wstring f(len, L'\0'); if (len && DragQueryFileW(d, i, &f[0], len + 1) == len) out.push_back(f); }
    }
    CloseClipboard(); return out;
  }
  // REMOTE, a paste with no text on this machine's clipboard: a picture (a snip: pastePicture) or ONE picture file
  // copied in Explorer (the office session, 2026-11-24: CF_HDROP pasted nothing) goes to the deck. Other files: the
  // note says what can be pasted - never this machine's path, which the session on the deck's PC cannot open. false =
  // nothing on the clipboard to take (the caller's "Nothing to paste").
  bool pasteClipboardPicture(const std::string& id) {
    const std::string png = clipboardPng();
    if (!png.empty()) { pastePicture(id, png); return true; }
    const std::vector<std::wstring> files = clipboardFiles();
    if (files.empty()) return false;
    if (files.size() == 1 && pastePicturePath(id, files[0], true)) return true;
    if (testRun()) logLine("paste: " + std::to_string(files.size()) + " copied file(s), not one picture file: nothing sent");
    note("Only one picture file (png, jpeg, gif or webp) can be pasted here.", 4000);
    return true;
  }
  std::wstring clipboardText() { return clipboardGet(hwnd); }
  void setClipboard(const std::wstring& s) { clipboardSet(hwnd, s); }
  // Ctrl+V / Ctrl+Shift+V (20-terminals.js): THIS machine's clipboard text, as xterm's paste sends it (line ends to
  // CR, bracketed when the application asked: ESC[200~ … ESC[201~).
  // No text on the clipboard (empty, or only an image or files): the Electron window sends the key itself, 0x16,
  // so Claude Code reads the clipboard itself (that is how an image is pasted there). LOCAL keeps that: the
  // session is on this machine. REMOTE does NOT (a deliberate difference, PARITY.md): 0x16 would make the session
  // read the clipboard of the DECK's PC, which is not what the person at this machine copied. Nothing is sent,
  // and the pane says why.
  void paste(const std::string& id) {
    auto it = terms.find(id); if (it == terms.end()) return;
    std::wstring t = clipboardText();
    if (t.empty()) {
      if (localMode) sendInput(id, "\x16");
      else {
        if (pasteClipboardPicture(id)) return;
        if (testRun()) logLine("paste: no text on this machine's clipboard, nothing sent (remote)"); note("Nothing to paste: the clipboard holds no text.", 3500);
      }
      return;
    }
    if (pastePicturePath(id, t)) return;
    sendInput(id, pasteBytes(narrow(t), it->second->term.bracketedPaste()));
  }
  // a deck call failed: say so in the session's own pane, in red (30-tiles.js `report`)
  void report(const std::string& id, const json& err) {
    auto it = terms.find(id); if (it == terms.end()) return;
    std::string s = "\r\n\x1b[31m[claude-deck] " + (err.is_string() ? err.get<std::string>() : err.dump()) + "\x1b[0m\r\n";
    it->second->term.feed(s.data(), s.size()); invalidate();
  }

  // ------------------------------------------------------------ text fields (edit.hpp): who has the caret
  // A text box takes the keys the way a focused <input> does; leaving the rename box saves it (its blur handler).
  void focusEdit(EditBox* e) {
    if (st.focusEdit == e) return;
    EditBox* old = st.focusEdit; st.focusEdit = e;
    if (old == &renameEdit) finishRename(true);
    if (e) { st.focusInTerminal = false; blinkReset(); }
    invalidate();
  }
  void focusTerminal() { focusEdit(nullptr); st.focusInTerminal = !st.selected.empty(); invalidate(); }
  EditBox* editFor(const std::string& id) {
    if (id == "rename") return &renameEdit;
    if (id == "search") return &searchEdit;
    if (id == "ctx-input") return &ctx.input;
    if (id.rfind("askbox:", 0) == 0) return askPane.editById(id);   // a native ask card's answer box
    EditBox* e = panelEdit(id);
    return e;
  }
  static std::wstring trim(const std::wstring& s) { size_t a = s.find_first_not_of(L" \t\r\n"); if (a == std::wstring::npos) return L""; size_t b = s.find_last_not_of(L" \t\r\n"); return s.substr(a, b - a + 1); }
  // Enter / Escape and the other keys a field's owner takes before the field itself (each <input>'s keydown handler)
  bool editKey(UINT vk, const Mods& m) {
    EditBox* e = st.focusEdit;
    if (askPane.ownsEdit(e)) return askPane.editKey(e, vk, m);   // a native ask card's box: Tab / Shift+Tab to the next, Esc leaves it
    if (e == &renameEdit) {
      if (vk == VK_RETURN) { finishRename(true); return true; }
      if (vk == VK_ESCAPE) { finishRename(false); return true; }
      return false;
    }
    if (e == &searchEdit) {                                // 46-search.js
      if (vk == VK_ESCAPE) { closeSearch(); focusTerminal(); return true; }
      if (vk == VK_RETURN) { if (!sidebar.order().empty()) activate(sidebar.order()[0], true); return true; }   // the first match, and its terminal takes the keys
      return false;
    }
    if (e == &ctx.input) {                                 // 35-groups.js promptInline
      if (vk == VK_RETURN) { std::wstring v = trim(ctx.input.text); auto done = ctx.onDone; closeCtxMenu(); if (!v.empty() && done) done(v); return true; }
      if (vk == VK_ESCAPE) { closeCtxMenu(); return true; }
      return false;
    }
    return panelEditKey(e, vk, m);
  }

  // ------------------------------------------------------------ rename (30-tiles.js renameFns)
  // Opened from the tile's menu. Enter saves, Escape cancels, leaving the box saves; an empty title gives the
  // session its own title back (main decides).
  void startRename(const std::string& id) {
    const json* t = tile(id); if (!t || st.renaming == id) return;
    if (!st.renaming.empty()) finishRename(true);
    st.renaming = id; st.renameEdit = &renameEdit;
    renameEdit.set(widen(str(*t, "title"))); renameEdit.selectAll();
    focusEdit(&renameEdit);
  }
  void finishRename(bool save) {
    if (st.renaming.empty()) return;
    std::string id = st.renaming; st.renaming.clear(); st.renameEdit = nullptr;
    if (st.focusEdit == &renameEdit) st.focusEdit = nullptr;
    if (save) call("renameTile", json::array({ id, narrow(renameEdit.text) }), orSay(id));
    invalidate();
  }

  // ------------------------------------------------------------ search (46-search.js)
  void openSearch() { st.searchOpen = true; st.searchEdit = &searchEdit; searchEdit.selectAll(); focusEdit(&searchEdit); clampScroll(); invalidate(); }
  void closeSearch() { st.searchOpen = false; st.search.clear(); searchEdit.set(L""); if (st.focusEdit == &searchEdit) st.focusEdit = nullptr; invalidate(); }

  // ------------------------------------------------------------ the selected tile's action row (30-tiles.js renderActions)
  // A session that has done something asks first, in the tile itself: state.confirm[id] and the row redraws as
  // the question with its danger button and Cancel (sidebar.cpp actions()).
  json browserOpts(const ui::State::Confirm& c) const { return c.browser < 0 ? json() : json{ { "browser", c.browser == 1 } }; }
  void tileAction(const std::string& id, const std::string& act) {
    const json* t = tile(id); if (!t) return;
    const bool fresh = t->value("fresh", false);
    auto none = orSay(id);
    auto rep = orSay(id);
    if (act == "sleep") call("sleepTile", json::array({ id }), rep);
    else if (act == "wake") call("wakeTile", json::array({ id }), none);
    else if (act == "resume" || act == "restart" || act == "retry") call("restartTile", json::array({ id }), none);
    else if (act == "bring-here") call("bringHere", json::array({ id }), none);   // the same-session guard (main.js bringHere)
    else if (act == "refresh") { if (fresh) call("refreshTile", json::array({ id }), none); else { st.confirm[id] = { "refresh", "", -1 }; invalidate(); } }
    else if (act.rfind("open:", 0) == 0) { st.confirm[id] = { "move", act.substr(5), -1 }; invalidate(); }
    else if (act == "special") openHoldForm(id);
    else if (act == "reload") { redrawCloud(id); call("refreshTile", json::array({ id }), none); }   // 30-tiles.js: cloudRedraw(t.id); deck.refreshTile(t.id)
    else if (act == "browser") openCloudOutside(*t);
  }
  void confirmYes(const std::string& id) {
    auto it = st.confirm.find(id); if (it == st.confirm.end()) return;
    ui::State::Confirm c = it->second; st.confirm.erase(it);
    auto none = orSay(id);
    if (c.kind == "move") call("moveTile", json::array({ id, c.account, browserOpts(c) }), none);
    else if (c.kind == "refresh") call("refreshTile", json::array({ id, browserOpts(c) }), none);
    else if (c.kind == "close") call("closeTile", json::array({ id }), none);
    invalidate();
  }
  void confirmNo(const std::string& id) { st.confirm.erase(id); if (hold.tile == id) closeHoldForm(); invalidate(); }
  void closeTile(const std::string& id) {
    const json* t = tile(id); if (!t) return;
    const bool fresh = t->value("fresh", false);
    const bool exited = t->contains("exited") && (*t)["exited"].is_boolean() && (*t)["exited"].get<bool>();
    if (fresh || exited || str(*t, "kind") == "cloud") { call("closeTile", json::array({ id }), orSay()); return; }
    st.confirm[id] = { "close", "", -1 }; invalidate();
  }
  // 48-cloud.js openCloudOutside: the session's claude.ai page in the default browser (the URL carries no deck secret)
  // (main.js open:external: claude.ai links only: nothing else opens through here)
  void openCloudOutside(const json& t) {
    std::string url = str(t, "cloudUrl");
    if (url.empty()) return;
    if (url.rfind("https://claude.ai/", 0) != 0) { reportToActive(json("only claude.ai links open from here")); return; }
    openOutside(url);
  }
  // 48-cloud.js cloudRedraw from a button: the page again, the pane's name flashing
  void redrawCloud(const std::string& id) { if (cloudPanes.redraw(id)) { flashName = id; flashUntil = GetTickCount64() + 600; invalidate(); } }

  // ------------------------------------------------------------ context menus (35-groups.js)
  static ui::MenuItem mi(const std::wstring& label, std::function<void()> run, bool checked = false, bool dim = false) { ui::MenuItem i; i.label = label; i.run = run; i.checked = checked; i.dim = dim; return i; }
  static ui::MenuItem sep() { ui::MenuItem i; i.sep = true; return i; }
  void showCtxMenu(float x, float y, std::vector<ui::MenuItem> items, const std::string& owner = "") {
    const std::string own = owner;                      // (a reference into ctx would be cleared by the close)
    closeCtxMenu();
    ctx.open = true; ctx.prompt = false; ctx.x = x; ctx.y = y; ctx.items = std::move(items); ctx.owner = own; ctx.hover = -1; ctx.rows.clear();
    invalidate();
  }
  void closeCtxMenu() {
    if (!ctx.open) return;
    if (st.focusEdit == &ctx.input) st.focusEdit = nullptr;
    ctx.open = false; ctx.prompt = false; ctx.items.clear(); ctx.rows.clear(); ctx.onDone = nullptr; ctx.owner.clear(); ctx.parents.clear();
    invalidate();
  }
  // 35-groups.js ctxStep: the next row from i in direction d (separators skipped, wrapping); -1 when there is none
  int ctxStep(int i, int d) const {
    const int n = (int)ctx.items.size();
    if (i < 0 || i >= n) i = d > 0 ? -1 : n;
    for (int k = 1; k <= n; k++) { const int j = ((i + d * k) % n + n) % n; if (!ctx.items[j].sep) return j; }
    return -1;
  }
  // a submenu in the menu's place, beside its row (Right, Enter or a click); Left comes back (ctxBack)
  void openCtxSub(int i, bool keyboard) {
    if (!ctx.open || ctx.prompt || i < 0 || i >= (int)ctx.items.size() || ctx.items[i].sub.empty()) return;
    ui::CtxMenu::Level lv; lv.x = ctx.x; lv.y = ctx.y; lv.items = ctx.items; lv.hover = i;
    const RectF row = i < (int)ctx.rows.size() && ctx.rows[i].w > 0 ? ctx.rows[i] : RectF(ctx.x, ctx.y, 150, 20);
    std::vector<ui::MenuItem> sub = ctx.items[i].sub;
    std::vector<ui::CtxMenu::Level> parents = ctx.parents; parents.push_back(lv);
    showCtxMenu(row.r() - 4, row.y, std::move(sub), ctx.owner);
    ctx.parents = std::move(parents);
    if (keyboard) ctx.hover = ctxStep(-1, 1);
  }
  bool ctxBack() {
    if (!ctx.open || ctx.parents.empty()) return false;
    std::vector<ui::CtxMenu::Level> parents = ctx.parents; ui::CtxMenu::Level lv = parents.back(); parents.pop_back();
    showCtxMenu(lv.x, lv.y, lv.items, ctx.owner);
    ctx.parents = std::move(parents); ctx.hover = lv.hover;
    return true;
  }
  // a row picked (Enter, or a click): a submenu opens, anything else closes the menu and runs
  void ctxPick(int i, bool keyboard) {
    if (!ctx.open || i < 0 || i >= (int)ctx.items.size() || ctx.items[i].sep) return;
    if (!ctx.items[i].sub.empty()) { openCtxSub(i, keyboard); return; }
    std::function<void()> run = ctx.items[i].run; closeCtxMenu(); if (run) run();
  }
  // The tile menu from the keyboard, as Windows does it (Shift+F10, the Menu key; 2026-11-23): the selected tile's
  // menu under its sidebar tile (the middle of the window when the sidebar is closed), its first row lit
  void openTileMenuByKey(const std::string& id) {
    RectF r; float x = viewW / 2 - 100, y = viewH / 3;
    if (st.sidebarOpen && sidebar.tileRect(id, r)) { x = r.x + 24; y = std::max(0.f, std::min(r.y + 22, viewH - 40)); }
    openTileMenu(x, y, id);
    if (ctx.open && !ctx.prompt) { ctx.hover = ctxStep(-1, 1); invalidate(); }
  }
  void promptInline(float x, float y, const wchar_t* placeholder, std::function<void(const std::wstring&)> done, const std::wstring& initial = std::wstring()) {
    closeCtxMenu();
    ctx.open = ctx.prompt = true; ctx.x = x; ctx.y = y; ctx.input.set(initial); ctx.input.placeholder = placeholder; ctx.onDone = done;
    focusEdit(&ctx.input);
  }
  std::vector<std::string> groupNames() const {
    std::vector<std::string> out;
    if (meta.contains("groups") && meta["groups"].is_array()) for (const auto& g : meta["groups"]) if (g.is_string()) out.push_back(g.get<std::string>());
    return out;
  }
  // Pause / Resume / Pause until restart for one tile (30-tiles.js pauseItems): the tile menu and the palette list the same
  struct PauseItem { std::wstring label, hint; std::string fn; json args; };
  std::vector<PauseItem> pauseItems(const json& t) {
    std::vector<PauseItem> out;
    if (str(t, "kind") != "claude" || (t.contains("exited") && t["exited"].is_boolean() && t["exited"].get<bool>())) return out;
    const std::string id = str(t, "id"), status = str(t, "status");
    const json p = t.contains("pause") && t["pause"].is_object() ? t["pause"] : json();
    const std::string ps = p.is_object() ? str(p, "state") : std::string();
    const bool want = p.is_object() && p.contains("want") && p["want"].is_boolean() && p["want"].get<bool>();
    if ((ps == "pausing" || ps == "paused") && !want) out.push_back({ L"Resume", L"when its turn ends", "resumeTile", json::array({ id }) });
    if (status != "asleep" && status != "off" && (ps.empty() || ps == "resuming")) {
      out.push_back({ L"Pause", L"Esc, then the PAUSE prompt", "pauseTile", json::array({ id, json::object() }) });
      out.push_back({ L"Pause until restart", L"resumed by itself at the next deck start", "pauseTile", json::array({ id, json{ { "untilRestart", true } } }) });
    }
    return out;
  }
  // Auditors and their children for one tile (30-tiles.js auditorItems): the tile menu (x, y: where it opened; "Set as
  // child of..." opens a second menu of the auditors) and the palette (flat: one row per auditor). Every action is the
  // deck's by name (makeAuditor, setAuditorChild, ...), so local and remote mode are the same.
  struct AuditorItem { std::wstring label, hint; std::function<void()> run; std::vector<ui::MenuItem> sub; };   // sub: "Set as child of…" in the tile menu
  std::string nextAuditorString() const {
    std::set<std::string> used;
    if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& x : meta["tiles"]) if (x.contains("auditor") && x["auditor"].is_object()) { std::string s = str(x["auditor"], "string"); for (char& c : s) c = (char)std::tolower((unsigned char)c); used.insert(s); }
    for (int k = 1; ; k++) if (!used.count("auditor " + std::to_string(k))) return "auditor " + std::to_string(k);
  }
  std::vector<AuditorItem> auditorItems(const json& t, float x, float y, bool flat) {
    std::vector<AuditorItem> out;
    auto exitedOf = [](const json& j) { return j.contains("exited") && j["exited"].is_boolean() && j["exited"].get<bool>(); };
    if (str(t, "kind") != "claude" || exitedOf(t)) return out;
    const std::string id = str(t, "id");
    auto none = orSay(id);
    const float px = std::min(x, viewW - 200), py = std::min(y, viewH - 40);
    auto ask = [this, id, px, py, none](const std::wstring& initial) { promptInline(px, py, L"auditor name", [this, id, none](const std::wstring& v) { call("makeAuditor", json::array({ id, narrow(v) }), none); }, initial); };
    const json au = t.contains("auditor") && t["auditor"].is_object() ? t["auditor"] : json();
    const json ch = t.contains("child") && t["child"].is_object() ? t["child"] : json();
    if (au.is_object()) {
      const size_t n = au.contains("children") && au["children"].is_array() ? au["children"].size() : 0;
      const std::wstring s = widen(str(au, "string"));
      out.push_back({ L"Rename auditor\x2026", L"its children are renamed with it", [ask, s] { ask(s); } });
      out.push_back({ L"Un-make auditor (" + std::to_wstring(n) + (n == 1 ? L" child)" : L" children)"), L"its children end with it", [this, id, none] { call("unmakeAuditor", json::array({ id }), none); } });
      return out;
    }
    std::vector<std::pair<std::string, std::string>> auds;          // the other auditors: tile id, string
    if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& a : meta["tiles"]) if (a.contains("auditor") && a["auditor"].is_object() && str(a, "id") != id && !exitedOf(a)) auds.push_back({ str(a, "id"), str(a["auditor"], "string") });
    const std::string parent = ch.is_object() ? str(ch, "parent") : std::string();
    if (!ch.is_object()) { const std::wstring def = widen(nextAuditorString()); out.push_back({ L"Make auditor\x2026", L"the deck types the AUDITOR prompt into it", [ask, def] { ask(def); } }); }
    if (!auds.empty() && flat) {
      for (const auto& a : auds) { if (a.first == parent) continue; const std::string aid = a.first; out.push_back({ L"Set as child of " + widen(a.second), L"the deck tells the auditor; nothing is typed here", [this, id, aid, none] { call("setAuditorChild", json::array({ id, aid }), none); } }); }
    } else if (!auds.empty()) {
      std::vector<ui::MenuItem> items;
      for (const auto& a : auds) { const std::string aid = a.first; items.push_back(mi(widen(a.second), [this, id, aid, none] { call("setAuditorChild", json::array({ id, aid }), none); }, aid == parent)); }
      out.push_back({ L"Set as child of\x2026", L"the auditors", [this, items, x, y] { showCtxMenu(x, y, items); }, items });
    }
    if (ch.is_object()) out.push_back({ L"Un-child (child " + std::to_wstring(ch.contains("n") && ch["n"].is_number() ? ch["n"].get<int>() : 0) + L" of " + widen(str(ch, "auditorName")) + L")", L"the auditor's files no longer reach it", [this, id, none] { call("unchildTile", json::array({ id }), none); } });
    if (t.contains("auditorMail") && t["auditorMail"].is_array()) for (const auto& m : t["auditorMail"]) if (str(m, "state") == "hold") { const std::string mid = str(m, "id"); out.push_back({ L"Send " + widen(str(m, "name")) + L" from " + widen(str(m, "from")), widen(str(m, "firstLine")), [this, mid, none] { call("auditorSend", json::array({ mid }), none); } }); }
    return out;
  }
  void openTileMenu(float x, float y, const std::string& id) {
    const json* tp = tile(id); if (!tp) return;
    const json& t = *tp;
    const std::string kind = str(t, "kind"), group = str(t, "group");
    const bool pinned = t.value("pinned", false);
    const bool exited = t.contains("exited") && t["exited"].is_boolean() && t["exited"].get<bool>();
    auto none = orSay();
    std::vector<ui::MenuItem> items;
    items.push_back(mi(L"Rename", [this, id] { startRename(id); }));
    items.push_back(mi(pinned ? L"Unpin" : L"Pin", [this, id, pinned, none] { call("pin", json::array({ id, !pinned }), none); }));
    // the Special prompt (no longer a button on the strip, 2026-11-23)
    if (kind == "claude" && !exited) items.push_back(mi(L"Special prompt\x2026", [this, id] { activate(id, true); openHoldForm(id); }));
    // Prompts (35-groups.js): Pause / Resume / Pause until restart, then the auditors and their children ("Set as child
    // of…" a submenu of its own)
    std::vector<ui::MenuItem> prompts;
    for (const PauseItem& p : pauseItems(t)) { const std::string fn = p.fn; const json args = p.args; prompts.push_back(mi(p.label, [this, fn, args, none] { call(fn, args, none); })); }
    for (const AuditorItem& a : auditorItems(t, x, y, false)) { ui::MenuItem m = mi(a.label, a.run); m.sub = a.sub; prompts.push_back(m); }
    if (!prompts.empty()) { ui::MenuItem pm = mi(L"Prompts", nullptr); pm.sub = prompts; items.push_back(pm); }
    // its questions on an ask page, or its last answers read-only (49-asks.js askMenuItem): first in the menu, as F7 with it selected
    { const asks::MenuItem am = askPane.menuItem(t); if (am.has) { std::function<void()> go = am.run; items.insert(items.begin(), { mi(widen(am.label), [this, go] { askSync(); go(); }), sep() }); } }
    // F1–F3 without the keyboard or a drag. Fill works on any grid; the named halves are a 2×2 idea.
    items.push_back(sep());
    std::vector<std::pair<std::wstring, L::Cells>> opts = { { L"Fill the window", L::fullSet(grid) } };
    if (grid.rows == 2 && grid.cols == 2) { opts.push_back({ L"Top half", { 1, 2 } }); opts.push_back({ L"Bottom half", { 3, 4 } }); opts.push_back({ L"Left half", { 1, 3 } }); opts.push_back({ L"Right half", { 2, 4 } }); }
    const L::Cells occ = L::occupancy(layout, id, grid);
    for (const auto& o : opts) { L::Cells set = o.second; items.push_back(mi(o.first, [this, id, set] { placeTile(id, set); }, L::same(occ, set, grid))); }
    if (isPlaced(id)) items.push_back(mi(L"Remove from layout", [this, id] { unplaceTile(id); }));
    // a new deck window is a window on the deck's own box: a remote desk cannot open one (bridge.js openWindow answers
    // null). LOCAL: 35-groups.js `openInNewWindow(id, ev.screenX, ev.screenY)` — where the menu was opened.
    items.push_back(mi(L"Open in a new window", [this, id, x, y] { POINT p = screenPoint(x, y); openInNewWindow(id, true, p.x, p.y); }));
    if (kind == "cloud" && !str(t, "cloudUrl").empty()) items.push_back(mi(L"Open on claude.ai in the browser", [this, id] { const json* c = tile(id); if (c) openCloudOutside(*c); }));
    if (kind == "claude" || kind == "cloud") {
      items.push_back(sep());
      const std::vector<std::string> groups = groupNames();
      for (const auto& g : groups) { const bool on = group == g; items.push_back(mi(widen(g), [this, id, g, on, none] { call("setGroup", json::array({ id, on ? json() : json(g) }), none); }, on)); }
      if (!groups.empty()) items.push_back(mi(L"No group", [this, id, none] { call("setGroup", json::array({ id, json() }), none); }, group.empty(), true));
      items.push_back(mi(L"New group\x2026", [this, id, x, y, none] { promptInline(std::min(x, viewW - 200), std::min(y, viewH - 40), L"group name", [this, id, none](const std::wstring& name) { call("setGroup", json::array({ id, narrow(name) }), none); }); }));
    }
    showCtxMenu(x, y, items);
  }
  void openGroupMenu(float x, float y, const std::string& name) {
    auto none = orSay();
    std::vector<ui::MenuItem> items;
    items.push_back(mi(L"Rename \"" + widen(name) + L"\"", [this, x, y, name, none] {
      promptInline(std::min(x, viewW - 200), std::min(y, viewH - 40), L"new name", [this, name, none](const std::wstring& n) {
        std::vector<std::string> names = groupNames(); for (auto& g : names) if (g == name) g = narrow(n);
        std::vector<std::string> moved; if (meta.contains("tiles")) for (const auto& t : meta["tiles"]) if (str(t, "group") == name) moved.push_back(str(t, "id"));
        const std::string nn = narrow(n);
        call("setGroups", json::array({ names }), [this, moved, nn, none](bool ok, const json&) { if (ok) for (const auto& id : moved) call("setGroup", json::array({ id, nn }), none); });
      });
    }));
    items.push_back(mi(L"Remove \"" + widen(name) + L"\"", [this, name, none] { std::vector<std::string> names; for (const auto& g : groupNames()) if (g != name) names.push_back(g); call("setGroups", json::array({ names }), none); }, false, true));
    showCtxMenu(x, y, items);
  }
  void setGrouped(bool v) { st.grouped = v; savePrefs(); clampScroll(); invalidate(); }
  // the groups button's right click: the grouped toggle, each group with its count, a new group
  void openGroupsMenu(float x, float y) {
    auto none = orSay();
    std::vector<ui::MenuItem> items;
    items.push_back(mi(st.grouped ? L"List is grouped" : L"Group the list", [this] { setGrouped(!st.grouped); }, st.grouped));
    items.push_back(sep());
    const std::vector<std::string> groups = groupNames();
    if (groups.empty()) items.push_back(mi(L"No groups yet", [] {}, false, true));
    for (const auto& g : groups) {
      int n = 0; if (meta.contains("tiles")) for (const auto& t : meta["tiles"]) if (str(t, "group") == g) n++;
      items.push_back(mi(widen(g) + L"  (" + std::to_wstring(n) + L")", [this, x, y, g] { openGroupMenu(x, y, g); }));
    }
    items.push_back(sep());
    items.push_back(mi(L"New group\x2026", [this, x, y, none] { promptInline(std::min(x, viewW - 200), std::min(y, viewH - 40), L"new group name", [this, none](const std::wstring& n) {
      std::vector<std::string> names = groupNames(); names.push_back(narrow(n));
      call("setGroups", json::array({ names }), [this](bool ok, const json&) { if (ok) setGrouped(true); });
    }); }));
    showCtxMenu(x, y, items, "groups");
  }
  // 30-tiles.js browserMenu: right-click `open …` or `refresh` for the same action with Claude in Chrome on (or off)
  bool openBrowserMenu(float x, float y, const std::string& id, const std::string& act) {
    const json* tp = tile(id); if (!tp || str(*tp, "kind") != "claude" || !tp->contains("browser")) return false;
    const bool isOn = (*tp)["browser"].is_boolean() && (*tp)["browser"].get<bool>();
    const int next = isOn ? 0 : 1;
    const std::wstring phrase = isOn ? L"without the browser" : L"with the browser";
    if (act == "refresh") {
      showCtxMenu(x, y, { mi(L"Refresh " + phrase, [this, id, next] {
        const json* t = tile(id); if (!t) return;
        if (t->value("fresh", false)) { call("refreshTile", json::array({ id, json{ { "browser", next == 1 } } }), orSay()); return; }
        st.confirm[id] = { "refresh", "", next }; invalidate();
      }) });
      return true;
    }
    if (act.rfind("open:", 0) == 0) {
      const std::string acct = act.substr(5); std::string label = acct;
      if (meta.contains("accounts")) for (const auto& a : meta["accounts"]) if (str(a, "id") == acct) label = str(a, "label");
      showCtxMenu(x, y, { mi(L"Open " + widen(label) + L" " + phrase, [this, id, acct, next] { st.confirm[id] = { "move", acct, next }; invalidate(); }) });
      return true;
    }
    return false;
  }

  // the window's own keys (lib/platform.js keyAction; bridge.js keyAction for a remote)
  bool windowKey(UINT vk, const Mods& m) {
    switch (windowKeyOf(vk, m)) {                       // keys.hpp: which keys are the window's (one table, unit-tested)
      case WinKey::Redraw: if (!st.selected.empty()) redrawTile(st.selected); return true;
      // 25-layout.js: F6 answers the asks tile when it is the selection, and closes the every-session ask pane
      case WinKey::Zoom: if (askPane.asksZoom()) return true; if (!st.selected.empty()) zoomToggle(st.selected); return true;
      case WinKey::Asks: askSync(); askPane.asksKey(st.selected); return true;
      case WinKey::Sidebar: setCollapsed(st.sidebarOpen); return true;
      case WinKey::Fullscreen: toggleFullscreen(); return true;
      case WinKey::Prev: stepTile(-1); return true;
      case WinKey::Next: stepTile(1); return true;
      case WinKey::Palette: togglePalette(); return true;
      case WinKey::Settings: break;                 // the Mac's Cmd+, : never answered for Platform::Windows
      case WinKey::None: break;
    }
    return false;
  }
  void setCollapsed(bool v) { st.sidebarOpen = !v; savePrefs(); applyLayout(); }
  void toggleFullscreen() {
    if (testRun()) { logLine("fullscreen toggle (never done in a test: it would put the parked window on a screen)"); return; }
    if (headless()) return;
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!fullscreen) {
      MONITORINFO mi = { sizeof(mi) };
      if (GetWindowPlacement(hwnd, &beforeFullscreen) && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = true;
      }
    } else {
      SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
      SetWindowPlacement(hwnd, &beforeFullscreen);
      SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
      fullscreen = false;
    }
  }
  // 25-layout.js: the cell chord. Held keys are collected; the last release places the selected tile.
  // `key` is the physical key, so two keys naming one cell are still two held keys.
  void chordDown(UINT vk, int cell) { chordPressed.insert(cell); chordHeld.insert(vk); }
  void chordUp(UINT vk) {
    if (!chordHeld.count(vk)) return;
    chordHeld.erase(vk);
    if (!chordHeld.empty()) return;
    L::Cells set(chordPressed.begin(), chordPressed.end()); chordPressed.clear();
    if (!st.selected.empty()) placeTile(st.selected, set);
  }
  // On a grid of more than nine cells the letter block under the digits joins the chord, laid out like the grid:
  // 1 2 3 4 / Q W E R / A S D F / Z X C V (25-layout.js letterCell). 0 = not a chord letter here.
  int letterCell(UINT vk) const {
    L::Dims d = L::dims(grid);
    if (d.N <= 9) return 0;
    static const char rows[3][5] = { "QWER", "ASDF", "ZXCV" };
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) if ((UINT)rows[r][c] == vk) return (r + 1 < d.R && c < d.C) ? (r + 1) * d.C + c + 1 : 0;
    return 0;
  }

  // 85-focus.js: a panel that owns the keyboard is open
  bool panelOpen() const { return st.searchOpen || anyPanelOpen(); }
  bool tileKeysLive() const { return !termKeys() && !st.focusEdit && !panelOpen() && st.sidebarOpen && !st.selected.empty(); }

  // A key press. true = the key was TAKEN: no character may follow from it. The message loop asks this before it
  // translates the press (run(): a taken key is never handed to TranslateMessage, so no WM_CHAR / WM_SYSCHAR is
  // ever made for it — nothing has to be fished back out of the queue, whatever a layout makes of the key: a dead
  // key, AltGr, a key that types two characters). false = not ours: it is translated, and its text, if it has
  // any, arrives as WM_CHAR (Ctrl+letter as its control character, Alt+letter as WM_SYSCHAR).
  bool onKeyDown(UINT vk, bool repeat) {
    noteKey();                                          // any key down in the deck's own window = the user is at this PC (presence)
    // an input method is composing (the key is its own), or the "key" is a character injected as a packet
    // (SendInput KEYEVENTF_UNICODE: on-screen keyboards, dictation): never taken, their text comes as WM_CHAR
    if (vk == VK_PROCESSKEY || vk == VK_PACKET) return false;
    hideTip();
    Mods m = mods();
    // 25-layout.js: a key that is not a step ends the walk (commits it); Escape puts it back, and goes nowhere
    const bool stepKey = vk == VK_F11 || vk == VK_F12 || ((vk == VK_UP || vk == VK_DOWN) && tileKeysLive());
    if (!stepKey && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN) {
      const json se = setting("stepEscape");
      if (vk == VK_ESCAPE && stepSnap.on && !(se.is_boolean() && !se.get<bool>())) { stepPreviewCancel(); return true; }
      stepPreviewCommit();
    }
    if (windowKey(vk, m)) return true;
    if (vk == VK_ESCAPE && ctx.open && !ctx.prompt) { closeCtxMenu(); return true; }
    // an open menu has the arrows (35-groups.js, 2026-11-23): Up / Down move, Enter picks, Right opens a submenu,
    // Left closes it (Esc above closes the menu) - before a text box or the terminal sees them
    if (ctx.open && !ctx.prompt && !m.ctrl && !m.alt) {
      switch (vk) {
        case VK_DOWN: case VK_UP: ctx.hover = ctxStep(ctx.hover, vk == VK_DOWN ? 1 : -1); invalidate(); return true;
        case VK_HOME: ctx.hover = ctxStep(-1, 1); invalidate(); return true;
        case VK_END: ctx.hover = ctxStep(-1, -1); invalidate(); return true;
        case VK_RETURN: case VK_SPACE: ctxPick(ctx.hover, true); invalidate(); return true;
        case VK_RIGHT: if (ctx.hover >= 0 && ctx.hover < (int)ctx.items.size() && !ctx.items[ctx.hover].sub.empty()) openCtxSub(ctx.hover, true); return true;
        case VK_LEFT: ctxBack(); invalidate(); return true;
        case VK_TAB: return true;
        default: break;
      }
    }
    // the tile menu from the keyboard: Shift+F10 or the Menu key, for the selected tile (not from a text box or a panel)
    if (((vk == VK_APPS && !m.shift) || (vk == VK_F10 && m.shift)) && !m.ctrl && !m.alt && !st.focusEdit && !panelOpen() && !st.selected.empty() && tile(st.selected)) {
      if (!repeat) openTileMenuByKey(st.selected);
      return true;
    }
    if (st.focusEdit) {                                 // a text box has the keys
      if (editKey(vk, m)) return true;
      // the box's own keys (arrows, Backspace, Ctrl+A …) are taken; anything else is text for it, as WM_CHAR
      const bool took = st.focusEdit ? st.focusEdit->key(vk, m, hwnd) : false;
      if (took && st.focusEdit) askPane.edited(st.focusEdit);   // (a native ask box: the caret stays in sight)
      blinkReset(); invalidate();
      return took;
    }
    if (panelKey(vk, m)) return true;
    if (askPane.key(vk, m)) { invalidate(); return true; }   // Esc on the ask pane (the app has the keys, not a page): it closes; native: Tab, the scroll keys
    // Alt+digit: the chord from anywhere, the terminal included (85-focus.js)
    if (m.alt && !m.ctrl && !m.shift && vk >= '1' && vk <= '9') { if (!repeat) chordDown(vk, vk - '0'); return true; }
    if (!termKeys()) {                                 // the chrome has the keys (or the ask pane is over the terminals)
      if (m.ctrl || m.alt) return false;
      if (panelOpen()) return true;
      if (vk == VK_RETURN || vk == VK_SPACE) { if (!askPane.isOpen() && !st.selected.empty() && st.shown.count(st.selected)) { st.focusInTerminal = true; invalidate(); } return true; }
      if ((vk == VK_UP || vk == VK_DOWN) && !m.shift && tileKeysLive()) { stepTile(vk == VK_UP ? -1 : 1); return true; }
      if (vk >= '1' && vk <= '9' && !m.shift && tileKeysLive()) { if (!repeat) chordDown(vk, vk - '0'); return true; }
      if (int lc = letterCell(vk)) { if (!repeat && !m.shift && tileKeysLive()) { chordDown(vk, lc); return true; } }
      // a modifier alone is never "taken": Windows must still see Shift / Ctrl / Alt go down for what follows
      return !(vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL || vk == VK_NUMLOCK);
    }
    const std::string id = st.selected;
    auto it = terms.find(id); if (it == terms.end() || !st.shown.count(id)) return false;
    TermView& v = *it->second;
    const json* t = tile(id);
    // 20-terminals.js attachCustomKeyEventHandler
    if (m.ctrl && m.shift && vk == 'C') { std::wstring s = v.selectionText(); if (!s.empty()) setClipboard(s); return true; }
    // Safe copy (a deliberate addition over the Electron deck; 2026-11-18: "Ctrl+C risks interrupting the Claude
    // session"): Ctrl+C with a selection in this pane copies it, clears the selection and sends NOTHING to the pty —
    // the key is taken, so no 0x03 is made from it. With no selection it is the interrupt, as ever. Both modes.
    if (m.ctrl && !m.shift && !m.alt && vk == 'C' && !v.sel.empty()) {
      std::wstring s = v.selectionText();
      if (!s.empty()) { setClipboard(s); v.sel = Selection(); if (testRun()) logLine("copy (ctrl+c, nothing sent) " + std::to_string(s.size()) + " chars"); invalidate(); return true; }
    }
    if (m.ctrl && !m.alt && vk == 'V') { paste(id); return true; }
    // Chromium's own editing keys, which reach xterm's textarea as copy / paste: Ctrl+Insert, Shift+Insert
    if (vk == VK_INSERT && m.ctrl && !m.shift && !m.alt) { std::wstring s = v.selectionText(); if (!s.empty()) setClipboard(s); return true; }
    if (vk == VK_INSERT && m.shift && !m.ctrl && !m.alt) { std::wstring s = clipboardText(); if (!s.empty() && !pastePicturePath(id, s)) sendInput(id, pasteBytes(narrow(s), v.term.bracketedPaste())); return true; }
    if (m.ctrl && (vk == VK_HOME || vk == VK_END)) {
      bool appOwns = v.term.altScreen() || (t && str(*t, "kind") == "claude");
      if (!appOwns) { v.scrollOff = vk == VK_HOME ? v.term.scrollbackSize() : 0; invalidate(); return true; }
    }
    if (m.shift && !m.ctrl && !m.alt && (vk == VK_PRIOR || vk == VK_NEXT) && !v.term.altScreen()) { v.scrollBy(vk == VK_PRIOR ? v.term.rows() - 1 : -(v.term.rows() - 1)); invalidate(); return true; }
    std::string bytes;
    if (keyToBytes(vk, m, v.term.appCursorKeys(), bytes)) { sendInput(id, bytes); return true; }
    return false;
  }
  void onKeyUp(UINT vk) { if ((vk >= '1' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) chordUp(vk); }
  wchar_t charHigh = 0;                                 // the first half of a character that arrives as two WM_CHARs
  void onChar(wchar_t ch, bool sys) {
    std::wstring w;
    if (ch >= 0xD800 && ch <= 0xDBFF) { charHigh = ch; return; }
    if (ch >= 0xDC00 && ch <= 0xDFFF) { if (!charHigh) return; w.push_back(charHigh); w.push_back(ch); charHigh = 0; }
    else { charHigh = 0; w.push_back(ch); }
    if (st.focusEdit) { if (!sys) { for (wchar_t c : w) st.focusEdit->ch(c); blinkReset(); invalidate(); } return; }
    if (!termKeys() || st.selected.empty() || !st.shown.count(st.selected)) return;   // never typing into a terminal hidden behind the ask pane
    std::string bytes = narrow(w);
    if (sys) bytes = "\x1b" + bytes;                    // Alt+key: ESC then the key (xterm's default on Windows)
    sendInput(st.selected, bytes);
  }

  // ------------------------------------------------------------ mouse
  ui::Hit hitAt(float x, float y) {
    ui::Hit h;
    if (panelHit(x, y, h)) return h;                    // the palette, a menu, a panel: above everything
    if (ctx.open) {
      if (ctx.prompt && ctx.box.has(x, y)) { h.kind = "edit"; h.id = "ctx-input"; return h; }
      int i = ctx.itemAt(x, y);
      if (i >= 0) { h.kind = "ctx"; h.id = std::to_string(i); return h; }
      if (ctx.box.has(x, y)) { h.kind = "ctx-box"; return h; }
    }
    if (panelHitUnder(x, y, h)) return h;
    RectF sr = sidebarRect();
    if (sr.has(x, y)) {
      if (!st.sidebarOpen) { h.kind = "rail"; return h; }
      return sidebar.hitTest(x, y);
    }
    // the ask pane is over the whole terminal area: what is under it takes no click (its page is a window of its own;
    // what reaches this window is the pane's bar, or the still picture while an overlay is up)
    if (askPane.isOpen() && mainRect().has(x, y)) {
      std::string bid;
      if (askPane.editAt(x, y, &bid)) { h.kind = "edit"; h.id = bid; return h; }   // a native ask card's answer box: a text field like the others
      h.kind = "askpane"; h.rect = mainRect(); return h;
    }
    { std::string cid; RectF pr; if (cloudPanes.linkAt(x, y, cid) && st.shown.count(cid) && paneRect(cid, pr) && pr.has(x, y)) { h.kind = "cloud-link"; h.id = cid; return h; } }   // the card's "Open on claude.ai"
    for (const auto& kv : nameRects) if (st.shown.count(kv.first) && kv.second.has(x, y)) { RectF pr; if (paneRect(kv.first, pr)) { h.kind = "pane-name"; h.id = kv.first; return h; } }
    for (const auto& id : st.shown) { RectF pr; if (paneRect(id, pr) && pr.has(x, y)) { h.kind = "pane"; h.id = id; h.rect = pr; return h; } }
    if (mainRect().has(x, y)) h.kind = "main";
    return h;
  }
  void notYet(const char* what) { note(std::string(what) + ": not in the native preview yet \xE2\x80\x94 use the deck's own window or the browser desk for it."); }

  struct Press { bool on = false; ui::Hit hit; float x = 0, y = 0; int count = 1; } press;
  ULONGLONG lastDownAt = 0; float lastDownX = 0, lastDownY = 0; int downCount = 0;
  EditBox* editDrag = nullptr;
  ULONGLONG lastPinPaint = 0;
  bool releasing = false;                               // our own ReleaseCapture (not a drag taken away from us)
  std::map<std::string, std::string> beforeClick;       // 30-tiles.js: per tile, the selection before the first click of a double-click
  std::string nameClickId, pendingNew;
  static bool tileKind(const std::string& k) { return k == "tile" || k == "tile-acct"; }
  // every zone of a tile carries its id: the tile under a point, whatever part of it that is
  static bool inTile(const std::string& k) { return k.rfind("tile", 0) == 0 || k == "held-x" || k == "amail-send" || k == "amail-x" || k == "confirm-yes" || k == "confirm-no" || k == "hold" || k == "edit"; }

  // ---- LOCAL mode: the application tracks the mouse (DECSET 9 / 1000 / 1002 / 1003). xterm.js bindMouse: a press
  // in the terminal is reported instead of starting a selection, unless Shift is held (SelectionService
  // shouldForceSelection: Shift = the selection, as if the app did not track). REMOTE: never (R8) — there every
  // press selects, exactly as before, whatever the application asked for.
  std::string mousePane; int mouseButtons = 0;          // the pane and the buttons (1 left, 2 right, 4 middle) whose press was reported
  Mods heldMods;                                        // --do `mods shift`: the modifiers a scripted mouse step holds
  Mods mouseMods() const { return scripting ? heldMods : currentMods(); }
  bool appTracksMouse(const TermView& v) const { return localMode && inited && v.term.mouseMode() != 0; }
  bool mouseReport(const std::string& id, TermView& v, float x, float y, int button, int action, const Mods& m) {
    if (!appTracksMouse(v)) return false;
    mouse::Event e; mouse::reportCoords(x - v.lastOx, y - v.lastOy, v.lastCell.w, v.lastCell.h, v.term.cols(), v.term.rows(), e);
    e.button = button; e.action = action; e.ctrl = m.ctrl; e.alt = m.alt; e.shift = m.shift;
    std::string out;
    if (!v.mouseEnc.trigger(e, v.term.mouseMode(), v.term.mouseEncoding(), v.term.cols(), v.term.rows(), out)) return false;
    sendReport(id, mouse::wire(out));
    return true;
  }
  static int heldButton(int mask) { return (mask & 1) ? mouse::LEFT : (mask & 4) ? mouse::MIDDLE : (mask & 2) ? mouse::RIGHT : mouse::NONE; }
  void mouseButtonUp(int bit, int button, float x, float y) {
    if (!(mouseButtons & bit)) return;
    auto it = terms.find(mousePane);
    if (it != terms.end() && mouse::wantsUp(it->second->term.mouseMode())) mouseReport(mousePane, *it->second, x, y, button, mouse::UP, mouseMods());
    mouseButtons &= ~bit;
    if (!mouseButtons) { mousePane.clear(); if (GetCapture() == hwnd) { releasing = true; ReleaseCapture(); releasing = false; } }
  }
  // The right and the middle button on a pane. LOCAL with a tracking application: reported, as xterm does —
  // except a right press over a selection, which stays the copy (the contextmenu handler; 2026-11-18).
  // true = the press went to the application.
  bool onPaneButton(int button, bool down, float x, float y) {
    const int bit = button == mouse::RIGHT ? 2 : 4;
    if (!down) { const bool was = (mouseButtons & bit) != 0; mouseButtonUp(bit, button, x, y); return was; }
    ui::Hit h = hitAt(x, y); if (h.kind != "pane" || cloudPanes.isCloud(h.id)) return false;
    auto it = terms.find(h.id); if (it == terms.end()) return false;
    TermView& v = *it->second;
    if (!appTracksMouse(v) || mouseMods().shift) return false;
    if (button == mouse::RIGHT && !v.sel.empty()) return false;
    stepPreviewCommit();
    if (st.selected != h.id) activate(h.id, true);        // 20-terminals.js: wrap mousedown, any button
    mouseReport(h.id, v, x, y, button, mouse::DOWN, mouseMods());
    mousePane = h.id; mouseButtons |= bit; SetCapture(hwnd);
    return true;
  }
  // ---- the pane's scrollbar: the slider follows the pointer by the point it was gripped; a press on the track
  // puts the slider's middle there first (the VS Code scrollbar xterm 6's viewport is)
  bool thumbDrag = false; std::string thumbPane; float thumbGrab = 0;
  // ---- a selection dragged past the pane's top or bottom edge scrolls the view, every 50 ms, faster the further
  // out (xterm SelectionService _dragScroll: 1 to 15 lines a tick over 50 px)
  int dragScrollAmt = 0;
  // THE ALTERNATE SCREEN has no scrollback to move: past the edge, the APPLICATION is asked to scroll, as the wheel
  // asks it (one report; only an application that takes the wheel as reports - never as arrow keys, which would be
  // typing into it), and its redraw moves the drag (DragSpill, feedPane). Throttled: the lines a tick would have
  // scrolled (1 to 15, by how far out) are summed, and one report goes per kDragWheelLines of them - one every
  // 150 ms just past the edge, at most one a tick (50 ms) far out. 2026-11-24.
  static constexpr int kDragWheelLines = 3;
  int dragWheelBudget = 0;
  bool wheelToApp(const std::string& id, TermView& v, float x, float y, bool up) {
    Mods m = mouseMods(); m.shift = false;               // (the Shift that forced the selection is not the wheel's)
    if (appTracksMouse(v) && mouse::wantsWheel(v.term.mouseMode())) { mouseReport(id, v, x, y, mouse::WHEEL, up ? mouse::UP : mouse::DOWN, m); return true; }
    if (remoteWheelWanted(id, v)) { sendWheelReport(id, v, x, y, up, m); return true; }
    return false;
  }
  void dragScrollTick() {
    if (!selecting) { KillTimer(hwnd, 104); dragScrollAmt = 0; return; }
    auto it = terms.find(selectingId); if (it == terms.end() || !dragScrollAmt) return;
    TermView& v = *it->second;
    if (v.term.altScreen()) {
      if (v.drag.cut()) return;                         // a cut drag: nothing more is selected, nothing more is scrolled
      dragWheelBudget += std::abs(dragScrollAmt);
      if (dragWheelBudget < kDragWheelLines) return;
      dragWheelBudget = 0;
      const float y = v.lastOy + (dragScrollAmt > 0 ? (v.term.rows() - 0.5f) : 0.5f) * v.lastCell.h;   // the edge row the pointer is past
      if (wheelToApp(selectingId, v, st.mouseX, y, dragScrollAmt < 0) && testRun()) logLine(std::string("drag past the ") + (dragScrollAmt > 0 ? "bottom" : "top") + ": the application asked to scroll");
      if (dragScrollAmt > 0) { v.sel.bx = v.term.cols(); v.sel.by = v.term.rows() - 1; } else { v.sel.bx = 0; v.sel.by = 0; }
      invalidate();
      return;
    }
    v.scrollBy(-dragScrollAmt);
    if (dragScrollAmt > 0) { v.sel.bx = v.term.cols(); v.sel.by = std::min(v.topLine() + v.term.rows(), v.absLines() - 1); }
    else { v.sel.bx = 0; v.sel.by = v.topLine(); }
    invalidate();
  }

  void onMouseDown(float x, float y, bool shift = false) {
    if (scripting) shift = heldMods.shift;
    const ULONGLONG now = GetTickCount64();
    if (lastDownAt && now - lastDownAt <= GetDoubleClickTime() && std::fabs(x - lastDownX) <= 4 && std::fabs(y - lastDownY) <= 4) downCount++; else downCount = 1;
    lastDownAt = now; lastDownX = x; lastDownY = y;
    if (oneDeckClick(x, y)) return;                     // the one-deck strip has the window
    stepPreviewCommit();                                // 25-layout.js: any press ends a walk
    st.mouseX = x; st.mouseY = y;
    ui::Hit h = hitAt(x, y);
    press.on = true; press.hit = h; press.x = x; press.y = y; press.count = downCount;
    const std::string& k = h.kind;
    if (hold.pop == 1 && k != "holdpop" && !(k == "hold" && h.arg == "cal")) { hold.pop = 0; invalidate(); }
    // a row of the trigger list: picked on the press, before the box would lose the focus (and with it the list);
    // the box keeps the keyboard, as Chromium's does
    if (k == "holdtrig") { pickHoldTrigger(h.id); press.on = false; return; }
    if (cpick.open && (k == "cp-sv" || k == "cp-hue")) {          // the picker's square / strip: the colour follows the pointer
      if (st.focusEdit) focusEdit(nullptr);
      cpick.drag = k == "cp-sv" ? 1 : 2; press.on = false; SetCapture(hwnd); colorPickDrag(x, y); return;
    }
    // a press that is not a click on a button still counts as the page's click for the open menus: they close
    if (k == "edit" || k == "pane" || k == "main" || k == "resizer" || k == "askpane") {
      if (!(k == "edit" && h.id == "ctx-input")) closeCtxMenu();
      if (!(k == "edit" && h.id == "past-filter")) closePull();
      closeMoveMenu();
    }
    if (k == "edit") {
      EditBox* e = editFor(h.id);
      if (e) { focusEdit(e); e->mouseDown(x, y, shift, downCount); editDrag = e; SetCapture(hwnd); blinkReset(); editPressed(h.id); }
      invalidate(); return;
    }
    if (st.focusEdit) focusEdit(nullptr);               // a press anywhere else takes the caret out of the box
    if (k == "askpane") { press.on = false; askPane.click(x, y); invalidate(); return; }   // the bar: its x closes; nothing else there is a button
    if (k == "resizer") { resizingSidebar = true; SetCapture(hwnd); return; }
    if (k == "pane") {
      // 20-terminals.js: a press on a pane selects it; the terminal takes the keys; xterm starts a selection
      if (st.selected != h.id) activate(h.id, true);
      st.focusInTerminal = true; emptySlot.clear();
      // a cloud pane: the press selects it and its page takes the keys (nothing of the terminal under it is there to select)
      if (cloudPanes.isCloud(h.id)) { press.on = false; cloudWantFocus = h.id; cloudSync(); invalidate(); return; }
      auto it = terms.find(h.id);
      if (it != terms.end()) {
        TermView& v = *it->second;
        RectF track, thumb;
        if (v.scrollbarRects(h.rect, track, thumb) && track.has(x, y)) {
          if (!thumb.has(x, y)) { v.scrollThumbTo(h.rect, y - thumb.h / 2); v.scrollbarRects(h.rect, track, thumb); }
          thumbDrag = true; thumbPane = h.id; thumbGrab = y - thumb.y; SetCapture(hwnd); invalidate(); return;
        }
        const bool tracked = appTracksMouse(v);
        if (tracked && !shift) {                          // the application's press, not a selection
          mouseReport(h.id, v, x, y, mouse::LEFT, mouse::DOWN, mouseMods());
          mousePane = h.id; mouseButtons |= 1; SetCapture(hwnd); invalidate(); return;
        }
        int c, r; v.cellAt(x, y, c, r);
        v.heldText.clear(); v.drag.clear(); v.cutText.clear(); dragWheelBudget = 0;   // a new selection: whatever an old one kept goes
        // (a forced selection — Shift over a tracking application — starts anew: xterm extends only when its
        // selection manager is enabled)
        if (shift && v.sel.active && !tracked) { v.sel.bx = c; v.sel.by = r; v.snapSelection(); copyOnSelect(v); }
        else if (downCount == 2) { v.selectWord(c, r); v.snapSelection(); copyOnSelect(v); }
        else if (downCount >= 3) { v.selectLine(r); v.snapSelection(); copyOnSelect(v); }
        else { v.sel = Selection(); v.selFollow.clear(); v.sel.ax = v.sel.bx = c; v.sel.ay = v.sel.by = r; v.sel.active = true; }
        remoteClick = RemoteClick();
        if (downCount == 1 && !shift && remoteClickWanted(h.id, v)) { remoteClick.on = true; remoteClick.id = h.id; remoteClick.x = x; remoteClick.y = y; remoteClick.m = mouseMods(); }
        selecting = downCount == 1; selectingId = h.id; dragScrollAmt = 0;
        if (selecting && !(shift && v.sel.active && !tracked)) v.drag.start(v.term, c, r);   // the alt screen: a drag that can outlive the pane's rows
        if (selecting) { SetCapture(hwnd); SetTimer(hwnd, 104, mouse::kDragScrollIntervalMs, nullptr); }
      }
      invalidate(); return;
    }
    if (k == "main") {
      // a press on an empty part of the grid selects that slot (25-layout.js emptySlotAt)
      if (!L::isEmpty(layout) && zoom.empty()) {
        RectF mr = mainRect();
        std::set<int> taken; for (const auto& kv : layout) for (int c : kv.second) taken.insert(c);
        L::Cells free; for (int c : L::fullSet(grid)) if (!taken.count(c)) free.push_back(c);
        L::Cells at = L::cellAt((x - mr.x) / mr.w, (y - mr.y) / mr.h, grid);
        if (!at.empty() && std::find(free.begin(), free.end(), at[0]) != free.end()) emptySlot = L::isLegal(free, grid) ? free : L::Cells{ at[0] };
        invalidate();
      }
      return;
    }
    st.pressed = k + h.id + h.arg;
    SetCapture(hwnd);                                   // everything else acts on the release (a click)
    invalidate();
  }

  void onMouseMove(float x, float y) {
    st.mouseX = x; st.mouseY = y;
    if (resizingSidebar) {
      float w = sidebarRight() ? viewW - x : x;
      st.sidebarWidth = std::max(sbMin(), std::min(viewW * 0.7f, w));
      fitVisible(); invalidate(); return;
    }
    if (thumbDrag) {
      auto it = terms.find(thumbPane); RectF pr;
      if (it != terms.end() && paneRect(thumbPane, pr)) { it->second->scrollThumbTo(pr, y - thumbGrab); invalidate(); }
      return;
    }
    if (!mousePane.empty()) {                             // a reported button is held: the drag is the application's (1002 / 1003)
      auto it = terms.find(mousePane);
      if (it != terms.end() && mouse::wantsDrag(it->second->term.mouseMode())) mouseReport(mousePane, *it->second, x, y, heldButton(mouseButtons), mouse::MOVE, mouseMods());
      return;
    }
    if (selecting) {
      auto it = terms.find(selectingId);
      if (it != terms.end()) {
        TermView& v = *it->second;
        int c, r; v.cellAt(x, y, c, r); v.sel.bx = c; v.sel.by = r;
        // past the top or the bottom edge: the end goes to the line's start or end, and the tick scrolls
        dragScrollAmt = mouse::dragScrollAmount(y - v.lastOy, v.term.rows() * v.lastCell.h);
        if (dragScrollAmt > 0) v.sel.bx = v.term.cols(); else if (dragScrollAmt < 0) v.sel.bx = 0;
        invalidate();
      }
      return;
    }
    if (editDrag) { editDrag->mouseDrag(x, y); invalidate(); return; }
    if (cpick.drag) { colorPickDrag(x, y); return; }
    // a tile pressed and moved: the drag begins (30-tiles.js dragstart)
    if (press.on && dragSrc.empty() && inTile(press.hit.kind) && press.hit.kind != "edit" && tile(press.hit.id) && (std::fabs(x - press.x) > 4 || std::fabs(y - press.y) > 4)) {
      dragSrc = press.hit.id; st.dragging = dragSrc; st.pressed.clear();
      RectF tr; ghostW = 0;
      if (sidebar.tileRect(dragSrc, tr)) { grabDX = press.x - tr.x; grabDY = press.y - tr.y; ghostW = tr.w; }   // the drag image keeps the grip
    }
    if (!dragSrc.empty()) { dragMove(x, y); SetCursor(LoadCursor(nullptr, IDC_ARROW)); return; }
    ui::Hit h = hitAt(x, y);
    // the sidebar lights a thing up when hoverPart is its zone kind and hover its id (sidebar.cpp hov())
    std::string hov = h.kind == "restart" && h.id == "row" ? "row:" + h.arg : h.id;
    if (h.kind == "ctx") { int i = atoi(h.id.c_str()); if (i != ctx.hover) { ctx.hover = i; invalidate(); } } else if (ctx.hover != -1) { ctx.hover = -1; invalidate(); }
    panelHover(h);
    if (askPane.mouseMove(h.kind == "askpane" ? x : -1, h.kind == "askpane" ? y : -1)) invalidate();
    // the pointer rests on the asks tile: a click is likely, so the pane's web view is started now (asks.hpp
    // prewarm; it goes again after the grace if no click comes). Moving off the tile before the timer calls it off.
    { const bool over = h.kind == "asks" && askWarm() && hwnd && !headless() && !askPane.webMade() && !askPane.isOpen();
      if (over != askHoverArmed) { askHoverArmed = over; if (over) SetTimer(hwnd, TIMER_ASKWARM, asks::ASK_WARM_HOVER_MS, nullptr); else KillTimer(hwnd, TIMER_ASKWARM); } }
    if (hov != st.hover || h.kind != st.hoverPart || h.arg != st.hoverArg) { st.hover = hov; st.hoverPart = h.kind; st.hoverArg = h.arg; invalidate(); }
    else if (st.sidebarOpen && sidebarRect().has(x, y) && GetTickCount64() - lastPinPaint > 40) { lastPinPaint = GetTickCount64(); invalidate(); }   // the pin's corner zone follows the pointer itself
    const std::string& k = h.kind;
    // LOCAL, DECSET 1003: a move with no button held is the application's too
    if (k == "pane" && localMode) { auto it = terms.find(h.id); if (it != terms.end() && mouse::wantsMove(it->second->term.mouseMode())) mouseReport(h.id, *it->second, x, y, mouse::NONE, mouse::MOVE, mouseMods()); }
    LPCWSTR cur = k == "resizer" ? IDC_SIZEWE : (k == "pane" && cloudPanes.isCloud(h.id)) ? IDC_ARROW : (k == "pane" || k == "edit") ? IDC_IBEAM : (k.empty() || k == "main" || k == "ctx-box" || k == "panel" || k == "cdlg-back" || (k == "askpane" && !askPane.handAt(x, y))) ? IDC_ARROW : IDC_HAND;
    SetCursor(LoadCursor(nullptr, cur));
    updateTip(h, x, y);
  }

  void onMouseUp(float x, float y) {
    if (GetCapture() == hwnd) { releasing = true; ReleaseCapture(); releasing = false; }
    st.pressed.clear();
    if (resizingSidebar) { resizingSidebar = false; savePrefs(); applyLayout(); return; }
    if (thumbDrag) { thumbDrag = false; press.on = false; invalidate(); return; }
    if (cpick.drag) { cpick.drag = 0; press.on = false; if (testRun()) logLine("color picker set " + cpick.key + " = " + sset.colors[cpick.key]); invalidate(); return; }
    if (mouseButtons & 1) { mouseButtonUp(1, mouse::LEFT, x, y); press.on = false; return; }
    if (selecting) {
      selecting = false; dragScrollAmt = 0; dragWheelBudget = 0; KillTimer(hwnd, 104);
      auto it = terms.find(selectingId);
      const RemoteClick rc = remoteClick; remoteClick = RemoteClick();
      if (it != terms.end() && finishDrag(*it->second, true)) { invalidate(); press.on = false; return; }   // it outlived the screen, or was cut: never a click
      if (it != terms.end() && it->second->sel.empty()) {
        it->second->sel = Selection();
        if (rc.on && rc.id == selectingId) sendClickReport(rc.id, *it->second, rc.x, rc.y, rc.m);   // a click, not a drag: the session's
      }
      else if (it != terms.end()) { it->second->snapSelection(); copyOnSelect(*it->second); }
      invalidate(); press.on = false; return;
    }
    if (editDrag) { editDrag = nullptr; press.on = false; return; }
    if (!dragSrc.empty()) { dropTile(x, y); press.on = false; return; }
    if (!press.on) return;
    press.on = false;
    ui::Hit h = hitAt(x, y);
    const bool same = h.kind == press.hit.kind && h.id == press.hit.id && h.arg == press.hit.arg;
    // 35-groups.js: a click on a menu row runs it; a click anywhere else closes the menu (and is still a click there)
    if (ctx.open) {
      if (h.kind == "ctx") { if (same) ctxPick(atoi(h.id.c_str()), false); return; }
      if (h.kind == "ctx-box" || (h.kind == "edit" && h.id == "ctx-input")) return;
      const bool own = (ctx.owner == "groups" && h.kind == "tool" && h.id == "groups-btn") || (ctx.owner == "select" && (h.kind == "set-select" || h.kind == "move-select"));
      closeCtxMenu();
      if (own) return;                                  // the groups button (or a select) closes its own menu, and does no more
    }
    if (!same) return;
    click(h, x, y, press.count);
    invalidate();
  }

  // LOCAL mode: what the core deck still refuses is said in the pane note, where the Electron window would have
  // shown a control with nothing behind it; nothing is asked of the deck (and no dead tile is made). Two things:
  //  - what needs the Claude launch while it is switched off (DeckOptions::allowClaude; st.claudeOff);
  //  - cloud sessions (`kind: 'cloud'` is refused, meta.cloudOption is false).
  // Everything else the call table answers (HOST-NOTES.md) is live.
  bool claudeOffSays(const char* what) {
    if (!localMode || !st.claudeOff) return false;
    note(std::string(what) + " is switched off in this local deck (the Claude launch is off). Terminal tiles work.", 4000);
    return true;
  }
  bool notInLocal(const char* what) {
    if (!localMode) return false;
    note(std::string(what) + " \xE2\x80\x94 not in the local deck yet.", 4000);
    return true;
  }
  void newTile(const json& spec, bool inNewWindow = false) {
    if (spec.value("kind", std::string()) != "shell" && claudeOffSays("Starting a Claude session")) return;
    call("newTile", json::array({ spec }), [this, inNewWindow](bool ok, const json& r) {
      if (!ok) { refused("", r); return; }
      if (!r.is_string()) return;
      pendingNew = r.get<std::string>();
      if (inNewWindow && localMode) { pendingNewWindow = pendingNew; }
      adoptPendingNew();
    });
  }
  std::string pendingNewWindow;                         // a new session that goes into a window of its own once the meta lists it
  void adoptPendingNew() {
    if (pendingNew.empty() || !tile(pendingNew)) return;
    std::string id = pendingNew; pendingNew.clear(); ensureTerm(id);
    if (id == pendingNewWindow) {
      pendingNewWindow.clear();
      RECT wr = {}; if (hwnd) GetWindowRect(hwnd, &wr);
      if (openInNewWindow(id, true, wr.left + (int)((wr.right - wr.left) * 0.1) + 40, wr.top + (int)((wr.bottom - wr.top) * 0.1) + 20)) return;
    }
    placeNew(id);
  }

  // a click: press and release on the same thing
  void click(const ui::Hit& h, float x, float y, int count) {
    const std::string& k = h.kind;
    auto none = orSay();
    if (panelClick(h, x, y, count)) return;
    if (k == "rail") { setCollapsed(false); return; }
    if (tileKind(k)) {
      // A click selects and never places; a double-click places: the tile takes the pane (or the empty slot) that
      // was selected before the first click (30-tiles.js; 2026-09-10)
      const std::string id = h.id;
      if (st.selected != id) beforeClick[id] = st.selected;
      activate(id, false);
      if (count == 2) {
        const std::string before = beforeClick.count(id) ? beforeClick[id] : "";
        if (isPlaced(id) || before.empty() || before == id) return;
        if (zoomBlocks()) return;
        st.selected = before; activate(id, true); beforeClick.erase(id);
      }
      return;
    }
    if (k == "cloud-link") { const json* t = tile(h.id); if (t) openCloudOutside(*t); return; }
    if (k == "confirm-yes") { confirmYes(h.id); return; }
    if (k == "confirm-no") { confirmNo(h.id); return; }
    if (k == "hold") { holdClick(h); return; }
    if (k == "tile-close") { closeTile(h.id); return; }
    if (k == "tile-action") { tileAction(h.id, h.arg); return; }
    if (k == "held-x") { call("holdRemove", json::array({ h.arg }), none); return; }
    if (k == "amail-send") { call("auditorSend", json::array({ h.arg }), none); return; }   // a file from the auditor waiting for Send (30-tiles.js auditorMailRow)
    if (k == "amail-x") { call("auditorDrop", json::array({ h.arg }), none); return; }
    if (k == "tile-pin") { const json* t = tile(h.id); if (t) call("pin", json::array({ h.id, !t->value("pinned", false) }), none); return; }
    if (k == "new-tile") { newTile({ { "kind", "shell" } }); return; }
    if (k == "new-acct") { newTile({ { "kind", "claude" }, { "account", h.id } }); return; }
    if (k == "tool") {
      if (h.id == "collapse") setCollapsed(true);
      else if (h.id == "expand") setCollapsed(false);
      else if (h.id == "search-clear") { searchEdit.set(L""); st.search.clear(); st.searchEdit = &searchEdit; focusEdit(&searchEdit); return; }
      else if (h.id == "search-btn") { if (st.searchOpen) closeSearch(); else openSearch(); return; }
      else if (h.id == "bars-btn") { st.showBars = !st.showBars; savePrefs(); clampScroll(); }
      else if (h.id == "msgs-btn") { st.showMsgs = !st.showMsgs; savePrefs(); clampScroll(); }
      else if (h.id == "groups-btn") setGrouped(!st.grouped);
      else if (h.id == "drive-btn") {
        std::string mode = setting("driver").is_string() ? setting("driver").get<std::string>() : "auto";
        // A deck that speaks the claim's words (ui::newDriverWords; the branch's 80-sidebar.js click): ONE click
        // claims the size for this screen (`remote` for a remote desk, `office` for the deck's own window); a click on its own claim releases to auto. A deck from
        // before it: the three-state cycle, as its own renderer does it.
        std::string next = ui::newDriverWords(meta) ? (ui::driverMode(mode) == driveMe() ? "auto" : driveMe())
          : mode == "auto" ? "phone" : mode == "phone" ? "desk" : "auto";
        call("setDriver", json::array({ next }), [this](bool ok, const json& v) { if (!ok) refused("", v); if (ok && v.is_string() && meta.contains("settings") && meta["settings"].is_object()) { meta["settings"]["driver"] = v; metaGen++; invalidate(); } });
      }
      else if (h.id == "moveall-btn" && claudeOffSays("Moving sessions between accounts")) {}
      else if (h.id == "restart-btn") {                 // 82-restart.js: the button arms it, and cancels it
        const json r = meta.contains("restart") ? meta["restart"] : json();
        const bool armed = r.is_object() && r.value("armed", false);
        const bool upd = r.is_object() && r.contains("update") && !r["update"].is_null();
        if (armed) restartCancel(); else call("restartWhenQuiet", json::array({ true, upd ? "update" : "manual" }), none);
      }
      else if (h.id == "moveall-btn") toggleMoveAll();
      else if (h.id == "settings-btn") toggleSettings();
      st.focusInTerminal = false;
      return;
    }
    if (k == "restart") {
      const json r = meta.contains("restart") ? meta["restart"] : json();
      const bool upd = r.is_object() && r.contains("update") && !r["update"].is_null();
      if (h.id == "row") { if (tile(h.arg)) activate(h.arg, true); }
      else if (h.id == "arm") call("restartWhenQuiet", json::array({ true, upd ? "update" : "manual" }), none);
      else if (h.id == "cancel") restartCancel();
      else if (h.id == "later") call("updateLater", json::array(), none);
      else if (h.id == "now") {
        // "Now" past a blocker takes a second click within four seconds: the button reads "Stop n and restart"
        const size_t n = r.is_object() && r.value("armed", false) && r.contains("blockers") && r["blockers"].is_array() ? r["blockers"].size() : 0;
        if (n && GetTickCount64() >= nowConfirmUntil) { nowConfirmUntil = GetTickCount64() + 4000; st.restartConfirming = true; invalidate(); return; }
        nowConfirmUntil = 0; st.restartConfirming = false;
        call("restartNow", json::array(), none);
      }
      return;
    }
    if (k == "group") { if (st.collapsedGroups.count(h.id)) st.collapsedGroups.erase(h.id); else st.collapsedGroups.insert(h.id); saveCollapsed(); clampScroll(); return; }
    if (k == "elsewhere") { togglePull("elsewhere"); return; }
    if (k == "past") { togglePull("past"); return; }
    // 49-asks.js: a click on the asks tile is the tile selected and F6 pressed (a row: scrolled to it); the same
    // click on the tile while every session's asks show closes them
    if (k == "asks") { st.focusInTerminal = false; askSync(); askPane.asksSelect(h.id); askPane.tileClick(h.id); return; }
    if (k == "pane-name") {
      // click = redraw, after a beat in case a second click makes it a zoom (20-terminals.js)
      KillTimer(hwnd, 100);
      if (count == 2) { nameClickId.clear(); zoomToggle(h.id); }
      else { nameClickId = h.id; if (headless()) { redrawTile(h.id); nameClickId.clear(); } else SetTimer(hwnd, 100, 250, nullptr); }
      return;
    }
    if (k.empty()) st.focusInTerminal = false;
  }
  ULONGLONG nowConfirmUntil = 0;
  void restartCancel() { nowConfirmUntil = 0; st.restartConfirming = false; call("restartWhenQuiet", json::array({ false }), orSay()); }

  // the contextmenu event. On a pane (20-terminals.js): a selection is copied; otherwise paste, unless the app
  // takes the mouse itself. On a tile, a group bar, the groups button: their menus (35-groups.js).
  void onRightClick(float x, float y) {
    stepPreviewCommit();
    ui::Hit h = hitAt(x, y);
    const std::string& k = h.kind;
    if (k == "pane") {
      if (cloudPanes.isCloud(h.id)) return;               // a cloud pane's own menu is its page's
      auto it = terms.find(h.id); if (it == terms.end()) return;
      TermView& v = *it->second;
      std::wstring s = v.selectionText();
      if (!s.empty()) { setClipboard(s); v.sel = Selection(); invalidate(); return; }
      // 20-terminals.js: the deck pastes only where nobody else will — an application that tracks the mouse gets
      // the click as a report and pastes by itself. That holds in LOCAL mode. A REMOTE sends no click report (R8),
      // so there nobody else will: it pastes always (over Claude Code a right click did nothing at all).
      if (!localMode || v.term.mouseMode() == 0) {
        std::wstring t = clipboardText();
        if (!t.empty()) { if (!pastePicturePath(h.id, t)) sendInput(h.id, pasteBytes(narrow(t), v.term.bracketedPaste())); }
        else if (!localMode) { if (!pasteClipboardPicture(h.id)) { if (testRun()) logLine("right click: no text on the clipboard, nothing sent"); note("Nothing to paste: the clipboard holds no text.", 3500); } }
        else if (testRun()) logLine("right click: no text on the clipboard, nothing sent");
      }
      return;
    }
    if (panelRightClick(h, x, y)) return;
    if (k == "tool" && h.id == "groups-btn") { if (ctx.open && ctx.owner == "groups") closeCtxMenu(); else openGroupsMenu(x, y); return; }
    if (k == "new-acct") { openAcctMenu(x, y, h.id); return; }
    if (k == "group") { if (h.id != "pinned" && h.id != "ungrouped") openGroupMenu(x, y, h.id); return; }
    if (k == "tile-action" && openBrowserMenu(x, y, h.id, h.arg)) return;
    if (k == "hold") return;                            // the form's controls keep the event
    if (inTile(k) && tile(h.id)) { openTileMenu(x, y, h.id); return; }
  }
  // The wheel. `delta` is whatever the device reports: 120 a notch on a mouse, any small number, many times a
  // second, on a precision touchpad or a free-spinning wheel. Whole lines are taken out of a running sum, so
  // small deltas add up instead of each rounding to nothing.
  int wheelSum = 0; std::string wheelPane;
  void onWheel(float x, float y, int delta) {
    hideTip();
    ui::Hit h = hitAt(x, y);
    if (panelWheel(h, x, y, delta)) return;
    if (sidebarRect().has(x, y)) { st.scroll -= delta / 120.f * 60; clampScroll(); invalidate(); return; }
    if ((h.kind == "askpane" || h.kind == "edit") && askPane.wheel(x, y, delta)) { invalidate(); return; }   // the native ask pane scrolls itself
    if (h.kind == "pane" || h.kind == "pane-name") {
      if (cloudPanes.isCloud(h.id)) return;               // the page scrolls itself
      auto it = terms.find(h.id); if (it == terms.end()) return;
      TermView& v = *it->second;
      const Mods wm = mouseMods();
      // LOCAL, and the application asked for the wheel (1000 / 1002 / 1003): it gets a report — button 64 up, 65
      // down, ONE per wheel event that amounts to a line or more (xterm bindMouse + consumeWheelEvent; Chromium's
      // wheel is 100 css px a notch) — and nothing scrolls here. Never from a remote (R8).
      if (appTracksMouse(v) && mouse::wantsWheel(v.term.mouseMode())) {
        if (v.wheelSum.consume(-delta / 120.0 * 100.0, v.lastCell.h, wm.shift)) mouseReport(h.id, v, x, y, mouse::WHEEL, delta > 0 ? mouse::UP : mouse::DOWN, wm);
        return;
      }
      // REMOTE, the wheel only (remoteWheelWanted / sendWheelReport below): the session asks for SGR wheel reports
      // by the DECK's own word and by this pane's. Nothing scrolls here; one report per wheel event that amounts to a line.
      if (remoteWheelWanted(h.id, v)) {
        if (v.wheelSum.consume(-delta / 120.0 * 100.0, v.lastCell.h, wm.shift)) sendWheelReport(h.id, v, x, y, delta > 0, wm);
        return;
      }
      // a shell's own scrollback scrolls here (three lines a notch), as it does on the browser desk
      if (!v.term.altScreen()) {
        if (wheelPane != h.id || (wheelSum > 0) != (delta > 0)) { wheelPane = h.id; wheelSum = 0; }
        wheelSum += delta * 3;
        const int lines = wheelSum / 120; wheelSum -= lines * 120;
        if (lines) { v.scrollBy(lines); invalidate(); }
        return;
      }
      // The alternate screen has no scrollback. An application that did not ask for the wheel gets it as an arrow
      // key: ONE `ESC [ A` / `ESC O A` per wheel event that amounts to a line (xterm 6 CoreBrowserTerminal: "simply
      // send a single up or down sequence") — a key, so a remote sends it too. One that DID ask gets reports:
      // above in local mode, never from a remote (R8).
      if (!mouse::wantsWheel(v.term.mouseMode())) {
        if (!v.wheelSum.consume(-delta / 120.0 * 100.0, v.lastCell.h, wm.shift)) return;
        const std::string one = std::string("\x1b") + (v.term.appCursorKeys() ? "O" : "[") + (delta > 0 ? "A" : "B");
        if (inited && !probing()) { if (testRun()) logInput(h.id, one); send(proto::input(h.id, one)); }
      }
    }
  }

  // ---- R8, narrowed for the WHEEL (the owner's report, 2026-11-18: "I can't scroll up in the desktop remote").
  // Claude Code runs on the alternate screen (no scrollback) with mouse tracking on; R8 lets a remote send no
  // report at all, so the wheel did nothing. docs/NATIVE-PORT.md F6 documents the fallback used here: "allow
  // synthesized sequences only when the stream's own mouseOn / sgrMouse say the session is asking for them".
  // A REMOTE sends an SGR wheel report when ALL of these hold, and nothing else is relaxed (no click, no motion,
  // no focus report, never an X10 / default-encoding wheel: those are the bytes that leaked as text on 2026-09-05):
  //   * the deck's meta for the tile says mouseOn === true and sgrMouse === true (main.js: the STREAM's state,
  //     false while the tile sleeps) and the tile is not asleep;
  //   * this pane's own terminal agrees: a tracking mode that reports the wheel is on, and the encoding is SGR (1006).
  bool remoteWheelWanted(const std::string& id, const TermView& v) const {
    if (localMode || !inited || probing()) return false;
    const json* t = tile(id); if (!t) return false;
    auto flag = [t](const char* k) { auto it = t->find(k); return it != t->end() && it->is_boolean() && it->get<bool>(); };
    if (flag("asleep")) return false;
    // (a deck whose meta carries neither word — none known: origin/main's collectMeta always sends both — is not a
    // reason to do nothing: this pane's own modes, which the deck's mode prefix sets on every attach, decide alone)
    const bool said = t->contains("mouseOn") || t->contains("sgrMouse");
    if (said && (!flag("mouseOn") || !flag("sgrMouse"))) return false;
    return mouse::wantsWheel(v.term.mouseMode()) && v.term.mouseEncoding() == 1006;
  }
  // THE ONLY PLACE A REMOTE EMITS A REPORT. One SGR wheel report at the pointer's cell (1-based, clamped into the
  // grid): ESC [ < 64 ; col ; row M up, 65 down, with xterm's modifier bits (Shift 4, Alt 8, Ctrl 16), made by
  // mouse.hpp's encoder. Not through sendInput (proto::synthesised drops every report there, as it must), and
  // checked once more here: what leaves is exactly that shape or nothing. The frame is `kind: "wheel"`
  // (proto::wheel), as the branch's bridge.js marks it. For EVERY deck: neither server reads `kind` (both
  // origin/main's and the branch's lib/phone.js pass only `m.id, m.d` on), so a deck from before the word is
  // not asked to understand it.
  void sendWheelReport(const std::string& id, TermView& v, float x, float y, bool up, const Mods& m) {
    if (!remoteWheelWanted(id, v)) return;
    mouse::Event e; mouse::reportCoords(x - v.lastOx, y - v.lastOy, v.lastCell.w, v.lastCell.h, v.term.cols(), v.term.rows(), e);
    e.button = mouse::WHEEL; e.action = up ? mouse::UP : mouse::DOWN; e.ctrl = m.ctrl; e.alt = m.alt; e.shift = m.shift;
    mouse::Encoder enc; std::string out;
    if (!enc.trigger(e, v.term.mouseMode(), 1006, v.term.cols(), v.term.rows(), out)) return;
    int code = 0, col = 0, row = 0; char fin = 0, extra = 0;
    if (sscanf(out.c_str(), "\x1b[<%d;%d;%d%c%c", &code, &col, &row, &fin, &extra) != 4 || fin != 'M' || (code & ~(4 | 8 | 16)) != (up ? 64 : 65)
        || col < 1 || col > v.term.cols() || row < 1 || row > v.term.rows()) return;
    const json frame = proto::wheel(id, out);            // null unless `out` is exactly one SGR wheel report
    if (frame.is_null()) return;
    if (testRun()) logInput(id, out, "wheel report");
    send(frame);
  }

  // ---- R8, narrowed for a CLICK (2026-11-19, from the remote: "clicks in the remote don't work, like to tell
  // it how well claude is doing, or the yes/no questions, can hit 3 or use arrow and enter though"). Claude Code's
  // clickable prompts need the application's own press. A REMOTE sends a LEFT click - one SGR press and its release,
  // at the cell where the button went down - under the wheel's gate (remoteWheelWanted: the deck's meta says
  // mouseOn && sgrMouse, the tile awake, this pane's terminal tracks the mouse in SGR) and only for a CLICK: the
  // press and the release on the same spot with no selection made between them. It goes on the release (only then
  // is it known not to be a drag). A drag stays this machine's selection (copied on release), the right button
  // stays copy / paste, Shift+click extends the selection; no motion, middle button or focus report is ever sent.
  struct RemoteClick { bool on = false; std::string id; float x = 0, y = 0; Mods m; } remoteClick;
  bool remoteClickWanted(const std::string& id, const TermView& v) const { return remoteWheelWanted(id, v) && mouse::wantsUp(v.term.mouseMode()); }
  void sendClickReport(const std::string& id, TermView& v, float x, float y, const Mods& m) {
    if (!remoteClickWanted(id, v)) return;
    std::string both[2];
    for (int k = 0; k < 2; k++) {
      mouse::Event e; mouse::reportCoords(x - v.lastOx, y - v.lastOy, v.lastCell.w, v.lastCell.h, v.term.cols(), v.term.rows(), e);
      e.button = mouse::LEFT; e.action = k == 0 ? mouse::DOWN : mouse::UP; e.ctrl = m.ctrl; e.alt = m.alt; e.shift = false;
      mouse::Encoder enc;
      if (!enc.trigger(e, v.term.mouseMode(), 1006, v.term.cols(), v.term.rows(), both[k])) return;
      int code = 0, col = 0, row = 0; char fin = 0, extra = 0;
      if (sscanf(both[k].c_str(), "\x1b[<%d;%d;%d%c%c", &code, &col, &row, &fin, &extra) != 4 || fin != (k == 0 ? 'M' : 'm') || (code & ~(8 | 16)) != 0
          || col < 1 || col > v.term.cols() || row < 1 || row > v.term.rows()) return;
      if (proto::click(id, both[k]).is_null()) return;
    }
    if (!v.touched) { v.touched = true; fire("userInput", json::array({ id })); }   // a real click: the tile is no longer fresh
    for (int k = 0; k < 2; k++) { if (testRun()) logInput(id, both[k], "click report"); send(proto::click(id, both[k])); }
  }

  // ------------------------------------------------------------ F4: drag a tile (25-layout.js, 30-tiles.js)
  // Any tile can start a drag; what the drop means is where it lands: on the terminal area it is placed (the
  // preview shows the rectangle it WILL take and shades the rest), on another pinned tile it is the reorder.
  std::string dragSrc; float grabDX = 0, grabDY = 0, ghostW = 0;
  L::Cells previewSet;                                  // the drop preview's cells (empty = hidden)
  std::set<std::string> underPreview;                   // panes a multi-cell preview covers: no lines between them
  std::string growingId;                                // a placed source about to grow: its label goes bold
  L::Cells leaving;                                     // the leaving mark's cells (empty = hidden)
  L::LayoutMap dropBase() const {
    if (!L::isEmpty(layout)) return layout;
    if (!st.selected.empty() && st.selected != dragSrc && tile(st.selected)) return { { st.selected, L::fullSet(grid) } };
    return {};
  }
  L::Cells zoneAt(float x, float y) const {
    RectF mr = mainRect();
    L::Cells raw = L::dropTarget(dropBase(), (x - mr.x) / mr.w, (y - mr.y) / mr.h, grid);
    return (!dragSrc.empty() && isPlaced(dragSrc)) ? L::resolveDrop(layout, dragSrc, raw, grid) : raw;
  }
  void showPreview(const L::Cells& set) {
    L::Cells target = L::norm(set, grid);
    if (target.empty()) { hidePreview(); return; }
    previewSet = target;
    auto inside = [&](const L::Cells& cells) { for (int k : cells) if (std::find(target.begin(), target.end(), k) == target.end()) return false; return true; };
    underPreview.clear();
    for (const auto& kv : layout) if (!kv.second.empty() && target.size() > 1 && inside(kv.second)) underPreview.insert(kv.first);
    L::Cells own = dragSrc.empty() ? L::Cells() : L::occupancy(layout, dragSrc, grid);
    bool targetInOwn = true; for (int k : target) if (std::find(own.begin(), own.end(), k) == own.end()) targetInOwn = false;
    const bool grows = !own.empty() && inside(own) && target.size() > own.size();
    const bool moves = !own.empty() && !inside(own) && !targetInOwn;
    growingId = grows ? dragSrc : "";
    leaving = moves ? own : L::Cells();
    invalidate();
  }
  void hidePreview() { previewSet.clear(); underPreview.clear(); growingId.clear(); leaving.clear(); invalidate(); }
  // ---- the dragged tile's picture OUTSIDE the window. Inside, the drag image is drawn by the frame itself
  // (drawFrame: ui::drawTileGhost at the pointer, by the point it was gripped). Electron's is the system's drag image,
  // which follows the pointer across the whole desktop; a frame cannot draw outside its window, so while the pointer
  // is beyond the client area the same picture rides in a small layered window of its own: click-through, never
  // activated, no taskbar button, gone the moment the drag ends or comes back in. LOCAL mode only (a tile dragged
  // out opens a deck window there; a remote desk opens none, so its picture stops at the window's edge as before).
  // A test never shows it: `dump ghost` renders the same bitmap to a PNG instead.
  HWND ghostWnd = nullptr;
  bool renderGhost(Gfx& g2, int& pw, int& ph, float scale) {
    RectF tr; if (dragSrc.empty() || ghostW <= 0 || !sidebar.tileRect(dragSrc, tr)) return false;
    const int w = (int)std::ceil(ghostW) + 16, h = (int)std::ceil(tr.h) + 16;   // 8px round it: the pin sits half outside the tile
    if (!g2.initShared(gfx) || !g2.targetBitmap(w, h, scale)) return false;
    g2.begin(); g2.clear(Rgba(0, 0, 0, 0));
    ui::drawTileGhost(g2, meta, st, dragSrc, 8, 8, ghostW);
    g2.end();
    UINT bw = 0, bh = 0; g2.wicBitmap->GetSize(&bw, &bh); pw = (int)bw; ph = (int)bh;
    return pw > 0 && ph > 0;
  }
  void ghostHide() { if (ghostWnd) { DestroyWindow(ghostWnd); ghostWnd = nullptr; } }
  void ghostFollow(float x, float y) {
    const bool outside = x < 0 || y < 0 || x > viewW || y > viewH;
    if (!outside || !localMode || testRun() || dragSrc.empty() || !hwnd) { ghostHide(); return; }
    const POINT at = screenPoint(x - grabDX - 8, y - grabDY - 8);
    if (ghostWnd) { SetWindowPos(ghostWnd, HWND_TOPMOST, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE); return; }
    Gfx g2; int pw = 0, ph = 0;
    if (!renderGhost(g2, pw, ph, dpi / 96)) return;
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = inst; wc.lpszClassName = L"ClaudeDeckNativeGhost"; RegisterClassW(&wc); reg = true; }
    ghostWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"ClaudeDeckNativeGhost", L"", WS_POPUP, at.x, at.y, pw, ph, hwnd, nullptr, inst, nullptr);
    if (!ghostWnd) return;
    // the bitmap's own premultiplied pixels, straight onto the layered window
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HDC screen = GetDC(nullptr); HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
      WICRect all = { 0, 0, pw, ph }; Com<IWICBitmapLock> lock;
      if (SUCCEEDED(g2.wicBitmap->Lock(&all, WICBitmapLockRead, lock.put())) && lock) {
        UINT stride = 0, size = 0; BYTE* src = nullptr; lock->GetStride(&stride); lock->GetDataPointer(&size, &src);
        if (src) { for (int row = 0; row < ph; row++) memcpy((BYTE*)bits + (size_t)row * pw * 4, src + (size_t)row * stride, (size_t)pw * 4); ok = true; }
      }
      if (ok) {
        HGDIOBJ old = SelectObject(mem, dib);
        POINT zero = { 0, 0 }, pos = at; SIZE sz = { pw, ph }; BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        ok = UpdateLayeredWindow(ghostWnd, screen, &pos, &sz, mem, &zero, 0, &bf, ULW_ALPHA) != 0;
        SelectObject(mem, old);
      }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem); ReleaseDC(nullptr, screen);
    if (!ok) { ghostHide(); return; }
    ShowWindow(ghostWnd, SW_SHOWNOACTIVATE);
  }
  void dragMove(float x, float y) {
    ghostFollow(x, y);
    st.dropMark.clear(); invalidate();
    if (mainRect().has(x, y)) {
      if (!zoom.empty()) { zoomBlocks(); hidePreview(); return; }
      showPreview(zoneAt(x, y));
      return;
    }
    hidePreview();
    ui::Hit h = sidebarRect().has(x, y) && st.sidebarOpen ? sidebar.hitTest(x, y) : ui::Hit();
    const json* t = inTile(h.kind) ? tile(h.id) : nullptr; const json* s = tile(dragSrc);
    RectF tr;
    if (t && s && t->value("pinned", false) && s->value("pinned", false) && sidebar.tileRect(h.id, tr)) { st.dropMark = h.id; st.dropBefore = y < tr.y + tr.h / 2; }
    invalidate();
  }
  void dropTile(float x, float y) {
    const std::string id = dragSrc;
    const std::string mark = st.dropMark; const bool before = st.dropBefore;
    ghostHide();
    dragSrc.clear(); st.dragging.clear(); st.dropMark.clear();
    if (mainRect().has(x, y) && !sidebarRect().has(x, y)) {
      if (!zoom.empty()) { hidePreview(); return; }
      dragSrc = id; L::Cells set = zoneAt(x, y); dragSrc.clear();
      // first drop onto a full-screen session: seed it as a full pane so the conflict rules split it with the incoming tile
      if (L::isEmpty(layout) && !st.selected.empty() && st.selected != id && tile(st.selected)) layout = { { st.selected, L::fullSet(grid) } };
      hidePreview();
      placeTile(id, set);
      return;
    }
    hidePreview();
    if (!mark.empty() && mark != id) {                  // the sidebar reorder: pinned tiles only, on both ends
      std::vector<std::string> ids;
      if (meta.contains("tiles")) for (const auto& t : meta["tiles"]) if (t.is_object() && t.value("pinned", false) && str(t, "id") != id) ids.push_back(str(t, "id"));
      auto at = std::find(ids.begin(), ids.end(), mark);
      if (at != ids.end()) { ids.insert(before ? at : at + 1, id); call("reorder", json::array({ ids }), orSay()); }
    }
    // let go outside the window (30-tiles.js dragend: the pointer beyond the viewport, nothing took the drop): a new
    // deck window opens there with this tile filling it. A remote desk cannot (bridge.js openWindow).
    if (x < 0 || y < 0 || x > viewW || y > viewH) { POINT p = screenPoint(x, y); openInNewWindow(id, true, p.x, p.y); }
    invalidate();
  }
  // 25-layout.js openInNewWindow -> main.js `window:open`. False where no window can open (a remote desk).
  POINT screenPoint(float x, float y) const { POINT p = { (LONG)std::lround(x * dpi / 96), (LONG)std::lround(y * dpi / 96) }; if (hwnd) ClientToScreen(hwnd, &p); return p; }
  bool openInNewWindow(const std::string& id, bool at, int sx, int sy) {
    if (probing() || !inited) return false;
    return shellOpenWindow(*this, id, at, sx, sy) != nullptr;
  }

  // Tooltips: the element's title="" text. One tooltip window, used as a TRACKING tooltip (TTF_TRACK): the app
  // says when and where it shows. (The usual kind arms itself per tool rectangle, and this window is one
  // rectangle: after the first tip it would not show again until the pointer had left the window.) Chromium's
  // rule: the pointer rests on an element with a title for half a second, the tip appears under it; moving to
  // another element, a press, a key or the wheel takes it away.
  static TOOLINFOW tipInfo(HWND owner) { TOOLINFOW ti = {}; ti.cbSize = TTTOOLINFOW_V1_SIZE; ti.hwnd = owner; ti.uId = 1; return ti; }
  bool tipShown = false;
  void hideTip() {
    if (!tip) return;
    KillTimer(hwnd, TIMER_TIP);
    if (tipShown) { TOOLINFOW ti = tipInfo(hwnd); SendMessageW(tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); tipShown = false; }
  }
  // 20-terminals.js paintPaneNames: label.title = `<abbr> (cloud) <title> Â· <the ask mark's title>` (the name in full:
  // the label is cut at 70 % of the pane). Before a pane has a tile to name: createTerm's own hint.
  std::wstring paneNameTip(const std::string& id) {
    const json* t = tile(id); if (!t) return L"Click to redraw this pane \x00B7 double-click to zoom it";
    const std::string abbr = acctAbbr(*t), kind = str(*t, "kind");
    std::string tip = (abbr.empty() ? "" : abbr + " ") + (kind == "cloud" ? "(cloud) " : "") + tileTitle(*t);
    if (kind == "claude") { const asks::Mark m = askPane.markInfo(str(*t, "sessionId")); if (m.has && askPane.showMarks()) tip += " \xC2\xB7 " + m.title; }
    return widen(tip);
  }
  void updateTip(const ui::Hit& h, float x, float y) {
    if (!tip) return;
    std::wstring text = panelTip(h, x, y);
    if (text.empty() && !panelCovers(x, y) && !(ctx.open && ctx.box.has(x, y)))
      text = sidebarRect().has(x, y) && st.sidebarOpen ? sidebar.tooltipAt(x, y) : h.kind == "askpane" ? askPane.tooltipAt(x, y) : h.kind == "pane-name" ? paneNameTip(h.id) : h.kind == "cloud-link" ? L"" : L"";
    // the same element still: a showing tip stays where it is, a waiting one keeps waiting
    if (text == tipText && h.kind == tipHit.kind && h.id == tipHit.id && h.arg == tipHit.arg) return;
    tipText = text; tipHit = h;
    hideTip();
    if (!text.empty() && !press.on) SetTimer(hwnd, TIMER_TIP, 500, nullptr);
  }
  void showTip() {
    KillTimer(hwnd, TIMER_TIP);
    if (!tip || tipText.empty() || press.on || GetCapture() == hwnd || !windowFocused) return;
    POINT p; if (!GetCursorPos(&p)) return;
    // only while the pointer is really over this window (it may have left without a message reaching us)
    POINT c = p; ScreenToClient(hwnd, &c); RECT rc; GetClientRect(hwnd, &rc);
    if (!PtInRect(&rc, c) || WindowFromPoint(p) != hwnd) return;
    TOOLINFOW ti = tipInfo(hwnd); ti.lpszText = (LPWSTR)tipText.c_str();
    SendMessageW(tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(p.x, p.y + (int)std::lround(20 * dpi / 96)));   // under the pointer, clear of the cursor
    SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    tipShown = true;
  }

  // ------------------------------------------------------------ what this machine remembers
  void loadPrefs() {
    st.sidebarWidth = cfg.j.value("sbwidth", 340.f);      // style.css --sb-width
    st.showBars = !cfg.j.value("hidebars", false);
    st.showMsgs = !cfg.j.value("hidemsgs", false);
    st.grouped = cfg.j.value("grouped", false);
    st.sidebarOpen = !cfg.j.value("collapsed", false);
    if (cfg.j.contains("collapsedGroups") && cfg.j["collapsedGroups"].is_array()) for (const auto& g : cfg.j["collapsedGroups"]) if (g.is_string()) st.collapsedGroups.insert(g.get<std::string>());
  }
  void savePrefs() {
    if (opt.noSave) return;
    cfg.j["sbwidth"] = st.sidebarWidth; cfg.j["hidebars"] = !st.showBars; cfg.j["hidemsgs"] = !st.showMsgs;
    cfg.j["grouped"] = st.grouped; cfg.j["collapsed"] = !st.sidebarOpen;
    cfg.j["collapsedGroups"] = std::vector<std::string>(st.collapsedGroups.begin(), st.collapsedGroups.end());
    // the window's NORMAL rectangle (where it goes back to from maximised or minimised), and whether it is — or,
    // minimised, will come back — maximised. Never a test's parked window.
    if (hwnd && !fullscreen && !testRun()) {
      WINDOWPLACEMENT wp = { sizeof(wp) };
      if (GetWindowPlacement(hwnd, &wp)) cfg.j["window"] = { wp.rcNormalPosition.left, wp.rcNormalPosition.top, wp.rcNormalPosition.right, wp.rcNormalPosition.bottom,
        wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED)) };
    }
    cfg.save();
  }
  void saveCollapsed() { savePrefs(); }

  static long long nowMs() { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL; }

  // ------------------------------------------------------------ --bench: what a frame costs (tests)
  // No deck at all: a fixture meta in the sidebar (windows/test/fixtures/meta-sixty.json: sixty tiles) and four
  // panes of text in a 2 x 2 grid, drawn --frames times per case on the off-screen bitmap and on the REAL window
  // target (the window is parked off every screen, as --window-shot parks it). The HWND target presents without
  // waiting for the display refresh here, or every frame would read as one refresh interval.
  static double qpcMs() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
  static std::string benchText(int rows, int cols, unsigned seed) {
    // what a session's screen holds: prompts, coloured listings, code, a table with box characters, a few wide glyphs
    static const char* words[] = { "const", "return", "worklist", "batch", "inventory", "session", "render", "frame", "damage", "glyph", "SELECT", "FROM", "floor.tiles", "WHERE", "status", "=", "'busy'", "npm", "run", "build", "error", "warning", "ok", "->", "{", "}", "(", ")", ";", "0x1f", "42", "3.14" };
    std::string out; unsigned r = seed * 2654435761u + 12345;
    auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (r >> 10); };
    for (int y = 0; y < rows; y++) {
      const unsigned kind = rnd() % 6;
      std::string line;
      if (kind == 0) line = "\x1b[32muser@box\x1b[0m:\x1b[34m~/src/deck\x1b[0m$ git status --short";
      else if (kind == 1) { line = "\x1b[90m\xE2\x94\x82\x1b[0m "; for (int i = 0; i < 4; i++) { char b[40]; snprintf(b, sizeof b, "\x1b[3%um%-12s\x1b[0m \x1b[90m\xE2\x94\x82\x1b[0m ", 1 + rnd() % 6, words[rnd() % 32]); line += b; } }
      else {
        int w = 0;
        while (w < cols - 14) {
          const char* word = words[rnd() % 32]; const unsigned style = rnd() % 9;
          char b[64];
          if (style == 0) snprintf(b, sizeof b, "\x1b[1m%s\x1b[0m ", word);
          else if (style == 1) snprintf(b, sizeof b, "\x1b[38;5;%um%s\x1b[0m ", 16 + rnd() % 200, word);
          else if (style == 2) snprintf(b, sizeof b, "\x1b[4%um%s\x1b[0m ", 1 + rnd() % 6, word);
          else if (style == 3) snprintf(b, sizeof b, "\x1b[3%um%s\x1b[0m ", 1 + rnd() % 7, word);
          else snprintf(b, sizeof b, "%s ", word);
          line += b; w += (int)strlen(word) + 1;
          if (kind == 5 && w > cols / 2) break;
        }
      }
      out += line + "\r\n";
    }
    return out;
  }
  int runBench() {
    std::ofstream log(opt.bench.c_str(), std::ios::binary | std::ios::trunc);
    if (!log) return 4;
    json fx = json::object();
    { std::ifstream in(opt.fixture.c_str(), std::ios::binary); std::stringstream ss; ss << in.rdbuf(); fx = json::parse(ss.str(), nullptr, false); }
    if (!fx.is_object() || !fx.contains("tiles")) { log << "no fixture (--fixture <meta.json>)\n"; return 6; }
    fx["restoring"] = false;
    const int N = std::max(5, opt.frames);
    char line[256];
    snprintf(line, sizeof line, "bench: %d frames a case, %dx%d DIPs, fixture tiles %d, mono '%s'\n", N, opt.width, opt.height, (int)fx["tiles"].size(), narrow(monoFamily).c_str()); log << line;
    for (int pass = 0; pass < 2; pass++) {
      const bool onWindow = pass == 1;
      if (onWindow) { if (!gfx.targetWindow(hwnd, true)) { log << "hwnd target failed\n"; break; } RECT rc; GetClientRect(hwnd, &rc); viewW = rc.right * 96.f / dpi; viewH = rc.bottom * 96.f / dpi; }
      else { viewW = (float)opt.width; viewH = (float)opt.height; if (!gfx.targetBitmap(opt.width, opt.height, shotScale())) { log << "bitmap target failed\n"; break; } }
      metricsBySize.clear(); dropCaches();
      inited = true; st.connected = true; st.notice.clear(); st.windowIndex = 0; grid.rows = 2; grid.cols = 2;
      terms.clear(); layout.clear(); zoom.clear();
      applyMeta(fx);
      const std::vector<std::string> ids = tileIds();
      for (int i = 0; i < 4 && i < (int)ids.size(); i++) layout.push_back({ ids[i], { i + 1 } });
      st.selected = ids.empty() ? "" : ids[0]; st.focusInTerminal = true;
      applyLayout();
      std::vector<TermView*> views;
      for (int i = 0; i < 4 && i < (int)ids.size(); i++) { TermView* v = ensureTerm(ids[i]); std::string t = benchText(v->term.rows() * 3, v->term.cols(), 7 + i); v->term.feed(t.data(), t.size()); views.push_back(v); }
      if (views.empty()) { log << "no tiles\n"; break; }
      snprintf(line, sizeof line, "\n== %s target, view %.0fx%.0f DIPs at %.0f dpi; a pane is %dx%d cells\n", onWindow ? "HWND (parked window)" : "bitmap (off-screen, software)", viewW, viewH,
        onWindow ? dpi : 96 * shotScale(), views[0]->term.cols(), views[0]->term.rows()); log << line;
      auto frame = [&](double& draw, double& total) {
        const double t0 = qpcMs();
        gfx.begin(); drawFrame(); gfx.rt->Flush();
        const double t1 = qpcMs();
        gfx.end();
        const double t2 = qpcMs();
        draw = t1 - t0; total = t2 - t0;
      };
      auto run = [&](const char* name, const std::function<void(int)>& before) {
        std::vector<double> d, t; double a, b;
        frame(a, b);                                    // one frame to settle (fonts, caches) before timing
        sceneStats = SceneStats();
        for (int i = 0; i < N; i++) { before(i); frame(a, b); d.push_back(a); t.push_back(b); }
        auto avg = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return s / (double)v.size(); };
        auto p95 = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[(size_t)((v.size() - 1) * 0.95)]; };
        snprintf(line, sizeof line, "%-36s draw %7.3f ms (p95 %7.3f)   with present %7.3f ms (p95 %7.3f)   [full %u, panes whole %u, row bands %u, sidebar %u of %u frames]\n", name, avg(d), p95(d), avg(t), p95(t),
          sceneStats.full, sceneStats.paneFull, sceneStats.paneRows, sceneStats.sidebar, sceneStats.frames); log << line; log.flush();
      };
      TermView* v0 = views[0];
      run("idle (nothing changed)", [&](int) {});
      run("cursor blink", [&](int i) { blinkOn = caretOn = (i % 2) == 0; });
      blinkOn = caretOn = true;
      run("one line appended to one pane", [&](int i) { std::string t = benchText(1, v0->term.cols(), 1000 + i); v0->term.feed(t.data(), t.size()); });
      run("full-screen update of one pane", [&](int i) { std::string t = "\x1b[H\x1b[2J" + benchText(v0->term.rows() - 1, v0->term.cols(), 5000 + i); v0->term.feed(t.data(), t.size()); });
      run("all four panes update fully", [&](int i) { for (size_t k = 0; k < views.size(); k++) { std::string t = "\x1b[H\x1b[2J" + benchText(views[k]->term.rows() - 1, views[k]->term.cols(), 9000 + i * 7 + (int)k); views[k]->term.feed(t.data(), t.size()); } });
      run("sidebar hover moves", [&](int i) { st.hover = ids[(size_t)i % std::min<size_t>(ids.size(), 8)]; st.hoverPart = "tile"; });
      run("sidebar scrolls", [&](int i) { st.scroll = (float)((i * 37) % 900); });
      st.scroll = 0; st.hover.clear(); st.hoverPart.clear();
      // a new meta, as the deck's 2 s tick brings it: the clock moved (every age may read differently), one
      // tile's last message and status changed
      { json fx2 = fx; const long long now0 = fx.value("now", (long long)0);
        run("a new meta (clock + one tile changed)", [&](int i) {
          fx2["now"] = now0 + (long long)(i + 1) * 2000;
          json& t = fx2["tiles"][(size_t)i % fx2["tiles"].size()];
          t["lastText"] = "Step " + std::to_string(i) + ": changed by the bench.\nSecond line."; t["status"] = (i % 2) ? "busy" : "idle";
          applyMeta(fx2);
        });
        applyMeta(fx); }
      // animations: the clock ticks (33 ms a frame) and only the parts that move are painted again
      { json fa = fx; fa["settings"]["animations"] = true;
        auto animRun = [&](const char* name) {
          applyMeta(fa); animForced = true; st.animMs = 0;
          { double a, b; frame(a, b); }
          const size_t parts = sidebar.animCount();
          char nm[96]; snprintf(nm, sizeof nm, "%s (%zu parts)", name, parts);
          run(nm, [&](int i) { st.animMs = (i + 1) * 33.0; });
          snprintf(line, sizeof line, "%-36s animated parts repainted %u in %u frames\n", "", sceneStats.animParts, sceneStats.frames); log << line;
        };
        fa["tiles"][1]["ultracode"] = true; fa["tiles"][1]["effort"] = "high";
        animRun("anim: one ultracode tile pulses");
        fa["tiles"][2]["effort"] = "max"; fa["tiles"][3]["attention"] = true; fa["settings"]["effortStyle"] = "color";
        if (fa.contains("restart") && fa["restart"].is_object()) fa["restart"]["armed"] = true; else fa["restart"] = { { "armed", true }, { "blockers", json::array() } };
        animRun("anim: pulse + max drift + attention + restart icon");
        animForced = false; st.animMs = 0; applyMeta(fx);
        run("idle again (animations on, nothing animating)", [&](int) {}); }
      // the frame as it was before the scene: every part laid out and drawn again, straight to the target
      gfx.sceneOff = true;
      run("no scene, nothing kept (before)", [&](int) { gfx.runCacheDrop(); gfx.gradKept.clear(); ui::dropTextKept(); });
      gfx.sceneOff = opt.noScene; dropCaches();
    }
    return 0;
  }

  // ------------------------------------------------------------ --do: the scripted run (script.hpp)
  std::vector<ScriptStep> script; size_t scriptAt = 0; bool scriptStarted = false;
  void logLine(const std::string& s) {
    const std::wstring& base = !opt.shot.empty() ? opt.shot : opt.windowShot;
    if (base.empty() || opt.doScript.empty()) return;
    std::ofstream f((base + L".log.txt").c_str(), std::ios::binary | std::ios::app); f << s << "\n";
  }
  void renderHidden() { syncFocus(); if (!gfx.rt) return; gfx.begin(); drawFrame(); gfx.end(); }
  std::string resolveId(const std::string& a) const {
    if (a == "sel") return st.selected;
    if (a.rfind("title:", 0) == 0) return findTile(a.substr(6));
    return a;
  }
  bool zonePoint(const ScriptStep& s, float& x, float& y, bool overlay) {
    const std::string kind = s.args.size() > 0 ? s.args[0] : "";
    std::string id = s.args.size() > 1 ? resolveId(s.args[1]) : "-", arg = s.args.size() > 2 ? s.args[2] : "-";
    // an arg may hold spaces (an account label): everything after the id
    if (s.args.size() > 3) for (size_t i = 3; i < s.args.size(); i++) arg += " " + s.args[i];
    RectF r;
    if (overlay) { if (!panelZone(kind, id, r)) return false; }
    else {
      const ui::Hit* h = sidebar.find(kind, id, arg);
      if (!h) return false;
      r = h->rect;
    }
    x = r.x + r.w / 2; y = r.y + r.h / 2;
    if (!overlay && kind == "tile") y = r.y + 13;         // a tile: its title line (the middle may be a button)
    return true;
  }
  void scriptClick(float x, float y, int n) {
    lastDownAt = 0;
    for (int i = 0; i < n; i++) { onMouseMove(x, y); renderHidden(); onMouseDown(x, y); onMouseUp(x, y); renderHidden(); }
  }
  void runStep() {
    if (scriptAt >= script.size()) { scripting = false; SetTimer(hwnd, TIMER_SHOT, 250, nullptr); return; }
    const ScriptStep s = script[scriptAt++];
    bool stop = false;
    const int delay = execStep(s, stop);
    if (stop) return;
    renderHidden();
    SetTimer(hwnd, TIMER_SCRIPT, delay, nullptr);
  }
  // One step, carried out by THIS window (`win <n> <step>` hands one to another window of the deck). Returns how long
  // to wait before the next; `stop`: the run ends here (the window is closing).
  int execStep(const ScriptStep& s, bool& stop) {
    int delay = 120;
    renderHidden();                                     // the zones a step aims at are the ones the last frame drew
    auto num = [&](size_t i) { return i < s.args.size() ? (float)atof(s.args[i].c_str()) : 0.f; };
    float x = 0, y = 0;
    const std::string& c = s.cmd;
    if (c == "wait") delay = std::max(1, atoi(s.rest.c_str()));
    else if (c == "click") scriptClick(num(0), num(1), 1);
    else if (c == "dbl") scriptClick(num(0), num(1), 2);
    else if (c == "tri") scriptClick(num(0), num(1), 3);
    else if (c == "rclick") { onMouseMove(num(0), num(1)); onPaneButton(mouse::RIGHT, true, num(0), num(1)); onPaneButton(mouse::RIGHT, false, num(0), num(1)); onRightClick(num(0), num(1)); }
    else if (c == "mclick") { onMouseMove(num(0), num(1)); onPaneButton(mouse::MIDDLE, true, num(0), num(1)); onPaneButton(mouse::MIDDLE, false, num(0), num(1)); }
    else if (c == "move") onMouseMove(num(0), num(1));
    else if (c == "down") { lastDownAt = 0; onMouseMove(num(0), num(1)); onMouseDown(num(0), num(1)); }
    else if (c == "up") onMouseUp(num(0), num(1));
    else if (c == "zone" || c == "dzone" || c == "rzone" || c == "hzone" || c == "downzone" || c == "ozone" || c == "rozone" || c == "hozone") {
      const bool ov = c[0] == 'o' || c.rfind("rozone", 0) == 0 || c.rfind("hozone", 0) == 0;
      if (!zonePoint(s, x, y, ov)) logLine("NO ZONE: " + s.cmd + " " + s.rest);
      else if (c == "zone" || c == "ozone") scriptClick(x, y, 1);
      else if (c == "dzone") scriptClick(x, y, 2);
      else if (c == "rzone" || c == "rozone") { onMouseMove(x, y); onRightClick(x, y); }
      else if (c == "hzone" || c == "hozone") onMouseMove(x, y);
      else { lastDownAt = 0; onMouseMove(x, y); onMouseDown(x, y); }
    }
    else if (c == "menu") {
      int hit = -1; const std::wstring want = widen(s.rest);
      for (size_t i = 0; i < ctx.items.size(); i++) if (!ctx.items[i].sep && ctx.items[i].label.compare(0, want.size(), want) == 0 && i < ctx.rows.size()) { hit = (int)i; break; }
      if (hit < 0) logLine("NO MENU ROW: " + s.rest); else scriptClick(ctx.rows[hit].x + ctx.rows[hit].w / 2, ctx.rows[hit].y + ctx.rows[hit].h / 2, 1);
    }
    else if (c == "key" || c == "keyup" || c == "tap") {
      UINT vk; Mods m;
      if (!parseKeySpec(s.rest, vk, m)) logLine("NO KEY: " + s.rest);
      else { fakeMods = m; if (c != "keyup") onKeyDown(vk, false); if (c != "key") onKeyUp(vk); fakeMods = Mods(); }
    }
    else if (c == "text") { for (wchar_t ch : widen(s.rest)) onChar(ch, false); }
    else if (c == "sel") { std::string id = findTile(s.rest); if (id.empty()) logLine("NO TILE: " + s.rest); else activate(id, false); }
    else if (c == "shot") {
      const std::wstring& base = !opt.shot.empty() ? opt.shot : opt.windowShot;
      size_t sl = base.find_last_of(L"\\/"); const std::wstring path = (sl == std::wstring::npos ? L"" : base.substr(0, sl + 1)) + L"n-" + widen(s.rest) + L".png";
      if (!opt.windowShot.empty()) { InvalidateRect(hwnd, nullptr, FALSE); captureWindow(path); } else saveShot(path);
    }
    else if (c == "size") {
      if (!opt.windowShot.empty()) {                    // the real (parked) window: the WM_SIZE path itself
        RECT r = { 0, 0, (LONG)std::lround(std::max(200.f, num(0)) * dpi / 96), (LONG)std::lround(std::max(150.f, num(1)) * dpi / 96) };
        AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE), (UINT)dpi);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      } else { viewW = std::max(200.f, num(0)); viewH = std::max(150.f, num(1)); gfx.targetBitmap((int)viewW, (int)viewH, shotScale()); fitVisible(); }
    }
    else if (c == "dump") dump(s.rest);
    else if (c == "call") {                             // call <fn> <json array of args>: a deck call, through App::call (so the test blocks apply)
      const std::string fn = s.args.empty() ? "" : s.args[0];
      const size_t sp = s.rest.find(' ');
      std::string js = sp == std::string::npos ? "[]" : s.rest.substr(sp + 1);
      for (char& ch : js) if (ch == '\'') ch = '"';      // a script writes JSON with single quotes: no quoting through three shells
      for (size_t at; (at = js.find("$sel")) != std::string::npos;) js.replace(at, 4, st.selected);   // the selected tile's id
      json args = json::parse(js, nullptr, false);
      if (fn.empty() || !args.is_array()) logLine("BAD CALL: " + s.rest);
      else call(fn, args, [this, fn](bool ok, const json& r) { logLine(std::string(ok ? "ok " : "err ") + fn + " " + r.dump().substr(0, 160)); if (!ok) refused("", r); });
    }
    else if (c == "wheel") onWheel(num(0), num(1), (int)num(2));
    else if (c == "char") onChar((wchar_t)strtoul(s.rest.c_str(), nullptr, 0), false);   // one WM_CHAR by code (0x1b, 3 …)
    else if (c == "ws") {
      if (s.rest == "drop") onWs(WsClosed, ""); else if (s.rest == "open") onWs(WsOpen, "");
      else if (s.rest == "silence") ws.testSilence();    // the 60 s watchdog, now: the connection is given up, closed, and made again
      else if (s.rest == "count") { logLine("ws connections " + std::to_string(ws.liveConnections()) + " connected " + std::to_string(st.connected) + " inited " + std::to_string(inited)); logLine("ws client " + std::string(DeckSocket::nameOf(ws.mode())) + ": " + ws.describe()); }
    }
    else if (c == "close") { scripting = false; PostMessageW(hwnd, WM_CLOSE, 0, 0); stop = true; return 0; }   // the window's own close (WM_CLOSE): the run ends the way a person ends it   // the socket's own events, as ws.hpp posts them
    else if (c == "wm") {                               // window messages a test cannot cause for real: the handler itself
      if (s.rest == "min") { testMin = true; proc(WM_SIZE, SIZE_MINIMIZED, 0); reportWindowState(); }
      else if (s.rest == "restore") { testMin = false; askSync(); cloudSync(); invalidate(); reportWindowState(); }
      else if (s.rest == "capture-lost") proc(WM_CAPTURECHANGED, 0, 0);
      else if (s.rest == "paint") { InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd); }
    }
    else if (c == "clipimg") {                          // clipimg <n>: n bytes of a picture on the test's clipboard, no text (0 = none)
      const int n = std::max(0, atoi(s.rest.c_str())); fakeClipboard.clear(); fakeClipFiles.clear();
      fakeClipImage = n ? std::string("\x89PNG\r\n\x1a\n", 8) + std::string((size_t)std::max(0, n - 8), 'p') : std::string();
    }
    else if (c == "clipfile") {                         // clipfile <path>|<path>…: files copied as in Explorer, no text (empty = none)
      fakeClipboard.clear(); fakeClipImage.clear(); fakeClipFiles.clear();
      std::wstring all = widen(s.rest); size_t a = 0;
      while (a < all.size()) { size_t b = all.find(L'|', a); if (b == std::wstring::npos) b = all.size(); if (b > a) fakeClipFiles.push_back(all.substr(a, b - a)); a = b + 1; }
    }
    else if (c == "feed") {                             // bytes straight into the selected pane's terminal, as if the app had written them (\e = ESC)
      auto it = terms.find(st.selected);
      if (it == terms.end()) logLine("NO PANE: feed");
      else { std::string t = s.rest; for (size_t at; (at = t.find("\\e")) != std::string::npos;) t.replace(at, 2, "\x1b"); for (size_t at; (at = t.find("\\r")) != std::string::npos;) t.replace(at, 2, "\r"); for (size_t at; (at = t.find("\\n")) != std::string::npos;) t.replace(at, 2, "\n"); for (size_t at; (at = t.find("\\s")) != std::string::npos;) t.replace(at, 2, ";"); feedPane(st.selected, t, false); }
    }
    else if (c == "faketile") {                         // faketile <field> <json>: the selected tile reads as if the deck said so ('' for ")
      const size_t sp = s.rest.find(' '); std::string js = sp == std::string::npos ? "null" : s.rest.substr(sp + 1);
      for (char& ch : js) if (ch == '\'') ch = '"';
      json v = json::parse(js, nullptr, false);
      if (s.args.empty() || v.is_discarded() || st.selected.empty()) logLine("BAD FAKETILE: " + s.rest);
      else { fakeTile[st.selected][s.args[0]] = v; json m2 = meta; applyMeta(m2); }
    }
    else if (c == "fakeset") {                          // fakeset <key | settings.key> <json>: a field of the meta itself, as a deck in another state would send it
      const size_t sp = s.rest.find(' '); std::string js = sp == std::string::npos ? "null" : s.rest.substr(sp + 1);
      for (char& ch : js) if (ch == '\'') ch = '"';
      json v = json::parse(js, nullptr, false);
      if (s.args.empty() || v.is_discarded()) logLine("BAD FAKESET: " + s.rest); else { fakeMeta[s.args[0]] = v; json m2 = meta; applyMeta(m2); }
    }
    else if (c == "clip") { std::string t = s.rest; for (size_t at; (at = t.find("\\r")) != std::string::npos;) t.replace(at, 2, "\r"); for (size_t at; (at = t.find("\\n")) != std::string::npos;) t.replace(at, 2, "\n"); fakeClipboard = widen(t); fakeClipFiles.clear(); }   // what a paste will find (the test's own clipboard)
    else if (c == "mods") { heldMods = Mods(); heldMods.shift = s.rest.find("shift") != std::string::npos; heldMods.ctrl = s.rest.find("ctrl") != std::string::npos; heldMods.alt = s.rest.find("alt") != std::string::npos; }
    else if (c == "focus") { if (s.rest == "off") proc(WM_KILLFOCUS, 0, 0); else proc(WM_SETFOCUS, 0, 0); }
    else if (c == "ask" && s.args.size() > 1 && s.args[0] == "link") askPane.linkClicked(widen(s.rest.substr(5)));   // ask link <url>: a link on an ask page followed (what the web view's new-window request hands over)
    else if (c == "ask") askStep(s);
    else if (c == "webscan") webScan(s);
    // the process ends HERE, as a crash or a kill would end it: no close, no goodbye, no web view closed, no cookie
    // cleared. What is on disk afterwards is what the browser had written by itself (windows/test/webmem.ps1).
    else if (c == "crash") { logLine("crash: the process ends here (TerminateProcess), nothing is closed"); TerminateProcess(GetCurrentProcess(), 9); }
    else if (c == "webmem") logLine(webMem(s.rest));       // the web views' processes of THIS exe, counted and weighed (windows/test/webmem.ps1)
    else if (c == "cloud") cloudStep(s);
    else if (c == "anim") {                             // anim <ms>: the animation clock, set (a headless run has no timer); anim off: the static look again
      if (s.rest == "off") { animForced = false; st.animMs = 0; }
      else if (s.rest == "dump") logLine("anim: on " + std::to_string(st.animOn) + " clock " + std::to_string((long long)st.animMs) + " ms, timer " + std::to_string(animTimerOn) + ", sidebar parts that move " + std::to_string(sidebar.animCount()) + ", visible " + std::to_string(animVisible()) + ", repainted parts so far " + std::to_string(sceneStats.animParts));
      else { animForced = true; st.animMs = atof(s.rest.c_str()); }
    }
    else if (c == "ime") {                              // ime comp <text> | ime commit <text> | ime end: an input method's composition, as its messages are handled once read
      const size_t sp = s.rest.find(' '); const std::wstring text = widen(sp == std::string::npos ? std::string() : s.rest.substr(sp + 1));
      const std::string sub = s.args.empty() ? "" : s.args[0];
      if (sub == "comp") { imeInline = imePane() != nullptr; if (imeInline) imeUpdate(text, nullptr); else logLine("ime: no terminal has the keys (Windows' own composition window would show)"); }
      else if (sub == "commit") { if (imeInline) imeUpdate(L"", &text); else for (wchar_t ch : text) onChar(ch, false); }
      else if (sub == "end") imeEnd();
      else logLine("UNKNOWN IME STEP: " + s.rest);
    }
    else if (c == "stamp") {                            // stamp <text>: what the special prompt form's time box would make of this entry (ms, or 0 = no value), and the minutes of a time
      const std::wstring t = widen(s.rest); const long long ms = parseStamp(t);
      logLine("stamp '" + s.rest + "' -> " + (ms ? narrow(localStamp(ms, true)) + " (" + std::to_string(ms) + ")" : std::string("no value")) + "; as a time of day: " + std::to_string(clockOf(t)));
    }
    else if (c == "trans") {                            // trans <ms>: the css transitions' clock, set (a test's are off until a script says this); trans off; trans dump
      if (s.rest == "off") { transForced = false; st.transMs = 0; }
      else if (s.rest == "dump") { renderHidden(); logLine("trans: on " + std::to_string(st.transOn) + " clock " + std::to_string((long long)st.transMs) + " ms, sidebar transitions under way " + std::to_string(sidebar.transActive()) + ", settings rise " + std::to_string(settingsRise()) + ", timer wanted " + std::to_string(transVisible())); }
      else { transForced = true; st.transMs = atof(s.rest.c_str()); }
    }
    else if (c == "setsaved") {                         // setsaved <json>: the deck's answer to saveSettings, as it would arrive (a test never saves a real deck's settings)
      std::string js = s.rest; for (char& ch : js) if (ch == '\'') ch = '"';
      json v = json::parse(js, nullptr, false);
      if (v.is_discarded()) logLine("BAD SETSAVED: " + s.rest); else { settingsSaved(v); logLine("settings status '" + narrow(sset.status) + "'"); }
    }
    else if (c == "cdlg") { if (s.args.size() && s.args[0] == "fail") cloudFailed(s.rest.size() > 5 ? s.rest.substr(5) : std::string()); }   // cdlg fail <text>: the deck's refusal, as its err would arrive
    else if (c == "win") {                              // win <n> <step …>: the step, carried out by the deck window with that index
      App* to = nullptr; const int n = s.args.empty() ? -1 : atoi(s.args[0].c_str());
      for (App* a : shell().apps) if (!a->closing && a->inited && a->st.windowIndex == n) to = a;
      const size_t sp = s.rest.find(' ');
      const std::vector<ScriptStep> sub = sp == std::string::npos ? std::vector<ScriptStep>() : parseScript(s.rest.substr(sp + 1));
      if (!to || sub.empty()) logLine("NO WINDOW: " + s.rest);
      else if (to == this) delay = execStep(sub[0], stop);
      else { bool gone = false; to->scripting = true; delay = to->execStep(sub[0], gone); if (!gone) to->renderHidden(); }
    }
    else if (c == "toast") {                            // the notifier's test double (notify.hpp): a toast as the deck would raise it, a click on it, a flash
      Shell& sh = shell(); const std::string sub = s.args.empty() ? "" : s.args[0];
      if (sub == "click") {                             // the last toast shown (or the last one of that tile), clicked
        core::Toast t; if (s.args.size() > 1) t.tileId = resolveId(s.args[1]);
        for (auto it = sh.notifier.shown.rbegin(); it != sh.notifier.shown.rend(); ++it) if (s.args.size() < 2 || it->tileId == t.tileId) { t = *it; break; }
        shellToastClick(t);
      }
      else if (sub == "flash") sh.notifier.flash(s.args.size() > 1 ? atoi(s.args[1].c_str()) : st.windowIndex);
      else if (sub == "dump") { std::string o = "toasts " + std::to_string(sh.notifier.shown.size()) + ", flashes " + std::to_string(sh.notifier.flashed.size()) + ", real toasts shown 0 (a test only records)"; logLine(o); }
      else {
        const json n = setting("notify");
        core::Toast t; t.kind = sub.empty() ? "waiting" : sub; t.tileId = st.selected;
        const json* tl = tile(st.selected);
        if (t.kind == "ask") { t.sessionId = s.args.size() > 1 ? s.args[1] : tl ? str(*tl, "sessionId") : std::string(); if (s.args.size() > 1 && !(tl && str(*tl, "sessionId") == t.sessionId)) t.tileId.clear(); }   // toast ask [session id]: a new ask page
        t.title = "test \xC2\xB7 " + (tl ? tileTitle(*tl) : std::string("no tile"));
        t.body = t.kind == "waiting" ? "waiting for you" : t.kind == "ask" ? "asks 2 questions" : "done after 42 s";
        t.silent = n.is_object() && n.contains("sound") && n["sound"].is_boolean() && !n["sound"].get<bool>();
        sh.notifier.toast(t);
      }
    }
    else if (c == "event") {                            // event <name> <json>: an `event` frame, as the deck would send it (input:refused {'reason':'remote'})
      const size_t sp = s.rest.find(' '); std::string js = sp == std::string::npos ? "null" : s.rest.substr(sp + 1);
      for (char& ch : js) if (ch == '\'') ch = '"';
      json v = json::parse(js, nullptr, false);
      if (s.args.empty() || v.is_discarded()) logLine("BAD EVENT: " + s.rest); else onMessage(json{ { "t", "event" }, { "name", s.args[0] }, { "payload", v } });
    }
    else if (c == "life") {                             // the lifecycle (lifecycle.hpp): what a restart would start, a rebuilt exe, a second launch
      Shell& sh = shell();
      if (s.rest == "bump") { sh.life.salt += "+"; logLine("lifecycle: the exe's fingerprint now reads " + sh.life.fingerprint().dump()); }
      else if (s.rest == "spec") logLine("relaunch spec: " + narrow(sh.life.spec().commandLine));
      else if (s.rest == "second") { if (sh.hub) SendMessageW(sh.hub, SingleInstance::message(), 0, 0); }
      else if (s.rest == "lock") logLine(std::string("single instance: lock held ") + std::to_string(sh.single.held()) + ", a second launch finds the hub " + std::to_string(SingleInstance::findFirst(sh.single.key()) == sh.hub && sh.hub != nullptr));
      else if (s.rest == "deskwindow") {
        // (tests) the core's half of "a desk with no free cell", without a Claude session: the core registers a new
        // window client holding the selected tile, then the SHELL is asked for its window through the core's own
        // seam (core::Lifecycle::openWindow, the virtual call the deck makes) — bounds as placeDeskTile gives them
        const json b = boundsJson(); const int bw = b.value("width", 1200), bh = b.value("height", 800);
        const json bounds = { { "width", std::max(800, (int)std::lround(bw * 0.8)) }, { "height", std::max(500, (int)std::lround(bh * 0.8)) }, { "x", 80 + (int)std::lround(bw * 0.1) }, { "y", 80 + (int)std::lround(bh * 0.1) } };
        const core::ClientId cid = sh.host.running() ? sh.host.core().openWindow(st.selected, bounds) : 0;
        core::Lifecycle& asCore = sh.life;
        const bool made = cid && asCore.openWindow((int)cid, bounds, true);
        logLine("desk window: client " + std::to_string(cid) + " registered by the core, the shell " + (made ? "made its window" : "made NO window") + ", " + std::to_string(sh.apps.size()) + " windows");
      }
      else logLine("UNKNOWN LIFE STEP: " + s.rest);
    }
    else logLine("UNKNOWN STEP: " + s.cmd);
    return delay;
  }
  // ---- --do `ask …`: the ask pane and its page, driven as windows/web-test drives them. A click in the page is a
  // REAL (trusted) mouse event at page coordinates, sent through the web view's DevTools input; nothing is typed
  // or clicked through the system's input queue. Each step starts something and returns: follow it with a `wait`.
  void askRealClick(double x, double y) {
    for (const char* type : { "mousePressed", "mouseReleased" })
      askPane.web().devtoolsCall(L"Input.dispatchMouseEvent", json{ { "type", type }, { "x", x }, { "y", y }, { "button", "left" }, { "clickCount", 1 } }.dump(), nullptr);
  }
  // the middle of an element of the HOST document (`el`: a js expression that gives it), brought into view, clicked
  void askClickHost(const std::string& el) {
    askPane.web().exec("(function(){var e=" + el + ";if(!e)return null;var sc=document.querySelector('.ap-scroll'),r=e.getBoundingClientRect();"
      "if(r.top<60||r.bottom>sc.clientHeight-10){sc.scrollTop+=r.top-sc.clientHeight/2;r=e.getBoundingClientRect();}return [r.left+r.width/2,r.top+r.height/2];})()",
      [this, el](bool ok, const std::string& r) {
        json p2 = json::parse(r, nullptr, false);
        if (!ok || !p2.is_array()) { logLine("ASK: no host element: " + el); return; }
        logLine("ask click (host) " + p2.dump() + "  " + el);
        askRealClick(p2[0].get<double>(), p2[1].get<double>());
      });
  }
  // the same for an element of the n-th page on show (a sandboxed frame of another origin): the page says where the
  // element is in ITS window, the host document where that frame is, and the click lands on the sum
  void askClickFrame(int n, const std::string& el) {
    const std::string frameJs = "[...document.querySelectorAll('iframe')].filter(function(f){return f.getClientRects().length})[" + std::to_string(n) + "]";
    askPane.web().exec("(function(){var f=" + frameJs + ";return f?f.name:null})()", [this, n, el, frameJs](bool ok, const std::string& r) {
      json nm = json::parse(r, nullptr, false);
      if (!ok || !nm.is_string()) { logLine("ASK: no page frame " + std::to_string(n)); return; }
      const std::string name = nm.get<std::string>();
      askPane.web().execInFrame(widen(name), "(function(){var e=" + el + ";if(!e)return null;var r=e.getBoundingClientRect();return [r.left+r.width/2,r.top+r.height/2];})()", [this, name, el, frameJs](bool ok2, const std::string& r2) {
        json in = json::parse(r2, nullptr, false);
        if (!ok2 || !in.is_array()) { logLine("ASK: no element in page " + name + ": " + el); return; }
        askPane.web().exec("(function(){var f=" + frameJs + ";if(!f)return null;var sc=document.querySelector('.ap-scroll'),r=f.getBoundingClientRect(),y=r.top+" + in[1].dump() +
          ";if(y<90||y>sc.clientHeight-20){sc.scrollTop+=y-sc.clientHeight/2;r=f.getBoundingClientRect();}return [r.left+" + in[0].dump() + ",r.top+" + in[1].dump() + "];})()",
          [this, name, el](bool ok3, const std::string& r3) {
            json at = json::parse(r3, nullptr, false);
            if (!ok3 || !at.is_array()) { logLine("ASK: the page's frame went: " + name); return; }
            logLine("ask click (page " + name + ") " + at.dump() + "  " + el);
            askRealClick(at[0].get<double>(), at[1].get<double>());
          });
      });
    });
  }
  // ---- --do `cloud …`: the cloud panes and their pages (a test's page is its local stand-in, never claude.ai)
  //   cloud dump                        every pane's state
  //   cloud eval <tile|sel> <js>        a value from that pane's page, to the log (no `;` in js)
  //   cloud click <tile|sel> <js>       a REAL click on an element of the page (js gives the element)
  //   cloud text <tile|sel> <words>     typed into whatever has the caret in that page
  //   cloud key <tile|sel> <spec>       the keyboard into the page, then the key POSTED to the web view's own window
  //   cloud focus <tile|sel> [user]     the keyboard into the page (user: as a click into the page moves it there)
  //   cloud age <tile|sel> <ms>         the pane's off-screen clock wound on (cloud.hpp CLOUD_OFFSCREEN_CLOSE_MS)
  void cloudStep(const ScriptStep& s) {
    auto arg = [&](size_t i) { return i < s.args.size() ? s.args[i] : std::string(); };
    const std::string sub = arg(0), id = resolveId(arg(1));
    std::string tail; { size_t a = s.rest.find(' '); if (a != std::string::npos) { size_t b2 = s.rest.find(' ', a + 1); if (b2 != std::string::npos) tail = s.rest.substr(b2 + 1); } }
    if (sub == "dump") {
      std::ostringstream o; o << cloudPanes.dumpState();
      { HWND f = GetFocus(); wchar_t fc[80] = L"(none of this thread's)"; if (f) GetClassNameW(f, fc, 80); o << "cloud focus: this thread's focus window is " << (f == hwnd ? std::string("the app's own window") : narrow(fc)) << "; page with the keyboard '" << cloudPanes.focusedId() << "'; selected " << st.selected << " termKeys " << termKeys() << " windowFocused " << windowFocused; }
      logLine(o.str()); return;
    }
    web::WebPane* w = cloudPanes.webOf(id);
    if (sub == "age") { cloudPanes.testAge(id, (unsigned)atoi(arg(2).c_str())); return; }
    if (!w) { logLine("CLOUD: no page for '" + arg(1) + "'"); return; }
    if (sub == "eval") w->exec(tail, [this, tail, id](bool ok, const std::string& r) { logLine("cloud eval " + id + " " + std::string(ok ? "" : "FAILED ") + r.substr(0, 1200) + "   <- " + tail.substr(0, 60)); });
    else if (sub == "click") w->exec("(function(){var e=" + tail + ";if(!e)return null;e.scrollIntoView({block:'center'});var r=e.getBoundingClientRect();return [r.left+r.width/2,r.top+r.height/2];})()", [this, tail, id](bool ok, const std::string& r) {
      json at = json::parse(r, nullptr, false);
      if (!ok || !at.is_array()) { logLine("CLOUD: no element in " + id + ": " + tail); return; }
      logLine("cloud click " + id + " " + at.dump() + "  " + tail);
      web::WebPane* w2 = cloudPanes.webOf(id); if (!w2) return;
      for (const char* type : { "mousePressed", "mouseReleased" }) w2->devtoolsCall(L"Input.dispatchMouseEvent", json{ { "type", type }, { "x", at[0] }, { "y", at[1] }, { "button", "left" }, { "clickCount", 1 } }.dump(), nullptr);
    });
    else if (sub == "text") w->devtoolsCall(L"Input.insertText", json{ { "text", tail } }.dump(), nullptr);
    else if (sub == "focus") cloudPanes.focus(id, arg(2) == "user");
    else if (sub == "key") {
      UINT vk; Mods m;
      if (!parseKeySpec(tail, vk, m)) { logLine("NO KEY: " + tail); return; }
      cloudPanes.focus(id);
      std::vector<std::pair<HWND, std::wstring>> wins; EnumChildWindows(hwnd, askKeyEnum, (LPARAM)&wins);
      HWND f = GetFocus(); wchar_t fc[80] = L"-"; if (f) GetClassNameW(f, fc, 80);
      logLine("cloud key " + tail + " -> " + id + ": " + std::to_string(wins.size()) + " web windows under this one, this thread's focus window: " + narrow(fc) + (f == hwnd ? " (the app's own)" : ""));
      const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
      bool one = false; for (auto& x : wins) if (x.first == f) one = true;
      if (!one) { logLine("CLOUD: the page does not have the keyboard: the key is not posted"); return; }
      PostMessageW(f, WM_KEYDOWN, vk, 1 | (sc << 16)); PostMessageW(f, WM_KEYUP, vk, 1 | (sc << 16) | 0xC0000000);
    }
    else logLine("UNKNOWN CLOUD STEP: " + s.rest);
  }
  // ---- --do `webmem <label>`: what the web views cost right now. Every msedgewebview2.exe that DESCENDS from this
  // process (the browser process WebView2 starts is our child, its GPU / network / renderer processes are its
  // children): how many, their summed working set and private bytes. Other programs on the machine run
  // msedgewebview2.exe too: those are never counted (not our descendants) and nothing is ever done to any process.
  static std::string webMem(const std::string& label) {
    struct P { DWORD pid, ppid; std::wstring name; ULONGLONG born; };
    std::vector<P> all;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return "WEBMEM " + label + ": no process list";
    PROCESSENTRY32W e; e.dwSize = sizeof e;
    auto bornAt = [](DWORD pid) -> ULONGLONG {
      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) return 0;
      FILETIME c, x, k, u; ULONGLONG t = 0;
      if (GetProcessTimes(h, &c, &x, &k, &u)) t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
      CloseHandle(h); return t;
    };
    if (Process32FirstW(snap, &e)) do { all.push_back({ e.th32ProcessID, e.th32ParentProcessID, e.szExeFile, 0 }); } while (Process32NextW(snap, &e));
    CloseHandle(snap);
    std::map<DWORD, ULONGLONG> mine; mine[GetCurrentProcessId()] = bornAt(GetCurrentProcessId());
    for (bool grew = true; grew;) {
      grew = false;
      for (auto& p : all) {
        if (mine.count(p.pid)) continue;
        auto par = mine.find(p.ppid); if (par == mine.end()) continue;
        const ULONGLONG b = bornAt(p.pid);
        if (b && par->second && b < par->second) continue;   // older than its "parent": the parent's pid was used again, no child of ours
        mine[p.pid] = b; grew = true;
      }
    }
    // what kind of process each is: the value of `--type=` on its command line (none = the browser process itself).
    // Only that one word is taken; a command line is never logged.
    auto kindOf = [](HANDLE h) -> std::string {
      typedef LONG (WINAPI* NtQIP)(HANDLE, int, PVOID, ULONG, PULONG);
      static NtQIP q = (NtQIP)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
      if (!q || !h) return "?";
      std::vector<BYTE> buf(8192); ULONG need = 0;
      LONG st = q(h, 60 /* ProcessCommandLineInformation */, buf.data(), (ULONG)buf.size(), &need);
      if (st < 0 && need > buf.size()) { buf.resize(need); st = q(h, 60, buf.data(), (ULONG)buf.size(), &need); }
      if (st < 0) return "?";
      struct US { USHORT len, max; PWSTR buf; }; const US* u = (const US*)buf.data();
      const std::wstring cl(u->buf ? u->buf : L"", u->buf ? u->len / sizeof(wchar_t) : 0);
      auto word = [&](const wchar_t* key) { const size_t a = cl.find(key); if (a == std::wstring::npos) return std::string(); size_t b = a + wcslen(key), e = b; while (e < cl.size() && cl[e] != L' ' && cl[e] != L'"') e++; return narrow(cl.substr(b, e - b)); };
      std::string t = word(L"--type="); if (t.empty()) return "browser";
      const std::string sub = word(L"--utility-sub-type="); if (!sub.empty()) { const size_t dot = sub.find_last_of('.'); t = sub.substr(dot == std::string::npos ? 0 : dot + 1); }
      if (t == "gpu-process") t = "gpu"; if (t == "crashpad-handler") t = "crash";
      return t;
    };
    int n = 0, direct = 0; ULONGLONG ws = 0, priv = 0; std::string each;
    for (auto& p : all) {
      if (!mine.count(p.pid) || _wcsicmp(p.name.c_str(), L"msedgewebview2.exe") != 0) continue;
      n++; if (p.ppid == GetCurrentProcessId()) direct++;   // a browser process: the root of one process tree
      PROCESS_MEMORY_COUNTERS_EX mc; memset(&mc, 0, sizeof mc); mc.cb = sizeof mc;
      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
      if (h && GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&mc, sizeof mc)) { ws += mc.WorkingSetSize; priv += mc.PrivateUsage; char b[64]; snprintf(b, sizeof b, " %.0f/%.0f", mc.WorkingSetSize / 1048576.0, mc.PrivateUsage / 1048576.0); each += " " + kindOf(h) + b; }
      else each += " ?";
      if (h) CloseHandle(h);
    }
    char b[240]; snprintf(b, sizeof b, ": processes %d, working set %.1f MB, private %.1f MB, browser processes (our own children) %d", n, ws / 1048576.0, priv / 1048576.0, direct);
    return "WEBMEM " + label + b + (n ? "  [each: kind ws/private MB:" + each + "]" : "");
  }
  // ---- --do `webscan [word …]`: is anything of the deck's token on disk RIGHT NOW (the web views may be up)? Every
  // file under the test's state folder and under this process's own temp web profiles is read, and looked through
  // for the link's token (as bytes and as UTF-16) and for each word given (a cookie's NAME: Chromium encrypts
  // cookie values on disk, so a stored cookie shows by its name, not by its value). The token is never logged.
  void webScan(const ScriptStep& s) {
    std::vector<std::pair<std::string, std::string>> words;   // what the log calls it, the bytes
    if (!link.token.empty()) words.push_back({ "the deck's token", link.token });
    for (auto& a : s.args) if (!a.empty()) words.push_back({ "'" + a + "'", a });
    std::vector<std::wstring> roots;
    if (!opt.stateDir.empty()) roots.push_back(opt.stateDir);
    { wchar_t t[MAX_PATH + 2] = L""; GetTempPathW(MAX_PATH, t); std::wstring base = std::wstring(t) + L"ClaudeDeckNative-web";
      WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((base + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) { do { if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) roots.push_back(base + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd)); FindClose(h); } }
    int files = 0, unread = 0; ULONGLONG bytes = 0; std::vector<std::string> hits; std::string cookieStores, locked;
    std::function<void(const std::wstring&)> walk = [&](const std::wstring& dir) {
      WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd); if (h == INVALID_HANDLE_VALUE) return;
      do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) walk(p); continue; }
        files++;
        if (!_wcsicmp(fd.cFileName, L"Cookies")) cookieStores += " " + narrow(p.substr(p.find(L"EBWebView") != std::wstring::npos ? p.find(L"EBWebView") : 0)) + " (" + std::to_string(fd.nFileSizeLow) + " bytes)";
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) { unread++; locked += std::string(" ") + narrow(fd.cFileName); continue; }
        std::string buf; LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(f, &sz);
        if (sz.QuadPart > 0 && sz.QuadPart < (512ll << 20)) { buf.resize((size_t)sz.QuadPart); DWORD got = 0; if (!ReadFile(f, &buf[0], (DWORD)buf.size(), &got, nullptr)) got = 0; buf.resize(got); }
        CloseHandle(f); bytes += buf.size();
        for (auto& w : words) {
          std::string w16; for (char c : w.second) { w16 += c; w16 += '\0'; }
          if (buf.find(w.second) != std::string::npos || buf.find(w16) != std::string::npos) hits.push_back(w.first + " in " + narrow(p));
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    };
    for (auto& r : roots) walk(r);
    std::string o = "WEBSCAN: " + std::to_string(files) + " files (" + std::to_string(bytes / 1024) + " KB read, " + std::to_string(unread) + " could not be opened) under " + std::to_string(roots.size()) + " folder(s); looked for";
    for (auto& w : words) o += " " + w.first;
    o += ": " + std::to_string(hits.size()) + " hit(s)" + (hits.empty() ? " (nothing of it is on disk)" : "  <-- ON DISK");
    for (auto& h : hits) o += "\n  " + h;
    o += "\n  cookie stores on disk:" + (cookieStores.empty() ? std::string(" none") : cookieStores);
    if (unread) o += "\n  could not be opened while the browser runs (it holds them alone; they are read after the run instead):" + locked;
    logLine(o);
  }
  static BOOL CALLBACK askKeyEnum(HWND c, LPARAM lp) {
    wchar_t cls[80] = L""; GetClassNameW(c, cls, 80);
    if (wcsncmp(cls, L"Chrome_", 7) == 0) ((std::vector<std::pair<HWND, std::wstring>>*)lp)->push_back({ c, cls });
    return TRUE;
  }
  void askStep(const ScriptStep& s) {
    const std::string sub = s.args.empty() ? "" : s.args[0];
    const size_t sp = s.rest.find(' ');
    const std::string tail = sp == std::string::npos ? "" : s.rest.substr(sp + 1);                 // everything after the sub-command
    const size_t sp2 = tail.find(' ');
    const std::string tail2 = sp2 == std::string::npos ? "" : tail.substr(sp2 + 1);                 // … and after its first word
    auto arg = [&](size_t i) { return i < s.args.size() ? s.args[i] : std::string(); };
    if (sub == "open") { std::string mode = arg(1).empty() ? "all" : arg(1); if (mode == "sel") { const json* t = tile(st.selected); mode = t ? str(*t, "sessionId") : ""; } openAsks(mode, arg(2) == "recent" ? asks::AskPane::RECENT : arg(2)); }
    else if (sub == "close") askPane.closePane();
    else if (sub == "warm") { askSync(); askPane.prewarm(); }
    else if (sub == "click") askRealClick(atof(arg(1).c_str()), atof(arg(2).c_str()));              // page coordinates (css px from the web view's corner)
    else if (sub == "host") askClickHost(tail);                                                     // ask host <js: an element of the pane's own document>
    else if (sub == "in") askClickFrame(atoi(arg(1).c_str()), tail2);                               // ask in <n> <js: an element of the n-th page on show>
    else if (sub == "text") askPane.web().devtoolsCall(L"Input.insertText", json{ { "text", tail } }.dump(), nullptr);   // into whatever has the caret in the page
    else if (sub == "eval") {                                                                       // ask eval <n | -> <js>: its value, to the log
      auto done = [this, tail2](bool ok, const std::string& r) { logLine("ask eval " + std::string(ok ? "" : "FAILED ") + r.substr(0, 1500) + "   <- " + tail2.substr(0, 60)); };
      if (arg(1) == "-") askPane.web().exec(tail2, done);
      else {
        const int n = atoi(arg(1).c_str());
        askPane.web().exec("(function(){var f=[...document.querySelectorAll('iframe')].filter(function(f){return f.getClientRects().length})[" + std::to_string(n) + "];return f?f.name:null})()", [this, tail2, done](bool ok, const std::string& r) {
          json nm = json::parse(r, nullptr, false);
          if (!ok || !nm.is_string()) { done(false, "no such page frame"); return; }
          askPane.web().execInFrame(widen(nm.get<std::string>()), tail2, done);
        });
      }
    }
    else if (sub == "focus") { askPane.focusPage(); }
    // the native pane (askcards.hpp): what it shows (pages, cards, their state, the sections), and a REAL click on one
    // of its things through the window's own handler (as appclick): ask nclick ui done [n] | pick <page n> <qid> <k> |
    // box <page n> <qid> | copy <page n> <qid> | link <n> | lhead <n>  (scrolled into sight first, as a person would)
    else if (sub == "native") logLine("ask native: " + askPane.cards().dump().dump(-1, ' ', false, json::error_handler_t::replace));
    else if (sub == "nclick") {
      std::vector<std::string> w(s.args.begin() + 1, s.args.end()); float x = 0, y = 0;
      if (!askPane.native() || !askPane.cards().targetCenter(w, x, y)) { logLine("ask nclick: NO SUCH TARGET " + tail); return; }
      char b[64]; snprintf(b, sizeof b, " at %.0f,%.0f", x, y); logLine("ask nclick " + tail + b);
      invalidate();
      if (hwnd && !headless()) { const LPARAM at = MAKELPARAM((int)std::lround(x * dpi / 96), (int)std::lround(y * dpi / 96)); lastDownAt = 0; SendMessageW(hwnd, WM_MOUSEMOVE, 0, at); SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at); SendMessageW(hwnd, WM_LBUTTONUP, 0, at); }
      else { lastDownAt = 0; onMouseDown(x, y, false); onMouseUp(x, y); }
    }
    // a press and a release through the window's OWN message handler (WM_LBUTTONDOWN: SetFocus and all), at DIPs: a
    // message sent to this window, not input. What a click on a tile or a pane does to the keyboard is this path.
    else if (sub == "appclick") { const LPARAM at = MAKELPARAM((int)std::lround(atof(arg(1).c_str()) * dpi / 96), (int)std::lround(atof(arg(2).c_str()) * dpi / 96)); lastDownAt = 0; SendMessageW(hwnd, WM_MOUSEMOVE, 0, at); SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at); SendMessageW(hwnd, WM_LBUTTONUP, 0, at); }
    else if (sub == "key") {
      // The one thing no headless run could show so far: a window key pressed while the PAGE has the keyboard
      // (AcceleratorKeyPressed). No real key may be pressed (SendInput would type into whatever the user has in
      // front of them), so: the keyboard is moved into the web view (MoveFocus), and the key is POSTED to the web
      // view's own windows under this one. Whether the browser treats a posted key as pressed is what this reports.
      UINT vk; Mods m;
      if (!parseKeySpec(tail, vk, m)) { logLine("NO KEY: " + tail); return; }
      askPane.focusPage();
      std::vector<std::pair<HWND, std::wstring>> wins; EnumChildWindows(hwnd, askKeyEnum, (LPARAM)&wins);
      std::string names; for (auto& w : wins) { names += narrow(w.second) + (GetFocus() == w.first ? "(focus) " : " "); }
      HWND f = GetFocus(); wchar_t fc[80] = L"-"; if (f) GetClassNameW(f, fc, 80);
      logLine("ask key " + tail + ": " + std::to_string(wins.size()) + " web windows under this one [" + names + "], this thread's focus window: " + narrow(fc) + (f == hwnd ? " (the app's own)" : "") + ", accelerators seen so far " + std::to_string(askPane.acceleratorsSeen()));
      const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
      // to the one that has this thread's focus after MoveFocus (the browser's own top window under ours); to all of them if none has
      bool one = false; for (auto& w : wins) if (w.first == f) one = true;
      for (auto& w : wins) { if (one && w.first != f) continue; PostMessageW(w.first, WM_KEYDOWN, vk, 1 | (sc << 16)); PostMessageW(w.first, WM_KEYUP, vk, 1 | (sc << 16) | 0xC0000000); }
    }
    else if (sub == "dump") {
      std::ostringstream o;
      o << "ask pane: mode '" << askPane.mode() << "' open " << askPane.isOpen() << " title '" << askPane.barTitle() << "' hint '" << askPane.barHint() << "'\n";
      o << "ask web: state " << (int)askPane.web().state() << " (2 = ready) made " << askPane.webMade() << " host document ready " << askPane.hostReady() << " visible " << askPane.web().visible() << " memory target " << askPane.web().memoryTarget() << " covered " << askPane.covered()
        << " minimised " << askPane.isMinimised() << " suspended " << askPane.web().suspended() << " closed unseen " << askPane.webCloses() << " times; InPrivate (the runtime's word) " << askPane.web().privateMode() << " profile folder '" << narrow(askPane.web().profilePath()) << "' retired profiles " << askPane.retiredProfiles() << " page has the keyboard " << askPane.webFocused() << " tile selected " << askPane.asksFocused() << " pick '" << askPane.asksPick() << "'\n";
      o << "ask timing: create() " << askPane.createCallMs() << " ms on the window thread; web view ready +" << askPane.webReadyMs() << " ms; host document ready +" << askPane.hostReadyMs() << " ms (from the create); the last open to its first page ready: " << askPane.firstPageMs() << " ms\n";
      { HWND f = GetFocus(); wchar_t fc[80] = L"(none of this thread's)"; if (f) GetClassNameW(f, fc, 80); o << "ask focus: this thread's focus window is " << (f == hwnd ? std::string("the app's own window") : narrow(fc)) << "; windowFocused " << windowFocused << "\n"; }
      o << "ask keys from the web view (AcceleratorKeyPressed): " << askPane.acceleratorsSeen() << ", last vk " << askPane.lastAccelerator() << "; focusInTerminal " << st.focusInTerminal << " termKeys " << termKeys() << " note '" << paneNoteText << "'";
      logLine(o.str());
      if (askPane.native()) logLine("ask native: " + askPane.cards().dump().dump(-1, ' ', false, json::error_handler_t::replace));
      else if (askPane.hostReady()) askPane.web().exec("(function(){var S=[...document.querySelectorAll('section.ap-sess:not(.ap-recent)')].map(function(e){return {name:e.querySelector('.ap-name').textContent,hidden:e.hidden,count:e.querySelector('.ap-count').textContent,done:e.querySelector('.ap-done').textContent,doneOff:e.querySelector('.ap-done').disabled,mark:e.querySelector('.ap-mark').className+' '+e.querySelector('.ap-mark').textContent,flash:e.querySelector('.ap-flash').textContent,notes:[...e.querySelectorAll('.ap-rnote')].map(function(n){return n.textContent}),frames:[...e.querySelectorAll('iframe')].map(function(f){return Math.round(f.getBoundingClientRect().height)})}}),"
        "r=document.querySelector('.ap-recent');return {sections:S,recent:{hidden:r.hidden,head:r.querySelector('.ap-rhead').hidden,meta:r.querySelector('.ap-meta').textContent,rows:[...r.querySelectorAll('.ap-lrow')].map(function(x){return (x.classList.contains('open')?'open ':'')+x.querySelector('.ap-name').textContent+' | '+x.querySelector('.ap-meta').textContent+' | frames '+x.querySelectorAll('iframe').length})},empty:document.querySelector('.ap-empty').hidden?null:document.querySelector('.ap-empty').textContent,scrollTop:document.querySelector('.ap-scroll').scrollTop,active:document.activeElement?document.activeElement.tagName:null}})()",
        [this](bool ok, const std::string& r) { logLine("ask host document: " + (ok ? r : std::string("(no answer)"))); });
    }
    else logLine("UNKNOWN ASK STEP: " + s.rest);
  }
  void dump(const std::string& what) {
    std::ostringstream o;
    auto rc = [](const RectF& r) { char b[96]; snprintf(b, sizeof b, "[%.0f,%.0f %.0fx%.0f]", r.x, r.y, r.w, r.h); return std::string(b); };
    if (what.rfind("ghost", 0) == 0) {                  // dump ghost [name]: the picture a tile dragged OUT of the window carries (no window is made in a test)
      Gfx g2; int pw = 0, ph = 0;
      if (!renderGhost(g2, pw, ph, shotScale())) o << "ghost: no drag in progress\n";
      else {
        const std::wstring& base = !opt.shot.empty() ? opt.shot : opt.windowShot;
        const size_t sl = base.find_last_of(L"\\/"); const std::string nm = what.size() > 6 ? what.substr(6) : std::string("ghost");
        const std::wstring path = (sl == std::wstring::npos ? L"" : base.substr(0, sl + 1)) + L"n-" + widen(nm) + L".png";
        const bool outside = st.mouseX < 0 || st.mouseY < 0 || st.mouseX > viewW || st.mouseY > viewH;
        o << "ghost " << pw << "x" << ph << " px for " << dragSrc << ", pointer " << (outside ? "outside" : "inside") << " the window, saved " << g2.savePng(path) << " n-" << nm << ".png, window made " << (ghostWnd != nullptr) << "\n";
      }
    }
    else if (what == "phone") {                              // Settings -> Phone's facts, straight from the deck (the token never reaches a log)
      json ph = localMode ? shell().host.phoneStatus() : json();
      if (ph.is_object()) {
        const std::string tok = str(ph, "token");
        if (!tok.empty()) { std::string u = str(ph, "url"); const size_t at = u.find(tok); if (at != std::string::npos) u.replace(at, tok.size(), "<token>"); ph["url"] = u; ph["token"] = "<" + std::to_string(tok.size()) + " chars, hash " + std::to_string(std::hash<std::string>()(tok) % 100000) + ">"; }
      }
      o << "phone " << ph.dump() << (shell().host.testBindRefused().empty() ? "" : "  TEST BIND REFUSED " + shell().host.testBindRefused()) << "\n";
      if (sset.open) o << "settings phone-assets '" << narrow(sset.phoneAssets) << "'\n";
    }
    else if (what == "windows") {                       // every window of the process, and what the deck says of the tiles
      Shell& sh = shell();
      o << "windows " << sh.apps.size() << " (deck clients " << sh.host.windowCount() << ")\n";
      for (App* a : sh.apps) {
        json lay = json::object(); for (const auto& kv : a->layout) lay[kv.first] = kv.second;
        std::string shown; for (const auto& id : a->st.shown) shown += (shown.empty() ? "" : ",") + id;
        o << "window w" << a->st.windowIndex << (a->primary ? " primary" : "") << " inited " << a->inited << " " << (int)a->viewW << "x" << (int)a->viewH << " selected " << a->st.selected << " layout " << lay.dump() << " shown [" << shown << "] title '" << a->windowTitle() << "'\n";
      }
      if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"])
        o << "tile " << str(t, "id") << " '" << tileTitle(t) << "' inWindow " << (t.contains("inWindow") ? t["inWindow"].dump() : std::string("-")) << " shownBy " << (t.contains("shownBy") ? t["shownBy"].dump() : std::string("-")) << " exited " << (t.contains("exited") && t["exited"].is_boolean() && t["exited"].get<bool>()) << "\n";
      if (localMode && sh.host.running()) o << "saved windows (the state file at start) " << sh.host.core().savedWindows().dump() << "\n";
    }
    else if (what == "presence") {                      // meta.presence and the file the hosted sessions read
      o << "presence " << (meta.contains("presence") ? meta["presence"].dump() : std::string("-")) << "\n";
      if (localMode && shell().host.running()) {
        std::ifstream f(widen(shell().host.stateDir() + "\\deck-state.json").c_str(), std::ios::binary); std::stringstream ss; ss << f.rdbuf();
        const json d = json::parse(ss.str(), nullptr, false);
        if (d.is_object()) o << "deck-state.json where " << (d.contains("where") ? d["where"].dump() : "-") << " lastInputFrom " << (d.contains("lastInputFrom") ? d["lastInputFrom"].dump() : "-") << " deck " << (d.contains("deck") ? d["deck"].dump() : "-") << "\n";
        else o << "deck-state.json unreadable\n";
      }
    }
    else if (what == "sidebar") o << "sidebar width " << sidebarW() << " min " << sbMin() << " remembered " << st.sidebarWidth << "\n";   // the drawn width, --sb-min, the dragged one
    else if (what == "restart") o << "restart " << (meta.contains("restart") ? meta["restart"].dump() : std::string("-")) << "\n";
    // the selected pane's grid as last drawn: where its text starts, a cell, cols x rows (a script aims at rows and edges)
    else if (what == "grid") { auto it = terms.find(st.selected); if (it == terms.end()) o << "grid none\n"; else { const TermView& v = *it->second; o << "grid " << v.lastOx << "," << v.lastOy << " cell " << v.lastCell.w << "x" << v.lastCell.h << " " << v.term.cols() << "x" << v.term.rows() << " bottom " << (v.lastOy + v.term.rows() * v.lastCell.h) << "\n"; } }
    else if (what == "zones") { for (const auto& h : sidebar.all()) o << "zone " << h.kind << " " << h.id << (h.arg.empty() ? "" : " (" + h.arg + ")") << " " << rc(h.rect) << "\n"; }
    // the sidebar's tooltips (title="" in the renderer), one per zone that has one
    else if (what == "tips") { for (const auto& h : sidebar.all()) { const std::wstring tp = sidebar.tooltipAt(h.rect.x + h.rect.w / 2, h.rect.y + h.rect.h / 2); if (!tp.empty()) { std::string t1 = narrow(tp); for (char& ch : t1) if (ch == '\n') ch = '|'; o << "tip " << h.kind << " " << h.id << (h.arg.empty() ? "" : " (" + h.arg + ")") << " '" << t1 << "'\n"; } } }
    // dump mem: what this process holds now (windows/test/soak.ps1): private bytes + working set, the text-run cache,
    // and every terminal's lines and cells (what its scroll-back costs), so a soak can say WHICH part grows
    else if (what == "mem drop") {                      // the text-run cache emptied: what it held, in private bytes
      PROCESS_MEMORY_COUNTERS_EX m0; memset(&m0, 0, sizeof m0); m0.cb = sizeof m0; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m0, sizeof m0);
      const size_t n = gfx.runCacheSize(); gfx.runCacheDrop();
      PROCESS_MEMORY_COUNTERS_EX m1; memset(&m1, 0, sizeof m1); m1.cb = sizeof m1; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m1, sizeof m1);
      char b[200]; snprintf(b, sizeof b, "MEM drop runs %zu: private %.1f -> %.1f MB (%.2f KB a run)\n", n, m0.PrivateUsage / 1048576.0, m1.PrivateUsage / 1048576.0, n ? ((double)m0.PrivateUsage - (double)m1.PrivateUsage) / 1024.0 / n : 0.0);
      o << b;
    }
    else if (what == "mem") {
      PROCESS_MEMORY_COUNTERS_EX mc; memset(&mc, 0, sizeof mc); mc.cb = sizeof mc;
      GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&mc, sizeof mc);
      size_t lines = 0, cells = 0, sb = 0;
      for (const auto& kv : terms) { const vt::Terminal& t = kv.second->term; const int n = t.bufferLines(); sb += (size_t)t.scrollbackSize(); lines += (size_t)n; for (int i = 0; i < n; i++) cells += t.bufferLine(i).cells.capacity(); }
      char b[600];
      snprintf(b, sizeof b, "MEM private %.1f MB ws %.1f MB | runs %zu (%s) | sidebar %s | terms %zu lines %zu scrollback %zu cells %zu = %.1f MB | frames %u\n",
        mc.PrivateUsage / 1048576.0, mc.WorkingSetSize / 1048576.0, gfx.runCacheSize(), gfx.runCacheNote().c_str(), ui::textKeptNote().c_str(), terms.size(), lines, sb, cells, cells * sizeof(vt::Cell) / 1048576.0, sceneStats.frames);
      o << b;
    }
    else if (what == "tiles") {
      o << "window '" << windowTitle() << "'  tiles " << tileIds().size() << "\n";
      if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"]) {
        o << "tile " << str(t, "id") << " " << str(t, "kind") << " '" << tileTitle(t) << "' status " << str(t, "status") << " pinned " << t.value("pinned", false)
          << " exited " << (t.contains("exited") && t["exited"].is_boolean() && t["exited"].get<bool>()) << " pty " << (t.contains("ptySize") ? t["ptySize"].dump() : std::string("-")) << "\n";
      }
    }
    else if (what == "auditors") {                      // each tile's auditor / child / pause and its files from an auditor, as the deck said them
      if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"]) {
        auto field = [&](const char* k) { return t.contains(k) && !t[k].is_null() ? t[k].dump() : std::string("-"); };
        o << "aud tile " << str(t, "id") << " '" << tileTitle(t) << "' kind " << str(t, "kind") << " status " << str(t, "status") << " auditor " << field("auditor") << " child " << field("child") << " pause " << field("pause") << " words '" << deck::ui::pauseWords(t) << "'\n";
        if (t.contains("auditorMail") && t["auditorMail"].is_array()) for (const auto& m : t["auditorMail"])
          o << "aud mail " << str(t, "id") << " " << str(m, "id") << " " << str(m, "name") << " from '" << str(m, "from") << "' state " << str(m, "state") << " first '" << str(m, "firstLine") << "' sentAt " << (m.contains("sentAt") && m["sentAt"].is_number() ? "yes" : "no") << "\n";
      }
    }
    else if (what == "screen") {                        // the selected pane's text, row by row (trailing blanks cut)
      auto it = terms.find(st.selected);
      if (it != terms.end()) {
        const TermView& v = *it->second;
        o << "screen " << st.selected << " " << v.term.cols() << "x" << v.term.rows() << " cursor " << v.term.cursorX() << "," << v.term.cursorY() << " scrollback " << v.term.scrollbackSize() << "\n";
        for (int y = 0; y < v.term.rows(); y++) {
          std::u32string row; const vt::Line& ln = v.term.line(y);
          for (int x = 0; x < v.term.cols() && x < (int)ln.cells.size(); x++) { const vt::Cell& c2 = ln.cells[x]; if (c2.width == 0) continue; if (c2.ch == 0) row.push_back(U' '); else row += v.term.cellText(c2); }
          while (!row.empty() && row.back() == U' ') row.pop_back();
          if (!row.empty()) o << "  |" << narrow(TermView::utf16(row)) << "\n";
        }
      }
    }
    else if (what == "drive") {                         // the driver button: the deck's words, the value, and the tooltip as drawn
      const ui::Hit* h = sidebar.find("tool", "drive-btn", "-");
      const std::string d = setting("driver").is_string() ? setting("driver").get<std::string>() : "auto";
      const bool nw = ui::newDriverWords(meta);
      o << "drive words " << (nw ? "new" : "old") << " driver " << d << " look " << (!nw ? (d == "phone" ? "on" : d == "desk" ? "off" : "plain") : ui::driverMode(d) == driveMe() ? "mine" : ui::driverMode(d) == "auto" ? "auto" : "other")
        << " remoteDesk " << (meta.contains("remoteDesk") ? meta["remoteDesk"].dump() : std::string("-")) << "\n";
      if (h) o << "drive tip " << narrow(sidebar.tooltipAt(h->rect.x + h->rect.w / 2, h->rect.y + h->rect.h / 2)) << "\n";
      if (meta.contains("tiles") && meta["tiles"].is_array()) for (const auto& t : meta["tiles"]) if (st.shown.count(str(t, "id"))) {
        auto it = terms.find(str(t, "id"));
        o << "drive tile " << str(t, "id") << " sizeBy " << (t.contains("sizeBy") ? t["sizeBy"].dump() : std::string("-")) << " mirror " << (t.contains("mirror") ? t["mirror"].dump() : std::string("-"))
          << " pty " << (t.contains("ptySize") ? t["ptySize"].dump() : std::string("-")) << " draws " << (it != terms.end() ? std::to_string(it->second->term.cols()) + "x" + std::to_string(it->second->term.rows()) : std::string("-")) << " driver " << (t.contains("driver") ? t["driver"].dump() : std::string("-")) << "\n";
      }
    }
    else if (what == "clipboard") o << "clipboard '" << narrow(clipboardText()) << "'\n";
    else if (what == "selection") {                     // the selected pane's selection: where it is (absolute rows) and what it holds
      auto it = terms.find(st.selected);
      if (it == terms.end() || !it->second->sel.active) o << "selection none\n";
      else { const TermView& v = *it->second; int x0, y0, x1, y1; v.sel.ordered(x0, y0, x1, y1); o << "selection " << x0 << "," << y0 << " - " << x1 << "," << y1 << " (top line " << v.topLine() << ") '" << narrow(v.selectionText()) << "'\n"; }
    }
    else if (what == "tips") {                          // the tooltip at each pane name, and the Settings rows that carry one
      for (const auto& kv : nameRects) if (st.shown.count(kv.first)) o << "tip pane-name " << kv.first << ": " << narrow(paneNameTip(kv.first)) << "\n";
      for (const auto& z : sset.tips) { std::string t = narrow(z.tip); for (size_t at; (at = t.find("\r\n")) != std::string::npos;) t.replace(at, 2, " / "); o << "tip settings row [" << (int)z.rect.y << "+" << (int)z.rect.h << "]: " << t.substr(0, 110) << "\n"; }
      for (const auto& z : sset.zones) if (!z.tip.empty()) o << "tip settings " << z.kind << " " << z.id << ": " << narrow(z.tip).substr(0, 80) << "\n";
    }
    else if (what == "fonts") { o << fontsNote << "\nconsole font '" << narrow(monoFamily) << "' (asked for '" << (setting("fontFamily").is_string() ? setting("fontFamily").get<std::string>() : std::string()) << "'), history " << json(fontHistory()).dump() << "\n"; { const CellMetrics& m = metrics(baseFontSize()); o << "cell " << m.w << " x " << m.h << " at " << baseFontSize() << "px\n"; } }
    else if (what == "layout") { json o = json::object(); for (const auto& kv : layout) o[kv.first] = kv.second; logLine("layout " + o.dump() + " shown " + json(std::vector<std::string>(st.shown.begin(), st.shown.end())).dump()); return; }
    else if (what == "ozones") { if (ctx.open) { o << "ctx " << rc(ctx.box) << "\n"; for (size_t i = 0; i < ctx.items.size() && i < ctx.rows.size(); i++) o << "  " << (ctx.items[i].sep ? "---" : narrow(ctx.items[i].label)) << (ctx.items[i].checked ? " [x]" : "") << (ctx.items[i].dim ? " (dim)" : "") << (ctx.items[i].sub.empty() ? "" : " (sub)") << ((int)i == ctx.hover ? " (lit)" : "") << " " << rc(ctx.rows[i]) << "\n"; } panelDump(o); }
    else {
      o << "selected " << st.selected << "  focusInTerminal " << st.focusInTerminal << "  edit " << (st.focusEdit ? narrow(st.focusEdit->text) : std::string("-")) << "  zoom " << zoom << "\n";
      o << "layout " << json(std::map<std::string, L::Cells>(layout.begin(), layout.end())).dump() << "  preview " << json(previewSet).dump() << "  leaving " << json(leaving).dump() << "  emptySlot " << json(emptySlot).dump() << "  walk " << stepSnap.on << "\n";
      for (const auto& kv : st.confirm) o << "confirm " << kv.first << " " << kv.second.kind << " " << kv.second.account << " " << kv.second.browser << "\n";
      o << "renaming " << st.renaming << "  search '" << st.search << "' open " << st.searchOpen << "  grouped " << st.grouped << "  note '" << paneNoteText << "'\n";
      for (const auto& kv : terms) o << "term " << kv.first << " " << kv.second->term.cols() << "x" << kv.second->term.rows() << " attached " << kv.second->attached << " scrollOff " << kv.second->scrollOff << " of " << kv.second->term.scrollbackSize() << " held " << paneHeld(*kv.second) << " sel " << kv.second->selectionText().size() << " chars '" << narrow(kv.second->selectionText().substr(0, 60)) << "'\n";
    }
    logLine(o.str());
  }

  // ------------------------------------------------------------ the window
  static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* a = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) { a = (App*)((CREATESTRUCTW*)l)->lpCreateParams; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)a); a->hwnd = h; shell().apps.push_back(a); }
    if (!a) return DefWindowProcW(h, m, w, l);
    const LRESULT r = a->proc(m, w, l);
    if (m == WM_NCDESTROY) {
      // the window is gone. The last one going ends the process (window-all-closed); a window that was opened
      // from another one was made on the heap and goes with its HWND.
      Shell& sh = shell();
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      sh.apps.erase(std::remove(sh.apps.begin(), sh.apps.end(), a), sh.apps.end());
      a->hwnd = nullptr;
      if (a->heapOwned) delete a;
      if (sh.apps.empty()) { if (sh.hub) { DestroyWindow(sh.hub); sh.hub = nullptr; } PostQuitMessage(sh.exitCode); }
    }
    return r;
  }
  // Everything a held mouse button was doing, ended without a release: the sidebar's edge is where it is, a
  // selection stays as far as it got, a dragged tile goes back, a pressed button is not clicked.
  void cancelMouse() {
    if (resizingSidebar) { resizingSidebar = false; savePrefs(); applyLayout(); }
    if (selecting) { selecting = false; dragWheelBudget = 0; auto it = terms.find(selectingId); if (it != terms.end() && !finishDrag(*it->second, false) && it->second->sel.empty()) it->second->sel = Selection(); }
    editDrag = nullptr; thumbDrag = false; dragScrollAmt = 0; mousePane.clear(); mouseButtons = 0; if (hwnd) KillTimer(hwnd, 104);
    ghostHide();
    if (!dragSrc.empty()) { dragSrc.clear(); st.dragging.clear(); st.dropMark.clear(); hidePreview(); }
    press.on = false; st.pressed.clear();
    invalidate();
  }
  // Data arriving faster than the queue empties would keep WM_PAINT (the lowest-priority message) waiting for
  // ever: after 50 ms without a frame, paint now. Otherwise the paint is left to come by itself, once.
  ULONGLONG lastPaintAt = 0;
  void paintIfStarved() {
    if (!hwnd || headless() || IsIconic(hwnd)) return;
    if (GetTickCount64() - lastPaintAt >= 50 && GetUpdateRect(hwnd, nullptr, FALSE)) UpdateWindow(hwnd);
  }
  // where text is being typed, in client pixels: the focused box's caret, else the selected pane's cursor
  bool caretPoint(POINT& pt, float& lineH, float& fontPx) const {
    if (st.focusEdit) {
      const EditBox& e = *st.focusEdit; float cx = 0, cy = 0, h = 0; e.caretXY(cx, cy, h);
      pt.x = (LONG)std::lround((e.rect.x - e.scrollX + cx) * dpi / 96); pt.y = (LONG)std::lround((e.originY() + cy) * dpi / 96); lineH = h; fontPx = e.font.size;
      return true;
    }
    auto it = terms.find(st.selected);
    if (it == terms.end() || !st.focusInTerminal || !st.shown.count(st.selected)) return false;
    const TermView& v = *it->second;
    pt.x = (LONG)std::lround((v.lastOx + std::min(v.term.cursorX(), v.term.cols() - 1) * v.lastCell.w) * dpi / 96);
    pt.y = (LONG)std::lround((v.lastOy + v.term.cursorY() * v.lastCell.h) * dpi / 96); lineH = v.lastCell.h; fontPx = v.fontSize;
    return true;
  }
  void placeIme() {
    HIMC imc = ImmGetContext(hwnd); if (!imc) return;
    POINT pt; float lineH = 17, fontPx = 14;
    if (caretPoint(pt, lineH, fontPx)) {
      COMPOSITIONFORM cf = {}; cf.dwStyle = CFS_POINT; cf.ptCurrentPos = pt; ImmSetCompositionWindow(imc, &cf);
      CANDIDATEFORM cand = {}; cand.dwIndex = 0; cand.dwStyle = CFS_EXCLUDE; cand.ptCurrentPos = pt;
      cand.rcArea = { pt.x, pt.y, pt.x + 2, pt.y + (LONG)std::lround(lineH * dpi / 96) }; ImmSetCandidateWindow(imc, &cand);
      LOGFONTW lf = {}; lf.lfHeight = -(LONG)std::lround(fontPx * dpi / 96); lf.lfCharSet = DEFAULT_CHARSET;
      wcsncpy(lf.lfFaceName, (st.focusEdit ? std::wstring(L"Segoe UI") : monoFamily).c_str(), LF_FACESIZE - 1); ImmSetCompositionFontW(imc, &lf);
    }
    ImmReleaseContext(hwnd, imc);
  }
  // ---- an input method composing INTO A TERMINAL: the composing text is drawn in the grid at the cursor (xterm's
  // composition view; termview.hpp `composing`) instead of in Windows' own little composition window, and what the
  // input method commits is typed as characters. Windows still shows its candidate list, placed at the cursor
  // (placeIme). A text box of the deck's own (rename, search, Settings) keeps Windows' composition window.
  bool imeInline = false;                               // this composition is the terminal's: its messages stop here
  TermView* imePane() {
    if (st.focusEdit || !termKeys()) return nullptr;
    auto it = terms.find(st.selected);
    return it == terms.end() || !st.shown.count(st.selected) ? nullptr : it->second.get();
  }
  // one WM_IME_COMPOSITION, read: `result` (GCS_RESULTSTR) is typed; `comp` (GCS_COMPSTR) is what is shown now
  void imeUpdate(const std::wstring& comp, const std::wstring* result) {
    if (result) { if (testRun()) logLine("ime commit '" + narrow(*result) + "'"); for (wchar_t ch : *result) onChar(ch, false); }
    for (auto& kv : terms) if (!kv.second->composing.empty() && kv.first != st.selected) { kv.second->composing.clear(); kv.second->repaintAll = true; }
    if (TermView* v = imePane()) { if (v->composing != comp) { v->composing = comp; v->repaintAll = true; if (testRun()) logLine("ime composing '" + narrow(comp) + "' at " + std::to_string(v->term.cursorX()) + "," + std::to_string(v->term.cursorY())); } }
    invalidate();
  }
  void imeEnd() { for (auto& kv : terms) if (!kv.second->composing.empty()) { kv.second->composing.clear(); kv.second->repaintAll = true; } imeInline = false; invalidate(); }
  static std::wstring imeString(HIMC imc, DWORD what) {
    const LONG n = ImmGetCompositionStringW(imc, what, nullptr, 0); if (n <= 0) return L"";
    std::wstring s((size_t)n / sizeof(wchar_t), L'\0'); ImmGetCompositionStringW(imc, what, &s[0], (DWORD)n); return s;
  }
  float dipX(LPARAM l) const { return GET_X_LPARAM(l) * 96.f / dpi; }
  float dipY(LPARAM l) const { return GET_Y_LPARAM(l) * 96.f / dpi; }
  float dpi = 96;

  LRESULT proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
      case WM_CREATE: return 0;
      case WM_WS: { std::string* s = (std::string*)l; if (!closing) onWs(w, s ? *s : std::string()); delete s; if (!closing) paintIfStarved(); return 0; }
      case WM_MOVE: case WM_MOVING: askPane.parentMoved(); cloudPanes.parentMoved(); if (m == WM_MOVE && !testRun()) SetTimer(hwnd, TIMER_BOUNDS, 400, nullptr); break;   // the web view's own popups follow the window
      case WM_SIZE:
        if (!headless()) {
          // Minimised: the client area is 0 x 0. Nothing is laid out against that — fitting the panes to it would
          // send every visible pty a 2 x 1 resize and wreck its screen. The deck is only told the window shows
          // nothing now (`shown` with hidden, bridge.js document.hidden); the restore tells it the opposite.
          if (w == SIZE_MINIMIZED || LOWORD(l) == 0 || HIWORD(l) == 0) { cancelMouse(); syncShown(); askPane.minimised(true); cloudSync(); reportWindowState(); return 0; }   // (minimised: the web views are hidden and suspended)
          gfx.resizeWindow(LOWORD(l), HIWORD(l)); viewW = LOWORD(l) * 96.f / dpi; viewH = HIWORD(l) * 96.f / dpi;
          syncShown();
          fitVisible(); clampScroll(); askSync(); cloudSync(); invalidate();   // (the web view follows the terminal area at once, not a frame late)
          reportWindowState(); if (!testRun()) SetTimer(hwnd, TIMER_BOUNDS, 400, nullptr);
        }
        return 0;
      case WM_EXITSIZEMOVE: reportBounds(); break;
      case WM_DPICHANGED: {
        // the window moved to a monitor at another scale (or the scale was changed): Windows hands the new
        // rectangle; everything measured in device pixels is made again — the target, the scene, the tip's width
        dpi = (float)HIWORD(w); if (dpi < 1) dpi = 96;
        hideTip();
        RECT* r = (RECT*)l; SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        gfx.targetWindow(hwnd); metricsBySize.clear(); askPane.dropDeviceResources(); cloudPanes.dropDeviceResources();
        RECT rc; GetClientRect(hwnd, &rc); if (rc.right > 0 && rc.bottom > 0) { viewW = rc.right * 96.f / dpi; viewH = rc.bottom * 96.f / dpi; }
        if (tip) SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)std::lround(420 * dpi / 96));
        fitVisible(); clampScroll(); invalidate();
        return 0;
      }
      case WM_ERASEBKGND: return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        // One paint per display refresh at most: the window target presents on the refresh (EndDraw waits for
        // it), and Windows makes one WM_PAINT however many invalidations came before it, so a burst of output
        // costs one frame. A target that could not be made (or was lost) is made again here.
        if (closing) { EndPaint(hwnd, &ps); return 0; }
        if (!headless() && !gfx.hwndRt && !IsIconic(hwnd)) { gfx.targetWindow(hwnd); askPane.dropDeviceResources(); cloudPanes.dropDeviceResources(); }
        if (!headless() && gfx.hwndRt) {
          gfx.begin(); drawFrame();
          if (!gfx.end()) { gfx.targetWindow(hwnd); askPane.dropDeviceResources(); cloudPanes.dropDeviceResources(); invalidate(); }
        }
        EndPaint(hwnd, &ps);
        lastPaintAt = GetTickCount64();
        return 0;
      }
      case WM_TIMER:
        if (closing) return 0;
        if (w == TIMER_ASKWARM) { KillTimer(hwnd, TIMER_ASKWARM); askHoverArmed = false; if (!askPane.webMade() && !askPane.isOpen()) { askSync(); askPane.prewarm(); if (testRun()) logLine("ask web view warmed ahead (the pointer rested on the asks tile): create() took " + std::to_string(askPane.createCallMs()) + " ms on the window thread"); } return 0; }
        if (w == TIMER_WEBIDLE) { cloudSync(); return 0; }
        if (w == TIMER_BLINK) {
          const ULONGLONG el = GetTickCount64() - blinkEpoch;
          const bool b1 = (el / 600) % 2 == 0, b2 = (el / 500) % 2 == 0;
          if (b1 != blinkOn || b2 != caretOn) { blinkOn = b1; caretOn = b2; if (st.focusInTerminal || st.focusEdit) invalidate(); }
          if (flashUntil && GetTickCount64() > flashUntil) { flashUntil = 0; invalidate(); }
          if (noticeUntil && GetTickCount64() > noticeUntil) invalidate();
          if (paneNoteUntil && GetTickCount64() > paneNoteUntil) { paneNoteUntil = 0; paneNoteText.clear(); invalidate(); }
          if (st.restartConfirming && GetTickCount64() >= nowConfirmUntil) { st.restartConfirming = false; invalidate(); }
        }
        else if (w == TIMER_ANIM) {                       // the animation clock: one tick, one frame of the parts that move
          if ((!animVisible() && !transVisible()) || minimised()) { KillTimer(hwnd, TIMER_ANIM); animTimerOn = false; return 0; }
          st.animMs = (double)(GetTickCount64() - animEpoch);
          invalidate();
        }
        else if (w == TIMER_CLOCK) { invalidate(); adoptPendingNew(); panelTick(); }
        else if (w == TIMER_SHOT) {
          KillTimer(hwnd, TIMER_SHOT);
          if (!opt.type.empty() && !st.selected.empty()) { std::string t = narrow(opt.type); opt.type.clear(); size_t p; while ((p = t.find("\\r")) != std::string::npos) t.replace(p, 2, "\r"); sendInput(st.selected, t); SetTimer(hwnd, TIMER_SHOT, 1500, nullptr); return 0; }
          if (!opt.doScript.empty() && !scriptStarted) { scriptStarted = true; scripting = true; script = parseScript(narrow(opt.doScript)); scriptAt = 0; runStep(); return 0; }
          if (!opt.windowShot.empty()) captureWindow(opt.windowShot); else saveShot(opt.shot);
          shellQuit(this);
        }
        else if (w == TIMER_BOUNDS) { KillTimer(hwnd, TIMER_BOUNDS); reportBounds(); }
        else if (w == TIMER_TIP) showTip();
        else if (w == TIMER_SCRIPT) { KillTimer(hwnd, TIMER_SCRIPT); runStep(); }
        else if (w == TIMER_PROBE) { KillTimer(hwnd, TIMER_PROBE); finishProbe(); }
        else if (w == TIMER_DESKINIT) { KillTimer(hwnd, TIMER_DESKINIT); deskInit(); }
        else if (w == 102) { KillTimer(hwnd, 102); runPaneRedraws(); }
        else if (w == TIMER_UPDATE) { KillTimer(hwnd, TIMER_UPDATE); checkForUpdates(false); }
        else if (w == TIMER_QUIETREDRAW) { KillTimer(hwnd, TIMER_QUIETREDRAW); runQuietRedraws(); }
        else if (w == 103) { KillTimer(hwnd, 103); syncTimeout(); }
        else if (w == 104) dragScrollTick();
        else if (w == 101) { KillTimer(hwnd, 101); if (sset.open && sset.status == L"Saved") closeSettings(); }
        else if (w == 100) { KillTimer(hwnd, 100); if (!nameClickId.empty()) { redrawTile(nameClickId); nameClickId.clear(); } }
        return 0;
      case WM_SETFOCUS:
        windowFocused = true;
        // back from another window, and the page had the keyboard when this one was left: it has it again
        if (webHadFocus) { webHadFocus = false; if (askPane.isOpen() && !askPane.covered()) askPane.focusPage(); }
        if (!cloudHadFocus.empty()) { const std::string id = cloudHadFocus; cloudHadFocus.clear(); if (id == st.selected) cloudPanes.focus(id); }
        reportWindowState();
        invalidate(); return 0;
      // the window stops being the active one while the PAGE has the keyboard: no WM_KILLFOCUS comes here for that
      case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && askPane.webFocused()) { webHadFocus = true; windowFocused = false; chordPressed.clear(); chordHeld.clear(); hideTip(); stepPreviewCommit(); invalidate(); }
        else if (LOWORD(w) == WA_INACTIVE && cloudPanes.webFocused()) { cloudHadFocus = cloudPanes.focusedId(); windowFocused = false; chordPressed.clear(); chordHeld.clear(); hideTip(); stepPreviewCommit(); invalidate(); }
        reportWindowState();
        break;
      // The keys go elsewhere: a chord in progress is dropped (its key releases will never arrive here), a walk is
      // committed, a tip is taken down. A drag is ended by WM_CAPTURECHANGED, which the same switch brings.
      // (the keyboard going to the ask pane's page, a child of this window, is not the window losing it)
      case WM_KILLFOCUS: if (imeInline) imeEnd(); windowFocused = w && IsChild(hwnd, (HWND)w); chordPressed.clear(); chordHeld.clear(); charHigh = 0; hideTip(); stepPreviewCommit(); reportWindowState(); invalidate(); return 0;
      case WM_KEYDOWN: case WM_SYSKEYDOWN:
        // The press itself was offered to onKeyDown by the message loop BEFORE it was translated (run()); what
        // arrives here is either taken already or not ours. Alt+F4 and Alt+Space stay the window's own.
        if (m == WM_SYSKEYDOWN && (w == VK_F4 || w == VK_SPACE)) break;
        return 0;
      case WM_KEYUP: case WM_SYSKEYUP: onKeyUp((UINT)w); return 0;
      case WM_CHAR: onChar((wchar_t)w, false); return 0;
      case WM_SYSCHAR:
        if (w == VK_SPACE) break;                                          // Alt+Space: the window menu
        onChar((wchar_t)w, true); return 0;
      // a bare Alt or F10 must not open the system menu (they are the session's keys); Alt+Space still does
      case WM_SYSCOMMAND: if ((w & 0xFFF0) == SC_KEYMENU && l != ' ') return 0; break;
      // An input method: its composition window (drawn by Windows) goes where the text will land, in the pane's
      // own font size. The committed text needs nothing here: DefWindowProc turns it into WM_CHAR.
      case WM_IME_STARTCOMPOSITION: placeIme(); imeInline = imePane() != nullptr; if (imeInline) return 0; break;   // (not passed on: Windows' own composition window stays shut)
      case WM_IME_COMPOSITION:
        if (imeInline) {
          std::wstring comp, res; const bool hasRes = (l & GCS_RESULTSTR) != 0;
          if (HIMC imc = ImmGetContext(hwnd)) { if (l & GCS_COMPSTR) comp = imeString(imc, GCS_COMPSTR); if (hasRes) res = imeString(imc, GCS_RESULTSTR); ImmReleaseContext(hwnd, imc); }
          imeUpdate(comp, hasRes ? &res : nullptr); placeIme();
          return 0;                                     // (not passed on: the result was typed here, DefWindowProc would type it again)
        }
        placeIme(); break;
      case WM_IME_ENDCOMPOSITION: if (imeInline) imeEnd(); break;
      case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetFocus(hwnd); hideTip(); onMouseDown(dipX(l), dipY(l), (w & MK_SHIFT) != 0); return 0;
      case WM_LBUTTONUP: onMouseUp(dipX(l), dipY(l)); return 0;
      case WM_RBUTTONDOWN: hideTip(); onPaneButton(mouse::RIGHT, true, dipX(l), dipY(l)); return 0;
      case WM_RBUTTONUP: onPaneButton(mouse::RIGHT, false, dipX(l), dipY(l)); onRightClick(dipX(l), dipY(l)); return 0;
      case WM_MBUTTONDOWN: hideTip(); onPaneButton(mouse::MIDDLE, true, dipX(l), dipY(l)); return 0;
      case WM_MBUTTONUP: onPaneButton(mouse::MIDDLE, false, dipX(l), dipY(l)); return 0;
      // The mouse was taken from us mid-press (Alt+Tab, a system dialog, another window's capture): whatever was
      // going on with the button down is over, and no button-up will come to say so.
      case WM_CAPTURECHANGED: if ((HWND)l != hwnd && !releasing) cancelMouse(); return 0;
      case WM_MOUSEMOVE: {
        // a button-up that never reached us (released over another window while a modal loop ran): end the press
        if (!(w & MK_LBUTTON) && (resizingSidebar || selecting || editDrag || thumbDrag || (mouseButtons & 1) || !dragSrc.empty())) { onMouseUp(dipX(l), dipY(l)); return 0; }
        onMouseMove(dipX(l), dipY(l));
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t);
        return 0;
      }
      case WM_MOUSELEAVE: hideTip(); tipText.clear(); tipHit = ui::Hit(); st.mouseX = st.mouseY = -1; st.hover.clear(); st.hoverPart.clear(); st.hoverArg.clear(); ctx.hover = -1; invalidate(); return 0;
      case WM_MOUSEWHEEL: { POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(hwnd, &p); onWheel(p.x * 96.f / dpi, p.y * 96.f / dpi, GET_WHEEL_DELTA_WPARAM(w)); return 0; }
      case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
      // the smallest window, in DIPs: the sidebar at its narrowest (sbMin, the tile strip's width) and 300 for a pane beside it
      case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)l; mm->ptMinTrackSize.x = MulDiv((int)std::ceil(sbMin()) + 300, (int)dpi, 96); mm->ptMinTrackSize.y = MulDiv(320, (int)dpi, 96); return 0; }
      // the window goes at once; the goodbye to the deck (up to 1.5 s when it does not answer) happens behind it
      // (the ask pane's web view starts closing first, the goodbye runs meanwhile, then its browser process is waited
      // for, 3 s at most, and its profile folder removed)
      case WM_CLOSE: if (!st.renaming.empty()) finishRename(true); savePrefs(); ShowWindow(hwnd, SW_HIDE);
        if (!localMode && shell().remoteOwner == this) shellCloseOthers(this);   // REMOTE: the socket is this window's; the others go with it
        closing = true; beginWebShutdown(); stopTransport(); endAsks(); DestroyWindow(hwnd); return 0;
      // Windows is shutting down or the user is signing out: what a close would have saved, saved
      // (every window is told; the state file keeps them all, as it does across a restart: nothing is taken out of it)
      case WM_ENDSESSION: if (w) { if (!st.renaming.empty()) finishRename(true); savePrefs(); if (!closing) { closing = true; beginWebShutdown(); if (localMode) { if (shell().host.running()) shell().host.stop(); } else stopTransport(); endAsks(); } } return 0;
      case WM_DESTROY: ghostHide(); if (!headless()) setWindowIdentity(hwnd, inst, !localMode, opt.stateDir, true); if (primary) shell().exitCode = exitCode; return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
  }

  // ---- one window made: the class, the HWND, its target (an off-screen bitmap in a headless run), the tip, shown.
  // `place`: where (x, y, width, height in pixels, maximized) — the remembered place of the first window, the
  // saved or computed one of another; null = the default. 0 = made, else the exit code.
  int makeWindow(const json* place) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"ClaudeDeckNative";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);                              // (a second window: registered already, which is fine)

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = opt.width, h = opt.height; bool maxed = false, placed = false;
    const bool parked = !opt.windowShot.empty() || !opt.bench.empty();
    if (parked) { x = -32000; y = -32000; }
    // The remembered place is a WINDOWPLACEMENT's normal rectangle (workspace coordinates, which are not screen
    // coordinates when the taskbar is at the top or the left), so it goes back through SetWindowPlacement below.
    // It is used only if the window's title bar would be on a monitor that exists: a place on a screen that has
    // gone, or one that leaves only a sliver showing, is dropped for the default.
    RECT remembered = {};
    if (!headless() && !parked && place && place->is_object() && place->contains("width") && place->contains("height")) {
      const int pw = place->value("width", 0), ph = place->value("height", 0);
      maxed = place->value("maximized", false);
      if (place->contains("x") && place->contains("y") && (*place)["x"].is_number() && (*place)["y"].is_number()) {
        const int px = place->value("x", 0), py = place->value("y", 0);
        remembered = { px, py, px + pw, py + ph };
        const POINT grip[3] = { { remembered.left + 40, remembered.top + 12 }, { remembered.left + pw / 2, remembered.top + 12 }, { remembered.right - 40, remembered.top + 12 } };
        int on = 0; for (const POINT& g : grip) if (MonitorFromPoint(g, MONITOR_DEFAULTTONULL)) on++;
        placed = pw >= 300 && ph >= 200 && on >= 2;
      }
      if (!placed && pw >= 300 && ph >= 200) { w = pw; h = ph; }   // a size without a usable place: Windows picks where
    }
    viewW = (float)opt.width; viewH = (float)opt.height;
    hwnd = CreateWindowExW(0, wc.lpszClassName, localMode ? L"" DECK_EDITION_PRODUCT L" \x2014 local" : L"" DECK_EDITION_PRODUCT L" \x2014 remote", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, this);   // (WS_CLIPCHILDREN: a frame never paints over the ask pane's web view)
    if (!hwnd) return 2;
    if (!opt.bench.empty()) {                             // the bench's window: a client area of exactly --size
      const UINT d = GetDpiForWindow(hwnd); RECT r = { 0, 0, MulDiv(opt.width, d, 96), MulDiv(opt.height, d, 96) };
      AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, d);
      SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (headless()) {
      // every frame of a headless run is drawn into one off-screen bitmap: nothing is ever shown
      if (!gfx.targetBitmap((int)viewW, (int)viewH, shotScale())) return 3;
    } else {
      BOOL dark = TRUE; DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
      // its own taskbar group, pin and icon: the local deck's, or the remote's (notify.hpp setWindowIdentity)
      setWindowIdentity(hwnd, inst, !localMode, opt.stateDir);
      if (testRun()) logLine(std::string("taskbar identity: ") + (localMode ? "local" : "remote") + " -> " + narrow(windowIdentity(hwnd)));
      dpi = (float)GetDpiForWindow(hwnd); if (dpi < 1) dpi = 96;
      // (tests) --dpi on a --window-shot run: the parked window is on a 96 dpi screen; its client area is made
      // --size DIPs at that scale and everything is drawn, laid out and hit-tested as on a monitor at that scale
      if (!opt.windowShot.empty() && opt.forceDpi > 0) {
        const float real = dpi; dpi = (float)opt.forceDpi; gfx.forceDpi = (UINT)opt.forceDpi;
        RECT r = { 0, 0, (LONG)std::lround(opt.width * dpi / 96), (LONG)std::lround(opt.height * dpi / 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0, (UINT)real);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      gfx.targetWindow(hwnd);
      RECT rc; GetClientRect(hwnd, &rc); viewW = rc.right * 96.f / dpi; viewH = rc.bottom * 96.f / dpi;
      // no tooltip window in a test's parked window: nothing of a test may ever show
      if (!testRun()) tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, inst, nullptr);
      if (tip) {
        // one tool, tracked (see updateTip): TTF_ABSOLUTE = it goes exactly where TTM_TRACKPOSITION says (screen px)
        TOOLINFOW ti = tipInfo(hwnd); ti.uFlags = TTF_TRACK | TTF_ABSOLUTE; ti.lpszText = (LPWSTR)L"";
        if (!SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti)) { DestroyWindow(tip); tip = nullptr; }
        else SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)std::lround(420 * dpi / 96));   // also what lets a long tip wrap
      }
      if (placed) { WINDOWPLACEMENT wp = { sizeof(wp) }; wp.showCmd = SW_HIDE; wp.rcNormalPosition = remembered; wp.ptMinPosition = { -1, -1 }; wp.ptMaxPosition = { -1, -1 }; SetWindowPlacement(hwnd, &wp); }
      ShowWindow(hwnd, parked || showInactive ? SW_SHOWNOACTIVATE : (placed && maxed) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
      windowFocused = parked ? true : GetForegroundWindow() == hwnd;
      SetTimer(hwnd, TIMER_BLINK, 100, nullptr);
      SetTimer(hwnd, TIMER_CLOCK, 1000, nullptr);
    }
    return 0;
  }
  // ---- another window of the LOCAL deck (main.js createWindow for a second BrowserWindow): the same App on the
  // same deck, with its own view. Everything a window keeps for itself is made again here; config.json, the deck,
  // the notifier and the hub are the process's. The caller then makes it a client (DeckHost::openWindow / addWindow).
  bool initSecondary(App& first, const json& place, bool remote = false) {
    inst = first.inst; opt = first.opt; gfx.sceneOff = opt.noScene;
    // what belongs to the run's ONE scripted window stays there: this one is driven through `win <n> <step>`
    opt.select.clear(); opt.type.clear(); opt.place = false; opt.probe.clear();
    if (!gfx.initShared(first.gfx)) return false;        // the process's factories and bundled fonts (gfx.hpp)
    loadPrefs();
    localMode = !remote;
    if (remote) link = first.link;
    monoFamily = resolveMonoFamily(gfx, L"");
    searchEdit.onChange = [this] { st.search = narrow(searchEdit.text); st.scroll = 0; };
    initPanels();
    // (a test's other window has no screen to be on: it is the size window:open would have given it)
    if (testRun() && place.is_object() && place.contains("width") && place.contains("height")) { opt.width = place.value("width", opt.width); opt.height = place.value("height", opt.height); }
    if (makeWindow(&place)) return false;
    initAsks();
    initCloud();
    st.connected = false; st.claudeOff = first.st.claudeOff; st.ownLayoutOnly = false; st.officeScreen = true; st.notice.clear(); st.noticeDetail.clear();
    if (remote) { askPane.setLink(link); everOpened = true; st.connected = first.st.connected; st.ownLayoutOnly = true; st.officeScreen = false; updateTitle(); }
    return true;
  }

  // tiles.json `windows[0].bounds` (else the older top-level `bounds`) of this deck's state folder, when it is a
  // usable rectangle: { x?, y?, width, height, maximized? } — main.js's shape, which makeWindow reads as it is.
  json deckSavedPlace() const {
    const std::wstring file = (opt.stateDir.empty() ? configDir() : opt.stateDir) + L"\\local\\tiles.json";
    std::ifstream f(file.c_str(), std::ios::binary); if (!f) return json();
    std::stringstream ss; ss << f.rdbuf(); std::string t = ss.str();
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t = t.substr(3);
    const json j = json::parse(t, nullptr, false); if (!j.is_object()) return json();
    json b;
    if (j.contains("windows") && j["windows"].is_array() && !j["windows"].empty() && j["windows"][0].is_object() && j["windows"][0].contains("bounds")) b = j["windows"][0]["bounds"];
    else if (j.contains("bounds")) b = j["bounds"];
    if (!b.is_object() || !b.contains("width") || !b.contains("height") || !b["width"].is_number() || !b["height"].is_number()) return json();
    json out = { { "width", (int)b["width"].get<double>() }, { "height", (int)b["height"].get<double>() }, { "maximized", b.contains("maximized") && b["maximized"].is_boolean() && b["maximized"].get<bool>() } };
    if (b.contains("x") && b.contains("y") && b["x"].is_number() && b["y"].is_number()) { out["x"] = (int)b["x"].get<double>(); out["y"] = (int)b["y"].get<double>(); }
    return out;
  }

  int run(HINSTANCE hi, const Options& o) {
    inst = hi; opt = o; gfx.sceneOff = opt.noScene;
    if (testRun()) testClipboard() = &fakeClipboard;     // a test copies and pastes inside itself, never through the system clipboard
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!gfx.init()) return 2;
    // A test never reads or writes %APPDATA%\claude-deck-native: a headless run that would need the real folder
    // (no --state-dir, and either local mode or a link to look up) is refused.
    if (testRun() && opt.stateDir.empty() && (opt.local || opt.link.empty()) && opt.bench.empty()) return 7;
    Shell& sh = shell();
    // A deck that was restarted (lifecycle.hpp): the one that started this is on its way out. Wait for it — before
    // the lock, before any file is read, long before a window: the hand-over shows nothing.
    if (opt.waitPid) { waitForPid(opt.waitPid, 20000); removeStaleOld(); }
    // ONE deck per config folder (main.js requestSingleInstanceLock): a second launch brings the first forward
    // and leaves. A test takes the lock only for a local deck in its own --state-dir (two of those would both
    // write its tiles.json); a remote test run, a probe and a bench take none.
    const bool locked = opt.bench.empty() && opt.probe.empty() && (!testRun() || opt.local);
    if (locked) {
      const std::wstring key = SingleInstance::keyFor(opt.stateDir.empty() ? configDir() : opt.stateDir);
      if (!sh.single.acquire(key, opt.waitPid ? 20000 : 0)) {
        const bool told = SingleInstance::wakeFirst(key);
        if (testRun()) { std::ofstream f(((opt.shot.empty() ? opt.windowShot : opt.shot) + L".error.txt").c_str(), std::ios::binary | std::ios::trunc); f << "another deck holds this folder (single instance); it was " << (told ? "told to come forward" : "not found") << "\n"; }
        return 8;
      }
    }
    if (opt.bench.empty() && (!opt.stateDir.empty() || !opt.noSave || opt.link.empty())) cfg.load(opt.stateDir);
    // the self-update applied: this is the new exe's first start (its predecessor, moved aside, was removed above)
    if (updateAppliedAtStart() && opt.bench.empty() && opt.probe.empty()) { const bool lm = localMode; localMode = opt.local || cfg.j.value("mode", std::string()) == "local"; appLog("update: applied at start - this is the new exe; the old one (.old) is removed"); localMode = lm; }
    if (opt.forget) { cfg.forgetLink(); cfg.j.erase("mode"); cfg.save(); }
    loadPrefs();
    // Which deck: --local, else a link given now (--link / CLAUDE_DECK_LINK), else what was used last time
    // (config.json `mode`), else the remembered link, else ask — and the question has both answers.
    const std::string lastMode = cfg.j.value("mode", std::string());
    if (!opt.bench.empty()) localMode = false;
    else if (opt.local) localMode = true;
    else if (!opt.link.empty()) { if (!parseDeckLink(opt.link, link)) return 6; if (!opt.noSave) { cfg.setLink(link); cfg.save(); } }
    else if (lastMode == "local") localMode = true;
    else if (!cfg.link(link)) {
      if (testRun()) return 6;
      std::wstring text;
      const int answer = linkdlg::ask(inst, text, L"");
      if (!answer) return 0;
      if (answer == 2) localMode = true;
      else { parseDeckLink(text, link); cfg.setLink(link); }
    }
    if (!opt.noSave && opt.bench.empty() && lastMode != (localMode ? "local" : "remote")) { cfg.j["mode"] = localMode ? "local" : "remote"; cfg.save(); }
    loadBundledFonts();
    monoFamily = resolveMonoFamily(gfx, L"");
    searchEdit.onChange = [this] { st.search = narrow(searchEdit.text); st.scroll = 0; };
    initPanels();

    // The process's own parts: the hub, the notifier (a test's only records), the lifecycle.
    if (opt.bench.empty()) {
      WNDCLASSEXW hc = { sizeof(hc) }; hc.lpfnWndProc = hubProc; hc.hInstance = inst; hc.lpszClassName = kHubClass;
      RegisterClassExW(&hc);
      sh.hub = CreateWindowExW(0, kHubClass, sh.single.held() ? sh.single.key().c_str() : L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
      sh.notifier.record = testRun(); sh.notifier.hub = sh.hub; sh.notifier.clickMsg = WM_TOASTCLICK;
      sh.notifier.log = testRun() ? std::function<void(const std::string&)>([this](const std::string& l) { logLine(l); }) : nullptr;
      sh.notifier.onFlash = [](int index) {
        for (App* a : shell().apps) if (a->st.windowIndex == index && a->hwnd && !a->windowFocused) { FLASHWINFO f = { sizeof(f), a->hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 }; FlashWindowEx(&f); }
      };
      sh.notifier.identify(!localMode);                 // the taskbar identity + (local only) the Start-menu shortcut (never in a test)
      sh.life.test = testRun();
      { std::wstring folder = cfg.path; const size_t sl = folder.find_last_of(L"\\/"); sh.life.configFolder = sl == std::wstring::npos ? (opt.stateDir.empty() ? configDir() : opt.stateDir) : folder.substr(0, sl); }
      sh.life.log = testRun() ? std::function<void(const std::string&)>([this](const std::string& l) { logLine(l); }) : nullptr;
      // restartDeck (main.js): the core has saved everything, windows included. The new deck is started now and
      // waits for this one; this one goes once the call that asked has returned.
      sh.life.livePids = [] { Shell& s2 = shell(); return s2.host.running() ? s2.host.core().livePids() : std::vector<uint32_t>(); };
      // core::Lifecycle::openWindow (the on-call's desk with no free cell: deck_parity.cpp placeDeskTile): a window for
      // a client the core has ALREADY registered — only the HWND is new (DeckHost::adoptWindow). Shown without taking
      // the focus, and flashing, as main.js createWindow(bounds, { inactive: true }).
      sh.life.onOpenWindow = [this](int client, const json& bounds, bool inactive) {
        Shell& s2 = shell();
        if (!localMode || !s2.host.running()) return false;
        App* a = new App(*this); a->showInactive = inactive;
        if (!a->initSecondary(*this, bounds.is_object() ? bounds : json::object())) { if (a->hwnd) { a->closing = true; DestroyWindow(a->hwnd); } delete a; return false; }
        a->heapOwned = true; a->client = client;
        s2.host.adoptWindow(client, a->hwnd, WM_WS);
        if (inactive && !a->testRun()) { FLASHWINFO f = { sizeof(f), a->hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 }; FlashWindowEx(&f); }
        if (testRun()) logLine("window opened by the deck for client " + std::to_string(client) + (inactive ? " (inactive)" : ""));
        return true;
      };
      sh.life.onRestart = [this](const std::string& why) {
        Shell& s2 = shell();
        if (s2.restarting) return false;
        if (testRun()) logLine("restart asked: " + why);
        if (!s2.life.relaunch()) return false;
        s2.restarting = true;
        s2.later([] { shellQuit(nullptr); });
        return true;
      };
    }

    // the first window's remembered place: config.json `window` = a WINDOWPLACEMENT's normal rectangle + maximised
    json place;
    if (cfg.j.contains("window") && cfg.j["window"].is_array() && cfg.j["window"].size() >= 5) {
      const auto& a = cfg.j["window"];
      bool nums = true; for (int i = 0; i < 4; i++) if (!a[i].is_number()) nums = false;
      if (nums) place = { { "x", a[0].get<int>() }, { "y", a[1].get<int>() }, { "width", a[2].get<int>() - a[0].get<int>() }, { "height", a[3].get<int>() - a[1].get<int>() }, { "maximized", a[4].is_boolean() && a[4].get<bool>() } };
    }
    // LOCAL: the deck's own record of its first window wins when it has one — main.js keeps every window's bounds in
    // tiles.json (`windows[0].bounds`, the older single `bounds`), the core saves them there as this window reports
    // them (reportBounds), and a tiles.json imported from the Electron deck brings the Electron window's place along.
    // config.json `window` stays what a remote uses, and what a local deck falls back to before its first report.
    if (localMode) {
      const json fromDeck = deckSavedPlace();
      if (fromDeck.is_object()) { if (testRun()) logLine("first window: the deck's saved place " + fromDeck.dump() + " (tiles.json) is used" + (place.is_object() ? ", not config.json's " + place.dump() : std::string())); place = fromDeck; }
      else if (testRun() && place.is_object()) logLine("first window: config.json's place " + place.dump() + " (tiles.json has none)");
    }
    if (const int rc = makeWindow(place.is_object() ? &place : nullptr)) return rc;
    const bool parked = !opt.windowShot.empty() || !opt.bench.empty();
    if (!opt.bench.empty()) { exitCode = runBench(); DestroyWindow(hwnd); MSG m2; while (PeekMessageW(&m2, nullptr, 0, 0, PM_REMOVE)) {} return exitCode; }
    if (!headless()) web::WebPane::sweepStale();        // a web view's profile folder a crashed run left in %TEMP%
    if (opt.webFlagsSet && testRun()) { web::WebPane::setBrowserArguments(opt.webFlags); logLine("web flags (instead of the built-in ones): " + narrow(opt.webFlags)); }   // (tests: one browser argument at a time, measured)
    initAsks();
    initCloud();
    startTransport();
    if (headless() || parked) SetTimer(hwnd, 200, opt.hang > 0 ? (UINT)opt.hang : opt.doScript.empty() ? 30000 : 120000, nullptr);   // never hang a test

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if ((headless() || parked) && msg.hwnd == hwnd && msg.message == WM_TIMER && msg.wParam == 200 && !closing) { exitCode = 5; shellQuit(this); continue; }
      // A key press is offered to the app BEFORE it is translated. A key the app takes (a window key, a chord, an
      // arrow sent as its escape sequence, Enter in a box) is never translated, so no character is made from it;
      // any other press is translated as usual and its text arrives as WM_CHAR / WM_SYSCHAR. (onKeyDown.)
      // Whichever of the process's windows it is for: each is an App of its own.
      if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
        App* to = nullptr, *under = nullptr;
        for (App* a : sh.apps) { if (a->hwnd == msg.hwnd) to = a; else if (a->hwnd && IsChild(a->hwnd, msg.hwnd)) under = a; }
        if (under) under->noteKey();                    // a key in a child of the window (a page) is still a key in the deck's window
        if (to) {
          const bool sysOwn = msg.message == WM_SYSKEYDOWN && (msg.wParam == VK_F4 || msg.wParam == VK_SPACE);   // Alt+F4, Alt+Space: Windows'
          if (sysOwn) to->noteKey();
          else if (to->onKeyDown((UINT)msg.wParam, (msg.lParam & (1 << 30)) != 0)) continue;
        }
      }
      TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    sh.single.release();
    return (int)msg.wParam;
  }

  // ------------------------------------------------------------ the hub (the process's own messages)
  static LRESULT CALLBACK hubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Shell& sh = shell();
    if (m == WM_LOCALDECK) { sh.host.pump(); return 0; }
    if (m == WM_TIMER && w == TIMER_LOCALDECK) { sh.host.pump(); for (App* a : sh.apps) if (!a->closing) a->paintIfStarved(); return 0; }
    if (m == WM_TOASTCLICK) { std::string* s = (std::string*)l; if (s) { shellToastClick(ShellNotifier::fromClickKey(*s)); delete s; } return 0; }
    if (m == WM_SHELLTASK) { std::vector<std::function<void()>> t; t.swap(sh.tasks); for (auto& f : t) f(); return 0; }
    // main.js `second-instance`: another launch found this deck — the first window comes forward
    if (m == SingleInstance::message() && m) { secondInstance(); return 0; }
    return DefWindowProcW(h, m, w, l);
  }
  static void secondInstance() {
    Shell& sh = shell();
    App* a = sh.apps.empty() ? nullptr : sh.apps[0]; if (!a || !a->hwnd) return;
    if (a->testRun()) { a->logLine("second-instance: the first window comes forward"); return; }
    if (IsIconic(a->hwnd)) ShowWindow(a->hwnd, SW_RESTORE);
    SetForegroundWindow(a->hwnd);
  }
};

// ---------------------------------------------------------------- the process's windows
// A window made for the local deck beside the one(s) there are. Null when it could not be made.
static App* shellNewWindow(App& from, const json& place, bool remote = false) {
  App* a = new App(from);
  if (!a->initSecondary(from, place, remote)) { if (a->hwnd) { a->closing = true; DestroyWindow(a->hwnd); } delete a; return nullptr; }
  a->heapOwned = true;                                  // from here on it goes with its HWND (wndProc, WM_NCDESTROY)
  return a;
}
// main.js `window:open` (F5): a new deck window with `tileId` filling it; the tile leaves wherever it was. 80 % of
// the window it came from (800 x 500 at least); a point given is the grab point, 40 / 20 inside the new corner.
static App* shellOpenWindow(App& from, const std::string& tileId, bool at, int sx, int sy) {
  Shell& sh = shell();
  if (!from.tile(tileId)) return nullptr;
  if (!from.localMode) {
    // REMOTE (2026-11-19): a window of this process on the same socket (Shell::remoteOwner), holding the tile; the
    // tile leaves the window it came from (one home per tile among this machine's windows, as main.js does)
    App* o = sh.remoteOwner;
    if (!o || !o->inited || !o->st.connected || !sh.remoteInit.is_object()) return nullptr;
    const json base = from.boundsJson();
    const int bw = base.is_object() ? base.value("width", 1200) : 1200, bh = base.is_object() ? base.value("height", 800) : 800;
    json place = { { "width", std::max(800, (int)std::lround(bw * 0.8)) }, { "height", std::max(500, (int)std::lround(bh * 0.8)) } };
    if (at) { place["x"] = sx - 40; place["y"] = sy - 20; }
    App* a = shellNewWindow(from, place, true); if (!a) return nullptr;
    if (from.isPlaced(tileId)) from.layout = L::unplace(from.layout, tileId);
    if (from.st.selected == tileId) { auto ids = L::placedIds(from.layout); from.st.selected.clear(); if (!ids.empty()) from.activate(ids[0], true); }
    from.applyLayout();
    json lay = json::object(); { json cells = json::array(); for (int c : L::fullSet(a->grid)) cells.push_back(c); lay[tileId] = cells; }
    a->joinRemote(lay, tileId);
    if (from.testRun()) from.logLine("window opened (remote, shared socket): holding " + tileId + ", " + std::to_string(a->remoteWindows().size()) + " remote windows");
    else SetForegroundWindow(a->hwnd);
    return a;
  }
  if (!sh.host.running()) return nullptr;
  const json base = from.boundsJson();
  const int bw = base.is_object() ? base.value("width", 1200) : 1200, bh = base.is_object() ? base.value("height", 800) : 800;
  json place = { { "width", std::max(800, (int)std::lround(bw * 0.8)) }, { "height", std::max(500, (int)std::lround(bh * 0.8)) } };
  if (at) { place["x"] = sx - 40; place["y"] = sy - 20; }
  App* a = shellNewWindow(from, place); if (!a) return nullptr;
  a->client = sh.host.openWindow(a->hwnd, WM_WS, tileId, a->boundsJson());
  if (!a->client) { a->closing = true; a->endAsks(); DestroyWindow(a->hwnd); return nullptr; }
  if (from.testRun()) from.logLine("window opened: w" + std::to_string(sh.host.core().windowIndex(a->client)) + " holding " + tileId + (at ? " at " + std::to_string(sx) + "," + std::to_string(sy) : std::string()) + ", " + std::to_string(sh.apps.size()) + " windows");
  else SetForegroundWindow(a->hwnd);
  return a;
}
// R7: the windows the state file holds beyond the first come back, each with its place and its layout (R13: the
// core has already dropped an auxiliary window that had nothing in its cells).
static void shellRestoreWindows(App& first) {
  Shell& sh = shell();
  const json saved = sh.host.core().savedWindows();
  for (size_t i = 1; saved.is_array() && i < saved.size(); i++) {
    const json place = saved[i].is_object() && saved[i].contains("bounds") && saved[i]["bounds"].is_object() ? saved[i]["bounds"] : json::object();
    App* a = shellNewWindow(first, place); if (!a) break;
    a->client = sh.host.addWindow(a->hwnd, WM_WS, a->boundsJson());   // it takes the next saved place and layout
    if (first.testRun()) first.logLine("window restored: w" + std::to_string(sh.host.core().windowIndex(a->client)) + " layout " + (saved[i].is_object() && saved[i].contains("layout") ? saved[i]["layout"].dump() : std::string("{}")));
  }
  if (saved.is_array() && saved.size() > 1 && first.hwnd && !first.testRun()) SetForegroundWindow(first.hwnd);
}
// Every window goes at once and the process ends: a restart (the state file keeps every window), the end of a
// test's run, Windows shutting down. Not a window's own close — that is WM_CLOSE, one window at a time.
static void shellQuit(App* asking) {
  Shell& sh = shell();
  const std::vector<App*> all = sh.apps;
  bool local = false, test = false;
  for (App* a : all) { if (a->localMode) local = true; if (a->testRun()) test = true; if (!a->closing) { if (!a->testRun() && a->primary) a->savePrefs(); a->closing = true; a->beginWebShutdown(); } }
  if (local) {
    if (sh.host.running()) { if (!test) armExitWatchdog(sh.host.core().livePids()); sh.host.stop(); }
    if (sh.hub) KillTimer(sh.hub, TIMER_LOCALDECK);
  }
  for (App* a : all) { if (!a->localMode) a->stopTransport(); a->client = 0; a->endAsks(); }
  (void)asking;
  for (App* a : all) if (a->hwnd) DestroyWindow(a->hwnd);   // (a window opened from another deletes itself with its HWND)
}
// Every window but one closes, as its own close button would close it (its sessions run on).
static void shellCloseOthers(App* keep) {
  const std::vector<App*> all = shell().apps;
  for (App* a : all) if (a != keep && a->hwnd && !a->closing) { a->closing = true; a->beginWebShutdown(); a->stopTransport(); a->endAsks(); DestroyWindow(a->hwnd); }
}
// main.js maybeNotify `note.on('click')`: the window that holds the tile (else the first) comes up, on that tile.
// A toast was clicked. The CORE routes it (Deck::toastClicked): it names the window to raise (the one holding the
// tile, else the first) and has already sent that window `tiles:activate <tile>` or, for an ask toast, `asks:open
// { sessionId, tileId }`. Raising the window is the shell's. Without a running deck (no local deck in this
// process) the shell does what it always did: the window holding the tile, that tile selected.
static void shellToastClick(const core::Toast& toast) {
  Shell& sh = shell();
  const std::string& tileId = toast.tileId;
  App* w = nullptr; bool routed = false;
  if (sh.host.running()) {
    const core::ClientId c = sh.host.core().toastClicked(toast);
    if (c) for (App* a : sh.apps) if (!a->closing && a->client == c) { w = a; routed = true; break; }
  }
  if (!w) for (App* a : sh.apps) if (!a->closing && a->isPlaced(tileId)) { w = a; break; }
  if (!w) for (App* a : sh.apps) if (!a->closing) { w = a; break; }
  if (!w || !w->hwnd) return;
  if (w->testRun()) w->logLine("toast click -> window w" + std::to_string(w->st.windowIndex) + " comes up on tile " + tileId + (toast.kind == "ask" ? " (an ask toast: session " + toast.sessionId + ")" : "") + (routed ? " (routed by the core)" : ""));
  else { if (IsIconic(w->hwnd)) ShowWindow(w->hwnd, SW_RESTORE); ShowWindow(w->hwnd, SW_SHOW); SetForegroundWindow(w->hwnd); }
  if (routed) return;                                   // the core's event does the rest in that window
  if (toast.kind == "ask") { w->askSync(); w->askPane.onAskOpen(toast.sessionId, tileId); }
  else if (w->tile(tileId)) w->activate(tileId, true);
}

#include <commctrl.h>

// "Rename-aside" updates (PACKAGING.md request 5): Windows will not overwrite a running exe but lets it be RENAMED,
// so an installer moves the running one to `ClaudeDeckNative.exe.old` and copies the new one in under the real
// name; the running deck's update offer (lifecycle.hpp fingerprint: its file's size + time) then proposes the
// restart. The leftover is removed here, at start (and again after --wait-pid: until the old process is gone the
// file is still in use and the delete just fails).
static bool& updateAppliedAtStart() { static bool b = false; return b; }
static void removeStaleOld() { const std::wstring old = ownExePath() + L".old"; if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileW(old.c_str())) updateAppliedAtStart() = true; }

// `--register` / `--unregister [--shortcut-dir <folder>] [--shortcut-args "<args>"]`: the Start-menu shortcut with
// the AppUserModelID (notify.hpp ensureShortcut / removeShortcut: the same code a real run uses), and out. Nothing
// is shown, no deck is started, no lock is taken, no state folder is read. With --shortcut-dir the outcome is also
// one line in `<folder>/register.log` (never written into the real Start menu).
static int registerMain(const Options& o) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const std::string did = o.registerMode == 1 ? ensureShortcut(kAppId, o.shortcutDir, o.shortcutArgsSet ? &o.shortcutArgs : nullptr) : removeShortcut(o.shortcutDir);
  CoUninitialize();
  if (!o.shortcutDir.empty()) {
    std::ofstream f((o.shortcutDir + L"\\register.log").c_str(), std::ios::binary | std::ios::app);
    f << (o.registerMode == 1 ? "register: " : "unregister: ") << did << "\n";
  }
  if (o.registerMode == 1) return did == "created" || did == "updated" || did == "stamped" ? 0 : 1;
  return did == "removed" || did == "absent" ? 0 : did == "foreign" ? 2 : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES }; InitCommonControlsEx(&icc);
  Options o;
  int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; i++) {
    std::wstring a = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--link") o.link = next();
    else if (a == L"--shot") o.shot = next();
    else if (a == L"--probe") o.probe = next();
    else if (a == L"--window-shot") o.windowShot = next();
    else if (a == L"--select") o.select = next();
    else if (a == L"--desk-name") o.deskName = next();
    else if (a == L"--type") o.type = next();
    else if (a == L"--do") o.doScript = next();
    else if (a == L"--after") o.after = _wtoi(next().c_str());
    else if (a == L"--hang") o.hang = _wtoi(next().c_str());
    else if (a == L"--size") { std::wstring s = next(); size_t x = s.find(L'x'); if (x != std::wstring::npos) { o.width = _wtoi(s.substr(0, x).c_str()); o.height = _wtoi(s.substr(x + 1).c_str()); } }
    else if (a == L"--no-save") o.noSave = true;
    else if (a == L"--forget") o.forget = true;
    else if (a == L"--place") o.place = true;
    else if (a == L"--local") o.local = true;
    else if (a == L"--state-dir") o.stateDir = next();
    else if (a == L"--shell") o.shell = next();
    else if (a == L"--persist-shells") o.persistShells = true;
    else if (a == L"--bench") o.bench = next();
    else if (a == L"--fixture") o.fixture = next();
    else if (a == L"--frames") o.frames = _wtoi(next().c_str());
    else if (a == L"--dpi") o.forceDpi = _wtoi(next().c_str());
    else if (a == L"--no-scene") o.noScene = true;
    else if (a == L"--verify-scene") o.verifyScene = true;
    else if (a == L"--test-asks") o.testAsks = true;
    else if (a == L"--ask-pane") o.askPane = narrow(next());
    else if (a == L"--web-warm") o.webWarm = _wtoi(next().c_str());
    else if (a == L"--web-grace") { const std::wstring v = next(); o.askGrace = _wtoi(v.c_str()); const size_t c = v.find(L','); if (c != std::wstring::npos) o.cloudGrace = _wtoi(v.c_str() + c + 1); }
    else if (a == L"--web-flags") { o.webFlags = next(); o.webFlagsSet = true; }
    else if (a == L"--web-runtime") o.webRuntime = next();
    else if (a == L"--cloud-base") o.cloudBase = next();
    else if (a == L"--serve") { const int p = _wtoi(next().c_str()); if (p > 0 && p < 65536) o.servePort = p; }
    else if (a == L"--assets") o.assets = next();
    else if (a == L"--home") o.home = next();
    else if (a == L"--allow-claude") o.allowClaude = 1;
    else if (a == L"--no-claude") o.allowClaude = 0;
    else if (a == L"--driver-claim") o.driverClaim = true;
    else if (a == L"--register") o.registerMode = 1;
    else if (a == L"--unregister") o.registerMode = 2;
    else if (a == L"--shortcut-dir") o.shortcutDir = next();
    else if (a == L"--shortcut-args") { o.shortcutArgs = next(); o.shortcutArgsSet = true; }
    else if (a == L"--wait-pid") o.waitPid = (DWORD)_wtoi(next().c_str());
    else if (a == L"--fonts-dir") o.fontsDir = next();
    else if (a == L"--do-file") {                         // --do from a file (UTF-8): a script with quotes in it needs no quoting
      std::ifstream f(next().c_str(), std::ios::binary); std::stringstream ss; ss << f.rdbuf(); std::string t = ss.str();
      for (char& ch : t) if (ch == '\r' || ch == '\n') ch = ' ';
      o.doScript = widen(t);
    }
  }
  if (argv) LocalFree(argv);
  removeStaleOld();
  if (o.registerMode) return registerMain(o);
  // the link may also come from the environment, so a test never puts a token on a command line
  if (o.link.empty() && !o.local) { wchar_t buf[2048]; DWORD n = GetEnvironmentVariableW(L"CLAUDE_DECK_LINK", buf, 2048); if (n > 0 && n < 2048) o.link = buf; }
  App app;
  return app.run(inst, o);
}
