// core/test/deck/deck_scenarios.cpp — the deck (core/deck/deck.hpp) run on fake seams: a clock that moves only
// when told, files in memory, processes that are a record of what was asked of them. What windows/host-test
// checks against real pseudoconsoles is checked here against the fakes, and then what a single shell window
// cannot show: several clients at once — two windows, a phone and a desk.
#include "t.hpp"
#include "fakes.hpp"
#include "rig.hpp"
#include "../../deck/deck.hpp"
#include "../../deck/startup.hpp"
#include "../../deck/auditors.hpp"
#include "../../deck/edition.hpp"
#include "../../deck/pause.hpp"
#include "../../deck/remoterule.hpp"
#include "../../session/jsutil.hpp"

using namespace deck::core;
namespace js = deck::session::js;

namespace {


// ---------------------------------------------------------------- one window: what windows/host-test checks on real pseudoconsoles
void testOneWindow() {
  Rig r; r.opt.persistShells = true;
  r.make();
  const ClientId w = r.deck->addWindow();
  r.deck->start();
  check("deck: start sends the hello to the window", !r.out[w].empty() && r.out[w][0]["t"] == "init" && r.out[w][0]["home"] == P("C:\\home") && r.out[w][0]["platform"]["shellName"] == "cmd" && r.out[w][0]["platform"]["os"] == fake::osName() && r.out[w][0]["meta"]["tiles"].empty());
  check("deck: settings.json is written on first run, with a 16-character token", r.files.getJson(P("C:\\state\\settings.json"))["phone"]["token"] == "abababababababab" && r.files.atomicWrites >= 1);
  r.send(w, { { "t", "layout" }, { "layout", { { "ghost", { 1 } } } } });       // a window message before deskInit is ignored
  const json init = r.call(w, "deskInit");
  check("deck: deskInit answers { home, meta, platform, layout, windowIndex, grid }", init["t"] == "ok" && init["result"]["home"] == P("C:\\home") && init["result"]["windowIndex"] == 0 && init["result"]["grid"] == json{ { "rows", 2 }, { "cols", 2 } }
    && init["result"]["layout"] == json::object() && init["result"]["platform"]["keyHelp"]["next"] == (fake::posix() ? "Cmd+Shift+]" : "F12"), init.dump());
  const json m0 = init["result"]["meta"];
  check("deck: the snapshot's shape", m0["accounts"][0] == json{ { "id", "claudet" }, { "label", "t@example.com" }, { "abbr", nullptr }, { "email", "t@example.com" }, { "key", "t@example.com" }, { "isDefault", false } }
    && m0["accounts"].size() == 2 && m0["settings"]["driver"] == "auto" && m0["restart"]["armed"] == false && m0["restart"]["update"].is_null() && m0["triggers"].empty() && m0["usage"] == json::object()
    && m0["cloudOption"] == false && m0["browserOption"] == true && m0["restoring"] == true, m0.dump());
  r.advance(2100);
  check("deck: the two seconds after boot are over", r.fresh()["restoring"] == false);

  // ---- a shell tile
  const std::string id = r.shell(w, P("C:\\work dir"));
  fake::Pty* p = r.ptys.last();
  check("deck: newTile({ kind: 'shell' }) starts the shell command in the folder asked for, at 120 x 30", !id.empty() && p && p->spec.file == "cmd.exe" && p->spec.args == std::vector<std::string>{ "/d" } && p->spec.cwd == P("C:\\work dir") && p->spec.cols == 120 && p->spec.rows == 30);
  check("deck: its environment is the parent's without a Claude session's markers", p && noEnv(p->spec.env, "CLAUDECODE") && noEnv(p->spec.env, "CLAUDE_CODE_ENTRYPOINT") && noEnv(p->spec.env, "AI_AGENT") && noEnv(p->spec.env, "NO_COLOR")
    && hasEnv(p->spec.env, "TERM_PROGRAM", "claude-deck") && hasEnv(p->spec.env, "COLORTERM", "truecolor") && hasEnv(p->spec.env, "CLAUDE_CODE_FORCE_SESSION_PERSISTENCE", "1") && hasEnv(p->spec.env, "Path", P("C:\\Windows")) && noEnv(p->spec.env, "CLAUDE_CONFIG_DIR"));
  json t = r.tile(id);
  check("deck: the tile is in the meta as a running shell", t["kind"] == "shell" && t["status"] == "shell" && t["ptySize"] == json{ { "cols", 120 }, { "rows", 30 } } && t["account"].is_null() && t["sessionId"].is_null() && t["exited"] == false
    && t["cwd"] == P("C:\\work dir") && t["fresh"] == true && t["shownBy"].empty() && t["inWindow"].is_null() && t["sleepBlocker"] == "not a resumable session" && t["held"].empty(), t.dump());
  r.feed(p, "early ");
  check("deck: output is not sent to a window that has not attached the tile", r.count(w, "data", id) == 0);
  r.send(w, { { "t", "attach" }, { "id", id } });
  check("deck: attach answers with the replay, marked", r.data(w, id, 0, true) == "early ");
  r.feed(p, "live");
  check("deck: then output arrives as it is written", r.data(w, id) == "early live" && r.lastOf(w, "data").value("replay", false) == false);
  { const size_t at = r.out[w].size(); r.feed(p, "caf\xC3"); const size_t mid = r.count(w, "data", id, at); r.feed(p, "\xA9!");
    check("deck: a character split across two reads is sent whole", r.data(w, id, at) == "caf\xC3\xA9!" && mid == 1 && r.out[w][at]["d"] == "caf"); }
  r.send(w, { { "t", "call" }, { "rid", 0 }, { "fn", "userInput" }, { "args", json::array({ id }) } });
  r.send(w, { { "t", "input" }, { "id", id }, { "d", "dir\r" }, { "kind", "key" } });
  check("deck: input reaches the process", p->writes == std::vector<std::string>{ "dir\r" });
  t = r.tile(id);
  check("deck: a key press marks the driver; the tile is no longer fresh; rid 0 got no reply", t["driver"] == "desktop" && t["driverAt"].is_number() && t["fresh"] == false && r.count(w, "ok") == 2);

  // ---- shown / layout
  r.send(w, { { "t", "shown" }, { "ids", { id, "no-such-tile" } }, { "hidden", false } });
  r.send(w, { { "t", "layout" }, { "layout", { { id, { 1, 2, 3, 4 } }, { "diagonal", { 1, 4 } } } } });
  t = r.tile(id);
  check("deck: shown + layout -> shownBy win:0, inWindow 0", t["shownBy"] == json::array({ "win:0" }) && t["inWindow"] == 0 && t["sleepBlocker"] == "not a resumable session", t.dump());
  check("deck: an illegal block is not kept; the layout is saved with the window", r.res(w, "deskInit")["layout"] == json{ { id, { 1, 2, 3, 4 } } } && r.tilesFile()["windows"][0]["layout"] == json{ { id, { 1, 2, 3, 4 } } }, r.tilesFile().dump());
  r.send(w, { { "t", "shown" }, { "ids", { id } }, { "hidden", true } });
  check("deck: a hidden window shows nothing", r.tile(id)["shownBy"].empty());
  r.deck->windowState(w, false, false);
  r.send(w, { { "t", "shown" }, { "ids", { id } }, { "hidden", false } });
  check("deck: nor does a minimised one (the shell's word)", r.tile(id)["shownBy"].empty());
  r.deck->windowState(w, true, true);
  check("deck: back on screen, it shows it again", r.tile(id)["shownBy"] == json::array({ "win:0" }));

  // ---- resize (150 ms of quiet), nudge
  r.send(w, { { "t", "resize" }, { "id", id }, { "cols", 90 }, { "rows", 29 } });
  r.advance(100);
  r.send(w, { { "t", "resize" }, { "id", id }, { "cols", 97 }, { "rows", 31 } });
  r.advance(149);
  check("deck: a resize is not applied before its 150 ms of quiet", r.deck->ptySize(id) == std::make_pair(120, 30));
  r.advance(1);
  check("deck: the last size of a burst is applied, once", r.deck->ptySize(id) == std::make_pair(97, 31) && p->resizes.size() == 1 && r.tile(id)["ptySize"] == json{ { "cols", 97 }, { "rows", 31 } });
  r.send(w, { { "t", "resize" }, { "id", id }, { "cols", 97 }, { "rows", 31 } });
  r.send(w, { { "t", "resize" }, { "id", id }, { "cols", 1 }, { "rows", 1 } });
  r.advance(400);
  check("deck: the same size again, and 1 x 1, change nothing", p->resizes.size() == 1);
  r.send(w, { { "t", "call" }, { "rid", 0 }, { "fn", "nudge" }, { "args", { id, 97, 31 } } });
  check("deck: nudge narrows by one column at once", r.deck->ptySize(id) == std::make_pair(96, 31));
  r.advance(100);
  check("deck: and comes back 100 ms later", r.deck->ptySize(id) == std::make_pair(97, 31) && p->resizes.size() == 3);
  r.feed(p, "after-the-resize");
  r.send(w, { { "t", "detach" }, { "id", id } });
  { const size_t at = r.out[w].size(); r.feed(p, "|while-detached");
    check("deck: a detached tile's output is not sent", r.count(w, "data", id, at) == 0);
    r.send(w, { { "t", "attach" }, { "id", id } });
    check("deck: re-attach replays the whole window", r.data(w, id, at, true) == "early livecaf\xC3\xA9!after-the-resize|while-detached");
    const size_t at2 = r.out[w].size();
    r.send(w, { { "t", "attach" }, { "id", id }, { "since", true } });
    check("deck: attach { since } replays only what was written since the last resize", r.data(w, id, at2, true) == "after-the-resize|while-detached"); }
  { const size_t at = r.out[w].size(); r.feed(p, "\x1b[?1049h\x1b[?1000h\x1b[?1006hTUI");
    r.send(w, { { "t", "attach" }, { "id", id }, { "since", true } });
    const std::string rep = r.data(w, id, at, true);
    check("deck: the screen modes are tracked and replayed ahead of the window", rep.compare(0, 15, "\x1b[?1049h\x1b[2J\x1b[H") == 0 && r.tile(id)["altScreen"] == true && r.tile(id)["mouseOn"] == true && r.tile(id)["sgrMouse"] == true, rep.substr(0, 40)); }

  // ---- names, pins, groups, order; what is saved
  check("deck: renameTile", r.call(w, "renameTile", { id, "  build shell  " })["t"] == "ok" && r.tile(id)["title"] == "build shell");
  check("deck: pin answers the new state", r.res(w, "pin", { id, true }) == true && r.tile(id)["pinned"] == true);
  check("deck: setGroup adds the group", r.call(w, "setGroup", { id, " ops " })["t"] == "ok" && r.tile(id)["group"] == "ops" && r.fresh()["groups"] == json::array({ "ops" }));
  check("deck: setGroups answers the cleaned list", r.res(w, "setGroups", json::array({ json::array({ "ops", " builds ", "ops", "" }) })) == json::array({ "ops", "builds" }));
  const std::string id2 = r.shell(w);
  fake::Pty* p2 = r.ptys.last();
  check("deck: a tile with no folder named starts in the home folder", p2->spec.cwd == P("C:\\home"));
  check("deck: pinned tiles come first in the snapshot", r.fresh()["tiles"].size() == 2 && r.fresh()["tiles"][0]["id"] == id);
  r.call(w, "pin", { id2, true });
  check("deck: reorder sets the pinned order", r.call(w, "reorder", json::array({ json::array({ id2, id }) }))["t"] == "ok" && r.fresh()["tiles"][0]["id"] == id2);
  r.call(w, "pin", { id2, false });
  { const json saved = r.tilesFile();
    check("deck (persistShells): a shell tile is saved with its folder, pin, group and title", saved["tiles"].size() == 2 && saved["tiles"][0]["id"] == id && saved["tiles"][0]["kind"] == "shell" && saved["tiles"][0]["cwd"] == P("C:\\work dir")
      && saved["tiles"][0]["pinned"] == true && saved["tiles"][0]["group"] == "ops" && saved["tiles"][0]["userTitle"] == "build shell" && saved["groups"] == json::array({ "ops", "builds" }), saved.dump()); }

  // ---- a process that ends by itself, restart, close
  r.ptys.exits.emplace_back(p2->token, 5);
  r.pump();
  t = r.tile(id2);
  check("deck: an exit is told to the window, and the tile reads off / exited / its code", r.lastOf(w, "exit") == json{ { "t", "exit" }, { "id", id2 }, { "exitCode", 5 } } && t["status"] == "off" && t["exited"] == true && t["exitCode"] == 5 && t["ptySize"].is_null(), t.dump());
  { const size_t at = r.out[w].size();
    r.call(w, "restartTile", { id2 });
    check("deck: restartTile clears the screen and starts the same kind again", r.count(w, "reset", id2, at) == 1 && r.ptys.last() != nullptr && r.ptys.last()->spec.file == "cmd.exe" && r.tile(id2)["status"] == "shell" && r.tile(id2)["exited"] == false); }
  p2 = r.ptys.last();
  r.ptys.exitOnKill = false;
  { const size_t at = r.out[w].size();
    r.send(w, { { "t", "call" }, { "rid", 900 }, { "fn", "closeTile" }, { "args", { id2 } } });
    check("deck: closeTile kills a shell at once, and answers only when it is gone", p2->killed && r.count(w, "ok", "", at) == 0);
    r.ptys.exits.emplace_back(p2->token, 1); r.pump();
    check("deck: …then the answer, and the tile is gone", r.count(w, "ok", "", at) == 1 && r.tile(id2).is_null() && r.tilesFile()["tiles"].size() == 1); }
  { const std::string id3 = r.shell(w); fake::Pty* p3 = r.ptys.last(); const size_t at = r.out[w].size();
    r.send(w, { { "t", "call" }, { "rid", 901 }, { "fn", "closeTile" }, { "args", { id3 } } });
    r.advance(6499);
    const size_t before = r.count(w, "ok", "", at);
    r.advance(1);
    check("deck: a process that never reports its exit stops being waited for after the grace + 4 s", p3->killed && before == 0 && r.count(w, "ok", "", at) == 1 && r.tile(id3).is_null()); }
  r.ptys.exitOnKill = true;
  r.ptys.failNext = "File not found: cmd.exe";
  { const std::string bad = r.shell(w);
    r.send(w, { { "t", "attach" }, { "id", bad } });
    check("deck: a start that fails leaves a tile that says so", r.tile(bad)["exited"] == true && r.tile(bad)["exitCode"] == -1 && r.data(w, bad, 0, true) == "\r\n[claude-deck] failed to start cmd.exe: File not found: cmd.exe\r\n", r.data(w, bad, 0, true));
    check("log: a start that fails is one line of the deck's log", r.log.count("[spawn] tile " + bad.substr(0, 8) + " (shell): failed to start cmd.exe: File not found: cmd.exe") == 1, r.log.all());
    r.call(w, "closeTile", { bad }); }
  check("deck: an action on a tile that does not exist is harmless", r.call(w, "closeTile", { "nope" })["t"] == "ok" && r.call(w, "pin", { "nope", true })["result"] == false && r.call(w, "wakeTile", { "nope" })["result"] == false);

  // ---- settings
  { const size_t at = r.out[w].size();
    const json saved = r.res(w, "saveSettings", json::array({ { { "fontSize", 99 }, { "grid", { { "rows", 2 }, { "cols", 3 } } }, { "aliases", { { "t@example.com", "TT" } } }, { "phone", { { "port", 8815 } } } } }));
    check("deck: saveSettings validates, saves, and answers the settings", saved["fontSize"] == 28 && saved["grid"]["cols"] == 3 && saved["accounts"][0]["label"] == "TT" && saved["phone"]["port"] == 8815 && saved["phoneStatus"]["running"] == false
      && r.files.getJson(P("C:\\state\\settings.json"))["fontSize"] == 28, saved.dump().substr(0, 300));
    const auto ev = r.events(w, "grid:changed", at);
    check("deck: a grid change tells the window its remapped layout", ev.size() == 1 && ev[0]["rows"] == 2 && ev[0]["cols"] == 3 && ev[0]["layout"] == json{ { id, { 1, 2, 4, 5 } } }, ev.empty() ? "" : ev[0].dump()); }
  { const json got = r.res(w, "getSettings");
    check("deck: getSettings is the desktop shape", got["aliasFile"] == ALIAS && got["accountRows"][0]["keywords"] == json::array({ "claudet" }) && got["userswitch"] == P("C:\\home\\.local\\bin\\userswitch") && got["bash"] == P("C:\\Git\\bin\\bash.exe")
      && got["phoneStatus"]["running"] == false && got["phone"]["token"] == "abababababababab", got.dump().substr(0, 300)); }

  // ---- the driver setting says the phone: this window is read-only
  check("deck: setDriver", r.res(w, "setDriver", { "phone" }) == "phone" && r.res(w, "setDriver", { "nonsense" }) == "phone");
  { const size_t at = r.out[w].size(); const size_t writes = p->writes.size();
    r.send(w, { { "t", "input" }, { "id", id }, { "d", "should-not-run\r" } });
    r.send(w, { { "t", "input" }, { "id", id }, { "d", "again" } });
    r.send(w, { { "t", "input" }, { "id", id }, { "d", "\x1b[I" } });
    r.send(w, { { "t", "resize" }, { "id", id }, { "cols", 50 }, { "rows", 20 } });
    r.advance(400);
    const auto ev = r.events(w, "input:refused", at);
    check("deck: under driver 'phone' the window's input and size are refused, and it is told once", ev.size() == 1 && ev[0] == json{ { "id", id }, { "reason", "phone" } } && p->writes.size() == writes && r.deck->ptySize(id) == std::make_pair(97, 31));
    r.advance(3000);
    r.send(w, { { "t", "input" }, { "id", id }, { "d", "later" } });
    check("deck: …and again three seconds later", r.events(w, "input:refused", at).size() == 2); }
  r.call(w, "setDriver", { "auto" });

  // ---- refusals
  for (const char* fn : { "saveAccounts", "refreshUsage", "not-a-call" }) {
    const json a = r.call(w, fn, { id });
    check(std::string("deck: ") + fn + " is refused by name", a["t"] == "err" && a["error"] == std::string("not in the native local deck yet: ") + fn, a.dump());
  }
  check("deck: transcript of a tile that is not a session", r.call(w, "transcript", { id })["error"] == "no such session");
  check("deck: a cloud tile is refused", r.call(w, "newTile", json::array({ { { "kind", "cloud" }, { "task", "x" }, { "account", "claudet" } } }))["t"] == "err");

  // ---- shutdown: every process is killed
  const size_t live = r.ptys.live.size();
  r.deck->shutdown();
  check("deck: shutdown kills what is running and nothing answers after it", live >= 1 && r.ptys.live.empty() && (r.send(w, { { "t", "call" }, { "rid", 5 }, { "fn", "deskInit" } }), r.lastOf(w, "ok")["rid"] != 5));
}

// ---------------------------------------------------------------- what survives a restart
void testRestart() {
  Rig a; a.opt.persistShells = true;
  const ClientId w = a.boot();
  const std::string s1 = a.shell(w, P("C:\\one")), s2 = a.shell(w, P("C:\\two")), s3 = a.shell(w, P("C:\\three"));
  a.call(w, "pin", { s2, true }); a.call(w, "renameTile", { s3, "third" });
  a.send(w, { { "t", "layout" }, { "layout", { { s1, { 1, 2 } } } } });
  a.deck->windowBounds(w, { { "x", 10 }, { "y", 20 }, { "width", 900 }, { "height", 600 } });
  a.deck->shutdown();

  Rig b; b.opt.persistShells = true; b.files.files = a.files.files; b.clock.t = a.clock.t + 60000;
  b.make();
  const ClientId w2 = b.deck->addWindow({ { "x", 1 }, { "y", 1 }, { "width", 5 }, { "height", 5 } });
  b.deck->start();
  check("restart: nothing is spawned synchronously at boot, and the snapshot says it is restoring", b.ptys.spawned.empty() && b.out[w2][0]["meta"]["restoring"] == true && b.out[w2][0]["meta"]["tiles"].empty());
  const json init = b.call(w2, "deskInit");
  check("restart: the window takes its saved layout", init["result"]["layout"] == json{ { s1, { 1, 2 } } } && b.deck->savedWindows()[0]["bounds"]["width"] == 900, init["result"]["layout"].dump());
  b.advance(0);
  check("restart: the first tile comes back at once…", b.ptys.spawned.size() == 1);
  b.advance(399);
  const size_t early = b.ptys.spawned.size();
  b.advance(1);
  check("restart: …and the rest 400 ms apart", early == 1 && b.ptys.spawned.size() == 2);
  b.advance(400);
  check("restart: all three are back, pinned first, with their folders, ids and titles", b.ptys.spawned.size() == 3 && b.fresh()["tiles"].size() == 3 && b.fresh()["tiles"][0]["id"] == s2
    && b.ptys.spawned[0].cwd == P("C:\\two") && b.tile(s3)["title"] == "third" && b.tile(s1)["cwd"] == P("C:\\one"), b.fresh()["tiles"].dump().substr(0, 300));
  check("restart: still restoring for two seconds after the last spawn; then not", b.fresh()["restoring"] == true && (b.advance(2000), b.fresh()["restoring"] == false));
  check("restart: a backup of tiles.json was taken at boot", !b.files.list(P("C:\\state\\tiles-backups")).empty());
  b.deck->shutdown();

  // parity with main.js: a terminal tile is never in tiles.json
  Rig c;
  const ClientId w3 = c.boot();
  c.shell(w3);
  check("parity: without persistShells a shell tile is not saved (main.js saves Claude and cloud tiles only)", c.tilesFile()["tiles"].empty());
  c.deck->shutdown();

  // a tiles.json that will not parse is never read as "no tiles"
  Rig d; d.opt.persistShells = true; d.files.files = b.files.files; d.clock.t = b.clock.t + 60000;
  d.files.put(P("C:\\state\\tiles.json"), std::string("\0\0\0\0", 4));
  const ClientId w4 = d.boot();
  d.advance(1500);
  bool kept = false; for (const std::string& n : d.files.list(P("C:\\state"))) if (n.compare(0, 22, "tiles.json.unreadable-") == 0) kept = true;
  check("statefile: an unreadable tiles.json is kept aside and the newest good backup restores the tiles", kept && d.fresh()["tiles"].size() == 3, d.fresh()["tiles"].dump().substr(0, 200));
  (void)w4;
  d.deck->shutdown();
}

// ---------------------------------------------------------------- Claude tiles: the seam, the launch, its startup dialogs
void testClaude(const std::string& fx) {
  // ---- allowClaude off (the default): everything but the launch
  { Rig r; const ClientId w = r.boot();
    const std::string id = str(r.res(w, "newTile", json::array({ { { "kind", "claude" }, { "account", "claudet" }, { "cwd", P("C:\\work") }, { "browser", false } } })));
    const ClaudeLaunch l = r.deck->lastClaudeLaunch(id);
    json t = r.tile(id);
    check("claude (off): no process is started; the tile reads failed to start", r.ptys.spawned.empty() && t["kind"] == "claude" && t["account"] == "claudet" && t["status"] == "off" && t["exited"] == true && t["exitCode"] == -1 && t["browser"] == false, t.dump());
    check("claude (off): the launch the seam built: claude.exe --session-id <new id> --no-chrome, in the account's config dir", l.file == P("C:\\bin\\claude.exe") && l.args == std::vector<std::string>{ "--session-id", str(t["sessionId"]), "--no-chrome" }
      && l.cwd == P("C:\\work") && hasEnv(l.env, "CLAUDE_CONFIG_DIR", T_DIR) && noEnv(l.env, "CLAUDECODE") && hasEnv(l.env, "TERM_PROGRAM", "claude-deck"));
    r.send(w, { { "t", "attach" }, { "id", id } });
    check("claude (off): its pane says why", r.data(w, id).find("starting Claude sessions is switched off in the native local deck (allowClaude)") != std::string::npos);
    check("claude (off): the tile is saved", r.tilesFile()["tiles"][0]["sessionId"] == t["sessionId"] && r.tilesFile()["tiles"][0]["browser"] == false);
    r.transcript(T_DIR, str(t["sessionId"]), "x", "y");
    r.call(w, "refreshTile", { id, { { "browser", true } } }, 100);
    check("claude (off): once a transcript exists the same tile resumes; the browser choice is kept", r.deck->lastClaudeLaunch(id).args == std::vector<std::string>{ "--resume", str(t["sessionId"]), "--chrome" } && r.tile(id)["browser"] == true);
    { const std::string fresh2 = str(r.res(w, "newTile", json::array({ { { "kind", "claude" }, { "account", "claudet" } } })));
      check("claude (off): a session that never got a message has no transcript to page", r.call(w, "transcript", { fresh2 })["error"] == "no transcript on disk for this session yet"); }
    check("claude (off): an unknown account is refused", r.call(w, "newTile", json::array({ { { "kind", "claude" }, { "account", "nope" } } }))["error"] == "unknown account: nope");
    check("claude (off): adopting a session elsewhere is refused before anything is killed", r.call(w, "adoptSession", json::array({ { { "sessionId", "s-x" }, { "account", "claudet" }, { "pid", 4242 } } }))["error"].get<std::string>().find("allowClaude") != std::string::npos && r.procs.killedTrees.empty());
    r.deck->shutdown(); }

  // ---- allowClaude on, against the fake process seam
  Rig r; r.opt.allowClaude = true;
  const ClientId w = r.boot();
  const std::string id = r.claude(w);
  fake::Pty* p = r.ptys.last();
  json t = r.tile(id);
  const std::string sid = str(t["sessionId"]);
  check("claude: the process is the launch the seam built", p && p->spec.file == P("C:\\bin\\claude.exe") && p->spec.args == std::vector<std::string>{ "--session-id", sid } && p->spec.cwd == P("C:\\work") && hasEnv(p->spec.env, "CLAUDE_CONFIG_DIR", T_DIR));
  check("claude: starting until its own record says otherwise", t["status"] == "starting" && t["fresh"] == true && t["sleepBlocker"] == "nothing to resume yet", t.dump());

  // the trust prompt: nudge a column to force a full repaint, read the marker off it, Enter only on "Yes"
  r.feed(p, "\x1b[2J Quick safety check: Is this a project you created or one you trust?\r\n");
  r.advance(699);
  check("trust: nothing happens before 700 ms", p->resizes.empty() && p->writes.empty());
  r.advance(1);
  check("trust: then the pty is nudged one column", p->resizes == std::vector<std::pair<int, int>>{ { 119, 30 } });
  r.feed(p, "\r\n   Yes, I trust this folder\r\n \xE2\x9D\xAF No, exit\r\n Enter to confirm\r\n");
  r.advance(800);
  check("trust: the marker is on \"No\": Down, never Enter", p->writes == std::vector<std::string>{ "\x1b[B" });
  r.advance(800);
  check("trust: the next cycle nudges again (main.js narrows while the pty is over 100 columns)", p->resizes.size() == 2 && p->resizes[1] == std::make_pair(118, 30));
  r.feed(p, "\r\n \xE2\x9D\xAF Yes, I trust this folder\r\n   No, exit\r\n Enter to confirm\r\n");
  r.advance(800);
  check("trust: the marker was SEEN on \"Yes\": Enter", p->writes == std::vector<std::string>{ "\x1b[B", "\r" });
  r.advance(1800);
  r.advance(800);
  check("trust: the prompt is gone from the next repaint: the watch ends, nothing more is typed", p->writes.size() == 2 && (r.advance(5000), p->writes.size() == 2));
  check("trust: the deck's own answer is not the user's: the tile is still fresh", r.tile(id)["fresh"] == true && r.tile(id)["driver"].is_null());

  // the session is up
  r.record(T_DIR, p->pid(), sid, "idle");
  r.transcript(T_DIR, sid, "Worklist export", "All done.\nSecond line.");
  r.advance(2000);
  t = r.tile(id);
  check("claude: its record makes it idle; the transcript gives title, model, effort, context", t["status"] == "idle" && t["title"] == "Worklist export" && t["model"] == "claude-fable-5-1" && t["effort"] == "high" && t["lastText"] == "All done.\nSecond line."
    && t["context"] == 50 && t["fresh"] == false, t.dump());
  r.call(w, "pin", { id, false });
  check("claude: the title is saved for the next boot (with the next save)", r.tilesFile()["tiles"][0]["title"] == "Worklist export");

  { const ClientId ph = r.deck->addRemote();
    const json page = r.call(ph, "transcript", { id, { { "limit", 5 } } });
    check("transcript: the phone's text view reads the conversation off disk, paged", page["result"]["total"] == 2 && page["result"]["entries"][0] == json{ { "role", "user" }, { "at", "2027-01-15T08:00:00.000Z" }, { "text", "hello" }, { "i", 0 } }
      && page["result"]["entries"][1]["role"] == "assistant" && page["result"]["entries"][1]["model"] == "claude-fable-5-1" && page["result"]["entries"][1]["text"] == "All done.\nSecond line.", page.dump());
    r.deck->removeClient(ph); }

  // toasts: a turn that ran long enough, while no window has the focus
  r.record(T_DIR, p->pid(), sid, "busy"); r.advance(2000);
  check("claude: busy", r.tile(id)["status"] == "busy");
  r.advance(30000);
  r.record(T_DIR, p->pid(), sid, "idle"); r.advance(2000);
  check("toast: done after a long turn, with the account, the title and the last message", r.notes.toasts.size() == 1 && r.notes.toasts[0].title == "t@example.com \xC2\xB7 Worklist export" && r.notes.toasts[0].body.compare(0, 11, "done after ") == 0
    && r.notes.toasts[0].body.find(": All done. Second line.") != std::string::npos && r.notes.toasts[0].tileId == id && r.notes.flashes.size() == 1, r.notes.toasts.empty() ? "" : r.notes.toasts[0].body);
  r.record(T_DIR, p->pid(), sid, "busy"); r.advance(2000);
  r.record(T_DIR, p->pid(), sid, "waiting", { { "waitingFor", "permission prompt" } }); r.advance(2000);
  check("toast: a turn that ends in a question", r.notes.toasts.size() == 2 && r.notes.toasts[1].body == "waiting for you" && r.tile(id)["status"] == "waiting");
  r.deck->windowState(w, true, true);
  r.record(T_DIR, p->pid(), sid, "busy"); r.advance(2000); r.advance(30000); r.record(T_DIR, p->pid(), sid, "idle"); r.advance(2000);
  check("toast: none while a deck window has the focus", r.notes.toasts.size() == 2);
  r.deck->windowState(w, true, false);

  // sleep, wake
  { const size_t at = r.out[w].size();
    r.ptys.exitOnKill = false;
    r.send(w, { { "t", "call" }, { "rid", 700 }, { "fn", "sleepTile" }, { "args", { id } } });
    check("sleep: the tile reads asleep at once", r.tile(id)["status"] == "asleep" && r.tile(id)["asleep"] == true);
    r.advance(0);
    check("sleep: Claude is asked to exit by itself first: Ctrl+C", p->typed() == "\x1b[B\r\x03" && !p->killed);
    r.advance(360);
    check("sleep: three times, spaced", p->typed() == "\x1b[B\r\x03\x03\x03" && !p->killed);
    r.advance(2139);
    const bool notYet = !p->killed;
    r.advance(1);
    check("sleep: the hard kill only after the 2.5 s grace", notYet && p->killed && r.count(w, "ok", "", at) == 0);
    r.ptys.exits.emplace_back(p->token, 1); r.pump();
    check("sleep: answered true when the process is gone; saved asleep", r.lastOf(w, "ok")["rid"] == 700 && r.lastOf(w, "ok")["result"] == true && r.tilesFile()["tiles"][0]["asleep"] == true && r.tile(id)["ptySize"].is_null() && r.tile(id)["mouseOn"] == false);
    r.ptys.exitOnKill = true; }
  { const size_t at = r.out[w].size(); const size_t spawned = r.ptys.spawned.size();
    check("wake: answered true; the tile is starting, waiting its turn in the spawn queue", r.res(w, "wakeTile", { id }) == true && r.tile(id)["status"] == "starting" && r.ptys.spawned.size() == spawned);
    r.advance(0);
    check("wake: the screen is cleared and the session is resumed (--resume: a transcript exists)", r.count(w, "reset", id, at) == 1 && r.ptys.spawned.size() == spawned + 1 && r.ptys.spawned.back().args == std::vector<std::string>{ "--resume", sid });
    p = r.ptys.last(); }
  { r.record(T_DIR, p->pid(), sid, "idle"); r.advance(2000);
    r.call(w, "sleepTile", { id }, 3000);
    const size_t spawned = r.ptys.spawned.size();
    r.send(w, { { "t", "input" }, { "id", id }, { "d", "x" } });
    r.advance(0);
    check("wake: a key typed into a sleeping tile wakes it and is itself dropped", r.ptys.spawned.size() == spawned + 1 && r.ptys.last()->writes.empty());
    p = r.ptys.last(); }
  { r.record(T_DIR, p->pid(), sid, "idle"); r.advance(2000);
    r.call(w, "sleepTile", { id }, 3000);
    const size_t spawned = r.ptys.spawned.size();
    r.send(w, { { "t", "shown" }, { "ids", { id } }, { "hidden", false } });
    r.advance(0);
    check("wake: a sleeping tile that arrives on screen is woken", r.ptys.spawned.size() == spawned + 1);
    p = r.ptys.last();
    r.send(w, { { "t", "shown" }, { "ids", json::array() }, { "hidden", false } }); }

  // the settings warning: Continue on its first paint, unless a person typed since the spawn
  { const json f = loadFixture(fx, "startup");
    std::string raw; for (const json& s : f["streams"]) if (str(s["name"]) == "startup-settings-warning.raw" && s["chunks"].size() == 1) raw = str(s["chunks"][0]);
    const std::string id2 = r.claude(w, "claudet", P("C:\\elsewhere"));
    fake::Pty* q = r.ptys.last();
    r.feed(q, raw);
    r.advance(699);
    const bool none = q->writes.empty();
    r.advance(1);
    check("settings warning: Enter (Continue) 700 ms after the paint, and a toast naming what was skipped", none && q->writes == std::vector<std::string>{ "\r" } && r.notes.toasts.back().title == "Claude Code skipped part of a settings file"
      && r.notes.toasts.back().body.find("The deck pressed Continue") != std::string::npos, q->typed());
    const size_t toasts = r.notes.toasts.size();
    const std::string id3 = r.claude(w, "claudet", P("C:\\third"));
    fake::Pty* q3 = r.ptys.last();
    r.feed(q3, raw); r.advance(700);
    check("settings warning: the same warning on another tile is answered, and toasted only once per run", q3->writes == std::vector<std::string>{ "\r" } && r.notes.toasts.size() == toasts);
    const std::string id4 = r.claude(w, "claudet", P("C:\\fourth"));
    fake::Pty* q4 = r.ptys.last();
    r.send(w, { { "t", "input" }, { "id", id4 }, { "d", "2" } });
    r.feed(q4, raw); r.advance(700);
    check("settings warning: a person typed into the tile: their choice, the deck presses nothing", q4->writes == std::vector<std::string>{ "2" });
    r.advance(4000);
    { const json m = r.fresh(); bool said = false; for (const json& b : m["restart"]["blockers"]) if (b["id"] == id4 && b["reason"] == "waiting for you (startup: settings warning)") said = true;
      check("startup: a tile sitting on a dialog is WAITING, not starting, and says on what", r.tile(id4)["status"] == "waiting" && said, m["restart"].dump()); }
    r.call(w, "closeTile", { id2 }, 7000); r.call(w, "closeTile", { id3 }, 7000); r.call(w, "closeTile", { id4 }, 7000); }
  { r.opt.allowClaude = true; json s = r.files.getJson(P("C:\\state\\settings.json"));
    r.call(w, "saveSettings", json::array({ { { "autoTrust", false } } }));
    const std::string id5 = r.claude(w, "claudet", P("C:\\fifth"));
    fake::Pty* q = r.ptys.last();
    r.feed(q, "Quick safety check\r\n \xE2\x9D\xAF Yes, I trust this folder\r\n"); r.advance(5000);
    check("trust: with autoTrust off the deck answers nothing", q->writes.empty() && q->resizes.empty());
    r.call(w, "closeTile", { id5 }, 7000);
    r.call(w, "saveSettings", json::array({ { { "autoTrust", true } } })); }

  // ---- special prompts
  r.record(T_DIR, p->pid(), sid, "busy"); r.advance(2000);
  { const json item = r.res(w, "holdAdd", { id, { { "kind", "next" }, { "text", "then run the tests" } } });
    check("hold: a prompt for the next idle prompt is kept and shown on the tile", item["kind"] == "next" && item["state"] == "held" && r.tile(id)["held"].size() == 1 && r.tile(id)["sleepBlocker"] == "holding a prompt"
      && r.files.getJson(P("C:\\state\\held-prompts.json"))["items"].size() == 1, item.dump());
    const size_t writes = p->writes.size();
    r.advance(5000);
    check("hold: a busy session is not typed into", p->writes.size() == writes && r.tile(id)["held"][0]["note"] == "the session is busy");
    r.record(T_DIR, p->pid(), sid, "idle"); r.feed(p, "\r\n\xE2\x9D\xAF ");
    for (int i = 0; i < 80 && p->writes.size() == writes; i++) r.advance(100);
    check("hold: idle at its prompt: the text goes in as a paste", p->writes.size() == writes + 1 && p->writes[writes] == "\x1b[200~then run the tests\x1b[201~" && r.tile(id)["driver"] == "deck", p->typed());
    r.advance(150);
    check("hold: then Enter", p->writes.size() == writes + 2 && p->writes[writes + 1] == "\r");
    r.record(T_DIR, p->pid(), sid, "busy"); r.advance(3000);
    check("hold: the session turning busy is the confirmation", r.tile(id)["held"][0]["state"] == "sent" && r.tile(id)["held"][0]["note"].is_null() && p->writes.size() == writes + 2, r.tile(id)["held"].dump());
    check("hold: holdRemove", r.res(w, "holdRemove", { str(item["id"]) }) == true && r.tile(id)["held"].empty()); }
  // ---- triggers, and a prompt gated on one
  { check("triggers: declare, set, list", r.res(w, "triggerDeclare", { "ci_green" })["value"] == false && r.res(w, "triggersList").size() == 1 && r.call(w, "triggerSet", { "bad name", true })["error"] == "bad trigger name (letters, digits, . _ - only; 64 max)");
    r.res(w, "holdAdd", { id, { { "kind", "triggered" }, { "text", "the build is green" }, { "trigger", "ci_green" } } });
    r.record(T_DIR, p->pid(), sid, "idle"); r.advance(6000);
    const size_t writes = p->writes.size();
    r.files.put(P("C:\\state\\trigger-inbox\\drop-1.json"), "{\"name\":\"ci_green\",\"by\":\"session 12\"}");
    r.advance(4000);
    check("triggers: a file a session drops in trigger-inbox raises the trigger and is removed", r.fresh()["triggers"][0] == json{ { "name", "ci_green" }, { "value", true }, { "at", r.fresh()["triggers"][0]["at"] }, { "by", "session 12" } }
      && !r.files.has(P("C:\\state\\trigger-inbox\\drop-1.json")) && r.files.getJson(P("C:\\state\\triggers.json"))["triggers"]["ci_green"]["value"] == true, r.fresh()["triggers"].dump());
    r.advance(5200);
    check("triggers: the gated prompt is delivered on the rising edge", p->writes.size() > writes && p->writes[writes] == "\x1b[200~the build is green\x1b[201~");
    r.record(T_DIR, p->pid(), sid, "busy"); r.advance(3200); r.record(T_DIR, p->pid(), sid, "idle"); r.advance(12000);
    const size_t after = p->writes.size();
    r.advance(12000);
    check("triggers: once per edge, not every tick while it stays true", p->writes.size() == after);
    r.files.put(P("C:\\state\\trigger-inbox\\bare"), "ci_green"); r.advance(4000);
    check("triggers: a bare name sets it true; remove", r.res(w, "triggerRemove", { "ci_green" }) == true && r.res(w, "triggersList").empty()); }
  r.deck->shutdown();
}

// ---------------------------------------------------------------- hibernation, the memory guard, restart when quiet
void testQuietAndSleep() {
  Rig r; r.opt.allowClaude = true;
  const ClientId w = r.boot();
  const std::string a = r.claude(w), b = r.claude(w), c = r.claude(w);
  std::map<std::string, fake::Pty*> pty; std::map<std::string, std::string> sid;
  { const json m = r.fresh(); int i = 0; for (auto& kv : r.ptys.live) { const std::string id = i == 0 ? a : i == 1 ? b : c; pty[id] = kv.second; i++; }
    for (const std::string& id : { a, b, c }) { sid[id] = str(Rig::tileIn(m, id)["sessionId"]); r.transcript(T_DIR, sid[id], "t-" + id.substr(30), "x"); r.record(T_DIR, pty[id]->pid(), sid[id], "idle"); } }
  r.call(w, "pin", { b, true });
  r.send(w, { { "t", "shown" }, { "ids", { c } }, { "hidden", false } });
  r.advance(12000);
  check("hibernate: idle a moment: nothing sleeps", r.tile(a)["sleepBlocker"].get<std::string>().compare(0, 7, "active ") == 0 && r.tile(b)["sleepBlocker"] == "pinned" && r.tile(c)["sleepBlocker"] == "on screen (win:0)");
  r.advance(31 * 60000.0);
  check("hibernate: after half an hour idle the unpinned, off-screen session sleeps; the pinned and the shown stay up", r.tile(a)["status"] == "asleep" && r.tile(b)["status"] == "idle" && r.tile(c)["status"] == "idle", r.tile(b)["status"].dump() + r.tile(c)["status"].dump());
  // the memory guard: at 90 % of the commit limit pinned and on-screen sessions sleep too, with one toast
  r.procs.limit = 100; r.procs.used = 95;
  r.advance(30000 + 3000);
  check("memory guard: at 95 % commit every session that may sleep sleeps, pinned and on screen included", r.tile(b)["status"] == "asleep" && r.tile(c)["status"] == "asleep");
  check("memory guard: one toast says so", r.notes.toasts.size() == 1 && r.notes.toasts[0].title == std::string(deck::edition::kProductName) + ": memory is nearly full" && r.notes.toasts[0].body.find("95%") != std::string::npos, r.notes.toasts.empty() ? "" : r.notes.toasts[0].body);
  r.advance(60000);
  check("memory guard: while pressure lasts, being on screen does not wake a sleeper", r.tile(c)["status"] == "asleep");
  r.procs.used = 50;
  r.advance(61000);
  check("memory guard: pressure gone: the session on screen wakes (the tick after the one that saw it go)", r.tile(c)["asleep"] == false && r.tile(b)["status"] == "asleep");
  r.deck->shutdown();

  // ---- restart when quiet
  Rig q; q.opt.allowClaude = true;
  const ClientId w2 = q.boot();
  const std::string id = q.claude(w2); fake::Pty* p = q.ptys.last(); const std::string s = str(q.tile(id)["sessionId"]);
  q.shell(w2);
  q.record(T_DIR, p->pid(), s, "busy"); q.transcript(T_DIR, s, "Long job", "x"); q.advance(2000);
  check("quiet restart: armed, it names what is in the way and what a restart would cost", q.res(w2, "restartWhenQuiet", { true }) == true && q.fresh()["restart"]["armed"] == true && q.fresh()["restart"]["why"] == "manual"
    && q.fresh()["restart"]["blockers"].size() == 1 && q.fresh()["restart"]["blockers"][0]["title"] == "Long job" && q.fresh()["restart"]["blockers"][0]["reason"].get<std::string>().compare(0, 8, "mid-turn") == 0 && q.fresh()["restart"]["shells"] == 1, q.fresh()["restart"].dump());
  q.advance(120000);
  check("quiet restart: it never forces: a busy session holds it", q.life.restarts.empty());
  q.record(T_DIR, p->pid(), s, "idle"); q.advance(4000);
  check("quiet restart: quiet now: it settles for a minute first", q.life.restarts.empty() && q.fresh()["restart"]["quietSince"].is_number() && q.fresh()["restart"]["settleLeftMs"].get<double>() > 50000);
  q.send(w2, { { "t", "input" }, { "id", id }, { "d", "h" } }); q.advance(4000);
  check("quiet restart: typing resets the settle clock", q.fresh()["restart"]["quietSince"].is_null() && q.fresh()["restart"]["blockers"][0]["reason"].get<std::string>().compare(0, 6, "typed ") == 0, q.fresh()["restart"].dump());
  q.advance(60000 + 62000);
  check("quiet restart: a minute of quiet after the typing wore off: the shell is asked to relaunch, once", q.life.restarts == std::vector<std::string>{ "quiet" } && q.fresh()["restart"]["armed"] == false, std::to_string(q.life.restarts.size()));
  check("quiet restart: restartNow while one is under way is not a second one", q.res(w2, "restartNow") == false && q.life.restarts.size() == 1);
  q.deck->shutdown();

  // the update offer: the shell's files changed on disk
  Rig u; u.life.print = { { "deck.exe", "aaa" } };
  const ClientId w3 = u.boot();
  u.life.print = { { "deck.exe", "bbb" }, { "new.dll", "ccc" } };
  u.advance(30000);
  check("update: a change to the running files is offered", u.fresh()["restart"]["update"]["files"] == json::array({ "deck.exe", "new.dll" }));
  check("update: armed for the update, it says so", u.res(w3, "restartWhenQuiet", { true, "update" }) == true && u.fresh()["restart"]["why"] == "update" && (u.advance(62000 + 2000), u.life.restarts == std::vector<std::string>{ "quiet, for the update" }));
  Rig v; v.life.print = { { "deck.exe", "aaa" } };
  const ClientId w4 = v.boot();
  v.life.print = { { "deck.exe", "bbb" } }; v.advance(30000);
  v.call(w4, "updateLater");
  check("update: Later dismisses it until the files change again", v.fresh()["restart"]["update"].is_null() && (v.life.print = { { "deck.exe", "ddd" } }, v.advance(30000), v.fresh()["restart"]["update"]["files"] == json::array({ "deck.exe" })));
  u.deck->shutdown(); v.deck->shutdown();
}

// ---------------------------------------------------------------- several clients: two windows, a phone, a desk
// THE OLD RULE for a remote desk (DeckOptions::remoteDeskIsAWindow): what main.js on `main` does today — a desk is
// a window like any other for one home per session and for sleep. The remote-desk rule of 2026-11-18 (the core's
// default) is tested beside it in remoterule_test.cpp.
void testClients() {
  Rig r; r.opt.persistShells = true; r.opt.remoteDeskIsAWindow = true;
  r.make();
  const ClientId w1 = r.deck->addWindow(), w2 = r.deck->addWindow();
  r.deck->start();
  r.call(w1, "deskInit");
  check("clients: each window learns which it is", r.res(w2, "deskInit")["windowIndex"] == 1 && r.deck->windowIndex(w1) == 0 && r.deck->windowIndex(w2) == 1);
  r.advance(2100);
  const std::string a = r.shell(w1), b = r.shell(w1);
  fake::Pty* pa = nullptr; fake::Pty* pb = nullptr;
  { int i = 0; for (auto& kv : r.ptys.live) { if (i == 0) pa = kv.second; else pb = kv.second; i++; } }
  r.feed(pa, "A0 "); r.feed(pb, "B0 ");

  // ---- rid spaces and replies
  r.send(w1, { { "t", "call" }, { "rid", 7 }, { "fn", "getSettings" }, { "args", json::array() } });
  r.send(w2, { { "t", "call" }, { "rid", 7 }, { "fn", "triggersList" }, { "args", json::array() } });
  check("clients: two clients may use the same rid; each gets its own answer and only its own", r.lastOf(w1, "ok")["rid"] == 7 && r.lastOf(w1, "ok")["result"].contains("aliasFile") && r.lastOf(w2, "ok")["rid"] == 7 && r.lastOf(w2, "ok")["result"].is_array());

  // ---- a phone
  const ClientId ph = r.deck->addRemote();
  check("phone: the hello is its boot: init with home, platform, a meta", r.out[ph].size() == 1 && r.out[ph][0]["t"] == "init" && r.out[ph][0]["home"] == P("C:\\home") && r.out[ph][0]["meta"]["tiles"].size() == 2 && r.out[ph][0].contains("assets") && r.out[ph][0].contains("pageKey"));
  r.send(ph, { { "t", "shown" }, { "ids", { a } }, { "hidden", false } });
  r.send(ph, { { "t", "layout" }, { "layout", { { a, { 1 } } } } });
  check("phone: the window messages are ignored from a client that is not a desk", r.tile(a)["shownBy"].empty() && r.tile(a)["inWindow"].is_null());
  r.send(ph, { { "t", "attach" }, { "id", a } });
  check("phone: attach carries the replay", r.data(ph, a, 0, true) == "A0 " && r.tile(a)["shownBy"] == json::array({ "remote" }));
  r.feed(pa, "A1 "); r.feed(pb, "B1 ");
  check("phone: it is sent the tile it shows and no other", r.data(ph, a) == "A0 A1 " && r.count(ph, "data", b) == 0 && r.count(w1, "data") == 0);
  r.send(ph, { { "t", "attach" }, { "id", b } });
  r.feed(pa, "A2 "); r.feed(pb, "B2 ");
  check("phone: a second attach replaces the first", r.data(ph, a) == "A0 A1 " && r.data(ph, b) == "B0 B1 B2 " && r.tile(a)["shownBy"].empty());
  { const json ok = r.out[ph].back();
    r.send(ph, { { "t", "call" }, { "rid", 0 }, { "fn", "userInput" }, { "args", { b } } });
    check("phone: lib/phone.js answers every call, rid 0 included", r.out[ph].back()["t"] == "ok" && r.out[ph].back()["rid"] == 0 && r.tile(b)["driver"] == "phone"); (void)ok; }
  check("phone: an action that is not in the table is refused by name", r.call(ph, "setOscTitle", { b, "x" })["error"] == "unknown action: setOscTitle" && r.call(ph, "openWindow")["error"] == "unknown action: openWindow");
  { const json s = r.res(ph, "getSettings");
    check("phone: getSettings never carries the token or the desktop's file paths", s["phone"] == json{ { "enabled", false }, { "port", 8788 }, { "bind", "tailscale" } } && !s.contains("aliasFile") && !s.contains("phoneStatus") && s["accounts"].size() == 2, s["phone"].dump());
    r.call(ph, "saveSettings", json::array({ { { "fontSize", 17 }, { "phone", { { "enabled", true }, { "port", 9999 }, { "newToken", true } } } } }));
    const json disk = r.files.getJson(P("C:\\state\\settings.json"));
    check("phone: saveSettings from a remote changes the ordinary settings, never the phone block", disk["fontSize"] == 17 && disk["phone"]["enabled"] == false && disk["phone"]["port"] == 8788 && disk["phone"]["token"] == "abababababababab"); }

  // ---- who drives: auto, and a phone has the tile
  { const size_t at = r.out[w1].size(); const size_t writes = pb->writes.size();
    r.send(ph, { { "t", "resize" }, { "id", b }, { "cols", 44 }, { "rows", 20 } });
    r.send(w1, { { "t", "resize" }, { "id", b }, { "cols", 150 }, { "rows", 50 } });
    r.advance(200);
    check("driver (auto): the phone attached to a tile owns its size; the window's resize is ignored", r.deck->ptySize(b) == std::make_pair(44, 20));
    check("driver (auto): the window is told what to mirror", r.tile(b)["mirror"] == json{ { "cols", 44 }, { "rows", 20 } } && r.tile(a)["mirror"].is_null());
    r.send(w1, { { "t", "input" }, { "id", b }, { "d", "typed at the desk" } });
    r.send(w1, { { "t", "input" }, { "id", b }, { "d", "\x1b[O" } });
    const auto ev = r.events(w1, "input:refused", at);
    check("driver (R10): the window is read-only on a tile a remote drives, and is told why", pb->writes.size() == writes && ev.size() == 1 && ev[0] == json{ { "id", b }, { "reason", "phone-attached" } } && r.events(w2, "input:refused").empty());
    r.send(w1, { { "t", "input" }, { "id", a }, { "d", "free" } });
    check("driver (R10): only on THAT tile", pa->writes == std::vector<std::string>{ "free" });
    r.send(ph, { { "t", "input" }, { "id", b }, { "d", "from the phone" } });
    check("driver: the phone's input goes in", pb->writes.size() == writes + 1 && pb->writes.back() == "from the phone");
    r.send(w1, { { "t", "call" }, { "rid", 0 }, { "fn", "nudge" }, { "args", { b, 150, 50 } } });
    check("driver: the window's nudge is refused too", r.deck->ptySize(b) == std::make_pair(44, 20)); }
  r.call(w1, "setDriver", { "desk" });
  { r.send(ph, { { "t", "resize" }, { "id", b }, { "cols", 60 }, { "rows", 25 } });
    r.send(w1, { { "t", "resize" }, { "id", b }, { "cols", 150 }, { "rows", 50 } });
    r.advance(200);
    check("driver (desk): the desk owns the size; the phone is a viewer and nothing is mirrored", r.deck->ptySize(b) == std::make_pair(150, 50) && r.tile(b)["mirror"].is_null());
    const size_t writes = pb->writes.size();
    r.send(w1, { { "t", "input" }, { "id", b }, { "d", "desk types" } });
    check("driver (desk): and the window types", pb->writes.size() == writes + 1); }
  r.call(ph, "setDriver", { "auto" });
  r.send(ph, { { "t", "detach" } });
  check("driver (auto): the phone put the tile away: it lets go", r.tile(b)["mirror"].is_null() && r.tile(b)["shownBy"].empty());

  // ---- a desk
  const ClientId dk = r.deck->addRemote();
  const json dinit = r.call(dk, "deskInit", json::array({ { { "name", "LAPTOP" } } }));
  check("desk: deskInit registers the remote as a window: the next free index, its own (empty) layout", dinit["t"] == "ok" && dinit["result"]["windowIndex"] == 2 && dinit["result"]["layout"] == json::object() && dinit["result"]["grid"]["rows"] == 2 && r.deck->deskCount() == 1 && r.deck->remoteCount() == 2);
  { const ClientId dk2 = r.deck->addRemote();
    check("desk: one desk at a time: a second is refused while the first is connected", r.call(dk2, "deskInit")["error"] == "one desk at a time: another browser desk is connected \xE2\x80\x94 close that tab first" && r.deck->deskCount() == 1);
    r.deck->removeClient(dk2); }
  r.send(dk, { { "t", "attach" }, { "id", a } }); r.send(dk, { { "t", "attach" }, { "id", b } });
  r.send(dk, { { "t", "shown" }, { "ids", { a, b } }, { "hidden", false } });
  r.send(dk, { { "t", "layout" }, { "layout", { { a, { 1, 3 } }, { b, { 2, 4 } } } } });
  check("desk: it carries several panes; its layout is its own, saved as deskLayout", r.data(dk, a, 0, true) == "A0 A1 A2 " && r.data(dk, b, 0, true).compare(0, 9, "B0 B1 B2 ") == 0 && r.tile(a)["inWindow"] == 2 && r.tile(a)["shownBy"] == json::array({ "win:2", "remote" })
    && r.tilesFile()["deskLayout"] == json{ { a, { 1, 3 } }, { b, { 2, 4 } } } && r.tilesFile()["windows"].size() == 2, r.tile(a).dump());
  r.advance(2000);
  { const json dm = r.lastOf(dk, "meta")["meta"];
    check("desk (auto): it shows the tile and no phone has it: the desk DRIVES (mirror null) and the window is read-only", Rig::tileIn(dm, a)["mirror"].is_null() && Rig::tileIn(dm, b)["mirror"].is_null());
    const size_t at = r.out[w1].size();
    r.send(w1, { { "t", "input" }, { "id", a }, { "d", "nope" } });
    r.send(dk, { { "t", "resize" }, { "id", a }, { "cols", 100 }, { "rows", 40 } }); r.advance(200);
    check("desk (auto): a remote desk counts as a phone for the driver rule", r.events(w1, "input:refused", at).size() == 1 && r.deck->ptySize(a) == std::make_pair(100, 40)); }
  r.send(ph, { { "t", "attach" }, { "id", a } });
  r.send(ph, { { "t", "resize" }, { "id", a }, { "cols", 40 }, { "rows", 18 } }); r.advance(2200);
  { const json dm = r.lastOf(dk, "meta")["meta"];
    check("desk: a phone took the tile: the desk mirrors the pty's real size on it, and still drives the other", Rig::tileIn(dm, a)["mirror"] == json{ { "cols", 40 }, { "rows", 18 } } && Rig::tileIn(dm, b)["mirror"].is_null(), Rig::tileIn(dm, a).dump()); }
  r.send(ph, { { "t", "detach" } });
  r.send(dk, { { "t", "shown" }, { "ids", { a, b } }, { "hidden", true } });
  check("desk: a hidden tab shows nothing and lets go of what it drove", r.tile(a)["shownBy"].empty() && (r.send(w1, { { "t", "input" }, { "id", a }, { "d", "mine again" } }), pa->writes.back() == "mine again"));
  r.send(dk, { { "t", "shown" }, { "ids", { a, b } }, { "hidden", false } });
  { const json s = r.res(dk, "getSettings");
    check("desk: getSettings is the desktop shape (alias file, phone status)", s.contains("aliasFile") && s.contains("phoneStatus") && s["phone"]["token"] == "abababababababab"); }

  // ---- one home per tile, across windows and the desk
  { const size_t atD = r.out[dk].size(), at2 = r.out[w2].size();
    r.send(w1, { { "t", "layout" }, { "layout", { { a, { 1, 2, 3, 4 } } } } });
    const auto ev = r.events(dk, "layout:evict", atD);
    check("layout: a window that takes a tile evicts it from the desk, which is told", ev.size() == 1 && ev[0] == json::array({ a }) && r.tile(a)["inWindow"] == 0 && r.tilesFile()["deskLayout"] == json{ { b, { 2, 4 } } } && r.events(w2, "layout:evict", at2).empty(), r.tilesFile().dump().substr(0, 300));
    const size_t at1 = r.out[w1].size();
    r.send(w2, { { "t", "layout" }, { "layout", { { a, { 1 } }, { b, { 2 } } } } });
    check("layout: …and window 2 taking both evicts each from where it was", r.events(w1, "layout:evict", at1).size() == 1 && r.events(w1, "layout:evict", at1)[0] == json::array({ a }) && r.events(dk, "layout:evict", atD).size() == 2 && r.tile(b)["inWindow"] == 1);
    const size_t at1b = r.out[w1].size(), atDb = r.out[dk].size();
    r.send(w2, { { "t", "leaving" }, { "id", a }, { "on", true } });
    check("layout: a stepping walk's leaving mark is relayed to every OTHER window", r.events(w1, "layout:leaving", at1b).size() == 1 && r.events(w1, "layout:leaving", at1b)[0] == json{ { "id", a }, { "on", true } } && r.events(dk, "layout:leaving", atDb).size() == 1 && r.events(w2, "layout:leaving").empty()); }
  r.call(w1, "saveSettings", json::array({ { { "oneWindow", false } } }));
  { const size_t at2 = r.out[w2].size();
    r.send(w1, { { "t", "layout" }, { "layout", { { a, { 1 } } } } });
    check("layout: with oneWindow off a session may show in several windows: nobody is evicted", r.events(w2, "layout:evict", at2).empty() && r.res(w2, "deskInit")["layout"] == json{ { a, { 1 } }, { b, { 2 } } }); }
  r.call(w1, "saveSettings", json::array({ { { "oneWindow", true } } }));
  { const size_t at1 = r.out[w1].size(), at2 = r.out[w2].size(), atD = r.out[dk].size(), atP = r.out[ph].size();
    r.send(dk, { { "t", "layout" }, { "layout", { { b, { 1, 2 } } } } });
    r.call(dk, "saveSettings", json::array({ { { "grid", { { "rows", 3 }, { "cols", 3 } } } } }));
    const auto e1 = r.events(w1, "grid:changed", at1); const auto eD = r.events(dk, "grid:changed", atD);
    check("grid: a change tells every window its own remapped layout, the desk the new shape, a phone nothing", e1.size() == 1 && e1[0]["layout"] == json{ { a, { 1 } } } && r.events(w2, "grid:changed", at2).size() == 1 && eD.size() == 1 && eD[0] == json{ { "rows", 3 }, { "cols", 3 } }
      && r.events(ph, "grid:changed", atP).empty() && r.tilesFile()["deskLayout"] == json{ { b, { 1, 2 } } }, eD.empty() ? "" : eD[0].dump()); }

  // ---- exit and reset reach everyone; data only who carries the tile
  { const size_t at1 = r.out[w1].size(), at2 = r.out[w2].size(), atD = r.out[dk].size(), atP = r.out[ph].size();
    r.send(w2, { { "t", "attach" }, { "id", b } });
    r.feed(pb, "ONLY-CARRIERS");
    check("fan-out: data goes to the clients that carry the tile", r.data(w2, b, at2).find("ONLY-CARRIERS") != std::string::npos && r.data(dk, b, atD) == "ONLY-CARRIERS" && r.count(w1, "data", b, at1) == 0 && r.count(ph, "data", b, atP) == 0);
    r.ptys.exits.emplace_back(pb->token, 3); r.pump();
    check("fan-out: an exit goes to every client", r.count(w1, "exit", b, at1) == 1 && r.count(w2, "exit", b, at2) == 1 && r.count(dk, "exit", b, atD) == 1 && r.count(ph, "exit", b, atP) == 1);
    r.call(w1, "restartTile", { b });
    check("fan-out: and so does a reset", r.count(w2, "reset", b, at2) == 1 && r.count(dk, "reset", b, atD) == 1 && r.count(ph, "reset", b, atP) == 1);
    pb = r.ptys.last(); }

  // ---- what a remote is sent: clipped, deduplicated, kept alive, skipped when busy
  { Rig m; m.opt.allowClaude = true;
    const ClientId w = m.boot();
    const std::string id = m.claude(w); fake::Pty* p = m.ptys.last(); const std::string sid = str(m.tile(id)["sessionId"]);
    std::string longText; for (int i = 1; i <= 30; i++) longText += "line " + std::to_string(i) + "\n";
    m.record(T_DIR, p->pid(), sid, "idle"); m.transcript(T_DIR, sid, "T", longText);
    const ClientId phone = m.deck->addRemote(), desk = m.deck->addRemote();
    m.call(desk, "deskInit");
    m.advance(2000);
    auto lines = [](const json& meta) { const std::string s = str(meta["tiles"][0]["lastText"]); return (int)std::count(s.begin(), s.end(), '\n') + (s.empty() || s.back() == '\n' ? 0 : 1); };
    check("remote meta: the window gets the whole last message, a phone 3 lines of it, a desk 10", lines(m.lastOf(w, "meta")["meta"]) == 30 && lines(m.lastOf(phone, "init")["meta"]) == 3 && lines(m.lastOf(desk, "meta")["meta"]) == 10);
    check("remote meta: a desk's copy says what each pane must render at; the phone, whose hello already said all this, was sent nothing more", m.lastOf(desk, "meta")["meta"]["tiles"][0].contains("mirror") && m.lastOf(desk, "meta").contains("pageKey") && m.count(phone, "meta") == 0);
    // (an idle session's tooltip counts the seconds — "active 12s ago" — and that IS a change, in main.js too:
    // pinned, its reason not to sleep stands still — once the deck has looked for its transcript again, which it
    // does every ten seconds for a session that had none)
    m.call(w, "pin", { id, true }); m.advance(12000);
    const size_t wn = m.count(w, "meta"), pn = m.count(phone, "meta"), dn = m.count(desk, "meta");
    m.advance(10000);
    check("remote meta: unchanged, it is not sent again to a remote (the window still gets every tick)", m.count(w, "meta") - wn >= 4 && m.count(phone, "meta") == pn && m.count(desk, "meta") == dn, std::to_string(m.count(w, "meta") - wn) + " " + std::to_string(m.count(phone, "meta") - pn) + " " + std::to_string(m.count(desk, "meta") - dn));
    m.advance(10000);
    check("remote meta: …but once every 20 s all the same, so the clock keeps moving", m.count(phone, "meta") == pn + 1 && m.count(desk, "meta") == dn + 1, std::to_string(m.count(phone, "meta") - pn));
    m.call(w, "renameTile", { id, "renamed" });
    check("remote meta: a change goes out at once", m.count(phone, "meta") == pn + 2 && m.lastOf(phone, "meta")["meta"]["tiles"][0]["title"] == "renamed");
    m.busy.insert(phone);
    m.call(w, "renameTile", { id, "while busy" });
    const size_t pBusy = m.count(phone, "meta");
    m.feed(p, "lost output");
    m.busy.erase(phone);
    m.advance(2000);
    check("remote meta: a frame a busy client was not sent is sent at the next tick (it heals)", pBusy == pn + 2 && m.count(phone, "meta") == pn + 3 && m.lastOf(phone, "meta")["meta"]["tiles"][0]["title"] == "while busy");
    m.deck->shutdown(); }

  // ---- a desk that goes away, and comes back
  r.send(dk, { { "t", "layout" }, { "layout", { { b, { 1, 2, 3 } } } } });
  r.deck->removeClient(dk);
  check("desk: gone, its window entry goes; its tiles are held by no window", r.deck->deskCount() == 0 && r.tile(b)["inWindow"].is_null() && r.tile(b)["shownBy"].empty() && r.tilesFile()["deskLayout"] == json{ { b, { 1, 2, 3 } } });
  { const ClientId dk3 = r.deck->addRemote();
    r.send(w1, { { "t", "layout" }, { "layout", { { a, { 1 } } } } });
    const json again = r.call(dk3, "deskInit");
    check("desk: the next desk gets the same window index and the saved layout — minus what a real window holds now", again["result"]["windowIndex"] == 2 && again["result"]["layout"] == json{ { b, { 1, 2, 3 } } }, again["result"]["layout"].dump());
    r.send(dk3, { { "t", "layout" }, { "layout", { { a, { 5 } }, { b, { 1, 2, 3 } } } } });
    r.deck->removeClient(dk3);
    r.send(w1, { { "t", "layout" }, { "layout", { { a, { 1 } } } } });
    const ClientId dk4 = r.deck->addRemote();
    check("desk: connecting from afar never pulls a pane off the desktop", r.call(dk4, "deskInit")["result"]["layout"] == json{ { b, { 1, 2, 3 } } });
    r.deck->removeClient(dk4); }

  // ---- F5: a tile dragged out opens a window holding it alone
  { const size_t at1 = r.out[w1].size();
    const ClientId w3 = r.deck->openWindow(a, { { "x", 5 }, { "y", 5 }, { "width", 800 }, { "height", 600 } });
    check("window:open: the new window holds the tile, full; it left the window it was in", w3 != 0 && r.deck->windowIndex(w3) == 3 && r.res(w3, "deskInit")["layout"] == json{ { a, { 1, 2, 3, 4, 5, 6, 7, 8, 9 } } } && r.events(w1, "layout:evict", at1).size() == 1 && r.tilesFile()["windows"].size() == 3);
    r.deck->removeClient(w3);
    check("window: closing one of several touches no session; the tile is simply unplaced", r.tile(a)["inWindow"].is_null() && r.tile(a)["status"] == "shell" && r.tilesFile()["windows"].size() == 2); }
  r.deck->removeClient(ph);
  check("phone: gone, the deck has no remote", r.deck->remoteCount() == 0);
  r.deck->shutdown();
}

// ---------------------------------------------------------------- sessions elsewhere and past, adopt, moves, a session started in a shell
void testSessions() {
  Rig r; r.opt.allowClaude = true;
  r.files.put(std::string(U_DIR) + P("\\.claude.json"), "{\"oauthAccount\":{\"emailAddress\":\"u@example.com\"}}");
  const ClientId w = r.boot();
  // synthetic records: one live elsewhere, one dead, one headless, one hosted here
  r.procs.alivePids.insert(7001); r.procs.alivePids.insert(7003);
  r.record(T_DIR, 7001, "s-elsewhere", "busy", { { "cwd", P("C:\\proj") }, { "name", "elsewhere" } });
  r.record(T_DIR, 7002, "s-dead", "idle");
  r.record(U_DIR, 7003, "s-headless", "idle", { { "kind", "sdk" } });
  r.transcript(T_DIR, "s-elsewhere", "Running in a terminal", "x", 3000);
  r.transcript(T_DIR, "s-dead", "An old one \"quoted\"", "x", 3000);
  r.transcript(U_DIR, "s-tiny", "never used", "x");
  r.clock.t += 5000;
  r.transcript(U_DIR, "s-newer", "The newest", "x", 3000);
  { const json ext = r.res(w, "externalSessions");
    check("elsewhere: a live interactive session no tile holds, with its account, folder and title", ext.size() == 1 && ext[0]["sessionId"] == "s-elsewhere" && ext[0]["account"] == "claudet" && ext[0]["label"] == "t@example.com" && ext[0]["pid"] == 7001 && ext[0]["status"] == "busy"
      && ext[0]["cwd"] == P("C:\\proj") && ext[0]["name"] == "elsewhere" && ext[0]["title"] == "Running in a terminal", ext.dump());
    const json past = r.res(w, "pastSessions");
    check("past: every transcript that is neither open here nor running, newest first, tiny ones left out", past.size() == 2 && past[0]["sessionId"] == "s-newer" && past[0]["account"] == "claudeu" && past[0]["title"] == "The newest" && past[0]["cwd"] == P("C:\\work")
      && past[1]["sessionId"] == "s-dead" && past[1]["title"] == "An old one \"quoted\"" && !past[0].contains("file") && past[0]["size"].get<int>() > 3000, past.dump()); }
  // adopt: the process elsewhere is ended, and only when it is gone is the session resumed here
  { r.procs.killTreeEnds = false;
    const size_t at = r.out[w].size(); const size_t spawned = r.ptys.spawned.size();
    r.send(w, { { "t", "call" }, { "rid", 800 }, { "fn", "adoptSession" }, { "args", json::array({ { { "sessionId", "s-elsewhere" }, { "account", "claudet" }, { "pid", 7001 }, { "cwd", P("C:\\proj") } } }) } });
    r.advance(450);
    check("adopt: the session's process is told to end, and the deck waits for it", r.procs.killedTrees == std::vector<uint32_t>{ 7001 } && r.ptys.spawned.size() == spawned && r.count(w, "ok", "", at) == 0);
    r.procs.alivePids.erase(7001);
    r.advance(200);
    const json ok = r.lastOf(w, "ok");
    check("adopt: gone: resumed in a tile of the same account, in its folder", ok["rid"] == 800 && r.ptys.spawned.size() == spawned + 1 && r.ptys.spawned.back().args == std::vector<std::string>{ "--resume", "s-elsewhere" } && r.ptys.spawned.back().cwd == P("C:\\proj")
      && hasEnv(r.ptys.spawned.back().env, "CLAUDE_CONFIG_DIR", T_DIR) && r.tile(str(ok["result"]))["sessionId"] == "s-elsewhere");
    check("adopt: a session already open here is refused", r.call(w, "adoptSession", json::array({ { { "sessionId", "s-elsewhere" }, { "account", "claudet" }, { "pid", 1 } } }))["error"] == "already open here");
    check("elsewhere: it is no longer listed", r.res(w, "externalSessions").empty());

    // ---- moving it to another account: userswitch through bash
    const std::string id = str(ok["result"]); const uint64_t oldPty = r.ptys.last()->token;   // (the move deletes that Pty: ask by token)
    check("move: refusals", r.call(w, "moveTile", { id, "claudet" })["error"] == "the session is already in that account" && r.call(w, "moveTile", { id, "nope" })["error"] == "unknown account: nope" && r.call(w, "moveTile", { "nope", "claudeu" })["error"] == "no such tile");
    r.call(w, "moveTile", { id, "claudeu", { { "browser", true } } }, 3000);
    const PtySpawn& mv = r.ptys.spawned.back();
    check("move: the old process is ended; userswitch --start <to> \"<from> --resume <id>\" runs in Git Bash under the target account", r.ptys.wasKilled(oldPty) && mv.file == P("C:\\Git\\bin\\bash.exe")
      && mv.args == std::vector<std::string>{ P("C:/home/.local/bin/userswitch"), "--start", "claudeu", "claudet --resume s-elsewhere" } && hasEnv(mv.env, "CLAUDE_CONFIG_DIR", U_DIR) && hasEnv(mv.env, "USERSWITCH_ZSHRC", ALIAS)
      && hasEnv(mv.env, "CLAUDE_CODE_ENABLE_CFC", "1") && r.tile(id)["account"] == "claudeu" && r.tile(id)["browser"] == true, js::toString(json(mv.args)));
    // ---- moveAll, with "continue" typed into a session that had stopped on a usage notice (the private edition only:
    // the public build refuses it, core/deck/edition.hpp)
    const std::string second = r.claude(w, "claudeu", P("C:\\two")); const std::string sid2 = str(r.tile(second)["sessionId"]);
    r.transcript(U_DIR, sid2, "Hit the wall", "You've hit your weekly limit \xC2\xB7 resets 6pm");
    r.advance(2000);
    // the pages the deck serves read meta.edition (the public build only): its name, and no move-all button (ask r4 Q3, Q6)
    if (deck::edition::kPublic) check("meta.edition (public build): the name and moveAll false", r.fresh()["edition"] == json{ { "name", deck::edition::kProductName }, { "moveAll", false } }, r.fresh()["edition"].dump());
    else check("meta.edition: none in the owner's deck (its pages are as before)", !r.fresh().contains("edition"), r.fresh()["edition"].dump());
    if (deck::edition::kPublic) {
      check("moveAll: not in the public build", r.call(w, "moveAll", { "claudeu", "claudet", true })["error"] == "moving every session at once is not in this build: move each tile from its menu");
      r.call(w, "moveTile", { id, "claudet" }, 3000); r.call(w, "moveTile", { second, "claudet" }, 3000);   // where moveAll puts them, by hand
      check("moveAll (public build): the two tiles moved by hand instead", r.tile(second)["account"] == "claudet" && r.tile(id)["account"] == "claudet");
    } else {
    const size_t before = r.ptys.spawned.size();
    const json moved = r.call(w, "moveAll", { "claudeu", "claudet", true }, 12000);
    check("moveAll: every session of the account moves, one at a time, and it says how many", moved["result"] == json{ { "moved", 2 }, { "errors", json::array() }, { "usageSessions", 1 } } && r.ptys.spawned.size() == before + 2 && r.tile(second)["account"] == "claudet", moved.dump());
    fake::Pty* np = nullptr; for (auto& kv : r.ptys.live) if (kv.second->spec.args.size() == 4 && kv.second->spec.args[3] == "claudeu --resume " + sid2) np = kv.second;
    r.clock.t += 10; r.record(T_DIR, np ? np->pid() : 0, sid2, "idle");
    r.advance(1000 + 1200 + 250 + 50);
    check("moveAll: the moved session that had hit a usage limit is told to continue once it is idle at its prompt", np && np->typed() == "continue\r", np ? np->typed() : "no pty");
    check("moveAll: refusals", r.call(w, "moveAll", { "claudet", "claudet" })["error"] == "same account" && r.call(w, "moveAll", { "x", "claudet" })["error"] == "unknown account");
    }
    // ---- office check 2026-11-20: a session with NO conversation yet (no transcript in any account) is not moved by
    // userswitch (it refuses): it starts afresh on the new account under the same id
    const std::string fresh = r.claude(w, "claudet", P("C:\\three")); const std::string sid3 = str(r.tile(fresh)["sessionId"]);
    r.call(w, "moveTile", { fresh, "claudeu" }, 3000);
    const PtySpawn& fm = r.ptys.spawned.back();
    check("move: no conversation yet: a plain start on the new account, same id, no userswitch", fm.args.size() >= 2 && fm.args[0] == "--session-id" && fm.args[1] == sid3
      && hasEnv(fm.env, "CLAUDE_CONFIG_DIR", U_DIR) && r.tile(fresh)["account"] == "claudeu", js::toString(json(fm.args)));
    // ---- a move userswitch refuses (it exits 1 at once): the tile goes back to the account its session is in
    r.call(w, "moveTile", { second, "claudeu" }, 3000);
    fake::Pty* up = r.ptys.last();
    check("move: under way, the tile shows the new account", r.tile(second)["account"] == "claudeu" && up->spec.args.size() == 4);
    r.ptys.exits.emplace_back(up->token, 1); r.advance(50);
    check("move: refused (exit 1 at once): the tile is on its old account again", r.tile(second)["account"] == "claudet", r.tile(second).dump()); }
  r.deck->shutdown();

  // ---- a Claude session started by hand inside a shell tile becomes a Claude tile, and a shell again when it ends
  Rig s; s.opt.allowClaude = true;
  const ClientId w2 = s.boot();
  const std::string sh = s.shell(w2, P("C:\\proj")); fake::Pty* sp = s.ptys.last();
  const std::string other = s.shell(w2); fake::Pty* op = s.ptys.last();
  s.procs.alivePids.insert(8100); s.procs.alivePids.insert(8200);
  s.procs.parentOf = { { 8100, 8050 }, { 8050, sp->pid() }, { sp->pid(), 4 }, { op->pid(), 4 }, { 8200, 999999 } };
  s.record(T_DIR, 8100, "s-in-shell", "idle", { { "cwd", P("C:\\proj\\sub") }, { "startedAt", s.clock.t } });
  s.advance(3000);
  json t = s.tile(sh);
  check("shell host: the process tree places a session in the shell tile that started it: the tile is a Claude tile now", t["kind"] == "claude" && t["account"] == "claudet" && t["sessionId"] == "s-in-shell" && t["cwd"] == P("C:\\proj\\sub") && t["status"] == "idle"
    && s.tilesFile()["tiles"].size() == 1 && s.tile(other)["kind"] == "shell" && !sp->killed, t.dump());
  // a chain that broke on the way up (usw --start): the console it is attached to says which tile
  s.record(T_DIR, 8200, "s-broken-chain", "idle", { { "startedAt", s.clock.t } });
  s.procs.hostOf = { { 8200, 555 }, { op->pid(), 555 }, { sp->pid(), 777 } };
  s.advance(3000);
  check("shell host: a broken parent chain is placed by its console host", s.tile(other)["kind"] == "claude" && s.tile(other)["sessionId"] == "s-broken-chain", s.tile(other).dump());
  s.procs.alivePids.erase(8100);
  s.advance(3000);
  t = s.tile(sh);
  check("shell host: the session ended back at the prompt: the tile is a shell again", t["kind"] == "shell" && t["account"].is_null() && t["sessionId"].is_null() && t["status"] == "shell" && s.tilesFile()["tiles"].size() == 1);
  s.deck->shutdown();

  // ---- usage through the deck (fake endpoint, fake credentials), uploads, the account editor
  // the public build polls only with settings.json `usageLive` true (core/deck/edition.hpp): off, nothing is asked
  { Rig q; q.usage = true;
    q.creds.stamps[T_DIR] = "1"; q.creds.oauth[T_DIR] = { { "claudeAiOauth", { { "accessToken", "FAKE-TOKEN" }, { "subscriptionType", "max" }, { "expiresAt", 9e15 } } } };
    HttpResponse r0; r0.status = 200; r0.body = "{}"; q.http.byToken["FAKE-TOKEN"] = r0;
    const ClientId wq = q.boot(); q.advance(200000);
    if (deck::edition::kPublic) check("usage (public build): usageLive unset - no request, no bars, refreshUsage refused", q.http.asked.empty() && q.fresh()["usage"] == json::object() && q.call(wq, "refreshUsage")["error"] == "the live usage bars are off (Settings: Live usage bars)", q.fresh()["usage"].dump());
    else check("usage (the private edition): polled with usageLive unset", !q.http.asked.empty());
    q.deck->shutdown(); }
  Rig u; u.usage = true;
  u.files.put(P("C:\\state\\settings.json"), "{\"usageLive\":true}");      // the public build's opt-in (ignored by the private edition)
  u.creds.stamps[T_DIR] = "1"; u.creds.oauth[T_DIR] = { { "claudeAiOauth", { { "accessToken", "FAKE-TOKEN" }, { "subscriptionType", "max" }, { "expiresAt", 9e15 } } } };
  HttpResponse ok; ok.status = 200; ok.body = "{\"five_hour\":{\"utilization\":33,\"resets_at\":\"2027-01-15T12:00:00Z\"},\"seven_day\":{\"utilization\":5,\"resets_at\":null}}";
  u.http.byToken["FAKE-TOKEN"] = ok;
  const ClientId w3 = u.boot();
  check("usage: polled at boot, per account, into the meta", u.fresh()["usage"]["claudet"]["state"] == "ok" && u.fresh()["usage"]["claudet"]["session"]["used"] == 33 && u.fresh()["usage"]["claudeu"]["state"] == "no-credentials" && u.http.asked.size() == 1, u.fresh()["usage"].dump());
  u.advance(170000);
  check("usage: not again within three minutes", u.http.asked.size() == 1);
  u.advance(20000);
  check("usage: again after three", u.http.asked.size() == 2);
  check("usage: refreshUsage asks now", u.call(w3, "refreshUsage")["t"] == "ok" && u.http.asked.size() == 3);
  check("usage: the cache file holds numbers, never the token", u.files.has(P("C:\\state\\usage-cache.json")) && u.files.get(P("C:\\state\\usage-cache.json")).find("FAKE-TOKEN") == std::string::npos && u.files.get(P("C:\\state\\settings.json")).find("FAKE-TOKEN") == std::string::npos);
  { const std::string p = u.deck->saveUpload("my notes (1).txt", "hello");
    const std::string dir = P("C:\\state\\uploads\\");           // + "20270115-080312-" (16) + the safe name
    check("upload: kept under <state>/uploads, dated, the name made safe", p.compare(0, dir.size(), dir) == 0 && p.size() > dir.size() + 16 && p.substr(p.size() - 16) == "-my_notes_1_.txt" && u.files.get(p) == "hello" && u.deck->saveUpload("///", "x").substr(dir.size() + 16) == "file", p); }
  check("accounts: the editor is off unless the shell says the alias file may be written", u.call(w3, "saveAccounts", json::array({ json::array() }))["error"] == "not in the native local deck yet: saveAccounts");
  u.deck->shutdown();
  Rig e; e.opt.allowAccountEdit = true;
  const ClientId w4 = e.boot();
  { const json rows = e.res(w4, "saveAccounts", json::array({ json::array({ { { "keywords", "claudex, cx" }, { "dir", "~/.claude-x" } }, { { "keywords", "bad-word claude" }, { "dir", P("D:\\cfg\\y") } }, { { "keywords", "" }, { "dir", "z" } } }) }));
    const std::string text = e.files.get(ALIAS);
    bool bak = false; for (const std::string& n : e.files.list(P("C:\\home"))) if (n.compare(0, 34, ".claude-code-account-aliases.bak-2") == 0) bak = true;
    check("accounts (allowed): the alias file is rewritten in the form userswitch parses, the old one kept", text.find("alias claudex='CLAUDE_CONFIG_DIR=\"$HOME/.claude-x\" claude'\nalias cx='CLAUDE_CONFIG_DIR=\"$HOME/.claude-x\" claude'\n") != std::string::npos
      && text.find("bad-word") == std::string::npos && text.find("alias claude=") == std::string::npos && bak && rows.size() == 1 && rows[0]["id"] == "cx" && e.fresh()["accounts"][0]["id"] == "cx", text); }
  e.deck->shutdown();
}

}  // namespace

