// core/deck/server.hpp — the deck as the thing a phone or a remote desk connects to: lib/phone.js PhoneServer
// plus main.js syncPhone / phoneStatus, as pure byte-in / byte-out C++. The shell supplies a Listener (sockets),
// a Net (which addresses this machine has; `tailscale serve status`) and Files (the pages); everything else —
// the request parsing, the token and cookie, the routes and their headers, the websocket handshake, frames,
// heartbeat and close, the 4 MB skip rule, which address to bind — is here, shared with the Swift shell.
//
//   bind     settings.phone.bind, exactly lib/phone.js's three: `tailscale` (the default: this machine's address in
//            100.64.0.0/10, and "no Tailscale address on this machine (is Tailscale running?)" when it has none),
//            `localhost` (127.0.0.1, for `tailscale serve` in front), `all` (0.0.0.0) only when the setting says so
//   token    ?t=<token> or the deck_phone cookie, compared in constant time; a query hit sets the cookie
//   routes   /  /index.html …            the phone page (renderer/phone/*)        /vendor/xterm.js …
//            /desk/…                      the desktop renderer, with <base> and the bridge added as it is served
//            /manifest.webmanifest  /icon-192.png  /icon-512.png                   public (no token)
//            /ask/<id>.html  /view/<key>/<folder>/<file>  POST /upload            as lib/phone.js defines them
//   socket   an upgrade on any path (401 without the token), text frames in and out, ping / pong, a ping every
//            15 s and the connection dropped after two unanswered (any message counts as an answer), the close
//            handshake; heardAt / ping / drop for the deck's desk slot (a new desk takes over from a silent one)
//   status   phoneStatus() + `assets`: 'ok' | 'missing' and `assetRoot` (which folder was tried), for the shell
//            to say so when the pages are not beside the exe
#pragma once
#include "deck.hpp"
#include "http.hpp"
#include "seams.hpp"
#include "ws.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace deck { namespace core {

// ---- pure pieces (unit-tested on their own)
// Tailscale hands every node an address in 100.64.0.0/10: the first such address, or ""
std::string tailscaleAddress(const std::vector<std::string>& ipv4);
// The host to bind for a `bind` setting; "" with `error` set when there is none.
std::string bindHost(const std::string& bind, const std::vector<std::string>& ipv4, std::string* error);
// lib/phone.js tailscaleServeUrl: the tailnet HTTPS front door that proxies to http://localhost:<port>, read off
// `tailscale serve status --json`; "" when Serve is not set up for that port.
std::string serveUrlFromStatus(const std::string& statusJson, int port);
// renderer/index.html as the desk is served it: connect-src added to its CSP, <base> and the bridge after </title>
std::string deskPage(const std::string& indexHtml);
// which repo files the desk page may load ("" = none): the path under the asset root
bool deskFileAllowed(const std::string& rel);
const char* contentType(const std::string& ext);                // lib/phone.js TYPES
extern const char* const kUnauthorizedPage;                     // the 401 body
extern const char* const kAskCsp;                               // the ask page's sandbox policy
std::string setCookieHeader(const std::string& token);

struct ServerOptions {
  std::string assetRoot;          // the folder holding renderer/, lib/, assets/, node_modules/@xterm
  int forcePort = 0;              // --serve <port>: on, 127.0.0.1:<port>, whatever settings.phone says
  double pingEveryMs = 15000;     // the heartbeat
  int missedPings = 2;            // unanswered pings before the connection is dropped
};

class Server : public PhoneHost {
 public:
  Server(ServerOptions options, Files& files, Clock& clock, Listener& listener, Net& net);
  void attach(Deck* deck) { deck_ = deck; }

  // ---- PhoneHost (the deck calls these)
  json status(const json& phoneSettings) override;
  void sync(const json& phoneSettings) override;
  json extras() override;
  double heardAt(ClientId client) override;
  bool ping(ClientId client) override;
  void drop(ClientId client) override;

  // ---- the pump (the shell calls these on the deck's thread)
  void onAccept(uint64_t conn);
  void onBytes(uint64_t conn, const char* bytes, size_t n);
  void onClosed(uint64_t conn);
  void tick();
  // a frame for a remote client; false = not sent (gone, or its socket is over MAX_BUFFERED)
  bool deliver(ClientId client, const json& frame);
  bool running() const { return running_; }
  void stop();

  // ---- tests
  std::string pageKey() const;
  bool pagesFound() const;          // renderer/phone/index.html and renderer/index.html are under the asset root
  const std::string& error() const { return error_; }
  const std::string& boundHost() const { return host_; }
  int boundPort() const { return port_; }
  size_t sockets() const;

 private:
  struct Conn {
    uint64_t id = 0;
    RequestReader http;
    bool ws = false, closing = false;
    WsDecoder dec;
    ClientId client = 0;
    int unanswered = 0;
    double nextPingAt = 0;
    double heardAt = 0;                 // a pong or any message (lib/phone.js heard)
  };
  std::string authed(const HttpRequest& req, const Url& url) const;       // "query" | "cookie" | ""
  void onRequest(Conn& c, const HttpRequest& req);
  void upgrade(Conn& c, const HttpRequest& req);
  void onFrames(Conn& c, const char* bytes, size_t n);
  void respond(Conn& c, const HttpRequest& req, int status, std::vector<std::pair<std::string, std::string>> headers, const std::string& body);
  void serveFile(Conn& c, const HttpRequest& req, const std::string& file, std::vector<std::pair<std::string, std::string>> headers, const char* cacheLong);
  void deskRequest(Conn& c, const HttpRequest& req, const std::string& pathname, std::vector<std::pair<std::string, std::string>> headers);
  void drop(uint64_t conn);
  std::string serveUrl(int port);
  std::string join(std::initializer_list<const char*> parts) const;

  ServerOptions opt_;
  Files& files_; Clock& clock_; Listener& listener_; Net& net_;
  Deck* deck_ = nullptr;
  std::map<uint64_t, Conn> conns_;
  std::map<ClientId, uint64_t> byClient_;
  uint64_t adopting_ = 0;             // the socket whose addRemote is running…
  ClientId expecting_ = 0;            // …and the client id the deck is about to give it (Deck::nextClientId)
  bool running_ = false;
  std::string token_, host_, error_;
  int port_ = 0;
  struct { double at = 0; int port = 0; std::string url; bool has = false; } serveCache_;
};

}}  // namespace deck::core
