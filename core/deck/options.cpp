// core/deck/options.cpp — see options.hpp.
#include "options.hpp"
#include "asks.hpp"
#include "platform.hpp"
#include "util.hpp"
#include "version.hpp"
#include "edition.hpp"

namespace deck { namespace core {

bool shippingExeName(const std::string& exe) { return lowerAscii(baseName(exe)) == lowerAscii(edition::kExeName); }   // the edition's exe (core/deck/edition.hpp)

static bool hasArg(const std::vector<std::string>& args, const char* name) { for (const std::string& a : args) if (a == name) return true; return false; }

bool shippingAllowClaude(bool isApp, const std::vector<std::string>& args) {
  bool on = isApp && !hasArg(args, "--state-dir");
  for (const std::string& a : args) { if (a == "--allow-claude") on = true; else if (a == "--no-claude") on = false; }
  return on;
}
bool shippingAsks(bool isApp, const std::vector<std::string>& args, bool envNamesAskDir) {
  bool on = (isApp && !hasArg(args, "--state-dir")) || envNamesAskDir;
  for (const std::string& a : args) { if (a == "--asks") on = true; else if (a == "--no-asks") on = false; }
  return on;
}
bool shippingExitWatchdog(bool isApp) { return isApp; }
ShippingDefaults shippingDefaults(bool isApp, const std::vector<std::string>& args, Env& env) {
  ShippingDefaults d;
  d.allowClaude = shippingAllowClaude(isApp, args);
  d.asks = shippingAsks(isApp, args, !env.get("ASK_PAGE_STATE_DIR").empty());
  d.exitWatchdog = shippingExitWatchdog(isApp);
  return d;
}

std::string defaultStateDir(Env& env) {
  const std::string os = env.os();
  std::string base;
  if (os == "win32") base = env.get("APPDATA");
  else if (os == "darwin") base = joinPath(joinPath(env.home(), "Library"), "Application Support");
  else base = !env.get("XDG_CONFIG_HOME").empty() ? env.get("XDG_CONFIG_HOME") : joinPath(env.home(), ".config");
  return joinPath(joinPath(base, edition::kStateDirName), "local");   // the edition's folder (core/deck/edition.hpp)
}

DeckBuild buildDeckOptions(const HostOptions& in, Env& env, Files& files) {
  DeckBuild b;
  const std::string os = env.os();
  b.stateDir = !in.stateDir.empty() ? in.stateDir : defaultStateDir(env);
  DeckOptions& o = b.options;
  o.stateDir = b.stateDir;
  o.home = !in.home.empty() ? in.home : env.home();
  o.aliasFile = !in.aliasFile.empty() ? in.aliasFile : platform::aliasFiles(env, files).primary;
  o.allowClaude = in.allowClaude; o.persistShells = in.persistShells; o.allowAccountEdit = in.allowAccountEdit;
  if (!in.shellFile.empty()) { o.shellFile = in.shellFile; o.shellArgs = in.shellArgs; o.shellName = in.shellName; }
  else { const platform::Shell sh = platform::shellCommand(env); o.shellFile = sh.file; o.shellArgs = sh.args; o.shellName = sh.name; }
  o.claudeExe = platform::claudeExe(env, files, !in.claudeExe.empty() ? in.claudeExe : env.get("CLAUDE_DECK_CLAUDE"));
  o.bash = platform::bashPath(env, files, env.get("CLAUDE_DECK_BASH"));
  o.cwdOverride = env.get("CLAUDE_DECK_CWD");
  // the version: what the shell names, else the one place it is written (core/deck/version.hpp)
  o.version = !in.version.empty() ? in.version : std::string(kNativeDeckVersion);
  o.labelFallback = in.labelFallback.is_object() ? in.labelFallback : nlohmann::json::object();
  // how a remote desk and this deck's own windows live together (core/deck/deck.hpp)
  o.remoteDeskIsAWindow = in.remoteDeskIsAWindow; o.driverClaim = in.driverClaim;
  o.oneDeck = in.oneDeck;
  // the ask feed: the folders named outright, else — when it is on — where lib/asks.js locations() finds them
  if (!in.askStateDir.empty()) { o.askStateDir = in.askStateDir; o.askDownloads = in.askDownloads; }
  else if (in.asks) { const AskLocations loc = askLocations(files, env); o.askStateDir = loc.stateDir; o.askDownloads = loc.downloads; }
  o.deskOpenInbox = in.deskOpenInbox;
  o.cloudPolling = in.cloudPolling; o.allowCloudCreate = in.allowCloudCreate;
  o.usagePolling = in.usagePolling;
  b.wantsHttp = in.usagePolling || in.cloudPolling;
  // the platform's own words (the deck's defaults are the Windows ones; said outright here they are any platform's)
  o.keyHelp = platform::keyHelp(os);
  o.fontStack = platform::defaultFontStack(os);

  ServerOptions& so = b.server;
  so.assetRoot = in.assetRoot;
  if (so.assetRoot.empty() && !in.exeDir.empty()) {
    std::string dir = in.exeDir;
    for (int up = 0; up < 6 && !dir.empty(); up++, dir = dirName(dir))
      if (files.isFile(joinPath(joinPath(joinPath(dir, "renderer"), "phone"), "index.html"))) { so.assetRoot = dir; break; }
  }
  so.forcePort = in.servePort;
  o.userswitch = platform::userswitchPath(env, files, so.assetRoot, in.resourcesDir);
  return b;
}

}}  // namespace deck::core
