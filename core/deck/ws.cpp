// core/deck/ws.cpp — see ws.hpp.
#include "ws.hpp"
#include "hash.hpp"
#include "util.hpp"

namespace deck { namespace core {

std::string wsAcceptKey(const std::string& key) { return base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")); }

static std::string abortResponse(int code, const std::string& extraHeaders = "") {
  const std::string message = statusText(code);
  return "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) + "\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(message.size()) + "\r\n" + extraHeaders + "\r\n" + message;
}
WsHandshake wsHandshake(const HttpRequest& req) {
  WsHandshake h;
  if (req.method != "GET") { h.status = 405; h.response = abortResponse(405); return h; }
  if (lowerAscii(req.header("upgrade")) != "websocket") { h.status = 400; h.response = abortResponse(400); return h; }
  // /^[+/0-9A-Za-z]{22}==$/
  const std::string& key = req.header("sec-websocket-key");
  bool keyOk = key.size() == 24 && key[22] == '=' && key[23] == '=';
  for (size_t i = 0; keyOk && i < 22; i++) { const char c = key[i]; if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '+' || c == '/')) keyOk = false; }
  if (!keyOk) { h.status = 400; h.response = abortResponse(400); return h; }
  const std::string& version = req.header("sec-websocket-version");
  if (version != "13" && version != "8") { h.status = 426; h.response = abortResponse(426, "Sec-WebSocket-Version: 13, 8\r\n"); return h; }
  h.status = 101;
  h.response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n";
  return h;
}

std::string wsFrame(WsOp op, const std::string& payload, bool fin, bool mask, uint32_t maskKey) {
  std::string out;
  out += (char)((fin ? 0x80 : 0) | (op & 0x0F));
  const uint64_t n = payload.size();
  const unsigned char m = mask ? 0x80 : 0;
  if (n < 126) out += (char)(m | n);
  else if (n < 65536) { out += (char)(m | 126); out += (char)(n >> 8); out += (char)(n & 0xFF); }
  else { out += (char)(m | 127); for (int i = 7; i >= 0; i--) out += (char)((n >> (i * 8)) & 0xFF); }
  if (!mask) { out += payload; return out; }
  const unsigned char k[4] = { (unsigned char)(maskKey >> 24), (unsigned char)(maskKey >> 16), (unsigned char)(maskKey >> 8), (unsigned char)maskKey };
  out.append((const char*)k, 4);
  const size_t at = out.size();
  out += payload;
  for (size_t i = 0; i < payload.size(); i++) out[at + i] = (char)(out[at + i] ^ k[i & 3]);
  return out;
}
std::string wsCloseFrame(int code, const std::string& reason) {
  std::string p;
  if (code) { p += (char)(code >> 8); p += (char)(code & 0xFF); p += reason.substr(0, 123); }
  return wsFrame(WsClose, p);
}

bool wsValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    int n; uint32_t cp;
    if (c < 0x80) { i++; continue; }
    else if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
    else return false;
    for (int k = 1; k <= n; k++) { if (i + k >= s.size()) return false; const unsigned char cc = (unsigned char)s[i + k]; if ((cc & 0xC0) != 0x80) return false; cp = (cp << 6) | (cc & 0x3F); }
    if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += n + 1;
  }
  return true;
}

bool WsDecoder::fail(std::vector<WsEvent>& out, int code, const std::string& why) {
  failed_ = true; buf_.clear(); message_.clear();
  WsEvent e; e.kind = WsEvent::Error; e.code = code; e.data = why;
  out.push_back(e);
  return false;
}
static bool validCloseCode(int c) { return (c >= 1000 && c <= 1014 && c != 1004 && c != 1005 && c != 1006) || (c >= 3000 && c <= 4999); }

void WsDecoder::feed(const char* bytes, size_t n, std::vector<WsEvent>& out) {
  if (failed_) return;
  buf_.append(bytes, n);
  size_t at = 0;
  for (;;) {
    if (buf_.size() - at < 2) break;
    const unsigned char b0 = (unsigned char)buf_[at], b1 = (unsigned char)buf_[at + 1];
    const bool fin = (b0 & 0x80) != 0;
    const int op = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    if (b0 & 0x70) { fail(out, 1002, "RSV1, RSV2 and RSV3 must be clear"); return; }
    if (op != WsCont && op != WsText && op != WsBinary && op != WsClose && op != WsPing && op != WsPong) { fail(out, 1002, "invalid opcode"); return; }
    const bool control = op >= 8;
    if (control && !fin) { fail(out, 1002, "FIN must be set"); return; }
    if (control && len > 125) { fail(out, 1002, "invalid payload length"); return; }
    if (op == WsCont && !fragmented_) { fail(out, 1002, "invalid opcode 0"); return; }
    if ((op == WsText || op == WsBinary) && fragmented_) { fail(out, 1002, "invalid opcode"); return; }
    if (expectMasked_ && !masked) { fail(out, 1002, "MASK must be set"); return; }
    if (!expectMasked_ && masked) { fail(out, 1002, "MASK must be clear"); return; }
    size_t head = 2;
    if (len == 126) {
      if (buf_.size() - at < 4) break;
      len = ((uint64_t)(unsigned char)buf_[at + 2] << 8) | (unsigned char)buf_[at + 3];
      head = 4;
    } else if (len == 127) {
      if (buf_.size() - at < 10) break;
      len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | (unsigned char)buf_[at + 2 + i];
      head = 10;
      if (len >> 63) { fail(out, 1009, "Unsupported WebSocket frame: payload length > 2^53 - 1"); return; }
    }
    if (len > kWsMaxPayload || message_.size() + len > kWsMaxPayload) { fail(out, 1009, "Max payload size exceeded"); return; }
    const size_t maskLen = masked ? 4 : 0;
    if (buf_.size() - at < head + maskLen + len) break;
    std::string payload = buf_.substr(at + head + maskLen, (size_t)len);
    if (masked) { const unsigned char* k = (const unsigned char*)buf_.data() + at + head; for (size_t i = 0; i < payload.size(); i++) payload[i] = (char)(payload[i] ^ k[i & 3]); }
    at += head + maskLen + (size_t)len;

    if (op == WsPing || op == WsPong) { WsEvent e; e.kind = op == WsPing ? WsEvent::Ping : WsEvent::Pong; e.data = payload; out.push_back(e); continue; }
    if (op == WsClose) {
      WsEvent e; e.kind = WsEvent::Close;
      if (payload.empty()) e.code = 1005;
      else if (payload.size() == 1) { fail(out, 1002, "invalid payload length 1"); return; }
      else {
        e.code = ((unsigned char)payload[0] << 8) | (unsigned char)payload[1];
        if (!validCloseCode(e.code)) { fail(out, 1002, "invalid status code"); return; }
        e.data = payload.substr(2);
        if (!wsValidUtf8(e.data)) { fail(out, 1007, "invalid UTF-8 sequence"); return; }
      }
      out.push_back(e);
      failed_ = true; buf_.clear();            // nothing after a close frame is read
      return;
    }
    if (op != WsCont) { fragBinary_ = op == WsBinary; message_.clear(); }
    message_ += payload;
    if (!fin) { fragmented_ = true; continue; }
    fragmented_ = false;
    if (!fragBinary_ && !wsValidUtf8(message_)) { fail(out, 1007, "invalid UTF-8 sequence"); return; }
    WsEvent e; e.kind = WsEvent::Message; e.binary = fragBinary_; e.data.swap(message_);
    out.push_back(e);
  }
  buf_.erase(0, at);
}

}}  // namespace deck::core
