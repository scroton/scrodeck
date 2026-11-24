// core/test/deck/deck_test.cpp — core/deck against the fixtures the JavaScript generated
// (core/test/deck/gen-fixtures.js -> core/test/fixtures/deck/*.json), then the deck and its server run on fake
// seams (deck_scenarios.cpp, deck_server.cpp).   powershell -File core/test/deck/build.ps1 [section]
#include "t.hpp"
#include <csignal>
#include "fakes.hpp"
#include "../../deck/deck_impl.hpp"
#include "../../deck/hash.hpp"
#include "../../deck/held.hpp"
#include "../../deck/platform.hpp"
#include "../../deck/presence.hpp"
#include "../../deck/remote.hpp"
#include "../../deck/startup.hpp"
#include "../../deck/transcript_page.hpp"
#include "../../deck/usage.hpp"
#include "../../deck/ws.hpp"

using namespace deck::core;

int passed = 0, failed = 0;
std::string only, lastCheck;

// ---------------------------------------------------------------- what a remote client is sent (lib/phone.js)
static void testRemote(const std::string& fx) {
  const json f = loadFixture(fx, "remote");
  for (const json& c : f["clip"]) check("clipMessage(" + std::to_string(str(c["text"]).size()) + " bytes, " + c["lines"].dump() + ")", clipMessage(str(c["text"]), c["lines"].get<int>()) == str(c["out"]), clipMessage(str(c["text"]), c["lines"].get<int>()));
  int i = 0;
  for (const json& c : f["remote"]) { const json got = remoteMeta(c["meta"], c["desk"].get<bool>()); check("remoteMeta #" + std::to_string(i++), got == c["out"], got.dump()); }
  i = 0;
  for (const json& c : f["desk"]) {
    std::set<std::string> shown; for (const json& s : c["shown"]) shown.insert(str(s));
    std::set<std::string> phone; for (const json& s : c["phoneHas"]) phone.insert(str(s));
    // (lib/phone.js on main since 2026-11-19 is the branch native-remote-server's: the claim's mirror and the remote-desk rule's
    // inWindow null - the core's deskMeta with that rule)
    DeskRule rule; rule.ownWindowsOnly = true; rule.claim = true;
    const json got = deskMeta(c["meta"], shown, c["hidden"].get<bool>(), [&](const std::string& id) { return phone.count(id) > 0; }, rule);
    check("deskMeta #" + std::to_string(i++), got == c["out"], got.dump());
  }
  // metaKey: the clock is not part of what makes two metas the same; anything else is
  const json a = { { "now", 1 }, { "tiles", json::array({ { { "id", "a" } } }) } }, b = { { "now", 999 }, { "tiles", json::array({ { { "id", "a" } } }) } }, c = { { "now", 1 }, { "tiles", json::array({ { { "id", "b" } } }) } };
  check("metaKey ignores the clock", metaKey(a, "x", "y") == metaKey(b, "x", "y"));
  check("metaKey sees a tile change, the assets and the build", metaKey(a, "x", "y") != metaKey(c, "x", "y") && metaKey(a, "x", "y") != metaKey(a, "x2", "y") && metaKey(a, "x", "y") != metaKey(a, "x", nullptr));
}