// ---------------------------------------------------------------- the deck's contract with the sessions it hosts (deck-state.json)
// office check 2026-11-20: a deck with no labels of its own shows the ones the shell found (the Electron deck's)
void testLabelFallback() {
  Rig r; r.opt.labelFallback = { { "t@example.com", "ts" } };
  const ClientId w = r.boot();
  const json a = r.call(w, "deskInit")["result"]["meta"]["accounts"];
  check("labels: none of its own -> the fallback's (\"ts\" for t@example.com); an account the fallback does not name gets no label from it",
    a[0]["label"] == "ts" && a[0]["abbr"] == "ts" && a[1]["abbr"].is_null() && a[1]["label"] == "claudeu", a.dump());
  r.deck->shutdown();
}
void testPresence() {
  const std::string STATE = P("C:\\state\\deck-state.json");
  Rig r; r.opt.allowClaude = true; r.opt.version = "9.9.9";
  r.make();
  const ClientId w = r.deck->addWindow();
  r.deck->start();
  json st = r.files.getJson(STATE);
  check("presence: the state file is written at start: office, no input yet, nothing connected", st["v"] == 1 && st["where"] == "office" && st["lastInputFrom"].is_null() && st["remote"].is_null() && st["phone"] == json{ { "attached", false } }
    && st["deck"]["version"] == "9.9.9" && st["deck"]["startedAt"] == st["since"] && st["deliver"] == "The user is at the office PC (the deck's own window): a local path, or opening the file on this PC, works.", st.dump());
  { const std::string text = r.files.get(STATE);
    check("presence: two-space JSON, keys in the contract's order, a final newline", text.compare(0, 13, "{\n  \"v\": 1,\n ") == 0 && text.back() == '\n' && text.find("\"deck\"") < text.find("\"where\"") && text.find("\"where\"") < text.find("\"since\"")
      && text.find("\"since\"") < text.find("\"remote\"") && text.find("\"remote\"") < text.find("\"phone\"") && text.find("\"phone\"") < text.find("\"lastInputFrom\"") && text.find("\"lastInputFrom\"") < text.find("\"deliver\""), text); }
  r.send(w, { { "t", "call" }, { "rid", 1 }, { "fn", "deskInit" }, { "args", json::array() } });
  r.advance(2100);
  // every tile's process is told which tile it is and where the file is
  const std::string sh = r.shell(w); fake::Pty* sp = r.ptys.last();
  const std::string cl = r.claude(w); fake::Pty* cp = r.ptys.last();
  check("presence: a shell's and a Claude session's environment name the tile and the state file", hasEnv(sp->spec.env, "CLAUDE_DECK_TILE", sh) && hasEnv(sp->spec.env, "CLAUDE_DECK_STATE", STATE) && hasEnv(sp->spec.env, "TERM_PROGRAM", "claude-deck")
    && hasEnv(cp->spec.env, "CLAUDE_DECK_TILE", cl) && hasEnv(cp->spec.env, "CLAUDE_DECK_STATE", STATE) && hasEnv(r.deck->lastClaudeLaunch(cl).env, "CLAUDE_DECK_TILE", cl));
  check("presence: every tile's environment names the deck's own state folder (CLAUDE_DECK_USERDATA, the deck-triggers skill's inbox)", hasEnv(sp->spec.env, "CLAUDE_DECK_USERDATA", P("C:\\state")) && hasEnv(cp->spec.env, "CLAUDE_DECK_USERDATA", P("C:\\state"))
    && hasEnv(r.deck->lastClaudeLaunch(cl).env, "CLAUDE_DECK_USERDATA", P("C:\\state")) && r.deck->stateDir() == P("C:\\state"), r.deck->stateDir());
  check("presence: the meta carries it (and no remoteDesk key: that key means the driver claim on this protocol)", r.fresh()["presence"] == json{ { "where", "office" }, { "since", st["since"] }, { "lastInputFrom", nullptr } } && !r.fresh().contains("remoteDesk"), r.fresh()["presence"].dump());
  int writes = r.files.atomicWrites;
  r.advance(20000);
  { int mine = 0; (void)mine; }
  const std::string quiet = r.files.get(STATE);
  r.send(w, { { "t", "input" }, { "id", sh }, { "d", "\x1b[I" } });
  r.send(w, { { "t", "input" }, { "id", sh }, { "d", "\x1b[12;40R" } });
  check("presence: a quiet deck does not rewrite the file; a terminal's own replies are not input", r.files.get(STATE) == quiet && r.files.getJson(STATE)["lastInputFrom"].is_null());
  r.send(w, { { "t", "input" }, { "id", sh }, { "d", "d" } });
  st = r.files.getJson(STATE);
  check("presence: a key in a window: lastInputFrom office, written at once; `since` does not move", st["lastInputFrom"] == "office" && st["where"] == "office" && st["since"] == st["deck"]["startedAt"]);
  const std::string office = r.files.get(STATE);
  for (int i = 0; i < 200; i++) r.send(w, { { "t", "input" }, { "id", sh }, { "d", "x" } });
  check("presence: two hundred more keys change nothing", r.files.get(STATE) == office);
  (void)writes;

  // a phone
  const ClientId ph = r.deck->addRemote();
  r.advance(2000);
  check("presence: a phone page connected: phone.attached, and `where` stays (connected is not active)", r.files.getJson(STATE)["phone"]["attached"] == true && r.files.getJson(STATE)["where"] == "office");
  r.send(ph, { { "t", "attach" }, { "id", sh } });
  r.send(ph, { { "t", "input" }, { "id", sh }, { "d", "\x1b[1;5F\x1b[<64;40;12M" } });
  check("presence: what the phone page sends by itself while scrolling is not input", r.files.getJson(STATE)["where"] == "office");
  r.send(ph, { { "t", "input" }, { "id", sh }, { "d", "ls\r" } });
  st = r.files.getJson(STATE);
  check("presence: typed on the phone: where = phone, at once, and the line a session acts on", st["where"] == "phone" && st["lastInputFrom"] == "phone" && st["deliver"] == "The user is on the phone, not at this PC: keep it short. To hand something over, send files with SendUserFile, or give a link.", st.dump());
  check("presence: meta.presence follows", r.fresh()["presence"]["where"] == "phone");

  // a desk: a remote that came in on the desk's path is not a phone page; its deskInit names it
  r.deck->removeClient(ph);
  const ClientId dk = r.deck->addRemote(true);
  r.advance(2000);
  check("presence: a desk's socket before its deskInit is not a phone", r.files.getJson(STATE)["phone"]["attached"] == false && r.files.getJson(STATE)["remote"].is_null() && r.files.getJson(STATE)["deliver"].get<std::string>().find("last active on the phone, which is not connected now") != std::string::npos);
  r.call(dk, "deskInit", json::array({ { { "name", "  HOME-PC\x07 " }, { "kind", "native" } } }));
  r.advance(2000);
  st = r.files.getJson(STATE);
  check("presence: the connected desk: its kind and name; `where` still the phone", st["remote"] == json{ { "kind", "native" }, { "name", "HOME-PC" } } && st["where"] == "phone", st.dump());
  r.send(dk, { { "t", "input" }, { "id", sh }, { "d", "\x1b[<0;10;5M" } });
  st = r.files.getJson(STATE);
  check("presence: a mouse press from the desk is a hand on that mouse: where = remote", st["where"] == "remote" && st["deliver"] == "The user is on a remote desk (HOME-PC, native app), not at this PC: they cannot open files on this PC. To hand something over, send files with SendUserFile, or give a link.", st.dump());
  r.send(dk, { { "t", "input" }, { "id", sh }, { "d", "\x1b[<35;10;5M" } });
  r.send(w, { { "t", "input" }, { "id", sh }, { "d", "\x1b[<35;10;5M" } });
  check("presence: mouse motion is not input, from anywhere", r.files.getJson(STATE)["where"] == "remote");
  // R10: the desk shows the tile, so the window is refused — but a person typed at this PC
  r.send(dk, { { "t", "shown" }, { "ids", { sh } }, { "hidden", false } });
  r.send(w, { { "t", "input" }, { "id", sh }, { "d", "refused" } });
  check("presence: a key the driver rule refused still says the user is at the office", sp->writes.back() != "refused" && r.files.getJson(STATE)["where"] == "office");
  r.send(dk, { { "t", "call" }, { "rid", 0 }, { "fn", "userInput" }, { "args", { sh } } });
  check("presence: a desk's userInput: remote again (and the tile's driver keeps this tree's word)", r.files.getJson(STATE)["where"] == "remote" && r.tile(sh)["driver"] == "phone");
  r.deck->windowKey(w);
  check("presence: a key anywhere in the deck's own window (the shell reports it)", r.files.getJson(STATE)["where"] == "office");
  // the deck's own writes are not the user
  r.send(dk, { { "t", "input" }, { "id", sh }, { "d", "k" } });
  r.record(T_DIR, cp->pid(), str(r.tile(cl)["sessionId"]), "idle"); r.feed(cp, "\r\n> ");
  r.res(w, "holdAdd", { cl, { { "kind", "next" }, { "text", "held text" } } });
  r.advance(8000);
  check("presence: a held prompt the deck types is not input", cp->typed().find("held text") != std::string::npos && r.files.getJson(STATE)["where"] == "remote" && r.files.getJson(STATE)["lastInputFrom"] == "remote");

  // away
  check("presence: settings.awayMinutes is cleaned and saved", r.res(w, "saveSettings", json::array({ { { "awayMinutes", "5.4" } } }))["awayMinutes"] == 5 && r.files.getJson(P("C:\\state\\settings.json"))["awayMinutes"] == 5);
  r.advance(4 * 60000.0);
  const bool notYet = r.files.getJson(STATE)["where"] == "remote";
  r.advance(62000);
  st = r.files.getJson(STATE);
  check("presence: no input from any screen for that long: away, on the meta tick; lastInputFrom keeps the screen", notYet && st["where"] == "away" && st["lastInputFrom"] == "remote"
    && st["deliver"] == "No input on any screen for 5+ minutes (last input: a remote desk; a remote desk (HOME-PC, native app) is connected): where the user will read this is unknown. Give the local path AND send files with SendUserFile, or give a link.", st.dump());
  r.send(dk, { { "t", "input" }, { "id", sh }, { "d", "b" } });
  check("presence: any input ends it", r.files.getJson(STATE)["where"] == "remote");
  r.deck->shutdown();
  check("presence: the file is left in place when the deck quits", r.files.has(STATE));

  // a restart: the last input's screen carries over
  Rig b; b.files.files = r.files.files; b.clock.t = r.clock.t + 60000;
  b.opt.driverClaim = true;
  const ClientId w2 = b.boot();
  st = b.files.getJson(STATE);
  check("presence: a deck restarted from a remote desk does not claim the office; `since` carries over", st["where"] == "remote" && st["lastInputFrom"] == "remote" && st["since"] == r.files.getJson(STATE)["since"] && st["remote"].is_null()
    && st["deliver"] == "The user was last active on a remote desk, which is not connected now: they are not at this PC. To hand something over, send files with SendUserFile, or give a link.", st.dump());
  check("presence: awayMinutes survived the restart", b.fresh()["presence"]["where"] == "remote" && b.files.getJson(P("C:\\state\\settings.json"))["awayMinutes"] == 5);
  const ClientId d2 = b.deck->addRemote(true);
  b.call(d2, "deskInit", json::array({ { { "name", "laptop" } } }));
  check("meta.remoteDesk (when the shell turns it on): { name, kind }, kind 'browser' when the desk gave none; null with no desk", b.fresh()["remoteDesk"] == json{ { "name", "laptop" }, { "kind", "browser" } } && (b.deck->removeClient(d2), b.fresh()["remoteDesk"].is_null()));
  (void)w2;
  b.deck->shutdown();
}

