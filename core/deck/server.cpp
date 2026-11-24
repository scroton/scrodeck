// core/deck/server.cpp — see server.hpp. lib/phone.js, request by request.
#include "server.hpp"
#include "hash.hpp"
#include "remote.hpp"
#include "util.hpp"
#include "viewer.hpp"
#include "edition.hpp"
#include "../session/jsutil.hpp"

#include <algorithm>

namespace deck { namespace core {

namespace js = deck::session::js;
using Headers = std::vector<std::pair<std::string, std::string>>;
static const char* const COOKIE = "deck_phone";

const char* const kUnauthorizedPage =
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><body style=\"font:16px system-ui;background:#0f0f0f;color:#ccc;padding:24px\">"
  DECK_EDITION_PRODUCT ": this link needs its token. Open the link shown under Settings &rarr; Phone on the desk once (it carries the token); after that the app remembers it. "
  "The token changes only when you press &ldquo;new token&rdquo; there.</body>";
const char* const kAskCsp = "sandbox allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads; default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:";

std::string setCookieHeader(const std::string& token) { return std::string(COOKIE) + "=" + encodeURIComponent(token) + "; Path=/; Max-Age=31536000; SameSite=Lax; HttpOnly"; }

const char* contentType(const std::string& ext) {
  static const std::pair<const char*, const char*> T[] = {
    { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" }, { ".css", "text/css; charset=utf-8" }, { ".png", "image/png" },
    { ".webmanifest", "application/manifest+json" }, { ".json", "application/json" }, { ".svg", "image/svg+xml" }, { ".woff2", "font/woff2" }, { ".ttf", "font/ttf" } };
  for (const auto& kv : T) if (ext == kv.first) return kv.second;
  return "application/octet-stream";
}

// ---------------------------------------------------------------- which address (lib/phone.js tailscaleAddress + start)
std::string tailscaleAddress(const std::vector<std::string>& ipv4) {
  for (const std::string& a : ipv4) {
    int p[4] = { -1, -1, -1, -1 }; int n = 0; size_t at = 0; bool ok = true;
    while (n < 4 && at <= a.size()) {
      size_t dot = a.find('.', at); if (dot == std::string::npos) dot = a.size();
      if (dot == at) { ok = false; break; }
      int v = 0; for (size_t k = at; k < dot; k++) { if (a[k] < '0' || a[k] > '9') ok = false; else v = v * 10 + (a[k] - '0'); }
      p[n++] = v; at = dot + 1;
    }
    if (ok && n == 4 && p[0] == 100 && p[1] >= 64 && p[1] <= 127) return a;
  }
  return std::string();
}
std::string bindHost(const std::string& bind, const std::vector<std::string>& ipv4, std::string* error) {
  // 'localhost' is the tightest of the three: the port is on no network at all, and only `tailscale serve` reaches it
  if (bind == "all") return "0.0.0.0";
  if (bind == "localhost") return "127.0.0.1";
  const std::string ts = tailscaleAddress(ipv4);
  if (ts.empty() && error) *error = "no Tailscale address on this machine (is Tailscale running?)";
  return ts;
}
std::string serveUrlFromStatus(const std::string& statusJson, int port) {
  const nlohmann::ordered_json j = nlohmann::ordered_json::parse(statusJson, nullptr, false);
  if (!j.is_object() || !j.contains("Web") || !j["Web"].is_object()) return std::string();
  const std::string p = std::to_string(port);
  auto wanted = [&](const std::string& proxy) {     // ^https?://(localhost|127\.0\.0\.1):<port>/?$
    for (const char* scheme : { "http://", "https://" }) for (const char* host : { "localhost", "127.0.0.1" }) {
      const std::string base = std::string(scheme) + host + ":" + p;
      if (proxy == base || proxy == base + "/") return true;
    }
    return false;
  };
  std::string url;
  for (auto site = j["Web"].begin(); site != j["Web"].end(); ++site) {
    if (!site.value().is_object() || !site.value().contains("Handlers") || !site.value()["Handlers"].is_object()) continue;
    const auto& handlers = site.value()["Handlers"];
    for (auto h = handlers.begin(); h != handlers.end(); ++h) {
      if (!h.value().is_object() || !h.value().contains("Proxy") || !h.value()["Proxy"].is_string() || !wanted(h.value()["Proxy"].get<std::string>())) continue;
      const std::string hostPort = site.key(), path = h.key();
      const size_t i = hostPort.rfind(':');
      const bool split = i != std::string::npos && i > 0;
      const std::string host = split ? hostPort.substr(0, i) : hostPort, hp = split ? hostPort.substr(i + 1) : std::string();
      url = "https://" + host + (!hp.empty() && hp != "443" ? ":" + hp : std::string()) + (!path.empty() && path.back() == '/' ? path : path + "/");
    }
  }
  return url;
}

// ---------------------------------------------------------------- the desk page (lib/phone.js deskPage / deskFile)
std::string deskPage(const std::string& indexHtml) {
  std::string html = indexHtml;
  // (<meta http-equiv="Content-Security-Policy" content="[^"]*)"  ->  $1 + connect-src (unless it has one) + "
  const std::string open = "<meta http-equiv=\"Content-Security-Policy\" content=\"";
  const size_t m = html.find(open);
  if (m != std::string::npos) {
    const size_t q = html.find('"', m + open.size());
    if (q != std::string::npos) {
      const std::string a = html.substr(m, q - m);
      if (a.find("connect-src") == std::string::npos) html.insert(q, "; connect-src 'self' ws: wss:");
    }
  }
  const size_t t = html.find("</title>");
  if (t != std::string::npos) html.insert(t + 8, "\n  <base href=\"/desk/renderer/\">\n  <script src=\"/desk/renderer/desk/bridge.js\"></script>");
  return html;
}
static bool wordish(char c, const char* extra) { if (js::isWordChar((unsigned char)c)) return true; for (; *extra; extra++) if (c == *extra) return true; return false; }
bool deskFileAllowed(const std::string& rel) {
  if (rel == "renderer/style.css" || rel == "renderer/desk/bridge.js" || rel == "lib/layout.js") return true;
  if (rel == "node_modules/@xterm/xterm/lib/xterm.js" || rel == "node_modules/@xterm/xterm/css/xterm.css" || rel == "node_modules/@xterm/addon-fit/lib/addon-fit.js") return true;
  // ^renderer\/js\/[\w.-]+\.js$
  if (rel.compare(0, 12, "renderer/js/") == 0 && rel.size() > 15 && rel.compare(rel.size() - 3, 3, ".js") == 0) {
    bool ok = true;
    for (size_t i = 12; i < rel.size() - 3; i++) if (!wordish(rel[i], ".-")) ok = false;
    if (ok) return true;
  }
  // ^assets\/fonts\/([\w.-]+\/)?[\w.,\[\]-]+\.(css|ttf|woff2)$
  if (rel.compare(0, 13, "assets/fonts/") == 0) {
    std::string rest = rel.substr(13);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
      if (slash == 0) return false;
      for (size_t i = 0; i < slash; i++) if (!wordish(rest[i], ".-")) return false;
      rest = rest.substr(slash + 1);
    }
    size_t extLen = 0;
    for (const char* e : { ".css", ".ttf", ".woff2" }) { const size_t n = std::char_traits<char>::length(e); if (rest.size() > n && rest.compare(rest.size() - n, n, e) == 0) extLen = n; }
    if (!extLen) return false;
    for (size_t i = 0; i < rest.size() - extLen; i++) if (!wordish(rest[i], ".,[]-")) return false;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- the server
Server::Server(ServerOptions options, Files& files, Clock& clock, Listener& listener, Net& net)
  : opt_(std::move(options)), files_(files), clock_(clock), listener_(listener), net_(net) {}

std::string Server::join(std::initializer_list<const char*> parts) const {
  std::string p = opt_.assetRoot;
  for (const char* x : parts) p = joinPath(p, x);
  return p;
}
size_t Server::sockets() const { size_t n = 0; for (const auto& kv : conns_) if (kv.second.ws) n++; return n; }
// The key in a viewer path: derived from the token, so it needs no storage and a new token retires it
std::string Server::pageKey() const { return hexOf(hmacSha256(token_, "deck pages")).substr(0, 16); }

json Server::extras() {
  // assetsFingerprint: four stats; currentBuild: the BUILD string compiled into the page's own source
  std::string assets;
  for (const char* f : { "index.html", "app.js", "style.css", "sw.js" }) {
    const FileStat st = files_.stat(join({ "renderer", "phone", f }));
    if (st.exists && !st.isDir) assets += std::string(f) + ":" + json(st.mtimeMs).dump() + ":" + std::to_string(st.size) + ";";
    else assets += std::string(f) + ":none;";
  }
  json build;
  std::string src;
  if (files_.readRange(join({ "renderer", "phone", "app.js" }), 0, 16000, src)) {
    src = js::sliceUnits(src, 4000);
    const std::string mark = "const BUILD = '";
    const size_t a = src.find(mark);
    if (a != std::string::npos) { const size_t b = src.find('\'', a + mark.size()); if (b != std::string::npos && b > a + mark.size()) build = src.substr(a + mark.size(), b - a - mark.size()); }
  }
  return { { "assets", assets }, { "build", build }, { "pageKey", pageKey() } };
}

std::string Server::serveUrl(int port) {
  const double at = clock_.nowMs();
  if (serveCache_.has && serveCache_.port == port && at - serveCache_.at < 60000) return serveCache_.url;
  serveCache_.has = true; serveCache_.at = at; serveCache_.port = port;
  serveCache_.url = serveUrlFromStatus(net_.tailscaleServeStatus(), port);
  return serveCache_.url;
}

// main.js phoneStatus()
json Server::status(const json& p) {
  const bool forced = opt_.forcePort > 0;
  const json port = forced ? json(opt_.forcePort) : js::get(p, "port");
  const json bind = forced ? json("localhost") : js::get(p, "bind");
  const json& token = js::get(p, "token");
  const std::string ts = tailscaleAddress(net_.ipv4Addresses());
  const json tsJ = ts.empty() ? json() : json(ts);
  // behind Tailscale Serve (bind localhost) the link a phone can open is the HTTPS front door, not 127.0.0.1.
  // A forced port is a loopback run for a test or a local client: it asks Tailscale nothing.
  std::string serve;
  if (!forced && bind == "localhost" && port.is_number()) serve = serveUrl(port.get<int>());
  json url;
  if (running_) url = !serve.empty() ? serve + "?t=" + js::toString(token) : "http://" + host_ + ":" + std::to_string(port_) + "/?t=" + token_;
  return { { "enabled", forced || js::truthy(js::get(p, "enabled")) }, { "running", running_ }, { "port", port }, { "bind", bind }, { "token", token },
    { "address", running_ ? json(host_) : tsJ }, { "serve", serve.empty() ? json() : json(serve) }, { "url", url },
    { "clients", deck_ ? deck_->remoteCount() : 0 }, { "desks", deck_ ? deck_->deskCount() : 0 }, { "tailscale", tsJ }, { "error", error_.empty() ? json() : json(error_) },
    // NOT in main.js: are the pages this server hands out where it looks for them (an exe copied without its
    // folder serves 404s behind a healthy-looking link). `assetRoot`: the folder that was tried ("" = none found).
    { "assets", pagesFound() ? "ok" : "missing" }, { "assetRoot", opt_.assetRoot } };
}

// the phone page and the desk page, the two a link leads to
bool Server::pagesFound() const {
  if (opt_.assetRoot.empty()) return false;
  return files_.isFile(join({ "renderer", "phone", "index.html" })) && files_.isFile(join({ "renderer", "index.html" }));
}

// main.js syncPhone(): start it, stop it, or restart it on a new port / address / token
void Server::sync(const json& p) {
  error_.clear();
  const bool forced = opt_.forcePort > 0;
  const bool enabled = forced || js::truthy(js::get(p, "enabled"));
  stop();
  if (!enabled) return;
  const std::string token = js::truthy(js::get(p, "token")) ? js::toString(js::get(p, "token")) : std::string();
  if (token.empty()) { error_ = "no token"; return; }
  const std::string bind = forced ? "localhost" : (js::truthy(js::get(p, "bind")) ? js::toString(js::get(p, "bind")) : "tailscale");
  const int port = forced ? opt_.forcePort : (js::get(p, "port").is_number() ? js::get(p, "port").get<int>() : 0);
  std::string err;
  const std::string host = bindHost(bind, bind == "all" || bind == "localhost" ? std::vector<std::string>() : net_.ipv4Addresses(), &err);
  if (host.empty()) { error_ = err; return; }
  if (!listener_.listen(host, port, &err)) { error_ = err.empty() ? "listen failed" : err; return; }
  token_ = token; host_ = host; port_ = port; running_ = true;
}
void Server::stop() {
  if (!running_ && conns_.empty()) return;
  std::vector<ClientId> clients;
  for (auto& kv : conns_) { if (kv.second.ws) listener_.send(kv.first, wsCloseFrame(0)); if (kv.second.client) clients.push_back(kv.second.client); }
  listener_.stop();
  conns_.clear(); byClient_.clear();
  running_ = false;
  if (deck_) for (ClientId c : clients) deck_->removeClient(c);
}

std::string Server::authed(const HttpRequest& req, const Url& url) const {
  if (url.has("t") && safeEqual(url.get("t"), token_)) return "query";
  for (const auto& kv : req.headers) if (kv.first == "cookie") for (const std::string& v : cookieValues(kv.second, COOKIE)) if (safeEqual(v, token_)) return "cookie";
  return std::string();
}

void Server::respond(Conn& c, const HttpRequest& req, int status, Headers headers, const std::string& body) {
  const bool close = !req.keepAlive();
  listener_.send(c.id, writeResponse(status, headers, body, req.method == "HEAD", close));
  if (close) { c.closing = true; listener_.close(c.id, true); }
}
void Server::serveFile(Conn& c, const HttpRequest& req, const std::string& file, Headers headers, const char* cacheLong) {
  std::string buf;
  const FileStat st = files_.stat(file);
  if (!st.exists || st.isDir || !files_.read(file, buf)) { respond(c, req, 404, headers, "not found"); return; }
  const std::string ext = extName(file);
  headers.emplace_back("content-type", contentType(ext));
  // the page and the worker must never be cached: a redeploy has to win immediately
  const bool own = ext == ".html" || ext == ".js" || ext == ".css" || (ext == ".webmanifest" && std::string(cacheLong) == "max-age=300");
  headers.emplace_back("cache-control", own ? "no-cache" : cacheLong);
  respond(c, req, 200, headers, buf);
}

void Server::deskRequest(Conn& c, const HttpRequest& req, const std::string& pathname, Headers headers) {
  if (req.method != "GET" && req.method != "HEAD") { respond(c, req, 405, headers, ""); return; }
  std::string rel = pathname.substr(5);                       // .replace(/^\/desk\/?/, '')
  if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
  if (rel.empty() || rel == "index.html" || rel == "renderer/" || rel == "renderer/index.html") {
    std::string html;
    if (!files_.read(join({ "renderer", "index.html" }), html)) { respond(c, req, 500, headers, "desk page: renderer/index.html could not be read"); return; }
    headers.emplace_back("content-type", "text/html; charset=utf-8");
    headers.emplace_back("cache-control", "no-cache");
    respond(c, req, 200, headers, deskPage(html));
    return;
  }
  const std::string norm = posixNormalize(rel);
  if (!deskFileAllowed(norm)) { respond(c, req, 404, headers, "not found"); return; }
  std::string file = opt_.assetRoot;
  for (size_t a = 0;;) { const size_t b = norm.find('/', a); file = joinPath(file, norm.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
  serveFile(c, req, file, headers, "max-age=3600");            // the fonts are big and never change
}

void Server::onRequest(Conn& c, const HttpRequest& req) {
  const Url url = parseUrl(req.target);
  // The page viewer (lib/viewer.js): /view/<page key>/<folder id>/<file, or a path below the folder>. The deck
  // decides from the folder id it registered, never from a path in the request. An .html file is served SANDBOXED
  // (its own opaque origin), so it cannot reach this app. The key rides in the path rather than the cookie: what
  // the framed page asks for comes from that opaque origin, which never carries the SameSite cookie. It opens
  // the viewer and nothing else, and changes with the token.
  if (url.pathname.compare(0, 6, "/view/") == 0) {
    auto no = [&](int code) { respond(c, req, code, { { "content-type", "text/plain; charset=utf-8" } }, code == 404 ? "not found" : ""); };
    if (req.method != "GET" && req.method != "HEAD") { no(405); return; }
    // ^\/view\/([0-9a-f]{16})\/([0-9a-f]{16})\/(.+)$
    const std::string& p = url.pathname;
    auto hex16 = [&](size_t at) { if (p.size() < at + 16) return false; for (size_t k = at; k < at + 16; k++) if (!((p[k] >= '0' && p[k] <= '9') || (p[k] >= 'a' && p[k] <= 'f'))) return false; return true; };
    const bool shaped = hex16(6) && p.size() > 40 && p[22] == '/' && hex16(23) && p[39] == '/';
    if (!shaped || token_.empty() || !safeEqual(p.substr(6, 16), pageKey())) { no(404); return; }
    std::vector<std::string> parts; bool ok = true;
    for (size_t a = 40;;) {
      const size_t b = p.find('/', a);
      std::string part;
      if (!percentDecode(p.substr(a, b == std::string::npos ? b : b - a), part)) ok = false;
      parts.push_back(part);
      if (b == std::string::npos) break;
      a = b + 1;
    }
    const std::string file = ok && deck_ ? deck_->viewFile(p.substr(23, 16), parts) : std::string();
    std::string buf;
    if (file.empty() || !files_.read(file, buf)) { no(404); return; }
    const std::string ext = extName(file);
    const bool html = ext == ".html" || ext == ".htm";
    Headers head = { { "content-type", viewContentType(ext) }, { "cache-control", "no-cache" }, { "referrer-policy", "no-referrer" }, { "x-content-type-options", "nosniff" } };
    if (html) head.emplace_back("content-security-policy", kViewCsp);
    respond(c, req, 200, head, html ? withPageLinks(buf) : buf);
    return;
  }
  std::string decoded;
  if (!percentDecode(url.pathname, decoded)) { respond(c, req, 400, {}, "bad request"); return; }
  std::string how = authed(req, url);
  if (how.empty() && (decoded == "/manifest.webmanifest" || decoded == "/icon-192.png" || decoded == "/icon-512.png")) how = "public";
  if (how.empty()) { respond(c, req, 401, { { "content-type", "text/html; charset=utf-8" } }, kUnauthorizedPage); return; }
  Headers headers;
  // the first visit carries ?t=…; park it in a cookie so the installed app's start_url (a bare "/") stays authorised
  if (how == "query") headers.emplace_back("set-cookie", setCookieHeader(token_));

  // a file from the phone, for the session: the body is the file, the name rides in a header
  if (req.method == "POST" && url.pathname == "/upload" && how != "public") {
    if (!deck_) { respond(c, req, 501, headers, "no upload"); return; }
    std::string name;
    const std::string& raw = req.header("x-file-name");
    if (!percentDecode(raw.empty() ? "file" : raw, name)) { respond(c, req, 500, headers, "URI malformed"); return; }
    try {
      const std::string p = deck_->saveUpload(name, req.body);
      headers.emplace_back("content-type", "application/json");
      respond(c, req, 200, headers, json{ { "path", p } }.dump(-1, ' ', false, json::error_handler_t::replace));
    } catch (const std::exception& e) { respond(c, req, 500, headers, e.what()); }
    return;
  }
  // An ask page (the ask-page skill; lib/asks.js), for the phone's ask screen and a desk's ask pane:
  // /ask/<page id>.html, only a page that some session's ask-page state names, never a path. Served SANDBOXED:
  // the page runs as its own opaque origin, and talks to the screen that frames it by postMessage only.
  if (url.pathname.compare(0, 9, "/ask/ask_") == 0 && url.pathname.size() > 14 && url.pathname.compare(url.pathname.size() - 5, 5, ".html") == 0) {
    bool idOk = true;
    for (size_t i = 9; i < url.pathname.size() - 5; i++) if (!wordish(url.pathname[i], "-")) idOk = false;
    if (idOk) {
      if (how == "public" || (req.method != "GET" && req.method != "HEAD")) { respond(c, req, 403, headers, ""); return; }
      const std::string file = deck_ ? deck_->askPage(url.pathname.substr(5, url.pathname.size() - 10)) : std::string();
      if (file.empty()) { respond(c, req, 404, headers, "no such ask page"); return; }
      std::string buf;
      if (!files_.read(file, buf)) { respond(c, req, 404, headers, "not found"); return; }
      headers.emplace_back("content-type", "text/html; charset=utf-8");
      headers.emplace_back("cache-control", "no-cache");
      headers.emplace_back("content-security-policy", kAskCsp);
      respond(c, req, 200, headers, buf);
      return;
    }
  }
  std::string pathname = decoded;
  // the browser desk: the desktop renderer under /desk/, same token and cookie as the phone page
  if (pathname == "/desk" || pathname.compare(0, 6, "/desk/") == 0) { deskRequest(c, req, pathname, headers); return; }
  if (pathname == "/" || pathname.empty()) pathname = "/index.html";
  if (pathname == "/vendor/xterm.js") { serveFile(c, req, join({ "node_modules", "@xterm", "xterm", "lib", "xterm.js" }), headers, "max-age=300"); return; }
  if (pathname == "/vendor/xterm.css") { serveFile(c, req, join({ "node_modules", "@xterm", "xterm", "css", "xterm.css" }), headers, "max-age=300"); return; }
  if (pathname == "/vendor/addon-fit.js") { serveFile(c, req, join({ "node_modules", "@xterm", "addon-fit", "lib", "addon-fit.js" }), headers, "max-age=300"); return; }
  // never serve outside the page's folder
  std::vector<std::string> parts;
  if (!safeRelative(pathname, parts)) { respond(c, req, 403, headers, ""); return; }
  std::string file = join({ "renderer", "phone" });
  for (const std::string& p : parts) file = joinPath(file, p);
  serveFile(c, req, file, headers, "max-age=300");
}

void Server::upgrade(Conn& c, const HttpRequest& req) {
  const Url url = parseUrl(req.target);
  if (authed(req, url).empty()) { listener_.send(c.id, "HTTP/1.1 401 Unauthorized\r\n\r\n"); c.closing = true; listener_.close(c.id, true); return; }
  const WsHandshake hs = wsHandshake(req);
  listener_.send(c.id, hs.response);
  if (hs.status != 101) { c.closing = true; listener_.close(c.id, true); return; }
  c.ws = true; c.unanswered = 0; c.nextPingAt = clock_.nowMs() + opt_.pingEveryMs; c.heardAt = clock_.nowMs();
  if (!deck_) return;
  const uint64_t id = c.id;
  adopting_ = id; expecting_ = deck_->nextClientId();   // the hello is delivered from inside addRemote, to exactly that id
  const ClientId client = deck_->addRemote(url.pathname.compare(0, 6, "/desk/") == 0);   // (lib/phone.js viaDesk: /^\/desk\//)
  adopting_ = 0; expecting_ = 0;
  auto it = conns_.find(id);
  if (it == conns_.end()) { deck_->removeClient(client); return; }
  it->second.client = client; byClient_[client] = id;
}

bool Server::deliver(ClientId client, const json& frame) {
  auto it = byClient_.find(client);
  if (it == byClient_.end()) {
    if (!adopting_ || client != expecting_ || conns_.find(adopting_) == conns_.end()) return false;   // a client nobody here knows: not the newcomer
    byClient_[client] = adopting_; conns_[adopting_].client = client;
    it = byClient_.find(client);
  }
  auto cit = conns_.find(it->second);
  if (cit == conns_.end() || cit->second.closing) return false;
  if (listener_.buffered(it->second) > kMaxBuffered) return false;      // a slow client: skip rather than queue forever
  listener_.send(it->second, wsFrame(WsText, frame.dump(-1, ' ', false, json::error_handler_t::replace)));
  return true;
}

void Server::drop(uint64_t conn) {
  auto it = conns_.find(conn);
  if (it == conns_.end()) return;
  const ClientId client = it->second.client;
  conns_.erase(it);
  if (client) { byClient_.erase(client); if (deck_) deck_->removeClient(client); }
}

void Server::onFrames(Conn& c, const char* bytes, size_t n) {
  const uint64_t id = c.id;
  std::vector<WsEvent> events;
  c.dec.feed(bytes, n, events);
  for (const WsEvent& e : events) {
    auto it = conns_.find(id);
    if (it == conns_.end() || it->second.closing) return;
    Conn& cc = it->second;
    switch (e.kind) {
      case WsEvent::Message: {
        // lib/phone.js: `try { m = JSON.parse(raw) } catch { return }` — a text or a binary frame alike
        // any message counts as being there, readable or not: a client that talks is not dropped for a lost pong
        // (lib/phone.js heard, before the parse)
        cc.unanswered = 0; cc.heardAt = clock_.nowMs();
        const json m = json::parse(e.data, nullptr, false);
        if (!m.is_discarded() && deck_ && cc.client) deck_->handle(cc.client, m);
        break; }
      case WsEvent::Ping: listener_.send(id, wsFrame(WsPong, e.data)); break;
      case WsEvent::Pong: cc.unanswered = 0; cc.heardAt = clock_.nowMs(); break;
      case WsEvent::Close:
        listener_.send(id, wsCloseFrame(e.code == 1005 ? 0 : e.code));
        cc.closing = true; listener_.close(id, true); drop(id);
        return;
      case WsEvent::Error:
        listener_.send(id, wsCloseFrame(e.code, e.data));
        cc.closing = true; listener_.close(id, true); drop(id);
        return;
    }
  }
}

// ---- the desk slot's three questions (lib/phone.js heard / probe / drop; PhoneHost)
double Server::heardAt(ClientId client) {
  auto it = byClient_.find(client);
  if (it == byClient_.end()) return -1;
  auto c = conns_.find(it->second);
  return c == conns_.end() ? -1 : c->second.heardAt;
}
bool Server::ping(ClientId client) {
  auto it = byClient_.find(client);
  if (it == byClient_.end()) return false;
  auto c = conns_.find(it->second);
  if (c == conns_.end() || c->second.closing || !c->second.ws) return false;
  listener_.send(it->second, wsFrame(WsPing, ""));
  return true;
}
// terminate, not close: there is nobody to answer a close frame
void Server::drop(ClientId client) {
  auto it = byClient_.find(client);
  if (it == byClient_.end()) return;
  const uint64_t conn = it->second;
  listener_.close(conn, false);
  drop(conn);
}

void Server::onAccept(uint64_t conn) { Conn c; c.id = conn; conns_[conn] = std::move(c); }
void Server::onClosed(uint64_t conn) { drop(conn); }
void Server::onBytes(uint64_t conn, const char* bytes, size_t n) {
  auto it = conns_.find(conn);
  if (it == conns_.end() || it->second.closing) return;
  if (it->second.ws) { onFrames(it->second, bytes, n); return; }
  RequestReader::Result r = it->second.http.feed(bytes, n);
  for (;;) {
    it = conns_.find(conn);
    if (it == conns_.end() || it->second.closing) return;
    Conn& c = it->second;
    if (r == RequestReader::More) return;
    if (r == RequestReader::Bad || r == RequestReader::TooLarge) {
      // an upload over the limit is cut off (lib/phone.js destroys the request); anything else unreadable is a 400
      const int status = r == RequestReader::Bad ? 400 : (c.http.request().method.empty() ? 431 : 413);
      listener_.send(conn, writeResponse(status, {}, statusText(status), false, true));
      c.closing = true; listener_.close(conn, true);
      return;
    }
    const HttpRequest req = c.http.request();
    if (req.wantsUpgrade()) {
      upgrade(c, req);
      it = conns_.find(conn);
      if (it == conns_.end() || !it->second.ws) return;
      const std::string rest = it->second.http.takeRest();
      if (!rest.empty()) onFrames(it->second, rest.data(), rest.size());
      return;
    }
    onRequest(c, req);
    it = conns_.find(conn);
    if (it == conns_.end() || it->second.closing) return;
    it->second.http.next();
    r = it->second.http.poll();
  }
}

// The heartbeat: a ping every 15 s; a connection that left two unanswered is dropped (a phone that went out of
// range without closing would otherwise count as attached — and as the driver — for ever).
void Server::tick() {
  if (!running_) return;
  const double at = clock_.nowMs();
  std::vector<uint64_t> due;
  for (const auto& kv : conns_) if (kv.second.ws && !kv.second.closing && at >= kv.second.nextPingAt) due.push_back(kv.first);
  for (uint64_t id : due) {
    auto it = conns_.find(id);
    if (it == conns_.end()) continue;
    Conn& c = it->second;
    if (c.unanswered >= opt_.missedPings) { listener_.close(id, false); drop(id); continue; }
    c.unanswered++; c.nextPingAt = at + opt_.pingEveryMs;
    listener_.send(id, wsFrame(WsPing, ""));
  }
}

}}  // namespace deck::core