// ---------------------------------------------------------------- digests, the websocket codec, the desk page
static void testWire(const std::string& fx) {
  const json f = loadFixture(fx, "wire");
  for (const json& d : f["digests"]) {
    const std::string in = unhex(str(d["hex"]));
    check("sha1 of " + std::to_string(in.size()) + " bytes", hexOf(sha1(in)) == str(d["sha1"]), hexOf(sha1(in)));
    check("sha256 of " + std::to_string(in.size()) + " bytes", hexOf(sha256(in)) == str(d["sha256"]), hexOf(sha256(in)));
    check("base64 of " + std::to_string(in.size()) + " bytes", base64(in) == str(d["base64"]), base64(in));
  }
  for (const json& h : f["hmacs"]) check("hmac-sha256, a key of " + std::to_string(str(h["key"]).size()), hexOf(hmacSha256(str(h["key"]), str(h["msg"]))) == str(h["hex"]));
  for (const json& a : f["accepts"]) check("Sec-WebSocket-Accept for " + str(a["key"]), wsAcceptKey(str(a["key"])) == str(a["accept"]), wsAcceptKey(str(a["key"])));
  for (const json& k : f["pageKeys"]) check("the page key of a token", hexOf(hmacSha256(str(k["token"]), "deck pages")).substr(0, 16) == str(k["key"]));
  check("safeEqual", safeEqual("abc", "abc") && !safeEqual("abc", "abd") && !safeEqual("abc", "abcd") && safeEqual("", ""));

  // frames, against the `ws` package's own Sender
  auto key32 = [](const json& k) { return ((uint32_t)k[0].get<int>() << 24) | ((uint32_t)k[1].get<int>() << 16) | ((uint32_t)k[2].get<int>() << 8) | (uint32_t)k[3].get<int>(); };
  for (const json& fr : f["frames"]) {
    const int n = fr["n"].get<int>(); const WsOp op = (WsOp)fr["op"].get<int>();
    std::string payload; while ((int)payload.size() < n) payload += "0123456789abcdef"; payload.resize((size_t)n);
    const std::string unmasked = wsFrame(op, payload), masked = wsFrame(op, payload, true, true, key32(fr["maskKey"]));
    const std::string name = "frame op " + std::to_string(op) + ", " + std::to_string(n) + " bytes";
    if (fr.contains("unmasked")) { check(name + ": unmasked", hexOf(unmasked) == str(fr["unmasked"])); check(name + ": masked", hexOf(masked) == str(fr["masked"])); }
    else { check(name + ": unmasked", hexOf(sha256(unmasked)) == str(fr["unmaskedSha"]) && hexOf(unmasked).substr(0, 40) == str(fr["unmaskedHead"])); check(name + ": masked", hexOf(sha256(masked)) == str(fr["maskedSha"]) && hexOf(masked).substr(0, 40) == str(fr["maskedHead"])); }
    // and back: whole, then a byte at a time for the small ones
    { WsDecoder dec; std::vector<WsEvent> ev; dec.feed(masked.data(), masked.size(), ev);
      check(name + ": decodes", ev.size() == 1 && ev[0].kind == WsEvent::Message && ev[0].binary == (op == WsBinary) && ev[0].data == payload); }
    if (n <= 200) { WsDecoder dec; std::vector<WsEvent> ev; for (char ch : masked) dec.feed(&ch, 1, ev);
      check(name + ": decodes byte by byte", ev.size() == 1 && ev[0].data == payload); }
    { WsDecoder dec; std::vector<WsEvent> ev; dec.feed(unmasked.data(), unmasked.size(), ev);
      check(name + ": an unmasked frame from a client is a protocol error", ev.size() == 1 && ev[0].kind == WsEvent::Error && ev[0].code == 1002); }
    { WsDecoder dec(false); std::vector<WsEvent> ev; dec.feed(unmasked.data(), unmasked.size(), ev);
      check(name + ": a client decodes the server's", ev.size() == 1 && ev[0].kind == WsEvent::Message && ev[0].data == payload); }
  }
  for (const json& c : f["control"]) {
    const WsOp op = (WsOp)c["op"].get<int>(); const std::string payload = unhex(str(c["payload"]));
    check("control frame op " + std::to_string(op), hexOf(wsFrame(op, payload)) == str(c["unmasked"]) && hexOf(wsFrame(op, payload, true, true, key32(c["maskKey"]))) == str(c["masked"]));
    WsDecoder dec; std::vector<WsEvent> ev; const std::string wire = unhex(str(c["masked"])); dec.feed(wire.data(), wire.size(), ev);
    const WsEvent::Kind want = op == WsPing ? WsEvent::Ping : op == WsPong ? WsEvent::Pong : WsEvent::Close;
    check("control frame op " + std::to_string(op) + " decodes", ev.size() == 1 && ev[0].kind == want && (op == WsClose ? ev[0].code == 1000 : ev[0].data == payload));
  }
  { const std::string wire = unhex(str(f["frag"]["wire"]));
    for (size_t chunk : { wire.size(), (size_t)1, (size_t)7 }) {
      WsDecoder dec; std::vector<WsEvent> ev;
      for (size_t at = 0; at < wire.size(); at += chunk) dec.feed(wire.data() + at, std::min(chunk, wire.size() - at), ev);
      check("a message in three fragments with a ping between (chunks of " + std::to_string(chunk) + ")", ev.size() == 2 && ev[0].kind == WsEvent::Ping && ev[0].data == "p" && ev[1].kind == WsEvent::Message && ev[1].data == str(f["frag"]["text"]));
    } }
  // what the `ws` Receiver refuses, and with which close code
  auto firstError = [](const std::string& wire) { WsDecoder dec; std::vector<WsEvent> ev; dec.feed(wire.data(), wire.size(), ev); return !ev.empty() && ev.back().kind == WsEvent::Error ? ev.back().code : 0; };
  auto raw = [](std::initializer_list<int> b) { std::string s; for (int x : b) s += (char)x; return s; };
  check("bad frame: a reserved bit", firstError(raw({ 0xC1, 0x80, 1, 2, 3, 4 })) == 1002);
  check("bad frame: an unknown opcode", firstError(raw({ 0x83, 0x80, 1, 2, 3, 4 })) == 1002);
  check("bad frame: a fragmented ping", firstError(raw({ 0x09, 0x80, 1, 2, 3, 4 })) == 1002);
  check("bad frame: a ping over 125 bytes", firstError(raw({ 0x89, 0xFE, 0, 126, 1, 2, 3, 4 })) == 1002);
  check("bad frame: a continuation with nothing to continue", firstError(raw({ 0x80, 0x80, 1, 2, 3, 4 })) == 1002);
  check("bad frame: a new message inside a fragmented one", firstError(wsFrame(WsText, "a", false, true, 1) + wsFrame(WsText, "b", true, true, 2)) == 1002);
  check("bad frame: text that is not UTF-8", firstError(wsFrame(WsText, "\xC3\x28", true, true, 7)) == 1007);
  check("bad frame: UTF-8 is judged on the whole message, not a fragment", firstError(wsFrame(WsText, "\xC3", false, true, 7) + wsFrame(WsCont, "\xA9", true, true, 8)) == 0);
  check("bad frame: binary may hold anything", firstError(wsFrame(WsBinary, "\xC3\x28", true, true, 7)) == 0);
  check("bad frame: a close with one byte", firstError(wsFrame(WsClose, "x", true, true, 7)) == 1002);
  check("bad frame: a close code that may not be sent", firstError(wsFrame(WsClose, raw({ 0x03, 0xED }), true, true, 7)) == 1002 && firstError(wsFrame(WsClose, raw({ 0x03, 0xEE }), true, true, 7)) == 1002);
  check("bad frame: a close reason that is not UTF-8", firstError(wsFrame(WsClose, raw({ 0x03, 0xE8, 0xFF }), true, true, 7)) == 1007);
  check("bad frame: a 64-bit length with the top bit", firstError(raw({ 0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0 })) == 1009);
  check("bad frame: more than 100 MiB", firstError(raw({ 0x82, 0xFF, 0, 0, 0, 0, 0x06, 0x40, 0, 1 })) == 1009);
  { WsDecoder dec; std::vector<WsEvent> ev; const std::string w = wsFrame(WsClose, "", true, true, 3) + wsFrame(WsText, "after", true, true, 4); dec.feed(w.data(), w.size(), ev);
    check("a close with no code reads 1005, and nothing after it is read", ev.size() == 1 && ev[0].kind == WsEvent::Close && ev[0].code == 1005); }
  check("wsCloseFrame", hexOf(wsCloseFrame(1000)) == "880203e8" && hexOf(wsCloseFrame(0)) == "8800" && hexOf(wsCloseFrame(1002, "x")) == "880303ea78");

  // the handshake
  auto req = [](const std::string& method, std::initializer_list<std::pair<const char*, const char*>> h) { HttpRequest r; r.method = method; r.target = "/"; r.version = "HTTP/1.1"; for (const auto& kv : h) r.headers.emplace_back(kv.first, kv.second); return r; };
  const std::string key = str(f["accepts"][0]["key"]), accept = str(f["accepts"][0]["accept"]);
  { const WsHandshake h = wsHandshake(req("GET", { { "upgrade", "WebSocket" }, { "connection", "Upgrade" }, { "sec-websocket-key", key.c_str() }, { "sec-websocket-version", "13" } }));
    check("handshake: 101 with the accept key", h.status == 101 && h.response == "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n", h.response); }
  check("handshake: not a GET is 405", wsHandshake(req("POST", { { "upgrade", "websocket" }, { "sec-websocket-key", key.c_str() }, { "sec-websocket-version", "13" } })).status == 405);
  check("handshake: not a websocket upgrade is 400", wsHandshake(req("GET", { { "upgrade", "h2c" }, { "sec-websocket-key", key.c_str() }, { "sec-websocket-version", "13" } })).status == 400);
  check("handshake: a key that is not 16 bytes of base64 is 400", wsHandshake(req("GET", { { "upgrade", "websocket" }, { "sec-websocket-key", "short==" }, { "sec-websocket-version", "13" } })).status == 400
    && wsHandshake(req("GET", { { "upgrade", "websocket" }, { "sec-websocket-version", "13" } })).status == 400);
  { const WsHandshake h = wsHandshake(req("GET", { { "upgrade", "websocket" }, { "sec-websocket-key", key.c_str() }, { "sec-websocket-version", "12" } }));
    check("handshake: a version that is not 13 or 8 is 426 and names them", h.status == 426 && h.response.find("Sec-WebSocket-Version: 13, 8\r\n") != std::string::npos); }

  // the desk page and what it may load
  int i = 0;
  for (const json& p : f["pages"]) { const std::string got = deskPage(str(p["html"])); check("deskPage #" + std::to_string(i++), got == str(p["out"]), got.substr(0, 400)); }
  for (const json& d : f["deskFiles"]) {
    check("deskFile " + str(d["rel"]), deskFileAllowed(str(d["rel"])) == d["ok"].get<bool>());
    check("path.posix.normalize " + str(d["rel"]), posixNormalize(str(d["rel"])) == str(d["norm"]), posixNormalize(str(d["rel"])));
  }
  check("the cookie", setCookieHeader(str(f["token"])) == str(f["cookie"]), setCookieHeader(str(f["token"])));
  // which address, and the Serve link
  for (const json& a : f["addresses"]) {
    std::vector<std::string> ipv4; for (const json& x : a["ipv4"]) ipv4.push_back(str(x));
    check("tailscaleAddress of " + a["ipv4"].dump(), tailscaleAddress(ipv4) == str(a["address"]), tailscaleAddress(ipv4));
    std::string err;
    check("bindHost: tailscale / localhost / all over " + a["ipv4"].dump(), bindHost("tailscale", ipv4, &err) == str(a["address"]) && (a["address"].is_null() ? err == "no Tailscale address on this machine (is Tailscale running?)" : err.empty())
      && bindHost("localhost", ipv4, &err) == "127.0.0.1" && bindHost("all", ipv4, &err) == "0.0.0.0" && bindHost("anything else", ipv4, nullptr) == str(a["address"]));
  }
  i = 0;
  for (const json& s : f["serve"]) { const std::string got = serveUrlFromStatus(str(s["status"]), s["port"].get<int>()); check("tailscaleServeUrl #" + std::to_string(i++), got == str(s["url"]), got); }
}

