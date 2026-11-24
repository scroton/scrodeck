// core/deck/cloud.hpp — lib/cloud.js, ported: cloud sessions (Claude Code on the web). The ids' two spellings,
// what `claude --cloud "<task>"` printed, one API row as the deck's view of it, and CloudPoller: each cloud tile's
// own session every 15 s and each account's list of live ones every minute, one request after another, with the
// back-off after a 429. PURE over three seams: Clock, Http (one GET at a time) and Credentials (the token is read
// per request, never written, never kept). Tested with fakes only; no shell wires a real Http or Credentials to it.
//
// Not here, the shell's Http implementation's job: the JS aborts a request after 15 s (AbortController); Http::get
// has no timeout of its own, so a shell's GET must end by itself (an error) or the pass never finishes.
#pragma once
#include "seams.hpp"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace deck { namespace core {

// ---- ids (one session, two spellings: the web app and the CLI print session_<x>, the API answers cse_<x>)
std::string cloudKey(const std::string& id);            // cloudKey
std::string cloudWebId(const std::string& id);          // webId; "" for an empty id (JS null)
std::string cloudWebUrl(const std::string& id);         // webUrl (an empty id gives ".../code/null", as the JS does)
bool isCloudId(const std::string& id);                  // isCloudId
// ---- pure readers
json cloudParseCreated(const std::string& text);        // parseCreated: null | { id, title }   (title null when none)
json cloudView(const json& row);                        // view: null | the view object
bool cloudListable(const json& view);                   // listable
// main.js cloudSpec's text rules, so the deck words them identically: the cleaned task (\r\n? -> \n, trimmed) ...
std::string cloudCleanTask(const json& task);
// ... and what cloudSpec throws for it: "" when the cleaned task may be sent, else the JS's exact message
// (empty, or longer than CLOUD_TASK_MAX UTF-16 units).
std::string cloudTaskError(const std::string& cleanedTask);

constexpr double kCloudTileMs = 15000, kCloudListMs = 60000, kCloudBackoffMs = 600000, kCloudTickMs = 5000;
constexpr int kCloudTaskMax = 24000;                    // main.js CLOUD_TASK_MAX
extern const char* const kCloudApi;                     // https://api.anthropic.com

struct CloudAccount { std::string id, dir; };
struct CloudTile { std::string cloudId, account; };

class CloudPoller {
 public:
  CloudPoller(Clock& clock, Http* http, Credentials* credentials);
  ~CloudPoller();
  CloudPoller(const CloudPoller&) = delete;
  CloudPoller& operator=(const CloudPoller&) = delete;
  void setAccounts(std::vector<CloudAccount> accounts);
  // One pass (lib/cloud.js poll): every tile's own session that is due, then every account's list that is due,
  // one request after another; `done(changed)` when the pass ends. A pass already running: done(false) at once.
  void poll(const std::vector<CloudTile>& tiles, std::function<void(bool changed)> done);
  json session(const std::string& id) const;            // null | view + { account, fetchedAt, error }
  void soon(const std::string& id);                     // ask for it at the next pass, whatever its age
  json elsewhere(const std::set<std::string>& hostedKeys) const;   // [{ ...view, account }]
  bool busy() const { return busy_; }
  json lists() const;                                   // this.lists: account id -> [view] (tests, diagnostics)
 private:
  struct Pass;
  using Got = std::function<void(bool ok, const json& body, const std::string& error)>;
  void next(std::shared_ptr<Pass> pass);
  void get(const CloudAccount& a, const std::string& url, Got got);
  std::string token(const CloudAccount& a);             // "" when there is none (JS null)
  void store(const json& view, const std::string& account, double now);
  Clock& clock_; Http* http_; Credentials* creds_;
  std::vector<CloudAccount> accounts_;
  std::map<std::string, json> byKey_;                   // cloudKey -> view + { account, fetchedAt, error }
  std::map<std::string, json> lists_;                   // account id -> [view] (listable only)
  std::map<std::string, double> listAt_, backoffUntil_;
  bool busy_ = false;
  std::shared_ptr<bool> alive_;                         // false once the poller is gone: a late answer is dropped
};

}}  // namespace deck::core
