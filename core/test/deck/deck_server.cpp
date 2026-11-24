// core/test/deck/deck_server.cpp — the server (core/deck/server.hpp) with no socket anywhere: a connection is two
// strings (what the peer sent, what the server wrote), the listener records what it was asked to bind, and the
// pages are the repo's own files. Checked against what the REAL lib/phone.js answered to the same requests
// (core/test/fixtures/deck/routes.json, recorded over loopback by gen-fixtures.js --routes).
#include "t.hpp"
#include "fakes.hpp"
#include "../../deck/hash.hpp"
#include "../../deck/server.hpp"
#include "../../session/jsutil.hpp"
#include <cmath>

using namespace deck::core;
using fake::P;

namespace {

struct Res { int status = 0; std::map<std::string, std::string> h; std::string body; bool ok = false; size_t used = 0; };
// one HTTP response off the front of `wire`
Res parseResponse(const std::string& wire, bool head = false) {
  Res r;
  const size_t end = wire.find("\r\n\r\n");
  if (end == std::string::npos || wire.compare(0, 9, "HTTP/1.1 ") != 0) return r;
  r.status = std::atoi(wire.substr(9, 3).c_str());
  size_t at = wire.find("\r\n") + 2;
  while (at < end + 2) {
    const size_t e = wire.find("\r\n", at);
    const std::string line = wire.substr(at, e - at); at = e + 2;
    const size_t c = line.find(':');
    if (c != std::string::npos) r.h[lowerAscii(line.substr(0, c))] = line.substr(c + 2);
  }
  const size_t len = r.h.count("content-length") ? (size_t)std::atoll(r.h["content-length"].c_str()) : 0;
  r.body = head ? std::string() : wire.substr(end + 4, len);
  r.used = end + 4 + (head ? 0 : len);
  r.ok = head || r.body.size() == len;
  return r;
}

struct VaryRandom : fake::Random { int k = 0; std::string hex(int bytes) override { std::string s; char b[4]; std::snprintf(b, sizeof b, "%02x", ++k & 0xFF); for (int i = 0; i < bytes; i++) s += b; return s; } };

struct Net {
  fake::Clock clock; fake::MemFiles files; fake::DiskFiles disk; fake::Ptys ptys; fake::Procs procs; VaryRandom random; fake::Env env; fake::Listener listener; fake::Net net;
  std::unique_ptr<Server> server; std::unique_ptr<Deck> deck;
  ClientId window = 0; std::vector<json> out;
  uint64_t nextConn = 1; uint32_t maskKey = 0x1020304;
  std::map<uint64_t, std::unique_ptr<WsDecoder>> dec;

