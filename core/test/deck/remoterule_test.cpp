// core/test/deck/remoterule_test.cpp — the review of 2026-11-18, in the core deck:
//   remoterule   core/deck/remoterule.cpp against lib/remoterule.js (fixtures/deck/remoterule.json): the branch's
//                unit cases and a sweep of a small world
//   driverrule   core/deck/driverrule.cpp against lib/driverrule.js (fixtures/deck/driverrule.json): the checks of
//                the branch's test/driverrule-unit.js one for one (its 171), a sweep, and lib/phone.js deskMeta
//   ruling       the deck on fake seams — an office window (two), a remote desk, a phone: placements, sleep, the
//                wheel; sizes / mirrors / R10 under every driver value with the claim ON and OFF; the old rule
//                (DeckOptions::remoteDeskIsAWindow) beside the remote-desk rule
#include "../../deck/hash.hpp"
#include "rig.hpp"
#include "../../deck/driverrule.hpp"
#include "../../deck/remote.hpp"
#include "../../deck/remoterule.hpp"

namespace {

namespace RR = deck::core::remoterule;
namespace DR = deck::core::driverrule;

std::vector<std::string> strs(const json& a) { std::vector<std::string> v; if (a.is_array()) for (const json& x : a) v.push_back(x.get<std::string>()); return v; }
RR::Entry entryOf(const json& e) {
  RR::Entry r; r.index = e["index"].get<int>(); r.remote = e["remote"].get<bool>(); r.layout = strs(e["layout"]); r.shown = strs(e["shown"]);
  r.hidden = e["hidden"].get<bool>(); r.visible = e["visible"].get<bool>();
  return r;
}
std::vector<RR::Entry> entriesOf(const json& a) { std::vector<RR::Entry> v; for (const json& e : a) v.push_back(entryOf(e)); return v; }
RR::Entry E(int index, bool remote, std::vector<std::string> layout = {}, std::vector<std::string> shown = {}) { RR::Entry r; r.index = index; r.remote = remote; r.layout = std::move(layout); r.shown = std::move(shown); return r; }
json evJson(const std::vector<RR::Eviction>& ev) { json a = json::array(); for (const auto& e : ev) a.push_back({ { "entry", e.entry }, { "ids", e.ids } }); return a; }
RR::Shown shownOf(const json& s) { RR::Shown r; if (!s.is_object()) return r; r.has = true; r.shown = strs(s["shown"]); r.hidden = s["hidden"].get<bool>(); return r; }

// ---------------------------------------------------------------- lib/remoterule.js
void testRemoteRuleFixture(const std::string& fx) {
  const json f = loadFixture(fx, "remoterule");
  check("remoterule: the fixture is of the module this port was made from", f["exports"] == json::array({ "evictions", "holder", "isRemote", "officeShownBy", "officeShows", "own", "shownBy", "wakesForRemote" }), f["exports"].dump());
  const json& u = f["unit"];
  for (const json& sq : u["sequences"]) {
    std::vector<RR::Entry> state = entriesOf(sq["entries"]);
    int n = 0;
    for (const json& st : sq["steps"]) {
      const std::string name = "remoterule: " + str(sq["name"]) + ", take " + std::to_string(++n);
      const size_t taker = st["taker"].get<size_t>();
      state[taker].layout = strs(st["layout"]);
      const auto ev = RR::evictions(taker, state[taker].remote, state[taker].layout, state, sq["oneWindow"].get<bool>());
      check(name + ": who is evicted", evJson(ev) == st["evicted"], evJson(ev).dump());
      for (const auto& e : ev) { auto& l = state[e.entry].layout; l.erase(std::remove_if(l.begin(), l.end(), [&](const std::string& id) { return std::find(e.ids.begin(), e.ids.end(), id) != e.ids.end(); }), l.end()); }
      json layouts = json::array(); for (const auto& e : state) layouts.push_back(e.layout);
      check(name + ": every layout afterwards", layouts == st["layouts"], layouts.dump());
      json holders = json::object(); for (auto it = st["holders"].begin(); it != st["holders"].end(); ++it) holders[it.key()] = RR::holder(it.key(), state);
      check(name + ": who holds what (never the desk)", holders == st["holders"], holders.dump());
    }
  }
  for (const json& c : u["evictions"]) {
    const std::vector<RR::Entry> es = entriesOf(c["entries"]);
    const bool inList = c["taker"].is_number();
    const auto ev = RR::evictions(inList ? c["taker"].get<size_t>() : es.size(), inList ? es[c["taker"].get<size_t>()].remote : c["taker"]["remote"].get<bool>(), strs(c["layout"]), es, c["oneWindow"].get<bool>());
    check("remoterule: " + str(c["name"]), evJson(ev) == c["out"], evJson(ev).dump());
  }
  for (const json& c : u["holders"]) {
    const std::vector<RR::Entry> es = entriesOf(c["entries"]);
    json got = json::array(); for (const json& id : c["ids"]) got.push_back(RR::holder(id.get<std::string>(), es));
    check("remoterule: " + str(c["name"]), got == c["out"], got.dump());
  }
  for (const json& c : u["shown"]) {
    const std::vector<RR::Entry> es = entriesOf(c["entries"]);
    const std::string id = str(c["id"]);
    check("remoterule: shownBy " + id + (c["phone"].get<bool>() ? " with a phone" : "") + " over " + std::to_string(es.size()) + " entries",
      json(RR::shownBy(id, es, c["phone"].get<bool>())) == c["out"] && json(RR::officeShownBy(id, es)) == c["office"] && RR::officeShows(id, es) == c["shows"].get<bool>(), json(RR::shownBy(id, es, c["phone"].get<bool>())).dump());
  }
  for (const json& c : u["wakes"]) {
    const std::vector<std::string> asleep = strs(c["asleep"]);
    const json got = RR::wakesForRemote(shownOf(c["prev"]), shownOf(c["next"]), [&](const std::string& id) { return std::find(asleep.begin(), asleep.end(), id) != asleep.end(); });
    check("remoterule: " + str(c["name"]), got == c["out"], got.dump());
  }
  // ---- the sweep: two office windows (0, 1) and a remote desk (5)
  const json& s = f["sweep"];
  for (const json& c : s["holders"]) {
    const std::vector<RR::Entry> es = { E(0, false, strs(c["l"][0])), E(1, false, strs(c["l"][1])), E(5, true, strs(c["l"][2])) };
    const json got = { RR::holder("a", es), RR::holder("b", es), RR::holder("z", es) };
    check("remoterule sweep: holder " + c["l"].dump(), got == c["out"], got.dump());
  }
  for (const json& c : s["evictions"]) {
    std::vector<RR::Entry> es = { E(0, false, strs(c["l"][0])), E(1, false, strs(c["l"][1])), E(5, true, strs(c["l"][2])) };
    const size_t taker = c["taker"].get<size_t>();
    es[taker].layout = strs(c["take"]);
    const auto ev = RR::evictions(taker, es[taker].remote, es[taker].layout, es, c["oneWindow"].get<bool>());
    check("remoterule sweep: evictions " + c["l"].dump() + " taker " + std::to_string(taker) + " takes " + c["take"].dump() + (c["oneWindow"].get<bool>() ? "" : " (one-window off)"), evJson(ev) == c["out"], evJson(ev).dump());
  }
  for (const json& c : s["shown"]) {
    std::vector<RR::Entry> es = { E(0, false, {}, strs(c["s"][0])), E(1, false, {}, strs(c["s"][1])), E(5, true, {}, strs(c["s"][2])) };
    es[0].visible = c["v"][0].get<bool>(); es[1].visible = c["v"][1].get<bool>(); es[2].hidden = c["h"].get<bool>();
    const std::string id = str(c["id"]);
    check("remoterule sweep: shownBy " + c.dump().substr(0, 120), json(RR::shownBy(id, es, c["phone"].get<bool>())) == c["out"] && json(RR::officeShownBy(id, es)) == c["office"] && RR::officeShows(id, es) == c["shows"].get<bool>());
  }
  for (const json& c : s["wakes"]) {
    const std::vector<std::string> asleep = strs(c["asleep"]);
    const json got = RR::wakesForRemote(shownOf(c["prev"]), shownOf(c["next"]), [&](const std::string& id) { return std::find(asleep.begin(), asleep.end(), id) != asleep.end(); });
    check("remoterule sweep: wakes " + c["prev"].dump() + " -> " + c["next"].dump() + " asleep " + c["asleep"].dump(), got == c["out"], got.dump());
  }
}

// ---------------------------------------------------------------- lib/driverrule.js
DR::Who whoOf(const json& w) { DR::Who r; if (w.is_object()) { r.phone = w.value("phone", false); r.remote = w.value("remote", false); } return r; }
json nullable(const std::string& s) { return s.empty() ? json() : json(s); }
json rowOf(const json& m, const DR::Who& who, const json& pty) {
  json accepts = json::array(); for (const char* k : DR::KINDS) if (DR::acceptsSize(m, who, k)) accepts.push_back(k);
  json locked = json::object(); for (const char* k : DR::KINDS) locked[k] = DR::inputLocked(m, who, k);
  return { { "mode", nullable(DR::mode(m)) }, { "driver", DR::driverOf(m, who) }, { "accepts", accepts }, { "officeMirror", DR::officeMirror(m, who, pty) }, { "remoteMirror", DR::remoteMirror(m, who, pty) },
    { "phoneMirrors", DR::phoneMirrors(m) }, { "locked", locked }, { "reason", nullable(DR::refusedReason(m, who)) } };
}
bool rowSame(const json& got, const json& want) { for (auto it = got.begin(); it != got.end(); ++it) if (want[it.key()] != it.value()) return false; return true; }

void testDriverRuleFixture(const std::string& fx) {
  const json f = loadFixture(fx, "driverrule");
  const json PTY = f["PTY"];
  check("driverrule: the words", f["MODES"] == json(std::vector<std::string>(DR::MODES, DR::MODES + 4)) && f["KINDS"] == json(std::vector<std::string>(DR::KINDS, DR::KINDS + 3)));
  // ---- test/driverrule-unit.js, check for check
  const json& T = f["table"];
  { bool every = T.size() == 16; for (const char* m : DR::MODES) { int n = 0; for (const json& r : T) if (r["m"] == m) n++; if (n != 4) every = false; }
    check("driverrule: the table covers every driver value x who is looking", every); }
  for (const json& r : T) {
    const json& m = r["m"]; const DR::Who who = whoOf(r["who"]);
    const std::string name = "driverrule: " + str(m) + " phone " + (who.phone ? "attached" : "-") + " remote " + (who.remote ? "shows" : "-");
    const std::string driver = DR::driverOf(m, who);
    const json offMir = DR::officeMirror(m, who, PTY), remMir = DR::remoteMirror(m, who, PTY);
    check(name + ": " + str(r["driver"]) + " drives", driver == str(r["driver"]), driver);
    check(name + ": the office " + (r["officeMirror"].is_null() ? "fits its own pane" : "mirrors the pty"), offMir == r["officeMirror"] && (offMir.is_null() || offMir == PTY), offMir.dump());
    check(name + ": the remote desk " + (!r["remoteMirror"].is_null() ? "mirrors the pty" : who.remote ? "fits its own pane" : "is told nothing (it does not show it)"), remMir == r["remoteMirror"] && (remMir.is_null() || remMir == PTY), remMir.dump());
    check(name + ": the phone page " + (r["phoneMirrors"].get<bool>() ? "is a viewer" : "sizes what it opens"), DR::phoneMirrors(m) == r["phoneMirrors"].get<bool>());
    check(name + ": the office " + (r["locked"]["office"].get<bool>() ? "cannot type (R10)" : "types"), DR::inputLocked(m, who, "office") == r["locked"]["office"].get<bool>() && nullable(DR::refusedReason(m, who)) == r["reason"], DR::refusedReason(m, who));
    { json accepts = json::array(); for (const char* k : DR::KINDS) if (DR::acceptsSize(m, who, k)) accepts.push_back(k);
      check(name + ": only " + driver + "'s size reaches the pty", accepts == json::array({ driver }) && accepts == r["accepts"], accepts.dump()); }
    check(name + ": the driver never mirrors", !(driver == "office" && !offMir.is_null()) && !(driver == "remote" && !remMir.is_null()));
    check(name + ": the phone and the remote desk can always type", !DR::inputLocked(m, who, "phone") && !DR::inputLocked(m, who, "remote") && r["locked"]["phone"] == false && r["locked"]["remote"] == false);
  }
  for (const char* m : DR::MODES) for (const bool remote : { false, true }) {
    DR::Who w; w.phone = true; w.remote = remote;
    check(std::string("driverrule: ") + m + ", remote " + (remote ? "shows" : "-") + ": an attached phone is a viewer exactly when it does not drive", DR::phoneMirrors(m) == (DR::driverOf(m, w) != "phone"));
  }
  { DR::Who both; both.phone = true; both.remote = true; DR::Who ph; ph.phone = true; DR::Who rm; rm.remote = true;
    check("driverrule: 'desk' (the stored word before the claim) is 'office'", DR::mode("desk") == "office");
    check("driverrule: the four values pass through", DR::mode("auto") == "auto" && DR::mode("office") == "office" && DR::mode("phone") == "phone" && DR::mode("remote") == "remote");
    check("driverrule: anything else is no value at all (the setting is left alone)", DR::mode(json()).empty() && DR::mode("").empty() && DR::mode("Desk").empty() && DR::mode("away").empty() && DR::mode(7).empty() && DR::mode(json::object()).empty());
    check("driverrule: driverOf reads 'desk' as office, and an unknown value as auto", DR::driverOf("desk", both) == "office" && DR::driverOf("nonsense", ph) == "phone" && DR::driverOf(json(), DR::Who()) == "office");
    check("driverrule: a missing `who` is nobody looking", DR::driverOf("auto", DR::Who()) == "office" && DR::driverOf("remote", DR::Who()) == "office" && DR::remoteMirror("office", DR::Who(), PTY).is_null());
    check("driverrule: a session with no pty has nothing to mirror", DR::officeMirror("phone", DR::Who(), json()).is_null() && DR::remoteMirror("office", rm, json()).is_null()); }
  for (const char* me : DR::KINDS) {
    for (const char* m : DR::MODES) {
      const bool own = std::string(m) == me;
      check(std::string("driverrule: button on ") + me + ", driver " + m + ": one click -> " + (own ? "auto (release)" : std::string(me) + " (this screen drives)"), DR::click(m, me) == (own ? "auto" : me), DR::click(m, me));
      check(std::string("driverrule: button on ") + me + ", driver " + m + ": its look", DR::look(m, me) == (std::string(m) == "auto" ? "auto" : own ? "mine" : "other"), DR::look(m, me));
    }
    bool one = true;
    for (const char* m : DR::MODES) { int mine = 0; for (const char* k : DR::KINDS) if (DR::look(m, k) == "mine") mine++; if (mine != (std::string(m) == "auto" ? 0 : 1)) one = false; }
    check(std::string("driverrule: button on ") + me + ": green on at most one kind of screen at a time", one);
  }
  check("driverrule: the button reads the old word too: on the office, 'desk' is its own claim", DR::look("desk", "office") == "mine" && DR::click("desk", "office") == "auto" && DR::look("desk", "phone") == "other");
  // ---- beyond the table: what the JS answers for every value and every `who`
  for (const json& r : f["sweep"]) { const json got = rowOf(r["m"], whoOf(r["who"]), r["pty"]); check("driverrule sweep: " + r["m"].dump() + " " + r["who"].dump() + " pty " + r["pty"].dump(), rowSame(got, r), got.dump()); }
  for (const json& w : f["words"]) check("driverrule: mode(" + w["in"].dump() + ")", nullable(DR::mode(w["in"])) == w["out"], DR::mode(w["in"]));
  for (const json& b : f["button"]) check("driverrule: the button on " + str(b["me"]) + " under " + b["m"].dump(), DR::click(b["m"], str(b["me"])) == str(b["click"]) && DR::look(b["m"], str(b["me"])) == str(b["look"]));
  // ---- lib/phone.js deskMeta on the branch: the claim's mirror, and the remote-desk rule's inWindow null
  check("driverrule: lib/phone.js was there for the generator (deskMeta)", f["deskMeta"].is_array() && f["deskMeta"].size() >= 40, str(f["phoneNote"]));
  if (f["deskMeta"].is_array()) for (const json& c : f["deskMeta"]) {
    const std::vector<std::string> shown = strs(c["shown"]), phoneHas = strs(c["phoneHas"]);
    DeskRule rule; rule.ownWindowsOnly = true; rule.claim = true;
    const json got = deskMeta(c["meta"], std::set<std::string>(shown.begin(), shown.end()), c["hidden"].get<bool>(), [&](const std::string& id) { return std::find(phoneHas.begin(), phoneHas.end(), id) != phoneHas.end(); }, rule);
    check("deskMeta (the branch): driver " + c["meta"].value("settings", json::object()).value("driver", json()).dump() + " shown " + c["shown"].dump() + (c["hidden"].get<bool>() ? " hidden" : "") + " phone " + c["phoneHas"].dump(), got == c["out"], got.dump().substr(0, 400));
  }
}

// ---------------------------------------------------------------- the deck: an office (two windows), a remote desk, a phone
#define STATE (fake::pc("C:\\state\\deck-state.json"))     // (in the run's path mode: fakes.hpp "THE PATH MODE")
struct World {
  Rig r;
  ClientId w1 = 0, w2 = 0, dk = 0, ph = 0;
  void up(bool twoWindows = true) {
    r.make();
    w1 = r.deck->addWindow(); if (twoWindows) w2 = r.deck->addWindow();
    r.deck->start();
    r.call(w1, "deskInit"); if (twoWindows) r.call(w2, "deskInit");
    r.advance(2100);
  }
  json desk(json opts = json::object()) { dk = r.deck->addRemote(true); return r.call(dk, "deskInit", json::array({ opts })); }
  void phone() { ph = r.deck->addRemote(); }
  void layout(ClientId c, json l) { r.send(c, { { "t", "layout" }, { "layout", l } }); }
  void shown(ClientId c, json ids, bool hidden = false) { r.send(c, { { "t", "shown" }, { "ids", ids }, { "hidden", hidden } }); }
  json deskTile(const std::string& id) { r.advance(2100); return Rig::tileIn(r.lastOf(dk, "meta")["meta"], id); }
};

// ---- a picture pasted on a remote (main.js savePastedImage, the owner's list item 6): saved under the deck's folder, its
// path answered; anything else refused with a reason
void testPasteImage() {
  World x; x.r.opt.persistShells = true; x.up(false);
  Rig& r = x.r;
  const std::string a = r.shell(x.w1);
  x.desk({ { "name", "HOME-PC" }, { "kind", "native" } });
  const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + "pixels";
  const json ok = r.call(x.dk, "pasteImage", json::array({ a, deck::core::base64(png), "image/png" }));
  const std::string file = ok.value("result", json()).is_string() ? ok["result"].get<std::string>() : std::string();
  std::string back; const bool saved = !file.empty() && r.files.read(file, back);
  check("paste: a picture from the remote is saved in the deck's paste folder and its path answered", ok["t"] == "ok" && file.find("paste") != std::string::npos && file.size() > 4 && file.compare(file.size() - 4, 4, ".png") == 0 && saved && back == png, ok.dump());
  check("paste: another type is refused", r.call(x.dk, "pasteImage", json::array({ a, deck::core::base64(png), "text/html" }))["t"] == "err");
  check("paste: an empty picture is refused", r.call(x.dk, "pasteImage", json::array({ a, "", "image/png" }))["t"] == "err");
  check("paste: a session that does not exist is refused", r.call(x.dk, "pasteImage", json::array({ "nope", deck::core::base64(png), "image/png" }))["t"] == "err");
  check("paste: base64 round trip (Node's Buffer rules: url-safe letters, no padding)", deck::core::unbase64(deck::core::base64(png)) == png && deck::core::unbase64("aGk") == "hi" && deck::core::unbase64("_-8") == deck::core::unbase64("/+8"));
}

void testPlacements() {
  // ---- the remote-desk rule (the default): one home per session is a rule of THIS box
  World x; x.r.opt.persistShells = true; x.up();
  Rig& r = x.r;
  const std::string a = r.shell(x.w1), b = r.shell(x.w1), c = r.shell(x.w1);
  x.layout(x.w1, { { a, { 1 } }, { b, { 2 } } });
  x.layout(x.w2, { { c, { 1 } } });
  const json dinit = x.desk({ { "name", "HOME-PC" }, { "kind", "native" } });
  x.phone();
  check("ruling: a desk connects with an empty layout of its own, the next window index", dinit["result"]["layout"] == json::object() && dinit["result"]["windowIndex"] == 2 && r.deck->deskCount() == 1);
  check("ruling: what deskInit hands a desk names no office window as a holder (inWindow null on every tile)", Rig::tileIn(dinit["result"]["meta"], a)["inWindow"].is_null() && Rig::tileIn(dinit["result"]["meta"], c)["inWindow"].is_null() && r.tile(a)["inWindow"] == 0 && r.tile(c)["inWindow"] == 1);
  { const size_t at1 = r.out[x.w1].size(), at2 = r.out[x.w2].size();
    x.layout(x.dk, { { a, { 1, 2 } }, { c, { 3 } } });
    check("ruling: a remote desk placing sessions the office panes hold evicts nothing", r.events(x.w1, "layout:evict", at1).empty() && r.events(x.w2, "layout:evict", at2).empty()
      && r.tilesFile()["windows"][0]["layout"] == json{ { a, { 1 } }, { b, { 2 } } } && r.tilesFile()["windows"][1]["layout"] == json{ { c, { 1 } } }, r.tilesFile()["windows"].dump());
    check("ruling: and the desk has them in its own panes (deskLayout)", r.tilesFile()["deskLayout"] == json{ { a, { 1, 2 } }, { c, { 3 } } });
    check("ruling: the office's mark still names the office window; nothing is 'in' the desk", r.tile(a)["inWindow"] == 0 && r.tile(b)["inWindow"] == 0 && r.tile(c)["inWindow"] == 1); }
  { const size_t at1 = r.out[x.w1].size(), atD = r.out[x.dk].size();
    x.layout(x.w2, { { c, { 1 } }, { a, { 2 } } });
    check("ruling: an office window taking a session evicts it from the other OFFICE window, which is told", r.events(x.w1, "layout:evict", at1).size() == 1 && r.events(x.w1, "layout:evict", at1)[0] == json::array({ a }) && r.tile(a)["inWindow"] == 1);
    check("ruling: ...and takes nothing from the desk, which is told nothing", r.events(x.dk, "layout:evict", atD).empty() && r.tilesFile()["deskLayout"] == json{ { a, { 1, 2 } }, { c, { 3 } } }); }
  { const size_t at1 = r.out[x.w1].size(), at2 = r.out[x.w2].size();
    x.layout(x.dk, { { a, { 1 } }, { b, { 2 } }, { c, { 3 } } });
    check("ruling: a desk can hold every session at once, each also in an office pane", r.events(x.w1, "layout:evict", at1).empty() && r.events(x.w2, "layout:evict", at2).empty() && r.tile(b)["inWindow"] == 0 && r.tile(a)["inWindow"] == 1 && r.tile(c)["inWindow"] == 1
      && r.tilesFile()["deskLayout"].size() == 3); }
  { r.advance(2100);
    const json dm = r.lastOf(x.dk, "meta")["meta"], pm = r.lastOf(x.ph, "init")["meta"];
    bool none = true; for (const json& t : dm["tiles"]) if (!t["inWindow"].is_null()) none = false;
    check("ruling: every meta a desk is sent says inWindow null; a phone's keeps the office's mark", none && dm["tiles"].size() == 3 && Rig::tileIn(pm, c)["inWindow"] == 1 && Rig::tileIn(r.lastOf(x.w1, "meta")["meta"], a)["inWindow"] == 1); }
  { const size_t at1 = r.out[x.w1].size(), at2 = r.out[x.w2].size(), atD = r.out[x.dk].size();
    r.send(x.dk, { { "t", "leaving" }, { "id", a }, { "on", true } });
    check("ruling: a walk on a remote marks no pane here as being left (the message is taken and tells nobody)", r.events(x.w1, "layout:leaving", at1).empty() && r.events(x.w2, "layout:leaving", at2).empty());
    r.send(x.w1, { { "t", "leaving" }, { "id", a }, { "on", true } });
    check("ruling: a walk in an office window marks the OTHER office window, never the desk", r.events(x.w2, "layout:leaving", at2).size() == 1 && r.events(x.w2, "layout:leaving", at2)[0] == json{ { "id", a }, { "on", true } } && r.events(x.dk, "layout:leaving", atD).empty() && r.events(x.w1, "layout:leaving", at1).empty()); }
  { x.shown(x.w1, { b }); x.shown(x.dk, { a, b, c });
    check("ruling: 'on screen' is the deck's own windows: a desk showing a session adds nothing to shownBy", r.tile(b)["shownBy"] == json::array({ "win:0" }) && r.tile(a)["shownBy"].empty() && r.tile(c)["shownBy"].empty(), r.tile(a)["shownBy"].dump());
    r.send(x.ph, { { "t", "attach" }, { "id", c } });
    check("ruling: a phone attached to it still counts (the phone page is unchanged)", r.tile(c)["shownBy"] == json::array({ "remote" }));
    r.send(x.ph, { { "t", "detach" } }); }
  { // a toast's window to flash: the office window that holds it, the primary for one only the desk placed
    const std::string d = r.shell(x.w1);
    x.layout(x.dk, { { a, { 1 } }, { b, { 2 } }, { c, { 3 } }, { d, { 4 } } });
    check("ruling: a session only the desk has placed is held by no window", r.tile(d)["inWindow"].is_null() && r.tilesFile()["deskLayout"].size() == 4);
    r.call(x.w1, "closeTile", { d }, 3000); }
  // the desk goes and comes back: its layout is restored WHOLE, also what the office windows hold
  r.deck->removeClient(x.dk);
  check("ruling: the desk gone: the office is as it was, and its layout waits in the file", r.deck->deskCount() == 0 && r.tile(a)["inWindow"] == 1 && r.tile(b)["inWindow"] == 0 && r.tilesFile()["deskLayout"][a] == json::array({ 1 }) && r.tilesFile()["deskLayout"][c] == json::array({ 3 }), r.tilesFile()["deskLayout"].dump());
  { const size_t at1 = r.out[x.w1].size(), at2 = r.out[x.w2].size();
    const json again = x.desk();
    check("ruling: the next desk gets the same window index and its saved layout whole — the sessions an office window holds too", again["result"]["windowIndex"] == 2 && again["result"]["layout"] == json{ { a, { 1 } }, { b, { 2 } }, { c, { 3 } } }, again["result"]["layout"].dump());
    check("ruling: ...and its coming back took nothing from the office", r.events(x.w1, "layout:evict", at1).empty() && r.events(x.w2, "layout:evict", at2).empty() && r.tile(a)["inWindow"] == 1 && r.tile(b)["inWindow"] == 0); }
  { r.call(x.w1, "saveSettings", json::array({ { { "oneWindow", false } } }));
    const size_t at2 = r.out[x.w2].size();
    x.layout(x.w1, { { a, { 1 } }, { b, { 2 } } });
    check("ruling: one-window off: nothing is evicted anywhere", r.events(x.w2, "layout:evict", at2).empty() && r.res(x.w2, "deskInit")["layout"] == json{ { c, { 1 } }, { a, { 2 } } }); }
  r.deck->shutdown();

  // ---- the old rule (DeckOptions::remoteDeskIsAWindow): a remote desk is a window like any other
  World o; o.r.opt.persistShells = true; o.r.opt.remoteDeskIsAWindow = true; o.up();
  Rig& q = o.r;
  const std::string a2 = q.shell(o.w1), b2 = q.shell(o.w1);
  o.layout(o.w1, { { a2, { 1 } }, { b2, { 2 } } });
  const json di = o.desk();
  { const size_t at1 = q.out[o.w1].size();
    o.layout(o.dk, { { a2, { 1, 2 } } });
    check("old rule: a desk placing a session takes it from the office window, which is told", q.events(o.w1, "layout:evict", at1).size() == 1 && q.events(o.w1, "layout:evict", at1)[0] == json::array({ a2 }) && q.tile(a2)["inWindow"] == 2);
    o.shown(o.dk, { a2 });
    check("old rule: a desk showing a session counts as on screen", q.tile(a2)["shownBy"] == json::array({ "win:2", "remote" }));
    check("old rule: a desk is told which window holds a session", o.deskTile(b2)["inWindow"] == 0 && o.deskTile(a2)["inWindow"] == 2 && Rig::tileIn(di["result"]["meta"], a2)["inWindow"] == 0);
    const size_t atD = q.out[o.dk].size();
    o.layout(o.w1, { { a2, { 1 } }, { b2, { 2 } } });
    check("old rule: an office window takes it back, and the desk is told", q.events(o.dk, "layout:evict", atD).size() == 1 && q.tilesFile()["deskLayout"] == json::object());
    const size_t at1b = q.out[o.w1].size();
    q.send(o.dk, { { "t", "leaving" }, { "id", a2 }, { "on", true } });
    check("old rule: a walk on the desk marks the office pane", q.events(o.w1, "layout:leaving", at1b).size() == 1); }
  q.deck->shutdown();
}

// three Claude sessions with transcripts, idle: what hibernation may put to sleep
struct Sleepers {
  std::string a, b, c; std::map<std::string, std::string> sid;
  void make(Rig& r, ClientId w) {
    a = r.claude(w); b = r.claude(w); c = r.claude(w);
    const json m = r.fresh(); int i = 0;
    for (const std::string& id : { a, b, c }) { sid[id] = str(Rig::tileIn(m, id)["sessionId"]); r.transcript(T_DIR, sid[id], "t" + std::to_string(i++), "x"); }
    records(r);
  }
  // Claude Code's own record of each running session: idle at its prompt (a process is told which tile it is)
  void records(Rig& r) {
    for (auto& kv : r.ptys.live) for (const auto& e : kv.second->spec.env) if (e.first == "CLAUDE_DECK_TILE" && sid.count(e.second)) r.record(T_DIR, kv.second->pid(), sid[e.second], "idle");
  }
};

void testSleep() {
  // ---- the remote-desk rule: a remote showing or placing a session neither keeps it awake nor wakes it on the tick
  World x; x.r.opt.allowClaude = true; x.up(false);
  Rig& r = x.r;
  Sleepers s; s.make(r, x.w1);
  x.desk();
  x.layout(x.dk, { { s.a, { 1 } }, { s.b, { 2 } } });
  x.shown(x.dk, { s.a });                         // the desk shows a; the office window shows c; nobody shows b
  x.shown(x.w1, { s.c });
  r.advance(12000);
  check("ruling (sleep): a session only a remote desk shows is not 'on screen'; one an office window shows is", r.tile(s.a)["sleepBlocker"].get<std::string>().compare(0, 7, "active ") == 0 && r.tile(s.c)["sleepBlocker"] == "on screen (win:0)", r.tile(s.a)["sleepBlocker"].dump());
  r.advance(31 * 60000.0);
  check("ruling (sleep): its idle time up, the session a remote desk shows sleeps like one nobody shows; the office's stays up", r.tile(s.a)["status"] == "asleep" && r.tile(s.b)["status"] == "asleep" && r.tile(s.c)["asleep"] == false, r.tile(s.a)["status"].dump());
  r.advance(90000);
  check("ruling (sleep): a sleeping session a remote desk shows is not woken by the tick", r.tile(s.a)["asleep"] == true);
  x.shown(x.dk, { s.a, s.b });
  check("ruling (sleep): THE exception: a sleeping session ARRIVING on a remote's screen is woken", r.tile(s.b)["asleep"] == false);
  check("ruling (sleep): ...one already in its panes is not woken because another pane changed", r.tile(s.a)["asleep"] == true);
  x.shown(x.dk, { s.a, s.b }, true);
  check("ruling (sleep): a hidden tab wakes nothing", r.tile(s.a)["asleep"] == true);
  x.shown(x.dk, { s.a, s.b }, false);
  check("ruling (sleep): the tab coming back wakes what it shows", r.tile(s.a)["asleep"] == false);
  // awake again, the desk still showing it: nothing about the desk stands between it and its next sleep
  check("ruling (sleep): once awake, the desk's showing it is still not a reason to stay up", r.tile(s.a)["shownBy"].empty() && r.tile(s.b)["shownBy"].empty()
    && r.tile(s.a)["sleepBlocker"].get<std::string>().find("on screen") == std::string::npos, r.tile(s.a)["sleepBlocker"].dump());
  r.deck->shutdown();

  // ---- typing from the desk, and a phone
  World y; y.r.opt.allowClaude = true; y.up(false);
  Rig& k = y.r;
  Sleepers u; u.make(k, y.w1);
  y.desk(); y.shown(y.dk, { u.a, u.b });
  k.advance(12000 + 31 * 60000.0);
  check("ruling (sleep): everything a desk shows and the office does not is asleep", k.tile(u.a)["asleep"] == true && k.tile(u.b)["asleep"] == true && k.tile(u.c)["asleep"] == true);
  k.send(y.dk, { { "t", "input" }, { "id", u.a }, { "d", "x" } });
  check("ruling (sleep): typing into it from the desk wakes it (the keystroke itself is dropped, as ever)", k.tile(u.a)["asleep"] == false && k.tile(u.b)["asleep"] == true);
  y.phone();
  k.send(y.ph, { { "t", "attach" }, { "id", u.b } });
  k.advance(31000);
  check("ruling (sleep): a PHONE attached to a sleeping session still wakes it on the tick (unchanged)", k.tile(u.b)["asleep"] == false && k.tile(u.b)["shownBy"] == json::array({ "remote" }) && k.tile(u.c)["asleep"] == true);
  // a desk coming back with sleeping sessions in its saved layout wakes nothing until it says what it shows
  y.layout(y.dk, { { u.c, { 1 } } });
  k.deck->removeClient(y.dk);
  const json again = y.desk();
  check("ruling (sleep): a desk connecting with a sleeping session in its saved layout wakes nothing by connecting", again["result"]["layout"] == json{ { u.c, { 1 } } } && k.tile(u.c)["asleep"] == true);
  y.shown(y.dk, { u.c });
  check("ruling (sleep): ...its first 'what I show' does", k.tile(u.c)["asleep"] == false);
  k.deck->shutdown();

  // ---- the old rule: a desk's panes are on screen
  World o; o.r.opt.allowClaude = true; o.r.opt.remoteDeskIsAWindow = true; o.up(false);
  Rig& q = o.r;
  Sleepers t; t.make(q, o.w1);
  o.desk();
  o.shown(o.dk, { t.a });
  q.advance(12000);
  check("old rule (sleep): a session a remote desk shows is on screen", q.tile(t.a)["sleepBlocker"] == "on screen (win:1, remote)", q.tile(t.a)["sleepBlocker"].dump());
  q.advance(31 * 60000.0);
  check("old rule (sleep): ...and stays awake for that reason alone, while the others sleep", q.tile(t.a)["asleep"] == false && q.tile(t.b)["status"] == "asleep" && q.tile(t.c)["status"] == "asleep");
  o.shown(o.dk, { t.a, t.b });
  check("old rule (sleep): a sleeping session arriving on the desk's screen is woken", q.tile(t.b)["asleep"] == false);
  q.deck->shutdown();
}

// Sizes, mirrors and R10 under one driver value. Four shells: t1 the desk shows, t2 a phone has, t3 nobody remote
// looks at, t4 both. Each kind of screen sends a size of its own to each tile, one at a time.
struct SizeWorld : World {
  std::string t[4]; fake::Pty* p[4] = { nullptr, nullptr, nullptr, nullptr }; ClientId ph2 = 0;
  int n = 0;
  void build(bool claim, bool oldWindowRule = false) {
    r.opt.driverClaim = claim; r.opt.remoteDeskIsAWindow = oldWindowRule;
    up(false);
    for (int i = 0; i < 4; i++) t[i] = r.shell(w1);
    { int i = 0; for (auto& kv : r.ptys.live) p[i++] = kv.second; }
    desk({ { "name", "HOME-PC" }, { "kind", "native" } });
    phone(); ph2 = r.deck->addRemote();
    shown(w1, { t[0], t[1], t[2], t[3] });
    shown(dk, { t[0], t[3] });
    r.send(ph, { { "t", "attach" }, { "id", t[1] } });
    r.send(ph2, { { "t", "attach" }, { "id", t[3] } });
  }
  // does a size from that client reach the pty
  bool takes(ClientId c, const std::string& id, bool byCall = false) {
    const int cols = 60 + (++n), rows = 20 + (n % 7);
    if (byCall) r.call(c, "resize", { id, cols, rows }); else r.send(c, { { "t", "resize" }, { "id", id }, { "cols", cols }, { "rows", rows } });
    r.advance(200);
    return r.deck->ptySize(id) == std::make_pair(cols, rows);
  }
  bool nudges(ClientId c, const std::string& id) {
    const int cols = 60 + (++n), rows = 20 + (n % 7);
    r.call(c, "nudge", { id, cols, rows }); r.advance(200);
    return r.deck->ptySize(id) == std::make_pair(cols, rows);
  }
  // does the office window's typing reach the pty; `reason`: what it was told when not
  bool officeTypes(int i, std::string* reason) {
    r.advance(3100);                               // past the 3 s in which a second refusal is not repeated
    const size_t at = r.out[w1].size(), writes = p[i]->writes.size();
    r.send(w1, { { "t", "input" }, { "id", t[i] }, { "d", "k" } });
    const auto ev = r.events(w1, "input:refused", at);
    if (reason) *reason = ev.empty() ? std::string() : str(ev[0]["reason"]);
    return p[i]->writes.size() == writes + 1;
  }
  bool remoteTypes(ClientId c, int i) { const size_t writes = p[i]->writes.size(); r.send(c, { { "t", "input" }, { "id", t[i] }, { "d", "r" } }); return p[i]->writes.size() == writes + 1; }
};

void testSizesClaimOn() {
  // who looks at each tile: t1 the desk, t2 a phone, t3 nobody, t4 both
  const DR::Who who[4] = { { false, true }, { true, false }, { false, false }, { true, true } };
  const char* const what[4] = { "the desk shows it", "a phone has it", "nobody remote looks", "a phone and the desk" };
  for (const char* mode : DR::MODES) {
    SizeWorld x; x.build(true);
    Rig& r = x.r;
    const json set = r.res(x.w1, "setDriver", { mode });
    check(std::string("claim on, ") + mode + ": setDriver answers the value in force; the meta carries that word", set == mode && r.fresh()["settings"]["driver"] == mode, set.dump());
    check(std::string("claim on, ") + mode + ": meta.remoteDesk names the connected desk", r.fresh()["remoteDesk"] == json{ { "name", "HOME-PC" }, { "kind", "native" } }, r.fresh().value("remoteDesk", json()).dump());
    for (int i = 0; i < 4; i++) {
      const std::string driver = DR::driverOf(mode, who[i]);
      const std::string name = std::string("claim on, ") + mode + ", " + what[i] + ": ";
      const bool office = x.takes(x.w1, x.t[i]), phone = x.takes(x.ph, x.t[i]), remote = x.takes(x.dk, x.t[i]);
      check(name + "only " + driver + "'s size reaches the pty", office == (driver == "office") && phone == (driver == "phone") && remote == (driver == "remote"), std::string(office ? "office " : "") + (phone ? "phone " : "") + (remote ? "remote" : ""));
      const bool nOffice = x.nudges(x.w1, x.t[i]), nPhone = x.nudges(x.ph, x.t[i]), nRemote = x.nudges(x.dk, x.t[i]);
      check(name + "and only its redraw nudges it", nOffice == (driver == "office") && nPhone == (driver == "phone") && nRemote == (driver == "remote"));
      const auto size = r.deck->ptySize(x.t[i]);
      const json pty = { { "cols", size.first }, { "rows", size.second } };
      const json tile = r.tile(x.t[i]);
      check(name + "sizeBy says " + driver, tile["sizeBy"] == driver, tile.value("sizeBy", json()).dump());
      check(name + "the office " + (driver == "office" ? "fits its own pane (mirror null)" : "mirrors the pty's own size"), tile["mirror"] == (driver == "office" ? json() : pty), tile["mirror"].dump());
      const json dt = x.deskTile(x.t[i]);
      check(name + "the desk " + (!who[i].remote ? "is told nothing (it does not show it)" : driver == "remote" ? "fits its own pane (mirror null)" : "mirrors the pty's own size"),
        dt["mirror"] == ((who[i].remote && driver != "remote") ? pty : json()) && dt["sizeBy"] == driver && dt["inWindow"].is_null(), dt["mirror"].dump());
      std::string reason;
      const bool types = x.officeTypes(i, &reason);
      check(name + "the office " + (driver == "office" ? "types" : "cannot type (R10), and is told why"), types == (driver == "office") && reason == DR::refusedReason(mode, who[i]), reason);
      check(name + "the phone and the remote desk can always type", x.remoteTypes(x.ph, i) && x.remoteTypes(x.dk, i));
    }
    r.deck->shutdown();
  }

  // ---- becoming the driver: the pty is still at the last driver's size, and the new driver's size is taken
  { SizeWorld x; x.build(true);
    Rig& r = x.r;
    r.call(x.w1, "setDriver", { "office" });
    r.send(x.w1, { { "t", "resize" }, { "id", x.t[0] }, { "cols", 150 }, { "rows", 50 } }); r.advance(200);
    check("claim on: under office the remote desk mirrors what the office sized", x.deskTile(x.t[0])["mirror"] == json{ { "cols", 150 }, { "rows", 50 } } && r.tile(x.t[0])["mirror"].is_null());
    r.call(x.dk, "setDriver", { "remote" });
    { const json dt = x.deskTile(x.t[0]), ot = r.tile(x.t[0]);
      check("claim on: the desk claims the size: its mirror goes from set to null (its cue to say its size), the pty still at the office's", dt["mirror"].is_null() && dt["sizeBy"] == "remote" && dt["ptySize"] == json{ { "cols", 150 }, { "rows", 50 } } && r.deck->ptySize(x.t[0]) == std::make_pair(150, 50));
      check("claim on: ...and the office is told to render at the pty's own size, not a size remembered from anybody", ot["mirror"] == json{ { "cols", 150 }, { "rows", 50 } } && ot["sizeBy"] == "remote"); }
    r.send(x.dk, { { "t", "resize" }, { "id", x.t[0] }, { "cols", 100 }, { "rows", 40 } });
    check("claim on: the new driver's size is on its way: the office's mirror is the size the pty is about to be (mid-debounce)", r.tile(x.t[0])["mirror"] == json{ { "cols", 100 }, { "rows", 40 } } && r.deck->ptySize(x.t[0]) == std::make_pair(150, 50));
    r.advance(200);
    check("claim on: ...and then the pty's", r.deck->ptySize(x.t[0]) == std::make_pair(100, 40) && r.tile(x.t[0])["mirror"] == json{ { "cols", 100 }, { "rows", 40 } });
    check("claim on: under remote a session the desk does not show stays the office's", r.tile(x.t[2])["sizeBy"] == "office" && r.tile(x.t[2])["mirror"].is_null() && x.takes(x.w1, x.t[2]));
    x.shown(x.dk, { x.t[0], x.t[3] }, true);
    check("claim on: the desk's tab hidden: it shows nothing and drives nothing; the office has its sessions back", r.tile(x.t[0])["sizeBy"] == "office" && r.tile(x.t[0])["mirror"].is_null() && x.takes(x.w1, x.t[0]) && !x.takes(x.dk, x.t[0]));
    x.shown(x.dk, { x.t[0], x.t[3] }, false);
    r.deck->removeClient(x.dk);
    check("claim on: the desk gone: remoteDesk null (the key stays), every session the office's under remote", r.fresh().contains("remoteDesk") && r.fresh()["remoteDesk"].is_null() && r.tile(x.t[0])["sizeBy"] == "office" && r.tile(x.t[3])["sizeBy"] == "office");
    r.deck->shutdown(); }

  // ---- the words in and out
  { SizeWorld x; x.build(true);
    Rig& r = x.r;
    check("claim on: 'desk' from an older client is taken as 'office'", r.res(x.ph, "setDriver", { "desk" }) == "office" && r.files.getJson(P("C:\\state\\settings.json"))["driver"] == "office");
    check("claim on: anything else changes nothing", r.res(x.w1, "setDriver", { "Desk" }) == "office" && r.res(x.w1, "setDriver", { 7 }) == "office" && r.res(x.w1, "setDriver", json::array({ nullptr })) == "office");
    check("claim on: saveSettings takes the same words", r.res(x.w1, "saveSettings", json::array({ { { "driver", "remote" } } }))["driver"] == "remote" && r.res(x.dk, "saveSettings", json::array({ { { "driver", "desk" } } }))["driver"] == "office"
      && r.res(x.w1, "saveSettings", json::array({ { { "driver", "nope" }, { "fontSize", 15 } } }))["driver"] == "office");
    r.call(x.dk, "userInput", { x.t[0] }); r.call(x.ph, "userInput", { x.t[1] }); r.call(x.w1, "userInput", { x.t[2] });
    check("claim on: who typed last: a remote desk marks 'remote', a phone 'phone', the office 'desktop'", r.tile(x.t[0])["driver"] == "remote" && r.tile(x.t[1])["driver"] == "phone" && r.tile(x.t[2])["driver"] == "desktop");
    r.deck->shutdown(); }
  { Rig r; r.opt.driverClaim = true; r.files.put(P("C:\\state\\settings.json"), json{ { "driver", "desk" }, { "fontSize", 13 }, { "phone", { { "token", "0123456789abcdef" } } } }.dump());
    const ClientId w = r.boot();
    check("claim on: a stored 'desk' is 'office' in memory at load; the file takes the new word at the next save", r.fresh()["settings"]["driver"] == "office" && r.files.getJson(P("C:\\state\\settings.json"))["driver"] == "desk"
      && (r.call(w, "saveSettings", json::array({ { { "fontSize", 14 } } })), r.files.getJson(P("C:\\state\\settings.json"))["driver"] == "office"));
    r.deck->shutdown(); }
  { Rig r; r.opt.driverClaim = true; r.files.put(P("C:\\state\\settings.json"), json{ { "driver", "nonsense" } }.dump());
    r.boot();
    check("claim on: a stored value that is no value is the default", r.fresh()["settings"]["driver"] == "auto");
    r.deck->shutdown(); }
}

void testSizesClaimOff() {
  // this tree's rule, word for word: the office is refused while a remote drives (driver 'phone', or 'auto' and a
  // phone OR a desk has the tile: a remote desk counts as a phone); a remote's size is refused under 'desk'
  const bool watching[4] = { true, true, false, true };
  const char* const what[4] = { "the desk shows it", "a phone has it", "nobody remote looks", "a phone and the desk" };
  for (const bool oldWindowRule : { false, true }) for (const char* mode : { "auto", "phone", "desk" }) {
    SizeWorld x; x.build(false, oldWindowRule);
    Rig& r = x.r;
    const std::string tag = std::string("claim off") + (oldWindowRule ? " (old window rule), " : ", ") + mode;
    const json set = r.res(x.w1, "setDriver", { mode });
    const json meta = r.fresh();
    bool noSizeBy = true; for (const json& t : meta["tiles"]) if (t.contains("sizeBy")) noSizeBy = false;
    check(tag + ": this tree's word, and neither sizeBy nor remoteDesk in the meta (either would say 'this deck has the claim')", set == mode && meta["settings"]["driver"] == mode && noSizeBy && !meta.contains("remoteDesk"));
    { r.advance(2100); const json dm = r.lastOf(x.dk, "meta")["meta"]; bool none = true; for (const json& t : dm["tiles"]) if (t.contains("sizeBy")) none = false;
      check(tag + ": nor in a desk's or a phone's", none && !dm.contains("remoteDesk") && !r.lastOf(x.ph, "init")["meta"].contains("remoteDesk")); }
    for (int i = 0; i < 4; i++) {
      const bool remoteDrives = std::string(mode) == "phone" || (std::string(mode) == "auto" && watching[i]);
      const bool remoteSized = std::string(mode) != "desk";
      const std::string name = tag + ", " + what[i] + ": ";
      const bool office = x.takes(x.w1, x.t[i]), phone = x.takes(x.ph, x.t[i]), remote = x.takes(x.dk, x.t[i], true);
      check(name + "the office's size " + (remoteDrives ? "is refused" : "is taken") + "; a phone's and a desk's " + (remoteSized ? "are taken" : "are refused"), office == !remoteDrives && phone == remoteSized && remote == remoteSized);
      check(name + "the nudges follow", x.nudges(x.w1, x.t[i]) == !remoteDrives && x.nudges(x.ph, x.t[i]) == remoteSized && x.nudges(x.dk, x.t[i]) == remoteSized);
      std::string reason;
      const bool types = x.officeTypes(i, &reason);
      check(name + "the office " + (remoteDrives ? "cannot type (R10)" : "types"), types == !remoteDrives && reason == (remoteDrives ? (std::string(mode) == "phone" ? "phone" : "phone-attached") : ""), reason);
      check(name + "a phone and a desk can always type", x.remoteTypes(x.ph, i) && x.remoteTypes(x.dk, i));
      // the office mirrors the size a remote last sent while a remote has the tile; a desk its own rule
      const json tile = r.tile(x.t[i]);
      check(name + "the office's mirror is " + (remoteDrives ? "the size a remote last showed it at" : "null"), tile["mirror"].is_null() == !remoteDrives, tile["mirror"].dump());
      const json dt = x.deskTile(x.t[i]);
      const bool shows = i == 0 || i == 3, deskDrives = shows && std::string(mode) != "desk" && i != 3;
      check(name + "the desk's mirror: " + (deskDrives ? "null (it drives)" : shows ? "the pty's size" : "null (it does not show it)"), dt["mirror"].is_null() == (deskDrives || !shows), dt["mirror"].dump());
      check(name + "a desk is told " + (oldWindowRule ? "which window holds it, as before" : "inWindow null (the ruling does not wait for the claim)"), dt.contains("inWindow") && (oldWindowRule || dt["inWindow"].is_null()));
    }
    r.deck->shutdown();
  }
  { SizeWorld x; x.build(false);
    Rig& r = x.r;
    check("claim off: 'office' from a client that speaks the new words is taken as this tree's 'desk'", r.res(x.w1, "setDriver", { "office" }) == "desk" && r.files.getJson(P("C:\\state\\settings.json"))["driver"] == "desk");
    check("claim off: 'remote' is no value here and changes nothing", r.res(x.w1, "setDriver", { "remote" }) == "desk" && r.res(x.w1, "saveSettings", json::array({ { { "driver", "remote" } } }))["driver"] == "desk");
    check("claim off: the three words as ever", r.res(x.ph, "setDriver", { "phone" }) == "phone" && r.res(x.dk, "setDriver", { "auto" }) == "auto" && r.res(x.w1, "setDriver", { "desk" }) == "desk");
    r.call(x.dk, "userInput", { x.t[0] });
    check("claim off: a remote desk's typing marks the tile 'phone' (this tree's word)", r.tile(x.t[0])["driver"] == "phone");
    r.deck->shutdown(); }
  for (const auto& kv : std::vector<std::pair<const char*, const char*>>{ { "office", "desk" }, { "remote", "auto" }, { "desk", "desk" }, { "phone", "phone" }, { "junk", "auto" } }) {
    Rig r; r.files.put(P("C:\\state\\settings.json"), json{ { "driver", kv.first } }.dump());
    r.boot();
    check(std::string("claim off: a settings file saying '") + kv.first + "' (a deck with the claim wrote it) is read as '" + kv.second + "'", r.fresh()["settings"]["driver"] == kv.second, r.fresh()["settings"]["driver"].dump());
    r.deck->shutdown();
  }
}

// The wheel from a remote desk (the branch's narrowing of R8): the server has no filter — the client decides — and
// the core treats the report as lib/presence.js and main.js noteKeys do.
void testWheel() {
  for (const bool claim : { false, true }) {
    World x; x.r.opt.driverClaim = claim; x.up(false);
    Rig& r = x.r;
    const std::string tag = claim ? "wheel (claim on): " : "wheel: ";
    const std::string id = r.shell(x.w1);
    fake::Pty* p = r.ptys.last();
    x.desk(); x.phone();
    x.shown(x.dk, { id });
    r.send(x.ph, { { "t", "input" }, { "id", id }, { "d", "p" } });          // the user was last on the phone
    const std::string wheel = "\x1b[<64;10;5M";
    r.send(x.dk, { { "t", "input" }, { "id", id }, { "d", wheel }, { "kind", "wheel" } });
    check(tag + "a desk's wheel report reaches the session untouched (no server-side filter)", p->writes.back() == wheel);
    check(tag + "it counts as one wheel notch for where the user is: remote", r.files.getJson(STATE)["where"] == "remote" && r.files.getJson(STATE)["lastInputFrom"] == "remote");
    check(tag + "it is not typing: the tile is still fresh and nobody 'typed last'", r.tile(id)["fresh"] == true && r.tile(id)["driver"].is_null() && r.tile(id)["driverAt"].is_null());
    r.send(x.ph, { { "t", "input" }, { "id", id }, { "d", "p" } });
    r.send(x.dk, { { "t", "input" }, { "id", id }, { "d", "\x1b[<65;10;5M\x1b[<65;10;5M" }, { "kind", "wheel" } });
    check(tag + "two reports in one message are not one notch (the page sends exactly one): written, not counted", p->writes.back() == "\x1b[<65;10;5M\x1b[<65;10;5M" && r.files.getJson(STATE)["where"] == "phone");
    r.send(x.dk, { { "t", "input" }, { "id", id }, { "d", "\x1b[<35;10;5M" } });
    check(tag + "a motion report (were one ever sent) is no input at all", r.files.getJson(STATE)["where"] == "phone");
    r.send(x.dk, { { "t", "input" }, { "id", id }, { "d", "\x1b[<80;10;5M" }, { "kind", "wheel" } });
    check(tag + "the wheel with a modifier (Ctrl: 64 + 16) counts", r.files.getJson(STATE)["where"] == "remote");
    r.send(x.ph, { { "t", "input" }, { "id", id }, { "d", wheel } });
    check(tag + "the same report from a PHONE is its page scrolling by itself: written, not counted", p->writes.back() == wheel && r.files.getJson(STATE)["where"] == "remote");
    r.send(x.ph, { { "t", "input" }, { "id", id }, { "d", "p" } });
    // the office under R10: a wheel into a session a remote drives is a terminal reply — dropped silently, no notice
    r.call(x.w1, "setDriver", { "phone" });
    r.advance(3100);
    { const size_t at = r.out[x.w1].size(), writes = p->writes.size();
      r.send(x.w1, { { "t", "input" }, { "id", id }, { "d", wheel } });
      check(tag + "the office's wheel into a session it may not type into is dropped without a notice", p->writes.size() == writes && r.events(x.w1, "input:refused", at).empty() && r.files.getJson(STATE)["where"] == "phone"); }
    r.call(x.w1, "setDriver", { claim ? "office" : "desk" });
    r.send(x.w1, { { "t", "input" }, { "id", id }, { "d", wheel } });
    check(tag + "the office's wheel into its own session goes in and says the user is at the office", p->writes.back() == wheel && r.files.getJson(STATE)["where"] == "office");
    r.deck->shutdown();
  }
}

template <class F> void scenario(const char* name, F f) {
  try { f(); } catch (const std::exception& e) { const std::string after = lastCheck; check(std::string(name) + ": ran to its end", false, std::string(e.what()) + " — after: " + after); }
}

}  // namespace

void testRemoteRules(const std::string& fx) {
  if (want("remoterule")) scenario("remoterule fixture", [&] { testRemoteRuleFixture(fx); });
  if (want("driverrule")) scenario("driverrule fixture", [&] { testDriverRuleFixture(fx); });
  if (want("ruling")) {
    scenario("placements", testPlacements); scenario("sleep", testSleep);
    scenario("sizes, claim on", testSizesClaimOn); scenario("sizes, claim off", testSizesClaimOff); scenario("wheel", testWheel); scenario("paste a picture", testPasteImage);
  }
}
