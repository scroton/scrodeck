// windows/host-test/serve_host.cpp — the native deck with NO window, serving: the rig the server's real tests run
// against (windows/host-test/serve.ps1). It is the same LocalDeck ClaudeDeckNative.exe --local --serve <port>
// runs — real pseudoconsoles, real sockets on 127.0.0.1 — pumped by a plain loop instead of a window's messages.
//
//   deck-serve-host.exe --state-dir <folder> --port <8811..8819> [--assets <repo root>] [--shell cmd.exe]
//                       [--stop-file <path>] [--seconds <n>] [--ping-ms <n>]
// It writes <state-dir>\serve-host.ready when it is listening ({ pid, port, running, error, assets, assetRoot } — never the token:
// the test reads that from <state-dir>\settings.json, as a person reads it from Settings), runs until the stop
// file appears or the time is up, shuts down, and writes <state-dir>\serve-host.done ({ leftover: <processes it
// started that are still alive> }). Never starts Claude: allowClaude is not touched.
#include "../host/conpty.hpp"
#include "../host/localdeck.hpp"
#include "../host/statefile.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>

using namespace deck::host;

int main(int argc, char** argv) {
  LocalDeckOptions o;
  std::string stopFile; int seconds = 120;
  o.shellFile = L"cmd.exe"; o.shellArgs = { L"/d", L"/k", L"prompt $G" }; o.shellName = "cmd";
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--state-dir") o.stateDir = next();
    else if (a == "--port") o.servePort = std::atoi(next().c_str());
    else if (a == "--assets") o.assetRoot = next();
    else if (a == "--shell") { o.shellFile = widen(next()); o.shellArgs.clear(); }
    else if (a == "--stop-file") stopFile = next();
    else if (a == "--seconds") seconds = std::atoi(next().c_str());
  }
  if (o.stateDir.empty() || o.stateDir.find("AppData\\Roaming") != std::string::npos) { std::fprintf(stderr, "--state-dir <a temp folder> is required\n"); return 2; }
  if (o.servePort < 8811 || o.servePort > 8819) { std::fprintf(stderr, "--port must be 8811..8819 (the ports a test may bind)\n"); return 2; }
  o.home = joinPath(o.stateDir, "home");                  // a fake home: no account, no alias file, nothing of the user's
  makeDirs(o.home);
  o.aliasFile = joinPath(o.home, ".claude-code-account-aliases");
  o.claudeExe = joinPath(o.stateDir, "no-claude-here.exe");
  o.stateDir = joinPath(o.stateDir, "local");

  std::set<uint32_t> everPid;
  LocalDeck deck(o, [](const json&) {});
  deck.start();
  const json st = deck.serveStatus();
  writeAtomic(joinPath(dirName(o.stateDir), "serve-host.ready"), json{ { "pid", GetCurrentProcessId() }, { "port", st.value("port", json()) }, { "running", st.value("running", json()) },
    { "address", st.value("address", json()) }, { "error", st.value("error", json()) }, { "assets", st.value("assets", json()) }, { "assetRoot", st.value("assetRoot", json()) } }.dump());
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  int n = 0;
  while (std::chrono::steady_clock::now() < until) {
    deck.pump();
    if (++n % 20 == 0) { for (uint32_t p : deck.livePids()) everPid.insert(p); if (!stopFile.empty() && isFile(stopFile)) break; }
    deck.waitForWork(std::min(20, std::max(1, deck.nextDueMs())));
  }
  for (uint32_t p : deck.livePids()) everPid.insert(p);
  deck.shutdown();
  int leftover = 0;
  for (int i = 0; i < 200; i++) { leftover = 0; for (uint32_t p : everPid) if (pidAlive(p)) leftover++; if (!leftover) break; std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
  writeAtomic(joinPath(dirName(o.stateDir), "serve-host.done"), json{ { "leftover", leftover }, { "processesStarted", everPid.size() } }.dump());
  return leftover ? 1 : 0;
}