  explicit Net(const std::string& repo, int forcePort = 0, json phone = json(), std::function<void(DeckOptions&)> tweak = nullptr) {
    files.clock = &clock; ptys.alivePids = &procs.alivePids;
    DeckOptions o; o.stateDir = P("C:\\state"); o.home = P("C:\\home"); o.aliasFile = P("C:\\home\\aliases"); o.shellFile = "cmd.exe"; o.shellArgs = {}; o.shellName = "cmd";
    if (tweak) tweak(o);
    if (!phone.is_null()) files.put(P("C:\\state\\settings.json"), json{ { "phone", phone } }.dump());
    ServerOptions so; so.assetRoot = repo; so.forcePort = forcePort;
    server.reset(new Server(so, disk, clock, listener, net));
    Seams sm; sm.clock = &clock; sm.files = &files; sm.ptys = &ptys; sm.procs = &procs; sm.random = &random; sm.env = &env; sm.phone = server.get();
    deck.reset(new Deck(o, sm, [this](ClientId c, const json& f) { if (c == window) { out.push_back(f); return true; } return server->deliver(c, f); }));
    server->attach(deck.get());
    window = deck->addWindow();
    deck->start();
    deck->handle(window, { { "t", "call" }, { "rid", 1 }, { "fn", "deskInit" }, { "args", json::array() } });
    advance(2100);
  }
  void advance(double ms) {
    const double end = clock.t + ms;
    for (;;) {
      while (!ptys.exits.empty()) { const auto e = ptys.exits.front(); ptys.exits.pop_front(); if (fake::Pty* p = ptys.byToken(e.first)) p->gone = true; deck->onPtyExit(e.first, e.second); }
      deck->tick(); server->tick();
      if (clock.t >= end) break;
      clock.t += std::min(end - clock.t, std::max(1.0, (double)std::min(deck->nextDueMs(), 1000)));
    }
  }
  std::string token() { return str(deck->settings()["phone"]["token"]); }
  json status() { return server->status(deck->settings()["phone"]); }
  uint64_t connect() { const uint64_t c = nextConn++; server->onAccept(c); return c; }
  void feed(uint64_t c, const std::string& bytes) { server->onBytes(c, bytes.data(), bytes.size()); }
  // one request on a connection of its own
  Res http(const std::string& method, const std::string& target, const std::map<std::string, std::string>& headers = {}, const std::string& body = "") {
    const uint64_t c = connect();
    std::string raw = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    for (const auto& kv : headers) raw += kv.first + ": " + kv.second + "\r\n";
    if (!body.empty()) raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    raw += "Connection: close\r\n\r\n" + body;
    feed(c, raw);
    Res r = parseResponse(listener.sent[c], method == "HEAD");
    r.ok = r.ok && listener.closed[c];
    return r;
  }
  // a websocket: the upgrade, then frames
  uint64_t socket(const std::string& target, const std::string& cookie = "", std::string* response = nullptr, const std::string& extra = "") {
    const uint64_t c = connect();
    std::string raw = "GET " + target + " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    if (!cookie.empty()) raw += "Cookie: " + cookie + "\r\n";
    raw += "\r\n" + extra;
    feed(c, raw);
    std::string& sent = listener.sent[c];
    const size_t end = sent.find("\r\n\r\n");
    if (response) *response = end == std::string::npos ? sent : sent.substr(0, end + 4);
    if (end != std::string::npos) sent.erase(0, end + 4);
    dec[c].reset(new WsDecoder(false));
    return c;
  }
  std::string open(const std::string& extra = "") { return std::string("/?t=") + token() + extra; }
  void say(uint64_t c, const json& m) { feed(c, wsFrame(WsText, m.dump(), true, true, maskKey++)); }
  // every frame the server has written to this socket since the last look
  std::vector<WsEvent> heard(uint64_t c) { std::vector<WsEvent> ev; const std::string s = listener.take(c); dec[c]->feed(s.data(), s.size(), ev); return ev; }
  std::vector<json> frames(uint64_t c) { std::vector<json> v; for (const WsEvent& e : heard(c)) if (e.kind == WsEvent::Message) v.push_back(json::parse(e.data, nullptr, false)); return v; }
  static json find(const std::vector<json>& v, const char* t) { for (size_t i = v.size(); i-- > 0;) if (v[i].is_object() && v[i].value("t", "") == t) return v[i]; return json(); }
  json ask(uint64_t c, const std::string& fn, json args = json::array()) { static int rid = 500; const int id = ++rid; say(c, { { "t", "call" }, { "rid", id }, { "fn", fn }, { "args", args } });
    for (const json& f : frames(c)) { if ((f["t"] == "ok" || f["t"] == "err") && f["rid"] == id) return f; }
    return json(); }
};

void testBind(const std::string& repo) {
  // ---- off: nothing is bound
  { Net n(repo);
    const json st = n.status();
    check("serve: off by default — nothing listens, and the status says so", !n.listener.listening && n.listener.listens == 0 && st["enabled"] == false && st["running"] == false && st["url"].is_null() && st["error"].is_null());
    check("serve: phoneStatus has main.js's twelve fields, and the two this server adds (assets, assetRoot)", st.size() == 14 && st["assets"] == "ok" && st["assetRoot"] == repo && st.contains("enabled") && st.contains("running") && st.contains("port") && st.contains("bind") && st.contains("token") && st.contains("address")
      && st.contains("serve") && st.contains("url") && st.contains("clients") && st.contains("desks") && st.contains("tailscale") && st.contains("error"), st.dump());
    // Settings -> Phone turns it on: bind `tailscale` is the default
    n.net.addrs = { "192.168.1.9" };
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 2 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "enabled", true }, { "port", 8813 } } } } }) } });
    check("serve (tailscale): no Tailscale address on the machine: it refuses to start, with lib/phone.js's words", !n.listener.listening && n.listener.listens == 0 && n.status()["error"] == "no Tailscale address on this machine (is Tailscale running?)"
      && n.status()["enabled"] == true && n.status()["running"] == false && n.status()["tailscale"].is_null() && n.out.back()["result"]["phoneStatus"]["error"] == "no Tailscale address on this machine (is Tailscale running?)", n.status().dump());
    n.net.addrs = { "192.168.1.9", "100.101.102.103" };
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 3 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "port", 8814 } } } } }) } });
    check("serve (tailscale): it binds the machine's Tailscale address and nothing else", n.listener.listening && n.listener.host == "100.101.102.103" && n.listener.port == 8814 && n.status()["running"] == true && n.status()["address"] == "100.101.102.103"
      && n.status()["tailscale"] == "100.101.102.103" && n.status()["serve"].is_null() && n.status()["url"] == "http://100.101.102.103:8814/?t=" + n.token() && n.net.serveAsked == 0, n.status().dump());
    // behind Tailscale Serve
    n.net.serveStatus = "{\"Web\":{\"box.tail1234.ts.net:443\":{\"Handlers\":{\"/\":{\"Proxy\":\"http://localhost:8815\"}}}}}";
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 4 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "bind", "localhost" }, { "port", 8815 } } } } }) } });
    check("serve (localhost): 127.0.0.1 only, and the link shown is the tailnet HTTPS front door Serve gives it", n.listener.host == "127.0.0.1" && n.listener.port == 8815 && n.listener.stops == 1 && n.status()["serve"] == "https://box.tail1234.ts.net/"
      && n.status()["url"] == "https://box.tail1234.ts.net/?t=" + n.token() && n.status()["address"] == "127.0.0.1", n.status().dump());
    const int asked = n.net.serveAsked;
    n.status(); n.status(); n.clock.t += 30000; n.status();
    check("serve (localhost): `tailscale serve status` is asked at most once a minute", n.net.serveAsked == asked && (n.clock.t += 31000, n.status(), n.net.serveAsked == asked + 1));
    n.net.serveStatus.clear(); n.clock.t += 61000;
    check("serve (localhost): Serve not set up: no front door, the link is the loopback one", n.status()["serve"].is_null() && n.status()["url"] == "http://127.0.0.1:8815/?t=" + n.token());
    // every interface, only when the setting says so
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 5 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "bind", "all" } } } } }) } });
    check("serve (all): 0.0.0.0 — only on the setting's word", n.listener.host == "0.0.0.0" && n.listener.port == 8815 && n.status()["bind"] == "all");
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 6 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "bind", "everything" } } } } }) } });
    check("serve: a bind that is not one of the three changes nothing", n.listener.host == "0.0.0.0" && n.status()["bind"] == "all");
    // a new token restarts it, and the old one stops working
    const std::string old = n.token(); const int stops = n.listener.stops;
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 7 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "newToken", true } } } } }) } });
    check("serve: \"new token\" replaces it and restarts the server", n.token() != old && n.token().size() == 16 && n.listener.stops == stops + 1 && n.listener.listening && n.http("GET", "/?t=" + old).status == 401 && n.http("GET", "/?t=" + n.token()).status == 200);
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 8 }, { "fn", "saveSettings" }, { "args", json::array({ { { "fontSize", 15 } } }) } });
    check("serve: a save that does not touch the phone block leaves the server alone", n.listener.stops == stops + 1);
    n.listener.failWith = "listen EADDRINUSE: address already in use 0.0.0.0:8816";
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 9 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "port", 8816 } } } } }) } });
    check("serve: a port that is taken is an error on the status, not a crash", n.status()["running"] == false && n.status()["error"] == "listen EADDRINUSE: address already in use 0.0.0.0:8816" && n.status()["url"].is_null());
    n.listener.failWith.clear();
    n.deck->handle(n.window, { { "t", "call" }, { "rid", 10 }, { "fn", "saveSettings" }, { "args", json::array({ { { "phone", { { "enabled", false } } } } }) } });
    check("serve: switched off, it stops", !n.listener.listening && n.status()["enabled"] == false && n.status()["error"].is_null());
    n.deck->shutdown(); }
  // ---- restored from settings.json at boot
  { Net n(repo, 0, { { "enabled", true }, { "port", 8817 }, { "bind", "localhost" }, { "token", "feedfacefeedface" } });
    check("serve: a deck whose settings say on starts serving at boot, with the saved token", n.listener.listening && n.listener.host == "127.0.0.1" && n.listener.port == 8817 && n.token() == "feedfacefeedface" && n.http("GET", "/?t=feedfacefeedface").status == 200);
    n.deck->shutdown(); }
  // ---- --serve <port>: loopback, whatever the settings say
  { Net n(repo, 8811, { { "enabled", false }, { "port", 8788 }, { "bind", "all" }, { "token", "feedfacefeedface" } });
    n.net.addrs = { "100.101.102.103" }; n.net.serveStatus = "{\"Web\":{\"box.ts.net:443\":{\"Handlers\":{\"/\":{\"Proxy\":\"http://localhost:8811\"}}}}}";
    const json st = n.status();
    check("--serve <port>: 127.0.0.1:<port> for this run, never the address or the port in the settings, and Tailscale is asked nothing", n.listener.host == "127.0.0.1" && n.listener.port == 8811 && st["enabled"] == true && st["running"] == true && st["bind"] == "localhost"
      && st["port"] == 8811 && st["serve"].is_null() && st["url"] == "http://127.0.0.1:8811/?t=feedfacefeedface" && n.net.serveAsked == 0 && n.files.getJson(P("C:\\state\\settings.json"))["phone"]["enabled"] == false, st.dump());
    n.deck->shutdown(); }
}

