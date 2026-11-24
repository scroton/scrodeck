// core/deck/options.hpp — THE ONE PLACE a shell's wishes become the core's options, for every shell.
//
// A native shell (windows/src + windows/host; the Mac's DeckCoreBridge) knows a handful of plain facts — where its
// state lives, which switches the command line named, where the served pages are — and the core needs a complete
// DeckOptions + ServerOptions. buildDeckOptions is that step, once: every option the core grows is set HERE, so no
// shell that makes a deck can miss one. Everything it decides goes through core/deck/platform.hpp (lib/platform.js)
// over Env + Files, so the answers are the platform's (`Env::os()`): PowerShell or `$SHELL -l`, claude.exe or
// claude, `\` or `/`.
//
// Also here: THE SHIPPING DEFAULTS of the three switches that must never be on by accident (Claude sessions, the
// ask feed, the exit watchdog), as pure functions of { is this process the shipped app, its arguments, its
// environment }. A shell decides "is the shipped app" its own way (Windows: the exe's name, shippingExeName; the
// Mac: its bundle identifier) and asks the same rules.
//
// PURE: no OS header; reads only through Env and Files. Tested on fakes in both path modes: core/test/deck
// (sections `options`).
#pragma once
#include "deck.hpp"
#include "seams.hpp"
#include "server.hpp"
#include <string>
#include <vector>

