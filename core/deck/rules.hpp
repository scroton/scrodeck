// core/deck/rules.hpp — the parts of main.js / lib/accounts.js / lib/platform.js the deck needs
// that are rules rather than plumbing, as PURE functions (no OS calls, so they are unit-tested directly):
//   - the spawn environment (baseEnv / envFor) and what a `newTile` spec means (newTileSpec / browserSpec);
//   - THE SEAM: claudeLaunch() builds exactly the { exe, args, env, cwd } spawnTile's Claude branch hands to
//     node-pty. LocalDeck calls it for every Claude start and then refuses to launch unless allowClaude is on;
//   - account discovery from the alias file's text (lib/accounts.js loadAccounts);
//   - the settings: DEFAULT_SETTINGS, the merge loadSettings does over a saved file, applySettings' validation.
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace deck { namespace core {

using json = nlohmann::json;
using EnvList = std::vector<std::pair<std::string, std::string>>;   // NAME, value — in order, names exact-case as a JS object's keys

// ---- environment
// main.js baseEnv(): the parent's environment without any Claude session's markers (CLAUDECODE…, CLAUDE_…,
// AI_AGENT) and without NO_COLOR, plus CLAUDE_CODE_FORCE_SESSION_PERSISTENCE=1, TERM_PROGRAM=claude-deck,
// COLORTERM=truecolor.
EnvList baseEnv(const EnvList& parent);

// ---- accounts
struct Account {
  std::string id;                         // the shortest alias (or "claude" for the default)
  std::vector<std::string> aliases;
  std::string dir;                        // its CLAUDE_CONFIG_DIR
  bool isDefault = false;
  // filled by refreshAccounts (file reads; LocalDeck does them)
  json email;                             // null until logged in
  double contextWindow = 200000;
  std::string key, label;
  json abbr;
};
// lib/accounts.js loadAccounts over the alias file's text (missing file = ""), then main.js's ruling of
// 2026-09-02: the default ~/.claude account is offered only when it is the only one. `win`: the platform's
// path.normalize / path.join is path.win32's (else path.posix's: a Mac's account dirs keep their '/').
std::vector<Account> parseAccounts(const std::string& aliasFileText, const std::string& home, bool win = true);
const Account* accountById(const std::vector<Account>& accounts, const std::string& idOrAlias);
json publicAccounts(const std::vector<Account>& accounts);   // [{ id, label, abbr, email, key, isDefault }]
json accountRows(const std::vector<Account>& accounts);      // [{ id, keywords, dir, email, label, isDefault }]
// main.js envFor(acct): baseEnv, with CLAUDE_CONFIG_DIR set to the account's dir (removed for the default)
EnvList envFor(const EnvList& parent, const Account& acct);
// path.normalize for a Windows path (separators, '.', '..')
std::string normalizeWinPath(const std::string& p);
// path.normalize on the platform: path.win32's (normalizeWinPath) or path.posix's
std::string normalizePath(const std::string& p, bool win);

// ---- what a view asked for (main.js newTileSpec + browserSpec)
struct TileSpec {
  std::string kind;                       // 'claude' | 'shell'
  json account, sessionId, cwd;           // as given (null when not)
  bool hasBrowser = false;                // the caller named the browser choice…
  json browser;                           // …as true, false or null
  json toJson() const;
};
TileSpec newTileSpec(const json& spec);   // throws std::runtime_error for kind 'cloud' (not in the native local deck)

// ---- the Claude spawn (main.js spawnTile, the plain `claude` branch)
struct ClaudeLaunchInput {
  EnvList parentEnv;                      // process.env
  std::string claudeExe;                  // platform.claudeExe()
  Account account;
  std::string sessionId;                  // the tile's, or "" for a brand-new session
  bool transcriptExists = false;          // findTranscript(account.dir, sessionId): only then can it --resume
  std::optional<bool> browser;            // true --chrome, false --no-chrome, unset = the account's default
  std::optional<std::string> desk;        // a desk the on-call opened: CLAUDE_ONCALL_KIND=<desk>-desk
  std::string model, prompt;              // this start only (a desk's first message)
  std::string cwd;
  std::string freshUuid;                  // crypto.randomUUID(), used when sessionId is ""
};
struct ClaudeLaunch {
  std::string file;
  std::vector<std::string> args;
  EnvList env;
  std::string cwd;
  std::string sessionId;                  // what the tile's sessionId is after the spawn
};
ClaudeLaunch claudeLaunch(const ClaudeLaunchInput& in);

// ---- settings
json defaultSettings(const std::string& home, const std::string& cwdOverride = "");   // main.js DEFAULT_SETTINGS
// loadSettings' merge: `saved` is the parsed settings.json (or an empty object). The phone token is NOT
// generated here (the caller does, it needs randomness).
json mergeSettings(const json& defaults, const json& saved);
// applySettings(s): validate what a view sent and return the next settings (never throws on odd input).
// `newPhoneToken` is used only when s.phone.newToken === true.
json applySettings(const json& defaults, const json& current, const json& s, const std::string& newPhoneToken);
json cleanPages(const json& list);
// lib/hibernate.js config(), as the settings store it
json hibernateConfigJson(const json& v);

}}  // namespace deck::core
