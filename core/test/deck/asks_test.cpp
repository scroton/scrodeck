// core/test/deck/asks_test.cpp — core/deck/asks.cpp against core/test/fixtures/deck/asks.json, which
// core/test/deck/gen-asks.js records from the REAL lib/asks.js: the pure functions case by case, and the feed
// replayed step by step on fake::MemFiles / fake::Clock (the fixture's synthetic files under the same paths, with
// their pinned mtimes), comparing after every step what the JS showed and wrote.
#include "t.hpp"
#include "fakes.hpp"
#include "../../deck/asks.hpp"
#include "../../deck/edition.hpp"

#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

void testAsks(const std::string& fixtures);

namespace {

using namespace deck::core;
using OJ = nlohmann::ordered_json;

std::string show(const json& v) { return v.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace).substr(0, 300); }
// where two values first differ, for a failure's text
std::string diffAt(const json& got, const json& want, const std::string& at = "") {
  if (got == want) return "";
  if (got.is_object() && want.is_object()) {
    for (auto it = got.begin(); it != got.end(); ++it) {
      if (!want.contains(it.key())) return at + "/" + it.key() + ": only in the C++ = " + show(it.value());
      if (it.value() != want[it.key()]) return diffAt(it.value(), want[it.key()], at + "/" + it.key());
    }
    for (auto it = want.begin(); it != want.end(); ++it) if (!got.contains(it.key())) return at + "/" + it.key() + ": only in the JS = " + show(it.value());
  }
  if (got.is_array() && want.is_array()) {
    if (got.size() != want.size()) return at + ": " + std::to_string(got.size()) + " items, the JS has " + std::to_string(want.size()) + "; got " + show(got) + " want " + show(want);
    for (size_t i = 0; i < got.size(); i++) if (got[i] != want[i]) return diffAt(got[i], want[i], at + "/" + std::to_string(i));
  }
  return at + ": got " + show(got) + " want " + show(want);
}
void same(const std::string& name, const json& got, const json& want) { const bool ok = got == want; check(name, ok, ok ? "" : diffAt(got, want)); }
void sameText(const std::string& name, const std::string& got, const std::string& want) { check(name, got == want, "got  " + got.substr(0, 400) + "\n      want " + want.substr(0, 400)); }
json orNull(const std::string& s) { return s.empty() ? json() : json(s); }
std::string build(const json& parts) {
  std::string s;
  for (const json& p : parts) { const std::string chunk = p[0].get<std::string>(); for (int i = 0, n = p[1].get<int>(); i < n; i++) s += chunk; }
  return s;
}
bool under(const std::string& path, const std::string& dir) { return path == dir || path.compare(0, dir.size() + 1, dir + "\\") == 0; }
bool endsWith(const std::string& s, const std::string& tail) { return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0; }
std::vector<std::string> keysOf(const OJ& o) { std::vector<std::string> k; for (auto it = o.begin(); it != o.end(); ++it) k.push_back(it.key()); return k; }

void pureFunctions(const json& fx) {
  check("asks: BEAT_S", fx["beat"] == kAskBeatSeconds && kAskScanMs == 4000);
  for (const json& c : fx["ids"]) check("asks id: " + c["in"].get<std::string>(), askIdOk(c["in"].get<std::string>()) == c["ok"].get<bool>());
  int i = 0;
  for (const json& c : fx["composeDone"]) sameText("asks composeDone #" + std::to_string(i++), composeDone(c["in"]), c["out"].get<std::string>());
  i = 0;
  for (const json& c : fx["composePointer"]) sameText("asks composePointer #" + std::to_string(i++), composePointer(c["in"]), c["out"].get<std::string>());
  i = 0;
  for (const json& c : fx["picker"]) {
    const std::string recent = build(c["parts"]);
    const std::vector<std::string> questions = c["questions"].get<std::vector<std::string>>();
    check("asks pickerOnScreen #" + std::to_string(i++), pickerOnScreen(recent, questions) == c["out"].get<bool>(), recent.substr(0, 80));
  }
  i = 0;
  for (const json& seq : fx["newAskRows"]) {
    json seen;                                               // null on the first call
    int k = 0;
    for (const json& st : seq) {
      const json r = newAskRows(seen, st["view"]);
      const std::string name = "asks newAskRows #" + std::to_string(i) + "." + std::to_string(k++);
      same(name + " seen", r["seen"], st["seen"]);
      same(name + " fresh", r["fresh"], st["fresh"]);
      seen = r["seen"];
    }
    i++;
  }
  for (const json& c : fx["urls"]) {
    const std::string path = c["path"].get<std::string>();
    sameText(std::string("asks pathToFileUrl ") + (c["win"].get<bool>() ? "win " : "posix ") + show(c["path"]), pathToFileUrl(path, c["win"].get<bool>()), str(c["out"]));
  }
}