// ---------------------------------------------------------------- the real runs of 2026-11-18, on fakes
// What the two runs against a real claude.exe showed (windows/HOST-NOTES.md "The real runs"), pinned here so no
// change can quietly undo it. The first run sat on Claude Code's "New MCP server found in .mcp.json" dialog (its
// folder was below a folder with a .mcp.json) and typed its line into it; the second, in a folder with none above
// it, passed. Nothing in the deck was wrong: each check below is main.js's own behaviour.
void testRealRuns(const std::string& fx) {
  Rig r; r.opt.allowClaude = true;
  const ClientId w = r.boot();
  const json f = loadFixture(fx, "startup");
  std::string mcp; for (const json& s : f["streams"]) if (str(s["name"]) == "startup-mcp-prompt.raw" && s["chunks"].size() == 1) mcp = str(s["chunks"][0]);

  // ---- run 1: a session on the MCP server dialog
  const std::string id = r.claude(w);
  fake::Pty* p = r.ptys.last();
  const std::string sid = str(r.tile(id)["sessionId"]);
  r.feed(p, mcp);
  r.advance(3999);
  check("real run 1: an MCP server dialog is not the deck's to answer: nothing is typed, nothing is nudged (autoTrust is on)", !mcp.empty() && p->writes.empty() && p->resizes.empty() && r.tile(id)["status"] == "starting", p->typed());
  r.advance(1);
  { const json m = r.fresh(); bool said = false;
    for (const json& b : m["restart"]["blockers"]) if (b["id"] == id && b["reason"] == "waiting for you (startup: MCP server prompt)") said = true;
    check("real run 1: four quiet seconds on it and the tile is WAITING on \"startup: MCP server prompt\" - never idle, so nothing of the deck's types into it",
      r.tile(id)["status"] == "waiting" && said && p->writes.empty(), m["restart"].dump()); }
  check("real run 1: and it is toasted as a session waiting for its person", !r.notes.toasts.empty() && r.notes.toasts.back().body == "waiting for you" && r.notes.toasts.back().tileId == id);
  { const json item = r.res(w, "holdAdd", { id, { { "kind", "next" }, { "text", "say the single word ok" } } });
    r.advance(11000);
    check("real run 1: a special prompt is held back while the dialog is up (the deck's own delivery types only at an idle prompt)",
      p->writes.empty() && r.tile(id)["held"][0]["note"] == "the session is waiting for you (a question is up)", r.tile(id)["held"].dump());
    r.res(w, "holdRemove", { str(item["id"]) }); }
  // what a person (or a test that does not look first) types goes to the process as typed: the dialog takes it
  r.send(w, { { "t", "call" }, { "rid", 0 }, { "fn", "userInput" }, { "args", { id } } });
  r.send(w, { { "t", "input" }, { "id", id }, { "d", "say the single word ok" } });
  r.send(w, { { "t", "input" }, { "id", id }, { "d", "\r" } });
  check("real run 1: a window's keys reach the process byte for byte, dialog or not (main.js writeTile does not look at the screen)",
    p->writes == std::vector<std::string>{ "say the single word ok", "\r" } && r.tile(id)["driver"] == "desktop" && r.tile(id)["fresh"] == false);
  // the dialog answered, Claude Code writes its record - at first with no `status` at all (seen in run 2)
  r.files.put(std::string(T_DIR) + P("\\sessions\\") + std::to_string(p->pid()) + ".json", json{ { "pid", p->pid() }, { "sessionId", sid }, { "cwd", P("C:\\work") }, { "startedAt", r.clock.t }, { "kind", "interactive" } }.dump());
  r.advance(2000);
  { const json t = r.tile(id);
    check("real run 1: its record, still without a status, reads idle (lib/sessions.js: status || 'idle'); the startup watch is over",
      t["status"] == "idle" && t["sleepBlocker"] == "nothing to resume yet" && r.fresh()["restart"]["blockers"].dump().find("waiting for you") == std::string::npos, t.dump()); }
  // no turn ran, so there is no transcript: sleep, and the wake must not --resume
  check("real run 1: sleep", r.call(w, "sleepTile", { id }, 3000)["result"] == true && r.tile(id)["status"] == "asleep");
  { const size_t spawned = r.ptys.spawned.size();
    check("real run 1: wake is taken", r.res(w, "wakeTile", { id }) == true);
    r.advance(0);
    check("real run 1: a session that never got a message has no transcript: it comes back under the SAME id with --session-id, not --resume (which would exit: \"No conversation found\")",
      r.ptys.spawned.size() == spawned + 1 && r.ptys.spawned.back().args == std::vector<std::string>{ "--session-id", sid } && r.tile(id)["sessionId"] == sid, js::toString(json(r.ptys.spawned.back().args)));
    p = r.ptys.last(); }
  // the old process's record is gone with it; were it left behind, a dead pid is not a live session
  r.files.put(std::string(T_DIR) + P("\\sessions\\4999.json"), json{ { "pid", 4999 }, { "sessionId", sid }, { "status", "idle" }, { "statusUpdatedAt", r.clock.t } }.dump());
  r.advance(2000);
  check("real run 1: a record left behind by a process that is gone does not make the woken tile idle: it is starting until ITS record exists", r.tile(id)["status"] == "starting");
  r.call(w, "closeTile", { id }, 7000);

  // ---- run 2: the trust prompt answered, one line typed as the deck's delivery types it, sleep, wake
  const std::string id2 = r.claude(w, "claudet", P("C:\\fresh"));
  fake::Pty* q = r.ptys.last();
  const std::string sid2 = str(r.tile(id2)["sessionId"]);
  r.feed(q, "\x1b[2J Quick safety check: Is this a project you created or one you trust?\r\n");
  r.advance(700);
  r.feed(q, "\r\n   Yes, I trust this folder\r\n \xE2\x9D\xAF No, exit\r\n Enter to confirm\r\n");
  r.advance(800); r.advance(800);
  r.feed(q, "\r\n \xE2\x9D\xAF Yes, I trust this folder\r\n   No, exit\r\n Enter to confirm\r\n");
  r.advance(800); r.advance(1800);
  r.feed(q, "\x1b[2J\x1b[H Claude Code\r\n");
  r.advance(800); r.advance(5000);
  check("real run 2: the trust prompt: Down off \"No\", Enter on \"Yes\", three nudges - 119, 118, 117 - and no key after it is gone",
    q->writes == std::vector<std::string>{ "\x1b[B", "\r" } && q->resizes == std::vector<std::pair<int, int>>{ { 119, 30 }, { 118, 30 }, { 117, 30 } }, q->typed());
  check("real run 2: the pty is left at the last nudge and the meta says so (the view that shows the tile sizes it back, as the Electron window does)",
    r.tile(id2)["ptySize"] == json{ { "cols", 117 }, { "rows", 30 } }, r.tile(id2)["ptySize"].dump());
  r.record(T_DIR, q->pid(), sid2, "idle"); r.feed(q, "\r\n\xE2\x9D\xAF "); r.advance(2000);
  { const size_t writes = q->writes.size();
    r.send(w, { { "t", "call" }, { "rid", 0 }, { "fn", "userInput" }, { "args", { id2 } } });
    r.send(w, { { "t", "input" }, { "id", id2 }, { "d", "\x1b[200~say the single word ok\x1b[201~" } });
    r.advance(150);
    r.send(w, { { "t", "input" }, { "id", id2 }, { "d", "\r" } });
    check("real run 2: a paste and its Enter from the window reach the process whole and in order",
      q->writes.size() == writes + 2 && q->writes[writes] == "\x1b[200~say the single word ok\x1b[201~" && q->writes[writes + 1] == "\r"); }
  r.record(T_DIR, q->pid(), sid2, "busy"); r.advance(2000);
  const bool wasBusy = r.tile(id2)["status"] == "busy";
  r.transcript(T_DIR, sid2, "Single word acknowledgment", "ok");
  r.record(T_DIR, q->pid(), sid2, "idle"); r.advance(2000);
  { const json t = r.tile(id2);
    check("real run 2: busy, then idle; the transcript - found by the session's id under whatever folder name Claude Code gave the project - gives the model and the last message",
      wasBusy && t["status"] == "idle" && t["model"] == "claude-fable-5-1" && t["lastText"] == "ok" && t["title"] == "Single word acknowledgment", t.dump()); }
  // the transcript's folder is Claude Code's own mangling of the cwd: the deck never builds that name, it looks in every project folder
  { Rig other; other.opt.allowClaude = true;
    const ClientId ow = other.boot();
    const std::string oid = other.claude(ow, "claudet", P("C:\\Users\\Public\\deck-host-test-1-claude-real-work"));
    const std::string osid = str(other.tile(oid)["sessionId"]);
    other.files.put(std::string(T_DIR) + P("\\projects\\C--Users-Public-deck-host-test-1-claude-real-work\\") + osid + ".jsonl",
      json{ { "type", "assistant" }, { "timestamp", "2027-01-15T08:00:05.000Z" }, { "message", { { "role", "assistant" }, { "model", "claude-opus-5-5" }, { "content", json::array({ { { "type", "text" }, { "text", "ok" } } }) } } } }.dump() + "\n");
    other.advance(10000);
    check("real run 2: a transcript under C--Users-Public-… (the real run's folder name) is found and read", other.tile(oid)["lastText"] == "ok" && other.tile(oid)["model"] == "claude-opus-5-5" && other.tile(oid)["sleepBlocker"] != "nothing to resume yet", other.tile(oid).dump());
    other.deck->shutdown(); }
  check("real run 2: sleep", r.call(w, "sleepTile", { id2 }, 3000)["result"] == true && r.tile(id2)["status"] == "asleep" && r.tile(id2)["lastText"] == "ok");
  { const size_t spawned = r.ptys.spawned.size(); const size_t at = r.out[w].size();
    // a terminal's own reply to a query in the sleeping tile's last output travels the input path too: it wakes
    // the tile, as any input does in main.js (writeTile), and is itself dropped
    r.send(w, { { "t", "input" }, { "id", id2 }, { "d", "\x1b[?1;2c" } });
    check("real run 2: any input to a sleeping tile wakes it - a view's own terminal reply too, as in main.js - so a later wakeTile answers false", r.tile(id2)["status"] == "starting" && r.res(w, "wakeTile", { id2 }) == false);
    r.advance(0);
    check("real run 2: the wake resumes the same session (--resume: its transcript exists), the screen is reset first, the reply is dropped",
      r.ptys.spawned.size() == spawned + 1 && r.ptys.spawned.back().args == std::vector<std::string>{ "--resume", sid2 } && r.count(w, "reset", id2, at) == 1 && r.ptys.last()->writes.empty());
    q = r.ptys.last(); }
  r.advance(2000);
  check("real run 2: the woken tile is starting until the NEW process writes its record, then idle", r.tile(id2)["status"] == "starting" && (r.record(T_DIR, q->pid(), sid2, "idle"), r.advance(2000), r.tile(id2)["status"] == "idle"));
  r.deck->shutdown();
}

