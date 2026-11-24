// core/deck/onedeck.hpp — one deck at a time + a shared layout file (2026-11-24): lib/onedeck.js's twin, for
// the native decks (Windows; the Mac links the same core). The JavaScript is the specification: the file names, the
// marker and the shared file's shapes, the guard's rule, the decision at start and the writer's rules are its, and
// core/test/deck/gen-onedeck.js replays the JS's own answers here (core/test/fixtures/deck/onedeck.json).
//
//   %APPDATA%\claude-deck-shared\active.json         THE MARKER (~/.local/bin/active-deck.ps1 reads it)
//   %APPDATA%\claude-deck-shared\shared-layout.json  THE SHARED LAYOUT (deck-neutral: tiles, windows' layouts, groups,
//                                                    held prompts, triggers; no bounds, no token, no per-PC setting)
//   <state>\onedeck.json                             what this deck last saw, and its own real changes
//   <state>\onedeck-backups\<time>-<why>\            the copies made before an import, or of the side that lost
// A deck on a scratch state folder (--state-dir: every test) keeps all of it under <state>\onedeck-shared unless
// CLAUDE_DECK_SHARED_DIR names a folder, and never looks at the real decks' processes or files.
//
// PURE, as the rest of core/deck: reads and writes only through Files; the process facts come in as plain values.
#pragma once
#include "seams.hpp"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace deck { namespace core { namespace onedeck {

using json = nlohmann::json;

constexpr int V = 1;
extern const char* const MARKER;          // "active.json"
extern const char* const SHARED;          // "shared-layout.json"
extern const char* const SYNC;            // "onedeck.json"
extern const char* const BACKUPS;         // "onedeck-backups"
constexpr double HEARTBEAT_MS = 30000;
constexpr double FRESH_MS = 120000;       // active-deck.ps1: a heartbeat under 2 min is a running deck
std::string otherOf(const std::string& deck);

// ISO text (toISOString, or with a +hh:mm offset) -> ms since the epoch; NaN when it is not one
double parseIso(const std::string& s);
json readJson(Files& files, const std::string& path);     // an object, or null (missing, unreadable, a BOM tolerated)

// ---------------------------------------------------------------- the marker
struct Marker { std::string deck; bool running = false; double pid = 0; std::string exe; std::vector<std::string> args; std::string stateDir, startedAt, heartbeatAt; int liveSessions = 0; };
json markerJson(const Marker& m);                          // { v, deck, running, pid, exe, args, stateDir, startedAt, heartbeatAt, liveSessions }
json parseMarker(const json& m);                           // the object when it is a v1 marker of a known deck with a stateDir, else null
bool sameExe(const std::string& a, const std::string& b);   // the same file: case and separators aside
bool heartbeatFresh(const json& m, double now);
// running:true, the pid a process of its exe (`exeOf(pid)` -> that pid's file, "" = none), heartbeat under 2 min
bool markerRunning(const json& m, const std::function<std::string(double)>& exeOf, double now);
bool mayWriteMarker(const json& m, const std::string& deck, double pid, const std::function<std::string(double)>& exeOf, double now);

// ---------------------------------------------------------------- the guard
using Proc = ProcInfo;
struct Busy { bool busy = false; int n = 0; bool known = true; std::string source, other, why; };
// lib/onedeck.js otherBusy. otherTileIds: false = the other deck's tiles.json does not read.
Busy otherBusy(const std::string& me, const json& marker, const std::function<std::string(double)>& exeOf, double now,
               const std::vector<uint32_t>& otherPids, const std::function<bool(std::vector<std::string>&)>& otherTileIds,
               const std::set<std::string>& liveIds);
std::string blockedText(int n);
std::vector<uint32_t> findDeckPids(const std::vector<Proc>& procs, const std::string& deck, const std::string& exe, uint32_t self);
// the session ids of <stateDir>/tiles.json; false = it does not read (no file = true, none)
bool tileSessionIds(Files& files, const std::string& stateDir, std::vector<std::string>& out);

