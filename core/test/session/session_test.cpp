// core/test/session/session_test.cpp — core/session against the fixtures the JavaScript generated
// (core/test/session/gen-fixtures.js -> core/test/fixtures/session/*.json).
//   powershell -File core/test/session/build.ps1
#include "../../session/jsutil.hpp"
#include "../../session/livestate.hpp"
#include "../../session/meta.hpp"
#include "../../session/replay.hpp"
#include "../../session/transcript.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace deck::session;
using json = nlohmann::json;

static int passed = 0, failed = 0;
static void check(const std::string& name, bool ok, const std::string& got = "") {
  if (ok) { ++passed; return; }
  ++failed;
  std::printf("FAIL  %s%s%s\n", name.c_str(), got.empty() ? "" : "\n      ", got.c_str());
}
static json load(const std::string& dir, const char* name) {
  std::ifstream f(dir + "/" + name + ".json", std::ios::binary);
  if (!f) { std::printf("cannot open fixture %s/%s.json\n", dir.c_str(), name); std::exit(2); }
  std::stringstream ss; ss << f.rdbuf();
  return json::parse(ss.str());
}
static std::string unhex(const std::string& h) {
  std::string out;
  auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  for (size_t i = 0; i + 1 < h.size(); i += 2) out += (char)(v(h[i]) * 16 + v(h[i + 1]));
  return out;
}
static std::string tohex(const std::string& s) {
  static const char d[] = "0123456789abcdef"; std::string o;
  for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
  return o;
}
static std::string unb64(const std::string& s) {
  std::string out; int val = 0, bits = -8;
  for (unsigned char c : s) {
    int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (d < 0) continue;
    val = (val << 6) | d; bits += 6;
    if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
  }
  return out;
}
static std::string fnv(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
  char b[20]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
  return b;
}
static bool digestIs(const std::string& s, const json& d, std::string* why) {
  if (d.is_null()) { *why = "expected null"; return false; }
  if (d["utf8Bytes"].get<size_t>() != s.size()) { *why = "length " + std::to_string(s.size()) + " != " + std::to_string(d["utf8Bytes"].get<size_t>()); return false; }
  if (d["fnv"].get<std::string>() != fnv(s)) { *why = "content differs" + (s.size() < 200 ? " got " + tohex(s) : std::string()); return false; }
  return true;
}
// the first place two JSON values differ, for a readable failure
static std::string firstDiff(const json& a, const json& b, const std::string& path = "") {
  if (a == b) return "";
  if (a.is_object() && b.is_object()) {
    for (auto it = a.begin(); it != a.end(); ++it) {
      if (!b.contains(it.key())) return path + "." + it.key() + ": extra in C++ = " + it.value().dump();
      std::string d = firstDiff(it.value(), b[it.key()], path + "." + it.key()); if (!d.empty()) return d;
    }
    for (auto it = b.begin(); it != b.end(); ++it) if (!a.contains(it.key())) return path + "." + it.key() + ": missing in C++, JS = " + it.value().dump();
  }
  if (a.is_array() && b.is_array()) {
    if (a.size() != b.size()) return path + ": array size " + std::to_string(a.size()) + " != " + std::to_string(b.size());
    for (size_t i = 0; i < a.size(); i++) { std::string d = firstDiff(a[i], b[i], path + "[" + std::to_string(i) + "]"); if (!d.empty()) return d; }
  }
  return path + ": C++ " + a.dump().substr(0, 200) + "  JS " + b.dump().substr(0, 200);
}