// ---------------------------------------------------------------- refresh every session once the restore is done (restorerefresh.hpp)
// The pty a restored or refreshed session runs in: the newest live one whose launch names the session.
static fake::Pty* ptyOf(Rig& r, const std::string& sid) {
  fake::Pty* best = nullptr;
  for (auto& kv : r.ptys.live) for (const std::string& a : kv.second->spec.args) if (a == sid && !kv.second->killed && (!best || kv.second->token > best->token)) best = kv.second;
  return best;
}
void testRestoreRefresh() {
  Rig a; a.opt.allowClaude = true;
  const ClientId w = a.boot();
  for (int i = 0; i < 3; i++) { const std::string id = a.claude(w); a.transcript(T_DIR, str(a.tile(id)["sessionId"]), "t" + std::to_string(i), "x"); }
  a.advance(2100);
  a.deck->shutdown();

  Rig b; b.opt.allowClaude = true; b.files.files = a.files.files; b.clock.t = a.clock.t + 60000;
  for (auto it = b.files.files.begin(); it != b.files.files.end();) { if (it->first.find(P("\\sessions\\")) != std::string::npos) it = b.files.files.erase(it); else ++it; }   // the old processes' records
  // the restore's order (tiles.json's) is the refreshes' order
  const json saved = b.tilesFile(); std::vector<std::string> sid; for (const json& t : saved["tiles"]) sid.push_back(str(t["sessionId"]));
  b.make();
  const ClientId w2 = b.deck->addWindow();
  b.deck->start(); b.call(w2, "deskInit");
  b.advance(4000);
  const size_t restored = b.ptys.spawned.size();
  check("restore refresh: the three sessions are restored and nothing is refreshed while none is idle yet", sid.size() == 3 && restored == 3 && (b.advance(20000), b.ptys.spawned.size() == 3), std::to_string(b.ptys.spawned.size()));
  auto rec = [&](int i, const char* status) { b.record(T_DIR, ptyOf(b, sid[i])->pid(), sid[i], status); };
  rec(0, "idle"); rec(1, "idle"); rec(2, "busy");
  b.advance(10000);
  check("restore refresh: one still busy: the restore is not over, nothing is refreshed", b.ptys.spawned.size() == 3);
  // (a pty the deck has let go of is destroyed: hold its token, never the pointer, across a kill)
  const uint64_t tok0 = ptyOf(b, sid[0])->token;
  rec(2, "idle");
  b.advance(2100);
  const bool asked = b.ptys.byToken(tok0) && b.ptys.byToken(tok0)->typed().find('\x03') != std::string::npos;
  b.advance(3500);
  check("restore refresh: every restored session idle: the first is refreshed (asked to exit, then the same session resumed)", asked && (!b.ptys.byToken(tok0) || b.ptys.byToken(tok0)->killed) && b.ptys.spawned.size() == 4
    && b.ptys.spawned[3].args == std::vector<std::string>{ "--resume", sid[0] }, std::to_string(b.ptys.spawned.size()));
  b.advance(10000);
  check("restore refresh: one at a time: the next waits until the refreshed one is idle again", b.ptys.spawned.size() == 4 && !ptyOf(b, sid[1])->killed);
  rec(2, "busy");          // the last one starts a turn before its refresh
  rec(0, "idle");
  b.advance(10000);
  check("restore refresh: back idle: the next one (after the short gap)", b.ptys.spawned.size() == 5 && b.ptys.spawned[4].args == std::vector<std::string>{ "--resume", sid[1] }, std::to_string(b.ptys.spawned.size()));
  rec(1, "idle");
  b.advance(5 * 60000.0);
  check("restore refresh: a session mid-turn is never interrupted", b.ptys.spawned.size() == 5 && !ptyOf(b, sid[2])->killed && ptyOf(b, sid[2])->typed().find('\x03') == std::string::npos);
  b.advance(6 * 60000.0);
  rec(2, "idle");
  b.advance(60000);
  check("restore refresh: still busy 10 minutes after the restore ended: skipped, and once per start means no refresh after", b.ptys.spawned.size() == 5 && !ptyOf(b, sid[2])->killed && (b.advance(30 * 60000.0), b.ptys.spawned.size() == 5));
  check("log: restore refresh - one line per refresh, then its end", b.log.count("[restore-refresh] refreshing ") == 2 && b.log.count(" (2 to go)") == 1 && b.log.count(" (1 to go)") == 1
    && b.log.count("[restore-refresh] done: 2 refreshed") == 1, b.log.all());
  b.deck->shutdown();
}