void locations(const json& fx) {
  for (const json& c : fx["locations"]) {
    fake::MemFiles mem; fake::Env env;
    env.homeDir = fx["pinned"]["home"].get<std::string>();
    env.env.clear();
    for (auto it = c["files"].begin(); it != c["files"].end(); ++it) mem.put(it.key(), it.value().get<std::string>());
    for (auto it = c["env"].begin(); it != c["env"].end(); ++it) env.env.push_back({ it.key(), it.value().get<std::string>() });
    const AskLocations got = askLocations(mem, env);
    const std::string name = "asks locations: " + c["name"].get<std::string>();
    sameText(name + " (state)", got.stateDir, c["out"]["stateDir"].get<std::string>());
    json want = c["out"]["downloads"];
    // the public build's default Downloads folders (core/deck/edition.hpp) are only ~/Downloads
    if (deck::edition::kPublic && want.size() == 2 && want[0] == "D:\\Downloads" && !c["env"].contains("ASK_PAGE_DOWNLOADS")) want.erase(want.begin());
    same(name + " (downloads)", json(got.downloads), want);
  }
}

void draftMax(const json& fx) {
  fake::MemFiles mem; fake::Clock clock;
  AskDeps deps; deps.files = &mem; deps.clock = &clock; deps.stateDir = "C:\\nowhere\\state";
  AskFeed feed(deps);
  for (const json& c : fx["draftMax"]) {
    std::string text;
    const std::string ch = c["ch"].get<std::string>();
    for (int i = 0, n = c["n"].get<int>(); i < n; i++) text += ch;
    const json payload = { { "state", { { "Q1", { { "text", text } } } } }, { "drafted", 1 } };
    check("asks DRAFT_MAX: " + std::to_string(c["length"].get<int>()) + " units of " + show(c["ch"]), feed.setDraft("ask_test-a_aaaaaaaa_r1_1110-1200_abcd", payload) == c["ret"].get<bool>());
  }
  check("asks DRAFT_MAX: nothing written without a drafts file", mem.files.empty());
}

