// windows/host/localdeck.hpp — the deck, in-process, for a Windows shell. THE DECK ITSELF IS core/deck/deck.hpp
// (pure C++, multi-client, shared with the Swift shell); this class is its Windows half: the seams (ConPTY,
// files, the process list, Winsock), the queue the pty and socket threads post to, and ONE window client — the
// shell that made it. A phone or a remote desk reaches the same deck through the server (servePort / settings).
//
// What follows describes that one window: what main.js is to the Electron window, speaking
// core/PROTOCOL.md's frames instead of Electron IPC. A shell (windows/src/app.cpp) that today sends frames to
// a remote deck over a websocket can hand the same frames to handle() and get the same frames back through
// the emit callback — local mode, no socket (docs/NATIVE-PORT.md §4, PROTOCOL.md "Local vs remote").
//
// The local shell is THE DESKTOP WINDOW of this deck (window index 0), not a remote: its input is the
// desktop's (`tile:input`, refused only while the driver setting says `phone`), its resizes are the
// desktop's, its meta is collectMeta's own (no per-desk `mirror` rewrite).
//
// ---------------------------------------------------------------- threads
// ONE thread owns a LocalDeck — the thread that constructs it (the window thread). Every public method is
// called on that thread and the emit callback is only ever called on it, from inside start(), handle() or
// pump(). Nothing here locks on the owner's behalf.
// Each running tile has two ConPty threads (a reader and a waiter). They never touch a tile and never call
// emit: they append to one mutex-guarded queue and wake the owner — by PostMessage(notifyHwnd, notifyMsg,
// 0, 0) when a window was given (at most one message outstanding at a time), and by a condition variable
// that waitForWork() sleeps on (headless callers). The owner then calls pump(), which drains the queue
// (pty output -> replay + `data` frames, exits -> `exit` frames) and runs the timers that have come due
// (the 150 ms resize debounce, the nudge's second half, the 400 ms spawn queue, the 2 s meta tick, the
// kill grace). Timers only fire inside pump(): call it at least every nextDueMs() milliseconds (a WM_TIMER).
//
// ---------------------------------------------------------------- what it holds
// Tiles (shell tiles fully; Claude tiles in every respect but the launch — see allowClaude), the window's
// shown set and layout, the settings, groups; tiles.json + settings.json in ITS OWN state folder, never the
// Electron deck's. See windows/HOST-NOTES.md for every call of the protocol table and what is refused.
#pragma once
#include "../../core/deck/onedeck.hpp"
#include "conpty.hpp"
#include "deckrules.hpp"
#include "../../core/deck/deck.hpp"
#include "../../core/deck/options.hpp"
#include "../../core/deck/server.hpp"
#include "../../core/third_party/nlohmann/json.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace deck { namespace host {

// THE SHIPPING DEFAULT of allowClaude (decided 2026-11-18, after the launch was proven against a real claude.exe:
// HOST-NOTES.md "The real runs"). The owner's goal is a native deck that replaces the Electron one, so a REAL
// local deck starts Claude sessions, and nothing else does unless it says so:
//   - the app itself (the process is ClaudeDeckNative.exe) on its own state folder (no `--state-dir`): ON;
//   - the app on a scratch state folder (`--state-dir <folder>`: every test of the app — app.cpp refuses a test
//     run without one — and any throwaway deck): OFF;
//   - any other process (windows/host-test's exes, a tool that links the host): OFF;
//   - `--allow-claude` on the command line turns it on, `--no-claude` off, wherever; the last of the two wins.
// Pure: `exe` is the process's file (a path or a bare name), `args` its arguments without the program.
bool shippingAllowClaude(const std::wstring& exe, const std::vector<std::wstring>& args);
// …for THIS process (its own file name and command line), read once.
bool shippingAllowClaudeHere();