// ---------------------------------------------------------------- Pause / Resume / Pause until restart (pause.hpp)
// the session answers: one assistant message appended to its transcript, stamped with the rig's clock
static void answer(Rig& r, const std::string& sid, const std::string& text) {
  const std::string f = r.transcriptPath(T_DIR, sid);
  r.files.put(f, r.files.get(f) + json{ { "type", "assistant" }, { "timestamp", isoTime(r.clock.t) }, { "message", { { "role", "assistant" }, { "model", "claude-fable-5-1" },
    { "content", json::array({ { { "type", "text" }, { "text", text } } }) } } } }.dump() + "\n");
}
void testPause() {
  Rig r; r.opt.allowClaude = true; r.clock.offset = -5 * 3600000.0;
  const ClientId w = r.boot();
  const std::string id = r.claude(w); fake::Pty* p = r.ptys.last(); const std::string sid = str(r.tile(id)["sessionId"]);
  r.transcript(T_DIR, sid, "Long job", "x"); r.advance(10);
  r.record(T_DIR, p->pid(), sid, "busy"); r.advance(2000);
  const std::string file = P("C:\\state\\pause\\") + sid + ".md";
  const size_t w0 = p->writes.size();
  check("pause: refused for a tile the deck did not pause (Resume)", r.call(w, "resumeTile", { id })["error"] == "the deck did not pause this session");
  check("pause: taken", r.res(w, "pauseTile", { id, json::object() }) == true && r.tile(id)["pause"]["state"] == "pausing");
  check("pause: Esc first, at once (the turn is interrupted)", p->writes.size() == w0 + 1 && p->writes[w0] == "\x1b");
  r.advance(1000);
  const std::string typed = p->typed();
  check("pause: then the PAUSE prompt as a paste naming the pause file, and Enter", typed.find("\x1b[200~PAUSE: " DECK_EDITION_THE " is about to restart.") != std::string::npos && typed.find("Write " + file + ":") != std::string::npos
    && typed.find("PAUSED <your session name> - " + file + ".") != std::string::npos && p->writes.back() == "\r", typed.substr(0, 120));
  check("pause: saved with the tile", r.tilesFile()["tiles"][0]["pause"]["state"] == "pausing" && r.tilesFile()["tiles"][0]["pause"]["untilRestart"] == false);
  r.advance(4000);
  check("pause: still pausing while the session works", r.tile(id)["pause"]["state"] == "pausing");
  answer(r, sid, "Stopped the watcher.\nPAUSED t-1 - " + file); r.advance(4000);
  check("pause: its PAUSED line: paused", r.tile(id)["pause"]["state"] == "paused" && r.tilesFile()["tiles"][0]["pause"]["state"] == "paused", r.tile(id)["pause"].dump());
  // Resume while it is busy: queued, sent when the turn ends
  check("resume: taken while busy, nothing typed yet", (r.advance(10), true) && r.res(w, "resumeTile", { id }) == true && r.tile(id)["pause"]["want"] == true);
  const size_t w1 = p->writes.size();
  r.advance(6000);
  check("resume: never into a turn that is running", p->writes.size() == w1);
  r.record(T_DIR, p->pid(), sid, "idle"); r.advance(4000);
  const std::string typed2 = p->typed().substr(typed.size());
  check("resume: idle: the RESUME prompt, with the deck's start in local time, once", typed2.find("\x1b[200~RESUME: " DECK_EDITION_THE " restarted at 2027-01-15 03:00:") != std::string::npos && typed2.find(" -05:00. Read " + file + ".") != std::string::npos
    && typed2.compare(0, 6, "\x1b[200~") == 0 && r.tile(id)["pause"]["state"] == "resuming", typed2.substr(0, 120));
  check("resume: a resume under way is not kept in tiles.json", !r.tilesFile()["tiles"][0].contains("pause"));
  r.record(T_DIR, p->pid(), sid, "busy"); r.advance(2000); answer(r, sid, "RESUMED t-1"); r.record(T_DIR, p->pid(), sid, "idle"); r.advance(4000);
  check("resume: its turn ended: no longer paused", r.tile(id)["pause"].is_null(), r.tile(id)["pause"].dump());
  r.deck->shutdown();

  // ---- Pause until restart: at the next start the restore refresh first, then RESUME, once
  Rig a; a.opt.allowClaude = true;
  const ClientId wa = a.boot();
  const std::string ida = a.claude(wa); fake::Pty* pa = a.ptys.last(); const std::string sa = str(a.tile(ida)["sessionId"]);
  a.transcript(T_DIR, sa, "Watcher", "x"); a.advance(10); a.record(T_DIR, pa->pid(), sa, "idle"); a.advance(2000);
  a.res(wa, "pauseTile", { ida, { { "untilRestart", true } } }); a.advance(1000);
  a.record(T_DIR, pa->pid(), sa, "busy"); a.advance(2000); answer(a, sa, "PAUSED w - x"); a.record(T_DIR, pa->pid(), sa, "idle"); a.advance(4000);
  check("until restart: paused, and the mark is saved with the tile", a.tile(ida)["pause"]["state"] == "paused" && a.tile(ida)["pause"]["untilRestart"] == true && a.tilesFile()["tiles"][0]["pause"]["untilRestart"] == true);
  const size_t wa0 = pa->writes.size(); a.advance(60000);
  check("until restart: no Resume before the restart", pa->writes.size() == wa0);
  a.deck->shutdown();

  Rig b; b.opt.allowClaude = true; b.files.files = a.files.files; b.clock.t = a.clock.t + 60000;
  for (auto it = b.files.files.begin(); it != b.files.files.end();) { if (it->first.find(P("\\sessions\\")) != std::string::npos) it = b.files.files.erase(it); else ++it; }
  b.make();
  const ClientId wb = b.deck->addWindow();
  b.deck->start(); b.call(wb, "deskInit");
  b.advance(3000);
  fake::Pty* first = ptyOf(b, sa);
  check("until restart: restored as paused with its Resume queued", first && b.tile(ida)["pause"]["state"] == "paused" && b.tile(ida)["pause"]["want"] == true);
  const uint64_t tok1 = first->token;
  b.record(T_DIR, first->pid(), sa, "idle"); b.advance(2100);
  const bool noResume = b.ptys.byToken(tok1) && b.ptys.byToken(tok1)->typed().find("RESUME:") == std::string::npos && b.ptys.byToken(tok1)->typed().find('\x03') != std::string::npos;
  b.advance(3500);
  fake::Pty* second = ptyOf(b, sa);
  check("until restart: idle after the restore: the refresh comes first (no RESUME into the restored process)", noResume && second && second->token != tok1 && b.ptys.spawned.size() == 2,
    std::to_string(noResume) + " " + b.tile(ida)["pause"].dump() + " " + std::to_string(b.ptys.spawned.size()));
  b.advance(6000);
  check("until restart: no RESUME while the refreshed process is not idle yet", second->typed().find("RESUME:") == std::string::npos);
  b.record(T_DIR, second->pid(), sa, "idle"); b.advance(4000);
  check("until restart: refreshed and idle: RESUME, once; the mark is cleared", second->typed().find("\x1b[200~RESUME: " DECK_EDITION_THE " restarted at") != std::string::npos && b.tile(ida)["pause"]["state"] == "resuming"
    && b.tile(ida)["pause"]["untilRestart"] == false && (b.advance(20000), second->typed().find("RESUME:") == second->typed().rfind("RESUME:")), b.tile(ida)["pause"].dump());
  b.deck->shutdown();
}

