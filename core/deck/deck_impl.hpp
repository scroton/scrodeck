// core/deck/deck_impl.hpp — Deck::Impl, shared by deck.cpp (tiles, processes, clients, the call table) and
// deck_more.cpp (hibernation, restart when quiet, triggers, sessions elsewhere / past, moves). Private to core/deck.
#pragma once
#include "onedeck.hpp"
#include "deck.hpp"
#include "hash.hpp"
#include "asks.hpp"
#include "cloud.hpp"
#include "deskopen.hpp"
#include "driverrule.hpp"
#include "held.hpp"
#include "presence.hpp"
#include "remote.hpp"
#include "remoterule.hpp"
#include "restorerefresh.hpp"
#include "pause.hpp"
#include "auditors.hpp"
#include "edition.hpp"
#include "startup.hpp"
#include "statefile.hpp"
#include "transcript_page.hpp"
#include "usage.hpp"
#include "util.hpp"
#include "viewer.hpp"
#include "../model/layout.hpp"
#include "../session/jsutil.hpp"
#include "../session/livestate.hpp"
#include "../session/meta.hpp"
#include "../session/replay.hpp"
#include "../session/transcript.hpp"

#include <deque>
#include <map>
#include <optional>
#include <set>

namespace deck { namespace core {

namespace js = deck::session::js;
using deck::session::TileState;

constexpr double META_INTERVAL_MS = 2000;
constexpr double SPAWN_GAP_MS = 400;
constexpr double ACCOUNT_REFRESH_MS = 60000;
constexpr double HIBERNATE_TICK_MS = 30000;
constexpr double QUIET_TICK_MS = 2000;
constexpr double RESTORE_REFRESH_TICK_MS = 2000;      // main.js RESTORE_REFRESH_TICK_MS
constexpr double PAUSE_TICK_MS = 2000;                // main.js PAUSE_TICK_MS
constexpr double AUDITOR_TICK_MS = 2000;              // main.js AUDITOR_TICK_MS (auditors.hpp)
constexpr double SHELL_HOST_TICK_MS = 3000;
constexpr double UPDATE_CHECK_MS = 30000;
constexpr double USAGE_TICK_MS = 10000;
constexpr double HELD_TICK_MS = 5000;
constexpr double TRIGGER_INBOX_MS = 4000;
constexpr double DESK_INBOX_MS = 1500;                 // lib/deskopen.js start(pollMs = 1500)
constexpr double EXIT_WATCHDOG_MS = 8000;              // main.js armExitWatchdog
constexpr size_t CLOUD_TASK_MAX = 24000;               // main.js CLOUD_TASK_MAX

inline json optJ(const std::optional<std::string>& s) { return s && !s->empty() ? json(*s) : json(); }
inline json numJ(double d) { return d == std::floor(d) && std::fabs(d) < 9e15 ? json((int64_t)d) : json(d); }

extern const char* const kAskGuestReason;            // why a guest deck refuses askDone / askDraft / askClear
bool terminalReply(const std::string& s);            // main.js TERMINAL_REPLY
bool usageNotice(const std::string& text);           // main.js USAGE_NOTICE
std::string cleanTriggerName(const std::string& name);   // lib/triggers.js cleanName ("" = not a name)

struct TrustWatch { std::string text; bool done = false; double since = 0; bool answering = false; int tries = 0; };

struct Tile {
  TileState s;
  session::Replay replay;
  session::Utf8Decoder decoder;
  std::unique_ptr<Pty> pty;
  uint64_t gen = 0;                        // which process the tile holds: events of an older one are dropped (`tile.pty !== p`)
  uint32_t pid = 0;
  double createdAt = 0;
  std::unique_ptr<session::TranscriptTail> tail;
  std::string tailFile; uint64_t tailOffset = 0;
  bool hasPending = false; int pendCols = 0, pendRows = 0; int resizeTimer = 0;
  bool hasLastSize = false; int lastCols = 0, lastRows = 0;
  bool resumable = false; double resumableAt = 0;
  // a move under way (main.js tile.movePending): the account it left and when; a quick non-zero exit puts it back
  std::optional<std::string> movePendingFrom; double movePendingAt = 0;
  ClaudeLaunch lastLaunch;
  bool inShell = false;                    // shellTookSession: the pty is a shell with Claude running inside it
  // lib/tilefollow.js: the claude.exe this start runs, learned from its record (pid + procStart); `direct` = the pty IS
  // claude.exe (its pid writes sessions/<pid>.json); followRefused = the id last refused (another tile holds it)
  bool direct = false, claudeBound = false; double claudePid = 0; json claudeProcStart;
  std::optional<std::string> followRefused;
  std::unique_ptr<StartupWatch> startup;
  std::unique_ptr<TrustWatch> trust;
  json meta;                               // t.meta: what the last pass published
  // the same-session guard (onedeck.hpp holderOf): its session is live in another process, so it was not resumed here.
  // "deck" = the other Claude Deck has it, "terminal" = a Claude process outside both decks (elsewherePid)
  std::string elsewhere; double elsewherePid = 0; int elsewhereTries = 0;
};

struct SpawnSpec {
  std::string kind;
  std::optional<std::string> account, sessionId, cwd, moveFrom;
  bool hasBrowser = false; std::optional<bool> browser;
  bool hasDesk = false; std::optional<std::string> desk;
  std::string model, prompt;               // this start only
  std::optional<std::string> cloudId, task; // kind 'cloud': the session it holds, or the task `claude --cloud` makes one from
};

// A window of the deck (main.js `wins`): a real one, or a remote desk's virtual entry.
struct Entry {
  int index = 0;
  ClientId client = 0;
  bool remote = false;
  std::vector<std::string> shown; bool hidden = false;
  layout::LayoutMap layout;
  json bounds;
  bool visible = true, focused = false;    // the shell's word (a real window)
};
struct Client {
  ClientId id = 0;
  bool remote = false;
  bool inited = false;                     // a window: deskInit was called
  bool desk = false;                       // a remote: deskInit registered it as a window
  bool viaDesk = false;                    // a remote that came in on the desk's socket path
  json deskName; std::string deskKind = "browser";      // what its deskInit said ({ name, kind })
  Entry* entry = nullptr;
  bool hasAttached = false; std::string attached;        // a phone shows one tile
  std::set<std::string> attachedSet;                     // a window / a desk: every pane it carries
  std::set<std::string> shownSet; bool hidden = false;   // a desk's ws.shown / ws.hidden (as sent, unfiltered)
  bool hasMetaKey = false; std::string metaKey; double metaAt = 0;
};

struct QuietRestart {                      // lib/quietrestart.js
  double settleMs = 60000;
  bool armed = false; std::string why; double armedAt = 0; bool hasQuietSince = false; double quietSince = 0;
  void arm(const std::string& w, double now) { if (armed) { if (w == "manual") why = "manual"; return; } armed = true; why = w.empty() ? "manual" : w; armedAt = now; hasQuietSince = false; }
  void cancel() { armed = false; why.clear(); armedAt = 0; hasQuietSince = false; }
  std::string step(bool blocked, double now) {
    if (!armed) return "off";
    if (blocked) { hasQuietSince = false; return "wait"; }
    if (!hasQuietSince) { hasQuietSince = true; quietSince = now; return settleMs <= 0 ? "go" : "settling"; }
    return now - quietSince >= settleMs ? "go" : "settling";
  }
  json view(const json& rows, double now) const {
    return { { "armed", armed }, { "why", armed ? json(why) : json() }, { "armedAt", armed ? numJ(armedAt) : json() }, { "quietSince", hasQuietSince ? numJ(quietSince) : json() },
      { "settleLeftMs", armed && hasQuietSince ? numJ(std::max(0.0, settleMs - (now - quietSince))) : json() }, { "blockers", rows } };
  }
};

struct TriggerRow { bool value = false; double at = 0; std::string by; };

struct Deck::Impl {
  DeckOptions opt;
  Seams sm;
  Sink sink;
  std::string home, stateDir, aliasFile;
  json defaults, settings;
  std::vector<Account> accounts;
  std::vector<std::unique_ptr<Tile>> tiles;
  std::vector<std::string> groups;
  std::unique_ptr<StateFile> tilesStore;
  // clients and windows
  std::vector<std::unique_ptr<Client>> clients; ClientId nextClient = 1;
  std::vector<std::unique_ptr<Entry>> entries; int nextWindowIndex = 0;
  json savedWindows = json::array(); size_t savedTaken = 0;
  // windows closed while others stayed open (main.js closedWindows): the quit keeps those closed in its last 2 minutes
  struct ClosedWin { int index = 0; json bounds; layout::LayoutMap layout; double at = 0; };
  std::vector<ClosedWin> closedWins;
  bool hadWindow = false;                  // a window of the deck's own has existed (addWindow / openWindow)
  bool hasDeskIndex = false; int deskIndex = 0;
  // timers
  struct Timer { int id; double due; double every; std::function<void()> fn; };
  std::vector<Timer> timers; int nextTimer = 1;
  // spawn queue
  std::deque<std::function<void()>> spawnQueue; bool spawnTimerOn = false;
  double restoringUntil = 0, restoreCapAt = 0; int restorePending = 0;
  // kills in flight
  struct Dying { std::unique_ptr<Pty> pty; uint64_t gen; bool done; std::function<void()> then; };
  std::vector<Dying> dying;
  uint64_t nextGen = 1;
  std::map<std::string, double> refusedNoteAt;
  bool started = false, down = false, restarting = false;
  // special prompts, triggers, restart when quiet, the update offer, hibernation
  std::unique_ptr<HeldPrompts> held;
  std::map<std::string, TriggerRow> triggers;
  QuietRestart quiet;
  RestoreRefresh restoreRefresh; int restoreRefreshTimer = 0;   // refresh every restored session once (restorerefresh.hpp)
  Pauses pauses; double deckStartedAt = 0; bool pauseUnsaved = false;                      // Pause / Resume / Pause until restart (pause.hpp)
  Auditors auditors; Deliveries deliveries;                      // auditor sessions and their children (auditors.hpp)
  json bootPrint; bool hasUpdate = false; json updateFiles; double updateSince = 0; std::string updateSig, dismissedUpdate;
  bool pressureOn = false; double pressureNotifiedAt = 0;
  std::set<std::string> settingsWarningsShown;
  int metaSoonTimer = 0;
  // usage
  std::unique_ptr<UsagePoller> usage;
  // the live usage bars may poll (core/deck/edition.hpp): always in the private edition; in the public one only when
  // settings.json `usageLive` is true (an opt-in: it reads the accounts' Claude Code sign-in)
  bool usageLive() const { return !edition::kPublic || js::get(settings, "usageLive") == json(true); }
  // the cloud session list and each cloud tile's status (lib/cloud.js: the accounts' Claude Code sign-in again):
  // always in the owner's deck; in the public one only when settings.json `cloudLive` is true (2026-11-24 ask r4
  // Q5: the same opt-in as the usage bars, off by default). Off = no token read, no request
  bool cloudLive() const { return !edition::kPublic || js::get(settings, "cloudLive") == json(true); }
  // the shell-host scan
  std::set<std::string> shellHostSeen; std::map<std::string, int> shellConsoleFails;
  // ask pages (lib/asks.js), the page viewer (lib/viewer.js), the on-call's desks (lib/deskopen.js), cloud sessions (lib/cloud.js)
  std::unique_ptr<AskFeed> asks; bool asksSeenInit = false; json asksSeen;
  Viewer viewer;
  struct HtmlHit { uint64_t size = 0; double mtime = 0, at = 0; json list; };
  std::map<std::string, HtmlHit> sessionHtmlCache; std::deque<std::string> sessionHtmlOrder;
  std::unique_ptr<DeskOpenInbox> deskInbox;
  std::unique_ptr<CloudPoller> cloud;
  bool watchdogArmed = false;
  // one deck at a time (onedeck.hpp): the marker's start time; written only by a deck that passed the guard
  bool oneDeckOn = false, oneDeckStopped = false; std::string oneDeckStartedAt;
  int liveSessionCount() const;
  void writeDeckMarker(bool running);
  // every pid this deck started (its tiles' processes, ever, and the Claude processes it learned they run): never "elsewhere"
  std::set<double> ownPids;
  // who else holds this session now (onedeck.hpp holderOf): the live records, ownPids, the other deck's tiles while it runs
  onedeck::Holder sessionHolder(const std::string& sessionId);
  void bringHere(const std::string& id, std::function<void(bool)> done);   // "Bring it here" on an "open elsewhere" tile
  // what this deck's own files hold now, as the shared layout's writer reads them ({ tilesJson, held, triggers, settings })
  json ownLayoutFiles();
  // the call table's user actions (a real change: onedeck.hpp); "" = not one
  static std::string realChangeOf(const std::string& fn);
  // the shared layout's writer (onedeck.hpp Sharer), for a deck that passed the guard; a real change marks the next write
  std::unique_ptr<onedeck::Sharer> sharer;
  void noteReal(const std::string& what);
  void publishShared();

