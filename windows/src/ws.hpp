// windows/src/ws.hpp — a WebSocket client on WinHTTP (system TLS, system proxy settings, no third party).
//
// The transport of remote mode: the same frames core/PROTOCOL.md lists, over the deck's phone server
// (lib/phone.js). One thread connects, reads and reconnects with a back-off; a second drains the send
// queue (and watches for a connection that has gone silent), so the window thread never blocks on the
// network. Everything the socket has to say reaches the window as a posted message: wParam = WsEvent,
// lParam = a heap std::string the receiver deletes.
#pragma once
#include "gfx.hpp"
#include <winhttp.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace deck {

enum WsEvent : WPARAM { WsOpen = 1, WsMessage = 2, WsClosed = 3, WsFailed = 4,
  WsFallback = 5 };          // wsraw.hpp: the exe's own client gave up where WinHTTP may not (DeckSocket::fallBack, the window thread)

struct DeckLink {
  bool secure = true;
  std::wstring host;
  int port = 443;
  std::string token;
  bool valid() const { return !host.empty() && !token.empty(); }
};

// The link Settings -> Phone shows: http(s)://host[:port]/[desk/]?t=<token>. Anything else is not a link.
inline bool parseDeckLink(const std::wstring& text, DeckLink& out) {
  std::wstring s = text;
  while (!s.empty() && iswspace(s.back())) s.pop_back();
  size_t a = 0; while (a < s.size() && iswspace(s[a])) a++;
  s = s.substr(a);
  URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = L"", path[1024] = L"", extra[1024] = L"";
  uc.lpszHostName = host; uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path; uc.dwUrlPathLength = 1023;
  uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1023;
  if (!WinHttpCrackUrl(s.c_str(), 0, 0, &uc)) return false;
  if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
  out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
  out.host = host; out.port = uc.nPort;
  std::wstring q = extra;                       // "?t=…&x=…"
  size_t p = q.find(L"t=");
  while (p != std::wstring::npos && p > 0 && q[p - 1] != L'?' && q[p - 1] != L'&') p = q.find(L"t=", p + 1);
  if (p == std::wstring::npos) return false;
  size_t e = q.find_first_of(L"&#", p);
  out.token = narrow(q.substr(p + 2, e == std::wstring::npos ? std::wstring::npos : e - p - 2));
  return out.valid();
}

// Leaving must be unambiguous for the deck AND for any proxy in between (Tailscale Serve). lib/phone.js gives the
// one desk slot to a socket for as long as that socket reads OPEN, and it has no heartbeat of its own: a socket
// the deck never saw close holds the slot for ever ("one desk at a time"), and nobody else can register. So a
// connection this client gives up is always CLOSED BY THIS CLIENT, actively, never left to wind down:
//   1. the close frame (WinHttpWebSocketShutdown, status 1000) — the deck's `ws` leaves OPEN the moment it reads it;
//   2. a bounded wait for the deck's own close frame (the reader's receive returns it): CLOSE_WAIT_MS;
//   3. the websocket handle is closed (WinHttpCloseHandle: the TCP connection goes down) — by the reader when the
//      handshake finished, otherwise by whoever is giving the connection up, which also cancels the reader's
//      blocked receive. Only then does stop() return and the process exit.
//
// Threads and handle lifetime (the rules this class keeps):
//  * A connection's handles are opened by its reader thread. The websocket handle lives in a `Live` both the
//    reader and the Core hold; closing it is an atomic take (`Live::close`), so it is closed exactly once, by the
//    reader (normally) or by stop() / the watchdog (a connection being given up). The request, connection and
//    session handles never leave the reader.
//  * The writer thread is the only other user of the websocket handle, for sends only, and every send-type call
//    is made under `sendMu`. Whoever closes the handle first withdraws it from the writer under `sendMu` when it
//    can get the lock; a send stuck on a dead link is cancelled by the close itself (that is what closing a
//    WinHTTP handle under a call does).
//  * A connection that has gone silently dead: WinHTTP sends a keep-alive frame every 20 s (so TCP notices a peer
//    that is gone and the receive fails), the receive timeout is 60 s, and the writer watches the clock as well —
//    lib/phone.js sends a client a meta at least every 20 s (every 2 s in fact), so 60 s without a frame means
//    dead. The watchdog closes that connection (steps 1 and 3: there is nobody to wait for) BEFORE the new reader
//    connects, so the deck never has two live sockets of one client — the old one, abandoned but open, would make
//    it refuse the new deskInit.
class WsClient {
 public:
  ~WsClient() { stop(); }

