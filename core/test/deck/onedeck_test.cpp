// core/test/deck/onedeck_test.cpp — one deck at a time + the shared layout (core/deck/onedeck.hpp), section `onedeck`:
// the marker the native deck writes, on the fakes (both path modes).
#include "rig.hpp"
#include "../../deck/onedeck.hpp"

namespace {

#define SHARED_DIR (fake::P("C:\\shared"))
#define NATIVE_EXE (fake::P("C:\\bin\\ClaudeDeckNative.exe"))

json oneDeckOpt() { return { { "deck", "native" }, { "sharedDir", SHARED_DIR }, { "exe", NATIVE_EXE }, { "args", json::array({ "--local" }) }, { "defaultDeck", true } }; }
json markerIn(Rig& r) { return r.files.getJson(std::string(SHARED_DIR) + P("\\active.json")); }

void testMarker() {
  {
    Rig r; r.boot();
    check("marker: a deck with oneDeck off writes none", !r.files.has(std::string(SHARED_DIR) + P("\\active.json")));
  }
  {
    Rig r; r.opt.oneDeck = oneDeckOpt(); r.opt.allowClaude = true; r.env.pidV = 4242; r.files.makeDirs(P("C:\\work"));
    const ClientId w = r.boot();
    json m = markerIn(r);
    check("marker: written at start, v1, the native deck, running", m.is_object() && m["v"] == 1 && m["deck"] == "native" && m["running"] == true, m.dump());
    check("marker: its pid, exe, args, state folder", m["pid"] == 4242 && m["exe"] == NATIVE_EXE && m["args"] == json::array({ "--local" }) && m["stateDir"] == P("C:\\state"), m.dump());
    check("marker: startedAt and heartbeatAt are ISO times; no session yet", str(m["startedAt"]).size() == 24 && str(m["heartbeatAt"]).size() == 24 && m["liveSessions"] == 0, m.dump());
    check("marker: exactly the fields active-deck.ps1 reads", m.size() == 10, m.dump());
    const std::string started = str(m["startedAt"]), beat = str(m["heartbeatAt"]);
    const std::string id = r.claude(w);
    r.advance(31000);
    m = markerIn(r);
    check("marker: the heartbeat rewrites heartbeatAt every 30 s, startedAt kept", str(m["heartbeatAt"]) != beat && str(m["startedAt"]) == started, m.dump());
    check("marker: a running Claude tile is a live session", m["liveSessions"] == 1, m.dump());
    r.call(w, "sleepTile", json::array({ id }), 8000); r.advance(31000);
    m = markerIn(r);
    check("marker: an asleep tile still counts (the deck will run it)", m["liveSessions"] == 1, m.dump());
    r.deck->shutdown();
    m = markerIn(r);
    check("marker: a clean quit leaves it, running:false, no live session", m.is_object() && m["running"] == false && m["liveSessions"] == 0 && m["pid"] == 4242, m.dump());
  }
  {
    // another deck that runs holds the marker (it started after this one, which had no sessions): left alone
    Rig r; r.opt.oneDeck = oneDeckOpt(); r.env.pidV = 4242;
    r.boot();
    const json other = { { "v", 1 }, { "deck", "electron" }, { "running", true }, { "pid", 777 }, { "exe", P("C:\\e\\electron.exe") }, { "args", json::array() }, { "stateDir", P("C:\\e-state") },
      { "startedAt", isoTime(r.clock.t) }, { "heartbeatAt", isoTime(r.clock.t) }, { "liveSessions", 3 } };
    r.files.put(std::string(SHARED_DIR) + P("\\active.json"), other.dump());
    r.procs.alivePids.insert(777);
    r.advance(31000);
    check("marker: the other live deck's marker is not overwritten by the heartbeat", markerIn(r)["deck"] == "electron");
    r.procs.alivePids.erase(777);
    r.advance(31000);
    check("marker: once that deck is gone, this one writes it again", markerIn(r)["deck"] == "native" && markerIn(r)["running"] == true);
  }
  {
    // at start the deck has passed the guard: it takes the marker even from a live deck with no sessions
    Rig r; r.opt.oneDeck = oneDeckOpt(); r.env.pidV = 4242;
    r.files.put(std::string(SHARED_DIR) + P("\\active.json"), json{ { "v", 1 }, { "deck", "electron" }, { "running", true }, { "pid", 777 }, { "exe", P("C:\\e\\electron.exe") }, { "args", json::array() },
      { "stateDir", P("C:\\e-state") }, { "startedAt", isoTime(r.clock.t) }, { "heartbeatAt", isoTime(r.clock.t) }, { "liveSessions", 0 } }.dump());
    r.procs.alivePids.insert(777);
    r.boot();
    check("marker: a deck that passed the guard is the active one, beside an empty live deck", markerIn(r)["deck"] == "native" && markerIn(r)["pid"] == 4242);
    r.deck->shutdown();
  }
}

// THE GUARD AT START (onedeck::guardAtStart): what a shell asks before it starts its deck
void testGuard() {
  using namespace deck::core::onedeck;
  const double now = 1795000000000;
  auto marker = [&](Rig& r, const std::string& deck, double pid, const std::string& exe, int live, double beatAgo = 0) {
    r.files.put(std::string(SHARED_DIR) + P("\\active.json"), json{ { "v", 1 }, { "deck", deck }, { "running", true }, { "pid", pid }, { "exe", exe }, { "args", json::array() },
      { "stateDir", P("C:\\e-state") }, { "startedAt", isoTime(now - 60000) }, { "heartbeatAt", isoTime(now - beatAgo) }, { "liveSessions", live } }.dump());
  };
  const std::string EEXE = P("C:\\gh\\claude-deck\\node_modules\\electron\\dist\\electron.exe");
  {
    Rig r; r.env.pidV = 4242;
    marker(r, "electron", 777, EEXE, 5);
    r.procs.snap = { { 777, 1, "electron.exe", EEXE }, { 778, 777, "electron.exe", EEXE }, { 4242, 1, "ClaudeDeckNative.exe", NATIVE_EXE } };
    Busy b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: the Electron deck runs with 5 sessions (its marker): busy", b.busy && b.n == 5 && b.source == "marker" && b.other == "electron", b.why);
    check("guard: the strip's words", blockedText(b.n) == "The other Claude Deck is running with 5 sessions - quit it first");
    r.procs.snap[0].path = P("C:\\x\\notepad.exe");
    b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: the marker's pid is another program now: the marker does not count", b.source != "marker", b.why);
  }
  {
    // the marker says nothing (stale), the Electron deck's main process runs: its tiles.json's live sessions count
    Rig r; r.env.pidV = 4242;
    marker(r, "electron", 777, EEXE, 5, 5 * 60000);
    r.procs.snap = { { 777, 1, "electron.exe", EEXE }, { 778, 777, "electron.exe", EEXE } };
    r.files.put(P("C:\\e-state\\tiles.json"), json{ { "tiles", json::array({ { { "id", "a" }, { "sessionId", "sa" } }, { { "id", "b" }, { "sessionId", "sb" } } }) } }.dump());
    r.record(T_DIR, 9001, "sa", "idle"); r.procs.alivePids.insert(9001);
    r.record(T_DIR, 9002, "sb", "idle");                       // its process is gone
    Busy b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: a stale heartbeat, its process runs, one of its tiles' sessions is live: busy (1)", b.busy && b.n == 1 && b.source == "process", b.why);
    r.files.put(P("C:\\e-state\\tiles.json"), "{\"tiles\":[");
    b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: ... and its tiles.json does not read: taken as busy", b.busy && !b.known, b.why);
    r.procs.snap = { { 778, 777, "electron.exe", P("C:\\Program Files\\Slack\\electron.exe") } };   // another Electron app: not the deck
    b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: an electron.exe outside a claude-deck checkout is not the deck", !b.busy && b.source == "none", b.why);
    r.procs.snap = { { 776, 1, "electron.exe", P("C:\\Program Files\\Slack\\electron.exe") }, { 778, 776, "electron.exe", EEXE } };
    b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: an electron.exe whose parent is an electron.exe is a child, not the deck's main", !b.busy && b.source == "none", b.why);
  }
  {
    Rig r; r.env.pidV = 4242;
    marker(r, "electron", 777, EEXE, 0);
    r.procs.snap = { { 777, 1, "electron.exe", EEXE } };
    Busy b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: the other deck runs with no sessions: not busy", !b.busy && b.source == "marker", b.why);
    json m = json::parse(r.files.get(std::string(SHARED_DIR) + P("\\active.json"))); m["running"] = false; r.files.put(std::string(SHARED_DIR) + P("\\active.json"), m.dump());
    r.procs.snap.clear();
    b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: a clean quit and no process: not busy", !b.busy && b.source == "none", b.why);
  }
  {
    // a scratch deck (defaultDeck false) never looks at the real decks' processes: only otherExe, a test's stand-in
    Rig r; r.env.pidV = 4242;
    json od = oneDeckOpt(); od["defaultDeck"] = false; od["otherState"] = P("C:\\e-state");
    r.procs.snap = { { 777, 1, "electron.exe", EEXE } };
    r.files.put(P("C:\\e-state\\tiles.json"), json{ { "tiles", json::array({ { { "id", "a" }, { "sessionId", "sa" } } }) } }.dump());
    r.record(T_DIR, 9001, "sa", "idle"); r.procs.alivePids.insert(9001);
    Busy b = guardAtStart(r.files, r.procs, r.env, now, od, ALIAS, P("C:\\home"));
    check("guard: a scratch deck does not look at a real Electron deck's process", !b.busy && b.source == "none", b.why);
    od["otherExe"] = EEXE;
    b = guardAtStart(r.files, r.procs, r.env, now, od, ALIAS, P("C:\\home"));
    check("guard: ... only at the test's stand-in", b.busy && b.n == 1 && b.source == "process", b.why);
  }
  {
    Rig r; r.env.pidV = 4242; r.procs.snapOk = false;
    marker(r, "electron", 777, EEXE, 2);
    r.procs.alivePids.insert(777);
    Busy b = guardAtStart(r.files, r.procs, r.env, now, oneDeckOpt(), ALIAS, P("C:\\home"));
    check("guard: no snapshot: an alive marker pid counts as its exe (errs on the side of blocking)", b.busy && b.n == 2, b.why);
  }
}

// THE SAME-SESSION GUARD (D::spawnTile / sessionHolder / bringHere): a session live in another process is not resumed
void testSameSession() {
  const std::string S1 = "5e550000-0000-4000-8000-0000000000a1", S2 = "5e550000-0000-4000-8000-0000000000a2";
  auto seed = [&](Rig& r, const std::string& sid) {
    r.files.put(P("C:\\state\\tiles.json"), json{ { "tiles", json::array({ { { "id", "held1" }, { "kind", "claude" }, { "account", "claudet" }, { "sessionId", sid }, { "cwd", P("C:\\work") }, { "title", "held" } } }) },
      { "windows", json::array({ { { "bounds", nullptr }, { "layout", { { "held1", json::array({ 0 }) } } } } }) } }.dump());
  };
  auto spawnedFor = [](Rig& r, const std::string& sid) { int n = 0; for (const auto& sp : r.ptys.spawned) for (const auto& a : sp.args) if (a == sid) n++; return n; };
  {
    Rig r; r.opt.allowClaude = true; r.opt.oneDeck = oneDeckOpt(); r.env.pidV = 4242; r.files.makeDirs(P("C:\\work"));
    seed(r, S1);
    r.record(T_DIR, 9001, S1, "idle"); r.procs.alivePids.insert(9001);   // the session runs in a terminal
    const ClientId w = r.boot();
    json t = r.tile("held1");
    check("same: a session live in a terminal is not resumed at the restore", spawnedFor(r, S1) == 0 && t["exited"] == true && t["elsewhere"] == "terminal", t.dump().substr(0, 300));
    r.advance(12000);
    check("same: the start-up retries (5, 2 s apart) find it held each time: still not started", spawnedFor(r, S1) == 0 && r.tile("held1")["elsewhere"] == "terminal");
    check("same: the deck log says so", r.log.count("is open in another window (pid 9001): not resumed here") >= 1, r.log.all().substr(0, 600));
    check("same: the guard killed nothing", r.procs.killedTrees.empty() && r.procs.alive(9001));
    r.res(w, "restartTile", json::array({ "held1" }));
    check("same: resume looks again, and refuses again", spawnedFor(r, S1) == 0 && r.tile("held1")["elsewhere"] == "terminal");
    r.call(w, "bringHere", json::array({ "held1" }), 6000);
    check("same: Bring it here ends that process (the user's click)", r.procs.killedTrees.size() == 1 && r.procs.killedTrees[0] == 9001);
    r.advance(1000);
    t = r.tile("held1");
    check("same: ... and resumes it here", spawnedFor(r, S1) == 1 && !t.contains("elsewhere") && t["exited"] == false, t.dump().substr(0, 300));
    // this deck's own process is never "elsewhere": a refresh while its own claude.exe record is still alive
    const uint32_t own = r.ptys.live.rbegin()->second->pid_;
    r.record(T_DIR, own, S1, "idle");
    r.call(w, "refreshTile", json::array({ "held1" }), 9000);
    r.advance(1000);
    check("same: a refresh while this deck's own old process still shows: resumed (its own pids never count)", spawnedFor(r, S1) == 2 && !r.tile("held1").contains("elsewhere"));
    r.deck->shutdown();
  }
  {
    // the other deck runs with sessions and has the tile (not asleep): "open in the other deck", no Bring it here
    Rig r; r.opt.allowClaude = true; r.opt.oneDeck = oneDeckOpt(); r.env.pidV = 4242; r.files.makeDirs(P("C:\\work"));
    seed(r, S2);
    const ClientId w = r.boot();
    r.files.put(std::string(SHARED_DIR) + P("\\active.json"), json{ { "v", 1 }, { "deck", "electron" }, { "running", true }, { "pid", 777 }, { "exe", P("C:\\e\\electron.exe") }, { "args", json::array() },
      { "stateDir", P("C:\\e-state") }, { "startedAt", isoTime(r.clock.t) }, { "heartbeatAt", isoTime(r.clock.t) }, { "liveSessions", 1 } }.dump());
    r.procs.alivePids.insert(777);
    r.files.put(P("C:\\e-state\\tiles.json"), json{ { "tiles", json::array({ { { "id", "x" }, { "sessionId", S2 } } }) } }.dump());
    const size_t before = r.ptys.spawned.size();
    r.call(w, "refreshTile", json::array({ "held1" }), 9000); r.advance(1000);
    json t = r.tile("held1");
    check("same: the other Claude Deck's tile: \"deck\", not started here", t["elsewhere"] == "deck" && r.ptys.spawned.size() == before, t.dump().substr(0, 300));
    r.call(w, "bringHere", json::array({ "held1" }), 2000);
    check("same: no Bring it here for the other deck's session (nothing killed)", r.procs.killedTrees.empty() && r.tile("held1")["elsewhere"] == "deck");
    r.deck->shutdown();
  }
}

// THE TRANSLATORS AND THE DECISION, against lib/onedeck.js's own answers (core/test/deck/gen-onedeck.js) - and the round
// trip: Electron -> shared (JS) -> native (here) -> shared (here) -> Electron (here and JS)
void testTranslators(const std::string& fx) {
  using namespace deck::core::onedeck;
  const json f = loadFixture(fx, "onedeck");
  const double AT = f["at"].get<double>();
  const json& e = f["electron"];
  const json shared = toShared(e["tilesJson"], e["held"], e["triggers"], e["settings"], "electron", AT, 7, f["shared"]["lastChange"]);
  check("translate: the Electron deck's files -> the shared file, as the JS makes it", shared == f["shared"], shared.dump().substr(0, 400));
  check("translate: no bounds, no phone, no token, no fonts in the shared file", shared.dump().find("bounds") == std::string::npos && shared.dump().find("TOKEN") == std::string::npos
    && !shared["settings"].contains("phone") && !shared["settings"].contains("fontSize") && shared["settings"]["resumeAfterRestart"] == true);
  const Own n = fromShared(f["shared"], f["nativeOwn"]["tilesJson"], f["nativeOwn"]["settings"]);
  check("translate: shared -> the native deck's files, as the JS makes them", n.tilesJson == f["intoNative"]["tilesJson"] && n.held == f["intoNative"]["held"] && n.triggers == f["intoNative"]["triggers"] && n.settings == f["intoNative"]["settings"], n.tilesJson.dump().substr(0, 400));
  check("translate: the native deck keeps its own window bounds, phone and token", n.tilesJson["windows"][0]["bounds"]["width"] == 1400 && n.tilesJson["windows"][1]["bounds"].is_null()
    && n.settings["phone"]["token"] == "NATIVETOKEN00000" && n.settings["fontSize"] == 13 && n.settings["grid"]["cols"] == 4);
  const json again = toShared(n.tilesJson, n.held, n.triggers, n.settings, "native", AT + 1000, 7, f["shared"]["lastChange"]);
  json a = again, b = f["shared"]; for (const char* k : { "writtenBy", "writtenAt" }) { a.erase(k); b.erase(k); }
  check("round trip: native -> shared is the same shared file (but who wrote it and when)", a == b, a.dump().substr(0, 400));
  const Own back = fromShared(again, e["tilesJson"], e["settings"]);
  check("round trip: ... -> the Electron deck's files, as the JS makes them", back.tilesJson == f["backToElectron"]["tilesJson"] && back.settings == f["backToElectron"]["settings"]);
  check("round trip: every tile, session, title, group, pause mark (by:prompt too), auditor / child, cloud id as it was", back.tilesJson["tiles"] == e["tilesJson"]["tiles"]);
  check("round trip: both windows and every cell, with the Electron deck's own bounds; groups; the desk layout kept", back.tilesJson["windows"] == e["tilesJson"]["windows"] && back.tilesJson["groups"] == e["tilesJson"]["groups"] && back.tilesJson["deskLayout"] == e["tilesJson"]["deskLayout"]);
  check("round trip: held prompts, triggers and every setting as they were", back.held == e["held"] && back.triggers == e["triggers"] && back.settings == e["settings"]);
  for (const json& c : f["decideCases"]) {
    const json own = c.value("own", json::object());
    const json out = decide(c["shared"], c["sync"], own.value("hasState", false), own.value("savedAt", 0.0));
    check("decide: " + str(c["name"]) + " -> " + str(c["out"]["action"]), out == c["out"], out.dump());
  }
  Marker m; m.deck = "native"; m.running = true; m.pid = 4242; m.exe = "C:\\p\\ClaudeDeckNative.exe"; m.args = { "--local" }; m.stateDir = "C:\\s";
  m.startedAt = isoTime(AT); m.heartbeatAt = isoTime(AT); m.liveSessions = 3;
  check("marker: the JS's object", markerJson(m) == f["marker"], markerJson(m).dump());
  check("the strip's words, the JS's", blockedText(0) == str(f["blocked"][0]) && blockedText(1) == str(f["blocked"][1]) && blockedText(4) == str(f["blocked"][2]));
  json ls = json::array(); for (const auto& k : LOCAL_SETTINGS) ls.push_back(k);
  check("the per-PC settings, the JS's", ls == f["localSettings"]);
}

// THE WRITER ON A RUNNING DECK: realChange counts user actions only; never over a file another deck wrote
void testSharer() {
  using namespace deck::core::onedeck;
  auto sharedOf = [](Rig& r) { return r.files.getJson(std::string(SHARED_DIR) + P("\\shared-layout.json")); };
  {
    Rig r; r.opt.oneDeck = oneDeckOpt(); r.opt.oneDeck["publishAtStart"] = "write"; r.env.pidV = 4242;
    const ClientId w = r.boot();
    json s = sharedOf(r);
    check("sharer: a deck that is the newest writes the shared file at start, realChange 0", s.is_object() && s["writtenBy"] == "native" && s["realChange"] == 0, s.dump().substr(0, 200));
    r.call(w, "sleepTile", json::array({ "none" }), 500);   // not a user action that changes the layout
    r.advance(31000);
    check("sharer: automatic saves and the heartbeat do not count", sharedOf(r)["realChange"] == 0);
    r.res(w, "setGroups", json::array({ json::array({ "floor" }) })); r.advance(400);
    s = sharedOf(r);
    check("sharer: a user action (the groups) is a real change", s["realChange"] == 1 && s["groups"] == json::array({ "floor" }) && s["lastChange"]["what"] == "changed the groups", s.dump().substr(0, 300));
    json sync = r.files.getJson(P("C:\\state\\onedeck.json"));
    check("sharer: the deck's own record: what it last wrote, its own count", sync["seen"]["realChange"] == 1 && sync["local"]["n"] == 1 && sync["seen"]["writtenAt"] == s["writtenAt"], sync.dump());
    r.call(w, "saveSettings", json::array({ { { "grid", { { "rows", 3 }, { "cols", 3 } } } } }), 1000); r.advance(400);
    s = sharedOf(r);
    check("sharer: a setting is a real change, and travels", s["realChange"] == 2 && s["settings"]["grid"]["cols"] == 3 && !s["settings"].contains("phone"), s.dump().substr(0, 300));
    // another deck writes the shared file (beside this one, which had no sessions): this one never writes over it
    json theirs = s; theirs["writtenBy"] = "electron"; theirs["writtenAt"] = isoTime(r.clock.t + 1); theirs["realChange"] = 9;
    r.files.put(std::string(SHARED_DIR) + P("\\shared-layout.json"), theirs.dump());
    r.res(w, "setGroups", json::array({ json::array({ "floor", "x" }) })); r.advance(400);
    s = sharedOf(r);
    check("sharer: a shared file another deck wrote after this one saw it is NOT overwritten", s["writtenBy"] == "electron" && s["realChange"] == 9);
    sync = r.files.getJson(P("C:\\state\\onedeck.json"));
    check("sharer: ... the change is counted as this deck's own, unsynced (asked at the next start)", sync["local"]["n"] == 3 && sync["seen"]["realChange"] == 2, sync.dump());
    check("sharer: ... and the log says so once", r.log.count("not overwriting it") == 1, r.log.all().substr(0, 400));
    const json pre = preflight(r.files, P("C:\\state"), SHARED_DIR, r.clock.t);
    check("sharer: the next start asks (both changed)", pre["decision"]["action"] == "ask", pre["decision"].dump());
    r.deck->shutdown();
  }
  {
    // the import at start: a stale deck opened by accident never writes its layout over the newer one
    Rig r; r.opt.oneDeck = oneDeckOpt(); r.env.pidV = 4242;
    r.files.put(P("C:\\state\\tiles.json"), json{ { "tiles", json::array({ { { "id", "stale" }, { "kind", "claude" }, { "account", "claudet" }, { "sessionId", "s-stale" } } }) },
      { "windows", json::array({ { { "bounds", { { "width", 900 }, { "height", 700 } } }, { "layout", { { "stale", json::array({ 0 }) } } } } }) } }.dump());
    r.files.put(P("C:\\state\\onedeck.json"), json{ { "v", 1 }, { "local", { { "n", 2 }, { "last", nullptr } } }, { "seen", { { "realChange", 4 }, { "writtenAt", "2026-11-20T00:00:00.000Z" }, { "writtenBy", "native" }, { "local", 2 } } } }.dump());
    const json newer = { { "v", 1 }, { "writtenBy", "electron" }, { "writtenAt", "2026-11-24T19:00:00.000Z" }, { "realChange", 9 }, { "lastChange", { { "what", "a new tile" }, { "at", "2026-11-24T18:59:00.000Z" }, { "by", "electron" } } },
      { "tiles", json::array({ { { "id", "fresh" }, { "kind", "claude" }, { "account", "claudet" }, { "sessionId", "s-fresh" }, { "title", "fresh" } } }) },
      { "windows", json::array({ { { "index", 0 }, { "layout", { { "fresh", json::array({ 0 }) } } } } }) }, { "groups", json::array({ "g" }) }, { "heldPrompts", json::array() }, { "triggers", json::object() }, { "settings", { { "oneWindow", true } } } };
    r.files.put(std::string(SHARED_DIR) + P("\\shared-layout.json"), newer.dump());
    const json pre = preflight(r.files, P("C:\\state"), SHARED_DIR, r.clock.t);
    check("import: the shared file is newer and this deck changed nothing: import", pre["decision"]["action"] == "import", pre["decision"].dump());
    const std::string b = importShared(r.files, P("C:\\state"), pre["shared"], r.clock.t, "import");
    check("import: this deck's own files are backed up first", r.files.has(b + P("\\tiles.json")) && r.files.getJson(b + P("\\tiles.json"))["tiles"][0]["id"] == "stale");
    r.boot();
    const json tj = r.tilesFile();
    check("import: the deck starts on the shared layout (its own window bounds kept)", tj["tiles"].size() == 1 && tj["tiles"][0]["id"] == "fresh" && tj["windows"][0]["bounds"]["width"] == 900 && tj["groups"] == json::array({ "g" }), tj.dump().substr(0, 300));
    check("import: the newer shared file is not written over by the stale deck", sharedOf(r)["tiles"][0]["id"] == "fresh" && sharedOf(r)["realChange"] == 9);
    r.deck->shutdown();
  }
}

}  // namespace

void testOneDeck(const std::string& fixtures) {
  if (want("onedeck")) { testMarker(); testGuard(); testSameSession(); testTranslators(fixtures); testSharer(); }
}
