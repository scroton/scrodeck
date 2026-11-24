// mac/Sources/DeckMacSeams/MacListener.cpp — the server's sockets on BSD sockets (core/deck/seams.hpp Listener).
// All of HTTP and the websocket is core/deck/server.cpp; this only binds, accepts, reads and writes.
// UNBUILT, UNTESTED: written on Windows, never compiled.
// Windows counterpart: windows/host/listener.{hpp,cpp} (WinListener) — the same shape, the same rules:
//   one thread (started by listen, joined by stop) owns every socket: it accepts, reads what arrives and pushes it
//   to the inbox, and flushes what could not be written at once. send() tries the socket straight away from the
//   caller's thread and queues the rest, so a small frame is on the wire before send() returns; buffered() is what
//   is still queued (ws.bufferedAmount — the core's 4 MB skip rule reads it).
// JS mirrored: Node's http.Server / net.Socket as lib/phone.js uses them.
// (BSD sockets rather than Network.framework: NWListener cannot bind ONE chosen IPv4 address without more ceremony
//  than this whole file, and "bind the Tailscale address, not 0.0.0.0" is the point of lib/phone.js's bind rule.)
#include "MacSeams.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace deck { namespace mac {

struct MacListener::Impl {
  std::shared_ptr<db::Inbox> inbox;
  int testLow = 0, testHigh = 0;
  int listenFd = -1; int wake[2] = { -1, -1 };
  std::thread thread; std::atomic<bool> stopping{ false };
  struct Conn { int fd = -1; std::string out; bool closeAfterFlush = false, closeNow = false; };
  std::mutex m; std::map<uint64_t, Conn> conns; uint64_t nextId = 1;

  void poke() { const char c = 1; const ssize_t w = ::write(wake[1], &c, 1); (void)w; }
  static void nonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC); }

  void run() {
    char buf[65536];
    while (!stopping.load()) {
      std::vector<struct pollfd> fds; std::vector<uint64_t> ids;
      fds.push_back({ listenFd, POLLIN, 0 }); ids.push_back(0);
      fds.push_back({ wake[0], POLLIN, 0 }); ids.push_back(0);
      { std::lock_guard<std::mutex> g(m);
        for (auto& kv : conns) { short ev = POLLIN; if (!kv.second.out.empty()) ev |= POLLOUT; fds.push_back({ kv.second.fd, ev, 0 }); ids.push_back(kv.first); } }
      if (poll(fds.data(), (nfds_t)fds.size(), 1000) < 0 && errno != EINTR) break;
      if (stopping.load()) break;
      if (fds[1].revents & POLLIN) { char junk[64]; while (::read(wake[0], junk, sizeof junk) > 0) {} }
      if (fds[0].revents & POLLIN) {
        for (;;) {
          const int fd = ::accept(listenFd, nullptr, nullptr);
          if (fd < 0) break;
          nonBlocking(fd);
          int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);       // small frames go at once
          setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
          uint64_t id; { std::lock_guard<std::mutex> g(m); id = nextId++; conns[id].fd = fd; }
          inbox->netAccept(id);
        }
      }
      for (size_t i = 2; i < fds.size(); i++) {
        const uint64_t id = ids[i]; bool gone = false;
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
          for (;;) {
            const ssize_t n = ::recv(fds[i].fd, buf, sizeof buf, 0);
            if (n > 0) { inbox->netBytes(id, buf, (size_t)n); continue; }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) gone = true;
            break;
          }
        }
        std::lock_guard<std::mutex> g(m);
        auto it = conns.find(id);
        if (it == conns.end()) continue;
        Conn& c = it->second;
        if (!gone && !c.out.empty() && (fds[i].revents & POLLOUT)) {
          const ssize_t w = ::send(c.fd, c.out.data(), c.out.size(), 0);
          if (w > 0) c.out.erase(0, (size_t)w);
          else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) gone = true;
        }
        if (gone || c.closeNow || (c.closeAfterFlush && c.out.empty())) {
          ::close(c.fd); conns.erase(it);
          inbox->netClosed(id);                         // by the peer, an error, or close(): the core hears all three
        }
      }
      // connections closed from the deck's thread while they had no poll entry yet
      { std::lock_guard<std::mutex> g(m);
        for (auto it = conns.begin(); it != conns.end();) {
          Conn& c = it->second;
          if (c.closeNow || (c.closeAfterFlush && c.out.empty())) { ::close(c.fd); const uint64_t id = it->first; it = conns.erase(it); inbox->netClosed(id); }
          else ++it;
        } }
    }
  }
};