// ---- a /clear after Pause: the tile follows the new session id and the pause ENDS - the deck's own Pause and Pause until
// restart alike (2026-11-24: "A pause from the deck's own Pause button isn't cleared by a /clear - can we fix this").
// No RESUME goes, then or at the next start; the pause file stays as the session wrote it.
static void testPauseClear() {
  Rig r; r.opt.allowClaude = true; r.clock.offset = -5 * 3600000.0;
  const ClientId w = r.boot();
  const std::string id = r.claude(w); fake::Pty* p = r.ptys.last(); const std::string sid = str(r.tile(id)["sessionId"]);
  r.transcript(T_DIR, sid, "Long job", "x"); r.record(T_DIR, p->pid(), sid, "idle"); r.advance(2000);
  const std::string file = P("C:\\state\\pause\\") + sid + ".md";
  check("pause + clear: taken", r.res(w, "pauseTile", { id, json::object() }) == true);
  r.advance(1000);
  check("pause + clear: the file and the session id are kept with the pause in tiles.json", r.tilesFile()["tiles"][0]["pause"].value("file", std::string()) == file
    && r.tilesFile()["tiles"][0]["pause"].value("sid", std::string()) == sid, r.tilesFile()["tiles"][0]["pause"].dump());
  r.files.put(file, "task: the export; next step: the header row\n");          // the session writes its pause file
  answer(r, sid, "PAUSED t-1 - " + file); r.advance(4000);
  check("pause + clear: paused", r.tile(id)["pause"]["state"] == "paused", r.tile(id)["pause"].dump());
  const std::string sid2 = "5e55a1d2-1111-4222-8333-0000000000a2";
  const size_t n0 = p->typed().size();
  r.record(T_DIR, p->pid(), sid2, "idle"); r.advance(4000);
  check("pause + clear: the tile followed the /clear and the pause is gone (tile and tiles.json)", r.tile(id)["sessionId"] == sid2 && r.tile(id)["pause"].is_null()
    && !r.tilesFile()["tiles"][0].contains("pause"), r.tile(id).dump());
  check("pause + clear: Resume is refused now (nothing to resume)", r.call(w, "resumeTile", { id })["error"] == "the deck did not pause this session");
  r.advance(10000);
  check("pause + clear: no RESUME typed", p->typed().substr(n0).find("RESUME:") == std::string::npos, p->typed().substr(n0).substr(0, 160));
  check("pause + clear: the pause file is still there, untouched", r.files.has(file) && r.files.get(file) == "task: the export; next step: the header row\n");
  const std::string t8 = id.substr(0, 8);
  check("log: pause, paused, the follow, the pause ended - one line each, in order, timestamped in local time",
    r.log.count("[pause] " + t8 + ": PAUSE sent (" + file + ")") == 1 && r.log.count("[pause] " + t8 + ": pausing -> paused") == 1
    && r.log.count("[follow] tile " + t8 + ": session " + sid + " -> " + sid2) == 1 && r.log.count("[pause] " + t8 + ": another prompt after the pause, or a /clear (a new session id): no longer paused") == 1
    && r.log.count("[pause] resuming " + t8) == 0 && r.log.lines.size() >= 4 && r.log.lines[0].find(" -05:00 [") == 19, r.log.all());

  // ---- Pause until restart, then a /clear before the restart: no RESUME by itself at the next start
  const std::string id2 = r.claude(w); fake::Pty* p2 = r.ptys.last(); const std::string s2 = str(r.tile(id2)["sessionId"]);
  r.transcript(T_DIR, s2, "Watcher", "x"); r.record(T_DIR, p2->pid(), s2, "idle"); r.advance(2000);
  r.res(w, "pauseTile", { id2, { { "untilRestart", true } } }); r.advance(1000);
  answer(r, s2, "PAUSED w - x"); r.advance(4000);
  check("until restart + clear: paused until restart", r.tile(id2)["pause"]["state"] == "paused" && r.tile(id2)["pause"]["untilRestart"] == true, r.tile(id2)["pause"].dump());
  const std::string s3 = "5e55a1d2-1111-4222-8333-0000000000a3";
  r.record(T_DIR, p2->pid(), s3, "idle"); r.advance(4000);
  const json tf = r.tilesFile(); json kept; for (const json& t : tf["tiles"]) if (t["id"] == id2) kept = t;
  check("until restart + clear: the mark is gone, from tiles.json too (nothing left to resume at the next start)", r.tile(id2)["pause"].is_null() && kept.is_object() && !kept.contains("pause"), tf.dump().substr(0, 600));
  r.deck->shutdown();

  Rig b; b.opt.allowClaude = true; b.files.files = r.files.files; b.clock.t = r.clock.t + 60000;
  for (auto it = b.files.files.begin(); it != b.files.files.end();) { if (it->first.find(P("\\sessions\\")) != std::string::npos) it = b.files.files.erase(it); else ++it; }
  b.make();
  const ClientId wb = b.deck->addWindow();
  b.deck->start(); b.call(wb, "deskInit");
  bool resumed = false;
  for (int i = 0; i < 8; i++) {
    if (fake::Pty* q = ptyOf(b, s3)) b.record(T_DIR, q->pid(), s3, "idle");
    if (fake::Pty* q = ptyOf(b, sid2)) b.record(T_DIR, q->pid(), sid2, "idle");
    b.advance(5000);
    for (auto& kv : b.ptys.live) if (kv.second && kv.second->typed().find("RESUME:") != std::string::npos) resumed = true;
  }
  check("until restart + clear: the next start sends no RESUME", !resumed && b.tile(id2)["pause"].is_null(), b.tile(id2).dump());
  b.deck->shutdown();
}

// ---- a PAUSE the deck did not type (the user pastes it into the tile, or types it): read off the transcript, the tile marked
// until the session gets another prompt (2026-11-24). pause.hpp's `by: prompt`; the next prompt ends any pause.
// a prompt a person sent: a user record (origin human), or a queued_command attachment (sent mid-turn)
static void prompt(Rig& r, const std::string& sid, const std::string& text, bool queued = false) {
  const std::string f = r.transcriptPath(T_DIR, sid);
  const json rec = queued
    ? json{ { "type", "attachment" }, { "timestamp", isoTime(r.clock.t) }, { "attachment", { { "type", "queued_command" }, { "prompt", text }, { "commandMode", "prompt" }, { "origin", { { "kind", "human" } } }, { "timestamp", isoTime(r.clock.t) } } } }
    : json{ { "type", "user" }, { "timestamp", isoTime(r.clock.t) }, { "origin", { { "kind", "human" } } }, { "message", { { "role", "user" }, { "content", text } } } };
  r.files.put(f, r.files.get(f) + rec.dump() + "\n");
}
static void testPauseMark() {
  const std::string pasted = std::string("\n\n<pasted_content id=\"f150\">\n") + pausePromptText() + "\n</pasted_content>";
  Rig a; a.opt.allowClaude = true; a.clock.offset = -5 * 3600000.0;
  const ClientId w = a.boot();
  const std::string id = a.claude(w); fake::Pty* p = a.ptys.last(); const std::string sid = str(a.tile(id)["sessionId"]);
  a.transcript(T_DIR, sid, "Long job", "x"); a.record(T_DIR, p->pid(), sid, "idle"); a.advance(3000);
  check("pause mark: no PAUSE prompt, no mark", a.tile(id)["pause"].is_null());
  const size_t w0 = p->writes.size();
  prompt(a, sid, pasted); a.record(T_DIR, p->pid(), sid, "busy"); a.advance(4000);
  check("pause mark: a pasted PAUSE in the transcript: the tile says pausing, by its prompt", a.tile(id)["pause"]["state"] == "pausing" && a.tile(id)["pause"]["by"] == "prompt", a.tile(id)["pause"].dump());
  check("pause mark: the deck typed nothing (no Esc, no prompt of its own)", p->writes.size() == w0);
  check("pause mark: saved with the tile (by, its session id; no mark until restart)", a.tilesFile()["tiles"][0]["pause"]["by"] == "prompt" && a.tilesFile()["tiles"][0]["pause"]["sid"] == sid
    && a.tilesFile()["tiles"][0]["pause"]["untilRestart"] == false, a.tilesFile()["tiles"][0].dump());
  answer(a, sid, "Stopped the watcher.\nPAUSED t-1 - C:\\x.md"); a.record(T_DIR, p->pid(), sid, "idle"); a.advance(4000);
  check("pause mark: its PAUSED line: paused", a.tile(id)["pause"]["state"] == "paused", a.tile(id)["pause"].dump());
  a.advance(120000);
  check("pause mark: two minutes idle: still paused, and no RESUME of the deck's own", a.tile(id)["pause"]["state"] == "paused" && p->typed().find("RESUME:") == std::string::npos);
  const std::string t8 = id.substr(0, 8);
  check("pause mark: logged once", a.log.count("[pause] " + t8 + ": a PAUSE prompt in its transcript (not typed by the deck): pausing") == 1 && a.log.count("[pause] " + t8 + ": pausing -> paused") == 1, a.log.all());
  a.deck->shutdown();

  // ---- the deck restarts with settings.resumeAfterRestart OFF: the mark comes back (tiles.json, and the transcript
  // agrees) and no RESUME goes by itself (ON, the default: testPauseResumeSetting)
  Rig b; b.opt.allowClaude = true; b.files.files = a.files.files; b.clock.t = a.clock.t + 60000;
  { json st = b.files.getJson(P("C:\\state\\settings.json")); if (!st.is_object()) st = json::object(); st["resumeAfterRestart"] = false; b.files.put(P("C:\\state\\settings.json"), st.dump()); }
  for (auto it = b.files.files.begin(); it != b.files.files.end();) { if (it->first.find(P("\\sessions\\")) != std::string::npos) it = b.files.files.erase(it); else ++it; }
  b.make();
  const ClientId wb = b.deck->addWindow();
  b.deck->start(); b.call(wb, "deskInit");
  b.advance(3000);
  check("pause mark: after a restart (resume setting off): still paused, by its prompt, nothing queued", b.tile(id)["pause"]["state"] == "paused" && b.tile(id)["pause"]["by"] == "prompt" && b.tile(id)["pause"]["want"] == false, b.tile(id)["pause"].dump());
  bool resumed = false;
  for (int i = 0; i < 6; i++) {
    if (fake::Pty* q = ptyOf(b, sid)) b.record(T_DIR, q->pid(), sid, "idle");
    b.advance(5000);
    for (auto& kv : b.ptys.live) if (kv.second && kv.second->typed().find("RESUME:") != std::string::npos) resumed = true;
  }
  check("pause mark: restored, refreshed and idle for 30 s: no RESUME typed (setting off: only a mark until restart does that)", !resumed && b.tile(id)["pause"]["state"] == "paused", b.tile(id)["pause"].dump());
  // the user's next prompt (any) ends it
  prompt(b, sid, "carry on with the export"); b.advance(4000);
  check("pause mark: another prompt: the mark goes, from the tile and from tiles.json", b.tile(id)["pause"].is_null() && !b.tilesFile()["tiles"][0].contains("pause"), b.tile(id)["pause"].dump());
  check("pause mark: that logged", b.log.count("[pause] " + t8 + ": another prompt after the pause, or a /clear (a new session id): no longer paused") == 1, b.log.all());
  b.advance(10000);
  check("pause mark: and it does not come back", b.tile(id)["pause"].is_null());
  b.deck->shutdown();

  // ---- a start that finds the PAUSE only in the transcript (tiles.json without the mark): seen during the restore,
  // when saveState is skipped, and saved once the restore is over
  Rig d; d.opt.allowClaude = true; d.files.files = a.files.files; d.clock.t = a.clock.t + 60000;
  for (auto it = d.files.files.begin(); it != d.files.files.end();) { if (it->first.find(P("\\sessions\\")) != std::string::npos) it = d.files.files.erase(it); else ++it; }
  { json tj = d.tilesFile(); tj["tiles"][0].erase("pause"); d.files.put(P("C:\\state\\tiles.json"), tj.dump()); }
  d.make();
  const ClientId wd = d.deck->addWindow();
  d.deck->start(); d.call(wd, "deskInit");
  d.advance(4000);
  check("pause mark: a start with the PAUSE only in the transcript: paused within seconds", d.tile(id)["pause"]["state"] == "paused" && d.tile(id)["pause"]["by"] == "prompt", d.tile(id)["pause"].dump());
  for (int i = 0; i < 4; i++) { if (fake::Pty* q = ptyOf(d, sid)) d.record(T_DIR, q->pid(), sid, "idle"); d.advance(5000); }
  check("pause mark: ...and saved to tiles.json once the restore is over", d.tilesFile()["tiles"][0].contains("pause") && d.tilesFile()["tiles"][0]["pause"]["by"] == "prompt", d.tilesFile()["tiles"][0].dump());
  d.deck->shutdown();

  // ---- pasted mid-turn (a queued_command, no user record); RESUME pasted by hand ends it; a /clear ends a seen mark
  Rig c; c.opt.allowClaude = true;
  const ClientId wc = c.boot();
  const std::string ic = c.claude(wc); fake::Pty* pc = c.ptys.last(); const std::string sc = str(c.tile(ic)["sessionId"]);
  c.transcript(T_DIR, sc, "Busy one", "x"); c.record(T_DIR, pc->pid(), sc, "busy"); c.advance(3000);
  prompt(c, sc, pasted, true); c.advance(4000);
  check("pause mark: a PAUSE sent mid-turn (queued_command): pausing", c.tile(ic)["pause"]["state"] == "pausing" && c.tile(ic)["pause"]["by"] == "prompt", c.tile(ic)["pause"].dump());
  c.advance(10); prompt(c, sc, "RESUME: the Claude Deck restarted at {restartedAt}. Read {pauseFile}."); c.advance(4000);
  check("pause mark: RESUME pasted by hand: the mark goes", c.tile(ic)["pause"].is_null(), c.tile(ic)["pause"].dump());
  c.advance(10); prompt(c, sc, "PAUSE: again"); c.advance(4000);
  check("pause mark: a second PAUSE marks it again", c.tile(ic)["pause"]["state"] == "pausing");
  const std::string sc2 = "5e55a1d2-1111-4222-8333-0000000000a4";
  c.record(T_DIR, pc->pid(), sc2, "idle"); c.advance(4000);
  check("pause mark: a /clear (the tile follows the new session id): the seen mark goes", c.tile(ic)["sessionId"] == sc2 && c.tile(ic)["pause"].is_null(), c.tile(ic).dump());

  // ---- the deck's own Pause: the next prompt ends it too
  const std::string id2 = c.claude(wc); fake::Pty* p2 = c.ptys.last(); const std::string s2 = str(c.tile(id2)["sessionId"]);
  c.transcript(T_DIR, s2, "Second", "x"); c.record(T_DIR, p2->pid(), s2, "idle"); c.advance(3000);
  c.res(wc, "pauseTile", { id2, json::object() }); c.advance(1000);
  prompt(c, s2, pasted); answer(c, s2, "PAUSED t-2 - x"); c.advance(4000);
  check("pause mark: the deck's own Pause: its PAUSE in the transcript keeps it the deck's", c.tile(id2)["pause"]["state"] == "paused" && !c.tile(id2)["pause"].contains("by"), c.tile(id2)["pause"].dump());
  c.advance(10); prompt(c, s2, "never mind, keep going"); c.advance(4000);
  check("pause mark: the deck's own Pause ends with the next prompt", c.tile(id2)["pause"].is_null(), c.tile(id2)["pause"].dump());
  c.deck->shutdown();
}