  void start(const DeckLink& link, HWND notify, UINT msg) {
    stop();
    c_ = std::make_shared<Core>();
    c_->link = link; c_->hwnd = notify; c_->msg = msg;
    std::shared_ptr<Core> c = c_;
    c->readers = 1;
    std::thread([c] { readLoop(c, 0); }).detach();
    writer_ = std::thread([c] { writeLoop(c); });
  }

  void stop() {
    std::shared_ptr<Core> c = c_; c_.reset();
    if (!c) return;
    std::shared_ptr<Live> live;
    { std::lock_guard<std::mutex> l(c->m); c->quit = true; c->gen++; c->queue.clear(); live = c->live; }
    c->cv.notify_all();
    const ULONGLONG t0 = GetTickCount64();
    // 1. the close frame. Under sendMu like every send; a send stuck on a dead link is not waited for (step 3
    //    cancels it).
    bool said = false;
    for (int i = 0; i < 30 && !said; i++) {
      std::unique_lock<std::mutex> sl(c->sendMu, std::try_to_lock);
      if (sl.owns_lock()) { if (c->ws) c->closeSent = WinHttpWebSocketShutdown(c->ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0) == NO_ERROR; said = true; break; }
      Sleep(10);
    }
    // 2. the deck's close frame comes back through the reader, which then closes the connection and ends
    while (c->readers.load() > 0 && GetTickCount64() - t0 < CLOSE_WAIT_MS) Sleep(5);
    // 3. it did not (a dead link, a proxy that swallowed it, a connect in progress): the connection is closed
    //    from here, which cancels whatever the reader or the writer is blocked in; a moment for them to notice
    if (c->readers.load() > 0 && live) {
      c->closedHard = true;
      { std::unique_lock<std::mutex> sl(c->sendMu, std::try_to_lock); if (sl.owns_lock() && c->ws == live->ws.load()) { c->ws = nullptr; c->open = false; } live->close(); }
      for (int i = 0; i < 60 && c->readers.load() > 0; i++) Sleep(5);
    }
    for (int i = 0; i < 40 && !c->writerDone.load(); i++) Sleep(5);
    lastStop_.closeSent = c->closeSent.load(); lastStop_.closedHard = c->closedHard.load(); lastStop_.readerDone = c->readers.load() == 0; lastStop_.ms = (int)(GetTickCount64() - t0);
    if (writer_.joinable()) { if (c->writerDone.load()) writer_.join(); else writer_.detach(); }
  }
  // how the last stop() went (the tests log it)
  struct StopInfo { bool closeSent = false, closedHard = false, readerDone = false; int ms = 0; };
  StopInfo lastStop() const { return lastStop_; }
  // (tests) how many connections this client has open right now, readers included that were given up
  int liveConnections() const { return c_ ? c_->liveConns.load() : 0; }
  // (tests) behave as if the connection had gone silent for a minute: the watchdog path, at once
  void testSilence() { std::shared_ptr<Core> c = c_; if (!c) return; c->lastFrame = 1; c->cv.notify_all(); }

  // queued while the socket is down (bridge.js keeps a queue the same way) and sent on open
  void send(const std::string& text) {
    std::shared_ptr<Core> c = c_; if (!c) return;
    { std::lock_guard<std::mutex> l(c->m); c->queue.push_back(text); }
    c->cv.notify_all();
  }
  // drop whatever was queued for a socket that never opened (a reconnect starts a fresh window)
  void clearQueue() { std::shared_ptr<Core> c = c_; if (!c) return; std::lock_guard<std::mutex> l(c->m); c->queue.clear(); }
  bool open() const { return c_ && c_->open.load(); }

  static const DWORD SILENCE_MS = 60000;      // no frame for this long = the connection is dead
  static const DWORD CLOSE_WAIT_MS = 1500;    // how long a goodbye waits for the deck's own close frame
  static const DWORD KEEPALIVE_MS = 20000;    // WinHTTP's own keep-alive frame