void scenario(const json& fx, const json& sc) {
  const std::string tag = "asks [" + sc["name"].get<std::string>() + "] ";
  fake::MemFiles mem; fake::Clock clock;
  clock.t = fx["pinned"]["t0"].get<double>();
  clock.offset = fx["pinned"]["offsetMs"].get<double>();   // America/Bogota: UTC-5, no DST
  mem.clock = &clock;
  for (const json& d : sc["dirs"]) mem.dirs.insert(fake::norm(d.get<std::string>()));
  std::set<std::string> synthetic;
  int changes = 0, liveCalls = 0;
  json reads = json::array(), settings = sc["settings"];
  std::string liveMode = "set"; std::vector<std::string> liveIds;
  std::vector<std::function<void()>> timers;
  AskDeps deps;
  deps.files = &mem; deps.clock = &clock;
  deps.stateDir = sc["stateDir"].get<std::string>();
  deps.downloads = sc["downloads"].get<std::vector<std::string>>();
  deps.draftsFile = sc["draftsFile"].get<std::string>();
  deps.pid = fx["pinned"]["pid"].get<uint32_t>();
  deps.win = true;
  deps.home = fx["pinned"]["home"].get<std::string>();
  deps.settings = [&]() { return settings; };
  deps.onChange = [&]() { changes++; };
  deps.onRead = [&](const std::string& id, const json& s) { reads.push_back(json::array({ id, s })); };
  if (sc["live"].get<bool>()) deps.liveSessions = [&](std::set<std::string>& out) {
    liveCalls++;
    if (liveMode == "throw") return false;
    out.insert(liveIds.begin(), liveIds.end());
    return true;
  };
  deps.later = [&](double ms, std::function<void()> fn) { check(tag + "saveDrafts waits 300 ms", ms == 300); timers.push_back(std::move(fn)); };
  std::unique_ptr<AskFeed> feed;
  const std::string deckJson = fake::norm(deps.stateDir + "\\deck.json");
  json last = json::object();
  int n = 0;
  for (const json& st : sc["steps"]) {
    const std::string op = st["op"].get<std::string>();
    const std::string at = tag + "#" + std::to_string(n++) + " " + op + ": ";
    if (op == "put") { mem.put(st["path"].get<std::string>(), st["text"].get<std::string>(), st["mtimeMs"].get<double>()); synthetic.insert(fake::norm(st["path"].get<std::string>())); }
    else if (op == "rm") { mem.files.erase(fake::norm(st["path"].get<std::string>())); synthetic.erase(fake::norm(st["path"].get<std::string>())); }
    else if (op == "mkdir") mem.dirs.insert(fake::norm(st["path"].get<std::string>()));
    else if (op == "rmdir") {
      const std::string dir = fake::norm(st["path"].get<std::string>());
      for (auto it = mem.dirs.begin(); it != mem.dirs.end();) { if (under(*it, dir)) it = mem.dirs.erase(it); else ++it; }
      for (auto it = mem.files.begin(); it != mem.files.end();) { if (under(it->first, dir)) it = mem.files.erase(it); else ++it; }
      for (auto it = synthetic.begin(); it != synthetic.end();) { if (under(*it, dir)) it = synthetic.erase(it); else ++it; }
    }
    else if (op == "clock") clock.t = st["t"].get<double>();
    else if (op == "live") { liveMode = st["mode"].get<std::string>(); liveIds = st["ids"].get<std::vector<std::string>>(); }
    else if (op == "settings") settings = st["value"];
    else if (op == "new") feed.reset(new AskFeed(deps));
    else if (op == "scan") feed->scan();
    else if (op == "setDraft") check(at + "returns", feed->setDraft(st["id"].get<std::string>(), st["payload"]) == st["ret"].get<bool>());
    else if (op == "clearDraft") feed->clearDraft(st["id"].get<std::string>());
    else if (op == "markSent") feed->markSent(st["ids"].get<std::vector<std::string>>(), st["info"]);
    else if (op == "logDone") feed->logDone(st["id"].get<std::string>(), st["answers"], st["how"].get<std::string>(), st["title"]);
    else if (op == "answers") {
      std::string file, error;
      try { file = feed->writeAnswersFile(st["id"].get<std::string>(), st["answers"]); } catch (const std::runtime_error& e) { error = e.what(); }
      sameText(at + "the file", file, str(st["file"]));
      sameText(at + "the error", error, str(st["error"]));
    }
    else if (op == "deckFile") { synthetic.erase(deckJson); same(at + "the body", feed->writeDeckFile(), st["body"]); }
    else if (op == "removeDeckFile") feed->removeDeckFile();
    else check(at + "an op this test knows", false);
    if (!st.contains("after")) continue;
    // the JS's 300 ms timers, run at the end of the step
    { std::vector<std::function<void()>> run; run.swap(timers); for (auto& fn : run) fn(); }
    json want = st["after"];
    for (const char* k : { "view", "logView", "per", "files" }) { if (want[k] == "=") want[k] = last[k]; else last[k] = want[k]; }
    same(at + "view", feed->view(), want["view"]);
    same(at + "logView", feed->logView(), want["logView"]);
    for (auto it = want["per"].begin(); it != want["per"].end(); ++it) {
      const std::string id = it.key(); const json& w = it.value();
      const AskRound x = feed->round(id);
      json r;
      if (x.found) r = { { "sid", x.sid }, { "answered", x.answered }, { "later", x.later }, { "waiting", x.waiting }, { "items", x.items.size() }, { "round", x.round },
        { "name", x.state.contains("session_name") ? x.state["session_name"] : json() } };
      same(at + "round " + id, r, w["round"]);
      same(at + "draft " + id, feed->draft(id), w["draft"]);
      same(at + "logState " + id, feed->logState(id), w["logState"]);
      same(at + "pagePath " + id, orNull(feed->pagePath(id)), w["pagePath"]);
      same(at + "answersDir " + id, orNull(feed->answersDir(id)), w["answersDir"]);
    }
    check(at + "onChange calls", changes == want["changes"].get<int>(), std::to_string(changes) + " vs " + std::to_string(want["changes"].get<int>()));
    check(at + "liveSessions calls", liveCalls == want["liveCalls"].get<int>(), std::to_string(liveCalls) + " vs " + std::to_string(want["liveCalls"].get<int>()));
    // onRead: within one scan the C++ calls in page-id order, the JS in the order the marks were made
    { std::vector<json> a(reads.begin(), reads.end()), b(want["reads"].begin(), want["reads"].end()); std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end()); same(at + "onRead calls", json(a), json(b)); }
    // every file the deck wrote
    size_t wrote = 0;
    for (const auto& kv : mem.files) if (!synthetic.count(kv.first)) wrote++;
    check(at + "files written", wrote == want["files"].size(), std::to_string(wrote) + " vs " + std::to_string(want["files"].size()));
    for (auto it = want["files"].begin(); it != want["files"].end(); ++it) {
      const std::string path = fake::norm(it.key()), text = it.value().get<std::string>();
      const std::string name = at + "file " + it.key().substr(it.key().find_last_of('\\') + 1);
      if (!mem.has(path) || synthetic.count(path)) { check(name, false, "not written"); continue; }
      const std::string got = mem.get(path);
      if (endsWith(path, ".txt") || path == deckJson) { sameText(name, got, text); continue; }
      const json g = json::parse(got, nullptr, false), w = json::parse(text, nullptr, false);
      if (g.is_discarded()) { check(name, false, "not JSON: " + got.substr(0, 200)); continue; }
      same(name, g, w);
      // one-space JSON, as JSON.stringify(x, null, 1) writes it (key order aside: the values arrived as `json`)
      if (endsWith(path, ".answers.json") || path.find(".drafts-") != std::string::npos) sameText(name + " (its layout)", got, OJ::parse(got).dump(1));
      // a DRAFT copy is the deck's own object: its keys in the JS's order
      if (path.find(".drafts-") != std::string::npos) check(name + " (key order)", keysOf(OJ::parse(got)) == keysOf(OJ::parse(text)));
    }
  }
}

