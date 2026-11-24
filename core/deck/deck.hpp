// core/deck/deck.hpp — THE DECK: what main.js is to the Electron app, as pure C++ over the seams of seams.hpp.
// Tiles and their processes, the call table (PHONE_ACTIONS + the window's IPC handlers), frame dispatch per
// core/PROTOCOL.md, the meta, replay, settings, the layout / one-window rules, the driver rules, persistence.
//
// MULTI-CLIENT, as the Electron main process is. A client is
//   - a WINDOW of this deck (addWindow): its input is the desktop's (`tile:input`, refused while a remote
//     drives the tile — R10), its resizes are the desktop's, its meta is collectMeta's own; or
//   - a REMOTE (addRemote): a phone until it calls `deskInit`, a desk after — one remote desk at a time
//     (lib/phone.js). Its calls go through PHONE_ACTIONS; its meta is clipped (remoteMeta), a desk's carries its
//     own `mirror` (deskMeta), and it is sent a meta only when it differs from the last one or 20 s after it.
// Each client has its own attached set, shown set, layout (a window entry) and rid space: `ok` / `err` go to
// the caller only, `data` to the clients carrying the tile, `exit` / `reset` to everyone.
//
// PUMPED, never threaded: the shell calls handle(client, frame), onPtyData / onPtyExit (bytes and exits of the
// processes PtyHost started), and tick() at least every nextDueMs() ms; every frame out goes through the sink,
// from inside one of those calls. The sink answers whether the frame went out: false = the client is busy
// (a socket over MAX_BUFFERED), the frame is skipped, and a skipped meta is sent again at the next tick.
//
// windows/HOST-NOTES.md lists every call's status and every difference from main.js.
#pragma once
#include "rules.hpp"
#include "seams.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace deck { namespace core {

using ClientId = int;