// THE SHIPPING DEFAULT of the ask feed (lib/asks.js; decided 2026-11-19). A deck with a feed READS the ask-page
// skill's state folder and WRITES deck.json into it every 30 s (the skill then opens no browser window for an ask
// page: the deck shows it). That folder is the owner's real one unless ASK_PAGE_STATE_DIR says otherwise, so:
//   - the app itself (ClaudeDeckNative.exe) on its own state folder (no `--state-dir`): ON, the real folder, as
//     the Electron deck (lib/asks.js locations());
//   - anything else (a scratch `--state-dir` deck, a test exe): ON only when ASK_PAGE_STATE_DIR is set in the
//     environment (`envNamesAskDir`) — a throwaway deck never touches the real folder by default. (The Electron
//     deck does: its scratch decks use the real folder unless the variable is set. Deliberately stricter here.)
//   - `--no-asks` turns it off, `--asks` on, wherever; the last of the two wins.
bool shippingAsks(const std::wstring& exe, const std::vector<std::wstring>& args, bool envNamesAskDir);
bool shippingAsksHere();
// THE SHIPPING DEFAULT of the exit watchdog (main.js armExitWatchdog): the process is ended 8 s after the deck
// began to shut down if it is still there. ON only when the process IS ClaudeDeckNative.exe — a test exe makes
// and shuts down many decks and must outlive each of them.
bool shippingExitWatchdog(const std::wstring& exe);
bool shippingExitWatchdogHere();
// 2026-11-19 (an ask page): usage polling, cloud sessions (polling them and making them), the account editor and
// the driver as a claim are ON in the app's own real run, as the Electron deck has them; off under --state-dir (every
// test) and for any other exe.
bool shippingOwnerSwitchesHere();
// The watchdog itself: one detached thread that, `ms` later, ends every process this one started (a Toolhelp
// snapshot; no helper process, nothing on screen) and then this process, exit code 0. Arms once per process.
void armProcessExitWatchdog(double ms);

struct LocalDeckOptions {
  // Where tiles.json and settings.json live. Empty = %APPDATA%\claude-deck-native\local. (UTF-8)
  std::string stateDir;
  // os.homedir(). Empty = %USERPROFILE%. Accounts' default dir, the default cwd, ~/.local/bin/claude.exe.
  std::string home;
  // The account alias file. Empty = %USERSWITCH_ZSHRC% or <home>\.claude-code-account-aliases.
  std::string aliasFile;
  // THE SWITCH. Off: a Claude tile is built, saved, restored and shown like any other, its command line is
  // computed by claudeLaunch() — and the launch itself is refused: the tile reads "failed to start". On: the
  // process is started. Nothing else in the deck changes with it.
  // ITS DEFAULT is shippingAllowClaudeHere() (above): on in the app's own real run, off everywhere else — so a
  // shell that leaves it alone (windows/src/app.cpp reads it for its "Claude is off here" look) ships with Claude
  // sessions on, and nothing that merely constructs a LocalDeck in a test can start one.
  bool allowClaude = shippingAllowClaudeHere();
  // main.js never writes a shell tile to tiles.json: a terminal does not come back after a restart (the
  // sidebar's restart strip counts them as lost). Off (the default) keeps that. On: shell tiles are saved
  // and come back as fresh shells in the same folder, pin, group and title — NOT parity; for a shell-only
  // local deck, where otherwise nothing at all would survive a restart.
  bool persistShells = false;
  // The "New terminal" program (lib/platform.js shellCommand): powershell.exe -NoLogo, named PowerShell.
  std::wstring shellFile = L"powershell.exe";
  std::vector<std::wstring> shellArgs = { L"-NoLogo" };
  std::string shellName = "PowerShell";
  // platform.claudeExe(process.env.CLAUDE_DECK_CLAUDE): empty = <home>\.local\bin\claude.exe when it is a
  // file, else "claude.exe" (a Path lookup).
  std::string claudeExe;
  // Woken by PostMessage(notifyHwnd, notifyMsg, 0, 0) when pty output or an exit is waiting for pump().
  void* notifyHwnd = nullptr;
  unsigned notifyMsg = 0;
  // The phone / desk server (lib/phone.js). It follows settings.json `phone` exactly as the Electron deck's does
  // (enabled, port, bind: tailscale | localhost | all, token). `servePort` > 0 is `--serve <port>`: for THIS run
  // the server is on, on 127.0.0.1:<port>, whatever the settings say (nothing is written to them).
  int servePort = 0;
  // Where the pages it serves live: the folder holding renderer/, lib/, assets/ and node_modules/@xterm (a
  // checkout). Empty = looked for upwards from the exe (HOST-NOTES.md "The server").
  std::string assetRoot;
  // Usage bars: poll Anthropic's usage endpoint per account with the account's own sign-in token, read (never
  // written) from <configdir>\.credentials.json — lib/usage.js. OFF by default and NO app switch sets it: reading
  // the accounts' tokens is the owner's to allow (HOST-NOTES.md). The cache (<stateDir>\usage-cache.json) holds
  // usage numbers only.
  bool usagePolling = shippingOwnerSwitchesHere();
  // Diagnostics (the real-Claude test): called on the deck's thread for everything the deck does TO a tile's
  // process — token = which process (1, 2, … in the order started), what = "spawn" | "write" | "resize" | "kill".
  std::function<void(uint64_t token, const char* what, const std::string& detail)> ptyTrace;