  // ---------------------------------------------------------------- small things (deck.cpp)
  double now() const { return sm.clock->nowMs(); }
  // one line of the deck's log (seams.hpp Log): "<local time> [tag] text"; nothing without a log
  void logEvent(const std::string& tag, const std::string& text) { if (!sm.log) return; const double t = now(); sm.log->line(pauseLocalStamp(t, sm.clock->localOffsetMs(t)) + " [" + tag + "] " + text); }
  static std::string id8(const std::string& id) { return id.substr(0, 8); }
  Tile* tileById(const std::string& id) { for (auto& t : tiles) if (t->s.id == id) return t.get(); return nullptr; }
  Tile* tileByGen(uint64_t gen) { for (auto& t : tiles) if (t->pty && t->gen == gen) return t.get(); return nullptr; }
  Client* clientById(ClientId id) { for (auto& c : clients) if (c->id == id) return c.get(); return nullptr; }
  layout::Grid grid() const;
  json gridJson() const;
  std::string driver() const;
  bool send(ClientId c, const json& frame) { return sink ? sink(c, frame) : false; }
  int setTimer(double ms, std::function<void()> fn, double every = 0);
  void clearTimer(int id);
  void runTimers();

  // entries
  std::vector<Entry*> winEntries();        // real windows, in the order made
  std::vector<Entry*> allEntries();        // those, then the remote desks
  Entry* windowHolding(const std::string& id);
  bool winVisible(const Entry& w) const { return w.remote ? !w.hidden : (w.visible && !w.hidden); }
  void sendToEntry(Entry* e, const std::string& name, const json& payload);
  void deskEvent(const std::string& name, const json& payload);
  void toWindows(const json& frame);
  void toAll(const json& frame);
  json layoutJson(const layout::LayoutMap& l) const;
  json windowsState();