// ---------------------------------------------------------------- Claude Code's startup dialogs
static void testStartup(const std::string& fx) {
  const json f = loadFixture(fx, "startup");
  const ExistsFn exists = [](const std::string& p) { return p.size() >= 13 && p.compare(p.size() - 13, 13, "settings.json") == 0 && p.find(' ') == std::string::npos; };
  for (const json& s : f["streams"]) {
    StartupWatch w(1000, exists);
    double t = 1000;
    for (const json& c : s["chunks"]) w.feed(str(c), (t += 5));
    const std::string name = "startup " + str(s["name"]) + " in pieces of " + s["size"].dump();
    check(name + ": the dialog", w.prompt == s["prompt"], w.prompt.dump());
    check(name + ": the toast's words", describeWarning(w.prompt) == str(s["describe"]), describeWarning(w.prompt));
    check(name + ": waiting only after 4 s of quiet", w.waitingOn(t + 100) == s["waitingSoon"] && w.waitingOn(t + 4000) == s["waitingLater"]);
    check(name + ": when the deck may press Continue", w.wantsContinue(true, 0) == s["wants"].get<bool>() && w.wantsContinue(false, 0) == s["wantsOff"].get<bool>()
      && w.wantsContinue(true, 2000) == s["wantsTyped"].get<bool>() && w.wantsContinue(true, 999) == s["wantsOldKey"].get<bool>());
  }
  int i = 0;
  for (const json& c : f["screen"]) check("screenText #" + std::to_string(i++), startupScreenText(str(c["in"])) == str(c["out"]), startupScreenText(str(c["in"])));
  i = 0;
  for (const json& c : f["split"]) { const auto got = splitIncomplete(str(c["in"])); check("splitIncomplete #" + std::to_string(i++), got.first == str(c["whole"]) && got.second == str(c["rest"]), got.second); }
  i = 0;
  for (const json& c : f["classify"]) { const json got = classifyStartup(str(c["in"]), exists); check("classify #" + std::to_string(i++), got == c["out"] && describeWarning(got) == str(c["describe"]), got.dump()); }
  i = 0;
  for (const json& c : f["trust"]["cases"]) {
    const std::string t = str(c["text"]);
    check("trust text #" + std::to_string(i++), trustPromptSeen(t) == c["seen"].get<bool>() && trustOptionSeen(t) == c["option"].get<bool>() && trustMarkerLine(t) == str(c["marker"]), trustMarkerLine(t));
  }
  i = 0;
  for (const json& c : f["trust"]["append"]) check("trust text kept #" + std::to_string(i++), trustAppend(str(c["text"]), str(c["d"])) == str(c["out"]));
  { std::string acc; for (const json& c : f["trust"]["raw"]["chunks"]) acc = trustAppend(acc, str(c)); check("the recorded trust prompt, in pieces", acc == str(f["trust"]["raw"]["text"])); }
}