namespace deck { namespace core {

// ------------------------------------------------------------------------------------------ shipping defaults
// Windows: is this process file the app itself — ClaudeDeckNative.exe, any case, in any folder (a path or a bare
// name; '\' or '/'). Anything else (a test exe, a tool that links the host, "") is not.
bool shippingExeName(const std::string& exe);

// CLAUDE SESSIONS (decided 2026-11-18, after the launch was proven against a real claude.exe). The owner's goal is
// a native deck that replaces the Electron one, so a REAL local deck starts Claude sessions, and nothing else does
// unless it says so:
//   - the app itself on its own state folder (no `--state-dir`): ON;
//   - the app on a scratch state folder (`--state-dir <folder>`: every test of the app, any throwaway deck): OFF;
//   - any other process: OFF;
//   - `--allow-claude` turns it on, `--no-claude` off, wherever; the last of the two wins.
// `args`: the process's arguments without the program.
bool shippingAllowClaude(bool isApp, const std::vector<std::string>& args);
// THE ASK FEED (lib/asks.js; decided 2026-11-19). A deck with a feed READS the ask-page skill's state folder and
// WRITES deck.json into it every 30 s. That folder is the owner's real one unless ASK_PAGE_STATE_DIR says otherwise:
//   - the app itself on its own state folder: ON, the real folder, as the Electron deck;
//   - anything else (a scratch `--state-dir` deck, a test exe): ON only when ASK_PAGE_STATE_DIR is set in the
//     environment (`envNamesAskDir`) — a throwaway deck never touches the real folder by default. (The Electron
//     deck does. Deliberately stricter here.)
//   - `--no-asks` turns it off, `--asks` on, wherever; the last of the two wins.
bool shippingAsks(bool isApp, const std::vector<std::string>& args, bool envNamesAskDir);
// THE EXIT WATCHDOG (main.js armExitWatchdog): the process is ended 8 s after the deck began to shut down if it is
// still there. ON only for the app itself — a test exe makes and shuts down many decks and must outlive each.
bool shippingExitWatchdog(bool isApp);

struct ShippingDefaults { bool allowClaude = false, asks = false, exitWatchdog = false; };
// All three at once; the ask rule's environment fact is read from `env` (ASK_PAGE_STATE_DIR).
ShippingDefaults shippingDefaults(bool isApp, const std::vector<std::string>& args, Env& env);
// …for a Windows-style process file name. (The const char* twin is not decoration: without it a string literal
// converts to the BOOL overload - a pointer is "true" - and any exe would count as the app.)
inline ShippingDefaults shippingDefaults(const std::string& exe, const std::vector<std::string>& args, Env& env) { return shippingDefaults(shippingExeName(exe), args, env); }
inline ShippingDefaults shippingDefaults(const char* exe, const std::vector<std::string>& args, Env& env) { return shippingDefaults(std::string(exe ? exe : ""), args, env); }

// ------------------------------------------------------------------------------------------ the options builder
// What a shell says. Every field has a default that is right for a throwaway deck: nothing that touches the
// owner's own things (Claude, the ask folder, the accounts' tokens, the alias file) is on unless the shell says so
// — a shell fills those from shippingDefaults and its own command line.
struct HostOptions {
  // Where tiles.json and settings.json live. Empty = defaultStateDir(env).
  std::string stateDir;
  // os.homedir(). Empty = env.home().
  std::string home;
  // The account alias file. Empty = platform::aliasFiles(env, files).primary.
  std::string aliasFile;
  bool allowClaude = false;                 // THE SWITCH (DeckOptions::allowClaude)
  bool persistShells = false;
  bool allowAccountEdit = false;            // Settings -> Accounts may write the alias file (the owner's word)
  // The "New terminal" program. Empty file = platform::shellCommand(env): powershell.exe -NoLogo on Windows, the
  // login `$SHELL -l` elsewhere.
  std::string shellFile; std::vector<std::string> shellArgs; std::string shellName;
  // platform.claudeExe's override. Empty = %CLAUDE_DECK_CLAUDE%, else the platform's search.
  std::string claudeExe;
  // `--serve <port>`: for THIS run the server is on, on 127.0.0.1:<port>, whatever the settings say.
  int servePort = 0;
  // Where the served pages live (the folder holding renderer/, lib/, assets/, node_modules/@xterm). Empty = looked
  // for from `exeDir` upwards (its own folder, then five above it: a checkout's windows/build is two below the
  // repo root). None found: the pages answer 404 and phoneStatus().assets says 'missing'.
  std::string assetRoot;
  std::string exeDir;                       // the folder of the process's own file ("" = no search)
  std::string resourcesDir;                 // a packaged app's resources (userswitch may be under its bin/)
  bool usagePolling = false;                // read the accounts' tokens for the usage bars (the owner's word)
  // The ask feed: `askStateDir` / `askDownloads` name the folders outright (a test's temp folders) and turn the
  // feed on; else, when `asks` is on, they are resolved as lib/asks.js locations() does.
  bool asks = false;
  std::string askStateDir; std::vector<std::string> askDownloads;
  bool deskOpenInbox = true;
  bool cloudPolling = false, allowCloudCreate = false;
  // app.getVersion(), for deck-state.json. Empty = core/deck/version.hpp's kNativeDeckVersion.
  std::string version;
  // labels to show when this deck's own settings have none for an account: { login e-mail: short label }, e.g. the
  // Electron deck's settings.json `aliases` (office check 2026-11-20: a first local run showed the e-mails)
  nlohmann::json labelFallback = nlohmann::json::object();
  nlohmann::json oneDeck;                                   // one deck at a time (deck.hpp DeckOptions::oneDeck); null = off
  bool remoteDeskIsAWindow = false;                         // the remote-desk rule is the default (deck.hpp)
  bool driverClaim = DeckOptions().driverClaim;             // a proposal: whatever the core's default is
};

//   options    core::DeckOptions, complete
//   server     core::ServerOptions: the asset root (given, or found) and --serve's port
//   stateDir   the deck's state folder as resolved
//   wantsHttp  hand the core Http + Credentials (usage and / or cloud polling was asked for)
struct DeckBuild { DeckOptions options; ServerOptions server; std::string stateDir; bool wantsHttp = false; };
DeckBuild buildDeckOptions(const HostOptions& o, Env& env, Files& files);

// The native deck's OWN state folder — never the Electron deck's:
//   win32   %APPDATA%\claude-deck-native\local
//   darwin  ~/Library/Application Support/claude-deck-native/local
//   else    $XDG_CONFIG_HOME (or ~/.config)/claude-deck-native/local
std::string defaultStateDir(Env& env);

}}  // namespace deck::core
