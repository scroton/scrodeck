// core/deck/ws.hpp — RFC 6455, the server's half, pure and byte-in / byte-out: the handshake (what the `ws`
// package's handleUpgrade checks and answers), a frame decoder that enforces what its Receiver enforces
// (masking, reserved bits, control-frame rules, fragmentation, UTF-8, the 100 MiB limit), and a frame encoder.
#pragma once
#include "http.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace deck { namespace core {

// Sec-WebSocket-Accept for a key: base64(sha1(key + GUID))
std::string wsAcceptKey(const std::string& key);
// What an upgrade request gets: status 101 with the response to send, or the status `ws` refuses it with
// (400 a bad key or no websocket upgrade, 405 not a GET, 426 a version that is not 13 or 8).
struct WsHandshake { int status = 400; std::string response; };
WsHandshake wsHandshake(const HttpRequest& req);

enum WsOp { WsCont = 0, WsText = 1, WsBinary = 2, WsClose = 8, WsPing = 9, WsPong = 10 };
// One frame on the wire. A server sends unmasked; a client must mask (tests and a future client use `mask`).
std::string wsFrame(WsOp op, const std::string& payload, bool fin = true, bool mask = false, uint32_t maskKey = 0);
std::string wsCloseFrame(int code, const std::string& reason = "");

struct WsEvent {
  enum Kind { Message, Ping, Pong, Close, Error } kind;
  bool binary = false;
  std::string data;        // the message, a ping / pong payload, a close reason, or what was wrong
  int code = 0;            // Close: the peer's code (1005 when it sent none). Error: the code to close with
};
constexpr uint64_t kWsMaxPayload = 100ull * 1024 * 1024;   // ws's default maxPayload

class WsDecoder {
 public:
  explicit WsDecoder(bool expectMasked = true) : expectMasked_(expectMasked) {}
  // Bytes from the peer. Whole events are appended to `out`; after an Error nothing more is read.
  void feed(const char* bytes, size_t n, std::vector<WsEvent>& out);
  bool failed() const { return failed_; }
 private:
  bool fail(std::vector<WsEvent>& out, int code, const std::string& why);
  bool expectMasked_, failed_ = false;
  std::string buf_;
  bool fragmented_ = false; bool fragBinary_ = false; std::string message_;
};
bool wsValidUtf8(const std::string& s);

}}  // namespace deck::core
