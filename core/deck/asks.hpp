// core/deck/asks.hpp — the ask pane's data: lib/asks.js, ported. Claude sessions ask their questions on ask pages
// (the ask-page skill): one HTML page per round and one state file per session, <state_dir>/<session id>.json,
// written by the skill. The deck READS those files and never writes them. What the deck keeps of its own: the
// drafts (what was picked and typed on a page before Done), the sent marks, the log of answered pages and the
// "moved" notes, all in <userData>/ask-drafts.json; and deck.json in the skill's state folder (the deck's ask
// settings plus a heartbeat), which the skill reads.
//
// PURE: files through Files, time through Clock, no timers of its own. The DECK calls scan() every kAskScanMs
// and writeDeckFile() every kAskBeatSeconds (AskFeed.start() in the JS), flushDrafts() + removeDeckFile() when it
// stops (AskFeed.stop() has nothing left to do here: no watcher, no intervals).
// Checked against core/test/fixtures/deck/asks.json, which core/test/deck/gen-asks.js generates from lib/asks.js.
#pragma once
#include "seams.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace deck { namespace core {

constexpr int kAskBeatSeconds = 30;      // BEAT_S: deck.json's heartbeat; the skill treats 3 missed beats as "not running"
constexpr double kAskForeignFreshMs = 90000;   // 3 heartbeats: another deck's deck.json this young means that deck is serving the pages
constexpr double kAskScanMs = 4000;      // start(every = 4000): the poll of the state folder

struct AskLocations { std::string stateDir; std::vector<std::string> downloads; };
// lib/asks.js locations() + skillSettings(): the skill's settings.json under <home>/.claude, .claude-thirdacct,
// .claude-demo, .claude-vv (the first that parses), ASK_PAGE_STATE_DIR / ASK_PAGE_DOWNLOADS from the environment
// (split on path.delimiter: ';' on win32, ':' elsewhere), `~` expanded to home, path.resolve through
// env.resolvePath (and then normalised here the way path.resolve leaves an absolute path).
AskLocations askLocations(Files& files, Env& env);
bool askIdOk(const std::string& id);                                   // ID_RE.test(id)
std::string composeDone(const json& pages);                            // composeDone: [{ id, lines }]
std::string composePointer(const json& pages);                         // composePointer: [{ id, file, answered, total, later }]
bool pickerOnScreen(const std::string& recent, const std::vector<std::string>& questions);
// newAskRows(seen, view): `seen` null on the first call (the JS's Set, here an array of ids).
// -> { "seen": [ids, in order], "fresh": [{ "session": s, "rounds": [rows] }] }
json newAskRows(const json& seenOrNull, const json& view);
// url.pathToFileURL(p).href as Node prints it (matched against Node 24); "" where Node throws (a UNC path with
// no resource, an invalid host). `win`: path.win32 rules, else posix. See asks.cpp for what is not covered.
std::string pathToFileUrl(const std::string& path, bool win);

struct AskDeps {
  Files* files = nullptr; Clock* clock = nullptr;
  std::string stateDir; std::vector<std::string> downloads; std::string draftsFile;
  uint32_t pid = 0;                                                    // process.pid, for deck.json
  bool win = true;
  std::string home;                                                    // os.homedir(): a round's page path that starts with ~ (answersDir). Unset: left as it is
  std::function<json()> settings;                                      // { deckThreshold, picker, usePages, background }
  std::function<void()> onChange;
  std::function<void(const std::string& pageId, const json& sent)> onRead;
  std::function<bool(std::set<std::string>& out)> liveSessions;        // unset = deps.liveSessions absent; returns false = it threw (live = null)
  // setTimeout, for saveDrafts' 300 ms. Unset: ask-drafts.json is written when the call that changed it returns
  // (scan / setDraft / clearDraft / markSent / logDone), with the state as that call left it.
  std::function<void(double ms, std::function<void()>)> later;
};
struct AskRound { bool found = false; std::string sid; json state; json round; json items; int answered = 0, later = 0; bool waiting = false; };

class AskFeed {
 public:
  explicit AskFeed(AskDeps deps);          // loadDrafts() as the JS constructor does
  ~AskFeed();
  AskFeed(const AskFeed&) = delete;
  AskFeed& operator=(const AskFeed&) = delete;
  void scan();                             // scan() incl. prune / logRead / migrate / the signature + onChange
  json view();                             // meta.asks
  json logView();                          // meta.askLog
  json draft(const std::string& pageId);   // the state, or null
  bool setDraft(const std::string& pageId, const json& payload);
  void clearDraft(const std::string& pageId);
  json logState(const std::string& pageId);
  AskRound round(const std::string& pageId);
  std::string pagePath(const std::string& pageId);                     // "" = null
  std::string answersDir(const std::string& pageId);                   // "" = null
  std::string writeAnswersFile(const std::string& pageId, const json& answers);   // throws std::runtime_error, the JS's words
  void markSent(const std::vector<std::string>& ids, const json& info);
  void logDone(const std::string& pageId, const json& answers, const std::string& how, const json& title);
  // THE GUARD (not in lib/asks.js, which just writes): before every write the file is read, and when it carries a
  // heartbeat of ANOTHER deck (a pid that is not ours, `at` less than 90 s old) this deck is a GUEST — nothing is
  // written, null is returned, and scan() stops pruning / migrating / logging (it writes nothing into the shared
  // folders). It becomes the writer again at the first call that finds the other heartbeat 90 s stale or gone.
  json writeDeckFile();                    // returns the body, or null when this deck is a guest
  bool guest() const;                      // as the last writeDeckFile() found it (false before the first)
  void removeDeckFile();                   // only when its pid is ours
  void flushDrafts();                      // write ask-drafts.json now if a save is pending (the deck calls it at shutdown)
  const std::vector<std::string>& downloads() const;
  const std::string& stateDir() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}}  // namespace deck::core