// ---------------------------------------------------------------- special prompts (lib/queue.js)
static void testHeld(const std::string& fx) {
  const json f = loadFixture(fx, "held");
  int i = 0;
  for (const json& c : f["visible"]) check("promptVisible #" + std::to_string(i++), promptVisible(str(c["in"])) == c["out"].get<bool>());
  for (const json& c : f["windows"]) check("inWindow " + c.dump(), heldInWindow(c["m"].get<int>(), c["s"].get<int>(), c["e"].get<int>()) == c["out"].get<bool>());
  for (const json& sc : f["scenarios"]) {
    double clock = 0; int n = 0, changes = 0; bool hasFile = false; std::string file;
    std::map<std::string, json> tiles; std::map<std::string, double> risen; json writes = json::array();
    HeldDeps d;
    d.now = [&] { return clock; };
    d.minutesOfDay = [](double) { return 0; };
    d.uuid = [&] { return "id-" + std::to_string(++n); };
    d.load = [&](std::string& out) { if (!hasFile) return false; out = file; return true; };
    d.save = [&](const std::string& text) { hasFile = true; file = text; };
    d.tile = [&](const std::string& id) { HeldTile h; auto it = tiles.find(id); if (it == tiles.end()) return h;
      h.exists = true; h.kind = str(it->second["kind"]); h.sessionId = it->second.value("sessionId", json()); h.exited = it->second.value("exited", false); h.hasPty = it->second.value("pty", false); h.status = str(it->second["status"]); return h; };
    d.recentOutput = [&](const std::string& id) { auto it = tiles.find(id); return it == tiles.end() ? std::string() : it->second.value("recent", std::string()); };
    d.write = [&](const std::string& id, const std::string& data) { writes.push_back(json::array({ id, data })); };
    d.noteInput = [](const std::string&) {};
    d.triggerRisenAt = [&](const std::string& name) { auto it = risen.find(name); return it == risen.end() ? 0.0 : it->second; };
    d.onChange = [&] { changes++; };
    d.later = [](double, std::function<void()>) {};
    d.confirmMs = 0;
    std::unique_ptr<HeldPrompts> q(new HeldPrompts(d));
    int step = 0;
    for (const json& s : sc["steps"]) {
      const std::string op = str(s["op"]);
      if (s.contains("now") && !s["now"].is_null()) clock = s["now"].get<double>();
      json result; std::string error;
      try {
        if (op == "tile") { if (s.contains("tile") && s["tile"].is_object()) tiles[str(s["id"])] = s["tile"]; else tiles.erase(str(s["id"])); }
        else if (op == "add") result = q->add(str(s["tileId"]), s.value("spec", json()));
        else if (op == "remove") result = q->remove(str(s["qid"]));
        else if (op == "forget") q->forgetTile(str(s["tileId"]));
        else if (op == "tick") q->tick();
        else if (op == "risen") risen[str(s["name"])] = s["at"].get<double>();
        else if (op == "reload") q.reset(new HeldPrompts(d));
        else if (op == "file") { hasFile = true; file = str(s["text"]); }
      } catch (const std::exception& e) { error = e.what(); }
      const std::string name = "held " + str(sc["name"]) + " step " + std::to_string(step++) + " (" + op + ")";
      if (s.contains("error")) check(name + ": refused with the same words", error == str(s["error"]), error);
      else { check(name + ": not refused", error.empty(), error); if (s.contains("result")) check(name + ": the answer", result == s["result"], result.dump()); }
      check(name + ": the list", q->list() == s["list"], q->list().dump());
      check(name + ": what was typed", writes == s["writes"], writes.dump());
      check(name + ": how often it said it changed", changes == s["changes"].get<int>(), std::to_string(changes));
      if (s.contains("forTile")) check(name + ": forTile", q->forTile(str(s["tileId"])) == s["forTile"]);
      if (s["saved"].is_object()) { const json saved = json::parse(hasFile ? file : "null", nullptr, false); check(name + ": the file", saved == s["saved"], saved.dump()); }
    }
  }
}