// THE GUARD AT START, as a shell asks it before it starts its deck: the marker in od.sharedDir, a process snapshot, and
// - when the marker does not say the other deck runs - a direct look for the other deck's main process (the deck on its
// own folder: the real other deck; a scratch deck: only od.otherExe), counting od.otherState's tiles.json sessions that a
// live Claude process holds (the accounts of `aliasFile`). od = DeckOptions::oneDeck (+ otherState: the other deck's
// state folder, which the shell names).
Busy guardAtStart(Files& files, ProcessList& procs, Env& env, double now, const json& od, const std::string& aliasFile, const std::string& home);
// the session ids a live Claude process holds: every account's sessions/<pid>.json whose pid runs
std::set<std::string> liveSessionIds(Files& files, ProcessList& procs, const std::string& aliasFile, const std::string& home, bool win);

// ---------------------------------------------------------------- the same-session guard
struct Record { std::string sessionId; bool alive = false; double pid = 0; };
struct Holder { bool held = false; std::string where; double pid = 0; };   // where: "deck" | "terminal"
Holder holderOf(const std::string& sessionId, const std::vector<Record>& records, const std::set<double>& ownPids, const json& otherDeckTiles);

// ---------------------------------------------------------------- translators
// settings: the deck's behaviour travels; LOCAL_SETTINGS (the phone and its token, pages, fonts, the default folder) stay
extern const std::vector<std::string> LOCAL_SETTINGS;
json sharedSettings(const json& settings);
json mergeSettings(const json& own, const json& shared);
json toShared(const json& tilesJson, const json& held, const json& triggers, const json& settings, const std::string& deck, double atMs, int realChange, const json& lastChange);
struct Own { json tilesJson, held, triggers, settings; };
Own fromShared(const json& shared, const json& own, const json& ownSettings = json());
json parseShared(const json& s);
std::string contentKey(const json& s);

// ---------------------------------------------------------------- the sync record and the decision
json emptySync();
json readSync(Files& files, const std::string& stateDir);
bool writeSync(Files& files, const std::string& stateDir, const json& sync);
json seenOf(const json& shared, int localN);
bool sharedChanged(const json& shared, const json& seen);
// { action: none | write | import | ask, why, mine, theirs } (lib/onedeck.js decide). savedAtMs 0 = not known.
json decide(const json& shared, const json& sync, bool ownHasState, double savedAtMs);

// AT START, before anything shared is written: { shared (parsed, or null), decision (decide's answer) } for this deck's
// state folder and the shared folder
json preflight(Files& files, const std::string& stateDir, const std::string& sharedDir, double nowMs);
// "keep this deck's own layout" (the question's answer, or nothing to import): the shared file is backed up into this
// deck's backups and marked as seen, so this deck's next write replaces it (with its realChange one higher)
std::string keepOwn(Files& files, const std::string& stateDir, const std::string& sharedDir, const json& shared, double atMs);

// ---------------------------------------------------------------- backups, the import, the writer
extern const std::vector<std::string> OWN_FILES;           // tiles.json, settings.json, held-prompts.json, triggers.json, onedeck.json
std::string backup(Files& files, const std::string& stateDir, const std::vector<std::string>& names, const std::string& why, double atMs, const std::string& sharedFile);
// back up, then this deck's tiles.json / held-prompts.json / triggers.json from the shared file, then the sync record
std::string importShared(Files& files, const std::string& stateDir, const json& shared, double atMs, const std::string& why);

class Sharer {
 public:
  // read() -> { tilesJson, held, triggers } of this deck now
  Sharer(Files& files, std::string deck, std::string stateDir, std::string dir, std::function<json()> read, std::function<double()> now, std::function<void(const std::string&)> log);
  void noteReal(const std::string& what);
  bool publish(bool force = false);
  bool behind() const { return behind_; }
  const json& sync() const { return sync_; }
  bool hasPending() const { return !pending_.is_null(); }
 private:
  Files& files_; std::string deck_, stateDir_, dir_, file_;
  std::function<json()> read_; std::function<double()> now_; std::function<void(const std::string&)> log_;
  json sync_, pending_; std::string lastKey_; bool behind_ = false;
};

}}}  // namespace deck::core::onedeck