 private:
  // one connection's websocket handle, closed exactly once whoever gets there first
  struct Live {
    std::atomic<HINTERNET> ws{nullptr};
    bool close() { HINTERNET h = ws.exchange(nullptr); if (!h) return false; WinHttpCloseHandle(h); return true; }
  };
  struct Core {
    std::shared_ptr<Live> live;               // the current reader's connection (guarded by m)
    std::atomic<bool> closeSent{false}, closedHard{false};
    std::atomic<int> liveConns{0};
    DeckLink link;
    HWND hwnd = nullptr; UINT msg = 0;
    std::mutex m;                             // quit, gen, queue
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool quit = false;
    unsigned gen = 0;                         // which reader is the current one (an abandoned reader has an older number)
    std::mutex sendMu;                        // every send-type call, and the withdrawing + closing of `ws`
    HINTERNET ws = nullptr;                   // the open websocket of the current reader (guarded by sendMu)
    std::atomic<bool> open{false};
    std::atomic<ULONGLONG> lastFrame{0};      // GetTickCount64 of the last frame received (or of the open)
    std::atomic<int> readers{0};
    std::atomic<bool> writerDone{false};
  };
  std::shared_ptr<Core> c_;
  std::thread writer_;
  StopInfo lastStop_;

  static bool current(const std::shared_ptr<Core>& c, unsigned gen) { std::lock_guard<std::mutex> l(c->m); return !c->quit && c->gen == gen; }
  static void post(const std::shared_ptr<Core>& c, unsigned gen, WsEvent ev, const std::string& s) {
    if (!current(c, gen)) return;
    std::string* p = new std::string(s);
    if (!PostMessageW(c->hwnd, c->msg, ev, (LPARAM)p)) delete p;
  }
  static std::string lastError(const char* what) {
    DWORD e = GetLastError();
    char buf[160];
    const char* hint = e == ERROR_WINHTTP_CANNOT_CONNECT ? " (cannot connect: is Tailscale on, on both machines?)"
      : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? " (name not found: is Tailscale on here?)"
      : e == ERROR_WINHTTP_TIMEOUT ? " (timed out)"
      : e == ERROR_WINHTTP_SECURE_FAILURE ? " (TLS failed)" : "";
    snprintf(buf, sizeof buf, "%s: error %lu%s", what, (unsigned long)e, hint);
    return buf;
  }