  // persistence
  std::string settingsFile() const { return joinPath(stateDir, "settings.json"); }
  bool restoring(double at) const { return (restorePending > 0 && at < restoreCapAt) || at < restoringUntil; }
  void saveState(const json& extra = json::object());
  void saveWindows() { saveState({ { "windows", windowsState() } }); }
  void keepWindowsAtQuit();                // the last window closing: every window closed in the 2 minutes before it
  void saveSettings();
  void loadSettings();
  void loadAccounts();
  void refreshAccounts();
  const Account* acctById(const std::optional<std::string>& id) const { return id ? accountById(accounts, *id) : nullptr; }

  // transcripts + live status
  std::string findTranscript(const std::string& configDir, const std::string& sessionId) const;
  void pollTail(Tile& t, double at);
  session::LiveMap readLive() const;
  bool isResumable(Tile& t, double at);

  // meta
  bool watching(const std::string& id) const;
  bool phoneWatching(const std::string& id) const;
  bool remoteShows(const std::string& id) const;          // lib/phone.js remoteShows: a desk's visible tab shows it in a pane
  // the registry as remoterule.hpp reads it (`own`: the deck's own windows only)
  std::vector<remoterule::Entry> ruleEntries(const std::vector<Entry*>& list) const;
  DeskRule deskRule() const { DeskRule r; r.ownWindowsOnly = !opt.remoteDeskIsAWindow; r.claim = opt.driverClaim; return r; }
  json deskMetaFor(const json& remoteMetaOfDesk, const Client& c);
  std::vector<std::string> shownBy(const Tile& t);
  json collectMeta();
  bool followTiles(const session::LiveMap& live);   // /clear or /resume inside a tile: it follows its process (lib/tilefollow.js)
  void pushMeta();
  void pushMetaSoon();
  void sendRemoteMeta(const json& meta);
  json extras();
  json publicPlatform() const;
  json phoneStatus();
  void maybeNotify(Tile& t, const std::string& status, double turnMs);
  bool anyWindowFocused();

