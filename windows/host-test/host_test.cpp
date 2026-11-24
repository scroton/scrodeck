// windows/host-test/host_test.cpp — the headless test of windows/host. Prints PASS/FAIL lines, exits non-zero
// on any failure, and leaves nothing running.   powershell -File windows/host-test/build.ps1
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../host/conpty.hpp"
#include "../host/listener.hpp"
#include "../host/winhttp.hpp"
#include "../host/winseams.hpp"
#include "../../core/deck/asks.hpp"
#include "../../core/deck/usage.hpp"
#include "../host/deckrules.hpp"
#include "../host/localdeck.hpp"
#include "../../core/deck/edition.hpp"
// the edition's exe (core/deck/edition.hpp: ClaudeDeckNative.exe in this copy), and the same name in other letters
static const std::wstring kApp = L"" DECK_EDITION_EXE;
static std::wstring appOtherCase() { std::wstring s = kApp; for (size_t i = 0; i + 4 < s.size(); i++) s[i] = (wchar_t)towlower(s[i]); for (size_t i = s.size() - 3; i < s.size(); i++) s[i] = (wchar_t)towupper(s[i]); return s; }
#include "../../core/deck/version.hpp"
#include "../host/statefile.hpp"
#include "../../core/proto/frames.hpp"
#include "../../core/session/jsutil.hpp"
#include "../../core/vt/terminal.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>

using namespace deck::host;