// ---------------------------------------------------------------- usage (lib/usage.js), on a fake endpoint and fake credentials
static void testUsage(const std::string& fx) {
  const json f = loadFixture(fx, "usage");
  fake::Clock clock; fake::MemFiles files; files.clock = &clock; fake::Http http; fake::Creds creds;
  std::vector<Account> accounts;
  for (const json& a : f["accounts"]) { Account acc; acc.id = str(a["id"]); acc.dir = str(a["dir"]); accounts.push_back(acc); }
  const std::string cache = "C:\\state\\usage-cache.json";
  std::unique_ptr<UsagePoller> poller(new UsagePoller(clock, files, &http, &creds, cache));
  for (const json& s : f["steps"]) {
    const std::string name = "usage: " + str(s["label"]);
    clock.t = s["clock"].get<double>();
    creds.oauth.clear(); creds.stamps.clear(); http.byToken.clear();
    for (auto it = s["creds"].begin(); it != s["creds"].end(); ++it) creds.oauth[it.key()] = it.value();
    for (auto it = s["stamps"].begin(); it != s["stamps"].end(); ++it) creds.stamps[it.key()] = str(it.value());
    for (auto it = s["responses"].begin(); it != s["responses"].end(); ++it) {
      HttpResponse r; r.status = it.value()["status"].get<int>();
      if (it.value().contains("headers")) for (auto h = it.value()["headers"].begin(); h != it.value()["headers"].end(); ++h) r.headers[h.key()] = str(h.value());
      r.body = it.value().contains("text") ? str(it.value()["text"]) : it.value().value("json", json::object()).dump();
      http.byToken[it.key()] = r;
    }
    if (s["reload"].get<bool>()) poller.reset(new UsagePoller(clock, files, &http, &creds, cache));
    const size_t before = http.asked.size(); bool done = false;
    poller->pollAll(accounts, s["force"].get<bool>(), [&] { done = true; });
    check(name + ": every account answered", done);
    check(name + ": how many requests", (int)(http.asked.size() - before) == s["requests"].get<int>(), std::to_string(http.asked.size() - before));
    bool shape = true;
    for (size_t k = before; k < http.asked.size(); k++) {
      const auto& a = http.asked[k];
      if (a.url != "https://api.anthropic.com/api/oauth/usage" || a.headers.size() != 3 || a.headers.at("anthropic-beta") != "oauth-2025-04-20" || a.headers.at("Content-Type") != "application/json" || a.headers.at("Authorization").compare(0, 7, "Bearer ") != 0) shape = false;
    }
    check(name + ": the request is the Electron deck's (url, bearer, beta header)", shape);
    json got = poller->data(), want = s["data"];
    // (JSON.parse's own wording for a body that is not JSON is not reproduced)
    for (auto it = want.begin(); it != want.end(); ++it) if (it.value().value("state", "") == "error" && it.value().value("error", "").compare(0, 10, "Unexpected") == 0) { it.value().erase("error"); if (got.contains(it.key())) got[it.key()].erase("error"); }
    check(name + ": the data", got == want, got.dump());
    const json onDisk = files.has(cache) ? files.getJson(cache) : json();
    check(name + ": the cache file (numbers only)", onDisk == s["cache"], onDisk.dump());
    check(name + ": the cache never holds a token", files.get(cache).find("accessToken") == std::string::npos && files.get(cache).find("Bearer") == std::string::npos && files.get(cache).find("claudeAiOauth") == std::string::npos);
  }
  // with no credentials seam at all nothing is asked and every account reads no-credentials
  { fake::Http h2; UsagePoller p(clock, files, &h2, nullptr, ""); bool done = false; p.pollAll(accounts, true, [&] { done = true; });
    check("usage: no credentials seam, no request", done && h2.asked.empty() && p.data()["a1"]["state"] == "no-credentials"); }
}