  // spawning + ending
  void queueSpawn(std::function<void()> fn);
  void drainSpawns();
  std::string spawnTile(const SpawnSpec& spec, Tile* existing);
  void killPty(Tile& t, std::function<void()> then, double graceMs = 2500);
  void finishKill(uint64_t gen);
  void reapDying();
  void watchStartup(Tile& t, uint64_t gen, const std::string& d);
  void trustCycle(const std::string& id, uint64_t gen);

  // tile actions
  void closeTile(const std::string& id, std::function<void()> done);
  SpawnSpec respawnSpec(const Tile& t, const json& opts = json()) const;
  void refreshTile(const std::string& id, const json& opts, std::function<void()> done);
  void restartTile(const std::string& id);
  void sleepTile(const std::string& id, const std::string& why, std::function<void(bool)> done);
  bool wakeTile(const std::string& id, const std::string& why);
  bool setPinned(const std::string& id, bool pinned);
  void reorderPinned(const json& ids);
  void renameTile(const std::string& id, const json& title);
  void setTileGroup(const std::string& id, const json& group);
  json setGroups(const json& names);
  void resetTile(const std::string& id) { toAll({ { "t", "reset" }, { "id", id } }); }

  // input / size
  bool remoteDrives(const std::string& id) const;        // this tree's rule (the claim off)
  // the claim (driverrule.hpp; main.js whoLooks / sizeDriver on the review branch)
  driverrule::Who whoLooks(const std::string& id) const;
  std::string sizeDriver(const std::string& id) const;
  bool officeLocked(const std::string& id) const;          // R10 under either rule
  std::string refusedReason(const std::string& id) const;
  bool sizeAccepted(const std::string& id, const char* from) const;
  const char* kindOf(const Client& c) const { return !c.remote ? "office" : c.desk ? "remote" : "phone"; }
  void noteKeys(const std::string& id, const std::string& data, const char* kind = "office");
  void noteDriver(const std::string& id, const char* from, const char* kind);
  // the deck's contract with the sessions it hosts: deck-state.json (core/deck/presence.hpp)
  std::unique_ptr<Presence> presence; bool presenceEverWritten = false; std::string presenceWritten;
  std::string presenceFile() const { return joinPath(stateDir, "deck-state.json"); }
  std::string savePastedImage(const std::string& id, const json& b64, const json& mime, std::string& why);
  void startPresence();
  void writePresence();
  void sawInput(const char* kind) { if (presence && presence->input(kind, now())) writePresence(); }
  void presenceTick();
  json remoteDesk() const;
  void writeTile(const std::string& id, const std::string& data);
  void windowInput(Client& c, const std::string& id, const std::string& data);
  // `from`: the kind of screen that asked — 'office' | 'phone' | 'remote'. Only the driver's size reaches the pty.
  void resizeTile(const std::string& id, double cols, double rows, const char* from);
  void nudgeTile(const std::string& id, double cols, double rows, const char* from);