  // ---- what the 2026-11-19 build added (HOST-NOTES.md "Plugging it in")
  // The shell's toasts and taskbar flash (core::Notifier): a session waiting, a long turn done, a new ask page,
  // a desk the on-call opened, a settings warning skipped, memory nearly full. Null = none are raised. A clicked
  // toast goes back through core().toastClicked(toast).
  core::Notifier* notifier = nullptr;
  // What only the shell can do about the process and the desktop (core::Lifecycle): restart, the update
  // fingerprint, openExternal / openPath / showInFolder, a second window. Null = none of them (each answers false).
  // The exit watchdog is the host's own (below) whether or not one is given.
  core::Lifecycle* lifecycle = nullptr;
  // The ask feed (lib/asks.js): shippingAsksHere() above. `askStateDir` / `askDownloads` name the folders outright
  // (a test's temp folders) and turn the feed on; empty = resolved as lib/asks.js locations() does when `asks` is on.
  bool asks = shippingAsksHere();
  std::string askStateDir;
  std::vector<std::string> askDownloads;
  // The on-call's desk requests (<stateDir>\desk-open): on, as in the Electron deck. The folder is the deck's own.
  bool deskOpenInbox = true;
  // Cloud sessions (lib/cloud.js). `cloudPolling` reads the accounts' tokens and asks Anthropic's API, exactly as
  // usagePolling does: OFF by default, the owner's to allow, never run for real. `allowCloudCreate` lets
  // `claude --cloud "<task>"` be started (it makes a session in Anthropic's cloud): OFF by default.
  bool cloudPolling = shippingOwnerSwitchesHere();
  bool allowCloudCreate = shippingOwnerSwitchesHere();
  // Settings -> Accounts may write the alias file (core::HostOptions::allowAccountEdit)
  bool allowAccountEdit = shippingOwnerSwitchesHere();
  // End the process 8 s after shutdown() began if it is still there (shippingExitWatchdogHere()).
  bool exitWatchdog = shippingExitWatchdogHere();
  // app.getVersion(), for deck-state.json's `deck.version`. Empty = core/deck/version.hpp's kNativeDeckVersion,
  // the one place the version is written (buildDeckOptions).
  std::string version;
  // labels to show when this deck's own settings have none for an account: { login e-mail: short label }, e.g. the
  // Electron deck's settings.json `aliases` (office check 2026-11-20: a first local run showed the e-mails)
  nlohmann::json labelFallback = nlohmann::json::object();
  // One deck at a time + the shared layout (core/deck/deck.hpp DeckOptions::oneDeck): null = off. windows/src/app.cpp
  // fills it for a LOCAL deck: { deck, sharedDir, exe, args, defaultDeck } (+ otherExe / otherState in a test).
  nlohmann::json oneDeck;
  // ---- the review of 2026-11-18 (core/deck/deck.hpp has the long form; HOST-NOTES.md "The remote desk")
  // The remote-desk rule is the default: a remote desk is not one of the deck's own windows (one home per session
  // and sleep are this box's). True = the Electron deck's behaviour on `main` today.
  bool remoteDeskIsAWindow = false;
  // The driver as a claim — a proposal, so whatever the core's default is (false until it is accepted; the day it
  // is, core::DeckOptions::driverClaim's initialiser flips and this follows).
  bool driverClaim = shippingOwnerSwitchesHere();
};