// ---------------------------------------------------------------- main.js patterns, lib/triggers.js, lib/platform.js
static void testRules(const std::string& fx) {
  const json f = loadFixture(fx, "rules");
  int i = 0;
  for (const json& c : f["terminalReply"]) check("TERMINAL_REPLY #" + std::to_string(i++), terminalReply(str(c["in"])) == c["out"].get<bool>(), c["in"].dump());
  i = 0;
  for (const json& c : f["usageNotice"]) check("USAGE_NOTICE #" + std::to_string(i++) + " " + c["in"].dump(), usageNotice(str(c["in"])) == c["out"].get<bool>());
  for (const json& c : f["triggerNames"]) if (c["in"].is_string()) check("trigger name " + c["in"].dump(), cleanTriggerName(str(c["in"])) == str(c["out"]), cleanTriggerName(str(c["in"])));
  if (str(f["os"]) == "win32") {
    for (const json& k : f["keys"]) check("keyAction " + k["input"].dump(), platform::keyAction("win32", k["input"]) == str(k["action"]), platform::keyAction("win32", k["input"]));
    check("keyHelp and the font stack (Windows)", platform::keyHelp("win32") == f["keyHelp"] && platform::defaultFontStack("win32") == str(f["fontStack"]));
  }
  // lib/platform.js, the decisions: on fake files and a fake environment
  fake::MemFiles files; fake::Env env;
  check("claudeExe: an override wins; else ~/.local/bin/claude.exe when it is a file; else the Path", platform::claudeExe(env, files, "D:\\x\\claude.exe") == "D:\\x\\claude.exe" && platform::claudeExe(env, files) == "claude.exe"
    && (files.put("C:\\home\\.local\\bin\\claude.exe", "x"), platform::claudeExe(env, files)) == "C:\\home\\.local\\bin\\claude.exe");
  check("shellCommand (Windows): PowerShell -NoLogo", platform::shellCommand(env).file == "powershell.exe" && platform::shellCommand(env).args == std::vector<std::string>{ "-NoLogo" } && platform::shellCommand(env).name == "PowerShell");
  check("bashPath: Git Bash where it is installed, else bash.exe", platform::bashPath(env, files) == "bash.exe" && (files.put("C:\\Program Files\\Git\\bin\\bash.exe", "x"), platform::bashPath(env, files)) == "C:\\Program Files\\Git\\bin\\bash.exe"
    && platform::bashPath(env, files, "Z:\\bash.exe") == "Z:\\bash.exe");
  check("userswitchPath: the user's copy, else the bundled one", platform::userswitchPath(env, files, "C:\\app") == "C:\\app\\bin\\userswitch" && (files.put("C:\\home\\.local\\bin\\userswitch", "x"), platform::userswitchPath(env, files, "C:\\app")) == "C:\\home\\.local\\bin\\userswitch");
  check("aliasFiles: USERSWITCH_ZSHRC, else ~/.claude-code-account-aliases", platform::aliasFiles(env, files).primary == "C:\\home\\.claude-code-account-aliases"
    && (env.env.emplace_back("USERSWITCH_ZSHRC", "D:\\a.txt"), platform::aliasFiles(env, files).primary) == "D:\\a.txt" && platform::aliasFiles(env, files).extra.empty());
}

