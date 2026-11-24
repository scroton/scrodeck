// windows/host/listener.hpp — the server's sockets on Winsock (core/deck/seams.hpp Listener), and what the server
// asks about the network around it (Net). All of HTTP and the websocket is core/deck/server.cpp; this only binds,
// accepts, reads and writes.
//
// One thread (started by listen, joined by stop) owns every socket: it accepts, reads what arrives and hands it to
// `post` (which must be safe from that thread: the shell queues it for the deck's thread), and flushes what could
// not be written at once. send() tries the socket straight away from the caller's thread and queues the rest, so
// a small frame is on the wire before send() returns; buffered() is what is still queued (ws.bufferedAmount).
#pragma once
#include "../../core/deck/seams.hpp"
#include <functional>
#include <memory>
#include <string>

namespace deck { namespace host {

class WinListener : public core::Listener {
 public:
  // what: 0 a connection was accepted, 1 bytes arrived, 2 it closed (by the peer, an error, or close())
  using Post = std::function<void(uint64_t conn, int what, const char* bytes, size_t n)>;
  explicit WinListener(Post post);
  ~WinListener() override;
  bool listen(const std::string& host, int port, std::string* error) override;
  void stop() override;
  void send(uint64_t conn, const std::string& bytes) override;
  size_t buffered(uint64_t conn) override;
  void close(uint64_t conn, bool afterFlush) override;
  // tests: stop reading from AND writing to one connection, as a peer that stopped reading would make it
  void stall(uint64_t conn, bool on);
 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

class WinNet : public core::Net {
 public:
  std::vector<std::string> ipv4Addresses() override;      // GetAdaptersAddresses: up, not loopback
  std::string tailscaleServeStatus() override;            // `tailscale serve status --json`, hidden (CREATE_NO_WINDOW), 3 s at most
};

}}  // namespace deck::host