// ---------------------------------------------------------------- replay
static void testReplay(const std::string& dir) {
  const json fx = load(dir, "replay");
  check("replay: the limit is main.js REPLAY_LIMIT", fx["limit"].get<size_t>() == kReplayLimit);
  for (const json& c : fx["cases"]) {
    Replay r; Modes m;
    int stepNo = 0;
    for (const json& st : c["steps"]) {
      const std::string op = st["op"];
      if (op == "push" || op == "pushUncounted") {
        const std::string unit = unhex(st["dHex"]); std::string d;
        const int times = st["times"]; d.reserve(unit.size() * times);
        for (int i = 0; i < times; i++) d += unit;
        if (op == "push") { r.push(d); m.track(d); } else r.pushUncounted(d);
      } else if (op == "resize") r.markResize();
      else if (op == "reset") { r.reset(); m.reset(); }
      const json& a = st["after"];
      const std::string name = "replay: " + c["name"].get<std::string>() + " step " + std::to_string(++stepNo) + " (" + op + ")";
      check(name + " counters", r.chunks() == a["chunks"].get<size_t>() && r.bytes() == a["bytes"].get<size_t>() && r.total() == a["total"].get<size_t>() && r.dropped() == a["dropped"].get<size_t>() && r.cut() == a["cut"].get<size_t>(),
        "chunks " + std::to_string(r.chunks()) + " bytes " + std::to_string(r.bytes()) + " total " + std::to_string(r.total()) + " dropped " + std::to_string(r.dropped()) + " cut " + std::to_string(r.cut()) + "  want " + a.dump().substr(0, 120));
      check(name + " mode prefix", tohex(m.prefix()) == a["prefixHex"].get<std::string>(), tohex(m.prefix()));
      json modes = json::object();
      for (int k : { 1049, 1000, 1002, 1003, 1006, 2004 }) if (m.on(k)) modes[std::to_string(k)] = true;
      check(name + " modes", modes == a["modes"], modes.dump());
      std::string why;
      check(name + " all", digestIs(r.all(), a["all"], &why), why);
      check(name + " since resize", digestIs(r.sinceResize(), a["since"], &why), why);
      bool any = false; std::string full = replayFor(r, m, false, &any);
      check(name + " replayFor", a["replayFor"].is_null() ? !any : (any && digestIs(full, a["replayFor"], &why)), why);
      std::string since = replayFor(r, m, true, &any);
      check(name + " replayFor since", a["replayForSince"].is_null() ? !any : (any && digestIs(since, a["replayForSince"], &why)), why);
    }
  }
  const std::string mixed = unhex(fx["sliceOf"]["hex"]);
  for (const json& s : fx["sliceOf"]["slices"]) {
    const std::string got = sliceFromUtf16(mixed, s["skip"].get<size_t>());
    check("replay: slice at unit " + std::to_string(s["skip"].get<int>()), tohex(got) == s["hex"].get<std::string>(), tohex(got) + " want " + s["hex"].get<std::string>());
  }
  check("replay: utf16Length", utf16Length(mixed) == 6 && utf16Length("") == 0 && utf16Length("abc") == 3);
  for (const json& d : fx["decode"]) {
    Utf8Decoder dec; bool ok = true; std::string got;
    for (size_t i = 0; i < d["partsHex"].size(); i++) {
      const std::string in = unhex(d["partsHex"][i]);
      const std::string out = dec.write(in.data(), in.size());
      if (tohex(out) != d["outHex"][i].get<std::string>()) { ok = false; got += " part " + std::to_string(i) + ": " + tohex(out) + " want " + d["outHex"][i].get<std::string>(); }
    }
    const std::string end = dec.end();
    if (tohex(end) != d["endHex"].get<std::string>()) { ok = false; got += " end: " + tohex(end) + " want " + d["endHex"].get<std::string>(); }
    check("replay: StringDecoder " + d["name"].get<std::string>(), ok, got);
  }
}