// ---------------------------------------------------------------- the phone's text view (lib/transcript-page.js)
static void testTranscriptPage(const std::string& fx) {
  const json f = loadFixture(fx, "transcript-page");
  int i = 0;
  for (const json& c : f["cases"]) {
    const std::string raw = str(c["raw"]);
    const json got = transcriptEntries(raw);
    const std::string name = "transcript page #" + std::to_string(i++);
    check(name + ": " + std::to_string(c["entries"].size()) + " entries", got.size() == c["entries"].size(), std::to_string(got.size()));
    for (size_t k = 0; k < got.size() && k < c["entries"].size(); k++) check(name + ": entry " + std::to_string(k) + " (" + str(c["entries"][k]["role"]) + ")", got[k] == c["entries"][k], got[k].dump());
    for (const json& p : c["pages"]) { const json page = transcriptPage(raw, p["opts"]); check(name + ": page " + p["opts"].dump(), page == p["out"], page.dump().substr(0, 300)); }
  }
}

// ---------------------------------------------------------------- the deck's contract with the sessions it hosts (lib/presence.js)
static void testPresence(const std::string& fx) {
  const json f = loadFixture(fx, "presence");
  for (const json& sc : f["scenarios"]) {
    const json& o = sc["opts"];
    Presence p(o["deck"], o["awayMinutes"], o["now"].get<double>(), o["prev"], o.value("pcName", std::string()));
    int n = 0;
    for (const json& s : sc["steps"]) {
      const std::string op = str(s["op"]);
      const std::string name = "presence " + str(sc["name"]) + " step " + std::to_string(n++) + " (" + op + ")";
      bool changed = false;
      if (op == "input") changed = p.input(str(s["kind"]), s["now"].get<double>());
      else if (op == "connections") changed = p.connections(s.value("remote", json()), s["phone"].get<bool>(), s["now"].get<double>());
      else if (op == "tick") changed = p.tick(s["now"].get<double>());
      else if (op == "away") changed = p.setAwayMinutes(s.value("value", json()), s["now"].get<double>());
      if (op != "new") check(name + ": changed " + s["changed"].dump(), changed == s["changed"].get<bool>());
      check(name + ": the file's text, byte for byte", p.text() == str(s["text"]), p.text());
      check(name + ": the state", json(p.state()) == s["state"]);
    }
  }
  for (const json& c : f["away"]) check("awayMinutes " + c["in"].dump(), awayMinutes(c["in"]) == c["out"].get<int>(), std::to_string(awayMinutes(c["in"])));
  for (const json& c : f["kinds"]) check("deskKind " + c["in"].dump(), deskKind(c["in"]) == str(c["out"]));
  const auto env = tileEnv("tile-1", "C:\\state\\deck-state.json");
  check("tileEnv: CLAUDE_DECK_TILE and CLAUDE_DECK_STATE", env.size() == 2 && env[0].first == "CLAUDE_DECK_TILE" && env[0].second == str(f["env"]["CLAUDE_DECK_TILE"]) && env[1].first == "CLAUDE_DECK_STATE" && env[1].second == str(f["env"]["CLAUDE_DECK_STATE"])
    && f["defaultAway"] == kDefaultAwayMinutes);
}