struct DeckOptions {
  std::string stateDir;                   // tiles.json, settings.json, triggers.json, held-prompts.json, uploads/ … (required)
  std::string home;                       // os.homedir() (required)
  std::string aliasFile;                  // platform.aliasFiles().primary (required)
  // THE SWITCH. Off (the default): a Claude tile is built, saved, restored and shown like any other, its
  // command line is computed by claudeLaunch() — and the launch itself is refused: the tile reads "failed to
  // start". On: the process is started through PtyHost. Nothing else in the deck changes with it.
  bool allowClaude = false;
  // main.js never writes a shell tile to tiles.json. On: shell tiles are saved and come back as fresh shells
  // (NOT parity; HOST-NOTES difference 1).
  bool persistShells = false;
  // Settings -> Accounts writes the alias file (main.js writeAliasFile). Off: saveAccounts is refused. The
  // alias file is the owner's own (userswitch reads it too), so a shell turns this on only on their word.
  bool allowAccountEdit = false;
  std::string shellFile = "powershell.exe";             // platform.shellCommand()
  std::vector<std::string> shellArgs = { "-NoLogo" };
  std::string shellName = "PowerShell";
  std::string claudeExe = "claude.exe";                 // platform.claudeExe(), resolved by the shell (core/deck/platform.hpp)
  std::string bash = "bash.exe", userswitch;            // platform.bashPath() / userswitchPath()
  std::string cwdOverride;                              // CLAUDE_DECK_CWD
  std::string version;                                  // app.getVersion(), for deck-state.json ("" = null)
  // labels to show when this deck's own settings have none for an account: { login e-mail: short label }, e.g. the
  // Electron deck's settings.json `aliases` (office check 2026-11-20: a first local run showed the e-mails)
  nlohmann::json labelFallback = nlohmann::json::object();
  // ---- how a REMOTE DESK and this deck's own windows live together (the review of 2026-11-18)
  //
  // THE REMOTE-DESK RULE, on by default (remoterule.hpp; "'it only shows one place' should only be on the same
  // box ... the other things panes drive (going to sleep) shouldn't impact it from a remote session"): one home
  // per session, the "in window N" mark, the leaving mark and hibernation's "on screen" are decided among the
  // deck's OWN windows; a remote desk's layout evicts nothing and is never evicted; a desk is told
  // `inWindow: null`; an asleep session is still woken when it ARRIVES on a remote's screen. Size is not part of it.
  // TRUE restores what main.js on `main` does today (a remote desk is a window like any other for the one-window
  // rule and for sleep) — for a deck that must behave as the Electron deck does until the owner merges their
  // rule there, and for the tests of that rule.
  bool remoteDeskIsAWindow = false;
  // THE DRIVER AS A CLAIM — A PROPOSAL the owner has not accepted, so OFF (driverrule.hpp). On: the driver is
  // `auto | office | phone | remote` (`desk` is taken as `office`), a remote desk is its own kind of screen (not
  // a phone), every tile's meta carries `sizeBy`, `mirror` is the pty's own size, meta has `remoteDesk =
  // { name, kind }`, a remote desk's typing marks a tile's `driver` as `remote`, and R10 locks the office on a
  // session another kind drives. Off: this tree's words and rules exactly (`auto | phone | desk`, a remote desk
  // counts as a phone), and NEITHER `sizeBy` NOR `remoteDesk` is sent — on this tree's protocol either key tells
  // a client "this deck has the claim" (core/PROTOCOL.md "A new deck is any ONE of…").
  // THE DAY HE ACCEPTS IT: change this one initialiser to `true` (nothing else reads a default).
  bool driverClaim = false;
  // The desk slot (lib/phone.js DESK_SILENT_MS / DESK_PROBE_MS, 2026-11-18): a new deskInit takes the slot from
  // a desk that has not been heard from for `deskSilentMs` and then lets a ping go unanswered for `deskProbeMs`.
  // It needs a PhoneHost that knows when a client was last heard from (core/deck/server.hpp does); without one
  // a connected desk always keeps the slot.
  double deskSilentMs = 10000;
  double deskProbeMs = 3000;
  // ASK PAGES (lib/asks.js). Where the ask-page skill keeps its state and where its Done saves answers, as
  // lib/asks.js `locations()` resolves them (core/deck/asks.hpp askLocations does that for a shell). EMPTY = the
  // ask feed is OFF: meta.asks / askLog are [], no deck.json is written, the ask calls answer as main.js does
  // with `asks` null. A deck with a feed WRITES <askStateDir>/deck.json every 30 s (the skill reads it): a test
  // must hand in a temp folder, never the owner's own.
  std::string askStateDir;
  std::vector<std::string> askDownloads;
  // The on-call's desk requests (lib/deskopen.js): <stateDir>/desk-open is read every 1.5 s. The folder is the
  // deck's own; a request is still refused unless settings.deskOpen is on and (to start a session) allowClaude.
  bool deskOpenInbox = true;
  // CLOUD SESSIONS (lib/cloud.js). `cloudPolling`: read each cloud tile's session and each account's live list
  // from Anthropic's API with the account's own token — only with Http + Credentials from the shell AND this on
  // (the owner's word, as usage polling). `allowCloudCreate`: `claude --cloud "<task>"` may be started (it makes
  // a session in Anthropic's cloud); needs allowClaude too. Off: making one is refused by name; a cloud tile
  // that already has its session (restored, or opened from Elsewhere) is held either way — it runs nothing here.
  bool cloudPolling = false;
  bool allowCloudCreate = false;
  // Usage is polled when the shell hands in Http + Credentials (the owner's word) — unless this says no (a shell
  // that gave the two seams for cloud polling only).
  bool usagePolling = true;
  // ONE DECK AT A TIME + THE SHARED LAYOUT (2026-11-24; core/deck/onedeck.hpp, lib/onedeck.js). null = off (every
  // test deck on the fakes, a remote). On: { deck: "native", sharedDir, exe, args: [...], defaultDeck: bool } and, for a
  // test, otherExe / otherState (a stand-in for the other deck's process and state folder). The deck writes the marker
  // <sharedDir>\active.json at start, its heartbeat every 30 s and running:false at its shutdown.
  json oneDeck;
  json keyHelp;                                         // platform.keyHelp (null = the Windows one)
  std::string fontStack;                                // platform.defaultFontStack ("" = the Windows one)
};