// ---------------------------------------------------------------- transcript
static json optS(const std::optional<std::string>& s) { return s ? json(*s) : json(); }
static json optN(const std::optional<double>& d) { return d ? json(*d) : json(); }
static int localOffsetMin(double epochMs) {       // JavaScript getTimezoneOffset(): UTC minus local, in minutes
  const std::time_t t = (std::time_t)(epochMs / 1000);
  std::tm g = *std::gmtime(&t); g.tm_isdst = -1;
  std::tm l = *std::localtime(&t);
  const std::time_t asIfLocal = std::mktime(&g);
  (void)l;
  return (int)((asIfLocal - t) / 60);
}
static void testTranscript(const std::string& dir) {
  const json fx = load(dir, "transcript");
  for (const json& c : fx["cases"]) {
    const double now = c["now"];
    const bool local = c["note"] == "LOCAL-TIME";
    if (local && localOffsetMin(now) != fx["tzOffsetMin"].get<int>()) { std::printf("skip  transcript: %s (fixtures were generated in another time zone)\n", c["name"].get<std::string>().c_str()); continue; }
    TranscriptTail tail;
    int stepNo = 0;
    for (const json& st : c["steps"]) {
      if (st["restart"].get<bool>()) tail.restart();
      const std::string bytes = unb64(st["b64"]);
      tail.feed(bytes.data(), bytes.size(), now);
      const TailFacts& f = tail.facts();
      const json& a = st["after"];
      json got = { { "title", optS(f.title) }, { "aiTitle", optS(tail.aiTitle) }, { "customTitle", optS(tail.customTitle) }, { "lastText", optS(f.lastText) }, { "lastRole", optS(f.lastRole) },
        { "lastAt", optS(f.lastAt) }, { "lastAssistantAt", optS(f.lastAssistantAt) }, { "lastModel", optS(f.lastModel) }, { "lastEffort", optS(f.lastEffort) },
        { "lastContextTokens", optN(f.lastContextTokens) }, { "ultracode", f.ultracode }, { "ultracodeAt", optN(f.ultracodeAt) }, { "cwd", optS(f.cwd) },
        { "lastPromptKind", optS(f.lastPromptKind) }, { "lastPromptAt", optN(f.lastPromptAt) } };
      json queries = json::array();
      for (const json& q : a["queries"]) {
        json bg = json::array();
        for (const BackgroundWork& b : tail.background(q["since"], q["now"])) bg.push_back({ { "id", b.id }, { "kind", b.kind }, { "label", optS(b.label) }, { "at", b.at }, { "until", optN(b.until) }, { "recurring", b.recurring } });
        queries.push_back({ { "since", q["since"] }, { "now", q["now"] }, { "ultracodeSince", f.ultracodeSince(q["since"]) }, { "background", bg } });
      }
      got["queries"] = queries;
      check("transcript: " + c["name"].get<std::string>() + " poll " + std::to_string(++stepNo), got == a, firstDiff(got, a));
    }
  }
  if (localOffsetMin(fx["cron"][0]["from"]) == fx["tzOffsetMin"].get<int>()) {
    for (const json& c : fx["cron"]) {
      const std::optional<double> got = cronOnceAt(c["expr"], c["from"]);
      check("transcript: cronOnceAt '" + c["expr"].get<std::string>() + "'", optN(got) == c["out"], optN(got).dump() + " want " + c["out"].dump());
    }
  } else std::printf("skip  transcript: cronOnceAt (another time zone)\n");
  check("transcript: dateParse Z", js::dateParse("2026-10-12T08:01:00.250Z") == 1791792060250.0);
  check("transcript: dateParse offset", js::dateParse("2026-10-12T10:01:00+02:00") == 1791792060000.0);
  check("transcript: dateParse date only", js::dateParse("2026-10-12") == 1791763200000.0);
  check("transcript: dateParse junk is NaN", std::isnan(js::dateParse("not a date")) && std::isnan(js::dateParse("")) && std::isnan(js::dateParse("2026-13-01T00:00:00Z")));
}

// ---------------------------------------------------------------- livestate
static void testLive(const std::string& dir) {
  const json fx = load(dir, "livestate");
  auto alive = [&](double pid) { for (const json& p : fx["alivePids"]) if (p.get<double>() == pid) return true; return false; };
  for (const json& c : fx["cases"]) {
    LiveMap map;
    for (const json& f : c["files"]) {
      const std::string name = f["name"];
      if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;      // readLive's own filter
      map.add(f["account"], f["text"], alive);
    }
    json got = json::array();
    for (const auto& kv : map.entries()) { json o = kv.second.toJson(); o["sessionId"] = kv.first; got.push_back(o); }
    check("livestate: " + c["name"].get<std::string>(), got == c["out"], firstDiff(got, c["out"]));
  }
}