  // windows + desks
  layout::LayoutMap cleanLayout(const json& in) const;
  void takeLayout(Entry* entry, const json& in);
  void takeLayoutMap(Entry* entry, layout::LayoutMap l);
  void entryShown(Entry* e, const std::vector<std::string>& ids, bool hidden);
  json deskOpen(Client& c, const json& opts);
  // the desk slot: a deskInit waiting on a probe of the desk that holds it (lib/phone.js deskOpen + probe)
  struct DeskProbe { ClientId caller = 0, other = 0; json rid, opts; double before = 0, t0 = 0; int timer = 0; };
  std::vector<DeskProbe> deskProbes;
  bool deskSlot(Client& c, const json& rid, const json& opts);   // true = answered (or will be); the caller returns
  void deskProbeStep(ClientId caller);
  void deskGone(Client& c);
  void removeEntry(Entry* e);

  // settings
  json applySettingsCall(const json& s);
  json getSettings(bool desktopShape);
  json saveAccountRows(const json& rows);

  // boot
  void restore();

  // the call table
  void reply(const Client& c, const json& rid, const json& result);
  void fail(const Client& c, const json& rid, const std::string& error);
  void call(Client& c, const json& rid, const std::string& fn, const json& args);
  void handle(ClientId id, const json& m);
  void shutdown();

