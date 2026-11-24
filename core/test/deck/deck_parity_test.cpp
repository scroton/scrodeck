// core/test/deck/deck_parity_test.cpp — the 2026-11-19 ports as the DECK uses them, on fake seams: the ask feed
// (deck.json, meta.asks, drafts, Done, the toast), the page viewer (its calls, meta.settings.pages, and the two
// routes replayed against what the REAL lib/phone.js + lib/viewer.js + lib/asks.js answered: routes2.json), the
// on-call's desk requests (the inbox, the placement, the answer file), cloud tiles (made on a fake process, held,
// restored, read on a fake Http), the exit watchdog's core half and a clicked toast. The modules themselves are
// tested against their own fixtures in asks_test.cpp, viewer_test.cpp, deskopen_test.cpp and cloud_test.cpp.
#include "t.hpp"
#include "fakes.hpp"
#include "rig.hpp"
#include "../../deck/hash.hpp"
#include "../../deck/server.hpp"

using namespace deck::core;
namespace js = deck::session::js;

namespace {

const char* SID = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
const char* SID2 = "11111111-2222-4333-8444-555555555555";
const char* PAGE1 = "ask_deck-test_0123abcd_r1_1118-0930_beef";
const char* PAGE2 = "ask_other_89abcdef_r1_1118-0940_cafe";

json askState(const std::string& sid, const std::string& name, const std::string& pageId, const std::string& pageFile, const std::string& source = "skill") {
  return { { "session_id", sid }, { "session_name", name }, { "account", "claudet" }, { "answers", json::object() }, { "rounds", json::array({
    { { "id", pageId }, { "round", 1 }, { "status", "open" }, { "created", "2027-01-15T08:00:00-05:00" }, { "title", "Two things" }, { "source", source }, { "page", pageFile },
      { "items", json::array({ { { "qid", "Q1" }, { "key", "k1" }, { "title", "first" }, { "question", "Which colour should the button be?" } }, { { "qid", "Q2" }, { "key", "k2" }, { "title", "second" }, { "question", "And the second thing?" } } }) } } }) } };
}
json answersOf(const std::string& pageId) {
  return { { "kind", "ask-page-answers" }, { "id", pageId }, { "answered", 2 }, { "total", 2 }, { "later", 0 }, { "lines", { "Q1 (first): blue", "Q2 (second): yes" } }, { "state", json::object() } };
}

void testAsksWiring() {
  // ---- no feed: main.js with `asks` null
  { Rig r; const ClientId w = r.boot();
    check("asks: with no ask folder the feed is off: meta.asks and askLog are empty", r.fresh()["asks"] == json::array() && r.fresh()["askLog"] == json::array());
    check("asks: askDraftGet answers null, askDraft and askClear true", r.call(w, "askDraftGet", { PAGE1 })["result"].is_null() && r.res(w, "askDraft", { PAGE1, json::object() }) == true && r.res(w, "askClear", { PAGE1 }) == true);
    check("asks: askDone says the pane is not running", r.call(w, "askDone", { SID, json::array() })["error"] == "the ask pane is not running");
    check("asks: nothing was written outside the state folder", [&] { for (const auto& kv : r.files.files) if (kv.first.compare(0, 8, "C:\\state") != 0 && kv.first.compare(0, 7, "C:\\home") != 0) return false; return true; }()); }

  Rig r; r.opt.askStateDir = "C:\\asks"; r.opt.askDownloads = { "C:\\dl" }; r.opt.allowClaude = true;
  r.files.put("C:\\asks\\pages\\" + std::string(PAGE1) + ".html", "<body>page one</body>");
  r.files.put(std::string("C:\\asks\\") + SID + ".json", askState(SID, "outside session", PAGE1, "C:\\asks\\pages\\" + std::string(PAGE1) + ".html").dump());
  const ClientId w = r.boot();
  const json deckFile = r.files.getJson("C:\\asks\\deck.json");
  check("asks: deck.json is written into the skill's folder at start", deckFile["kind"] == "claude-deck-asks" && deckFile["beat_seconds"] == 30 && deckFile["picker"] == "alongside" && deckFile["deck_threshold"].is_null()
    && deckFile["use_pages"] == true && deckFile["background"] == true && deckFile["shows_pages"] == true && deckFile["migrates_drafts"] == true && deckFile.contains("pid") && deckFile.contains("at"), deckFile.dump());
  json m = r.fresh();
  check("asks: meta.asks lists the session with its page waiting", m["asks"].size() == 1 && m["asks"][0]["sessionId"] == SID && m["asks"][0]["name"] == "outside session" && m["asks"][0]["rounds"].size() == 1
    && m["asks"][0]["rounds"][0]["id"] == PAGE1 && m["asks"][0]["rounds"][0]["total"] == 2 && m["asks"][0]["rounds"][0]["drafted"] == 0, m["asks"].dump());
  check("asks: a page that was up when the deck started is not news (no toast)", r.notes.toasts.empty());
  // the heartbeat
  const std::string at0 = str(deckFile["at"]);
  r.advance(30100);
  check("asks: deck.json's heartbeat moves every 30 s", str(r.files.getJson("C:\\asks\\deck.json")["at"]) != at0);
  // settings reach the skill at once
  r.call(w, "saveSettings", { { { "asks", { { "deckThreshold", 0 }, { "picker", "pane" } } } } });
  check("asks: saving the ask settings rewrites deck.json (threshold, picker)", r.files.getJson("C:\\asks\\deck.json")["deck_threshold"] == 0 && r.files.getJson("C:\\asks\\deck.json")["picker"] == "pane", r.files.get("C:\\asks\\deck.json"));
  // drafts
  const json draft = { { "state", { { "Q1", { { "text", "blue" }, { "pick", nullptr } } } } }, { "drafted", 1 }, { "answered", 0 }, { "later", 0 }, { "total", 2 } };
  check("asks: askDraft keeps what was entered", r.res(w, "askDraft", { PAGE1, draft }) == true && r.res(w, "askDraftGet", { PAGE1 }) == draft["state"], r.call(w, "askDraftGet", { PAGE1 }).dump());
  check("asks: the draft shows in meta (drafted 1)", r.fresh()["asks"][0]["rounds"][0]["drafted"] == 1);
  r.advance(400);
  check("asks: drafts are saved in the DECK's folder (ask-drafts.json), not the skill's", r.files.getJson("C:\\state\\ask-drafts.json")["drafts"].contains(PAGE1) && !r.files.has("C:\\asks\\ask-drafts.json"));
  // a remote has the same calls
  const ClientId ph = r.deck->addRemote();
  check("asks: a phone reads the same draft", r.res(ph, "askDraftGet", { PAGE1 }) == draft["state"]);
  check("asks: askClear drops it", r.res(ph, "askClear", { PAGE1 }) == true && r.call(w, "askDraftGet", { PAGE1 })["result"].is_null());

  // ---- a NEW page while no deck window has the focus: one toast, and a click opens that session's questions
  const std::string tid = str(r.res(w, "newTile", json::array({ { { "kind", "claude" }, { "account", "claudet" }, { "sessionId", SID2 }, { "cwd", "C:\\work" } } })));
  fake::Pty* p = r.ptys.last();
  r.files.put("C:\\asks\\pages\\" + std::string(PAGE2) + ".html", "<body>page two</body>");
  r.files.put(std::string("C:\\asks\\") + SID2 + ".json", askState(SID2, "skill name", PAGE2, "C:\\asks\\pages\\" + std::string(PAGE2) + ".html", "picker").dump());
  r.call(w, "renameTile", { tid, "My session" });
  const size_t toasts0 = r.notes.toasts.size();
  r.advance(4200);
  const Toast* askToast = nullptr; for (size_t i = toasts0; i < r.notes.toasts.size(); i++) if (r.notes.toasts[i].kind == "ask") askToast = &r.notes.toasts[i];
  check("asks: a new page raises one toast naming the account, the tile's title and the count", askToast && askToast->title == "t@example.com \xC2\xB7 My session" && askToast->body == "asks you: 2 questions on a page" && askToast->tileId == tid && askToast->sessionId == SID2,
    askToast ? askToast->title + " | " + askToast->body : "no toast");
  check("asks: … and flashes the window", !r.notes.flashes.empty());
  if (askToast) {
    const size_t from = r.out[w].size();
    const ClientId raised = r.deck->toastClicked(*askToast);
    const auto ev = r.events(w, "asks:open", from);
    check("asks: a clicked ask toast names the window to raise and sends it asks:open { sessionId, tileId }", raised == w && ev.size() == 1 && ev[0] == json{ { "sessionId", SID2 }, { "tileId", tid } }, ev.empty() ? "" : ev[0].dump());
  }
  const size_t toasts1 = r.notes.toasts.size();
  r.advance(8200);
  check("asks: the same page is not news twice", r.notes.toasts.size() == toasts1 || [&] { for (size_t i = toasts1; i < r.notes.toasts.size(); i++) if (r.notes.toasts[i].kind == "ask") return false; return true; }());

  // ---- Done for a session OUTSIDE the deck: the answers file is all
  check("asks: askDone with pages of another session is refused", r.call(w, "askDone", { SID, json::array({ { { "id", PAGE2 }, { "answers", answersOf(PAGE2) } } }) })["error"] == "none of these pages is that session's");
  json empty = answersOf(PAGE1); empty["lines"] = json::array();
  check("asks: askDone with nothing answered is refused", r.call(w, "askDone", { SID, json::array({ { { "id", PAGE1 }, { "answers", empty } } }) })["error"] == "nothing answered yet");
  const json done1 = r.call(w, "askDone", { SID, json::array({ { { "id", PAGE1 }, { "answers", answersOf(PAGE1) } } }) });
  const std::string file1 = "C:\\asks\\pages\\" + std::string(PAGE1) + ".answers.json";
  check("asks: Done for a session outside the deck saves the answers file next to the page", done1["result"] == json{ { "how", "file" }, { "files", { file1 } } } && r.files.has(file1), done1.dump());
  const json saved = r.files.getJson(file1);
  check("asks: … the page's own answers, stamped by the deck", saved["kind"] == "ask-page-answers" && saved["id"] == PAGE1 && saved["via"] == "claude-deck" && saved["saved_at"].is_string() && saved["lines"].size() == 2, saved.dump());
  m = r.fresh();
  check("asks: the page reads sent, and is in the log", m["asks"].size() == 2 && [&] { for (const json& s : m["asks"]) if (s["sessionId"] == SID) return s["rounds"][0]["sent"].is_object() && s["rounds"][0]["sent"]["how"] == "file"; return false; }()
    && m["askLog"].size() == 1 && m["askLog"][0]["id"] == PAGE1 && m["askLog"][0]["how"] == "file", m["askLog"].dump());
  check("asks: askLogState answers the logged page's state", r.call(w, "askLogState", { PAGE1 })["t"] == "ok" && !r.call(w, "askLogState", { PAGE1 })["result"].is_null());

  // ---- Done for a session IN the deck whose own picker is on screen: Esc, then ONE line through the special-prompt queue
  r.record(T_DIR, p->pid_, SID2, "waiting");
  r.feed(p, "\r\n Which colour should the button be?\r\n 1. blue\r\n 2. red\r\n");
  r.fresh();
  const size_t writes0 = p->writes.size();
  const json done2 = r.call(w, "askDone", { SID2, json::array({ { { "id", PAGE2 }, { "answers", answersOf(PAGE2) } } }) });
  const std::string file2 = "C:\\asks\\pages\\" + std::string(PAGE2) + ".answers.json";
  check("asks: Done for a tile's session answers { how, files, tileId, esc, waiting }", done2["result"] == json{ { "how", "file" }, { "files", { file2 } }, { "tileId", tid }, { "esc", true }, { "waiting", false } }, done2.dump());
  check("asks: its picker was on screen while it waited: Esc went first", p->writes.size() == writes0 + 1 && p->writes.back() == "\x1b");
  const json heldRows = r.tile(tid)["held"];
  check("asks: one line naming the file is held for the session", heldRows.size() == 1 && heldRows[0]["kind"] == "next"
    && heldRows[0]["text"] == "My answers are saved in a file - read it with ask_page.py read <page id>. " + std::string(PAGE2) + " (2 of 2 answered): " + file2, heldRows.dump());
  // the session reads its answers from the file before the line lands: the line is dropped
  { json st = askState(SID2, "skill name", PAGE2, "C:\\asks\\pages\\" + std::string(PAGE2) + ".html", "picker");
    st["rounds"][0]["status"] = "answered"; st["rounds"][0]["read_at"] = "2027-01-15T08:01:00.000Z";
    st["answers"] = { { "k1", { { "status", "answered" }, { "answer", "blue" }, { "via", "file" } } }, { "k2", { { "status", "answered" }, { "answer", "yes" }, { "via", "file" } } } };
    r.clock.t += 50; r.files.put(std::string("C:\\asks\\") + SID2 + ".json", st.dump() + " "); }
  r.advance(4200);
  check("asks: once the session has read the file, the line still held for it is dropped", r.tile(tid)["held"].empty(), r.tile(tid)["held"].dump());

  // ---- quit: the skill opens its own browser window again
  r.deck->shutdown();
  check("asks: deck.json is removed when the deck stops", !r.files.has("C:\\asks\\deck.json"));
  check("asks: the skill's own state files are never written by the deck", r.files.getJson(std::string("C:\\asks\\") + SID + ".json")["session_name"] == "outside session");
}

// Two decks, one skill folder: the guard. The Electron deck (lib/asks.js) just writes deck.json every 30 s; the
// native deck must never fight it.
void testAskGuard() {
  auto electronBeat = [](Rig& r, const char* picker) {   // what lib/asks.js writeDeckFile writes, with ITS pid
    r.files.put("C:\\asks\\deck.json", json{ { "kind", "claude-deck-asks" }, { "pid", 777 }, { "at", isoTime(r.clock.t) }, { "beat_seconds", 30 }, { "deck_threshold", 0 }, { "picker", picker },
      { "use_pages", true }, { "background", true }, { "shows_pages", true }, { "migrates_drafts", true } }.dump(1));
  };
  Rig r; r.opt.askStateDir = "C:\\asks";
  const std::string page = "C:\\asks\\pages\\" + std::string(PAGE1) + ".html";
  r.files.put(page, "<body>page one</body>");
  r.files.put(std::string("C:\\asks\\") + SID + ".json", askState(SID, "outside session", PAGE1, page).dump());
  // a draft this deck kept from an earlier run, for a page that has since been answered: a writer would prune it
  r.files.put("C:\\state\\ask-drafts.json", json{ { "drafts", { { PAGE2, { { "state", { { "Q1", { { "text", "kept" } } } } }, { "drafted", 1 }, { "answered", 0 }, { "later", 0 }, { "total", 1 }, { "at", 1 } } } } }, { "sent", json::object() }, { "log", json::array() }, { "moved", json::object() } }.dump());
  electronBeat(r, "pane");
  const std::string theirs = r.files.get("C:\\asks\\deck.json");
  const ClientId w = r.boot();
  json m = r.fresh();
  check("guard: started while another deck's heartbeat is live, this deck is a guest and says so in meta.askFeed", m["askFeed"] == json{ { "state", "guest" }, { "reason", "another deck is serving the ask pages" } }, m["askFeed"].dump());
  check("guard: a guest never writes deck.json", r.files.get("C:\\asks\\deck.json") == theirs);
  check("guard: a guest still shows the waiting pages", m["asks"].size() == 1 && m["asks"][0]["rounds"][0]["id"] == PAGE1);
  check("guard: a guest refuses askDone, askDraft and askClear, naming the reason", r.call(w, "askDone", { SID, json::array({ { { "id", PAGE1 }, { "answers", answersOf(PAGE1) } } }) })["error"] == "another deck is serving the ask pages"
    && r.call(w, "askDraft", { PAGE1, json::object() })["error"] == "another deck is serving the ask pages" && r.call(w, "askClear", { PAGE1 })["error"] == "another deck is serving the ask pages"
    && r.call(w, "askDraftGet", { PAGE1 })["t"] == "ok");
  // the other deck keeps beating for two minutes: still a guest, still nothing written, nothing pruned, no toast
  r.files.put("C:\\asks\\pages\\" + std::string(PAGE2) + ".html", "<body>two</body>");
  for (int i = 0; i < 4; i++) { r.advance(30000); electronBeat(r, "pane"); if (i == 1) r.files.put(std::string("C:\\asks\\") + SID2 + ".json", askState(SID2, "second", PAGE2, "C:\\asks\\pages\\" + std::string(PAGE2) + ".html").dump()); }
  r.advance(5000);
  bool wroteShared = false; for (const auto& kv : r.files.files) if (kv.first.find(".drafts-") != std::string::npos || kv.first.find(".answers.json") != std::string::npos) wroteShared = true;
  check("guard: through two minutes of the other deck's beats: no write, no DRAFT copy, no answers file, no ask toast", r.fresh()["askFeed"]["state"] == "guest" && json::parse(r.files.get("C:\\asks\\deck.json"))["pid"] == 777 && !wroteShared
    && [&] { for (const Toast& t : r.notes.toasts) if (t.kind == "ask") return false; return true; }() && r.fresh()["asks"].size() == 2);
  check("guard: a guest's own drafts file is left as it was (nothing pruned)", r.files.getJson("C:\\state\\ask-drafts.json")["drafts"].contains(PAGE2));
  // the other deck stops (lib/asks.js removes its file at quit — or it crashed and the file goes stale)
  const double stopped = r.clock.t;
  r.advance(60000);
  check("guard: 60 s after the other deck's last beat this deck is still a guest", r.fresh()["askFeed"]["state"] == "guest" && json::parse(r.files.get("C:\\asks\\deck.json"))["pid"] == 777);
  r.advance(61000);
  json mine = r.files.getJson("C:\\asks\\deck.json");
  check("guard: once that heartbeat is 90 s stale, this deck takes over and writes its own", r.fresh()["askFeed"] == json{ { "state", "writer" }, { "reason", nullptr } } && mine["pid"] != 777 && mine["picker"] == "alongside" && r.clock.t - stopped < 125000, mine.dump());
  check("guard: as the writer it takes drafts again", r.res(w, "askDraft", { PAGE1, { { "state", { { "Q1", { { "text", "x" } } } } }, { "drafted", 1 }, { "total", 2 } } }) == true);
  // a second deck appears while this one is the writer: it steps back at its next beat rather than fight
  r.advance(31000);
  electronBeat(r, "pane");
  const std::string theirs2 = r.files.get("C:\\asks\\deck.json");
  r.advance(31000);
  check("guard: a foreign fresh write makes the writer step back to guest at its next beat", r.fresh()["askFeed"]["state"] == "guest" && r.files.get("C:\\asks\\deck.json") == theirs2);
  r.call(w, "saveSettings", { { { "asks", { { "picker", "alongside" } } } } });
  check("guard: a guest's settings change does not reach deck.json", r.files.get("C:\\asks\\deck.json") == theirs2);
  r.deck->shutdown();
  check("guard: a guest's shutdown leaves the other deck's deck.json alone", r.files.get("C:\\asks\\deck.json") == theirs2);
  // the file removed outright (a clean quit of the other deck): takeover at the next beat
  { Rig q; q.opt.askStateDir = "C:\\asks"; electronBeat(q, "pane"); q.boot();
    check("guard: (guest at start)", q.fresh()["askFeed"]["state"] == "guest");
    q.files.remove("C:\\asks\\deck.json"); q.advance(31000);
    check("guard: the other deck's file gone: writer at the next beat", q.fresh()["askFeed"]["state"] == "writer" && q.files.has("C:\\asks\\deck.json")); }
  { Rig q; const ClientId qw = q.boot(); (void)qw; check("guard: with no feed meta.askFeed says off", q.fresh()["askFeed"] == json{ { "state", "off" }, { "reason", nullptr } }); }
}

// ---------------------------------------------------------------- the viewer + the two routes, against the real server's answers
struct Res2 { int status = 0; std::map<std::string, std::string> h; std::string body; };
Res2 parse2(const std::string& wire, bool head) {
  Res2 r; const size_t end = wire.find("\r\n\r\n");
  if (end == std::string::npos || wire.compare(0, 9, "HTTP/1.1 ") != 0) return r;
  r.status = std::atoi(wire.substr(9, 3).c_str());
  size_t at = wire.find("\r\n") + 2;
  while (at < end + 2) { const size_t e = wire.find("\r\n", at); const std::string line = wire.substr(at, e - at); at = e + 2; const size_t c = line.find(':'); if (c != std::string::npos) r.h[lowerAscii(line.substr(0, c))] = line.substr(c + 2); }
  if (!head) r.body = wire.substr(end + 4);
  return r;
}
void testViewerWiring(const std::string& fx) {
  const json f = loadFixture(fx, "routes2");
  Rig r; fake::Listener listener; fake::Net net;
  r.opt.askStateDir = str(f["stateDir"]);
  for (auto it = f["files"].begin(); it != f["files"].end(); ++it) r.files.put(it.key(), it.value().contains("hex") ? unhex(str(it.value()["hex"])) : str(it.value()["text"]));
  r.files.put("C:\\state\\settings.json", json{ { "phone", { { "enabled", true }, { "port", 8813 }, { "bind", "localhost" }, { "token", f["token"] } } } }.dump());
  ServerOptions so; so.assetRoot = "C:\\no-assets";
  Server server(so, r.files, r.clock, listener, net);
  r.phone = &server;
  r.make(); server.attach(r.deck.get());
  const ClientId w = r.deck->addWindow(); r.deck->start(); r.advance(2100);
  check("viewer: the server follows the settings", server.running() && listener.host == "127.0.0.1" && server.pageKey() == str(f["pageKey"]), server.error());
  // viewHtml: the page's folder becomes servable
  const json shown = r.call(w, "viewHtml", { f["page"] });
  check("viewer: viewHtml answers { dir, name, label, path }", shown["result"]["dir"] == f["shown"]["dir"] && shown["result"]["name"] == f["shown"]["name"] && shown["result"]["label"] == "report.html" && shown["result"]["path"] == f["page"], shown.dump());
  check("viewer: viewHtml refuses what is not an .html file on this PC, in lib/viewer.js's words", str(r.call(w, "viewHtml", { "relative.html" })["error"]).compare(0, 16, "not a full path:") == 0
    && r.call(w, "viewHtml", { "C:\\nope\\x.txt" })["error"] == "not an .html file: x.txt" && r.call(w, "viewHtml", { "C:\\nope\\gone.html" })["error"] == "not on this PC: C:\\nope\\gone.html", r.call(w, "viewHtml", { "relative.html" }).dump());
  uint64_t conn = 100; int i = 0;
  for (const json& q : f["requests"]) {
    const std::string method = str(q["method"]), name = "route2 #" + std::to_string(i++) + " " + method + " " + str(q["target"]).substr(0, 90);
    std::string raw = method + " " + str(q["target"]) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    for (auto it = q["headers"].begin(); it != q["headers"].end(); ++it) raw += it.key() + ": " + str(it.value()) + "\r\n";
    raw += "Connection: close\r\n\r\n";
    const uint64_t c = conn++;
    server.onAccept(c); server.onBytes(c, raw.data(), raw.size());
    const Res2 got = parse2(listener.sent[c], method == "HEAD");
    const json& e = q["res"];
    auto hdr = [&](const char* k) { auto it = got.h.find(k); return it == got.h.end() ? json() : json(it->second); };
    check(name + ": status " + e["status"].dump(), got.status == e["status"].get<int>(), std::to_string(got.status) + " " + got.body.substr(0, 200));
    check(name + ": content type, cache policy, cookie, CSP, referrer policy, nosniff", hdr("content-type") == e["type"] && hdr("cache-control") == e["cache"] && hdr("set-cookie") == e["cookie"]
      && hdr("content-security-policy") == e["csp"] && hdr("referrer-policy") == e["referrer"] && hdr("x-content-type-options") == e["nosniff"],
      json{ hdr("content-type"), hdr("cache-control"), hdr("set-cookie"), hdr("content-security-policy"), hdr("referrer-policy"), hdr("x-content-type-options") }.dump());
    const auto eb = editionBody(e);
    check(name + ": the body, byte for byte", got.body.size() == eb.first && hexOf(sha256(got.body)) == eb.second, got.body.substr(0, 300));
  }
  // Settings -> Phone -> "Pages on the phone": labels and viewer ids only, never a path
  r.call(w, "saveSettings", { { { "pages", { { { "label", "Report" }, { "file", f["page"] } } } } } });
  const json pages = r.fresh()["settings"]["pages"];
  check("viewer: meta.settings.pages carries the label and the viewer's ids, never the path", pages == json::array({ { { "label", "Report" }, { "dir", f["shown"]["dir"] }, { "name", "report.html" } } }), pages.dump());
  // sessionHtml
  check("viewer: sessionHtml of no session is refused", r.call(w, "sessionHtml", { "nope" })["error"] == "no such session");
  r.opt.allowClaude = false;
  const std::string tid = r.claude(w);
  const std::string sid = str(r.tile(tid)["sessionId"]);
  check("viewer: a session with no transcript lists nothing", r.res(w, "sessionHtml", { tid }) == json::array());
  const std::string line = json{ { "type", "assistant" }, { "timestamp", "2027-01-15T08:00:05.000Z" }, { "message", { { "role", "assistant" }, { "content", json::array({ { { "type", "text" }, { "text", "I wrote " + str(f["page"]) + " for you." } } }) } } } }.dump() + "\n";
  r.files.put(r.transcriptPath(T_DIR, sid), line);
  const json list = r.res(w, "sessionHtml", { tid });
  check("viewer: sessionHtml lists the page the session named, on disk now", list.size() == 1 && list[0]["path"] == f["page"] && list[0]["name"] == "report.html" && list[0].contains("mtime") && list[0].contains("size"), list.dump());
  const ClientId ph = r.deck->addRemote();
  check("viewer: a phone has both calls", r.res(ph, "sessionHtml", { tid }) == list && r.call(ph, "viewHtml", { f["page"] })["t"] == "ok");
}

// ---------------------------------------------------------------- the on-call's desks
json deskRequest(const std::string& id, const std::string& sid, json more = json::object()) {
  json q = { { "id", id }, { "kind", "desk-open" }, { "desk", "web" }, { "title", "web desk" }, { "account", "C:/home/.claude-t" }, { "accountName", "claudet" }, { "cwd", "C:\\work" }, { "sessionId", sid },
    { "prompt", "Three bells are waiting.\r\nStart with the oldest." }, { "note", "two more bells" }, { "model", "claude-fable-5-1" }, { "bells", { "b1", "b2" } }, { "expiresAt", "2099-01-01T00:00:00.000Z" } };
  for (auto it = more.begin(); it != more.end(); ++it) q[it.key()] = it.value();
  return q;
}
void testDeskOpenWiring() {
  const std::string dir = "C:\\state\\desk-open\\";
  const std::string id1 = "web-20270115T080000Z-ab12", id2 = "web-20270115T080100Z-ab13", id3 = "sec-20270115T080200Z-ab14", id4 = "web-20270115T080300Z-ab15";
  // ---- a deck that may not start Claude answers no, and makes no tile
  { Rig r; r.files.makeDirs("C:\\work"); const ClientId w = r.boot(); (void)w;
    r.files.put(dir + id1 + ".request.json", deskRequest(id1, SID).dump());
    r.advance(1600);
    const json ack = r.files.getJson(dir + id1 + ".ack.json");
    check("desk-open: with allowClaude off the request is answered no, before any tile exists", ack["ok"] == false && ack["id"] == id1 && ack["error"] == "starting Claude sessions is switched off in the native local deck (allowClaude)" && r.fresh()["tiles"].empty(), ack.dump());
    check("desk-open: the request was taken (renamed away) and the taken file cleared", !r.files.has(dir + id1 + ".request.json") && !r.files.has(dir + id1 + ".taken.json"));
    check("desk-open: one line in log.jsonl, without the prompt", r.files.get(dir + "log.jsonl").find(id1) != std::string::npos && r.files.get(dir + "log.jsonl").find("Three bells") == std::string::npos, r.files.get(dir + "log.jsonl")); }

  Rig r; r.opt.allowClaude = true; r.files.makeDirs("C:\\work");
  const ClientId w = r.boot();
  // ---- a new desk: a tile on the account, in the folder, with its first message; first free cell of the main window
  r.files.put(dir + id1 + ".request.json", "\xEF\xBB\xBF" + deskRequest(id1, SID).dump());
  const size_t from = r.out[w].size();
  r.advance(1600);
  json ack = r.files.getJson(dir + id1 + ".ack.json");
  fake::Pty* p = r.ptys.last();
  check("desk-open: the answer names the tile, its pid and where it landed", ack["ok"] == true && ack["sessionId"] == SID && ack["tileId"].is_string() && p && ack["pid"] == p->pid_ && ack["window"] == 0 && ack["primary"] == true
    && ack["cells"] == json::array({ 1 }) && ack["newWindow"] == false && ack["how"] == "opened", ack.dump());
  const std::string tid = str(ack["tileId"]);
  const ClaudeLaunch l = r.deck->lastClaudeLaunch(tid);
  check("desk-open: the launch carries the model and the first message last, on a new session id", l.args.size() == 5 && l.args[0] == "--session-id" && l.args[1] == SID && l.args[2] == "--model" && l.args[3] == "claude-fable-5-1"
    && l.args[4] == "Three bells are waiting.\nStart with the oldest." && l.cwd == "C:\\work" && hasEnv(l.env, "CLAUDE_ONCALL_KIND", "web-desk") && hasEnv(l.env, "CLAUDE_CONFIG_DIR", T_DIR), json(l.args).dump());
  json t = r.tile(tid);
  check("desk-open: the tile is the desk, titled, pulsing until typed into", t["desk"] == "web" && t["attention"] == true && t["title"] == "On-call web desk" && t["inWindow"] == 0, t.dump());
  const auto placed = r.events(w, "layout:place", from);
  check("desk-open: the window is told to place it without selecting it", placed.size() == 1 && placed[0] == json{ { "id", tid }, { "cells", { 1 } } }, placed.empty() ? "" : placed[0].dump());
  const Toast* toast = nullptr; for (const Toast& x : r.notes.toasts) if (x.kind == "desk") toast = &x;
  check("desk-open: a toast while no deck window has the focus, in main.js's words", toast && toast->title == "t@example.com \xC2\xB7 On-call web desk" && toast->body == "the on-call opened the web desk: the main window, cell 1 - 2 bells" && toast->tileId == tid,
    toast ? toast->title + " | " + toast->body : "none");
  check("desk-open: tiles.json keeps the desk", r.tilesFile()["tiles"][0]["desk"] == "web" && r.tilesFile()["windows"][0]["layout"][tid] == json::array({ 1 }), r.tilesFile().dump());
  if (toast) { const size_t f2 = r.out[w].size(); check("desk-open: a clicked toast activates the tile", r.deck->toastClicked(*toast) == w && r.events(w, "tiles:activate", f2) == std::vector<json>{ json(tid) }); }
  // typing into it ends the pulse
  r.send(w, { { "t", "call" }, { "rid", 0 }, { "fn", "userInput" }, { "args", { tid } } });
  r.clock.t += 5;
  r.send(w, { { "t", "call" }, { "rid", 0 }, { "fn", "userInput" }, { "args", { tid } } });
  check("desk-open: the pulse ends when the user types into it", r.tile(tid)["attention"] == false);

  // ---- the same session again while it runs: its one line, held for its next idle prompt; never a second process
  const size_t spawned = r.ptys.spawned.size();
  r.files.put(dir + id2 + ".request.json", deskRequest(id2, SID).dump());
  r.advance(1600);
  ack = r.files.getJson(dir + id2 + ".ack.json");
  check("desk-open: a running desk is told, not started twice", ack["ok"] == true && ack["how"] == "typed" && ack["tileId"] == tid && r.ptys.spawned.size() == spawned && ack["cells"] == json::array({ 1 }), ack.dump());
  check("desk-open: … its line is a held prompt", r.tile(tid)["held"].size() == 1 && r.tile(tid)["held"][0]["text"] == "two more bells", r.tile(tid)["held"].dump());

  // ---- switched off in the settings; an unknown account; a bad request
  r.call(w, "saveSettings", { { { "deskOpen", false } } });
  const bool kept = r.deck->settings().value("deskOpen", true);
  r.files.put(dir + id3 + ".request.json", deskRequest(id3, SID2, { { "desk", "security" } }).dump());
  r.advance(1600);
  ack = r.files.getJson(dir + id3 + ".ack.json");
  if (!kept) check("desk-open: settings.deskOpen off refuses", ack["ok"] == false && ack["error"] == "desk requests are switched off in the deck (settings.json deskOpen)", ack.dump());
  else check("desk-open: (settings.deskOpen is not a setting a view can change: the request was served)", ack["ok"] == true, ack.dump());
  { Rig q; q.opt.allowClaude = true; q.files.makeDirs("C:\\work"); q.boot();
    q.files.put(dir + id1 + ".request.json", deskRequest(id1, SID, { { "account", "C:/home/.claude-nobody" } }).dump());
    q.files.put(dir + id2 + ".request.json", deskRequest(id2, SID, { { "expiresAt", "2020-01-01T00:00:00.000Z" } }).dump());
    q.files.put(dir + id4 + ".request.json", deskRequest(id4, SID, { { "cwd", "C:\\nowhere" } }).dump());
    const std::vector<json> acks = q.deck->drainDeskInbox();
    check("desk-open: an unknown account, an expired request and a missing folder are each answered no", acks.size() == 3 && q.files.getJson(dir + id1 + ".ack.json")["error"] == "no deck account has the folder C:/home/.claude-nobody"
      && q.files.getJson(dir + id2 + ".ack.json")["error"] == "expired: the on-call stopped waiting for it" && q.files.getJson(dir + id4 + ".ack.json")["error"] == "no such folder: C:\\nowhere" && q.ptys.spawned.empty(),
      q.files.get(dir + id1 + ".ack.json") + q.files.get(dir + id2 + ".ack.json") + q.files.get(dir + id4 + ".ack.json")); }

  // ---- no cell free anywhere: a new auxiliary window when the shell can open one, else the main window's sidebar
  for (bool can : { false, true }) {
    Rig q; q.opt.allowClaude = true; q.files.makeDirs("C:\\work"); q.life.canOpenWindow = can;
    const ClientId qw = q.boot();
    const std::string a = q.shell(qw), b = q.shell(qw);
    q.send(qw, { { "t", "layout" }, { "layout", { { a, { 1, 2 } }, { b, { 3, 4 } } } } });
    q.files.put(dir + id1 + ".request.json", deskRequest(id1, SID, { { "chrome", true } }).dump());
    const std::vector<json> acks = q.deck->drainDeskInbox();
    const json a1 = acks.empty() ? json() : acks[0];
    if (!can) check("desk-open: every cell taken and a shell with one window: the desk starts and stays in the main window's sidebar", a1["ok"] == true && a1["how"] == "opened" && a1["newWindow"] == false && a1["cells"] == json::array() && a1["window"] == 0
      && a1["primary"] == true && q.life.windows.empty() && q.deck->windowIndex(qw) == 0, a1.dump());
    else {
      check("desk-open: every cell taken: a new auxiliary window, the tile alone in its top-left cell, shown inactive", a1["ok"] == true && a1["newWindow"] == true && a1["cells"] == json::array({ 1 }) && a1["window"] == 1 && a1["primary"] == false
        && q.life.windows.size() == 1 && q.life.windows[0].inactive && q.life.windows[0].bounds == json{ { "width", 960 }, { "height", 640 }, { "x", 200 }, { "y", 160 } }, a1.dump() + (q.life.windows.empty() ? "" : q.life.windows[0].bounds.dump()));
      const ClientId nw = q.life.windows.empty() ? 0 : q.life.windows[0].client;
      check("desk-open: the new window is a client of the deck with the tile in its layout, and is sent the hello", q.deck->windowIndex(nw) == 1 && q.count(nw, "init") == 1 && q.tile(str(a1["tileId"]))["inWindow"] == 1);
      check("desk-open: --chrome when the on-call asked for the browser", [&] { const ClaudeLaunch l2 = q.deck->lastClaudeLaunch(str(a1["tileId"])); return std::find(l2.args.begin(), l2.args.end(), "--chrome") != l2.args.end(); }());
    }
  }
  // ---- an asleep desk is woken in place with the message
  { Rig q; q.opt.allowClaude = true; q.files.makeDirs("C:\\work");
    q.transcript(T_DIR, SID, "web desk", "done for now", 3000);
    q.files.put("C:\\state\\tiles.json", json{ { "tiles", { { { "id", "tile-desk" }, { "kind", "claude" }, { "account", "claudet" }, { "sessionId", SID }, { "cwd", "C:\\work" }, { "asleep", true }, { "sleptAt", 5 }, { "desk", "web" } } } } }.dump());
    const ClientId qw = q.boot(); (void)qw;
    check("desk-open: (the desk came back asleep)", q.tile("tile-desk")["status"] == "asleep" && q.ptys.spawned.empty());
    q.files.put(dir + id1 + ".request.json", deskRequest(id1, SID, { { "resume", true } }).dump());
    const std::vector<json> acks = q.deck->drainDeskInbox();
    const ClaudeLaunch l2 = q.deck->lastClaudeLaunch("tile-desk");
    check("desk-open: a sleeping desk is woken in its own tile, resumed, with the message", acks.size() == 1 && acks[0]["ok"] == true && acks[0]["how"] == "woken" && acks[0]["tileId"] == "tile-desk" && q.ptys.spawned.size() == 1
      && l2.args.size() >= 2 && l2.args[0] == "--resume" && l2.args[1] == SID && l2.args.back() == "Three bells are waiting.\nStart with the oldest." && q.tile("tile-desk")["asleep"] == false, acks.empty() ? "" : acks[0].dump() + json(l2.args).dump()); }
  // ---- a deck on its way out opens nothing
  r.deck->shutdown();
  r.files.put(dir + id4 + ".request.json", deskRequest(id4, SID2).dump());
  check("desk-open: after shutdown no request is taken", r.deck->drainDeskInbox().empty() && r.files.has(dir + id4 + ".request.json"));
}

// ---------------------------------------------------------------- cloud sessions
void testCloudWiring() {
  const char* CID = "session_01SyntheticCloudId000000";
  // ---- off (the default): refused by name, after main.js's own checks
  { Rig r; r.opt.allowClaude = true; r.files.makeDirs("C:\\work"); const ClientId w = r.boot();
    check("cloud: meta.cloudOption is false while the deck may not make one", r.fresh()["cloudOption"] == false);
    check("cloud: a task is asked for first, in main.js's words", r.call(w, "newTile", json::array({ { { "kind", "cloud" }, { "account", "claudet" }, { "task", "  \r\n " } } }))["error"] == "a cloud session starts from a task: say what it should do");
    check("cloud: a task over 24,000 characters is refused with the counts", r.call(w, "newTile", json::array({ { { "kind", "cloud" }, { "account", "claudet" }, { "task", std::string(24001, 'x') } } }))["error"]
      == "the task is 24,001 characters; a command line takes at most 24,000. Put the detail in a file in the repo and point the task at it.");
    check("cloud: a folder that is not there is refused", r.call(w, "newTile", json::array({ { { "kind", "cloud" }, { "account", "claudet" }, { "task", "do it" }, { "cwd", "C:\\nowhere" } } }))["error"] == "no such folder: C:\\nowhere");
    check("cloud: making one is refused unless the shell allows it, and nothing is started", r.call(w, "newTile", json::array({ { { "kind", "cloud" }, { "account", "claudet" }, { "task", "do it" }, { "cwd", "C:\\work" } } }))["error"]
      == "making cloud sessions is switched off in the native local deck (allowCloudCreate)" && r.ptys.spawned.empty() && r.fresh()["tiles"].empty());
    // a cloud session from Elsewhere is held either way: it runs nothing here
    const json opened = r.call(w, "adoptSession", { { { "cloudId", "cse_01SyntheticCloudId000000" }, { "account", "claudet" } } });
    const std::string tid = str(opened["result"]);
    json t = r.tile(tid);
    check("cloud: adoptSession({ cloudId }) opens the session as a tile that runs nothing", !tid.empty() && r.ptys.spawned.empty() && t["kind"] == "cloud" && t["cloudId"] == CID && t["bridge"] == CID
      && t["cloudUrl"] == std::string("https://claude.ai/code/") + CID && t["status"] == "idle" && t["fresh"] == true && t["exited"] == false && t["ptySize"].is_null(), t.dump());
    check("cloud: … once", r.call(w, "adoptSession", { { { "cloudId", CID }, { "account", "claudet" } } })["error"] == "already open here"
      && r.call(w, "adoptSession", { { { "cloudId", "session_x" }, { "account", "claudet" } } })["error"] == "not a cloud session id"
      && r.call(w, "adoptSession", { { { "cloudId", CID }, { "account", "nobody" } } })["error"] == "unknown account: nobody");
    check("cloud: tiles.json keeps it", r.tilesFile()["tiles"].size() == 1 && r.tilesFile()["tiles"][0]["kind"] == "cloud" && r.tilesFile()["tiles"][0]["cloudId"] == CID, r.tilesFile().dump());
    r.call(w, "refreshTile", { tid }); r.call(w, "restartTile", { tid });
    check("cloud: refresh and restart of a held session start nothing", r.ptys.spawned.empty() && r.call(w, "sleepTile", { tid })["result"] == false);
    check("cloud: it cannot be moved between accounts", r.call(w, "moveTile", { tid, "claudeu" })["error"] == "only Claude sessions can be moved");
    // open:external
    check("cloud: openExternal opens claude.ai links only", r.call(w, "openExternal", { "https://example.com/" })["error"] == "only claude.ai links open from here"
      && r.call(w, "openExternal", { "https://claude.ai.evil.example/" })["error"] == "only claude.ai links open from here"
      && r.res(w, "openExternal", { std::string("https://claude.ai/code/") + CID }) == true && r.life.opened == std::vector<std::string>{ std::string("https://claude.ai/code/") + CID });
    check("cloud: a remote cannot reach openExternal", r.call(r.deck->addRemote(), "openExternal", { "https://claude.ai/" })["error"] == "unknown action: openExternal");
    // the deck comes back with it
    Rig r2; r2.files.files = r.files.files; r2.opt.allowClaude = true; const ClientId w2 = r2.boot(); (void)w2; r2.advance(1000);
    check("cloud: a restored cloud tile is its session again, with no process", r2.fresh()["tiles"].size() == 1 && r2.tile(tid)["cloudId"] == CID && r2.ptys.spawned.empty(), r2.fresh()["tiles"].dump()); }

  // ---- the cloud list and the tiles' status read each account's Claude Code sign-in: the PUBLIC build does it only
  // with settings.json `cloudLive` true (core/deck/edition.hpp; 2026-11-24 ask r4 Q5), the owner's deck always
  { Rig q; q.opt.allowClaude = true; q.cloud = true;
    q.creds.oauth[T_DIR] = { { "claudeAiOauth", { { "accessToken", "TOK-T" }, { "expiresAt", 9e15 } } } };
    const ClientId wq = q.boot();
    q.call(wq, "adoptSession", { { { "cloudId", "cse_01SyntheticCloudId000000" }, { "account", "claudet" } } });
    q.advance(130000);
    if (deck::edition::kPublic) {
      check("cloud (public build): cloudLive unset - no sign-in read, no request, Elsewhere lists no cloud session", q.http.asked.empty() && q.creds.reads == 0
        && [&] { for (const json& e : q.res(wq, "externalSessions")) if (e.value("cloud", false)) return false; return true; }(), std::to_string(q.http.asked.size()) + " asked, " + std::to_string(q.creds.reads) + " reads");
      q.call(wq, "saveSettings", { { { "cloudLive", true } } }); q.advance(20000);
      check("cloud (public build): Settings' cloudLive on - the tile's session is asked for", !q.http.asked.empty() && q.creds.reads > 0, std::to_string(q.http.asked.size()));
    } else check("cloud (the owner's deck): polled with cloudLive unset", !q.http.asked.empty() && q.creds.reads > 0);
    q.deck->shutdown(); }

  // ---- allowed: `claude --cloud "<task>"` on the (fake) process seam
  Rig r; r.opt.allowClaude = true; r.opt.allowCloudCreate = true; r.cloud = true; r.files.makeDirs("C:\\work");
  r.files.put("C:\\state\\settings.json", "{\"cloudLive\":true}");      // the public build's opt-in (ignored by the owner's deck)
  r.creds.oauth[T_DIR] = { { "claudeAiOauth", { { "accessToken", "TOK-T" }, { "expiresAt", 9e15 } } } };
  r.http.answerAtOnce = false;
  const ClientId w = r.boot();
  check("cloud: meta.cloudOption is true when the deck may make one", r.fresh()["cloudOption"] == true);
  check("cloud: usage is NOT polled just because the cloud poller has the seams", r.fresh()["usage"] == json::object() && [&] { for (const auto& a : r.http.asked) if (a.url.find("usage") != std::string::npos) return false; return true; }());
  const std::string task = "Fix the \"flaky\" test\r\nthen open a PR";
  const std::string tid = str(r.res(w, "newTile", json::array({ { { "kind", "cloud" }, { "account", "claudet" }, { "task", task }, { "cwd", "C:\\work" } } })));
  fake::Pty* p = r.ptys.last();
  check("cloud: the run is `claude --cloud <task>` in the folder, as the account", p && p->spec.file == "C:\\bin\\claude.exe" && p->spec.args == std::vector<std::string>{ "--cloud", "Fix the \"flaky\" test\nthen open a PR" } && p->spec.cwd == "C:\\work"
    && hasEnv(p->spec.env, "CLAUDE_CONFIG_DIR", T_DIR) && hasEnv(p->spec.env, "CLAUDE_DECK_TILE", tid), p ? json(p->spec.args).dump() : "");
  json t = r.tile(tid);
  check("cloud: while it runs the tile is a starting cloud tile titled by its task's first line", t["kind"] == "cloud" && t["status"] == "starting" && t["title"] == "Fix the \"flaky\" test" && t["cloudId"].is_null() && t["cloudUrl"].is_null(), t.dump());
  check("cloud: a tile with no session yet is not saved", r.tilesFile()["tiles"].empty());
  r.feed(p, "\x1b[32m\x1b[0mCreated cloud session: Flaky test fix\x1b[0m\r\nView: https://claude.ai/code/session_01SyntheticCloudId000000?from=cli&m=0\r\nResume with: claude --teleport session_01SyntheticCloudId000000\r\n");
  r.ptys.exits.emplace_back(p->token, 0); r.pump();
  t = r.tile(tid);
  check("cloud: when the run exits having made one, the tile becomes that session", t["cloudId"] == CID && t["exited"] == false && t["exitCode"].is_null() && t["status"] == "idle" && t["title"] == "Flaky test fix" && t["cloudUrl"] == std::string("https://claude.ai/code/") + CID, t.dump());
  check("cloud: … and is saved", r.tilesFile()["tiles"].size() == 1 && r.tilesFile()["tiles"][0]["cloudId"] == CID && r.tilesFile()["tiles"][0]["title"] == "Flaky test fix");
  // its status, read through the fake Http with the account's (made-up) token
  r.advance(400);
  size_t answered = 0;
  auto answerAll = [&](const json& row) {
    for (int guard = 0; guard < 20 && answered < r.http.asked.size(); guard++) {
      const auto q = r.http.asked[answered++];
      HttpResponse res; res.status = 200;
      res.body = q.url.find("?limit=") != std::string::npos ? json{ { "data", json::array({ row, { { "id", "cse_ZZZZZZZZZZZZ" }, { "title", "Another one" }, { "worker_status", "requires_action" }, { "updated_at", "2027-01-15T07:00:00Z" } } }) } }.dump()
        : json{ { "response_shape", row } }.dump();
      q.done(res);
    }
  };
  const json row = { { "id", "cse_01SyntheticCloudId000000" }, { "title", "Flaky test: fixed" }, { "worker_status", "running" }, { "last_event_at", "2027-01-15T08:00:00Z" },
    { "external_metadata", { { "post_turn_summary", { { "status_detail", "running the suite" } } }, { "last_served_model", "claude-fable-5-1" }, { "context_usage", { { "max_tokens", 200000 }, { "used_tokens", 50000 } } } } } };
  answerAll(row); r.advance(100);                 // (the list asked at start was still out: it is answered first)
  r.call(w, "refreshTile", { tid }); r.advance(5100); answerAll(row); r.advance(100);
  const fake::Http::Asked* one = nullptr;
  for (const auto& a : r.http.asked) if (a.url == std::string("https://api.anthropic.com/v1/code/sessions/") + CID) one = &a;
  check("cloud: the poller asks Anthropic's sessions endpoint for the tile with the account's token", one && one->headers.at("Authorization") == "Bearer TOK-T" && one->headers.at("anthropic-beta") == "oauth-2025-04-20"
    && one->headers.at("anthropic-version") == "2023-06-01", std::to_string(r.http.asked.size()) + " asked");
  t = r.tile(tid);
  check("cloud: the tile shows what was read: busy, the session's title, summary, model, context", t["status"] == "busy" && t["title"] == "Flaky test: fixed" && t["lastText"] == "running the suite" && t["model"] == "claude-fable-5-1" && t["context"] == 25 && t["cloudError"].is_null(), t.dump());
  check("cloud: a learned title is kept in tiles.json", r.tilesFile()["tiles"][0]["title"] == "Flaky test: fixed");
  const json ext = r.res(w, "externalSessions");
  check("cloud: Elsewhere lists the account's other live cloud session, not the one a tile holds", ext.size() == 1 && ext[0]["cloud"] == true && ext[0]["cloudId"] == "session_ZZZZZZZZZZZZ" && ext[0]["account"] == "claudet" && ext[0]["pid"] == "cloud"
    && ext[0]["status"] == "waiting" && ext[0]["name"] == "cloud session" && ext[0]["title"] == "Another one" && ext[0]["cwd"].is_null(), ext.dump());
  // the turn ends: a toast with the cloud mark and the session's summary
  r.clock.t += 60000;
  json idleRow = row; idleRow["worker_status"] = "idle"; idleRow["external_metadata"]["post_turn_summary"]["status_detail"] = "all green";
  const size_t toasts0 = r.notes.toasts.size();
  r.advance(16000); answerAll(idleRow); r.advance(2100);
  const Toast* done = nullptr; for (size_t i = toasts0; i < r.notes.toasts.size(); i++) if (r.notes.toasts[i].kind == "done") done = &r.notes.toasts[i];
  check("cloud: a cloud turn that ends is toasted with the cloud mark and its summary", done && done->title == "t@example.com \xE2\x98\x81 \xC2\xB7 Flaky test: fixed" && done->body.find(": all green") != std::string::npos && r.tile(tid)["status"] == "idle",
    done ? done->title + " | " + done->body : "no toast; status " + r.tile(tid)["status"].dump());
  // a run that makes nothing stays exited, and restart runs it again
  const std::string tid2 = str(r.res(w, "newTile", json::array({ { { "kind", "cloud" }, { "account", "claudet" }, { "task", "second" }, { "cwd", "C:\\work" } } })));
  fake::Pty* p2 = r.ptys.last();
  r.feed(p2, "Error: cloud sessions are not enabled for this account\r\n");
  r.ptys.exits.emplace_back(p2->token, 1); r.pump();
  check("cloud: a run that made no session leaves the tile exited with the run on screen", r.tile(tid2)["exited"] == true && r.tile(tid2)["exitCode"] == 1 && r.tile(tid2)["cloudId"].is_null());
  const size_t n = r.ptys.spawned.size();
  r.call(w, "restartTile", { tid2 });
  check("cloud: restart runs the same task again", r.ptys.spawned.size() == n + 1 && r.ptys.spawned.back().args == std::vector<std::string>{ "--cloud", "second" });
}

void testLifecycleBits() {
  Rig r; const ClientId w = r.boot();
  check("watchdog: nothing is armed while the deck runs", r.life.watchdogs.empty() && r.files.has("C:\\state\\running.json"));
  const json running = r.files.getJson("C:\\state\\running.json");
  check("running.json: { pid, at } as main.js markRunning writes it", running.size() == 2 && running.contains("pid") && running["at"].is_string() && str(running["at"]).size() == 24, running.dump());
  // ask:open-link (a window's): the core decides which of the three the shell does
  r.call(w, "askOpenLink", { "https://example.com/a?b=1" });
  r.call(w, "askOpenLink", { "file:///C:/Users/x/My%20Report.pdf" });
  r.call(w, "askOpenLink", { "file:///C:/Users/x/setup.EXE" });
  r.call(w, "askOpenLink", { "file:///C:/a%2Fb.txt" });
  r.call(w, "askOpenLink", { "javascript:alert(1)" });
  check("ask links: a web address opens outside, a file in its own program, something that would RUN is only shown in its folder", r.life.opened == std::vector<std::string>{ "https://example.com/a?b=1" }
    && r.life.openedPaths == std::vector<std::string>{ "C:\\Users\\x\\My Report.pdf" } && r.life.shownInFolder == std::vector<std::string>{ "C:\\Users\\x\\setup.EXE" }, json(r.life.openedPaths).dump() + json(r.life.shownInFolder).dump());
  check("ask links: a remote has no such call", r.call(r.deck->addRemote(), "askOpenLink", { "https://example.com/" })["error"] == "unknown action: askOpenLink");
  check("relaunch.json: none, no command", r.deck->relaunchSpec().is_null());
  r.files.put("C:\\state\\relaunch.json", "\xEF\xBB\xBF{ \"command\": \" wscript.exe \", \"args\": [\"//B\", 7] }");
  check("relaunch.json: the command a restart starts instead of the exe, as main.js relaunchSpec reads it", r.deck->relaunchSpec() == json{ { "command", "wscript.exe" }, { "args", { "//B", "7" } } }, r.deck->relaunchSpec().dump());
  r.files.put("C:\\state\\relaunch.json", "{ \"command\": \"  \" }");
  check("relaunch.json: a blank command is none", r.deck->relaunchSpec().is_null());
  r.deck->shutdown(); r.deck->shutdown();
  check("watchdog: the quit arms it once, for 8 s", r.life.watchdogs == std::vector<double>{ 8000 });
  check("running.json: removed on a deliberate quit", !r.files.has("C:\\state\\running.json"));
  // a restart arms it too
  Rig q; q.boot();
  check("watchdog: a restart arms it", q.res(q.deck->addWindow(), "restartNow") == true && q.life.watchdogs == std::vector<double>{ 8000 } && q.life.restarts.size() == 1);
}

}  // namespace

void testParity(const std::string& fixtures) {
  if (!want("parity")) return;
  testAsksWiring();
  testAskGuard();
  testViewerWiring(fixtures);
  testDeskOpenWiring();
  testCloudWiring();
  testLifecycleBits();
}