// ---------------------------------------------------------------- meta
struct TestTile {
  TileState s;
  std::unique_ptr<TranscriptTail> tail;
  bool resumable = false;
  json held = json::array();
  bool hasStartup = false, startupDone = false; double startupSince = 0; std::optional<std::string> startupTitle;
};
static std::optional<std::string> optStr(const json& v) { return v.is_string() ? std::optional<std::string>(v.get<std::string>()) : std::nullopt; }
static double numOr0(const json& v) { return v.is_number() ? v.get<double>() : 0; }
static void setField(TestTile& t, const std::string& k, const json& v, const json& whole, double now) {
  TileState& s = t.s;
  if (k == "id") s.id = v;
  else if (k == "kind") s.kind = v;
  else if (k == "account") s.account = optStr(v);
  else if (k == "sessionId") s.sessionId = optStr(v);
  else if (k == "pinned") s.pinned = js::truthy(v);
  else if (k == "group") s.group = optStr(v);
  else if (k == "userTitle") s.userTitle = optStr(v);
  else if (k == "knownTitle") s.knownTitle = optStr(v);
  else if (k == "oscTitle") s.oscTitle = optStr(v);
  else if (k == "running") { s.running = js::truthy(v); if (s.running) { if (whole.contains("ptyCols")) s.ptyCols = whole["ptyCols"]; else if (!s.ptyCols) s.ptyCols = 120; if (whole.contains("ptyRows")) s.ptyRows = whole["ptyRows"]; else if (!s.ptyRows) s.ptyRows = 30; } }
  else if (k == "ptyCols") { if (s.running) s.ptyCols = v; }
  else if (k == "ptyRows") { if (s.running) s.ptyRows = v; }
  else if (k == "asleep") s.asleep = js::truthy(v);
  else if (k == "waking") s.waking = js::truthy(v);
  else if (k == "sleptAt") s.sleptAt = numOr0(v);
  else if (k == "startedAt") s.startedAt = numOr0(v);
  else if (k == "lastStatus") s.lastStatus = optStr(v);
  else if (k == "lastStoppedInit") s.lastStoppedInit = js::truthy(v);
  else if (k == "lastStoppedAt") s.lastStoppedAt = numOr0(v);
  else if (k == "busySince") s.busySince = numOr0(v);
  else if (k == "browser") { if (v.is_boolean()) s.browser = v.get<bool>(); else s.browser.reset(); }
  else if (k == "desk") s.desk = optStr(v);
  else if (k == "attentionAt") s.attentionAt = numOr0(v);
  else if (k == "lastInputFrom") s.lastInputFrom = optStr(v);
  else if (k == "lastInputAt") s.lastInputAt = numOr0(v);
  else if (k == "lastKeyAt") s.lastKeyAt = numOr0(v);
  else if (k == "hadInput") s.hadInput = js::truthy(v);
  else if (k == "exited") s.exited = js::truthy(v);
  else if (k == "exitCode") { if (v.is_number()) s.exitCode = v.get<int>(); else s.exitCode.reset(); }
  else if (k == "cwd") s.cwd = v;
  else if (k == "modes") { s.modes.reset(); for (auto it = v.begin(); it != v.end(); ++it) if (js::truthy(it.value())) s.modes.track("\x1b[?" + it.key() + "h"); }
  else if (k == "phoneSize") { if (v.is_object()) s.phoneSize = std::make_pair(v["cols"].get<int>(), v["rows"].get<int>()); else s.phoneSize.reset(); }
  else if (k == "bridge") s.bridge = js::truthy(v) ? v : json();
  else if (k == "transcript" || k == "appendTranscript") { if (v.is_array()) { if (!t.tail) t.tail.reset(new TranscriptTail()); for (const json& line : v) t.tail->ingest(line.get<std::string>(), now); } }
  else if (k == "dropTail") t.tail.reset();
  else if (k == "resumable") t.resumable = js::truthy(v);
  else if (k == "held") t.held = v;
  else if (k == "startup") { t.hasStartup = true; t.startupDone = false; t.startupSince = numOr0(v["since"]); t.startupTitle = v.contains("waitTitle") ? optStr(v["waitTitle"]) : std::nullopt; }
  else if (k == "startupWaitTitle") t.startupTitle = optStr(v);
  else { std::printf("session_test: unknown tile field %s\n", k.c_str()); std::exit(2); }
}
static void testMeta(const std::string& dir) {
  const json fx = load(dir, "meta");
  for (const json& c : fx["cleanOsc"]) {
    const std::optional<std::string> got = cleanOsc(optStr(c["in"]));
    check("meta: cleanOsc " + c["in"].dump(), optS(got) == c["out"], optS(got).dump());
  }
  for (const json& c : fx["cases"]) {
    std::vector<std::unique_ptr<TestTile>> tiles;
    for (const json& n : c["tiles"]) {
      std::unique_ptr<TestTile> t(new TestTile());
      // spawn-time defaults the generator's mkTile gives (kind shell, 120 x 30, the fixture home as cwd)
      t->s.cwd = fx["home"];
      if (n.contains("kind")) t->s.kind = n["kind"];
      for (auto it = n.begin(); it != n.end(); ++it) setField(*t, it.key(), it.value(), n, 0);
      if (t->s.kind != "claude") t->s.browser.reset();
      tiles.push_back(std::move(t));
    }
    const json& settings = c["settings"];
    int tickNo = 0;
    for (const json& tk : c["ticks"]) {
      const double now = tk["now"];
      const std::string name = "meta: " + c["name"].get<std::string>() + " tick " + std::to_string(++tickNo);
      for (auto it = tk["patch"].begin(); it != tk["patch"].end(); ++it)
        for (auto& t : tiles) if (t->s.id == it.key()) for (auto f = it.value().begin(); f != it.value().end(); ++f) setField(*t, f.key(), f.value(), it.value(), now);
      if (tk["reorder"].is_array()) {
        std::vector<std::unique_ptr<TestTile>> next;
        for (const json& id : tk["reorder"]) for (auto& t : tiles) if (t && t->s.id == id.get<std::string>()) next.push_back(std::move(t));
        tiles = std::move(next);
      }
      // readLive's map, as records
      std::vector<std::pair<std::string, LiveRecord>> live;
      for (auto it = tk["live"].begin(); it != tk["live"].end(); ++it) {
        const json& v = it.value(); LiveRecord r;
        r.account = v["account"]; r.status = v["status"]; r.statusUpdatedAt = v["statusUpdatedAt"]; r.alive = v["alive"]; r.pid = v["pid"]; r.name = v["name"];
        r.bridgeSessionId = v["bridgeSessionId"]; r.waitingFor = v["waitingFor"]; r.kind = v["kind"]; r.cwd = v["cwd"]; r.startedAt = v["startedAt"]; r.procStart = v["procStart"];
        live.emplace_back(it.key(), r);
      }
      std::vector<std::pair<bool, json>> metas; std::vector<QuietView> views; json notify = json::array(); json after = json::object();
      std::vector<json> perTile;
      for (auto& tp : tiles) {
        TestTile& t = *tp;
        TileTick in;
        if (t.s.sessionId) for (auto& kv : live) if (kv.first == *t.s.sessionId) in.live = &kv.second;
        in.tail = t.tail.get();
        for (const json& w : c["windows"]) {
          const bool visible = !(w.contains("visible") && w["visible"] == false);
          bool shown = false; if (w.contains("shown")) for (const json& id : w["shown"]) if (id == t.s.id) shown = true;
          if (visible && shown) in.shownBy.push_back("win:" + std::to_string(w["index"].get<int>()));
          if (!in.inWindow && w.contains("layout") && w["layout"].contains(t.s.id)) in.inWindow = w["index"].get<int>();
        }
        if (c["remoteWatching"].is_array()) for (const json& id : c["remoteWatching"]) if (id == t.s.id) { in.shownBy.push_back("remote"); in.remoteWatching = true; }
        in.held = t.held;
        in.contextWindow = 200000;
        if (t.s.account) for (const json& a : c["accounts"]) if (a["id"] == *t.s.account) in.contextWindow = a["contextWindow"];
        in.resumable = t.resumable;
        in.startupOpen = t.hasStartup && !t.startupDone; in.startupSince = t.startupSince;
        if (in.startupOpen) in.startupWaitTitle = t.startupTitle;
        TickEffects fx2;
        json m = tileMeta(t.s, in, now, settings, &fx2);
        if (fx2.finishStartup) t.startupDone = true;
        if (fx2.notify) notify.push_back({ { "id", t.s.id }, { "status", fx2.notifyStatus }, { "turnMs", fx2.turnMs } });
        perTile.push_back(m);
      }
      // quietViews() runs after every tile's meta is built
      for (size_t i = 0; i < tiles.size(); i++) {
        TestTile& t = *tiles[i];
        views.push_back(quietView(t.s, perTile[i], t.tail.get(), now));
        metas.emplace_back(t.s.pinned, perTile[i]);
        after[t.s.id] = { { "lastStatus", optS(t.s.lastStatus) }, { "busySince", t.s.busySince }, { "lastStoppedAt", t.s.lastStoppedAt }, { "lastStoppedInit", t.s.lastStoppedInit },
          { "waitingFor", t.s.waitingFor }, { "knownTitle", optS(t.s.knownTitle) }, { "bridge", t.s.bridge }, { "startupDone", t.hasStartup ? json(t.startupDone) : json() } };
      }
      DeckInputs in;
      in.now = now; in.restoring = tk["restoring"]; in.accounts = c["publicAccounts"]; in.settings = settings;
      in.pagesShown = tk["meta"]["settings"]["pages"];           // the viewer's ids are the viewer's (lib/viewer.js), not collectMeta's
      for (const json& g : c["groups"]) in.groups.push_back(g);
      in.restart = restartViewUnarmed(restartBlockers(views, now), shellsLost(views));
      const json got = deckMeta(in, metas);
      check(name + " snapshot", got == tk["meta"], firstDiff(got, tk["meta"]));
      check(name + " tile state after", after == tk["after"], firstDiff(after, tk["after"]));
      check(name + " toasts asked for", notify == tk["notify"], firstDiff(notify, tk["notify"]));
    }
  }
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "core/test/fixtures/session";
  const std::string only = argc > 2 ? argv[2] : "";
  if (only.empty() || only == "replay") testReplay(dir);
  if (only.empty() || only == "transcript") testTranscript(dir);
  if (only.empty() || only == "livestate") testLive(dir);
  if (only.empty() || only == "meta") testMeta(dir);
  std::printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