  // One connection's handles: they never leave the reader thread that opened them.
  struct Conn {
    HINTERNET session = nullptr, conn = nullptr, req = nullptr, ws = nullptr;
    void closeAll() {
      if (ws) { WinHttpCloseHandle(ws); ws = nullptr; }
      if (req) { WinHttpCloseHandle(req); req = nullptr; }
      if (conn) { WinHttpCloseHandle(conn); conn = nullptr; }
      if (session) { WinHttpCloseHandle(session); session = nullptr; }
    }
  };
  static bool connect(const DeckLink& link, Conn& k, std::string& why) {
    k.session = WinHttpOpen(L"ClaudeDeckNative/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!k.session) { why = lastError("open"); return false; }
    // resolve, connect, send, receive. The receive limit bounds the handshake, and should WinHTTP apply it to the
    // socket's receives too, it is the same rule the watchdog keeps: a minute of silence is a dead deck.
    WinHttpSetTimeouts(k.session, 8000, 8000, 15000, (int)SILENCE_MS);
    k.conn = WinHttpConnect(k.session, link.host.c_str(), (INTERNET_PORT)link.port, 0);
    if (!k.conn) { why = lastError("connect"); return false; }
    std::wstring path = L"/desk/ws?t=" + widen(link.token);
    k.req = WinHttpOpenRequest(k.conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, link.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!k.req) { why = lastError("request"); return false; }
    if (!WinHttpSetOption(k.req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) { why = lastError("upgrade option"); return false; }
    if (!WinHttpSendRequest(k.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) { why = lastError("send"); return false; }
    if (!WinHttpReceiveResponse(k.req, nullptr)) { why = lastError("response"); return false; }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(k.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
      why = status == 401 ? "the deck refused the token (401): paste the link from Settings -> Phone again"
        : "the deck answered HTTP " + std::to_string(status) + " instead of a websocket";
      return false;
    }
    k.ws = WinHttpWebSocketCompleteUpgrade(k.req, 0);
    if (!k.ws) { why = lastError("websocket upgrade"); return false; }
    WinHttpCloseHandle(k.req); k.req = nullptr;
    DWORD keep = KEEPALIVE_MS;                // the least WinHTTP takes is 15 s; its default is 30 s
    WinHttpSetOption(k.ws, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keep, sizeof keep);
    return true;
  }

  static void readLoop(std::shared_ptr<Core> c, unsigned gen) {
    int backoff = 500;
    while (current(c, gen)) {
      Conn k; std::string why;
      if (connect(c->link, k, why) && current(c, gen)) {
        backoff = 500;
        c->lastFrame = GetTickCount64();
        std::shared_ptr<Live> live = std::make_shared<Live>(); live->ws = k.ws; c->liveConns++;
        bool mine = false;
        { std::lock_guard<std::mutex> l(c->m); if (!c->quit && c->gen == gen) { c->live = live; mine = true; } }
        if (!mine) { k.ws = nullptr; WinHttpWebSocketShutdown(live->ws.load(), WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); live->close(); c->liveConns--; k.closeAll(); break; }   // given up while it connected: said goodbye, closed
        { std::lock_guard<std::mutex> sl(c->sendMu); c->ws = k.ws; c->open = true; }
        post(c, gen, WsOpen, "");
        c->cv.notify_all();
        std::string msg;
        std::vector<char> buf(256 * 1024);
        for (;;) {
          DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
          const DWORD err = WinHttpWebSocketReceive(k.ws, buf.data(), (DWORD)buf.size(), &got, &type);
          if (err != NO_ERROR) break;
          if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            // The deck's close frame: its answer to ours (the handshake is complete), or its own goodbye, which is
            // answered with a close frame before the connection goes down (RFC 6455 5.5.1). Bounded: WinHTTP's
            // send timeout.
            if (!c->closeSent.load()) { std::lock_guard<std::mutex> sl(c->sendMu); if (live->ws.load()) WinHttpWebSocketShutdown(k.ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); }
            break;
          }
          c->lastFrame = GetTickCount64();
          msg.append(buf.data(), got);
          if (msg.size() > (64u << 20)) break;   // not a deck frame: give the connection up rather than grow
          if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) { post(c, gen, WsMessage, msg); msg.clear(); }
          if (!current(c, gen)) break;
        }
        // withdraw the handle from the writer, then close it: under sendMu, so never while a send is in flight.
        // (A reader whose connection was given up finds another connection's handle there, or none, and leaves it
        // alone; its own handle may already have been closed by whoever gave it up: Live::close takes it once.)
        bool cur = false;                                 // still the connection the writer sends on? (by identity: a handle's value may be reused)
        { std::lock_guard<std::mutex> l(c->m); cur = c->live == live; if (cur) c->live.reset(); }
        {
          std::lock_guard<std::mutex> sl(c->sendMu);
          if (cur) { c->ws = nullptr; c->open = false; }
          k.ws = nullptr; live->close();
          k.closeAll();
        }
        c->liveConns--;
        post(c, gen, WsClosed, "");
      } else {
        k.closeAll();
        if (!why.empty()) post(c, gen, WsFailed, why);
      }
      // bridge.js: 500 ms, x1.7, capped at 8 s
      for (int waited = 0; waited < backoff && current(c, gen); waited += 50) Sleep(50);
      backoff = backoff * 17 / 10; if (backoff > 8000) backoff = 8000;
    }
    c->readers--;
  }

  static void writeLoop(std::shared_ptr<Core> c) {
    for (;;) {
      std::string text; bool have = false, dead = false; unsigned gen = 0;
      {
        std::unique_lock<std::mutex> l(c->m);
        c->cv.wait_for(l, std::chrono::milliseconds(2000), [&] { return c->quit || (c->open.load() && !c->queue.empty()); });
        if (c->quit) break;
        // the watchdog: open, and nothing heard for a minute
        if (c->open.load() && GetTickCount64() - c->lastFrame.load() > SILENCE_MS) { dead = true; gen = c->gen; }
        else if (c->open.load() && !c->queue.empty()) { text.swap(c->queue.front()); c->queue.pop_front(); have = true; }
      }
      if (dead) {
        // Give the silent connection up — and CLOSE it, before anything new connects: a close frame in case the
        // deck is still listening on it (then the deck frees the desk slot now, not when its TCP gives up), and
        // the handle closed, which takes the TCP connection down and wakes its reader out of its receive. The
        // window is told the deck is away, and a new reader starts connecting.
        post(c, gen, WsClosed, "");
        std::shared_ptr<Live> old;
        { std::lock_guard<std::mutex> l(c->m); old = c->live; c->live.reset(); }
        {
          std::lock_guard<std::mutex> sl(c->sendMu);
          if (c->ws) WinHttpWebSocketShutdown(c->ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
          c->ws = nullptr; c->open = false;
          if (old) old->close();
        }
        unsigned next = 0;
        { std::lock_guard<std::mutex> l(c->m); if (c->quit) break; next = ++c->gen; c->queue.clear(); }
        c->readers++;
        std::thread([c, next] { readLoop(c, next); }).detach();
        continue;
      }
      if (!have) continue;
      std::lock_guard<std::mutex> sl(c->sendMu);
      if (c->ws) WinHttpWebSocketSend(c->ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(), (DWORD)text.size());
    }
    c->writerDone = true;
  }
};

}  // namespace deck