  // ---------------------------------------------------------------- deck_more.cpp
  void initMore();                          // triggers, held prompts, the periodic ticks
  // triggers
  void loadTriggers(); void saveTriggers(); void triggersChanged();
  json triggerPub(const std::string& name) const;
  json triggersList() const;
  json triggerSet(const std::string& name, bool value, const std::string& by);
  json triggerDeclare(const std::string& name, const std::string& by);
  bool triggerRemove(const std::string& name);
  double triggerRisenAt(const std::string& name) const;
  void drainTriggerInbox();
  // hibernation
  struct HView { std::string id, kind, status; bool hasSession, running, resumable, asleep, pinned; std::vector<std::string> shownBy; int held, background; double lastActivityAt; };
  HView hibernateView(Tile& t, double at);
  void hibernateTick();
  // restart when quiet
  std::vector<session::QuietView> quietViews(double at);
  bool setQuiet(bool on, const std::string& why);
  void quietTick();
  bool restartDeck(const std::string& why);
  void checkUpdate();
  void dismissUpdate();
  json restartView(double at);
  // sessions elsewhere / past, adopt, moves
  json externalSessions();
  json pastSessions(size_t limit = 80);
  void adoptSession(const json& s, std::function<void(const json&)> ok, std::function<void(const std::string&)> err);
  void moveTile(const std::string& id, const std::string& target, const json& opts, std::function<void()> ok, std::function<void(const std::string&)> err);
  void moveAll(const std::string& from, const std::string& to, bool continueIfUsage, const json& onlyIds, std::function<void(const json&)> ok, std::function<void(const std::string&)> err);
  void sayContinueWhenReady(const std::string& tileId, const std::string& sessionId, int i);
  // refresh every session once the restore is done (main.js startRestoreRefresh / restoreRefreshTick)
  bool readyAfterStart(const Tile& t, const session::LiveMap& live) const;
  void startRestoreRefresh(const std::vector<std::string>& ids);
  void restoreRefreshTick();
  // Pause / Resume / Pause until restart (main.js pauseTile / resumeTile / pauseAll / resumeAll / pauseTick)
  std::string pauseFileOf(const std::string& sid) const { return joinPath(joinPath(stateDir, "pause"), sid + ".md"); }
  bool pausable(const Tile& t) const { return t.s.kind == "claude" && t.s.sessionId && !t.s.sessionId->empty() && t.pty && !t.s.asleep && !t.s.exited; }
  void typePrompt(const std::string& id, const std::string& text, bool esc);
  bool pauseTile(const std::string& id, bool untilRestart, std::string& why);
  bool resumeTile(const std::string& id, std::string& why);
  int pauseAll(bool untilRestart);
  int resumeAll();
  void pauseTick();
  // auditors and their children (main.js makeAuditor / setAuditorChild / auditorTick; auditors.hpp)
  std::string auditorInbox(const std::string& id) const { return joinPath(joinPath(stateDir, "auditors"), id); }
  json makeAuditor(const std::string& id, const json& string, std::string& why);
  bool unmakeAuditor(const std::string& id, std::string& why);
  json setAuditorChild(const std::string& id, const std::string& parent, std::string& why);
  bool unchildTile(const std::string& id, std::string& why, const std::string& leftWhy = "un-childed");
  void tellChildLeft(const std::string& parent, const std::string& id, int n, const std::string& title, const std::string& leftWhy);
  void logAuditorFile(const std::string& from, int n, const std::string& name, const std::string& outcome);
  void endAuditorTies(const std::string& id);
  bool auditorSend(const std::string& itemId, std::string& why);
  bool auditorDrop(const std::string& itemId, std::string& why);
  std::string moveAuditorFile(const std::string& from, const std::string& name, const std::string& sub, const std::string& note);
  void refuseAuditorFile(const std::string& from, const std::string& name, const std::string& why = std::string());
  void drainAuditorInboxes(double at);
  void auditorTick();
  // the shell-host scan
  void scanShellHosted();

  // ---------------------------------------------------------------- deck_parity.cpp
  void initParity();                        // the ask feed and the cloud poller (before the first meta)
  void startDeskInbox();                    // after the restore: a desk is placed in a window
  void armExitWatchdog();
  Entry* primaryEntry();
  std::string tileTitle(const Tile& t) const;          // t.userTitle || t.knownTitle || t.tail.title, "" = none
  // ask pages
  json doneAsk(const std::string& sessionId, const json& pages);
  void notifyNewAsks();
  void openAskLink(const std::string& url);
  // the page viewer
  json pagesShown();
  json viewHtml(const json& p);
  json sessionHtml(const std::string& tileId);
  // the on-call's desks
  json deskWindows();
  json placeDeskTile(Tile& t, Entry** entryOut);
  void notifyDeskOpened(Tile& t, Entry* entry, const json& req, const json& place, const std::string& how);
  json openDeskRequest(const json& req);
  // cloud sessions
  SpawnSpec cloudSpec(const json& s);
  void cloudCreated(Tile& t);
  void cloudTick();
  void syncCloudAccounts();
  std::string openCloud(const json& s);
  json cloudMeta(const Tile& t, const json& c) const;
};

}}  // namespace deck::core
