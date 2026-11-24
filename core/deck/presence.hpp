// core/deck/presence.hpp — where the user is, as the sessions the deck hosts can read it: lib/presence.js of the
// deck's review branch ("The deck's contract with the sessions it hosts", core/PROTOCOL.md there), ported. PURE:
// every call is handed `now`; the deck writes the file (deck-state.json) through the Files seam, atomically and
// only when its text changes. Checked against core/test/fixtures/deck/presence.json, which
// core/test/deck/gen-presence.js generates from whichever lib/presence.js it is pointed at.
//
// The file, keys in this order, two-space JSON + a newline:
//   { "v": 1, "deck": { "pid", "startedAt", "version" }, "where": "office" | "remote" | "phone" | "away",
//     "since": <ISO>, "remote": { "kind": "browser" | "native", "name": <string | null> } | null,
//     "phone": { "attached": <bool> }, "lastInputFrom": "office" | "remote" | "phone" | null, "deliver": <one line> }
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <string>
#include <utility>
#include <vector>

namespace deck { namespace core {

using json = nlohmann::json;

constexpr int kDefaultAwayMinutes = 30;
int awayMinutes(const json& v);                                  // the setting, cleaned: whole minutes, 0 = never away, at most 1440
std::string deskKind(const json& v);                             // 'native', or 'browser' for anything else (also nothing)
json presenceName(const json& v);                                // a desk's name: control characters out, trimmed, 40 at most; null when empty
// What a tile's process is told at spawn: CLAUDE_DECK_TILE, CLAUDE_DECK_STATE
std::vector<std::pair<std::string, std::string>> tileEnv(const std::string& tileId, const std::string& stateFile);

class Presence {
 public:
  // deck: { pid, startedAt (ms or ISO), version }; prev: the parsed state file of the previous run, or null
  // pcName: this PC's name (os.hostname()); a remote desk of that name is the user AT this PC (lib/presence.js)
  Presence(const json& deck, const json& awayMinutesSetting, double now, const json& prev = json(), const std::string& pcName = std::string());
  bool input(const std::string& kind, double now);               // real input from 'office' | 'remote' | 'phone'; true = the state changed
  bool connections(const json& remote, bool phone, double now);  // remote: { kind, name } or null
  bool tick(double now);                                         // time alone: the one step it can take is into `away`
  bool setAwayMinutes(const json& v, double now);
  nlohmann::ordered_json state() const;                          // keys in the file's order
  std::string text() const;                                      // the file's text
 private:
  bool place(double now);
  nlohmann::ordered_json deck_;
  int awayMin_;
  std::string lastKind_;            // "" = no input yet
  double lastAt_;
  bool hasRemote_ = false; std::string remoteKind_; json remoteName_;
  bool phone_ = false;
  std::string pcName_;
  std::string where_ = "office";
  double since_;
};

}}  // namespace deck::core
