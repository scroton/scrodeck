// core/session/livestate.hpp — lib/sessions.js readLive, ported: Claude Code's own live status, read from
// <configdir>/sessions/<pid>.json  ->  { pid, sessionId, status: busy|idle|waiting, statusUpdatedAt, name, … }.
// PURE: the caller lists each account's sessions folder, reads the files and says whether a pid is running;
// this decides what a record means and which record wins for a session.
//
// The rules (all of lib/sessions.js):
//   - a file that is not JSON, or has no sessionId or no pid, is not a record;
//   - a record is LIVE only while its pid is running (the files outlive their process);
//   - one session can have several records (a resume under a new pid, a move to another account): a live one
//     beats a dead one, and between two of the same liveness the later statusUpdatedAt wins — a tie keeps
//     the one seen first (accounts in their order, files in the folder's listing order).
// Checked against core/test/fixtures/session/livestate.json.
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace deck { namespace session {

struct LiveRecord {
  std::string account;                    // the account id whose folder held the file
  nlohmann::json status;                  // j.status || 'idle'
  double statusUpdatedAt = 0;             // j.statusUpdatedAt || j.updatedAt || 0
  bool alive = false;
  nlohmann::json pid;                     // as written (a number)
  nlohmann::json name, bridgeSessionId, waitingFor, kind, cwd, procStart;   // the value, or null
  double startedAt = 0;                   // j.startedAt || 0

  std::string statusText() const { return status.is_string() ? status.get<std::string>() : std::string(); }
  nlohmann::json toJson() const;          // the object readLive stores, field for field
};

class LiveMap {
 public:
  using AliveFn = std::function<bool(double pid)>;
  // One file of one account's sessions folder (the caller has already kept only names ending ".json").
  // Returns true when the text was a record, whether or not it replaced what the map held.
  bool add(const std::string& accountId, const std::string& fileText, const AliveFn& pidAlive);
  const LiveRecord* get(const std::string& sessionId) const;
  // session ids in first-seen order, as the JS Map iterates
  const std::vector<std::pair<std::string, LiveRecord>>& entries() const { return items_; }
  // every record added, before the per-id choice (lib/sessions.js readLive's `all`): an id two processes hold keeps
  // one of them in entries(); a tile's own process is looked up by pid here (lib/tilefollow.js, D::followTiles)
  const std::vector<std::pair<std::string, LiveRecord>>& all() const { return all_; }
  void clear() { items_.clear(); all_.clear(); }
 private:
  std::vector<std::pair<std::string, LiveRecord>> items_;
  std::vector<std::pair<std::string, LiveRecord>> all_;
};

}}  // namespace deck::session