// ---- settings.resumeAfterRestart (2026-11-24: "have the 'send resume on restart after pause' as a setting,
// default on"): on, every tile still paused at the start gets RESUME (the deck's Pause and a seen PAUSE alike); off, only
// "Pause until restart" does
static void testPauseResumeSetting() {
  Rig a; a.opt.allowClaude = true;
  const ClientId w = a.boot();
  const std::string i1 = a.claude(w); fake::Pty* p1 = a.ptys.last(); const std::string s1 = str(a.tile(i1)["sessionId"]);
  const std::string i2 = a.claude(w); fake::Pty* p2 = a.ptys.last(); const std::string s2 = str(a.tile(i2)["sessionId"]);
  a.transcript(T_DIR, s1, "Deck paused", "x"); a.transcript(T_DIR, s2, "Pasted pause", "y");
  a.record(T_DIR, p1->pid(), s1, "idle"); a.record(T_DIR, p2->pid(), s2, "idle"); a.advance(3000);
  a.res(w, "pauseTile", { i1, json::object() }); a.advance(1000);
  answer(a, s1, "PAUSED t-1 - x"); a.advance(10);
  prompt(a, s2, "PAUSE: the Claude Deck is about to restart."); a.advance(10); answer(a, s2, "PAUSED t-2 - y"); a.advance(4000);
  check("resume setting: the deck's Pause and a pasted PAUSE, both paused, neither until restart", a.tile(i1)["pause"]["state"] == "paused" && a.tile(i2)["pause"]["state"] == "paused"
    && a.tile(i1)["pause"]["untilRestart"] == false && a.tile(i2)["pause"]["by"] == "prompt", a.tile(i1)["pause"].dump() + " " + a.tile(i2)["pause"].dump());
  const std::string sf = P("C:\\state\\settings.json");
  auto setOf = [&](Rig& x) { json j = x.files.getJson(sf); return j.is_object() ? j : json::object(); };
  check("resume setting: on by default (nothing in settings.json)", !setOf(a).contains("resumeAfterRestart") || setOf(a)["resumeAfterRestart"] == true);
  a.res(w, "saveSettings", { { { "resumeAfterRestart", false } } });
  check("resume setting: saveSettings keeps it (false) in settings.json", setOf(a).value("resumeAfterRestart", true) == false, setOf(a).dump().substr(0, 200));
  a.res(w, "saveSettings", { { { "resumeAfterRestart", true } } });
  check("resume setting: and true again", setOf(a).value("resumeAfterRestart", false) == true);
  a.deck->shutdown();

  // the next start, with the setting on or off: which tiles get RESUME by themselves
  auto restart = [&](const json& setting, std::string& got, json& pauses) {
    Rig b; b.opt.allowClaude = true; b.files.files = a.files.files; b.clock.t = a.clock.t + 60000;
    for (auto it = b.files.files.begin(); it != b.files.files.end();) { if (it->first.find(P("\\sessions\\")) != std::string::npos) it = b.files.files.erase(it); else ++it; }
    json st = setOf(b); if (setting.is_null()) st.erase("resumeAfterRestart"); else st["resumeAfterRestart"] = setting; b.files.put(sf, st.dump());
    b.make();
    const ClientId wb = b.deck->addWindow();
    b.deck->start(); b.call(wb, "deskInit");
    std::set<std::string> resumed;
    for (int i = 0; i < 10; i++) {
      for (const std::string& s : { s1, s2 }) if (fake::Pty* q = ptyOf(b, s)) b.record(T_DIR, q->pid(), s, "idle");
      b.advance(5000);
      for (const std::string& s : { s1, s2 }) if (fake::Pty* q = ptyOf(b, s)) if (q->typed().find("RESUME:") != std::string::npos) resumed.insert(s == s1 ? "deck" : "pasted");
    }
    got.clear(); for (const std::string& x : resumed) got += (got.empty() ? "" : ",") + x;
    pauses = json::array({ b.tile(i1)["pause"], b.tile(i2)["pause"] });
    b.deck->shutdown();
  };
  std::string got; json ps;
  restart(nullptr, got, ps);
  check("resume setting ON (the default): the deck's Pause and the pasted PAUSE both get RESUME after the restart", got == "deck,pasted", got + " " + ps.dump());
  restart(true, got, ps);
  check("resume setting ON (saved true): the same", got == "deck,pasted", got + " " + ps.dump());
  restart(false, got, ps);
  check("resume setting OFF: no RESUME to either; both keep their mark, nothing queued", got.empty() && ps[0]["state"] == "paused" && ps[1]["state"] == "paused" && ps[0]["want"] == false && ps[1]["want"] == false, got + " " + ps.dump());
}

// ---- a tile follows its session (lib/tilefollow.js): /clear or /resume inside it moves the same process to a new id
static void testFollow() {
  Rig r; r.opt.allowClaude = true;
  const ClientId w = r.boot();
  const std::string id = r.claude(w); fake::Pty* p = r.ptys.last(); const std::string sid = str(r.tile(id)["sessionId"]);
  r.transcript(T_DIR, sid, "Numbered options test", "x"); r.record(T_DIR, p->pid(), sid, "idle"); r.advance(2000);
  check("follow: before /clear the tile reads the session it was spawned with", r.tile(id)["title"] == "Numbered options test" && r.tile(id)["status"] == "idle", r.tile(id).dump());
  // /clear: the same pid's record names a new id, which has no transcript yet
  const std::string sid2 = "5e55a1d2-1111-4222-8333-0000000000a2";
  r.record(T_DIR, p->pid(), sid2, "busy"); r.advance(2000);
  json t = r.tile(id);
  check("follow: /clear: the tile takes the new id (no transcript yet), the old title is gone, tiles.json has it",
    t["sessionId"] == sid2 && t["title"] != "Numbered options test" && t["status"] == "busy" && r.tilesFile()["tiles"][0]["sessionId"] == sid2, t.dump());
  r.transcript(T_DIR, sid2, "Reporting page", "y"); r.record(T_DIR, p->pid(), sid2, "idle"); r.advance(2000);
  check("follow: the new session's transcript gives the title", r.tile(id)["title"] == "Reporting page" && r.tile(id)["status"] == "idle", r.tile(id).dump());
  // a reused pid: an old file under the pid of a second tile, written before that tile's process started
  r.advance(10000);
  const std::string id2 = r.claude(w); fake::Pty* q = r.ptys.last(); const std::string sid3 = str(r.tile(id2)["sessionId"]);
  r.record(T_DIR, q->pid(), "eeeeeeee-0000-4000-8000-00000000000e", "idle", { { "startedAt", r.clock.t - 60000 } }); r.advance(2000);
  check("follow: a stale record of a reused pid (started before the tile's process) is not a move", r.tile(id2)["sessionId"] == sid3, r.tile(id2).dump());
  r.record(T_DIR, q->pid(), sid3, "idle"); r.advance(2000);
  // /resume of the first tile's id inside the second: refused, both keep their ids
  r.record(T_DIR, q->pid(), sid2, "idle"); r.advance(2000);
  check("follow: /resume of an id another tile holds: refused, the old id kept", r.tile(id2)["sessionId"] == sid3 && r.tile(id)["sessionId"] == sid2);
  r.advance(4000);
  check("log: the refusal is one line, however many looks see it", r.log.count("[follow] tile " + id2.substr(0, 8) + ": its process moved to session " + sid2 + ", which tile " + id.substr(0, 8) + " holds: kept " + sid3) == 1, r.log.all());
  // /resume of a free id: followed
  const std::string sid4 = "44444444-0000-4000-8000-000000000004";
  r.record(T_DIR, q->pid(), sid4, "idle"); r.advance(2000);
  check("follow: /resume of a free id: followed", r.tile(id2)["sessionId"] == sid4 && r.tile(id)["sessionId"] == sid2);
  r.deck->shutdown();
}

// ---------------------------------------------------------------- auditors and their children (auditors.hpp; test/auditors-unit.js)
static void testAuditorsPure() {
  check("auditors: a child's name, a dot after a digit", auditorChildName("Build Bot MVP S", 2) == "Build Bot MVP S2" && auditorChildName("auditor 1", 3) == "auditor 1.3");
  check("auditors: the next free string", auditorNextString({}) == "auditor 1" && auditorNextString({ "auditor 1", "Auditor 3" }) == "auditor 2");
  const std::string p = auditorPrompt("", "auditor 1", "C:\\state\\auditors\\t1", "\\");
  // the prompt's first words by edition (auditors.hpp): the private edition's are the JS file's text
  const std::string head = deck::edition::kPublic ? "AUDITOR: the user made this session an auditor in scroDeck, named \"auditor 1\". They will"
                                                  : "AUDITOR: the user made this session an auditor in the Claude Deck, named \"auditor 1\". They will";
  check("auditors: the AUDITOR prompt (the JS file's text; the deck's name by edition)", p.rfind(head, 0) == 0
    && p.find("write the whole prompt to C:\\state\\auditors\\t1\\to-<n>.md:") != std::string::npos && p.find("moves the file to C:\\state\\auditors\\t1\\sent\\.") != std::string::npos
    && p.find("auditor 1.1, auditor 1.2, ...") != std::string::npos && p.find('{') == std::string::npos, p.substr(0, 200));
  AuditorChildFacts f; f.sessionId = "abc-1"; f.title = "Receiving \"page\""; f.account = "demo"; f.cwd = "C:\\Users\\demo"; f.browser = 1;
  check("auditors: the CHILD line", auditorChildLine(2, f) == "CHILD 2: session abc-1, tile \"Receiving 'page'\", account demo, folder C:\\Users\\demo, browser on", auditorChildLine(2, f));
  check("auditors: the AUDITOR prompt - the .now.md file, only for a change of course; the log file", p.find("Name the file C:\\state\\auditors\\t1\\to-<n>.now.md instead and the deck interrupts child n at once (Esc, mid-turn)") != std::string::npos
    && p.find("Use that only when the child must change course before it does more work on something wrong; everything else waits for its turn as C:\\state\\auditors\\t1\\to-<n>.md.") != std::string::npos
    && p.find("each delivery and refusal is one line in C:\\state\\auditors\\t1\\log.md.") != std::string::npos, p);
  check("auditors: a .now.md file - its n, urgent; nothing else is", auditorToIndex("to-2.now.md") == 2 && auditorIsNow("to-2.now.md") && auditorIsNow("TO-2.NOW.MD") && !auditorIsNow("to-2.md") && auditorToIndex("to-2.later.md") == 0
    && auditorToIndex("to-2.now.txt") == 0 && auditorToIndex("to-.now.md") == 0 && auditorToIndex("to-12345.now.md") == 0);
  check("auditors: the log line", auditorLogLine("2026-11-23 15:02:10 -05:00", 2, "to-2.now.md", "delivered mid-turn (Esc)") == "2026-11-23 15:02:10 -05:00 | child 2 | to-2.now.md | delivered mid-turn (Esc)");
  check("auditors: the left line (lib/auditors.js leftLine)", auditorLeftLine(2, "Receiving \"page\"", "closed") == "CHILD 2 left: tile \"Receiving 'page'\" (closed)" && auditorLeftLine(3, "x", "") == "CHILD 3 left: tile \"x\" (un-childed)", auditorLeftLine(2, "Receiving \"page\"", "closed"));
  check("auditors: browser off / default", auditorChildLine(1, AuditorChildFacts{ "", "", "", "", 0 }).find("browser off") != std::string::npos && auditorChildLine(1, AuditorChildFacts{}).find("session -, tile \"\", account -, folder -, browser default") != std::string::npos);
  Auditors s;
  check("auditors: make with the default string; the next free; a taken one refused", s.make("a1", "", true, "Long job").string == "auditor 1" && s.make("a2", "", false, "").string == "auditor 2" && !s.make("a3", "AUDITOR 1", false, "").ok && !s.make("a3", std::string(41, 'x'), false, "").ok);
  check("auditors: children numbered per auditor", s.setChild("c1", "a1").n == 1 && s.setChild("c2", "a1").n == 2 && s.setChild("c3", "a2").n == 1 && s.nameOf("c2") == "auditor 1.2" && s.setChild("c1", "a1").again);
  check("auditors: refused - an auditor as a child, a child as an auditor, its own child", !s.setChild("a2", "a1").ok && !s.make("c1", "x", false, "").ok && !s.setChild("a1", "a1").ok && !s.setChild("c9", "zz").ok);
  s.setPrevTitle("c1", true, "Reporting page");
  Auditors::Ended e;
  check("auditors: un-child gives its name back; the number is not given again", s.unchild("c1", &e) && e.prevTitle == "Reporting page" && s.setChild("c4", "a1").n == 3);
  check("auditors: saved", s.saved("a1").dump() == R"({"auditor":{"last":3,"prevTitle":"Long job","string":"auditor 1"}})" && s.saved("c3").dump() == R"({"child":{"n":1,"parent":"a2"}})", s.saved("a1").dump());
  check("auditors: the meta", s.view("a1").dump() == R"({"auditor":{"children":[{"id":"c2","n":2},{"id":"c4","n":3}],"string":"auditor 1"}})" && s.view("c4").dump() == R"({"child":{"auditorName":"auditor 1","n":3,"parent":"a1"}})" && s.view("zz").is_null(), s.view("a1").dump());
  Auditors t;
  for (const char* id : { "a1", "a2", "c2", "c3", "c4" }) t.load(id, s.saved(id));
  t.load("c9", json::parse(R"({"child":{"parent":"gone","n":1}})")); t.load("c8", json::parse(R"({"child":{"parent":"a2","n":1}})"));
  t.settle();
  check("auditors: loaded; an orphan ends; no two share an n", t.nameOf("c4") == "auditor 1.3" && !t.child("c9") && !t.child("c8") && t.child("c3") && t.setChild("c5", "a1").n == 4);
  Auditors::Ended prev; std::vector<Auditors::Ended> kids;
  check("auditors: un-make ends every child", s.unmake("a1", prev, kids) && prev.prevTitle == "Long job" && kids.size() == 2 && kids[0].id == "c2" && !s.child("c4") && !s.isAuditor("a1"));
  const AuditorInboxPlan ip = auditorInboxPlan({ "to-2.md", "notes.txt", "sent", "to-1.md", "TO-7.md", "to-1.md.tmp", "to-x.md", "to-3.md" }, { 1, 2, 3 }, { "to-3.md" });
  check("auditors: the inbox plan", ip.take.size() == 2 && ip.take[0].first == "to-1.md" && ip.take[1].second == 2 && ip.refuse.size() == 1 && ip.refuse[0].first == "TO-7.md" && ip.refuse[0].second == 7);
  check("auditors: the text, the note, the stamp", auditorPromptText("\xEF\xBB\xBFline 1\r\nline 2\r\n\r\n  ") == "line 1\nline 2" && auditorRefusedNote("x", "auditor 1", 7) == "REFUSED x by the Claude Deck: auditor 1 has no child 7."
    && auditorFileStamp(1795464130000.0, -5 * 3600000.0) == "2026-11-23 15-02-10", auditorFileStamp(1795464130000.0, -5 * 3600000.0));
  // the deliveries
  Deliveries d; const double T0 = 1790690000000.0, S = 1000;
  auto v = [](const std::string& id, bool ready, double statusAt) { Deliveries::View x; x.id = id; x.running = true; x.ready = ready; x.statusAt = statusAt; return x; };
  Deliveries::Item i1; i1.kind = "auditor"; i1.target = "a1"; i1.text = "AUDITOR"; d.add(i1, false, T0);
  Deliveries::Item i2; i2.kind = "line"; i2.target = "a1"; i2.text = "CHILD 1"; d.add(i2, false, T0);
  std::vector<std::string> wake;
  check("deliveries: nothing into a tile not idle", d.step({ v("a1", false, T0) }, T0 + S, wake).empty());
  std::vector<Deliveries::Item> ty = d.step({ v("a1", true, T0 - S) }, T0 + 2 * S, wake);
  check("deliveries: idle: the first only", ty.size() == 1 && ty[0].text == "AUDITOR" && d.items.size() == 1);
  check("deliveries: the next waits for the turn to end", d.step({ v("a1", true, T0 - S) }, T0 + 4 * S, wake).empty() && d.step({ v("a1", true, T0 + 9 * S) }, T0 + 10 * S, wake).size() == 1);
  Deliveries::Item f1; f1.kind = "file"; f1.target = "c1"; f1.from = "a1"; f1.fromName = "auditor 1"; f1.n = 1; f1.name = "to-1.md";
  const std::string fid = d.add(f1, true, T0).id;
  check("deliveries: held: never typed; its name on its way", d.step({ v("c1", true, 0) }, T0 + S, wake).empty() && d.pendingFiles("a1").size() == 1 && d.forTile("c1")[0]["state"] == "hold");
  Deliveries::View sl = v("c1", true, 0); sl.asleep = true;
  check("deliveries: Send; asleep: woken", d.send(fid) && !d.send(fid) && d.step({ sl }, T0 + 2 * S, wake).empty() && wake.size() == 1 && wake[0] == "c1");
  check("deliveries: typed and listed as sent", d.step({ v("c1", true, 0) }, T0 + 3 * S, wake).size() == 1 && d.forTile("c1")[0]["state"] == "sent" && d.forTile("c1")[0]["from"] == "auditor 1" && d.pendingFiles("a1").empty());
  Deliveries::Item f2 = f1; f2.target = "c2"; d.add(f2, false, T0);
  check("deliveries: un-child drops what waits for it", d.dropFor("c2") == 1 && d.dropFrom("zz") == 0);
  // URGENT: first in the queue, into a running turn with Esc; never while paused or before the session is up
  Deliveries dq; Deliveries::Item n1 = f1; n1.target = "c5"; const std::string nid = dq.add(n1, false, T0).id;
  Deliveries::Item u1 = f1; u1.target = "c5"; u1.name = "to-1.now.md"; u1.urgent = true; dq.add(u1, false, T0 + 1);
  auto busy = [&](bool live, bool paused) { Deliveries::View x = v("c5", false, T0 - S); x.live = live; x.paused = paused; return x; };
  check("deliveries: urgent - not before the session is up, not while paused", dq.step({ busy(false, false) }, T0 + S, wake).empty() && dq.step({ busy(true, true) }, T0 + S, wake).empty());
  std::vector<Deliveries::Item> tu = dq.step({ busy(true, false) }, T0 + 2 * S, wake);
  check("deliveries: urgent - into the running turn, before the file that waits, with Esc", tu.size() == 1 && tu[0].name == "to-1.now.md" && tu[0].esc && dq.get(nid)->state == "queued"
    && dq.forTile("c5")[0]["urgent"] == false && dq.forTile("c5")[1]["urgent"] == true, dq.forTile("c5").dump());
  check("deliveries: urgent - the file that waits still waits for the idle turn", dq.step({ busy(true, false) }, T0 + 4 * S, wake).empty() && (tu = dq.step({ v("c5", true, T0 + 5 * S) }, T0 + 6 * S, wake), tu.size() == 1 && tu[0].name == "to-1.md" && !tu[0].esc));
  Deliveries::Item u2 = u1; dq.add(u2, false, T0 + 7 * S);
  check("deliveries: urgent into an idle child - no Esc", (tu = dq.step({ v("c5", true, T0 + 7 * S) }, T0 + 8 * S, wake), tu.size() == 1 && tu[0].urgent && !tu[0].esc));
}