MacListener::MacListener(std::shared_ptr<db::Inbox> inbox, int testLow, int testHigh) : d(new Impl) { d->inbox = std::move(inbox); d->testLow = testLow; d->testHigh = testHigh; }
MacListener::~MacListener() { stop(); }

bool MacListener::listen(const std::string& host, int port, std::string* error) {
  stop();
  auto fail = [&](const std::string& what) { if (error) *error = what; return false; };
  if (d->testLow > 0 && (host != "127.0.0.1" || port < d->testLow || port > d->testHigh))
    return fail("a test binds 127.0.0.1:" + std::to_string(d->testLow) + "-" + std::to_string(d->testHigh) + " only (asked: " + host + ":" + std::to_string(port) + ")");
  struct sockaddr_in addr; std::memset(&addr, 0, sizeof addr);
  addr.sin_len = sizeof addr; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return fail("not an IPv4 address: " + host);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return fail(std::string("socket: ") + strerror(errno));
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);     // Node sets it too on POSIX
  if (::bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
    const int e = errno; ::close(fd);
    // Node's wording, which the Settings panel shows as it is
    if (e == EADDRINUSE) return fail("listen EADDRINUSE: address already in use " + host + ":" + std::to_string(port));
    if (e == EADDRNOTAVAIL) return fail("listen EADDRNOTAVAIL: address not available " + host + ":" + std::to_string(port));
    return fail(std::string("listen: ") + strerror(e));
  }
  if (::listen(fd, 64) != 0) { const int e = errno; ::close(fd); return fail(std::string("listen: ") + strerror(e)); }
  Impl::nonBlocking(fd);
  if (pipe(d->wake) != 0) { ::close(fd); return fail("pipe() failed"); }
  Impl::nonBlocking(d->wake[0]); Impl::nonBlocking(d->wake[1]);
  d->listenFd = fd; d->stopping.store(false);
  Impl* impl = d.get();
  d->thread = std::thread([impl] { impl->run(); });
  return true;
}

void MacListener::stop() {
  if (d->listenFd < 0) return;
  d->stopping.store(true); d->poke();
  if (d->thread.joinable()) d->thread.join();
  { std::lock_guard<std::mutex> g(d->m); for (auto& kv : d->conns) ::close(kv.second.fd); d->conns.clear(); }
  ::close(d->listenFd); d->listenFd = -1;
  ::close(d->wake[0]); ::close(d->wake[1]); d->wake[0] = d->wake[1] = -1;
}

void MacListener::send(uint64_t conn, const std::string& bytes) {
  if (bytes.empty()) return;
  std::lock_guard<std::mutex> g(d->m);
  auto it = d->conns.find(conn);
  if (it == d->conns.end() || it->second.closeNow) return;
  Impl::Conn& c = it->second;
  size_t done = 0;
  if (c.out.empty()) {
    while (done < bytes.size()) {
      const ssize_t w = ::send(c.fd, bytes.data() + done, bytes.size() - done, 0);
      if (w > 0) done += (size_t)w; else if (errno == EINTR) continue; else break;
    }
  }
  if (done < bytes.size()) { c.out.append(bytes, done, std::string::npos); d->poke(); }
}
size_t MacListener::buffered(uint64_t conn) { std::lock_guard<std::mutex> g(d->m); auto it = d->conns.find(conn); return it == d->conns.end() ? 0 : it->second.out.size(); }
void MacListener::close(uint64_t conn, bool afterFlush) {
  { std::lock_guard<std::mutex> g(d->m);
    auto it = d->conns.find(conn);
    if (it == d->conns.end()) return;
    if (afterFlush) it->second.closeAfterFlush = true; else it->second.closeNow = true; }
  d->poke();
}

}}  // namespace deck::mac