void testRoutes(const std::string& fx, const std::string& repo) {
  const json f = loadFixture(fx, "routes");
  Net n(repo, 8811, { { "enabled", false }, { "port", 8788 }, { "bind", "tailscale" }, { "token", str(f["token"]) } });
  check("routes: the page key is lib/phone.js's (an HMAC of the token)", n.server->pageKey() == str(f["pageKey"]) && n.server->extras()["pageKey"] == str(f["pageKey"]));
  for (const json& q : f["requests"]) {
    std::map<std::string, std::string> headers;
    for (auto it = q["headers"].begin(); it != q["headers"].end(); ++it) headers[it.key()] = str(it.value());
    const std::string method = str(q["method"]), target = str(q["target"]);
    const Res r = n.http(method, target, headers, q["body"].is_null() ? std::string() : str(q["body"]));
    const json& w = q["res"];
    const std::string name = "route " + method + " " + target + (headers.count("cookie") ? " (cookie)" : "");
    auto h = [&](const char* k) { auto it = r.h.find(k); return it == r.h.end() ? json() : json(it->second); };
    const bool upload = method == "POST" && target == "/upload" && w["status"] == 200;
    check(name + ": status " + w["status"].dump(), r.ok && r.status == w["status"].get<int>(), std::to_string(r.status));
    check(name + ": content-type, cache-control, the cookie, the policy", h("content-type") == w["type"] && h("cache-control") == w["cache"] && h("set-cookie") == w["cookie"] && h("content-security-policy") == w["csp"],
      h("content-type").dump() + " " + h("cache-control").dump() + " " + h("set-cookie").dump());
    if (upload) continue;
    const auto eb = editionBody(w);
    check(name + ": the body, byte for byte", r.body.size() == eb.first && hexOf(sha256(r.body)) == eb.second, std::to_string(r.body.size()) + " bytes: " + r.body.substr(0, 120));
  }
  { const Res r = n.http("POST", "/upload", { { "cookie", "deck_phone=" + str(f["token"]) }, { "x-file-name", "my%20notes%20(1).txt" } }, "hello upload");
    const json j = json::parse(r.body, nullptr, false);
    check("upload: the file is saved by the deck and its path handed back", r.status == 200 && j.is_object() && n.files.get(str(j["path"])) == "hello upload" && str(j["path"]).find(P("\\uploads\\")) != std::string::npos && str(j["path"]).find("my_notes_1_.txt") != std::string::npos, r.body);
    std::string chunked = "POST /upload?t=" + str(f["token"]) + " HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nX-File-Name: c.bin\r\nConnection: close\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n";
    const uint64_t c = n.connect();
    for (char ch : chunked) n.feed(c, std::string(1, ch));
    const json j2 = json::parse(parseResponse(n.listener.sent[c]).body, nullptr, false);
    check("upload: a chunked body, a byte at a time", j2.is_object() && n.files.get(str(j2["path"])) == "hello world", n.listener.sent[c]); }
  // who is let in (lib/phone.js authed over the same cases)
  const json wire = loadFixture(fx, "wire");
  for (const json& a : wire["authCases"]) {
    std::map<std::string, std::string> headers; if (!str(a["cookie"]).empty()) headers["cookie"] = str(a["cookie"]);
    const Res r = n.http("GET", str(a["url"]), headers);
    const std::string how = r.status == 401 ? "" : (r.h.count("set-cookie") ? "query" : "cookie");
    check("auth " + str(a["url"]) + " | " + str(a["cookie"]), how == str(a["how"]) && (how != "query" || r.h.at("set-cookie") == str(wire["cookie"])), how);
  }
  { const json ex = n.server->extras(); std::string app; n.disk.read(repo + P("\\renderer\\phone\\app.js"), app);
    const size_t a = app.find("const BUILD = '");
    check("extras: the page's own BUILD string and a fingerprint of its four files ride with every meta", a != std::string::npos && ex["build"] == app.substr(a + 15, app.find('\'', a + 15) - a - 15)
      && str(ex["assets"]).find("index.html:") == 0 && str(ex["assets"]).find(";app.js:") != std::string::npos && str(ex["assets"]).find("sw.js:") != std::string::npos && str(ex["assets"]).find("none") == std::string::npos, ex.dump()); }
  // the socket, as the real one answered
  for (const json& s : f["sockets"]) {
    std::string response;
    const uint64_t c = n.socket(str(s["target"]), s["headers"].value("cookie", std::string()), &response);
    const int status = response.compare(0, 12, "HTTP/1.1 101") == 0 ? 101 : std::atoi(response.substr(9, 3).c_str());
    const std::string name = "socket " + str(s["target"]) + (s["headers"].contains("cookie") ? " (cookie)" : "");
    check(name + ": " + s["res"]["status"].dump(), status == s["res"]["status"].get<int>(), response);
    if (status != 101) { check(name + ": refused before the handshake, and closed", response == "HTTP/1.1 401 Unauthorized\r\n\r\n" && n.listener.closed[c]); continue; }
    const std::vector<json> fr = n.frames(c);
    json keys = json::array(); if (!fr.empty()) for (auto it = fr[0].begin(); it != fr[0].end(); ++it) keys.push_back(it.key());
    check(name + ": the first frame is the hello, with the same fields", !fr.empty() && fr[0]["t"] == s["res"]["first"] && keys == s["res"]["keys"], keys.dump());
    n.feed(c, wsFrame(WsClose, std::string("\x03\xe8", 2), true, true, 9));
  }
  n.deck->shutdown();

  // ---- the wire itself
  Net m(repo, 8811, { { "token", "feedfacefeedface" } });
  const std::string T = "?t=feedfacefeedface";
  { const uint64_t c = m.connect();
    m.feed(c, "GET /manifest.webmanifest HTTP/1.1\r\nHost: x\r\n\r\nGET /icon-192.png HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    std::string w = m.listener.sent[c];
    const Res a = parseResponse(w); w.erase(0, a.used); const Res b = parseResponse(w); w.erase(0, b.used); const Res d = parseResponse(w);
    check("http: three requests in one read on one connection get three answers, in order; `Connection: close` ends it", a.status == 200 && a.h.at("connection") == "keep-alive" && b.status == 200 && b.h.at("content-type") == "image/png" && d.status == 401
      && d.h.at("connection") == "close" && m.listener.closed[c]); }
  { const uint64_t c = m.connect(); const std::string req = "GET /icon-192.png HTTP/1.1\r\nHost: x\r\n\r\n";
    for (char ch : req) m.feed(c, std::string(1, ch));
    check("http: a request that arrives a byte at a time; the connection stays open", parseResponse(m.listener.sent[c]).status == 200 && !m.listener.closed[c]); }
  { const uint64_t c = m.connect(); m.feed(c, "GET /icon-192.png HTTP/1.0\r\n\r\n");
    check("http: HTTP/1.0 closes unless it asks to stay", parseResponse(m.listener.sent[c]).status == 200 && m.listener.closed[c]); }
  { const uint64_t c = m.connect(); m.feed(c, "this is not http\r\n\r\n");
    check("http: nonsense is a 400 and the end of the connection", parseResponse(m.listener.sent[c]).status == 400 && m.listener.closed[c]);
    const uint64_t c2 = m.connect(); m.feed(c2, "GET / HTTP/1.1\r\nX: " + std::string(17000, 'a'));
    check("http: a head over 16 KB is a 431", parseResponse(m.listener.sent[c2]).status == 431 && m.listener.closed[c2]);
    const uint64_t c3 = m.connect(); m.feed(c3, "POST /upload" + T + " HTTP/1.1\r\nContent-Length: 209715201\r\n\r\n");
    check("http: an upload over 200 MB is cut off before a byte of it is read", parseResponse(m.listener.sent[c3]).status == 413 && m.listener.closed[c3]);
    const uint64_t c4 = m.connect(); m.feed(c4, "GET /%E0%A4%A HTTP/1.1\r\n\r\n");
    check("http: a path that will not decode is a 400", parseResponse(m.listener.sent[c4]).status == 400); }

  // ---- a phone over the socket
  std::string response;
  const uint64_t ph = m.socket("/anything" + T, "", &response, wsFrame(WsText, "{\"t\":\"call\",\"rid\":41,\"fn\":\"newTile\",\"args\":[{\"kind\":\"shell\"}]}", true, true, 77));
  check("socket: 101 with the accept key of RFC 6455's own example", response == "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", response);
  std::vector<json> fr = m.frames(ph);
  const json made = Net::find(fr, "ok");
  check("socket: frames that arrive in the same read as the upgrade are not lost", !fr.empty() && fr[0]["t"] == "init" && made["rid"] == 41 && made["result"].is_string() && m.deck->remoteCount() == 1, made.dump());
  const std::string id = str(made["result"]);
  fake::Pty* p = m.ptys.last();
  m.say(ph, { { "t", "attach" }, { "id", id } });
  m.deck->onPtyData(p->token, "hello phone", 11);
  check("socket: an attached tile's output arrives as a data frame", Net::find(m.frames(ph), "data") == json{ { "t", "data" }, { "id", id }, { "d", "hello phone" } });
  m.say(ph, { { "t", "input" }, { "id", id }, { "d", "ls\r" } });
  check("socket: input reaches the process", p->writes == std::vector<std::string>{ "ls\r" });
  { const std::string text = "{\"t\":\"input\",\"id\":\"" + id + "\",\"d\":\"split\"}";
    m.feed(ph, wsFrame(WsText, text.substr(0, 10), false, true, 5) + wsFrame(WsPing, "hb", true, true, 6) + wsFrame(WsCont, text.substr(10), true, true, 7));
    const std::vector<WsEvent> ev = m.heard(ph);
    bool pong = false; for (const WsEvent& e : ev) if (e.kind == WsEvent::Pong && e.data == "hb") pong = true;
    check("socket: a fragmented message is put together; a ping between the fragments is answered", p->writes.back() == "split" && pong); }
  m.feed(ph, wsFrame(WsBinary, "{\"t\":\"input\",\"id\":\"" + id + "\",\"d\":\"bin\"}", true, true, 8));
  m.feed(ph, wsFrame(WsText, "not json {", true, true, 9));
  m.feed(ph, wsFrame(WsText, "[1,2,3]", true, true, 10));
  check("socket: a binary frame holding JSON is read as lib/phone.js reads it; what is not a message is ignored", p->writes.back() == "bin" && p->writes.size() == 3 && !m.listener.closed[ph]);
  check("socket: a call answers on the socket that made it", m.ask(ph, "triggersList")["result"].is_array() && m.ask(ph, "nope")["error"] == "unknown action: nope");
  check("socket: phoneStatus counts it", m.status()["clients"] == 1 && m.status()["desks"] == 0);

  // ---- a desk, the one-desk rule, an eviction
  const uint64_t dk = m.socket("/desk/", "deck_phone=feedfacefeedface");
  m.frames(dk);
  const json dinit = m.ask(dk, "deskInit");
  check("socket: deskInit registers the desk", dinit["result"]["windowIndex"] == 1 && m.status()["desks"] == 1 && m.status()["clients"] == 2, dinit.dump().substr(0, 200));
  const uint64_t dk2 = m.socket("/desk/" + T);
  m.frames(dk2);
  check("socket: one desk at a time", m.ask(dk2, "deskInit")["error"] == "one desk at a time: another browser desk is connected \xE2\x80\x94 close that tab first");
  m.say(dk, { { "t", "layout" }, { "layout", { { id, { 1 } } } } });
  m.deck->handle(m.window, { { "t", "layout" }, { "layout", { { id, { 1, 2 } } } } });
  { const std::vector<json> ev = m.frames(dk); json evict; for (const json& e : ev) if (e["t"] == "event" && e["name"] == "layout:evict") evict = e["payload"];
    check("socket: a window taking a tile the desk holds takes nothing from the desk (the remote-desk rule of 2026-11-18): no layout:evict", evict.is_null() && m.files.getJson(P("C:\\state\\tiles.json"))["deskLayout"] == json{ { id, { 1 } } }, evict.dump()); }

  // ---- the close handshake, and a socket that just goes away
  m.feed(dk, wsFrame(WsClose, std::string("\x03\xe8", 2) + "bye", true, true, 11));
  { std::vector<WsEvent> ev = m.heard(dk);
    check("socket: a close is answered with a close and the connection ends; the desk's window entry goes with it", !ev.empty() && ev.back().kind == WsEvent::Close && ev.back().code == 1000 && m.listener.closed[dk] && m.status()["desks"] == 0 && m.status()["clients"] == 2);
    check("socket: with the first desk gone the second may register", m.ask(dk2, "deskInit")["result"]["windowIndex"] == 1); }
  m.server->onClosed(dk2);
  check("socket: a connection that drops without a close is noticed all the same", m.status()["clients"] == 1 && m.status()["desks"] == 0);
  { const uint64_t bad = m.socket("/" + T); m.frames(bad);
    m.feed(bad, wsFrame(WsText, "{\"t\":\"detach\"}"));           // not masked
    const std::vector<WsEvent> ev = m.heard(bad);
    check("socket: an unmasked frame from a client is a protocol error: closed with 1002", !ev.empty() && ev.back().kind == WsEvent::Close && ev.back().code == 1002 && m.listener.closed[bad] && m.status()["clients"] == 1); }

  // ---- the 4 MB skip rule
  { m.say(ph, { { "t", "attach" }, { "id", id } }); m.frames(ph);
    m.listener.pending[ph] = 4 * 1024 * 1024 + 1;
    m.deck->onPtyData(p->token, "dropped", 7);
    m.deck->handle(m.window, { { "t", "call" }, { "rid", 60 }, { "fn", "renameTile" }, { "args", { id, "while slow" } } });
    m.advance(2000);
    check("skip: a client with more than 4 MB waiting is sent nothing more — neither output nor meta", m.listener.sent[ph].empty());
    m.listener.pending[ph] = 4 * 1024 * 1024;
    m.advance(2000);
    const std::vector<json> after = m.frames(ph);
    check("skip: when it has caught up, the meta it missed comes at the next tick", Net::find(after, "meta")["meta"]["tiles"][0]["title"] == "while slow" && Net::find(after, "data").is_null());
    m.say(ph, { { "t", "attach" }, { "id", id } });
    check("skip: and its next attach replays what it lost", str(Net::find(m.frames(ph), "data")["d"]).find("dropped") != std::string::npos); }

  // ---- the heartbeat
  { const std::vector<WsEvent> none = m.heard(ph);
    m.advance(14000 - std::fmod(m.clock.t, 1.0));
    size_t pings = 0; std::vector<WsEvent> ev;
    auto listen = [&] { ev = m.heard(ph); for (const WsEvent& e : ev) if (e.kind == WsEvent::Ping) { pings++; m.feed(ph, wsFrame(WsPong, e.data, true, true, 3)); } };
    for (int i = 0; i < 70; i++) { m.advance(1000); listen(); }
    check("heartbeat: a ping every 15 s; a client that answers stays", pings >= 4 && pings <= 5 && m.status()["clients"] == 1, std::to_string(pings));
    pings = 0; int droppedAfter = -1;
    for (int i = 0; i < 60 && droppedAfter < 0; i++) { m.advance(1000); ev = m.heard(ph); for (const WsEvent& e : ev) if (e.kind == WsEvent::Ping) pings++; if (m.listener.closed[ph]) droppedAfter = i + 1; }
    check("heartbeat: two pings unanswered and the third beat drops it — the deck no longer counts it", pings == 2 && droppedAfter > 30 && droppedAfter <= 46 && m.status()["clients"] == 0 && m.deck->remoteCount() == 0, std::to_string(pings) + " pings, dropped after " + std::to_string(droppedAfter) + " s");
    (void)none; }
  // ---- the server stopping
  { const uint64_t c = m.socket("/" + T); m.frames(c);
    m.server->stop();
    check("stop: every socket is told (a close frame), the port is freed, the deck has no remote", m.listener.sent[c] == wsCloseFrame(0) && !m.listener.listening && m.deck->remoteCount() == 0 && !m.server->running()); }
  m.deck->shutdown();
}

// ---------------------------------------------------------------- the heartbeat and the desk slot (lib/phone.js, 2026-11-18)
// test/desk/heartbeat-unit.js on the review branch, case for case, on the fake listener and the fake clock: a
// client that stops answering pings is dropped (any message counts as an answer); a new desk takes the slot from
// one that is connected but silent for 10 s and then lets a 3 s ping go unanswered; a desk that answers keeps it.
struct Wire {
  Net& m;
  std::map<uint64_t, std::vector<json>> msgs; std::map<uint64_t, int> pings; std::set<uint64_t> answers;
  explicit Wire(Net& n) : m(n) {}
  uint64_t open(const std::string& path, bool answersPings) { const uint64_t c = m.socket(path + "?t=feedfacefeedface"); if (answersPings) answers.insert(c); listen(); return c; }
  void listen() {
    for (auto& kv : m.dec) { const uint64_t c = kv.first; if (m.listener.closed[c]) continue;
      for (const WsEvent& e : m.heard(c)) { if (e.kind == WsEvent::Message) msgs[c].push_back(json::parse(e.data, nullptr, false)); if (e.kind == WsEvent::Ping) { pings[c]++; if (answers.count(c)) m.feed(c, wsFrame(WsPong, e.data, true, true, 3)); } } }
  }
  void run(double ms) { for (double left = ms; left > 0; left -= 50) { m.advance(std::min(50.0, left)); listen(); } }
  int rid = 900;
  int call(uint64_t c, const std::string& fn, json args = json::array()) { m.say(c, { { "t", "call" }, { "rid", ++rid }, { "fn", fn }, { "args", args } }); listen(); return rid; }
  json answer(uint64_t c, int id) { for (const json& f : msgs[c]) if (f.is_object() && (f["t"] == "ok" || f["t"] == "err") && f["rid"] == id) return f; return json(); }
};
void testDeskSlot(const std::string& repo) {
  const json phone = { { "token", "feedfacefeedface" } };
  const char* const ONE = "one desk at a time: another browser desk is connected \xE2\x80\x94 close that tab first";
  // ---- the heartbeat: a message is as good as a pong
  { Net m(repo, 8811, phone); Wire w(m);
    const uint64_t alive = w.open("/", true), dead = w.open("/", false), quiet = w.open("/", false);
    for (int i = 0; i < 12; i++) { w.run(5000); if (i < 8) m.say(quiet, { { "t", "detach" } }); }      // quiet talks for 40 s, then stops
    check("heartbeat: a client that answers stays; one that missed two pings is dropped at the third beat", !m.listener.closed[alive] && m.listener.closed[dead] && w.pings[dead] == 2 && w.pings[alive] >= 3, std::to_string(w.pings[dead]));
    check("heartbeat: any message counts as being there (a client that talks is not dropped for a lost pong)", !m.listener.closed[quiet] && m.deck->remoteCount() == 2, std::to_string(m.deck->remoteCount()));
    w.run(50000);
    check("heartbeat: ...until it goes quiet too", m.listener.closed[quiet] && m.deck->remoteCount() == 1);
    m.deck->shutdown(); }
  // ---- a dead phone lets go of its tile
  { Net m(repo, 8811, phone); Wire w(m);
    const uint64_t ph = w.open("/", false);
    const int made = w.call(ph, "newTile", json::array({ { { "kind", "shell" } } }));
    const std::string id = str(w.answer(ph, made)["result"]);
    m.say(ph, { { "t", "attach" }, { "id", id } });
    auto shownBy = [&] { const json meta = m.deck->collectMeta(); for (const json& t : meta["tiles"]) if (t["id"] == id) return json(t["shownBy"]); return json(); };
    check("heartbeat: a phone attached to a tile is watching it", shownBy() == json::array({ "remote" }), shownBy().dump());
    w.run(50000);
    check("heartbeat: a phone whose connection died stops counting as watching", m.listener.closed[ph] && shownBy() == json::array() && m.deck->remoteCount() == 0);
    m.deck->shutdown(); }
  // ---- the desk slot: a living desk keeps it
  { Net m(repo, 8811, phone); Wire w(m);
    const uint64_t first = w.open("/desk/ws", true), second = w.open("/desk/ws", true);
    const int r1 = w.call(first, "deskInit", json::array({ { { "name", "OFFICE-LAPTOP" }, { "kind", "native" } } }));
    check("desk slot: the first desk gets it", w.answer(first, r1)["t"] == "ok" && m.status()["desks"] == 1);
    const int r2 = w.call(second, "deskInit");
    check("desk slot: a second desk is refused at once while the first was just heard from (nobody is pinged)", w.answer(second, r2)["error"] == ONE && w.pings[first] == 0);
    w.run(5000); m.say(first, { { "t", "shown" }, { "ids", json::array() }, { "hidden", false } }); w.run(6000);
    const int r2b = w.call(second, "deskInit");
    check("desk slot: a desk that keeps talking is never 'silent'", w.answer(second, r2b)["error"] == ONE && w.pings[first] == 0);
    w.run(15000);                                    // 26 s: it answered the heartbeat at 15 s and has said nothing since
    const int before = w.pings[first];
    const int r3 = w.call(second, "deskInit");
    check("desk slot: the first is silent now, so it is asked: the answer waits for the probe", w.answer(second, r3).is_null() && w.pings[first] == before + 1, std::to_string(before));
    w.run(400);
    check("desk slot: a quiet but living desk answers the probe and keeps the slot", w.answer(second, r3)["error"] == ONE && !m.listener.closed[first] && m.status()["desks"] == 1 && m.status()["clients"] == 2, w.answer(second, r3).dump());
    m.deck->shutdown(); }
  // ---- the desk slot: a dead desk loses it
  { Net m(repo, 8811, phone, [](DeckOptions& o) { o.driverClaim = true; }); Wire w(m);
    const uint64_t dead = w.open("/desk/ws", false);
    const int r1 = w.call(dead, "deskInit", json::array({ { { "name", "OLD" } } }));
    const uint64_t next = w.open("/desk/ws", true);
    const int early = w.call(next, "deskInit");
    check("desk slot: a desk that died a moment ago still holds the slot (not yet silent long enough)", w.answer(dead, r1)["t"] == "ok" && w.answer(next, early)["error"] == ONE);
    w.run(10500);
    const double t0 = m.clock.t;
    const int r2 = w.call(next, "deskInit", json::array({ { { "name", "HOME-PC" }, { "kind", "native" } } }));
    w.run(2800);
    check("desk slot: the new desk waits for the probe (3 s)", w.answer(next, r2).is_null() && !m.listener.closed[dead] && w.pings[dead] == 1, std::to_string(w.pings[dead]));
    double answeredAt = 0;
    for (int i = 0; i < 40 && w.answer(next, r2).is_null(); i++) { w.run(50); answeredAt = m.clock.t; }
    const json ok = w.answer(next, r2);
    check("desk slot: a new desk takes over from one that is connected but silent", ok["t"] == "ok" && ok["result"]["windowIndex"] == 1 && answeredAt - t0 >= 3000 && answeredAt - t0 < 3400, ok.dump().substr(0, 160) + " after " + std::to_string(answeredAt - t0));
    check("desk slot: the dead desk was terminated and its window entry closed; one desk is connected, the new one", m.listener.closed[dead] && m.status()["desks"] == 1 && m.status()["clients"] == 1 && m.deck->collectMeta()["remoteDesk"] == json{ { "name", "HOME-PC" }, { "kind", "native" } },
      m.deck->collectMeta()["remoteDesk"].dump());
    // an asker that leaves while it waits changes nothing
    const uint64_t third = w.open("/desk/ws", true);
    w.answers.erase(next);                           // the holder goes silent
    w.run(10500);
    const int r3 = w.call(third, "deskInit");
    m.server->onClosed(third);
    w.run(4000);
    check("desk slot: an asker that left while it waited takes nothing", w.answer(third, r3).is_null() && m.status()["desks"] == 1 && !m.listener.closed[next]);
    m.deck->shutdown(); }
  // ---- the two times are options
  { Net m(repo, 8811, phone, [](DeckOptions& o) { o.deskSilentMs = 60; o.deskProbeMs = 80; }); Wire w(m);
    const uint64_t dead = w.open("/desk/ws", false);
    w.call(dead, "deskInit");
    const uint64_t next = w.open("/desk/ws", true);
    w.run(100);
    const int r = w.call(next, "deskInit");
    w.run(200);
    check("desk slot: the two times are options (a test shortens them, as the branch's does)", w.answer(next, r)["t"] == "ok" && m.listener.closed[dead]);
    m.deck->shutdown(); }
  // ---- the pages: found, or said to be missing
  { Net m(repo + P("\\no-such-folder"), 8811, phone);
    const json st = m.status();
    check("serve: an asset root without the pages says so (assets 'missing' + the folder that was tried)", st["assets"] == "missing" && st["assetRoot"] == repo + P("\\no-such-folder") && st["running"] == true && m.http("GET", "/?t=feedfacefeedface").status == 404, st.dump());
    m.deck->shutdown(); }
  { Net m("", 8811, phone);
    check("serve: no asset root at all: assets 'missing', assetRoot \"\"", m.status()["assets"] == "missing" && m.status()["assetRoot"] == "");
    m.deck->shutdown(); }
}

}  // namespace

void testServer(const std::string& fx, const std::string& repo) {
  if (!want("server")) return;
  try { testBind(repo); testRoutes(fx, repo); testDeskSlot(repo); }
  catch (const std::exception& e) { const std::string after = lastCheck; check("server: ran to its end", false, std::string(e.what()) + " — after: " + after); }
}