// ---- the deck: make an auditor, set children, the inbox typed into the child as the user's own turn (main.js; auditors.hpp)
static size_t countIn(fake::MemFiles& files, const std::string& dir) { size_t n = 0; for (const std::string& f : files.list(dir)) if (files.stat(dir + P("\\") + f).exists && !files.stat(dir + P("\\") + f).isDir) n++; return n; }
static void testAuditors() {
  Rig r; r.opt.allowClaude = true; r.clock.offset = -5 * 3600000.0;
  // auditorAutoSend unset: sent without asking in the private edition, held for Send in the public build (edition.hpp)
  check("auditors: auditorAutoSend's default is the edition's", auditorAutoSendOn(json::object()) == !deck::edition::kPublic && auditorAutoSendOn({ { "auditorAutoSend", false } }) == false && auditorAutoSendOn({ { "auditorAutoSend", true } }) == true);
  if (deck::edition::kPublic) r.files.put(P("C:\\state\\settings.json"), "{\"auditorAutoSend\":true}");   // the scenario below is written for auto send on
  const ClientId w = r.boot();
  const std::string a = r.claude(w); fake::Pty* pa = r.ptys.last(); const std::string sa = str(r.tile(a)["sessionId"]);
  const std::string c1 = r.claude(w); fake::Pty* p1 = r.ptys.last(); const std::string s1 = str(r.tile(c1)["sessionId"]);
  const std::string c2 = r.claude(w); fake::Pty* p2 = r.ptys.last(); const std::string s2 = str(r.tile(c2)["sessionId"]);
  r.transcript(T_DIR, sa, "Overnight plan", "x"); r.transcript(T_DIR, s1, "Reporting page", "y"); r.transcript(T_DIR, s2, "Worklist", "z");
  r.res(w, "renameTile", { c2, "my worklist" }); r.advance(10);
  r.record(T_DIR, pa->pid(), sa, "busy"); r.record(T_DIR, p1->pid(), s1, "idle"); r.record(T_DIR, p2->pid(), s2, "idle"); r.advance(2000);
  const std::string inbox = P("C:\\state\\auditors\\") + a;
  check("auditors: refused - a child of a tile that is no auditor", r.call(w, "setAuditorChild", { c1, a })["error"] == "not an auditor");
  const json m = r.res(w, "makeAuditor", { a, "" });
  check("auditors: made, with the default string and its inbox", m["string"] == "auditor 1" && m["inbox"] == inbox && r.tile(a)["auditor"]["string"] == "auditor 1" && r.tile(a)["title"] == "auditor 1"
    && r.files.stat(inbox + P("\\sent")).isDir && r.files.stat(inbox + P("\\refused")).isDir, r.tile(a).dump().substr(0, 400));
  check("auditors: saved with the tile", r.tilesFile()["tiles"][0]["auditor"]["string"] == "auditor 1");
  check("auditors: a second auditor's default is the next number", r.call(w, "makeAuditor", { c1, "auditor 1" })["error"] == "\"auditor 1\" is taken by another auditor");
  const json k1 = r.res(w, "setAuditorChild", { c1, a });
  check("auditors: child 1, renamed", k1["n"] == 1 && k1["name"] == "auditor 1.1" && r.tile(c1)["title"] == "auditor 1.1" && r.tile(c1)["child"]["auditorName"] == "auditor 1" && r.tile(a)["auditor"]["children"].size() == 1);
  r.advance(4000);
  check("auditors: nothing typed into the auditor while its turn runs", pa->typed().find("AUDITOR:") == std::string::npos);
  r.record(T_DIR, pa->pid(), sa, "idle"); r.advance(4000);
  const std::string ta = pa->typed();
  check("auditors: idle: the AUDITOR prompt, as a paste, no Esc; not yet the CHILD line", ta.find(std::string("\x1b[200~AUDITOR: ") + (deck::edition::kPublic ? "the user made this session an auditor in scroDeck" : "the user made this session an auditor in the Claude Deck") + ", named \"auditor 1\".") != std::string::npos
    && ta.find("write the whole prompt to " + inbox + P("\\to-<n>.md:")) != std::string::npos && ta.find("\x1b\x1b[200~AUDITOR") == std::string::npos && ta.find("CHILD 1:") == std::string::npos, ta.substr(0, 200));
  r.record(T_DIR, pa->pid(), sa, "busy"); r.advance(2000); r.record(T_DIR, pa->pid(), sa, "idle"); r.advance(4000);
  const std::string line = "CHILD 1: session " + s1 + ", tile \"Reporting page\", account claudet, folder " + P("C:\\work") + ", browser default";
  check("auditors: its turn ended: the CHILD line, once", pa->typed().find("\x1b[200~" + line + "\x1b[201~") != std::string::npos && pa->typed().find("CHILD 1:") == pa->typed().rfind("CHILD 1:"), pa->typed().substr(ta.size(), 200));
  check("auditors: nothing typed into the child", p1->typed().find("\x1b[200~") == std::string::npos);
  // the inbox: to-1.md -> child 1 (idle), to-5.md -> refused
  r.files.put(inbox + P("\\to-1.md"), "\xEF\xBB\xBF" "Do the overnight run.\r\nAll of it.\r\n");
  r.files.put(inbox + P("\\to-5.md"), "who?");
  r.files.put(inbox + P("\\notes.txt"), "mine");
  r.advance(1000);
  check("auditors: a file still being written is left (2 s)", r.files.has(inbox + P("\\to-1.md")) && p1->typed().find("overnight") == std::string::npos);
  r.advance(4000);
  const std::string t1 = p1->typed();
  check("auditors: to-1.md typed into child 1 as the user's own turn: the paste, then Enter", t1.find("\x1b[200~Do the overnight run.\nAll of it.\x1b[201~") != std::string::npos && t1.find("\x1b[201~\r") != std::string::npos, t1.substr(0, 120));
  check("auditors: moved to sent/, the inbox's other files left", !r.files.has(inbox + P("\\to-1.md")) && countIn(r.files, inbox + P("\\sent")) == 1 && r.files.has(inbox + P("\\notes.txt")));
  const std::vector<std::string> refused = r.files.list(inbox + P("\\refused"));
  check("auditors: to-5.md (no child 5) moved to refused/ with its note line on top", refused.size() == 1 && refused[0].find(" to-5.md") != std::string::npos
    && r.files.get(inbox + P("\\refused\\") + refused[0]).find("REFUSED 2027-01-15 ") == 0 && r.files.get(inbox + P("\\refused\\") + refused[0]).find(" -05:00 by the Claude Deck: auditor 1 has no child 5.\n\nwho?") != std::string::npos,
    refused.empty() ? std::string("none") : r.files.get(inbox + P("\\refused\\") + refused[0]));
  check("auditors: logged on the child tile: from auditor 1, sent", r.tile(c1)["auditorMail"].size() == 1 && r.tile(c1)["auditorMail"][0]["state"] == "sent" && r.tile(c1)["auditorMail"][0]["from"] == "auditor 1"
    && r.tile(c1)["auditorMail"][0]["firstLine"] == "Do the overnight run." && r.tile(c1)["auditorMail"][0]["sentAt"].is_number(), r.tile(c1)["auditorMail"].dump());
  // a second file while the child works: waits for the turn to end
  r.record(T_DIR, p1->pid(), s1, "busy"); r.advance(1000);
  r.files.put(inbox + P("\\to-1.md"), "Second."); r.advance(6000);
  check("auditors: never into a running turn", p1->typed().find("Second.") == std::string::npos && r.files.has(inbox + P("\\to-1.md")));
  r.record(T_DIR, p1->pid(), s1, "idle"); r.advance(4000);
  check("auditors: the turn ended: typed", p1->typed().find("\x1b[200~Second.\x1b[201~") != std::string::npos && !r.files.has(inbox + P("\\to-1.md")));
  // auditorAutoSend off: the file waits on the child tile for Send
  r.res(w, "saveSettings", { { { "auditorAutoSend", false } } });
  r.record(T_DIR, p1->pid(), s1, "busy"); r.advance(1000); r.record(T_DIR, p1->pid(), s1, "idle");
  r.files.put(inbox + P("\\to-1.md"), "Third."); r.advance(6000);
  const json mail = r.tile(c1)["auditorMail"];
  check("auditors: auto send off: held on the child tile, the file left in the inbox", mail.size() == 3 && mail[0]["state"] == "hold" && p1->typed().find("Third.") == std::string::npos && r.files.has(inbox + P("\\to-1.md")), mail.dump());
  check("auditors: Send", r.res(w, "auditorSend", { mail[0]["id"] }) == true && (r.advance(4000), p1->typed().find("\x1b[200~Third.\x1b[201~") != std::string::npos));
  check("auditors: Send of nothing waiting refused", r.call(w, "auditorSend", { mail[0]["id"] })["error"] == "nothing waiting to send");
  r.res(w, "saveSettings", { { { "auditorAutoSend", true } } });
  // a second child, un-childed: its name given back; set again: a new number
  check("auditors: child 2 keeps its own name to give back", r.res(w, "setAuditorChild", { c2, a })["n"] == 2 && r.tile(c2)["title"] == "auditor 1.2");
  check("auditors: un-child: the name given back", r.res(w, "unchildTile", { c2 }) == true && r.tile(c2)["title"] == "my worklist" && r.tile(c2)["child"].is_null());
  r.advance(8000);
  check("auditors: set again: child 3; the CHILD line names the tile's own name", r.res(w, "setAuditorChild", { c2, a })["n"] == 3 && (r.record(T_DIR, pa->pid(), sa, "busy"), r.advance(2000), r.record(T_DIR, pa->pid(), sa, "idle"), r.advance(4000),
    pa->typed().find("CHILD 3: session " + s2 + ", tile \"my worklist\",") != std::string::npos && pa->typed().find("CHILD 2:") == std::string::npos), pa->typed().substr(pa->typed().size() > 300 ? pa->typed().size() - 300 : 0));
  r.files.put(inbox + P("\\to-2.md"), "for the old child 2"); r.advance(5000);
  check("auditors: a file for the un-childed number is refused", r.files.list(inbox + P("\\refused")).size() == 2 && p2->typed().find("old child 2") == std::string::npos);
  // restart: the auditor and its children come back
  const json saved = r.tilesFile();
  check("auditors: tiles.json keeps the auditor and each child", saved["tiles"][0]["auditor"]["last"] == 3 && saved["tiles"][1]["child"]["n"] == 1 && saved["tiles"][2]["child"]["n"] == 3 && saved["tiles"][2]["child"]["prevTitle"] == "my worklist", saved.dump().substr(0, 600));
  r.deck->shutdown();
  Rig b; b.opt.allowClaude = true; b.files.files = r.files.files; b.files.dirs = r.files.dirs; b.clock.t = r.clock.t + 60000;
  for (auto it = b.files.files.begin(); it != b.files.files.end();) { if (it->first.find(P("\\sessions\\")) != std::string::npos) it = b.files.files.erase(it); else ++it; }
  b.make();
  const ClientId wb = b.deck->addWindow();
  b.deck->start(); b.call(wb, "deskInit");
  b.advance(3000);
  check("auditors: restored - the auditor, its children, their names", b.tile(a)["auditor"]["string"] == "auditor 1" && b.tile(c1)["child"]["n"] == 1 && b.tile(c1)["title"] == "auditor 1.1" && b.tile(c2)["title"] == "auditor 1.3", b.tile(c2).dump().substr(0, 300));
  check("auditors: un-make: every child ended, the names given back", b.res(wb, "unmakeAuditor", { a }) == true && b.tile(a)["auditor"].is_null() && b.tile(c1)["child"].is_null() && b.tile(c2)["title"] == "my worklist" && b.tile(a)["title"] == "Overnight plan",
    b.tile(a)["title"].dump() + " " + b.tile(c2)["title"].dump());
  check("auditors: made again after un-make: a new string, numbering from 1", b.res(wb, "makeAuditor", { a, "Floor S" })["string"] == "Floor S" && b.res(wb, "setAuditorChild", { c1, a })["name"] == "Floor S1");
  b.call(wb, "closeTile", { a }, 2000);
  check("auditors: closing the auditor ends its children", b.tile(c1)["child"].is_null() && b.tile(c1)["title"] == "Reporting page", b.tile(c1)["title"].dump());
  const std::string a8 = a.substr(0, 8);
  check("log: made, the CHILD line, typed, refused", r.log.count("[auditor] " + a8 + " made \"auditor 1\" (" + inbox + ")") == 1 && r.log.count("[auditor] " + a8 + ": " + line) == 1
    && r.log.count("[auditor] the AUDITOR prompt typed into auditor 1") == 1 && r.log.count("[auditor] " + a8 + ": to-5.md refused (no child 5)") == 1, r.log.all().substr(0, 900));
  b.deck->shutdown();
}

// ---- the auditor is told when a child leaves it: un-childed, set as another auditor's child, or its tile closed (2026-11-23)
static void testAuditorLeaves() {
  Rig r; r.opt.allowClaude = true;
  const ClientId w = r.boot();
  const std::string a = r.claude(w); fake::Pty* pa = r.ptys.last(); const std::string sa = str(r.tile(a)["sessionId"]);
  const std::string b = r.claude(w); fake::Pty* pb = r.ptys.last(); const std::string sb = str(r.tile(b)["sessionId"]);
  const std::string c1 = r.claude(w); fake::Pty* p1 = r.ptys.last(); const std::string s1 = str(r.tile(c1)["sessionId"]);
  const std::string c2 = r.claude(w); fake::Pty* p2 = r.ptys.last(); const std::string s2 = str(r.tile(c2)["sessionId"]);
  const std::string c3 = r.claude(w); fake::Pty* p3 = r.ptys.last(); const std::string s3 = str(r.tile(c3)["sessionId"]);
  r.transcript(T_DIR, s1, "Reporting page", "y"); r.transcript(T_DIR, s2, "Worklist \"v2\"", "z"); r.transcript(T_DIR, s3, "Labels", "w");
  for (auto& x : std::vector<std::pair<fake::Pty*, std::string>>{ { pa, sa }, { pb, sb }, { p1, s1 }, { p2, s2 }, { p3, s3 } }) r.record(T_DIR, x.first->pid(), x.second, "idle");
  r.advance(2000);
  // one turn of the auditor's session (busy, then idle: the deck types the next line); until: turns until the text is typed
  auto turn = [&] { r.record(T_DIR, pa->pid(), sa, "busy"); r.advance(2000); r.record(T_DIR, pa->pid(), sa, "idle"); r.advance(4000); };
  auto until = [&](const std::string& text) { for (int i = 0; i < 6 && pa->typed().find(text) == std::string::npos; i++) turn(); };
  r.res(w, "makeAuditor", { a, "" }); r.res(w, "makeAuditor", { b, "" });
  until("AUDITOR: the user made");
  r.res(w, "setAuditorChild", { c1, a }); r.res(w, "setAuditorChild", { c2, a }); r.res(w, "setAuditorChild", { c3, a });
  until("CHILD 3:");
  check("auditor leaves: the three CHILD lines typed", pa->typed().find("CHILD 1:") != std::string::npos && pa->typed().find("CHILD 2:") != std::string::npos && pa->typed().find("CHILD 3:") != std::string::npos, pa->typed().substr(pa->typed().size() > 400 ? pa->typed().size() - 400 : 0));
  check("auditor leaves: un-child", r.res(w, "unchildTile", { c1 }) == true);
  until("CHILD 1 left:");
  check("auditor leaves: un-childed - the line typed into the auditor as a paste, with the tile's own name", pa->typed().find("\x1b[200~CHILD 1 left: tile \"Reporting page\" (un-childed)\x1b[201~") != std::string::npos, pa->typed().substr(pa->typed().size() > 200 ? pa->typed().size() - 200 : 0));
  r.call(w, "closeTile", { c2 }, 2000);
  until("CHILD 2 left:");
  check("auditor leaves: its tile closed - the line says closed (a quote in the name made a single one)", pa->typed().find("CHILD 2 left: tile \"Worklist 'v2'\" (closed)") != std::string::npos, pa->typed().substr(pa->typed().size() > 200 ? pa->typed().size() - 200 : 0));
  check("auditor leaves: set as another auditor's child - the old auditor told it left", r.res(w, "setAuditorChild", { c3, b })["name"] == "auditor 2.1");
  until("CHILD 3 left:"); turn();
  check("auditor leaves: ...as un-childed, once", pa->typed().find("CHILD 3 left: tile \"Labels\" (un-childed)") != std::string::npos && pa->typed().find("CHILD 3 left:") == pa->typed().rfind("CHILD 3 left:"));
  check("auditor leaves: nothing about it typed into the new auditor but its CHILD line", pb->typed().find("left:") == std::string::npos);
  // a child that leaves before the deck typed its CHILD line: the auditor never heard of it, so it is not told
  r.record(T_DIR, pa->pid(), sa, "busy"); r.advance(10);   // (an idle auditor would get the CHILD line at once)
  r.res(w, "setAuditorChild", { c1, a }); r.res(w, "unchildTile", { c1 });
  turn(); turn(); turn();
  check("auditor leaves: un-childed before its CHILD line was typed - neither line typed", pa->typed().find("CHILD 4") == std::string::npos, pa->typed().substr(pa->typed().size() > 200 ? pa->typed().size() - 200 : 0));
  check("log: the left lines", r.log.count("[auditor] " + a.substr(0, 8) + ": CHILD 1 left: tile \"Reporting page\" (un-childed)") == 1 && r.log.count("[auditor] " + a.substr(0, 8) + ": CHILD 2 left: tile \"Worklist 'v2'\" (closed)") == 1, r.log.all().substr(0, 600));
  r.deck->shutdown();
}

// ---- URGENT delivery: to-<n>.now.md is typed at once, Esc first into a running turn; to-<n>.md waits; log.md (2026-11-23)
static void testAuditorUrgent() {
  Rig r; r.opt.allowClaude = true; r.clock.offset = -5 * 3600000.0;
  if (deck::edition::kPublic) r.files.put(P("C:\\state\\settings.json"), "{\"auditorAutoSend\":true}");   // written for auto send on (edition.hpp)
  const ClientId w = r.boot();
  const std::string a = r.claude(w); fake::Pty* pa = r.ptys.last(); const std::string sa = str(r.tile(a)["sessionId"]);
  const std::string c1 = r.claude(w); fake::Pty* p1 = r.ptys.last(); const std::string s1 = str(r.tile(c1)["sessionId"]);
  r.record(T_DIR, pa->pid(), sa, "idle"); r.record(T_DIR, p1->pid(), s1, "idle"); r.advance(2000);
  auto turn = [&] { r.record(T_DIR, pa->pid(), sa, "busy"); r.advance(2000); r.record(T_DIR, pa->pid(), sa, "idle"); r.advance(4000); };
  auto until = [&](const std::string& text) { for (int i = 0; i < 6 && pa->typed().find(text) == std::string::npos; i++) turn(); };
  const std::string inbox = P("C:\\state\\auditors\\") + a, logFile = inbox + P("\\log.md");
  r.res(w, "makeAuditor", { a, "" }); until("AUDITOR: the user made");
  r.res(w, "setAuditorChild", { c1, a }); until("CHILD 1:");
  // the child works; a file for its next turn and an urgent one arrive
  r.record(T_DIR, p1->pid(), s1, "busy"); r.advance(1000);
  r.files.put(inbox + P("\\to-1.md"), "Later.");
  r.files.put(inbox + P("\\to-1.now.md"), "Stop: that is the wrong table.");
  r.advance(6000);
  const std::string t1 = p1->typed();
  check("urgent: typed into the running turn at once, Esc first (the Pause path)", t1.find("\x1b\x1b[200~Stop: that is the wrong table.\x1b[201~") != std::string::npos, t1.substr(0, 200));
  check("urgent: the file for its turn still waits (the child has not been idle since)", t1.find("Later.") == std::string::npos && r.files.has(inbox + P("\\to-1.md")) && !r.files.has(inbox + P("\\to-1.now.md")));
  check("urgent: moved to sent/; listed on the child as urgent", countIn(r.files, inbox + P("\\sent")) == 1 && r.tile(c1)["auditorMail"].size() == 2 && r.tile(c1)["auditorMail"][1]["urgent"] == true && r.tile(c1)["auditorMail"][0]["urgent"] == false, r.tile(c1)["auditorMail"].dump());
  { const std::string lg = r.files.get(logFile);
    check("log.md: one line - time, child, file, delivered mid-turn", lg.find(" -05:00 | child 1 | to-1.now.md | delivered mid-turn (Esc)\n") != std::string::npos && lg.rfind("2027-", 0) == 0 && std::count(lg.begin(), lg.end(), '\n') == 1, lg); }
  // the turn ends: the waiting file, no Esc
  r.record(T_DIR, p1->pid(), s1, "idle"); r.advance(4000);
  const std::string t2 = p1->typed();
  check("urgent: the other file at the idle turn, no Esc", t2.find("\x1b[200~Later.\x1b[201~") != std::string::npos && t2.find("\x1b\x1b[200~Later.") == std::string::npos, t2.substr(t1.size()));
  // urgent into an idle child: no Esc needed
  r.record(T_DIR, p1->pid(), s1, "busy"); r.advance(1000); r.record(T_DIR, p1->pid(), s1, "idle"); r.advance(1000);
  r.files.put(inbox + P("\\to-1.now.md"), "Also this."); r.advance(6000);
  check("urgent into an idle child: typed, no Esc", p1->typed().find("\x1b[200~Also this.\x1b[201~") != std::string::npos && p1->typed().find("\x1b\x1b[200~Also this.") == std::string::npos);
  // a paused child: an urgent file waits for Resume
  r.res(w, "pauseTile", { c1 }); r.advance(4000);
  r.files.put(inbox + P("\\to-1.now.md"), "While paused."); r.advance(6000);
  check("urgent: never into a paused child", p1->typed().find("While paused.") == std::string::npos && r.files.has(inbox + P("\\to-1.now.md")));
  // refused: no child 7
  r.files.put(inbox + P("\\to-7.now.md"), "who?"); r.advance(5000);
  const std::string lg = r.files.get(logFile);
  check("log.md: deliveries and refusals, one line each", lg.find(" | child 1 | to-1.md | delivered\n") != std::string::npos && lg.find(" | child 1 | to-1.now.md | delivered\n") != std::string::npos
    && lg.find(" | child 7 | to-7.now.md | refused: auditor 1 has no child 7\n") != std::string::npos && std::count(lg.begin(), lg.end(), '\n') == 4, lg);
  check("log: the urgent file seen and typed mid-turn", r.log.count("[auditor] " + a.substr(0, 8) + ": to-1.now.md for auditor 1.1 (URGENT: mid-turn)") >= 1 && r.log.all().find("to-1.now.md from auditor 1 typed into auditor 1.1 mid-turn (Esc)") != std::string::npos, r.log.all().substr(0, 600));
  r.deck->shutdown();
}

// a scenario that throws (a JSON shape it did not expect) is one failure, not the end of the run
template <class F> void scenario(const char* name, F f) {
  try { f(); } catch (const std::exception& e) { const std::string after = lastCheck; check(std::string(name) + ": ran to its end", false, std::string(e.what()) + " — after: " + after); }
}
// 14b (2026-11-19, built 2026-11-23): exactly one screen answers a session's terminal queries, named per tile in
// meta `answerer` - the deck's window that shows it, else the remote desk that shows it, else the phone attached to it,
// else the office (remoterule::answerer). The remotes (the exe, the browser desk, the phone) answer only when named.
static void testAnswerer() {
  check("answerer (pure): office first, then the remote desk, then the phone, else the office",
    remoterule::answerer(true, true, true) == "office" && remoterule::answerer(false, true, true) == "remote" && remoterule::answerer(false, false, true) == "phone"
    && remoterule::answerer(false, false, false) == "office" && remoterule::answerer(true, false, false) == "office");
  Rig r; r.opt.persistShells = true;
  const ClientId w = r.boot();
  const std::string a = r.shell(w), b = r.shell(w);
  auto ans = [&](const std::string& id) { return str(r.tile(id)["answerer"]); };
  check("answerer: nothing shows a session: the office (a terminal it may still have)", ans(a) == "office" && ans(b) == "office", ans(a) + " " + ans(b));
  r.send(w, { { "t", "shown" }, { "ids", { a } }, { "hidden", false } });
  const ClientId dk = r.deck->addRemote(true);
  r.call(dk, "deskInit", json::array({ { { "kind", "native" } } }));
  r.send(dk, { { "t", "shown" }, { "ids", { a, b } }, { "hidden", false } });
  check("answerer: the office window shows it: the office, even with a remote desk showing it too", ans(a) == "office", ans(a));
  check("answerer: no office window shows it, a remote desk does: the remote", ans(b) == "remote", ans(b));
  const ClientId ph = r.deck->addRemote();
  r.send(ph, { { "t", "attach" }, { "id", b } });
  check("answerer: a phone attached as well: still the remote desk (it shows it)", ans(b) == "remote", ans(b));
  r.send(dk, { { "t", "shown" }, { "ids", { a, b } }, { "hidden", true } });
  check("answerer: the desk's tab hidden: the phone (it shows nothing then)", ans(b) == "phone" && ans(a) == "office", ans(a) + " " + ans(b));
  r.send(ph, { { "t", "detach" } });
  check("answerer: the phone gone too: back to the office", ans(b) == "office", ans(b));
  r.send(dk, { { "t", "shown" }, { "ids", { a, b } }, { "hidden", false } });
  r.send(w, { { "t", "shown" }, { "ids", { a } }, { "hidden", true } });
  check("answerer: the office window hidden (minimised): the remote desk that shows it", ans(a) == "remote" && ans(b) == "remote", ans(a) + " " + ans(b));
  r.advance(2100);
  { const json m = r.lastOf(dk, "meta"); std::string got;
    for (const json& t : m["meta"]["tiles"]) if (t["id"] == b) got = str(t["answerer"]);
    check("answerer: the desk is sent it in its meta", got == "remote", got); }
  r.deck->shutdown();
}

void testScenarios(const std::string& fx, const std::string&) {
  if (!want("deck")) return;
  scenario("one window", testOneWindow); scenario("restart", testRestart); scenario("claude", [&] { testClaude(fx); });
  scenario("quiet and sleep", testQuietAndSleep); scenario("clients", testClients); scenario("sessions", testSessions); scenario("presence", testPresence); scenario("labels", testLabelFallback);
  scenario("real runs", [&] { testRealRuns(fx); }); scenario("restore refresh", testRestoreRefresh); scenario("pause", testPause); scenario("pause + clear", testPauseClear); scenario("pause mark", testPauseMark); scenario("pause resume setting", testPauseResumeSetting); scenario("follow", testFollow);
  scenario("auditors (pure)", testAuditorsPure); scenario("auditors", testAuditors); scenario("auditor leaves", testAuditorLeaves); scenario("auditor urgent", testAuditorUrgent);
  scenario("answerer", testAnswerer);
}