static int passed = 0, failed = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
  if (ok) { ++passed; std::printf("PASS  %s\n", name.c_str()); }
  else { ++failed; std::printf("FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str()); }
  std::fflush(stdout);
}
static int64_t tickMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// every viewport row of a terminal as UTF-8 text, rows joined by \n
static std::string screenText(const deck::vt::Terminal& t) {
  std::string out;
  for (int y = 0; y < t.rows(); y++) {
    const deck::vt::Line& ln = t.line(y);
    std::string row;
    for (int x = 0; x < t.cols() && x < (int)ln.cells.size(); x++) {
      const deck::vt::Cell& c = ln.cells[x];
      if (c.width == 0) continue;
      char32_t ch = c.ch ? c.ch : U' ';
      if (ch < 0x80) row += (char)ch;
      else if (ch < 0x800) { row += (char)(0xC0 | (ch >> 6)); row += (char)(0x80 | (ch & 0x3F)); }
      else if (ch < 0x10000) { row += (char)(0xE0 | (ch >> 12)); row += (char)(0x80 | ((ch >> 6) & 0x3F)); row += (char)(0x80 | (ch & 0x3F)); }
      else { row += (char)(0xF0 | (ch >> 18)); row += (char)(0x80 | ((ch >> 12) & 0x3F)); row += (char)(0x80 | ((ch >> 6) & 0x3F)); row += (char)(0x80 | (ch & 0x3F)); }
    }
    while (!row.empty() && row.back() == ' ') row.pop_back();
    out += row; out += '\n';
  }
  return out;
}

// Top-level windows on screen that belong to this process's descendants (the shells it started, and the
// conhost of every pseudoconsole, which is a child of the process that created it). The rule is none, ever.
struct WinScan { std::set<DWORD> pids; int visible; };
static BOOL CALLBACK scanProc(HWND w, LPARAM l) {
  WinScan* s = (WinScan*)l; DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (s->pids.count(pid) && IsWindowVisible(w)) s->visible++;
  return TRUE;
}
static int visibleChildWindows(size_t* childCount = nullptr) {
  WinScan s; s.visible = 0;
  for (uint32_t p : descendantsOf(GetCurrentProcessId())) s.pids.insert(p);
  if (childCount) *childCount = s.pids.size();
  EnumWindows(scanProc, (LPARAM)&s);
  return s.visible;
}

// ---------------------------------------------------------------- ConPty alone
static void testConPty() {
  std::mutex m; std::string pending; bool gotExit = false; uint32_t exitCode = 999; bool dataAfterExit = false;
  PtySpec spec;
  spec.file = L"cmd.exe"; spec.args = { L"/d", L"/k", L"prompt $G" };
  spec.env = currentEnvironment();
  wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); spec.cwd = tmp;
  std::string err;
  auto pty = ConPty::spawn(spec,
    [&](const char* b, size_t n) { std::lock_guard<std::mutex> g(m); if (gotExit) dataAfterExit = true; pending.append(b, n); },
    [&](uint32_t code) { std::lock_guard<std::mutex> g(m); gotExit = true; exitCode = code; }, &err);
  check("conpty: cmd.exe starts in a pseudoconsole", !!pty, err);
  if (!pty) return;
  check("conpty: pid is a live process", pty->pid() != 0 && pidAlive(pty->pid()));
  check("conpty: starts at 120 x 30", pty->cols() == 120 && pty->rows() == 30);

  deck::vt::Terminal term(120, 30);
  auto pumpUntil = [&](const std::string& want, int ms) {
    const int64_t until = tickMs() + ms;
    for (;;) {
      std::string take; { std::lock_guard<std::mutex> g(m); take.swap(pending); }
      if (!take.empty()) { term.feed(take.data(), take.size()); std::string r = term.takeReplies(); if (!r.empty()) pty->write(r); }
      if (screenText(term).find(want) != std::string::npos) return true;
      if (tickMs() > until) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
  };
  check("conpty: the prompt arrives", pumpUntil(">", 15000), screenText(term));
  pty->write("echo deck-host-ok\r");
  check("conpty: typed command's output is on the parsed screen", pumpUntil("\ndeck-host-ok", 15000), screenText(term));

  check("conpty: resize is taken", pty->resize(97, 31) && pty->cols() == 97 && pty->rows() == 31);
  term.resize(97, 31);
  pty->write("mode con\r");
  bool sized = pumpUntil("97", 15000);
  std::string scr = screenText(term);
  check("conpty: the console reports the new size (mode con: 97 columns)", sized && scr.find("Columns:") != std::string::npos && scr.find("97") != std::string::npos, scr);
  check("conpty: resize refuses a zero size", !pty->resize(0, 10));

  // a grandchild, so the tree kill has a tree: cmd -> cmd (waiting on its own stdin)
  pty->write("cmd /d /k prompt $G$G\r");
  check("conpty: a nested shell starts", pumpUntil(">>", 15000), screenText(term));
  std::vector<uint32_t> tree = pty->processTree();
  check("conpty: the process tree holds the shell and its child", tree.size() >= 2, "size " + std::to_string(tree.size()));
  { size_t kids = 0; const int vis = visibleChildWindows(&kids);
    check("conpty: the shells and their console host show no window", vis == 0 && kids >= 3, std::to_string(vis) + " visible, " + std::to_string(kids) + " child processes"); }

  pty->kill();
  const int64_t until = tickMs() + 10000;
  for (;;) { { std::lock_guard<std::mutex> g(m); if (gotExit) break; } if (tickMs() > until) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  { std::lock_guard<std::mutex> g(m);
    check("conpty: kill ends in an exit callback", gotExit);
    check("conpty: a killed shell exits 1 (TerminateProcess, as taskkill /F)", exitCode == 1, "code " + std::to_string(exitCode)); }
  bool gone = false;
  for (int i = 0; i < 200 && !gone; i++) { gone = true; for (uint32_t p : tree) if (pidAlive(p)) gone = false; if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
  check("conpty: no process of the tree is left", gone);
  check("conpty: write after the end is refused", !pty->write("x"));
  pty.reset();
  check("conpty: no data callback after the exit callback", !dataAfterExit);

  // a natural exit carries the shell's own exit code, after its last output
  std::string out2; bool exit2 = false; uint32_t code2 = 999;
  PtySpec s2 = spec; s2.args = { L"/d", L"/c", L"echo bye-from-cmd& exit 7" };
  auto p2 = ConPty::spawn(s2, [&](const char* b, size_t n) { std::lock_guard<std::mutex> g(m); out2.append(b, n); },
    [&](uint32_t c) { std::lock_guard<std::mutex> g(m); exit2 = true; code2 = c; }, &err);
  check("conpty: a one-shot command starts", !!p2, err);
  if (p2) {
    const int64_t u2 = tickMs() + 15000;
    for (;;) { { std::lock_guard<std::mutex> g(m); if (exit2) break; } if (tickMs() > u2) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::lock_guard<std::mutex> g(m);
    check("conpty: natural exit reports the exit code", exit2 && code2 == 7, "code " + std::to_string(code2));
    check("conpty: the output was delivered before the exit", out2.find("bye-from-cmd") != std::string::npos);
  }
  p2.reset();

  PtySpec bad = spec; bad.file = L"no-such-program-deck.exe";
  auto p3 = ConPty::spawn(bad, [](const char*, size_t) {}, [](uint32_t) {}, &err);
  check("conpty: a missing program is 'File not found: '", !p3 && err.rfind("File not found: ", 0) == 0, err);

  check("argsToCommandLine: plain", argsToCommandLine(L"powershell.exe", { L"-NoLogo" }) == L"powershell.exe -NoLogo");
  check("argsToCommandLine: spaces are quoted", argsToCommandLine(L"C:\\Program Files\\x.exe", { L"a b", L"" }) == L"\"C:\\Program Files\\x.exe\" \"a b\" \"\"");
  check("argsToCommandLine: quotes and backslashes", argsToCommandLine(L"x", { L"say \"hi\"", L"dir\\", L"a b\\" }) == L"x \"say \\\"hi\\\"\" dir\\ \"a b\\\\\"");
}

// ---------------------------------------------------------------- the pure rules (deckrules, statefile)
using json = nlohmann::json;
static json loadJson(const std::string& file) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return json(json::value_t::discarded);
  std::stringstream ss; ss << f.rdbuf();
  return json::parse(ss.str(), nullptr, false);
}
static json envObject(const EnvList& env) { json o = json::object(); for (const auto& kv : env) o[kv.first] = kv.second; return o; }
static std::string tempRoot() {
  wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
  std::string dir = narrow(tmp);
  while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
  return dir + "\\deck-host-test-" + std::to_string(GetCurrentProcessId());
}
static void removeTree(const std::string& dir) {
  for (const std::string& n : listDir(dir)) { const std::string p = joinPath(dir, n); if (isDir(p)) removeTree(p); else removeFile(p); }
  RemoveDirectoryW(widen(dir).c_str());
}
static bool writeText(const std::string& path, const std::string& text) { makeDirs(dirName(path)); std::ofstream f(widen(path).c_str(), std::ios::binary); f << text; return (bool)f; }

static void testRules(const std::string& fixtures) {
  const json dflt = loadJson(fixtures + "/default-settings.json");
  if (dflt.is_discarded()) check("rules: fixtures found (run node core/test/session/gen-fixtures.js)", false, fixtures);
  else {
    const json mine = defaultSettings(dflt["home"]);
    check("rules: DEFAULT_SETTINGS is main.js's", mine == dflt["settings"], mine.dump().substr(0, 300));
    check("rules: a saved file with nothing in it merges to the defaults", mergeSettings(mine, json::object()) == mine);
  }
  const json meta = loadJson(fixtures + "/meta.json");
  if (!meta.is_discarded()) {
    for (const json& c : meta["env"]) {
      EnvList parent; for (auto it = c["parent"].begin(); it != c["parent"].end(); ++it) parent.emplace_back(it.key(), it.value().get<std::string>());
      EnvList got;
      if (c["acct"].is_null()) got = baseEnv(parent);
      else { Account a; a.id = c["acct"]["id"]; a.dir = c["acct"]["dir"]; a.isDefault = c["acct"]["isDefault"]; got = envFor(parent, a); }
      check("rules: spawn environment — " + c["name"].get<std::string>(), envObject(got) == c["out"], envObject(got).dump());
    }
    for (const json& c : meta["newTileSpec"]) {
      const TileSpec t = newTileSpec(c["in"]);
      check("rules: newTileSpec " + c["in"].dump(), t.toJson() == c["out"] && t.hasBrowser == c["hasBrowser"].get<bool>(), t.toJson().dump());
    }
  }
  bool threw = false; try { newTileSpec({ { "kind", "cloud" }, { "task", "x" } }); } catch (const std::exception&) { threw = true; }
  check("rules: a cloud tile is refused", threw);

  // the Claude spawn seam
  Account acct; acct.id = "claudev"; acct.dir = "C:\\Users\\fixture\\.claude-v";
  ClaudeLaunchInput in;
  in.parentEnv = { { "Path", "C:\\Windows" }, { "CLAUDECODE", "1" }, { "CLAUDE_CODE_SSE_PORT", "9" }, { "NO_COLOR", "1" } };
  in.claudeExe = "C:\\Users\\fixture\\.local\\bin\\claude.exe"; in.account = acct; in.cwd = "C:\\work"; in.freshUuid = "11111111-2222-4333-8444-555555555555";
  ClaudeLaunch l = claudeLaunch(in);
  check("claude seam: a new session gets --session-id <fresh uuid>", l.args == std::vector<std::string>{ "--session-id", in.freshUuid } && l.sessionId == in.freshUuid && l.file == in.claudeExe && l.cwd == "C:\\work");
  json env = envObject(l.env);
  check("claude seam: the account's config dir, no parent session markers, colour on",
    env == json{ { "Path", "C:\\Windows" }, { "CLAUDE_CODE_FORCE_SESSION_PERSISTENCE", "1" }, { "TERM_PROGRAM", "claude-deck" }, { "COLORTERM", "truecolor" }, { "CLAUDE_CONFIG_DIR", acct.dir } }, env.dump());
  in.sessionId = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"; in.transcriptExists = true;
  l = claudeLaunch(in);
  check("claude seam: a session with a transcript is resumed", l.args == std::vector<std::string>{ "--resume", in.sessionId } && l.sessionId == in.sessionId);
  in.transcriptExists = false;
  l = claudeLaunch(in);
  check("claude seam: a session that never got a message starts afresh under the same id", l.args == std::vector<std::string>{ "--session-id", in.sessionId });
  in.browser = true; in.transcriptExists = true;
  check("claude seam: browser on -> --chrome", claudeLaunch(in).args == std::vector<std::string>{ "--resume", in.sessionId, "--chrome" });
  in.browser = false;
  check("claude seam: browser off -> --no-chrome", claudeLaunch(in).args == std::vector<std::string>{ "--resume", in.sessionId, "--no-chrome" });
  in.model = "claude-fable-5-2"; in.prompt = "check the \"bells\"\nplease"; in.desk = "security";
  l = claudeLaunch(in);
  check("claude seam: a desk's model and first message ride last, and its kind is in the environment",
    l.args == std::vector<std::string>{ "--resume", in.sessionId, "--no-chrome", "--model", "claude-fable-5-2", in.prompt } && envObject(l.env)["CLAUDE_ONCALL_KIND"] == "security-desk");
  Account dflt2; dflt2.id = "claude"; dflt2.dir = "C:\\Users\\fixture\\.claude"; dflt2.isDefault = true;
  in = ClaudeLaunchInput(); in.parentEnv = { { "CLAUDE_CONFIG_DIR", "C:\\x" }, { "A", "1" } }; in.account = dflt2; in.claudeExe = "claude.exe"; in.freshUuid = "u";
  check("claude seam: the default account runs with no CLAUDE_CONFIG_DIR", !envObject(claudeLaunch(in).env).contains("CLAUDE_CONFIG_DIR"));
  check("claude seam: the command line node-pty would build", argsToCommandLine(L"C:\\Users\\a b\\claude.exe", { L"--resume", L"id", L"say \"hi\" now" }) == L"\"C:\\Users\\a b\\claude.exe\" --resume id \"say \\\"hi\\\" now\"");

  // accounts
  const std::string home = "C:\\Users\\fixture";
  std::vector<Account> as = parseAccounts(
    "# comment\r\nalias claudev='CLAUDE_CONFIG_DIR=\"$HOME/.claude-vv\" claude'\nalias vv='CLAUDE_CONFIG_DIR=\"$HOME/.claude-vv\" claude'\n"
    "  alias claudedemo=\"CLAUDE_CONFIG_DIR='~/.claude-m' claude\"  \nalias plain='claude'\nalias other='CLAUDE_CONFIG_DIR=D:/cfg/./x/../other claude'\nalias bad name='x'\nalias nq=CLAUDE\n", home);
  check("accounts: one per config dir, the default dropped when there are others", as.size() == 3, std::to_string(as.size()));
  if (as.size() == 3) {
    check("accounts: the shortest alias is the id", as[0].id == "vv" && as[0].aliases == std::vector<std::string>{ "claudev", "vv" } && as[0].dir == home + "\\.claude-vv");
    check("accounts: ~ and quotes", as[1].id == "claudedemo" && as[1].dir == home + "\\.claude-m");
    check("accounts: an unquoted dir is normalised", as[2].id == "other" && as[2].dir == "D:\\cfg\\other", as[2].dir);
    check("accounts: lookup by any alias", accountById(as, "claudev") == &as[0] && accountById(as, "nobody") == nullptr);
  }
  as = parseAccounts("", home);
  check("accounts: no alias file -> the default account alone", as.size() == 1 && as[0].isDefault && as[0].id == "claude" && as[0].dir == home + "\\.claude");
  as = parseAccounts("alias cl='claude --dangerously'\n", home);
  check("accounts: an alias of the default dir joins it", as.size() == 1 && as[0].isDefault && as[0].aliases.size() == 2);

  // settings
  const json D = defaultSettings(home);
  json next = applySettings(D, D, { { "fontSize", 99 }, { "zoomFontSize", -3 }, { "cwd", "  D:\\w  " }, { "effortStyle", "bogus" }, { "sidebarSide", "right" }, { "driver", "phone" },
    { "grid", { { "rows", 9 }, { "cols", "3" } } }, { "bars", { { "week", { { "show", false }, { "from", "#123abc" }, { "to", "red" }, { "name", "  W  " } } } } },
    { "notify", { { "minTurnSec", 99999 }, { "sound", false } } }, { "msgLines", { { "normal", 0 } } }, { "hibernate", { { "idleMinutes", 5000 } } },
    { "asks", { { "deckThreshold", "7" }, { "picker", "pane" }, { "usePages", false } } }, { "phone", { { "enabled", true }, { "port", 80 }, { "token", "stolen" }, { "newToken", false } } },
    { "pages", { { { "label", "", }, { "file", "\"C:\\p\\Report.HTML\"" } }, { { "file", "relative.html" } }, { { "file", "C:\\p\\x.txt" } } } }, { "unknownKey", 1 } }, "tok");
  check("settings: numbers are clamped", next["fontSize"] == 28 && next["zoomFontSize"] == 0 && next["notify"]["minTurnSec"] == 3600 && next["msgLines"]["normal"] == 1 && next["msgLines"]["wide"] == 10 && next["hibernate"]["idleMinutes"] == 1440, next.dump().substr(0, 400));
  check("settings: enums only take their values", next["effortStyle"] == "tag" && next["sidebarSide"] == "right" && next["driver"] == "phone" && next["cwd"] == "D:\\w");
  check("settings: grid 1..4", next["grid"] == json{ { "rows", 4 }, { "cols", 3 } }, next["grid"].dump());
  check("settings: a bar keeps defaults for what was not valid", next["bars"]["week"] == json{ { "show", false }, { "name", "W" }, { "from", "#123abc" }, { "to", "#ef4444" } } && next["bars"]["session"] == D["bars"]["session"], next["bars"].dump());
  check("settings: the ask block", next["asks"]["deckThreshold"] == 7 && next["asks"]["picker"] == "pane" && next["asks"]["usePages"] == false && next["asks"]["showTile"] == true);
  check("settings: a phone token is never taken from the page", next["phone"] == json{ { "enabled", true }, { "port", 1024 }, { "bind", "tailscale" }, { "token", "" } }, next["phone"].dump());
  check("settings: pages need an absolute .html path", next["pages"] == json::array({ { { "label", "Report" }, { "file", "C:\\p\\Report.HTML" } } }), next["pages"].dump());
  check("settings: unknown keys are not stored", !next.contains("unknownKey"));
  check("settings: nothing sent changes nothing", applySettings(D, D, json::object(), "t") == D && applySettings(D, D, nullptr, "t") == D);
  json merged = mergeSettings(D, { { "fontSize", 17 }, { "bars", { { "week", { { "show", false } } } } }, { "phone", { { "port", 9000 } } }, { "aliases", { { "a@b.c", "AB" } } } });
  check("settings: a saved file merges per key under nested groups", merged["fontSize"] == 17 && merged["bars"]["week"]["show"] == false && merged["bars"]["week"]["name"] == "Week" && merged["phone"]["port"] == 9000 && merged["phone"]["bind"] == "tailscale" && merged["aliases"]["a@b.c"] == "AB");

  // the state file
  const std::string root = tempRoot() + "-sf";
  removeTree(root);
  const std::string file = joinPath(root, "tiles.json");
  StateFile sf(file, [](const json& v) { return v.contains("tiles") && v["tiles"].is_array() ? v["tiles"].size() : (size_t)0; }, json{ { "tiles", json::array() } }, 600000.0);
  check("statefile: a missing file reads as the empty state", sf.read() == json{ { "tiles", json::array() } } && sf.loadAtBoot().data == json{ { "tiles", json::array() } });
  sf.write({ { "tiles", { 1, 2, 3 } } });
  check("statefile: write then read", sf.read()["tiles"].size() == 3 && !isFile(file + ".tmp"));
  const size_t b0 = sf.backups().size();
  sf.write({ { "tiles", { 1, 2, 3, 4 } } });
  sf.write({ { "tiles", { 1 } } });
  bool dropBackup = false; for (const std::string& b : sf.backups()) if (b.find("-drop.json") != std::string::npos) dropBackup = true;
  check("statefile: a write that drops items backs the file up first", dropBackup && sf.backups().size() > b0);
  writeText(file, std::string(64, '\0'));
  check("statefile: NUL bytes are not a state", sf.current().is_discarded());
  StateFile::Boot boot = sf.loadAtBoot();
  check("statefile: an unreadable file is kept aside and the newest good backup loads", boot.data.contains("tiles") && boot.data["tiles"].size() == 4 && !boot.recoveredFrom.empty() && isFile(boot.unreadableCopy) && sf.read()["tiles"].size() == 4, boot.data.dump());
  check("statefile: parse rejects junk, accepts a BOM", parseState("  ").is_discarded() && parseState("nope").is_discarded() && parseState("7").is_discarded() && parseState("\xEF\xBB\xBF{\"a\":1}")["a"] == 1);
  removeTree(root);
  check("statefile: iso time", isoTime(1791792060250.0) == "2026-10-12T08:01:00.250Z" && isoStamp(0) == "1970-01-01T00-00-00-000Z", isoTime(1791792060250.0));
}

// ---------------------------------------------------------------- the deck, end to end
struct Harness {
  std::vector<json> frames;
  std::unique_ptr<LocalDeck> deck;
  int rid = 0;
  size_t seen = 0;                       // frames[0..seen) were already looked at by next()
  std::set<uint32_t> pidsEver;

  void open(const LocalDeckOptions& o) { deck.reset(new LocalDeck(o, [this](const json& f) { frames.push_back(f); })); deck->start(); }
  void send(const json& f) { deck->handle(f); }
  void pump(int ms) {
    const int64_t until = tickMs() + ms;
    for (;;) {
      deck->pump();
      for (uint32_t p : deck->livePids()) pidsEver.insert(p);
      const int64_t left = until - tickMs();
      if (left <= 0) break;
      deck->waitForWork((int)std::min<int64_t>(left, std::min(20, std::max(1, deck->nextDueMs()))));
    }
  }
  bool pumpUntil(const std::function<bool()>& done, int ms) {
    const int64_t until = tickMs() + ms;
    for (;;) {
      deck->pump();
      if (done()) return true;
      if (tickMs() > until) return false;
      deck->waitForWork(std::min(15, std::max(1, deck->nextDueMs())));
    }
  }
  // a call, pumped until its ok / err
  json call(const std::string& fn, json args = json::array(), int ms = 15000) {
    const int id = ++rid;
    send(deck::proto::call(id, fn, args));
    json out;
    pumpUntil([&] { for (const json& f : frames) if ((f["t"] == "ok" || f["t"] == "err") && f["rid"] == id) { out = f; return true; } return false; }, ms);
    if (fn != "deskInit") for (uint32_t p : deck->livePids()) pidsEver.insert(p);
    return out;
  }
  json lastMeta() const { for (size_t i = frames.size(); i-- > 0;) if (frames[i]["t"] == "meta") return frames[i]["meta"]; return json(); }
  json freshMeta() { frames.push_back({ { "t", "meta" }, { "meta", call("deskInit")["result"]["meta"] } }); return frames.back()["meta"]; }
  json tile(const std::string& id) { const json m = freshMeta(); for (const json& t : m["tiles"]) if (t["id"] == id) return t; return json(); }
  bool has(const std::string& type, const std::string& id, size_t from = 0) const { for (size_t i = from; i < frames.size(); i++) if (frames[i]["t"] == type && frames[i].value("id", "") == id) return true; return false; }
  // everything `data` for a tile from frame index `from`, in order
  std::string dataFor(const std::string& id, size_t from, bool replayOnly = false) const {
    std::string s;
    for (size_t i = from; i < frames.size(); i++) if (frames[i]["t"] == "data" && frames[i]["id"] == id && (!replayOnly || frames[i].value("replay", false))) s += frames[i]["d"].get<std::string>();
    return s;
  }
};
// a terminal fed from the harness's data frames for one tile; its replies go back as input, as a view's do
struct View {
  Harness& h; std::string id; deck::vt::Terminal term; size_t at;
  View(Harness& hh, const std::string& tileId, int cols = 120, int rows = 30) : h(hh), id(tileId), term(cols, rows), at(hh.frames.size()) {}
  void feed() {
    for (; at < h.frames.size(); at++) {
      const json& f = h.frames[at];
      if (f["t"] == "reset" && f["id"] == id) term.reset();
      if (f["t"] != "data" || f["id"] != id) continue;
      const std::string d = f["d"];
      term.feed(d.data(), d.size());
      const std::string r = term.takeReplies();
      if (!r.empty()) h.send(deck::proto::input(id, r));
    }
  }
  bool waitFor(const std::string& text, int ms = 20000) { return h.pumpUntil([&] { feed(); return screenText(term).find(text) != std::string::npos; }, ms); }
  void type(const std::string& s) { h.send(deck::proto::call(0, "userInput", json::array({ id }))); h.send(deck::proto::input(id, s)); }
};
static bool allGone(const std::set<uint32_t>& pids, int ms) {
  const int64_t until = tickMs() + ms;
  for (;;) {
    bool any = false; for (uint32_t p : pids) if (pidAlive(p)) any = true;
    if (!any) return true;
    if (tickMs() > until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

static void testDeck() {
  const std::string root = tempRoot();
  removeTree(root);
  const std::string home = joinPath(root, "home"), state = joinPath(root, "state"), work = joinPath(root, "work dir");
  makeDirs(home); makeDirs(work);
  const std::string aliasFile = joinPath(home, ".claude-code-account-aliases");
  writeText(aliasFile, "alias claudet='CLAUDE_CONFIG_DIR=\"$HOME/.claude-t\" claude'\n");
  writeText(joinPath(joinPath(home, ".claude-t"), ".claude.json"), "{\"oauthAccount\":{\"emailAddress\":\"t@example.com\"}}");
  LocalDeckOptions opt;
  opt.stateDir = state; opt.home = home; opt.aliasFile = aliasFile;
  opt.shellFile = L"cmd.exe"; opt.shellArgs = { L"/d", L"/k", L"prompt $G" }; opt.shellName = "cmd";
  opt.persistShells = true;
  opt.claudeExe = joinPath(root, "no-claude-here.exe");
  std::set<uint32_t> everyPid;
  std::string shellId, claudeId, claudeSession;

  {
    Harness h; h.open(opt);
    const int64_t bootAt = tickMs();
    check("deck: start emits the hello", !h.frames.empty() && h.frames[0]["t"] == "init" && h.frames[0]["meta"]["tiles"].empty() && h.frames[0]["home"] == home && h.frames[0]["platform"]["shellName"] == "cmd" && h.frames[0]["platform"]["os"] == "win32", h.frames.empty() ? "" : h.frames[0].dump().substr(0, 300));
    check("deck: its own state folder, settings written on first run", isFile(joinPath(state, "settings.json")) && h.deck->stateDir() == state);
    // a window message before deskInit is ignored
    h.send(deck::proto::layout({ { "ghost", { 1 } } }));
    json init = h.call("deskInit");
    check("deck: deskInit answers { home, meta, platform, layout, windowIndex, grid }", init["t"] == "ok" && init["result"]["home"] == home && init["result"]["windowIndex"] == 0 && init["result"]["grid"] == json{ { "rows", 2 }, { "cols", 2 } }
      && init["result"]["layout"] == json::object() && init["result"]["meta"]["tiles"].is_array() && init["result"]["platform"]["keyHelp"]["next"] == "F12", init.dump().substr(0, 300));
    const json m0 = init["result"]["meta"];
    check("deck: the snapshot's shape", m0["accounts"] == json::array({ { { "id", "claudet" }, { "label", "t@example.com" }, { "abbr", nullptr }, { "email", "t@example.com" }, { "key", "t@example.com" }, { "isDefault", false } } })
      && m0["settings"]["fontSize"] == 14 && m0["settings"]["driver"] == "auto" && m0["restart"]["armed"] == false && m0["groups"].empty() && m0["cloudOption"] == false && m0["browserOption"] == true && m0.contains("restoring"), m0.dump().substr(0, 500));

    // ---- a shell tile
    json made = h.call("newTile", json::array({ { { "kind", "shell" }, { "cwd", work } } }));
    check("deck: newTile({ kind: 'shell' }) answers the tile's id", made["t"] == "ok" && made["result"].is_string(), made.dump());
    shellId = made["result"].is_string() ? made["result"].get<std::string>() : "";
    json t = h.tile(shellId);
    check("deck: the tile is in the meta as a running shell at 120 x 30", t["kind"] == "shell" && t["status"] == "shell" && t["ptySize"] == json{ { "cols", 120 }, { "rows", 30 } } && t["account"].is_null() && t["sessionId"].is_null()
      && t["exited"] == false && t["exitCode"].is_null() && t["cwd"] == work && t["fresh"] == true && t["shownBy"].empty() && t["inWindow"].is_null() && t["sleepBlocker"] == "not a resumable session", t.dump());
    View v(h, shellId);
    const size_t beforeAttach = h.frames.size();
    h.send(deck::proto::attach(shellId));
    check("deck: output reaches an attached view (the prompt)", v.waitFor(">"), screenText(v.term));
    v.type("echo deck-host-ok\r");
    check("deck: typed input runs and its output parses onto the screen", v.waitFor("\ndeck-host-ok"), screenText(v.term));
    // the deck's contract with what it hosts: the process knows its tile and the state file (core/deck/presence.hpp)
    check("deck: the shell's own environment names its tile (CLAUDE_DECK_TILE)", (v.type("echo tile=%CLAUDE_DECK_TILE%=\r"), v.waitFor("\ntile=" + shellId + "=")), screenText(v.term));
    { const json ds = loadJson(joinPath(state, "deck-state.json"));
      check("deck: deck-state.json is there, and a key typed in the window reads as the office", ds["v"] == 1 && ds["where"] == "office" && ds["lastInputFrom"] == "office" && ds["deck"]["pid"] == GetCurrentProcessId() && ds["phone"]["attached"] == false, ds.dump());
      check("deck: …and the path the shell was given is that file", (v.type("if exist \"%CLAUDE_DECK_STATE%\" echo state-file-found\r"), v.waitFor("\nstate-file-found")), screenText(v.term)); }
    check("deck: the shell runs in the folder asked for", (v.type("cd\r"), v.waitFor("work dir")), screenText(v.term));
    t = h.tile(shellId);
    check("deck: a key press marks the driver and the tile is no longer fresh", t["driver"] == "desktop" && t["driverAt"].is_number() && t["fresh"] == false, t.dump());
    (void)beforeAttach;

    // ---- shown / layout
    h.send(deck::proto::shown({ shellId, "no-such-tile" }, false));
    h.send(deck::proto::layout({ { shellId, { 1, 2, 3, 4 } }, { "diagonal", { 1, 4 } } }));
    t = h.tile(shellId);
    check("deck: shown + layout -> shownBy win:0, inWindow 0", t["shownBy"] == json::array({ "win:0" }) && t["inWindow"] == 0, t.dump());
    check("deck: an illegal block is not kept in the layout", h.call("deskInit")["result"]["layout"] == json{ { shellId, { 1, 2, 3, 4 } } });
    h.send(deck::proto::shown({ shellId }, true));
    check("deck: a hidden window shows nothing", h.tile(shellId)["shownBy"].empty());
    h.send(deck::proto::shown({ shellId }, false));

    // ---- resize (debounced 150 ms) and the pty really getting it
    h.send(deck::proto::resize(shellId, 90, 29));
    h.send(deck::proto::resize(shellId, 97, 31));
    check("deck: a resize is not applied before its 150 ms of quiet", h.deck->ptySize(shellId) == std::make_pair(120, 30));
    check("deck: the last size of a burst is applied", h.pumpUntil([&] { return h.deck->ptySize(shellId) == std::make_pair(97, 31); }, 3000));
    v.term.resize(97, 31);
    v.type("mode con\r");
    // (wait for mode con's own "Columns:" line to say 97: a bare "97" also matches the tile id or a pid on the
    // screen above, and the check then ran before mode con had answered - a flake of about one run in six)
    auto columnsLine = [&] { const std::string s = screenText(v.term); const size_t c = s.find("Columns:"); return c == std::string::npos ? std::string() : s.substr(c, s.find('\n', c) - c); };
    check("deck: the console itself reports 97 columns", h.pumpUntil([&] { v.feed(); return columnsLine().find("97") != std::string::npos; }, 20000), screenText(v.term));
    check("deck: meta carries the pty's real size", h.tile(shellId)["ptySize"] == json{ { "cols", 97 }, { "rows", 31 } });
    h.send(deck::proto::resize(shellId, 1, 1));
    h.pump(250);
    check("deck: a 1 x 1 resize is ignored", h.deck->ptySize(shellId) == std::make_pair(97, 31));
    // nudge: one column narrower and back, 100 ms apart
    h.send(deck::proto::call(0, "nudge", json::array({ shellId, 97, 31 })));
    check("deck: nudge narrows by one column at once", h.deck->ptySize(shellId) == std::make_pair(96, 31));
    check("deck: and comes back 100 ms later", h.pumpUntil([&] { return h.deck->ptySize(shellId) == std::make_pair(97, 31); }, 3000));
    h.pump(300); v.feed();

    // ---- replay on re-attach
    h.send(deck::proto::detach(shellId));
    const size_t afterDetach = h.frames.size();
    h.send(deck::proto::input(shellId, "echo while-detached\r"));
    h.pump(700);
    check("deck: a detached tile's output is not sent", h.dataFor(shellId, afterDetach).empty());
    const size_t reattach = h.frames.size();
    h.send(deck::proto::attach(shellId));
    const std::string full = h.dataFor(shellId, reattach, true);
    check("deck: re-attach replays the window, marked replay", !full.empty() && full.find("deck-host-ok") != std::string::npos && full.find("while-detached") != std::string::npos, std::to_string(full.size()));
    { deck::vt::Terminal fresh(97, 31); fresh.feed(full.data(), full.size());
      check("deck: the replay alone redraws the screen in a fresh terminal", screenText(fresh).find("while-detached") != std::string::npos, screenText(fresh)); }
    const size_t sinceAt = h.frames.size();
    h.send(deck::proto::attach(shellId, true));
    const std::string since = h.dataFor(shellId, sinceAt, true);
    check("deck: attach { since } replays only what was written since the last resize", !since.empty() && since.size() < full.size() && full.compare(full.size() - since.size(), since.size(), since) == 0,
      std::to_string(since.size()) + " of " + std::to_string(full.size()));
    v.at = h.frames.size();

    // ---- names, pins, groups
    check("deck: renameTile", h.call("renameTile", json::array({ shellId, "  build shell  " }))["t"] == "ok" && h.tile(shellId)["title"] == "build shell");
    check("deck: pin answers the new state", h.call("pin", json::array({ shellId, true }))["result"] == true && h.tile(shellId)["pinned"] == true);
    check("deck: setGroup adds the group", h.call("setGroup", json::array({ shellId, " ops " }))["t"] == "ok" && h.tile(shellId)["group"] == "ops" && h.lastMeta()["groups"] == json::array({ "ops" }));
    check("deck: setGroups answers the cleaned list", h.call("setGroups", json::array({ json::array({ "ops", " builds ", "ops", "" }) }))["result"] == json::array({ "ops", "builds" }));
    json second = h.call("newTile", json::array({ { { "kind", "shell" } } }));
    const std::string id2 = second["result"].is_string() ? second["result"].get<std::string>() : "";
    check("deck: a second shell; pinned tiles come first in the snapshot", h.freshMeta()["tiles"].size() == 2 && h.lastMeta()["tiles"][0]["id"] == shellId && h.lastMeta()["tiles"][1]["id"] == id2);
    h.call("pin", json::array({ id2, true }));
    check("deck: reorder sets the pinned order", h.call("reorder", json::array({ json::array({ id2, shellId }) }))["t"] == "ok" && h.freshMeta()["tiles"][0]["id"] == id2 && h.lastMeta()["tiles"][1]["id"] == shellId);
    h.call("pin", json::array({ id2, false }));
    check("deck: removing a group ungroups its tiles", h.call("setGroups", json::array({ json::array({ "builds" }) }))["result"] == json::array({ "builds" }) && h.tile(shellId)["group"].is_null());
    h.call("setGroup", json::array({ shellId, "builds" }));
    check("deck: setOscTitle feeds the title when there is no user title", h.call("setOscTitle", json::array({ id2, "Administrator: tools" }))["t"] == "ok" && h.tile(id2)["title"] == "Administrator: tools"
      && (h.call("setOscTitle", json::array({ id2, "C:\\Windows\\system32\\cmd.exe" })), h.tile(id2)["title"].is_null()));

    // ---- exit, restart, refresh
    View v2(h, id2);
    h.send(deck::proto::attach(id2));
    check("deck: the second shell's prompt", v2.waitFor(">"));
    const size_t beforeExit = h.frames.size();
    v2.type("exit 5\r");
    check("deck: a shell that exits sends { t: 'exit', id, exitCode }", h.pumpUntil([&] { for (size_t i = beforeExit; i < h.frames.size(); i++) if (h.frames[i]["t"] == "exit" && h.frames[i]["id"] == id2 && h.frames[i]["exitCode"] == 5) return true; return false; }, 15000));
    t = h.tile(id2);
    check("deck: an exited tile reads off / exited / its code", t["status"] == "off" && t["exited"] == true && t["exitCode"] == 5 && t["ptySize"].is_null(), t.dump());
    const size_t beforeRestart = h.frames.size();
    check("deck: restartTile sends reset and runs it again", h.call("restartTile", json::array({ id2 }))["t"] == "ok" && h.has("reset", id2, beforeRestart) && h.tile(id2)["status"] == "shell" && h.tile(id2)["exited"] == false);
    check("deck: the restarted shell talks", (v2.waitFor(">"), v2.type("echo second-run\r"), v2.waitFor("\nsecond-run")), screenText(v2.term));
    h.pump(50);
    const std::vector<uint32_t> before = h.deck->livePids();
    const size_t beforeRefresh = h.frames.size();
    json refreshed = h.call("refreshTile", json::array({ id2 }));
    const std::vector<uint32_t> after = h.deck->livePids();
    bool replaced = true; for (uint32_t p : before) { bool mine = false; for (uint32_t q : after) if (p == q) mine = true; if (!mine && pidAlive(p)) replaced = false; }
    check("deck: refreshTile kills the process, resets the view, starts a new one", refreshed["t"] == "ok" && h.has("reset", id2, beforeRefresh) && !h.has("exit", id2, beforeRefresh) && h.tile(id2)["status"] == "shell" && replaced && after.size() >= 2);
    check("deck: the refreshed shell's replay starts empty of the old run", (v2.waitFor(">"), screenText(v2.term).find("second-run") == std::string::npos), screenText(v2.term));

    // ---- the driver setting and refused input
    check("deck: setDriver answers the mode", h.call("setDriver", json::array({ "phone" }))["result"] == "phone" && h.lastMeta()["settings"]["driver"] == "phone");
    const size_t beforeRefused = h.frames.size();
    h.send(deck::proto::input(shellId, "echo should-not-run\r"));
    h.send(deck::proto::input(shellId, "\x1b[I"));
    h.send(deck::proto::resize(shellId, 80, 24));
    h.pump(400);
    int refused = 0; for (size_t i = beforeRefused; i < h.frames.size(); i++) if (h.frames[i]["t"] == "event" && h.frames[i]["name"] == "input:refused" && h.frames[i]["payload"]["id"] == shellId && h.frames[i]["payload"]["reason"] == "phone") refused++;
    check("deck: under driver 'phone' the desktop's input and size are refused, and the window is told once", refused == 1 && h.deck->ptySize(shellId) == std::make_pair(97, 31) && h.dataFor(shellId, beforeRefused).find("should-not-run") == std::string::npos);
    h.call("setDriver", json::array({ "auto" }));

    // ---- settings
    const size_t beforeGrid = h.frames.size();
    json saved = h.call("saveSettings", json::array({ { { "fontSize", 99 }, { "grid", { { "rows", 3 }, { "cols", 3 } } }, { "aliases", { { "t@example.com", "TT" } } } } }));
    bool gridEvent = false; for (size_t i = beforeGrid; i < h.frames.size(); i++) if (h.frames[i]["t"] == "event" && h.frames[i]["name"] == "grid:changed" && h.frames[i]["payload"]["rows"] == 3 && h.frames[i]["payload"]["cols"] == 3 && h.frames[i]["payload"]["layout"].contains(shellId)) gridEvent = true;
    check("deck: saveSettings validates, answers the settings, and announces a grid change with the remapped layout", saved["result"]["fontSize"] == 28 && saved["result"]["grid"]["rows"] == 3 && saved["result"]["accounts"][0]["abbr"] == "TT" && saved["result"].contains("phoneStatus") && gridEvent, saved.dump().substr(0, 300));
    json got = h.call("getSettings");
    check("deck: getSettings is the desktop shape", got["result"]["fontSize"] == 28 && got["result"]["aliasFile"] == aliasFile && got["result"]["accountRows"][0]["keywords"] == json::array({ "claudet" }) && got["result"]["phoneStatus"]["running"] == false && got["result"]["accounts"][0]["label"] == "TT", got.dump().substr(0, 300));
    const json onDisk = loadJson(joinPath(state, "settings.json"));
    check("deck: settings.json holds them", onDisk["fontSize"] == 28 && onDisk["grid"]["cols"] == 3 && onDisk["phone"]["token"].is_string() && onDisk["phone"]["token"].get<std::string>().size() == 16);

    // ---- what the local deck refuses by name (the list shrank on 2026-11-18: moves, Elsewhere / Past, special
    // prompts, triggers and restart-when-quiet are in the core deck now; core/test/deck checks them)
    for (const char* fn : { "saveAccounts", "refreshUsage" }) {
      const json r = h.call(fn, json::array({ shellId }));
      check(std::string("deck: ") + fn + " is refused by name", r["t"] == "err" && r["error"] == std::string("not in the native local deck yet: ") + fn, r.dump());
    }
    // ---- and what it answers now, against real files
    check("deck: transcript of a terminal tile says there is no session", h.call("transcript", json::array({ shellId }))["error"] == "no such session");
    check("deck: running.json marks a deck that is running", isFile(joinPath(state, "running.json")) && loadJson(joinPath(state, "running.json"))["pid"] == GetCurrentProcessId());
    check("deck: externalSessions / pastSessions answer lists", h.call("externalSessions")["result"].is_array() && h.call("pastSessions")["result"].is_array());
    check("deck: moveTile says why a terminal cannot move", h.call("moveTile", json::array({ shellId, "claudet" }))["error"] == "only Claude sessions can be moved");
    check("deck: a special prompt is for a Claude session", h.call("holdAdd", json::array({ shellId, { { "kind", "next" }, { "text", "x" } } }))["error"] == "only a Claude session takes a special prompt");
    { const json tr = h.call("triggerSet", json::array({ "host_test_flag", true }));
      check("deck: a trigger is set, listed in the meta and kept in triggers.json", tr["result"]["value"] == true && tr["result"]["by"] == "desk" && h.freshMeta()["triggers"][0]["name"] == "host_test_flag"
        && loadJson(joinPath(state, "triggers.json"))["triggers"]["host_test_flag"]["value"] == true, tr.dump());
      // the inbox: a file a session drops (the deck-triggers skill) clears it again
      writeText(joinPath(joinPath(state, "trigger-inbox"), "drop.json"), "{\"name\":\"host_test_flag\",\"value\":false,\"by\":\"a session\"}");
      check("deck: a file dropped in trigger-inbox sets the trigger and is removed", h.pumpUntil([&] { return h.call("triggersList")["result"][0]["value"] == false; }, 8000) && !isFile(joinPath(joinPath(state, "trigger-inbox"), "drop.json")));
      check("deck: triggerRemove", h.call("triggerRemove", json::array({ "host_test_flag" }))["result"] == true && h.call("triggersList")["result"].empty()); }
    check("deck: restartWhenQuiet arms and cancels (meta.restart)", h.call("restartWhenQuiet", json::array({ true }))["result"] == true && h.freshMeta()["restart"]["armed"] == true
      && h.call("restartWhenQuiet", json::array({ false }))["result"] == false && h.freshMeta()["restart"]["armed"] == false);
    check("deck: restartNow answers false (this shell has no relauncher)", h.call("restartNow")["result"] == false);
    check("deck: a cloud tile is refused", h.call("newTile", json::array({ { { "kind", "cloud" }, { "task", "x" }, { "account", "claudet" } } }))["t"] == "err");
    const size_t beforeFire = h.frames.size();
    h.send(deck::proto::call(0, "moveTile", json::array({ shellId, "x" })));
    h.pump(50);
    bool answered = false; for (size_t i = beforeFire; i < h.frames.size(); i++) if (h.frames[i]["t"] == "ok" || h.frames[i]["t"] == "err") answered = true;
    check("deck: rid 0 is fire-and-forget (no reply, even for a refusal)", !answered);

    // ---- a Claude tile: everything but the launch
    json bad = h.call("newTile", json::array({ { { "kind", "claude" }, { "account", "nope" } } }));
    check("deck: an unknown account is an error", bad["t"] == "err" && bad["error"] == "unknown account: nope", bad.dump());
    json cl = h.call("newTile", json::array({ { { "kind", "claude" }, { "account", "claudet" }, { "cwd", work }, { "browser", false } } }));
    claudeId = cl["result"].is_string() ? cl["result"].get<std::string>() : "";
    t = h.tile(claudeId);
    claudeSession = t["sessionId"].is_string() ? t["sessionId"].get<std::string>() : "";
    check("deck: a Claude tile exists, with a session id, and did NOT start (allowClaude is off)", cl["t"] == "ok" && t["kind"] == "claude" && t["account"] == "claudet" && claudeSession.size() == 36 && claudeSession[14] == '4'
      && t["status"] == "off" && t["exited"] == true && t["exitCode"] == -1 && t["ptySize"].is_null() && t["browser"] == false && t["sleepBlocker"] == "nothing to resume yet", t.dump());
    const size_t clAttach = h.frames.size();
    h.send(deck::proto::attach(claudeId));
    const std::string why = h.dataFor(claudeId, clAttach, true);
    check("deck: its pane says why", why.find("[claude-deck] failed to start") != std::string::npos && why.find("allowClaude") != std::string::npos, why);
    ClaudeLaunch launch = h.deck->lastClaudeLaunch(claudeId);
    json lenv = envObject(launch.env);
    check("deck: the seam built the command a real start would run", launch.file == opt.claudeExe && launch.args == std::vector<std::string>{ "--session-id", claudeSession, "--no-chrome" } && launch.cwd == work
      && lenv["CLAUDE_CONFIG_DIR"] == joinPath(home, ".claude-t") && lenv["TERM_PROGRAM"] == "claude-deck" && lenv["CLAUDE_CODE_FORCE_SESSION_PERSISTENCE"] == "1" && !lenv.contains("NO_COLOR") && !lenv.contains("CLAUDECODE"), json(launch.args).dump() + " " + lenv.dump().substr(0, 200));
    // a synthetic transcript and a live-status record appear: the meta reads them
    const std::string tr = joinPath(joinPath(joinPath(joinPath(home, ".claude-t"), "projects"), "C--fixture"), claudeSession + ".jsonl");
    writeText(tr, "{\"type\":\"ai-title\",\"aiTitle\":\"Synthetic session\"}\n"
                  "{\"type\":\"user\",\"timestamp\":\"2030-01-01T00:00:00.000Z\",\"cwd\":\"C:\\\\x\",\"message\":{\"role\":\"user\",\"content\":\"hello\"}}\n"
                  "{\"type\":\"assistant\",\"timestamp\":\"2030-01-01T00:00:05.000Z\",\"effort\":\"high\",\"message\":{\"role\":\"assistant\",\"model\":\"claude-fable-5-2\",\"content\":[{\"type\":\"text\",\"text\":\"Hi from a fixture.\"}],\"usage\":{\"input_tokens\":1000,\"cache_read_input_tokens\":49000}}}\n");
    writeText(joinPath(joinPath(joinPath(home, ".claude-t"), "sessions"), "77.json"), "{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"sessionId\":\"" + claudeSession + "\",\"status\":\"busy\",\"statusUpdatedAt\":1791792000000,\"bridgeSessionId\":\"session_fixture\",\"name\":\"n\"}");
    t = h.tile(claudeId);
    check("deck: transcript facts and the live record reach the tile's meta", t["title"] == "Synthetic session" && t["lastText"] == "Hi from a fixture." && t["model"] == "claude-fable-5-2" && t["effort"] == "high" && t["context"] == 25
      && t["bridge"] == "session_fixture" && t["lastAt"] == 1791792000000.0 && t["status"] == "off" && t["fresh"] == false && t["lastStoppedAt"] == 1893456005000.0, t.dump());
    { std::ofstream f(widen(tr).c_str(), std::ios::binary | std::ios::app); f << "{\"type\":\"custom-title\",\"customTitle\":\"renamed in claude\"}\n{\"type\":\"assis"; }
    check("deck: the transcript is read incrementally (a half-written record waits)", h.tile(claudeId)["title"] == "renamed in claude" && h.tile(claudeId)["lastText"] == "Hi from a fixture.");
    // (the live record names THIS process as busy in the session: since the same-session guard (3df1245) that reads as
    // "open in another process" and is never resumed - so it goes before the restart, as when that process ends)
    DeleteFileW(widen(joinPath(joinPath(joinPath(home, ".claude-t"), "sessions"), "77.json")).c_str());
    h.call("restartTile", json::array({ claudeId }));
    launch = h.deck->lastClaudeLaunch(claudeId);
    check("deck: with a transcript on disk the same tile would --resume", launch.args == std::vector<std::string>{ "--resume", claudeSession, "--no-chrome" } && h.tile(claudeId)["sessionId"] == claudeSession, json(launch.args).dump());
    check("deck: sleepTile on a tile with no process answers false", h.call("sleepTile", json::array({ claudeId }))["result"] == false && h.call("wakeTile", json::array({ claudeId }))["result"] == false);

    // ---- persistence. main.js writes nothing for 2 s after boot (the restore's grace): wait it out, then save.
    h.pump((int)std::max<int64_t>(0, 2300 - (tickMs() - bootAt)));
    h.call("renameTile", json::array({ shellId, "kept shell" }));
    h.send(deck::proto::layout({ { shellId, { 1, 2, 4, 5 } } }));     // (a layout sent inside the grace was not written either)
    json st = loadJson(joinPath(state, "tiles.json"));
    bool hasShell = false, hasClaude = false;
    if (st.is_object() && st.contains("tiles")) for (const json& s : st["tiles"]) { if (s["id"] == shellId && s["kind"] == "shell" && s["userTitle"] == "kept shell" && s["pinned"] == true && s["group"] == "builds" && s["cwd"] == work) hasShell = true;
      if (s["id"] == claudeId && s["kind"] == "claude" && s["sessionId"] == claudeSession && s["account"] == "claudet" && s["browser"] == false && s["title"] == "renamed in claude") hasClaude = true; }
    check("deck: tiles.json holds the tiles, groups and the window's layout", hasShell && hasClaude && st["groups"] == json::array({ "builds" }) && st["windows"][0]["layout"].contains(shellId) && st["tiles"].size() == 3, st.dump().substr(0, 600));

    // ---- close
    check("deck: closeTile answers once the process is gone and the tile is out of the meta", h.call("closeTile", json::array({ id2 }))["t"] == "ok" && h.tile(id2).is_null() && h.freshMeta()["tiles"].size() == 2);
    st = loadJson(joinPath(state, "tiles.json"));
    check("deck: a close is saved, and backed up first (it drops a tile)", st["tiles"].size() == 2 && !listDir(joinPath(state, "tiles-backups")).empty());
    h.pump(100);
    for (uint32_t p : h.deck->livePids()) h.pidsEver.insert(p);
    check("deck: one shell is still running before shutdown", !h.deck->livePids().empty());
    { size_t kids = 0; const int vis = visibleChildWindows(&kids);
      check("deck: nothing the deck started has a window on screen", vis == 0 && kids >= 2, std::to_string(vis) + " visible, " + std::to_string(kids) + " child processes"); }
    h.deck->shutdown();
    check("deck: shutdown leaves no process of any tile behind", allGone(h.pidsEver, 8000), std::to_string(h.pidsEver.size()) + " pids seen");
    everyPid.insert(h.pidsEver.begin(), h.pidsEver.end());
  }

  // ---- the same state folder, a new deck: the tiles come back
  {
    Harness h; h.open(opt);
    check("deck (restart): the hello says a restore is under way", h.frames[0]["t"] == "init" && h.frames[0]["meta"]["restoring"] == true);
    h.send(deck::proto::attach(shellId));        // a view attaches before the staggered restore has spawned it
    check("deck (restart): both tiles are restored", h.pumpUntil([&] { const json m = h.lastMeta(); return m.is_object() && m["tiles"].size() == 2; }, 8000), h.lastMeta().dump().substr(0, 300));
    json init = h.call("deskInit");
    check("deck (restart): the window's layout came back (regridded to 3 x 3 before the restart)", init["result"]["layout"].contains(shellId) && init["result"]["grid"] == json{ { "rows", 3 }, { "cols", 3 } }, init["result"]["layout"].dump());
    json t = h.tile(shellId);
    check("deck (restart): the shell tile is the same tile — id, title, pin, group, folder — with a fresh process", t["kind"] == "shell" && t["title"] == "kept shell" && t["pinned"] == true && t["group"] == "builds" && t["cwd"] == work && t["status"] == "shell" && t["exited"] == false, t.dump());
    json c = h.tile(claudeId);
    check("deck (restart): the Claude tile kept its session and is again not started", c["kind"] == "claude" && c["sessionId"] == claudeSession && c["account"] == "claudet" && c["browser"] == false && c["exited"] == true && c["title"] == "renamed in claude", c.dump());
    check("deck (restart): its restore went through the seam as a --resume", h.deck->lastClaudeLaunch(claudeId).args == std::vector<std::string>{ "--resume", claudeSession, "--no-chrome" });
    View v(h, shellId); v.at = 0;
    check("deck (restart): the early attach is carried: the new shell's prompt arrives", v.waitFor(">"), screenText(v.term));
    v.type("echo second-life\r");
    check("deck (restart): and it runs commands", v.waitFor("\nsecond-life"), screenText(v.term));
    check("deck (restart): groups and settings survived", h.lastMeta()["groups"] == json::array({ "builds" }) && h.lastMeta()["settings"]["fontSize"] == 28);
    h.pump(50);
    for (uint32_t p : h.deck->livePids()) h.pidsEver.insert(p);
    h.deck.reset();                               // the destructor is a shutdown too
    check("deck (restart): nothing left running", allGone(h.pidsEver, 8000));
    everyPid.insert(h.pidsEver.begin(), h.pidsEver.end());
  }

  // ---- parity mode (the default): main.js never saves a shell tile
  {
    LocalDeckOptions po = opt; po.persistShells = false; po.stateDir = joinPath(root, "state-parity");
    std::string sid;
    { Harness h; h.open(po);
      const int64_t bootAt = tickMs();
      h.call("deskInit");
      sid = h.call("newTile", json::array({ { { "kind", "shell" } } }))["result"].get<std::string>();
      h.pump((int)std::max<int64_t>(0, 2300 - (tickMs() - bootAt)));
      h.call("renameTile", json::array({ sid, "gone after a restart" }));
      const json st = loadJson(joinPath(po.stateDir, "tiles.json"));
      check("deck (parity): a shell tile is not written to tiles.json", st.is_object() && st["tiles"].empty(), st.dump());
      check("deck (parity): the restart strip counts it as a terminal that would come back empty", h.freshMeta()["restart"]["shells"] == 1 && h.lastMeta()["restart"]["blockers"].empty());
      for (uint32_t p : h.deck->livePids()) h.pidsEver.insert(p);
      h.deck.reset();
      everyPid.insert(h.pidsEver.begin(), h.pidsEver.end()); }
    { Harness h; h.open(po);
      h.pump(2500);
      const json m = h.call("deskInit")["result"]["meta"];
      check("deck (parity): after a restart the deck has no tiles", m["tiles"].empty() && m["restoring"] == false, m.dump().substr(0, 200));
      h.deck.reset(); }
  }
  check("deck: no process started by any deck in this test is still alive", allGone(everyPid, 8000), std::to_string(everyPid.size()) + " pids seen");
  removeTree(root);
  check("deck: the test's folder is gone", !isDir(root));
}

// ---------------------------------------------------------------- the Windows seams the core deck runs on
// (winseams.hpp, statefile.hpp, listener.hpp): each against the real machine, loopback only (127.0.0.1:8819).
static void testSeams() {
  WinProcessList procs;
  std::map<uint32_t, uint32_t> parents;
  check("seams: the process snapshot holds this process and its parent", procs.parents(parents) && parents.count(GetCurrentProcessId()) && parents[GetCurrentProcessId()] != 0 && procs.alive((double)GetCurrentProcessId()) && !procs.alive(0));
  double used = 0, limit = 0;
  check("seams: the commit charge is read (used < limit)", procs.commit(used, limit) && used > 0 && used < limit);
  std::map<uint32_t, int64_t> hosts;
  check("seams: a console host can be asked for", procs.consoleHosts({ GetCurrentProcessId() }, hosts) && hosts.count(GetCurrentProcessId()) == 1);
  WinEnv env("C:\\fake-home");
  check("seams: the environment", env.os() == "win32" && env.home() == "C:\\fake-home" && !env.get("SystemRoot").empty() && env.get("no-such-variable-here").empty() && !env.environment().empty() && env.resolvePath("C:\\a\\..\\b") == "C:\\b");
  WinRandom rnd;
  const std::string u = rnd.uuid();
  check("seams: uuid v4 and hex", u.size() == 36 && u[14] == '4' && std::string("89ab").find(u[19]) != std::string::npos && rnd.hex(8).size() == 16 && rnd.uuid() != u);
  const std::string root = tempRoot() + "-seams";
  removeTree(root);
  WinFiles& files = winFiles();
  files.makeDirs(joinPath(root, "a\\b"));
  check("seams: files — write, stat (a time near now), ranges, list, copy, remove", files.write(joinPath(root, "a\\b\\x.txt"), "0123456789") && files.stat(joinPath(root, "a\\b\\x.txt")).size == 10
    && std::abs(files.stat(joinPath(root, "a\\b\\x.txt")).mtimeMs - winClock().nowMs()) < 60000 && files.stat(joinPath(root, "a")).isDir && !files.stat(joinPath(root, "nope")).exists
    && ([&] { std::string s; return files.readRange(joinPath(root, "a\\b\\x.txt"), 3, 4, s) && s == "3456"; })() && ([&] { std::string s; uint64_t n = 0; return files.readFrom(joinPath(root, "a\\b\\x.txt"), 7, s, &n) && s == "789" && n == 10; })()
    && files.copy(joinPath(root, "a\\b\\x.txt"), joinPath(root, "a\\b\\y.txt")) && files.list(joinPath(root, "a\\b")) == std::vector<std::string>{ "x.txt", "y.txt" } && files.remove(joinPath(root, "a\\b\\y.txt")) && files.isFile(joinPath(root, "a\\b\\x.txt")));
  removeTree(root);
  // the shipping default of allowClaude: on in the app's own real run, off everywhere else unless it is named
  { const std::wstring app = L"C:\\Program Files\\Claude Deck\\" + kApp, test = L"C:\\Temp\\deck-host-test.exe";
    check("seams: Claude sessions are ON in the app's own real run (with or without --local), OFF on a scratch --state-dir (every test of the app) and in any other exe, unless --allow-claude; --no-claude is off anywhere; the last switch wins",
      shippingAllowClaude(app, {}) && shippingAllowClaude(appOtherCase(), { L"--local", L"--serve", L"8812" }) && !shippingAllowClaude(app, { L"--local", L"--state-dir", L"C:\\t" })
      && shippingAllowClaude(app, { L"--state-dir", L"C:\\t", L"--allow-claude" }) && !shippingAllowClaude(app, { L"--no-claude" }) && !shippingAllowClaude(test, {}) && !shippingAllowClaude(L"", {})
      && shippingAllowClaude(test, { L"--allow-claude" }) && shippingAllowClaude(app, { L"--no-claude", L"--allow-claude" }) && !shippingAllowClaude(app, { L"--allow-claude", L"--no-claude" }));
    check("seams: THIS exe is not the app: a LocalDeck made here starts no Claude session unless the test says so", !shippingAllowClaudeHere() && !LocalDeckOptions().allowClaude); }
  const double off = winClock().localOffsetMs(winClock().nowMs());
  check("seams: the local time offset is a whole number of quarter hours", std::fmod(std::fabs(off), 900000.0) == 0 && std::fabs(off) <= 14 * 3600000.0, std::to_string(off));

  // the listener: accept, read, write, close — over a real loopback socket
  struct Ev { uint64_t conn; int what; std::string bytes; };
  std::mutex m; std::vector<Ev> events;
  WinListener listener([&](uint64_t conn, int what, const char* b, size_t n) { std::lock_guard<std::mutex> g(m); events.push_back({ conn, what, b ? std::string(b, n) : std::string() }); });
  std::string err;
  const bool up = listener.listen("127.0.0.1", 8819, &err);
  check("listener: binds 127.0.0.1:8819", up, err);
  if (up) {
    WinListener second([](uint64_t, int, const char*, size_t) {});
    std::string err2;
    check("listener: a port already taken is refused with Node's words", !second.listen("127.0.0.1", 8819, &err2) && err2 == "listen EADDRINUSE: address already in use 127.0.0.1:8819", err2);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a; ZeroMemory(&a, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(8819); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool connected = connect(s, (const sockaddr*)&a, sizeof a) == 0;
    send(s, "hello listener", 14, 0);
    auto waitFor = [&](const std::function<bool()>& ok) { const int64_t until = tickMs() + 5000; for (;;) { { std::lock_guard<std::mutex> g(m); if (ok()) return true; } if (tickMs() > until) return false; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } };
    uint64_t conn = 0;
    const bool got = waitFor([&] { std::string all; for (const Ev& e : events) { if (e.what == 0) conn = e.conn; if (e.what == 1) all += e.bytes; } return all == "hello listener"; });
    check("listener: a connection is accepted and its bytes are handed over", connected && got && conn != 0);
    listener.send(conn, "and back");
    char buf[64]; const int n = recv(s, buf, sizeof buf, 0);
    check("listener: what is sent arrives; nothing is left queued", n == 8 && std::string(buf, 8) == "and back" && listener.buffered(conn) == 0);
    listener.stall(conn, true);
    listener.send(conn, std::string(100000, 'x'));
    check("listener: a connection that takes nothing reports what is waiting (bufferedAmount)", listener.buffered(conn) == 100000);
    listener.stall(conn, false);
    size_t total = 0; while (total < 100000) { const int k = recv(s, buf, sizeof buf, 0); if (k <= 0) break; total += (size_t)k; }
    check("listener: …and flushes it when it can", total == 100000 && waitFor([&] { return listener.buffered(conn) == 0; }));
    listener.send(conn, "last words");
    listener.close(conn, true);
    std::string tail; for (;;) { const int k = recv(s, buf, sizeof buf, 0); if (k <= 0) break; tail.append(buf, (size_t)k); }
    check("listener: close-after-flush sends what was queued, then ends the connection, and says so", tail == "last words" && waitFor([&] { for (const Ev& e : events) if (e.what == 2 && e.conn == conn) return true; return false; }));
    closesocket(s);
    listener.stop();
    SOCKET s2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check("listener: stopped, nothing listens on the port", connect(s2, (const sockaddr*)&a, sizeof a) != 0);
    closesocket(s2);
  }
  WinNet net;
  bool sane = true; for (const std::string& ip : net.ipv4Addresses()) if (ip.compare(0, 4, "127.") == 0 || ip.find('.') == std::string::npos) sane = false;
  check("net: the interface list holds IPv4 addresses and no loopback", sane);
}

// ---------------------------------------------------------------- the usage seams (winhttp.hpp), on fakes only
// WinHttpGet is asked for a page of THIS test's own loopback listener; WinCredentials reads a made-up file in a
// temp folder. Nothing here reads a real credential or leaves the machine.
static void testUsageSeams() {
  std::mutex m; std::string asked; std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{\"five_hour\":1}";
  WinListener* lp = nullptr;
  WinListener listener([&](uint64_t conn, int what, const char* b, size_t n) {
    if (what != 1) return;
    std::lock_guard<std::mutex> g(m);
    asked.append(b, n);
    if (asked.find("\r\n\r\n") != std::string::npos && lp) { lp->send(conn, answer); lp->close(conn, true); }
  });
  lp = &listener;
  std::string err;
  if (!listener.listen("127.0.0.1", 8819, &err)) { check("usage seams: a loopback listener for the fake endpoint", false, err); return; }
  std::mutex qm; std::vector<std::function<void()>> tasks;
  { WinHttpGet http([&](std::function<void()> t) { std::lock_guard<std::mutex> g(qm); tasks.push_back(std::move(t)); });
    auto run = [&](int ms) { const int64_t until = tickMs() + ms; for (;;) { std::vector<std::function<void()>> take; { std::lock_guard<std::mutex> g(qm); take.swap(tasks); } for (auto& t : take) t(); if (!take.empty()) return true; if (tickMs() > until) return false; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } };
    deck::core::HttpResponse got; bool done = false;
    http.get("http://127.0.0.1:8819/api/oauth/usage", { { "Authorization", "Bearer FAKE-TOKEN-FOR-THE-TEST" }, { "anthropic-beta", "oauth-2025-04-20" }, { "Content-Type", "application/json" } }, [&](const deck::core::HttpResponse& r) { got = r; done = true; });
    check("usage seams: the answer is not delivered on the request's thread: it waits to be run", !done);
    check("usage seams: a GET through WinHTTP: status and body", run(10000) && done && got.status == 200 && got.body == "{\"five_hour\":1}" && got.error.empty(), got.error + " " + std::to_string(got.status));
    { std::lock_guard<std::mutex> g(m);
      check("usage seams: the request carries the three headers lib/usage.js sends", asked.compare(0, 30, "GET /api/oauth/usage HTTP/1.1\r") == 0 && asked.find("Authorization: Bearer FAKE-TOKEN-FOR-THE-TEST\r\n") != std::string::npos
        && asked.find("anthropic-beta: oauth-2025-04-20\r\n") != std::string::npos && asked.find("Content-Type: application/json\r\n") != std::string::npos, asked.substr(0, 60));
      asked.clear(); answer = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; }
    done = false;
    http.get("http://127.0.0.1:8819/again", {}, [&](const deck::core::HttpResponse& r) { got = r; done = true; });
    check("usage seams: a 429 arrives with its Retry-After", run(10000) && done && got.status == 429 && got.headers.count("retry-after") && got.headers["retry-after"] == "120");
    listener.stop();
    done = false;
    http.get("http://127.0.0.1:8819/nobody-home", {}, [&](const deck::core::HttpResponse& r) { got = r; done = true; });
    check("usage seams: nobody listening is an error, not a hang", run(20000) && done && !got.error.empty()); }
  const std::string dir = tempRoot() + "-creds";
  removeTree(dir); makeDirs(dir);
  WinCredentials creds;
  check("usage seams: no credentials file: no stamp, null", creds.stamp(dir).empty() && creds.readOAuth(dir).is_null());
  writeText(joinPath(dir, ".credentials.json"), "{\"claudeAiOauth\":{\"accessToken\":\"FAKE-TOKEN-FOR-THE-TEST\",\"subscriptionType\":\"max\",\"expiresAt\":9999999999999}}");
  check("usage seams: a (made-up) credentials file is read, and stamped with its modification time", creds.readOAuth(dir)["claudeAiOauth"]["subscriptionType"] == "max" && !creds.stamp(dir).empty());
  writeText(joinPath(dir, ".credentials.json"), "not json");
  check("usage seams: a file that will not parse is null", creds.readOAuth(dir).is_null());
  removeTree(dir);
}

// ================================================================ THE TWO REAL RUNS (never part of a normal run)
// Each needs BOTH its section named on the command line AND its environment variable set; neither is run by
// build.ps1 without them, and no other code path in the repo sets them. They exist so the owner — whose word they
// need — can run them once. windows/HOST-NOTES.md "The two real runs" says what each does and what it costs.
// The Claude run WAS made on 2026-11-18, on their word, and passes (HOST-NOTES.md "The real Claude runs"); the usage
// run has not been made: it compiles, and every step it takes is one the fake-seam tests cover.

// DECK_HOST_TEST_REAL_USAGE=<config dir>   one poll of Anthropic's usage endpoint for ONE account, with the token
// in that dir's .credentials.json (read, never written; never printed). The cache goes to a temp folder.
static void testUsageReal() {
  const std::string dir = envVar(L"DECK_HOST_TEST_REAL_USAGE");
  if (dir.empty()) { std::printf("usage-real: not run (DECK_HOST_TEST_REAL_USAGE is not set)\n"); return; }
  const std::string state = tempRoot() + "-usage-real";
  removeTree(state); makeDirs(state);
  std::mutex qm; std::vector<std::function<void()>> tasks;
  WinHttpGet http([&](std::function<void()> t) { std::lock_guard<std::mutex> g(qm); tasks.push_back(std::move(t)); });
  WinCredentials creds;
  deck::core::UsagePoller poller(winClock(), winFiles(), &http, &creds, joinPath(state, "usage-cache.json"));
  deck::core::Account acc; acc.id = "account"; acc.dir = dir;
  bool done = false;
  poller.pollAll({ acc }, false, [&] { done = true; });
  const int64_t until = tickMs() + 20000;
  while (!done && tickMs() < until) { std::vector<std::function<void()>> take; { std::lock_guard<std::mutex> g(qm); take.swap(tasks); } for (auto& t : take) t(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  const json d = poller.data().value("account", json::object());
  const std::string st = d.value("state", std::string("(none)"));
  std::printf("usage-real: state %s, plan %s, session %s, week %s, requests made %d\n", st.c_str(), d.value("plan", json()).dump().c_str(), d.value("session", json()).dump().c_str(), d.value("week", json()).dump().c_str(), poller.requests());
  check("usage-real: one request at most, and an answer", done && poller.requests() <= 1);
  check("usage-real: the account read ok (a 429 reads rate-limited: stop, do not ask again)", st == "ok", st);
  std::string cache; readFile(joinPath(state, "usage-cache.json"), cache);
  const json oauth = creds.readOAuth(dir);
  const std::string tok = oauth.is_object() && oauth.contains("claudeAiOauth") ? oauth["claudeAiOauth"].value("accessToken", std::string()) : std::string();
  check("usage-real: the cache file holds usage numbers and nothing of the token", !tok.empty() && cache.find(tok) == std::string::npos && cache.find("accessToken") == std::string::npos);
  removeTree(state);
}

// DECK_HOST_TEST_REAL_CLAUDE=<account keyword>   ONE real Claude session on that account, in a fresh temp folder,
// with the deck's state in a temp folder: start (hidden), the startup prompts answered as main.js answers them,
// starting -> idle with the prompt on screen, one harmless line typed the way the deck's own prompt delivery types
// (lib/queue.js deliver: bracketed paste, Enter 150 ms later, Enter again if the session is not busy 3 s on),
// sleep (the process ends), wake (--resume), close. It spends the account's tokens on that one line and leaves no
// process behind. DECK_HOST_TEST_REAL_LOG=<file>: everything it prints goes to that file instead (UTF-8, whole).
//
// It never types into a session that is not at its prompt: a session still on a startup dialog is left alone
// (status `waiting`), the run says what is on screen and goes straight to close.
//
// The folder. Claude Code reads `.mcp.json` from the session's folder AND every folder above it, and asks about
// each server it has not been told about in THAT folder ("New MCP server found in .mcp.json") - a dialog only a
// person may answer, in the Electron deck as here (lib/startupprompts.js: kind `mcp`, the tile reads `waiting`).
// The owner's home folder has one, and %TEMP% is below the home folder: the first real run (2026-11-18) sat on
// that dialog for its two minutes and then typed its line into it. So the fresh folder is made under the first
// of %TEMP%, %PUBLIC%, %ProgramData% that has no `.mcp.json` anywhere above it.
static int64_t realT0 = 0;
static std::string realStamp() { char b[32]; std::snprintf(b, sizeof b, "[%8.3fs] ", (double)(tickMs() - realT0) / 1000.0); return b; }
static std::string escBytes(const std::string& s, size_t max = 6000) {
  std::string o;
  for (size_t i = 0; i < s.size() && i < max; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c == 0x1b) o += "\\e"; else if (c == '\r') o += "\\r"; else if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t"; else if (c == '\\') o += "\\\\";
    else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\x%02x", c); o += b; }
    else o += (char)c;
  }
  if (s.size() > max) o += " ...(+" + std::to_string(s.size() - max) + " bytes)";
  return o;
}
static bool mcpJsonAbove(std::string dir) {
  for (int i = 0; i < 64 && !dir.empty(); i++) {
    if (isFile(joinPath(dir, ".mcp.json"))) return true;
    const std::string up = dirName(dir);
    if (up.empty() || up.size() >= dir.size()) break;
    dir = up;
  }
  return false;
}
// the screen row that is Claude Code's input prompt: optional box edge, then the marker (U+276F or >) and a blank
// or the end of the row - and not a dialog's numbered option ("> 1. Yes"). -1: none.
static int promptRow(const std::string& screen) {
  int row = 0;
  for (size_t a = 0; a <= screen.size(); row++) {
    size_t nl = screen.find('\n', a); if (nl == std::string::npos) nl = screen.size();
    const std::string ln = screen.substr(a, nl - a);
    a = nl + 1;
    size_t i = 0;
    while (i < ln.size() && ln[i] == ' ') i++;
    if (ln.compare(i, 3, "\xE2\x94\x82") == 0) { i += 3; while (i < ln.size() && ln[i] == ' ') i++; }
    if (ln.compare(i, 3, "\xE2\x9D\xAF") == 0) i += 3; else if (i < ln.size() && ln[i] == '>') i += 1; else continue;
    if (i < ln.size() && ln[i] != ' ' && ln.compare(i, 2, "\xC2\xA0") != 0) continue;
    size_t j = i; while (j < ln.size() && (ln[j] == ' ' || (unsigned char)ln[j] == 0xC2 || (unsigned char)ln[j] == 0xA0)) j++;
    if (j + 1 < ln.size() && ln[j] >= '0' && ln[j] <= '9' && ln[j + 1] == '.') continue;
    return row;
  }
  return -1;
}
static bool dialogOnScreen(const std::string& screen) { return screen.find("Enter to confirm") != std::string::npos; }
static int claudeProcessCount() {
  int n = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return -1;
  PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
  if (Process32FirstW(snap, &pe)) do { if (_wcsicmp(pe.szExeFile, L"claude.exe") == 0) n++; } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
  return n;
}

static void testClaudeReal() {
  const std::string account = envVar(L"DECK_HOST_TEST_REAL_CLAUDE");
  if (account.empty()) { std::printf("claude-real: not run (DECK_HOST_TEST_REAL_CLAUDE is not set)\n"); return; }
  const std::string logFile = envVar(L"DECK_HOST_TEST_REAL_LOG");
  if (!logFile.empty()) { makeDirs(dirName(logFile)); if (!_wfreopen(widen(logFile).c_str(), L"wb", stdout)) return; }
  realT0 = tickMs();
  const int claudeBefore = claudeProcessCount();
  const std::string root = tempRoot() + "-claude-real";
  removeTree(root);
  // the session's folder: fresh, and with no .mcp.json above it (see the comment above)
  std::string work;
  { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::string t = narrow(tmp); while (!t.empty() && (t.back() == '\\' || t.back() == '/')) t.pop_back();
    for (const std::string& base : { t, envVar(L"PUBLIC"), envVar(L"ProgramData") }) {
      if (base.empty() || !isDir(base)) continue;
      const bool mcp = mcpJsonAbove(base);
      std::printf("%sfolder candidate %s: %s\n", realStamp().c_str(), base.c_str(), mcp ? "a .mcp.json at or above it - not used" : "no .mcp.json above it");
      if (mcp) continue;
      const std::string w = joinPath(base, "deck-host-test-" + std::to_string(GetCurrentProcessId()) + "-claude-real-work");
      removeTree(w);
      if (makeDirs(w)) { work = w; break; }
    } }
  check("claude-real: a fresh folder with no .mcp.json above it (a session under one waits on Claude Code's MCP server dialog, a person's to answer)", !work.empty());
  if (work.empty()) return;

  LocalDeckOptions opt;
  opt.stateDir = joinPath(root, "state");          // the deck's own state: a temp folder. Home, the alias file and claude.exe are the real ones.
  opt.allowClaude = true;
  std::vector<std::pair<uint64_t, std::string>> written;       // every byte the deck wrote to a process, in order
  opt.ptyTrace = [&](uint64_t token, const char* what, const std::string& detail) {
    if (std::string(what) == "write") written.emplace_back(token, detail);
    std::printf("%spty#%llu %s \"%s\"\n", realStamp().c_str(), (unsigned long long)token, what, escBytes(detail, 400).c_str()); std::fflush(stdout);
  };
  Harness h; h.open(opt);
  h.call("deskInit");
  const size_t frames0 = h.frames.size();
  const json made = h.call("newTile", json::array({ { { "kind", "claude" }, { "account", account }, { "cwd", work } } }));
  const std::string id = made.value("result", json()).is_string() ? made["result"].get<std::string>() : "";
  check("claude-real: a Claude tile was made", !id.empty(), made.dump());
  if (id.empty()) { h.deck->shutdown(); return; }
  View v(h, id);
  h.send(deck::proto::attach(id));
  h.send(json{ { "t", "shown" }, { "ids", json::array({ id }) }, { "hidden", false } });     // a window showing the tile, as a desk does

  // ---- what the run can say about itself
  std::string configDir;
  auto launchText = [&] {
    const ClaudeLaunch l = h.deck->lastClaudeLaunch(id);
    std::string keys; std::vector<std::string> names;
    for (const auto& kv : l.env) { names.push_back(kv.first); if (kv.first == "CLAUDE_CONFIG_DIR") configDir = kv.second; }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) keys += (keys.empty() ? "" : " ") + n;
    std::string args; for (const std::string& a : l.args) args += " [" + escBytes(a, 200) + "]";
    std::string shown;
    for (const auto& kv : l.env) for (const char* k : { "CLAUDE_CONFIG_DIR", "TERM_PROGRAM", "CLAUDE_DECK_TILE", "CLAUDE_DECK_STATE", "CLAUDE_ONCALL_KIND" }) if (kv.first == k) shown += "  " + kv.first + "=" + kv.second + "\n";
    return "launch: file " + l.file + "\n  args" + args + "\n  cwd " + l.cwd + "\n  session " + l.sessionId + "\n" + shown + "  env keys (names only, " + std::to_string(names.size()) + "): " + keys + "\n";
  };
  const std::string session = h.deck->lastClaudeLaunch(id).sessionId;
  auto sessionId = [&] { return session; };
  // the account's sessions\ folder: names only, and of the one record that is this session's, its public fields
  auto recordsText = [&] {
    const std::string dir = joinPath(configDir, "sessions");
    std::string out = "session records in " + dir + ":";
    std::string ours;
    const std::string sid = sessionId();
    for (const std::string& n : listDir(dir)) {
      if (n.size() < 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;      // the records only: never another file's name
      out += " " + n;
      std::string text; if (sid.empty() || !readFile(joinPath(dir, n), text) || text.find(sid) == std::string::npos) continue;
      const json j = json::parse(text, nullptr, false);
      if (!j.is_object() || j.value("sessionId", std::string()) != sid) continue;
      const json pid = j.value("pid", json());
      ours += "\n  OURS " + n + ": pid " + pid.dump() + (pid.is_number() ? (pidAlive((uint32_t)pid.get<double>()) ? " (alive)" : " (dead)") : "") + " status " + j.value("status", json()).dump()
        + " statusUpdatedAt " + j.value("statusUpdatedAt", json()).dump() + " startedAt " + j.value("startedAt", json()).dump() + " procStart " + j.value("procStart", json()).dump() + " waitingFor " + j.value("waitingFor", json()).dump();
    }
    return out + (ours.empty() ? "\n  (none is this session's)" : ours) + "\n";
  };
  auto transcriptFile = [&]() -> std::string {
    const std::string projects = joinPath(configDir, "projects");
    for (const std::string& slug : listDir(projects)) { const std::string f = joinPath(joinPath(projects, slug), sessionId() + ".jsonl"); if (isFile(f)) return f; }
    return std::string();
  };
  auto transcriptText = [&] {
    const std::string f = transcriptFile(); uint64_t size = 0;
    if (f.empty()) return std::string("transcript: none for ") + sessionId() + "\n";
    fileSize(f, size);
    return "transcript: " + f + " (" + std::to_string(size) + " bytes)\n";
  };
  auto pidsText = [&] { std::string s = "deck's live pids:"; for (uint32_t p : h.deck->livePids()) s += " " + std::to_string(p); return s; };
  auto dump = [&](const std::string& stage) {
    v.feed();
    std::string scr = screenText(v.term);
    while (scr.size() > 1 && scr[scr.size() - 1] == '\n' && scr[scr.size() - 2] == '\n') scr.pop_back();
    const std::string raw = h.dataFor(id, frames0);
    std::printf("\n%s==== %s\n%s%s\n%s%stile: %s\nprompt row %d, dialog on screen %s, raw output %zu bytes, its last 500: \"%s\"\n---- screen\n%s---- end of screen\n\n", realStamp().c_str(), stage.c_str(),
      launchText().c_str(), pidsText().c_str(), recordsText().c_str(), transcriptText().c_str(), h.tile(id).dump().c_str(), promptRow(scr), dialogOnScreen(scr) ? "yes" : "no", raw.size(),
      escBytes(raw.size() > 500 ? raw.substr(raw.size() - 500) : raw).c_str(), scr.c_str());
    std::fflush(stdout);
  };
  auto rcheck = [&](const std::string& name, bool ok, const std::string& detail = "") { check(name, ok, detail); if (!ok) dump("FAILED: " + name); return ok; };
  // the tile's status, read at most every 100 ms (each read is a whole collectMeta), every change printed
  std::string lastStatus; int64_t statusAt = 0;
  auto status = [&] {
    if (tickMs() - statusAt < 100 && !lastStatus.empty()) return lastStatus;
    const json t = h.tile(id);
    const std::string s = t.value("status", std::string());
    statusAt = tickMs();
    if (s != lastStatus) {
      std::printf("%sstatus %s -> %s  (waitingFor %s; %s)\n%s", realStamp().c_str(), lastStatus.empty() ? "(none)" : lastStatus.c_str(), s.c_str(), t.value("waitingFor", json()).dump().c_str(), pidsText().c_str(), recordsText().c_str());
      std::fflush(stdout);
      lastStatus = s;
    }
    return s;
  };
  auto noWindow = [&](const std::string& when) { size_t kids = 0; const int vis = visibleChildWindows(&kids); rcheck("claude-real: it shows no window (" + when + ")", vis == 0 && kids >= 1, std::to_string(vis) + " visible, " + std::to_string(kids) + " child processes"); };
  // at the prompt: idle by its own record, the prompt marker on the parsed screen, no dialog
  auto atPrompt = [&] { v.feed(); if (status() != "idle") return false; const std::string scr = screenText(v.term); return (promptRow(scr) >= 0 || scr.find("? for shortcuts") != std::string::npos) && !dialogOnScreen(scr); };

  std::printf("%s%sclaude.exe processes on the box before: %d\n", realStamp().c_str(), launchText().c_str(), claudeBefore);
  rcheck("claude-real: the process started (the tile is not 'off')", status() != "off" && !h.deck->livePids().empty());
  h.pump(1500);
  noWindow("just started");
  const bool idle = h.pumpUntil([&] { v.feed(); return status() == "idle"; }, 120000);
  rcheck("claude-real: starting -> idle (its trust prompt answered on the way, within two minutes)", idle, lastStatus);
  bool ready = idle && h.pumpUntil(atPrompt, 60000);
  if (ready) { h.pump(1500); ready = atPrompt(); if (!ready) ready = h.pumpUntil(atPrompt, 30000); }
  rcheck("claude-real: and its prompt is on the screen, no dialog over it", ready);
  noWindow("at its prompt");
  dump(ready ? "at its prompt, before typing" : "NOT at its prompt: nothing is typed");

  if (ready) {
    // lib/queue.js deliver(): bracketed paste, Enter 150 ms later; not busy 3 s on: Enter again, twice at most
    const size_t wroteBefore = written.size();
    v.type("\x1b[200~say the single word ok\x1b[201~");
    h.pump(150);
    v.type("\r");
    bool busy = false; int enters = 1;
    for (int k = 0; k < 3 && !busy; k++) {
      busy = h.pumpUntil([&] { v.feed(); const std::string s = status(); return s == "busy" || s == "waiting"; }, 3000);
      if (!busy && k < 2) { dump("not busy 3 s after Enter " + std::to_string(enters) + ": Enter again"); v.type("\r"); enters++; }
    }
    std::string typed; for (size_t i = wroteBefore; i < written.size(); i++) typed += written[i].second;
    std::printf("%styped (every byte the deck wrote since): \"%s\"; Enter sent %d time(s); busy %s\n", realStamp().c_str(), escBytes(typed).c_str(), enters, busy ? "yes" : "no");
    if (busy) dump("busy: the turn is running");
    const bool ended = busy && h.pumpUntil([&] { v.feed(); return status() == "idle"; }, 180000);
    rcheck("claude-real: one line typed; the turn ran and ended", ended, std::string(busy ? "went busy, " : "never busy, ") + lastStatus);
    h.pumpUntil([&] { return !transcriptFile().empty(); }, 5000);
    h.pump(2500);                                   // one meta tick: the tail is read
    json t = h.tile(id);
    rcheck("claude-real: the transcript gives a model and the last message", t.value("model", json()).is_string() && t.value("lastText", json()).is_string(), t.dump().substr(0, 400));
    std::printf("%smodel %s, lastText \"%s\"\n", realStamp().c_str(), t.value("model", json()).dump().c_str(), escBytes(t.value("lastText", json()).is_string() ? t["lastText"].get<std::string>() : std::string("(none)"), 300).c_str());
    dump("after the turn");

    const std::vector<uint32_t> pids = h.deck->livePids();
    const std::set<uint32_t> firstPids(pids.begin(), pids.end());
    const bool slept = h.call("sleepTile", json::array({ id }), 20000).value("result", json()) == true;
    lastStatus.clear();
    const std::string afterSleep = status();
    rcheck("claude-real: sleep: the tile reads asleep and its process is gone", slept && afterSleep == "asleep" && allGone(firstPids, 10000), afterSleep);
    std::printf("%safter the sleep: %s", realStamp().c_str(), recordsText().c_str());

    const bool woke = h.call("wakeTile", json::array({ id })).value("result", json()) == true;
    bool reuseSaid = false;
    auto wokenIdle = [&] {
      v.feed();
      if (status() != "idle") return false;
      const std::vector<uint32_t> now = h.deck->livePids();
      if (now.empty()) return false;
      if (!reuseSaid) for (uint32_t p : now) if (firstPids.count(p)) { reuseSaid = true; std::printf("%sNOTE: pid %u of the first process is a pid of the woken one too (reused)\n", realStamp().c_str(), p); }
      return true;
    };
    const bool wokeIdle = woke && h.pumpUntil(wokenIdle, 120000);
    const ClaudeLaunch l2 = h.deck->lastClaudeLaunch(id);
    rcheck("claude-real: wake: the same session comes back with --resume and reaches idle", wokeIdle && l2.args.size() >= 2 && l2.args[0] == "--resume" && l2.args[1] == session,
      std::string(woke ? "woken, " : "wakeTile refused, ") + lastStatus + ", args[0] " + (l2.args.empty() ? std::string("(none)") : l2.args[0]));
    // the resumed conversation is on screen: its last message (the line itself may have scrolled off above what
    // the owner's session-start hooks print) over the prompt
    const std::string said = t.value("lastText", json()).is_string() ? t["lastText"].get<std::string>() : std::string();
    const std::string saidLine = said.substr(0, said.find('\n'));
    const bool back = wokeIdle && h.pumpUntil(atPrompt, 60000);
    rcheck("claude-real: wake: its prompt is on the screen again, under the conversation's last message", back && !saidLine.empty() && screenText(v.term).find(saidLine) != std::string::npos, "last message \"" + escBytes(saidLine, 80) + "\"");
    noWindow("after the wake");
    dump("after the wake");
    for (uint32_t p : pids) h.pidsEver.insert(p);
  }
  for (uint32_t p : h.deck->livePids()) h.pidsEver.insert(p);
  check("claude-real: close", h.call("closeTile", json::array({ id }), 20000)["t"] == "ok" && h.tile(id).is_null());
  h.deck->shutdown();
  check("claude-real: no process it started is left", allGone(h.pidsEver, 15000), std::to_string(h.pidsEver.size()) + " pids seen");
  const int claudeAfter = claudeProcessCount();
  std::printf("%sclaude.exe processes on the box: %d before, %d after (the owner's own sessions come and go; ours are checked by pid above)\n", realStamp().c_str(), claudeBefore, claudeAfter);
  std::printf("%s%s%s", realStamp().c_str(), recordsText().c_str(), transcriptText().c_str());
  std::printf("claude-real: session %s in %s (its transcript stays in the account's config dir)\n", session.c_str(), work.c_str());
  if (!failed) { removeTree(work); removeTree(root); } else std::printf("claude-real: kept for a look: %s and %s\n", work.c_str(), root.c_str());
}

// ---------------------------------------------------------------- the 2026-11-19 ports on the real seams
// The ask feed on REAL files in a temp folder (never the owner's: the folders are named outright), the desk-open
// inbox's take by a real rename, the shipping rules, and the exit watchdog ending a process that hangs.
static std::string slurp(const std::string& path) { std::string s; winFiles().read(path, s); return s; }
static void testParityHost(const char* self) {
  // ---- the rules
  check("asks rule: the app on its own state folder has the feed", shippingAsks(L"C:\\x\\" + kApp, {}, false) && shippingAsks(appOtherCase(), { L"--local" }, false));
  check("asks rule: a scratch deck and any other exe do not, unless ASK_PAGE_STATE_DIR names a folder", !shippingAsks(kApp, { L"--state-dir", L"C:\\t" }, false) && !shippingAsks(L"deck-host-test.exe", {}, false)
    && shippingAsks(kApp, { L"--state-dir", L"C:\\t" }, true) && shippingAsks(L"deck-host-test.exe", {}, true));
  check("asks rule: --no-asks / --asks say so outright, the last one wins", !shippingAsks(kApp, { L"--no-asks" }, true) && shippingAsks(L"other.exe", { L"--asks" }, false) && !shippingAsks(L"other.exe", { L"--asks", L"--no-asks" }, false));
  check("asks rule: this test process has no feed by default", !shippingAsksHere() || !envVar(L"ASK_PAGE_STATE_DIR").empty());
  check("watchdog rule: only the app itself is ended by it", shippingExitWatchdog(L"C:\\a\\" + kApp) && !shippingExitWatchdog(L"deck-host-test.exe") && !shippingExitWatchdogHere());

  const std::string root = tempRoot() + "-parity";
  removeTree(root);
  // ---- the one place a LocalDeckOptions becomes the core's options
  { LocalDeckOptions o; o.stateDir = "C:\\s"; o.home = "C:\\h"; o.aliasFile = "C:\\h\\al"; o.version = "9.9.9"; o.askStateDir = "C:\\ask"; o.askDownloads = { "C:\\dl" }; o.cloudPolling = true; o.allowCloudCreate = true;
    o.deskOpenInbox = false; o.servePort = 8815; o.assetRoot = "C:\\assets"; o.shellFile = L"cmd.exe"; o.shellArgs = { L"/d" };
    WinEnv env(o.home);
    const DeckBuild b = buildDeckOptions(o, env);
    check("builder: every option reaches the core's (the ask folders, the cloud switches, the version, the desk inbox, usage)", b.stateDir == "C:\\s" && b.options.stateDir == "C:\\s" && b.options.home == "C:\\h" && b.options.aliasFile == "C:\\h\\al"
      && b.options.version == "9.9.9" && b.options.askStateDir == "C:\\ask" && b.options.askDownloads == std::vector<std::string>{ "C:\\dl" } && b.options.cloudPolling && b.options.allowCloudCreate && !b.options.deskOpenInbox
      && !b.options.usagePolling && !b.options.allowClaude && b.options.shellFile == "cmd.exe" && b.wantsHttp && b.server.forcePort == 8815 && b.server.assetRoot == "C:\\assets");
    LocalDeckOptions plain; plain.stateDir = "C:\\s"; plain.home = "C:\\h"; plain.asks = false;
    const DeckBuild p2 = buildDeckOptions(plain, env);
    check("builder: with the feed off no ask folder is named, and no Http is asked for", p2.options.askStateDir.empty() && !p2.wantsHttp && !p2.options.cloudPolling && !p2.options.allowCloudCreate && p2.options.deskOpenInbox);
    check("builder: no version named = the one place it is written (core/deck/version.hpp), which the resource script can include too", p2.options.version == deck::core::kNativeDeckVersion && p2.options.version == std::string(deck::core::kNativeDeckNumber) + "-native"
      && std::string(CLAUDE_DECK_NATIVE_VERSION) == deck::core::kNativeDeckNumber, p2.options.version);
    check("builder: the review of 2026-11-18: the remote-desk rule is the default, the claim follows the core's default (off: a proposal)", !p2.options.remoteDeskIsAWindow && p2.options.driverClaim == deck::core::DeckOptions().driverClaim && !p2.options.driverClaim);
    plain.remoteDeskIsAWindow = true; plain.driverClaim = true;
    const DeckBuild p3 = buildDeckOptions(plain, env);
    check("builder: ...and both reach the core when the shell sets them", p3.options.remoteDeskIsAWindow && p3.options.driverClaim); }
  WinFiles& files = winFiles();
  // ---- Files::rename: replaces, and fails when the source is gone (the inbox's "whoever renames first owns it")
  files.makeDirs(root);
  files.write(joinPath(root, "a.txt"), "one"); files.write(joinPath(root, "b.txt"), "two");
  check("seams: rename replaces the target and removes the source", files.rename(joinPath(root, "a.txt"), joinPath(root, "b.txt")) && slurp(joinPath(root, "b.txt")) == "one" && !files.isFile(joinPath(root, "a.txt")));
  check("seams: renaming a file that is gone fails", !files.rename(joinPath(root, "a.txt"), joinPath(root, "c.txt")) && !files.isFile(joinPath(root, "c.txt")));

  // ---- askLocations against a FAKE home (the skill's settings.json there), never the real one
  { const std::string home = joinPath(root, "home");
    files.makeDirs(joinPath(home, ".claude-thirdacct\\skills\\ask-page"));
    files.write(joinPath(home, ".claude-thirdacct\\skills\\ask-page\\settings.json"), "{\"state_dir\":\"~/ask-state\",\"downloads\":[\"~/dl\"]}");
    WinEnv env(home);
    if (envVar(L"ASK_PAGE_STATE_DIR").empty() && envVar(L"ASK_PAGE_DOWNLOADS").empty()) {
      const deck::core::AskLocations loc = deck::core::askLocations(files, env);
      check("asks: locations come from the skill's own settings under the home given", loc.stateDir == joinPath(home, "ask-state") && loc.downloads == std::vector<std::string>{ joinPath(home, "dl") }, loc.stateDir);
    } }

  // ---- a LocalDeck with a feed: deck.json on disk while it runs, a page in meta.asks, Done's answers file, gone at shutdown
  const std::string askDir = joinPath(root, "ask-state"), pages = joinPath(root, "ask-pages"), state = joinPath(root, "state");
  const std::string pageId = "ask_host-test_0123abcd_r1_1118-0930_beef", sid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  files.makeDirs(askDir); files.makeDirs(pages);
  files.write(joinPath(pages, pageId + ".html"), "<body>Q1?</body>");
  files.write(joinPath(askDir, sid + ".json"), json{ { "session_id", sid }, { "session_name", "host test" }, { "account", "" }, { "answers", json::object() }, { "rounds", json::array({
    { { "id", pageId }, { "round", 1 }, { "status", "open" }, { "created", "2026-11-18T09:30:00-05:00" }, { "title", "T" }, { "page", joinPath(pages, pageId + ".html") }, { "items", json::array({ { { "qid", "Q1" }, { "key", "k1" }, { "title", "first" } } }) } } }) } }.dump());
  const std::string sessionState = slurp(joinPath(askDir, sid + ".json"));
  {
    Harness h;
    LocalDeckOptions o; o.stateDir = state; o.home = joinPath(root, "home"); o.aliasFile = joinPath(root, "aliases"); o.allowClaude = false;
    o.shellFile = L"cmd.exe"; o.shellArgs = { L"/d" }; o.shellName = "cmd";
    o.askStateDir = askDir; o.askDownloads = { joinPath(root, "dl") };
    h.open(o);
    h.call("deskInit");
    const json deckFile = json::parse(slurp(joinPath(askDir, "deck.json")), nullptr, false);
    check("asks (real files): deck.json is in the folder named, with this process's pid", deckFile.is_object() && deckFile["kind"] == "claude-deck-asks" && deckFile["pid"] == GetCurrentProcessId() && deckFile["shows_pages"] == true, slurp(joinPath(askDir, "deck.json")));
    const json m = h.deck->core().collectMeta();
    check("asks (real files): the page shows in meta.asks", m["asks"].size() == 1 && m["asks"][0]["sessionId"] == sid && m["asks"][0]["rounds"][0]["id"] == pageId && m["asks"][0]["rounds"][0]["url"].is_string(), m["asks"].dump());
    check("asks (real files): the page file is what /ask/<id>.html would serve", h.deck->core().askPage(pageId) == joinPath(pages, pageId + ".html") && h.deck->core().askPage("ask_x_0123abcd_r9_1118-0930_beef").empty());
    const json done = h.call("askDone", { sid, json::array({ { { "id", pageId }, { "answers", { { "kind", "ask-page-answers" }, { "id", pageId }, { "answered", 1 }, { "total", 1 }, { "later", 0 }, { "lines", { "Q1 (first): yes" } } } } } }) });
    const std::string answers = joinPath(pages, pageId + ".answers.json");
    check("asks (real files): Done saves the answers file next to the page", done["t"] == "ok" && done["result"]["how"] == "file" && done["result"]["files"] == json::array({ answers }) && files.isFile(answers)
      && json::parse(slurp(answers), nullptr, false)["via"] == "claude-deck", done.dump());
    // the page viewer on real files
    const json shown = h.call("viewHtml", { joinPath(pages, pageId + ".html") });
    check("viewer (real files): viewHtml registers the folder and viewFile finds what is in it", shown["t"] == "ok" && shown["result"]["name"] == pageId + ".html"
      && h.deck->core().viewFile(deck::session::js::str(shown["result"]["dir"]), { pageId + ".html" }) == joinPath(pages, pageId + ".html")
      && h.deck->core().viewFile(deck::session::js::str(shown["result"]["dir"]), { pageId + ".answers.json" }).empty(), shown.dump());
    // the desk-open inbox: a real request file is taken by rename and answered (refused: this deck may not start Claude)
    const std::string inbox = joinPath(state, "desk-open"), rid = "web-20261119T010203Z-ab12";
    check("desk-open (real files): the inbox folder exists", files.isDir(inbox));
    files.write(joinPath(inbox, rid + ".request.json"), json{ { "id", rid }, { "kind", "desk-open" }, { "desk", "web" }, { "account", "C:/nowhere/.claude-x" }, { "cwd", root }, { "sessionId", sid }, { "prompt", "hello" } }.dump());
    h.pumpUntil([&] { return files.isFile(joinPath(inbox, rid + ".ack.json")); }, 4000);
    const json ack = json::parse(slurp(joinPath(inbox, rid + ".ack.json")), nullptr, false);
    check("desk-open (real files): the request is taken and answered whole", ack.is_object() && ack["id"] == rid && ack["ok"] == false && ack["error"].is_string() && !files.isFile(joinPath(inbox, rid + ".request.json")) && !files.isFile(joinPath(inbox, rid + ".taken.json"))
      && slurp(joinPath(inbox, "log.jsonl")).find(rid) != std::string::npos, slurp(joinPath(inbox, rid + ".ack.json")));
    check("cloud (real seams): making a cloud session is refused, and the option is not offered", h.call("newTile", json::array({ { { "kind", "cloud" }, { "task", "x" }, { "account", "claudet" }, { "cwd", root } } }))["error"] == "making cloud sessions is switched off in the native local deck (allowCloudCreate)"
      && m["cloudOption"] == false);
    h.deck->shutdown();
    check("asks (real files): deck.json is removed at shutdown; the session's own file is untouched", !files.isFile(joinPath(askDir, "deck.json")) && slurp(joinPath(askDir, sid + ".json")) == sessionState);
    check("watchdog: a test's deck does not arm it (this process is still here)", true);
  }
  // ---- the exit watchdog: a process that hangs after arming it is ended, with what it started, exit code 0
  {
    wchar_t exe[32768]; GetModuleFileNameW(nullptr, exe, 32768);
    (void)self;
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" x watchdog-child";
    STARTUPINFOW si; ZeroMemory(&si, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
    const int64_t t0 = tickMs();
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD code = 99; DWORD waited = WAIT_FAILED;
    std::vector<uint32_t> kids;
    if (ok) {
      Sleep(250); kids = descendantsOf(pi.dwProcessId);
      waited = WaitForSingleObject(pi.hProcess, 10000); GetExitCodeProcess(pi.hProcess, &code);
      if (waited != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 3);
      CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    const int64_t took = tickMs() - t0;
    Sleep(200);
    bool kidsGone = true; WinProcessList procs; for (uint32_t k : kids) if (procs.alive((double)k)) kidsGone = false;
    check("watchdog: a process that hangs after its deck began to quit is ended at its time, exit code 0", ok && waited == WAIT_OBJECT_0 && code == 0 && took >= 600 && took < 6000, "waited " + std::to_string(waited) + " code " + std::to_string(code) + " took " + std::to_string(took));
    check("watchdog: … and so is the process it had started", !kids.empty() && kidsGone, std::to_string(kids.size()) + " children");
  }
  if (!failed) removeTree(root); else std::printf("parity: kept for a look: %s\n", root.c_str());
}
// `deck-host-test x watchdog-child`: start a hidden child (so there is something to clean up), arm the watchdog
// for 700 ms and hang. The parent test waits for this process to be ended.
static int watchdogChild() {
  wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring cmd = std::wstring(L"\"") + sys + L"\\ping.exe\" -n 60 127.0.0.1";
  STARTUPINFOW si; ZeroMemory(&si, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  armProcessExitWatchdog(700);
  armProcessExitWatchdog(60000);            // a second arming changes nothing
  for (;;) Sleep(1000);
  return 7;
}

int main(int argc, char** argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "core/test/fixtures/session";
  std::string only = argc > 2 ? argv[2] : "";
  if (only == "watchdog-child") return watchdogChild();
  if (only.empty() || only == "conpty") testConPty();
  if (only.empty() || only == "rules") testRules(fixtures);
  if (only.empty() || only == "seams") { testSeams(); testUsageSeams(); }
  if (only == "usage-real") testUsageReal();          // never without its name AND DECK_HOST_TEST_REAL_USAGE
  if (only == "claude-real") testClaudeReal();        // never without its name AND DECK_HOST_TEST_REAL_CLAUDE
  if (only.empty() || only == "deck") testDeck();
  if (only.empty() || only == "parity") testParityHost(argv[0]);
  std::printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