// with no `later`, ask-drafts.json is written when the call that changed it returns, as that call left it
void withoutLater(const json& fx) {
  fake::MemFiles mem; fake::Clock clock; mem.clock = &clock;
  const std::string id = "ask_test-a_aaaaaaaa_r1_1110-1200_abcd", gone = "ask_test-a_aaaaaaaa_r9_1110-1200_abcd";
  mem.put("C:\\s\\state\\aaaaaaaa.json", json{ { "session_id", "aaaaaaaa" }, { "rounds", json::array({ { { "id", id }, { "status", "open" }, { "page", "C:\\s\\pages\\" + id + ".html" },
    { "items", json::array({ { { "qid", "Q1" }, { "key", "r1-Q1" }, { "title", "t" } } }) } } }) } }.dump(), 1000);
  mem.put("C:\\s\\pages\\" + id + ".html", "<html/>", 1000);
  AskDeps deps; deps.files = &mem; deps.clock = &clock; deps.stateDir = "C:\\s\\state"; deps.draftsFile = "C:\\s\\ask-drafts.json"; deps.pid = fx["pinned"]["pid"].get<uint32_t>();
  AskFeed feed(deps);
  feed.scan();
  check("asks, no timer: nothing to save, nothing written", !mem.has(deps.draftsFile));
  check("asks, no timer: a draft is saved as setDraft returns", feed.setDraft(id, { { "state", { { "Q1", { { "text", "yes" } } } } }, { "drafted", 1 } }) && mem.getJson(deps.draftsFile)["drafts"].contains(id));
  feed.markSent({ id, gone }, { { "how", "file" } });
  const json f = mem.getJson(deps.draftsFile);
  check("asks, no timer: the file holds what markSent's own scan left (a mark for a page not waiting is gone)", f["sent"].contains(id) && !f["sent"].contains(gone), f["sent"].dump());
  const int writes = mem.atomicWrites;
  feed.scan(); feed.flushDrafts();
  check("asks, no timer: a scan that changes nothing writes nothing", mem.atomicWrites == writes);
  feed.clearDraft(id);
  check("asks, no timer: clearDraft saves", mem.getJson(deps.draftsFile)["drafts"].empty());
  check("asks: downloads() and stateDir() are the deps'", feed.downloads().empty() && feed.stateDir() == "C:\\s\\state");
  // a save pending when the deck stops: flushDrafts writes it; a timer that fires after the feed is gone does nothing
  std::vector<std::function<void()>> timers;
  deps.later = [&](double, std::function<void()> fn) { timers.push_back(std::move(fn)); };
  {
    AskFeed g(deps);
    g.scan();
    g.setDraft(id, { { "state", { { "Q1", { { "text", "later" } } } } }, { "drafted", 1 } });
    check("asks, a timer: not written until it fires", mem.getJson(deps.draftsFile)["drafts"].empty() && !timers.empty());
    g.flushDrafts();
    check("asks, a timer: flushDrafts writes at once", mem.getJson(deps.draftsFile)["drafts"].contains(id));
    const int before = mem.atomicWrites;
    for (auto& fn : timers) fn();
    check("asks, a timer: nothing left to write after a flush", mem.atomicWrites == before);
    timers.clear();
    g.clearDraft(id);
  }
  const int before = mem.atomicWrites;
  for (auto& fn : timers) fn();
  check("asks, a timer: one that outlives its feed does nothing", mem.atomicWrites == before && !timers.empty());
}

}  // namespace

void testAsks(const std::string& fixtures) {
  if (!want("asks")) return;
  const json fx = loadFixture(fixtures, "asks");
  pureFunctions(fx);
  locations(fx);
  draftMax(fx);
  for (const json& sc : fx["scenarios"]) scenario(fx, sc);
  withoutLater(fx);
}