// The phone server as the deck sees it (main.js phoneStatus / syncPhone). core/deck/server.hpp implements it.
class PhoneHost {
 public:
  virtual ~PhoneHost() {}
  virtual json status(const json& phoneSettings) = 0;   // phoneStatus(): { enabled, running, port, bind, token, address, serve, url, clients, desks, tailscale, error }
  virtual void sync(const json& phoneSettings) = 0;     // syncPhone(): bring the server in line with the settings
  // what rides with every remote meta and the hello: { assets, build, pageKey }
  virtual json extras() = 0;
  // ---- the heartbeat, as the desk slot needs it (lib/phone.js heard / probe / drop)
  // When a remote client was last heard from (a pong, any message), on the deck's clock; < 0 = not known.
  virtual double heardAt(ClientId /*client*/) { return -1; }
  // Ping it now; false = it could not be sent.
  virtual bool ping(ClientId /*client*/) { return false; }
  // Terminate it (no close handshake: there is nobody to answer one). The host calls Deck::removeClient.
  virtual void drop(ClientId /*client*/) {}
};
// What only the shell can do about the process itself.
class Lifecycle {
 public:
  virtual ~Lifecycle() {}
  virtual bool restart(const std::string& /*why*/) { return false; }   // restartDeck: relaunch; false = it cannot
  // the main-process files as they are on disk now ({ name: hash }), for the update offer; null = not offered
  virtual json fingerprint() { return json(); }
  // The quit has begun (the last window closed, or a restart): the process must be gone within `ms` whatever
  // holds it (main.js armExitWatchdog: a pseudoconsole that will not close). Called at most once.
  virtual void armExitWatchdog(double /*ms*/) {}
  // shell.openExternal / openPath / showItemInFolder: a link in the default browser, a file in its own program,
  // a file shown in its folder. The core has already decided WHICH (main.js open:external, openAskLink).
  virtual bool openExternal(const std::string& /*url*/) { return false; }
  virtual bool openPath(const std::string& /*path*/) { return false; }
  virtual bool showInFolder(const std::string& /*path*/) { return false; }
  // A new window of this deck for `client` (already registered, its layout set): the on-call's desk when no
  // cell is free anywhere (main.js placeDeskTile -> createWindow(bounds, { inactive: true })). False = this
  // shell cannot open one; the core then leaves the tile in the main window's sidebar.
  virtual bool openWindow(int /*client*/, const json& /*bounds*/, bool /*inactive*/) { return false; }
};

struct Seams {
  Clock* clock = nullptr; Files* files = nullptr; PtyHost* ptys = nullptr; ProcessList* procs = nullptr;
  Random* random = nullptr; Env* env = nullptr;
  Notifier* notifier = nullptr; Http* http = nullptr; Credentials* credentials = nullptr;
  PhoneHost* phone = nullptr; Lifecycle* lifecycle = nullptr;
  Log* log = nullptr;                               // optional: the deck's event log (seams.hpp Log)
};

class Deck {
 public:
  using Sink = std::function<bool(ClientId, const json& frame)>;

  Deck(DeckOptions options, Seams seams, Sink sink);
  ~Deck();
  Deck(const Deck&) = delete;
  Deck& operator=(const Deck&) = delete;

  // Load settings + state and restore the saved tiles through the 400 ms spawn queue (main.js whenReady).
  void start();