// THE ONE PLACE a shell's options become the core's is core/deck/options.hpp (core::buildDeckOptions, shared with
// the Mac shell): every option the core grows is set THERE. These two are the Windows doorway to it — LocalDeck
// uses them, and so must any other shell code that makes a deck itself (windows/src/deckhost.hpp).
//   toHostOptions     a LocalDeckOptions as the core's builder reads it (UTF-8; + this exe's folder, from which the
//                     asset root is looked for upwards when none was named)
//   buildDeckOptions  core::buildDeckOptions over the Windows files. `env` supplies the home (WinEnv(o.home)).
//     options    core::DeckOptions, complete      server     core::ServerOptions: the asset root and --serve's port
//     stateDir   the state folder as resolved     wantsHttp  hand the core Http + Credentials (polling was asked for)
// The shipping rules above are core/deck/options.hpp's too (shippingAllowClaude / shippingAsks /
// shippingExitWatchdog), asked with "is this exe ClaudeDeckNative.exe".
using DeckBuild = core::DeckBuild;
core::HostOptions toHostOptions(const LocalDeckOptions& o);
DeckBuild buildDeckOptions(const LocalDeckOptions& o, core::Env& env);

// ONE DECK AT A TIME, before a shell starts its deck (core/deck/onedeck.hpp; the shell asks, then starts or not):
//   oneDeckGuard   is the OTHER deck running with live sessions? (the marker, a process snapshot, its tiles.json)
// A LocalDeckOptions without oneDeck answers "not busy".
core::onedeck::Busy oneDeckGuard(const LocalDeckOptions& o);
//   oneDeckPreflight   the shared layout at start: { shared, decision: { action: none|write|import|ask, why, mine, theirs } }
//   oneDeckImport      back up this deck's files, then take the shared layout in (the decision, or the question's answer)
//   oneDeckKeepOwn     the question's other answer: the shared file backed up and marked seen, so this deck replaces it
// Each returns / writes nothing for a LocalDeckOptions without oneDeck. The folder written is the deck's own (+ backups).
nlohmann::json oneDeckPreflight(const LocalDeckOptions& o);
std::string oneDeckImport(const LocalDeckOptions& o, const nlohmann::json& shared, const std::string& why);
std::string oneDeckKeepOwn(const LocalDeckOptions& o, const nlohmann::json& shared);

class LocalDeck {
 public:
  using Emit = std::function<void(const json& frameToShell)>;

  LocalDeck(LocalDeckOptions options, Emit emit);
  ~LocalDeck();                           // shutdown()
  LocalDeck(const LocalDeck&) = delete;
  LocalDeck& operator=(const LocalDeck&) = delete;

  // Load settings + state, restore the saved tiles (through the 400 ms spawn queue, as main.js does) and
  // emit the hello: { t:'init', meta, home, platform, build, assets }.
  void start();
  // One frame from the shell (core/PROTOCOL.md "Shell -> core"). Replies and anything the frame causes are
  // emitted before this returns, except what a kill or a timer finishes later (emitted from pump()).
  void handle(const json& frameFromShell);
  // Drain pty output / exits and run due timers. Cheap when there is nothing to do.
  void pump();
  // Milliseconds until a timer is due (0 = now); never more than 2000 (the meta tick).
  int nextDueMs() const;
  // Headless callers: sleep until pty work is queued or `ms` has passed. True when work is queued.
  bool waitForWork(int ms);
  // What window-all-closed does: every pty killed (killAll), threads joined. Nothing is running afterwards.
  // State was saved as it changed; this writes nothing.
  void shutdown();

  // ---- for tests and diagnostics
  // The spec the seam built for a Claude tile's most recent start (empty file = it never tried).
  ClaudeLaunch lastClaudeLaunch(const std::string& tileId) const;
  // every live process the deck started (each tile's child and its descendants)
  std::vector<uint32_t> livePids() const;
  // the pty's size as the deck last set it ({0,0} when the tile has no process)
  std::pair<int, int> ptySize(const std::string& tileId) const;
  const std::string& stateDir() const;
  // main.js phoneStatus(): { enabled, running, port, bind, token, address, serve, url, clients, desks, tailscale, error }
  // + two of this server's own: `assets` ('ok' | 'missing': are the pages under the asset root) and `assetRoot`
  // (the folder that was tried, "" = none found) — for the shell to say so beside the link
  json serveStatus() const;
  // the deck itself (core/deck/deck.hpp), for a shell that wants more than frames: windows, their state
  core::Deck& core();
  // this deck's one window as the core knows it: for core().windowState / windowKey / windowBounds
  core::ClientId windowClient() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}}  // namespace deck::host