int main(int argc, char** argv) {
  // a JSON assertion or an uncaught throw ends the run: say where (the last check that ran)
  std::signal(SIGABRT, [](int) { std::printf("\nABORTED after the check: %s\n", lastCheck.c_str()); std::fflush(stdout); });
  // --posix (anywhere): the POSIX twin of the fakes (fakes.hpp "THE PATH MODE") - `/` paths, os() darwin, a login
  // $SHELL - for the suites that run the deck on the fakes: the scenarios, hosting, options, the remote / driver
  // rules under the deck, the server. The pure fixture sections (remote, wire, startup, … transcript, asks,
  // viewer, deskopen, cloud) and `parity` replay what the JavaScript answered on THIS box - Windows paths in the
  // recordings themselves - and run in the default (win32) mode only.
  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) { if (std::string(argv[i]) == "--posix") fake::setPosix(true); else args.push_back(argv[i]); }
  if (args.size() < 2) { std::printf("usage: deck-test <fixtures dir> <repo root> [section] [--posix]\n"); return 2; }
  const std::string fx = args[0], repo = args[1];
  if (args.size() == 6 && args[2] == "askcards-emit") return askCardsEmit(args[3], args[4], args[5]);   // core/test/deck/askcards-read.js
  if (args.size() > 2) only = args[2];
  if (fake::posix()) {
    std::printf("deck-test: POSIX mode (darwin, '/' paths, $SHELL -l): deck, hosting, options, remoterule, driverrule, ruling, server\n");
    testScenarios(fx, repo);
    testHostingAndOptions(fx);
    testRemoteRules(fx);
    testServer(fx, repo);
    testOneDeck(fx);
    std::printf("\n%d passed, %d failed  (posix mode)\n", passed, failed);
    return failed ? 1 : 0;
  }
  if (want("remote")) testRemote(fx);
  if (want("wire")) testWire(fx);
  if (want("startup")) testStartup(fx);
  if (want("held")) testHeld(fx);
  if (want("usage")) testUsage(fx);
  if (want("rules")) testRules(fx);
  if (want("presence")) testPresence(fx);
  if (want("transcript")) testTranscriptPage(fx);
  testAsks(fx);
  testAskCards(fx);
  testViewer(fx);
  testDeskOpen(fx);
  testCloud(fx);
  testScenarios(fx, repo);
  testServer(fx, repo);
  testParity(fx);
  testRemoteRules(fx);
  testHostingAndOptions(fx);
  testOneDeck(fx);
  std::printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