  // ---- clients
  // A window of this deck. The n-th one made takes the n-th saved window's bounds and layout (R7) — whether it
  // is made before start() (it takes its place when start() reads the state) or after it; `bounds` is what the
  // shell opened it at when nothing was saved. Indexes count up from 0 and are never reused.
  // A window made when the deck has NONE left (the Mac: Cmd+W on the last window, then the Dock icon — main.js
  // `activate`) takes the FIRST entry of the state file as it is NOW: the closed window's own bounds and layout
  // (the last window closing leaves the file as it was), with a new index.
  // It is sent `init` (a window ignores it and calls deskInit). Returns its client id; its window index is
  // windowIndex(id).
  ClientId addWindow(const json& bounds = json());
  // The id the NEXT addWindow / openWindow / addRemote will return. A host reads it just before the call, so the
  // frames raised from inside the call (the hello) can be routed to the newcomer by its exact id.
  ClientId nextClientId() const;
  // F5 `window:open`: a new window holding `tileId` alone, full (the tile leaves wherever it was).
  ClientId openWindow(const std::string& tileId, const json& bounds);
  // The shell's own facts about a window: on screen (not hidden / minimised), focused; its bounds when moved.
  void windowState(ClientId window, bool visible, bool focused);
  void windowBounds(ClientId window, const json& bounds);
  // A remote socket opened: it is sent the hello (`init`, a phone's boot).
  // `viaDesk`: it came in on the desk's socket path (/desk/…), so it is not a phone page even before its deskInit
  ClientId addRemote(bool viaDesk = false);
  // A key went down in one of the deck's own windows, wherever in it (main.js windowKeys): the user is at this PC.
  void windowKey(ClientId window);
  // A window closed, or a remote socket closed (a desk's window entry goes with it).
  void removeClient(ClientId client);
  int windowIndex(ClientId client) const;                 // -1 when it has no window entry
  json savedWindows() const;                              // [{ bounds, layout }] the state file holds (after start())

  // ---- the pump
  void handle(ClientId client, const json& frame);
  void onPtyData(uint64_t token, const char* bytes, size_t n);
  void onPtyExit(uint64_t token, uint32_t exitCode);
  void tick();                                            // run the timers that are due
  int nextDueMs() const;                                  // 0 = now; never more than 2000
  // window-all-closed: every process killed (killAll). State was saved as it changed.
  void shutdown();

  // ---- for the server, tests and diagnostics
  int remoteCount() const;                                // phone.clients.size
  int deskCount() const;
  const json& settings() const;
  json collectMeta();                                     // one pass, with its stamping, as main.js
  ClaudeLaunch lastClaudeLaunch(const std::string& tileId) const;
  std::vector<uint32_t> livePids() const;
  std::pair<int, int> ptySize(const std::string& tileId) const;
  const std::string& stateDir() const;
  bool restoring() const;
  // a file from the phone (main.js saveUpload): kept under <stateDir>/uploads, dated, the name made safe; the path
  std::string saveUpload(const std::string& name, const std::string& bytes);
  // ---- for the server's routes (lib/phone.js deps.askPage / deps.viewFile)
  // /ask/<id>.html: the page file of an ask some session's ask-page state names; "" = no such page
  std::string askPage(const std::string& pageId);
  // /view/<key>/<folder id>/<parts…>: a file in a folder the deck was asked to show (lib/viewer.js); "" = none
  std::string viewFile(const std::string& folderId, const std::vector<std::string>& parts);
  // ---- for the shell
  // A toast the deck raised was CLICKED (main.js note.on('click')): the window to bring up is returned (0 = none)
  // and is sent `tiles:activate` (the tile) or `asks:open` (an ask toast). Raising the window is the shell's.
  ClientId toastClicked(const Toast& toast);
  // Run the desk-open inbox now (the 1.5 s poll does it anyway): the answers written this pass.
  std::vector<json> drainDeskInbox();
  // main.js relaunchSpec(): what <stateDir>/relaunch.json names as the command a restart starts instead of the
  // exe itself ({ command, args }), or null. The shell's Lifecycle::restart runs it; no screen can write that file.
  json relaunchSpec() const;

  struct Impl;
 private:
  std::unique_ptr<Impl> d;
};

}}  // namespace deck::core
