// windows/src/wsraw.hpp — the exe's OWN WebSocket client: TCP + Schannel TLS + RFC 6455 framing + RFC 7692 inflate.
//
// Why: the office deck's server offers permessage-deflate (lib/phone.js since 6473b09: threshold 1 KB,
// serverMaxWindowBits 13); WinHTTP's socket (ws.hpp) negotiates no extension, so a remote got its terminal output
// uncompressed (it squeezes 5-10x). This client asks for the extension and inflates the messages that come RSV1;
// what it SENDS stays uncompressed (allowed: RSV1 0 - the exe sends keys, small).
//
// `RawWsClient` has WsClient's interface (start / stop / send / clearQueue / open / lastStop / liveConnections /
// testSilence), posts the same events and keeps the same rules: the goodbye in three steps (ws.hpp: our close frame,
// a bounded wait for the deck's, then the connection closed by us), the 60 s silence watchdog, a connection's
// handles closed exactly once. `DeckSocket` (the bottom of this file) is what the window holds: config.json
// `wsClient` "auto" (the default: this client, and WinHTTP for the rest of the run should this one fail where
// WinHTTP may not), "winhttp" or "raw".
//
// Included by ONE translation unit per program (app.cpp; windows/test/wsraw_test.cpp): it carries tinfl's code.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ws.hpp"
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <stdlib.h>
#include <string.h>
#include <cstdint>
#include <algorithm>
#include "../../core/deck/hash.hpp"   // sha1 + base64: the upgrade's accept key

namespace deck {

// the inflater: miniz's tinfl (public domain, core/third_party/miniz-tinfl), its own namespace
namespace tinfl {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../../core/third_party/miniz-tinfl/tinfl.c"
#pragma GCC diagnostic pop
}

// RFC 7692 7.2.2: a message's payload with 00 00 FF FF appended, inflated on the window the messages before it left
// (the server's context takeover: one deflate stream across messages; tinfl in its wrapping mode keeps the 32 KB
// window and its own state between calls). `noContext` (the server said server_no_context_takeover): every message
// starts a fresh stream.
class PmdInflater {
 public:
  bool noContext = false;
  PmdInflater() : dict_(TINFL_LZ_DICT_SIZE) { reset(); }
  void reset() { tinfl_init(&d_); pos_ = 0; }
  // false = not a deflate stream, or more than `limit` bytes out
  bool message(const char* p, size_t n, std::string& out, size_t limit) {
    if (noContext) reset();
    in_.assign(p, n); in_.append("\x00\x00\xff\xff", 4);
    size_t at = 0;
    for (;;) {
      size_t inN = in_.size() - at, outN = TINFL_LZ_DICT_SIZE - pos_;
      const tinfl::tinfl_status s = tinfl::tinfl_decompress(&d_, (const tinfl::mz_uint8*)in_.data() + at, &inN, dict_.data(), dict_.data() + pos_, &outN, tinfl::TINFL_FLAG_HAS_MORE_INPUT);
      at += inN;
      out.append((const char*)dict_.data() + pos_, outN);
      pos_ = (pos_ + outN) & (TINFL_LZ_DICT_SIZE - 1);
      if (s < 0 || out.size() > limit) return false;
      // a final block (BFINAL): the rest of the payload (the tail) is not part of it; the next message is a new
      // stream on the same window
      if (s == tinfl::TINFL_STATUS_DONE) { tinfl_init(&d_); return true; }
      if (s == tinfl::TINFL_STATUS_NEEDS_MORE_INPUT) return at >= in_.size();
      if (!inN && !outN) return false;                 // HAS_MORE_OUTPUT that made no progress: never seen, never loop
    }
  }
 private:
  tinfl::tinfl_decompressor d_;
  std::vector<tinfl::mz_uint8> dict_;
  size_t pos_ = 0;
  std::string in_;
};

// One frame off the front of buf[at..] (RFC 6455 5.2): 1 = a frame (at moved past it), 0 = not all of it yet,
// -1 = not a frame a server may send (why says what).
struct WsFrame { int op = 0; bool fin = false, rsv1 = false; std::string data; };
inline int wsParseFrame(const std::string& buf, size_t& at, WsFrame& f, std::string& why, uint64_t maxLen) {
  const size_t n = buf.size() - at; const unsigned char* p = (const unsigned char*)buf.data() + at;
  if (n < 2) return 0;
  const unsigned b0 = p[0], b1 = p[1];
  const int op = b0 & 0x0f; const bool fin = (b0 & 0x80) != 0, rsv1 = (b0 & 0x40) != 0;
  if (b0 & 0x30) { why = "a frame with RSV2/RSV3 set"; return -1; }
  if (b1 & 0x80) { why = "a masked frame from the server"; return -1; }
  if (!(op <= 2 || (op >= 8 && op <= 10))) { why = "a frame with the unknown opcode " + std::to_string(op); return -1; }
  uint64_t len = b1 & 0x7f; size_t h = 2;
  if (len == 126) { if (n < 4) return 0; len = ((uint64_t)p[2] << 8) | p[3]; h = 4; }
  else if (len == 127) {
    if (n < 10) return 0;
    len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
    h = 10;
    if (len >> 63) { why = "a 64-bit length with its top bit set"; return -1; }
  }
  if (op >= 8) {
    if (!fin) { why = "a fragmented control frame"; return -1; }
    if (len > 125) { why = "a control frame over 125 bytes"; return -1; }
    if (rsv1) { why = "RSV1 on a control frame"; return -1; }
  }
  if (len > maxLen) { why = "a frame of " + std::to_string(len) + " bytes"; return -1; }
  if (n < h + len) return 0;
  f.op = op; f.fin = fin; f.rsv1 = rsv1; f.data.assign((const char*)p + h, (size_t)len);
  at += h + (size_t)len;
  return 1;
}

// the RNG of the upgrade key and the frame masks (the system's)
inline bool wsRandom(void* p, DWORD n) {
  static HCRYPTPROV prov = [] { HCRYPTPROV h = 0; if (!CryptAcquireContextW(&h, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) h = 0; return h; }();
  if (prov && CryptGenRandom(prov, n, (BYTE*)p)) return true;
  static std::atomic<uint64_t> x{GetTickCount64() * 0x9E3779B97F4A7C15ull};   // never seen: a CSP that will not start
  for (DWORD i = 0; i < n; i++) ((BYTE*)p)[i] = (BYTE)((x += 0x9E3779B97F4A7C15ull) >> 56);
  return false;
}

// A client frame (RFC 6455 5.3: masked, a fresh key per frame), FIN set: the exe never fragments what it sends.
inline std::string wsClientFrame(int op, const char* data, size_t n) {
  unsigned char mask[4]; wsRandom(mask, 4);
  std::string f; f.reserve(n + 14);
  f.push_back((char)(0x80 | op));
  if (n < 126) f.push_back((char)(0x80 | n));
  else if (n < 65536) { f.push_back((char)(0x80 | 126)); f.push_back((char)(n >> 8)); f.push_back((char)n); }
  else { f.push_back((char)(0x80 | 127)); for (int i = 7; i >= 0; i--) f.push_back((char)((uint64_t)n >> (8 * i))); }
  f.append((const char*)mask, 4);
  const size_t at = f.size(); f.append(data, n);
  for (size_t i = 0; i < n; i++) f[at + i] = (char)(f[at + i] ^ mask[i & 3]);
  return f;
}
inline std::string wsCloseFrame(unsigned code) { const char b[2] = { (char)(code >> 8), (char)code }; return wsClientFrame(8, b, 2); }

inline std::string wsLower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
inline std::string wsTrim(const std::string& s) { size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r"); return a == std::string::npos ? std::string() : s.substr(a, b - a + 1); }
inline std::string wsHex(long v) { char b[16]; snprintf(b, sizeof b, "0x%08lX", (unsigned long)v); return b; }

// The server's answer to our offer (RFC 7692 5 + 7.1): permessage-deflate and its parameters, or nothing. false =
// something we did not offer (the connection must fail).
inline bool wsParseExtensions(const std::string& value, bool& deflate, bool& noContext, std::string& why) {
  deflate = false; noContext = false;
  size_t a = 0;
  while (a <= value.size()) {
    size_t e = value.find(',', a); if (e == std::string::npos) e = value.size();
    const std::string one = wsTrim(value.substr(a, e - a));
    a = e + 1;
    if (one.empty()) continue;
    size_t p = 0; bool firstPart = true;
    while (p <= one.size()) {
      size_t q = one.find(';', p); if (q == std::string::npos) q = one.size();
      const std::string part = wsLower(wsTrim(one.substr(p, q - p)));
      p = q + 1;
      if (firstPart) {
        firstPart = false;
        if (part != "permessage-deflate" || deflate) { why = "the deck answered an extension the exe did not offer: " + one; return false; }
        deflate = true; continue;
      }
      if (part.empty()) continue;
      const std::string name = part.substr(0, part.find('=')), val = part.find('=') == std::string::npos ? "" : wsTrim(part.substr(part.find('=') + 1));
      if (name == "server_no_context_takeover") noContext = true;
      else if (name == "server_max_window_bits") { const int b = atoi(val.c_str()); if (b < 8 || b > 15) { why = "server_max_window_bits=" + val; return false; } }
      else if (name == "client_no_context_takeover" || name == "client_max_window_bits") {}   // about what WE compress: nothing
      else { why = "an unknown permessage-deflate parameter: " + part; return false; }
    }
  }
  return true;
}

// One connection: the socket, its TLS (Schannel), the upgrade by hand. The reader owns `in` and reads; every write is
// made under the client's sendMu (the writer, the reader's pongs and close answer, stop()'s close frame).
// closeSocket() is the one call any thread may make at any time: it closes the socket once, and a recv or send
// blocked on it returns.
class RawConn {
 public:
  enum Result { Ok, Unreachable, RawPath, Refused };
  bool deflate = false, noContext = false, tls = false;
  std::string extensions;                     // what the deck answered (Sec-WebSocket-Extensions), for the log
  std::string in;                             // plain bytes received, not yet parsed (the reader's)
  bool insecure = false;                      // tests only: a self-signed certificate is accepted

  ~RawConn() { closeSocket(); if (haveCtx_) DeleteSecurityContext(&ctx_); if (haveCred_) FreeCredentialsHandle(&cred_); }
  bool closeSocket() {
    SOCKET s = s_.exchange(INVALID_SOCKET);
    if (s == INVALID_SOCKET) return false;
    shutdown(s, SD_BOTH); closesocket(s);
    return true;
  }

  Result open(const DeckLink& link, std::string& why) {
    static const bool started = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!started) { why = "winsock did not start"; return RawPath; }
    // 1. TCP: the host and port of the link, 8 s to connect (WinHTTP's own limit in ws.hpp)
    ADDRINFOW hints = {}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* res = nullptr;
    const std::wstring port = std::to_wstring(link.port);
    if (GetAddrInfoW(link.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) { why = "connect: name not found (is Tailscale on here?)"; return Unreachable; }
    int lastErr = 0;
    for (ADDRINFOW* ai = res; ai && s_.load() == INVALID_SOCKET; ai = ai->ai_next) {
      SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (s == INVALID_SOCKET) { lastErr = WSAGetLastError(); continue; }
      u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
      int r = ::connect(s, ai->ai_addr, (int)ai->ai_addrlen);
      if (r != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set w, e; FD_ZERO(&w); FD_SET(s, &w); FD_ZERO(&e); FD_SET(s, &e);
        timeval tv = { 8, 0 };
        r = (select(0, nullptr, &w, &e, &tv) == 1 && FD_ISSET(s, &w)) ? 0 : -1;
        if (r != 0) { int so = 0, sl = sizeof so; getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so, &sl); lastErr = so ? so : WSAETIMEDOUT; }
      } else if (r != 0) lastErr = WSAGetLastError();
      nb = 0; ioctlsocket(s, FIONBIO, &nb);
      if (r == 0) s_ = s; else closesocket(s);
    }
    FreeAddrInfoW(res);
    if (s_.load() == INVALID_SOCKET) {
      why = "connect: error " + std::to_string(lastErr) + (lastErr == WSAETIMEDOUT ? " (timed out)" : " (cannot connect: is Tailscale on, on both machines?)");
      return Unreachable;
    }
    SOCKET s = s_.load();
    BOOL one = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);   // keys are small and late is slow
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof one);
    DWORD t = 15000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
    // 2. TLS (an https link: the tailnet's certificate is a public one, so Schannel's own validation, name included)
    if (link.secure && !handshake(link.host, why)) return netFail_ ? Unreachable : RawPath;   // a deck that went away mid-handshake is away, not a TLS fault
    // 3. the upgrade (RFC 6455 4.1), offering permessage-deflate (RFC 7692 5)
    unsigned char kb[16]; wsRandom(kb, 16);
    const std::string key = core::base64(std::string((const char*)kb, 16));
    std::string host = narrow(link.host);
    if (host.find(':') != std::string::npos) host = "[" + host + "]";
    if (link.port != (link.secure ? 443 : 80)) host += ":" + std::to_string(link.port);
    const std::string req = "GET /desk/ws?t=" + link.token + " HTTP/1.1\r\nHost: " + host +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
      "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\nUser-Agent: ClaudeDeckNative/0.1\r\n\r\n";
    if (!write(req)) { why = "upgrade: the request could not be sent"; return RawPath; }
    size_t end;
    while ((end = in.find("\r\n\r\n")) == std::string::npos) {
      if (in.size() > 32768) { why = "upgrade: no end to the deck's answer"; return RawPath; }
      bool tlsBroke = false;
      if (!readMore(why, tlsBroke)) { why = "upgrade: " + why; return RawPath; }
    }
    const std::string head = in.substr(0, end + 2);
    in.erase(0, end + 4);                     // what follows the answer is the first frames
    const int status = head.size() > 12 && head.compare(0, 5, "HTTP/") == 0 ? atoi(head.c_str() + head.find(' ') + 1) : 0;
    if (status == 401) { why = "the deck refused the token (401): paste the link from Settings -> Phone again"; return Refused; }
    if (status != 101) { why = "the deck answered HTTP " + std::to_string(status) + " instead of a websocket"; return RawPath; }
    std::string upgrade, connection, accept;
    bool haveExt = false;
    for (size_t a = head.find("\r\n") + 2; a < head.size();) {
      size_t e = head.find("\r\n", a); if (e == std::string::npos) e = head.size();
      const std::string line = head.substr(a, e - a); a = e + 2;
      const size_t c = line.find(':'); if (c == std::string::npos) continue;
      const std::string name = wsLower(wsTrim(line.substr(0, c))), val = wsTrim(line.substr(c + 1));
      if (name == "upgrade") upgrade = wsLower(val);
      else if (name == "connection") connection = wsLower(val);
      else if (name == "sec-websocket-accept") accept = val;
      else if (name == "sec-websocket-extensions") { extensions += (haveExt ? ", " : "") + val; haveExt = true; }
    }
    if (upgrade != "websocket" || connection.find("upgrade") == std::string::npos) { why = "upgrade: the deck's 101 is not a websocket's"; return RawPath; }
    if (accept != core::base64(core::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))) { why = "upgrade: a wrong Sec-WebSocket-Accept"; return RawPath; }
    if (haveExt && !wsParseExtensions(extensions, deflate, noContext, why)) { why = "upgrade: " + why; return RawPath; }
    // from here, a minute of silence is a dead deck (ws.hpp's rule; the watchdog keeps it too)
    t = WsClient::SILENCE_MS; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    return Ok;
  }

  // More plain bytes into `in`. false = the connection ended or failed; tlsBroke = TLS itself failed (not the network)
  bool readMore(std::string& why, bool& tlsBroke) {
    tlsBroke = false;
    char buf[65536];
    if (!tls) {
      const int n = recvSome(buf, sizeof buf, why); if (n <= 0) return false;
      in.append(buf, n); return true;
    }
    for (;;) {
      if (!enc_.empty()) {
        SecBuffer b[4] = { { (unsigned long)enc_.size(), SECBUFFER_DATA, &enc_[0] }, { 0, SECBUFFER_EMPTY, nullptr }, { 0, SECBUFFER_EMPTY, nullptr }, { 0, SECBUFFER_EMPTY, nullptr } };
        SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
        const SECURITY_STATUS ss = DecryptMessage(&ctx_, &d, 0, nullptr);
        if (ss == SEC_E_OK) {
          bool got = false; std::string extra;
          for (auto& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) { in.append((const char*)x.pvBuffer, x.cbBuffer); got = true; }
            else if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) extra.assign((const char*)x.pvBuffer, x.cbBuffer);
          }
          enc_.swap(extra);                   // both pointed into enc_: copied out before it changes
          if (got) return true;
          continue;
        }
        if (ss == SEC_I_CONTEXT_EXPIRED) { why = "the deck ended TLS"; return false; }
        if (ss != SEC_E_INCOMPLETE_MESSAGE) { why = "TLS: decrypt failed (" + wsHex(ss) + ")"; tlsBroke = true; return false; }   // SEC_I_RENEGOTIATE too: TLS 1.2 only, no renegotiation
      }
      const int n = recvSome(buf, sizeof buf, why); if (n <= 0) return false;
      enc_.append(buf, n);
    }
  }

  // every byte out, encrypted for an https link (under the client's sendMu)
  bool write(const std::string& bytes) {
    if (!tls) return sendAll(bytes.data(), bytes.size());
    std::string rec;
    for (size_t at = 0; at < bytes.size();) {
      const size_t n = std::min((size_t)sizes_.cbMaximumMessage, bytes.size() - at);
      rec.assign(sizes_.cbHeader + n + sizes_.cbTrailer, '\0');
      memcpy(&rec[sizes_.cbHeader], bytes.data() + at, n);
      SecBuffer b[4] = { { sizes_.cbHeader, SECBUFFER_STREAM_HEADER, &rec[0] }, { (unsigned long)n, SECBUFFER_DATA, &rec[sizes_.cbHeader] },
                         { sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, &rec[sizes_.cbHeader + n] }, { 0, SECBUFFER_EMPTY, nullptr } };
      SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
      if (EncryptMessage(&ctx_, 0, &d, 0) != SEC_E_OK) return false;
      if (!sendAll(rec.data(), (size_t)b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
      at += n;
    }
    return true;
  }

 private:
  std::atomic<SOCKET> s_{INVALID_SOCKET};
  bool haveCred_ = false, haveCtx_ = false, netFail_ = false;
  CredHandle cred_ = {}; CtxtHandle ctx_ = {};
  SecPkgContext_StreamSizes sizes_ = {};
  std::string enc_;                           // TLS records received, not yet decrypted

  bool sendAll(const char* p, size_t n) {
    while (n) {
      const SOCKET s = s_.load(); if (s == INVALID_SOCKET) return false;
      const int k = ::send(s, p, (int)std::min(n, (size_t)1 << 20), 0);
      if (k <= 0) return false;
      p += k; n -= (size_t)k;
    }
    return true;
  }
  int recvSome(char* p, int n, std::string& why) {
    const SOCKET s = s_.load(); if (s == INVALID_SOCKET) { why = "closed"; return -1; }
    const int k = ::recv(s, p, n, 0);
    if (k == 0) why = "the deck closed the connection";
    else if (k < 0) { const int e = WSAGetLastError(); why = e == WSAETIMEDOUT ? "receive timed out" : "receive: error " + std::to_string(e); }
    return k;
  }

  // the client side of the TLS handshake (Schannel, TLS 1.2: no post-handshake messages to handle mid-stream)
  bool handshake(const std::wstring& host, std::string& why) {
    SCHANNEL_CRED sc = {}; sc.dwVersion = SCHANNEL_CRED_VERSION; sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO | (insecure ? SCH_CRED_MANUAL_CRED_VALIDATION : SCH_CRED_AUTO_CRED_VALIDATION);
    TimeStamp ts;
    SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &cred_, &ts);
    if (ss != SEC_E_OK) { why = "TLS: no credentials (" + wsHex(ss) + ")"; return false; }
    haveCred_ = true;
    const DWORD want = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    std::wstring target = host;
    char buf[16384];
    for (int round = 0; round < 64; round++) {
      SecBuffer inB[2] = { { (unsigned long)enc_.size(), SECBUFFER_TOKEN, enc_.empty() ? nullptr : &enc_[0] }, { 0, SECBUFFER_EMPTY, nullptr } };
      SecBufferDesc inD = { SECBUFFER_VERSION, 2, inB };
      SecBuffer outB[1] = { { 0, SECBUFFER_TOKEN, nullptr } };
      SecBufferDesc outD = { SECBUFFER_VERSION, 1, outB };
      ULONG got = 0;
      const bool first = !haveCtx_;
      ss = InitializeSecurityContextW(&cred_, first ? nullptr : &ctx_, &target[0], want, 0, 0, first ? nullptr : &inD, 0, first ? &ctx_ : nullptr, &outD, &got, &ts);
      if (first && (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_OK)) haveCtx_ = true;
      if (outB[0].cbBuffer && outB[0].pvBuffer) {
        const bool sent = sendAll((const char*)outB[0].pvBuffer, outB[0].cbBuffer);
        FreeContextBuffer(outB[0].pvBuffer);
        if (!sent) { why = "TLS: the connection dropped"; netFail_ = true; return false; }
      }
      if (ss == SEC_E_INCOMPLETE_MESSAGE) { const int n = recvSome(buf, sizeof buf, why); if (n <= 0) { why = "TLS: " + why; netFail_ = true; return false; } enc_.append(buf, n); continue; }
      if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED && ss != SEC_I_INCOMPLETE_CREDENTIALS) {
        why = "TLS failed (" + wsHex(ss) + (ss == SEC_E_UNTRUSTED_ROOT ? ": the certificate is not trusted" : ss == (SECURITY_STATUS)CERT_E_CN_NO_MATCH || ss == SEC_E_WRONG_PRINCIPAL ? ": the certificate names another host" : "") + ")";
        return false;
      }
      // what the call did not read stays for the next (SECBUFFER_EXTRA: the last cbBuffer bytes of the input)
      if (!first && inB[1].BufferType == SECBUFFER_EXTRA && inB[1].cbBuffer) enc_.erase(0, enc_.size() - inB[1].cbBuffer); else if (!first) enc_.clear();
      if (ss == SEC_E_OK) {
        if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) { why = "TLS: no stream sizes"; return false; }
        tls = true; return true;              // enc_ may already hold the first application records
      }
      if (ss == SEC_I_INCOMPLETE_CREDENTIALS) continue;   // a client certificate asked for: there is none, go on
      if (enc_.empty()) { const int n = recvSome(buf, sizeof buf, why); if (n <= 0) { why = "TLS: " + why; netFail_ = true; return false; } enc_.append(buf, n); }
    }
    why = "TLS: the handshake did not finish"; return false;
  }
};

// The client. Its threads, lifetimes and goodbye are WsClient's (ws.hpp says why each rule is there); what differs is
// the connection under them (RawConn) and two duties WinHTTP did by itself: the deck's pings are answered (lib/phone.js
// drops a client that does not answer its heartbeat), and an idle connection sends a ping every KEEPALIVE_MS.
// `allowFallback` (DeckSocket's "auto"): a failure of THIS client's own path (TLS, the upgrade, a frame or a message it
// cannot read) ends it with WsFallback (after a WsClosed when it was open) instead of a reconnect, and the window
// switches the run to WinHTTP. A deck that is merely away (no route, refused, a dropped connection) is reconnected to
// as ws.hpp does.
class RawWsClient {
 public:
  bool allowFallback = false;
  bool insecure = false;                      // tests only (windows/test/wsraw_test.cpp): a self-signed certificate
  ~RawWsClient() { stop(); }

  void start(const DeckLink& link, HWND notify, UINT msg) {
    stop();
    c_ = std::make_shared<Core>();
    c_->link = link; c_->hwnd = notify; c_->msg = msg; c_->allowFallback = allowFallback; c_->insecure = insecure;
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
    // 1. the close frame (status 1000), under sendMu like every send
    for (int i = 0; i < 30; i++) {
      std::unique_lock<std::mutex> sl(c->sendMu, std::try_to_lock);
      if (sl.owns_lock()) { if (c->ws) c->closeSent = c->ws->write(wsCloseFrame(1000)); break; }
      Sleep(10);
    }
    // 2. the deck's close frame comes back through the reader, which then closes the connection and ends
    while (c->readers.load() > 0 && GetTickCount64() - t0 < WsClient::CLOSE_WAIT_MS) Sleep(5);
    // 3. it did not: the connection is closed from here (a blocked recv or send returns)
    if (c->readers.load() > 0 && live) {
      c->closedHard = true;
      { std::unique_lock<std::mutex> sl(c->sendMu, std::try_to_lock); if (sl.owns_lock() && c->ws == live->conn) { c->ws.reset(); c->open = false; } live->close(); }
      for (int i = 0; i < 60 && c->readers.load() > 0; i++) Sleep(5);
    }
    for (int i = 0; i < 40 && !c->writerDone.load(); i++) Sleep(5);
    lastStop_.closeSent = c->closeSent.load(); lastStop_.closedHard = c->closedHard.load(); lastStop_.readerDone = c->readers.load() == 0; lastStop_.ms = (int)(GetTickCount64() - t0);
    stats_ = c->statsNow();
    if (writer_.joinable()) { if (c->writerDone.load()) writer_.join(); else writer_.detach(); }
  }
  WsClient::StopInfo lastStop() const { return lastStop_; }
  int liveConnections() const { return c_ ? c_->liveConns.load() : 0; }
  void testSilence() { std::shared_ptr<Core> c = c_; if (!c) return; c->lastFrame = 1; c->cv.notify_all(); }

  void send(const std::string& text) {
    std::shared_ptr<Core> c = c_; if (!c) return;
    { std::lock_guard<std::mutex> l(c->m); c->queue.push_back(text); }
    c->cv.notify_all();
  }
  void clearQueue() { std::shared_ptr<Core> c = c_; if (!c) return; std::lock_guard<std::mutex> l(c->m); c->queue.clear(); }
  bool open() const { return c_ && c_->open.load(); }

  // what the deck agreed to on the current (or last) connection, and what came in: wire = the frames' payload
  // bytes as they arrived, plain = the messages after inflating
  struct Stats { std::string extensions; bool connected = false, deflate = false, tls = false; uint64_t messages = 0, compressed = 0, wire = 0, plain = 0; };
  Stats stats() const { return c_ ? c_->statsNow() : stats_; }

 private:
  struct Live {
    std::shared_ptr<RawConn> conn;
    std::atomic<bool> closed{false};
    bool close() { if (closed.exchange(true)) return false; conn->closeSocket(); return true; }
  };
  struct Core {
    std::shared_ptr<Live> live;
    std::atomic<bool> closeSent{false}, closedHard{false};
    std::atomic<int> liveConns{0};
    DeckLink link; bool allowFallback = false, insecure = false;
    HWND hwnd = nullptr; UINT msg = 0;
    std::mutex m;                             // quit, gen, queue, stats
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool quit = false;
    unsigned gen = 0;
    std::mutex sendMu;                        // every write, and the withdrawing of `ws`
    std::shared_ptr<RawConn> ws;              // the open connection of the current reader (guarded by sendMu)
    std::atomic<bool> open{false};
    std::atomic<ULONGLONG> lastFrame{0}, lastSent{0};
    std::atomic<int> readers{0};
    std::atomic<bool> writerDone{false};
    Stats stats;
    Stats statsNow() { std::lock_guard<std::mutex> l(m); return stats; }
  };
  std::shared_ptr<Core> c_;
  std::thread writer_;
  WsClient::StopInfo lastStop_;
  Stats stats_;

  static bool current(const std::shared_ptr<Core>& c, unsigned gen) { std::lock_guard<std::mutex> l(c->m); return !c->quit && c->gen == gen; }
  static void post(const std::shared_ptr<Core>& c, unsigned gen, WPARAM ev, const std::string& s) {
    if (!current(c, gen)) return;
    std::string* p = new std::string(s);
    if (!PostMessageW(c->hwnd, c->msg, ev, (LPARAM)p)) delete p;
  }
  // a write on the current connection (the reader's pong / close answer), under sendMu
  static bool writeLive(const std::shared_ptr<Core>& c, const std::shared_ptr<Live>& live, const std::string& bytes) {
    std::lock_guard<std::mutex> sl(c->sendMu);
    if (live->closed.load()) return false;
    c->lastSent = GetTickCount64();
    return live->conn->write(bytes);
  }

  static void readLoop(std::shared_ptr<Core> c, unsigned gen) {
    int backoff = 500;
    const uint64_t kMax = 64u << 20;          // not a deck frame: give the connection up rather than grow (ws.hpp)
    while (current(c, gen)) {
      std::shared_ptr<RawConn> k = std::make_shared<RawConn>(); k->insecure = c->insecure;
      std::string why;
      const RawConn::Result r = k->open(c->link, why);
      if (r == RawConn::Ok && current(c, gen)) {
        backoff = 500;
        c->lastFrame = GetTickCount64();
        std::shared_ptr<Live> live = std::make_shared<Live>(); live->conn = k; c->liveConns++;
        bool mine = false;
        {
          std::lock_guard<std::mutex> l(c->m);
          if (!c->quit && c->gen == gen) { c->live = live; mine = true; c->stats = Stats(); c->stats.connected = true; c->stats.extensions = k->extensions; c->stats.deflate = k->deflate; c->stats.tls = k->tls; }
        }
        if (!mine) { writeLive(c, live, wsCloseFrame(1000)); live->close(); c->liveConns--; break; }   // given up while it connected: said goodbye, closed
        { std::lock_guard<std::mutex> sl(c->sendMu); c->ws = k; c->open = true; c->lastSent = GetTickCount64(); }
        post(c, gen, WsOpen, "");
        c->cv.notify_all();
        PmdInflater inflater; inflater.noContext = k->noContext;
        std::string msg, rawWhy, ioWhy; int msgOp = 0; bool msgDeflated = false;
        size_t at = 0;
        for (bool going = true; going;) {
          WsFrame f; std::string err;
          const int got = wsParseFrame(k->in, at, f, err, kMax);
          if (got < 0) { rawWhy = err; break; }
          if (got == 0) {
            if (at) { k->in.erase(0, at); at = 0; }
            bool tlsBroke = false;
            if (!k->readMore(ioWhy, tlsBroke)) { if (tlsBroke) rawWhy = ioWhy; break; }
            continue;
          }
          c->lastFrame = GetTickCount64();
          { std::lock_guard<std::mutex> l(c->m); c->stats.wire += f.data.size(); }
          switch (f.op) {
            case 9: writeLive(c, live, wsClientFrame(10, f.data.data(), f.data.size())); break;   // ping -> pong, the same bytes
            case 10: break;
            case 8:
              // the deck's close frame: the answer to ours (the handshake is complete), or its own goodbye, answered
              // before the connection goes down (RFC 6455 5.5.1)
              if (!c->closeSent.load()) writeLive(c, live, wsCloseFrame(1000));
              going = false; break;
            default:
              if (f.op != 0) {                // the first frame of a message
                if (msgOp) { rawWhy = "a new message inside a fragmented one"; going = false; break; }
                if (f.rsv1 && !k->deflate) { rawWhy = "a compressed frame without permessage-deflate"; going = false; break; }
                msgOp = f.op; msgDeflated = f.rsv1; msg.swap(f.data);
              } else {
                if (!msgOp) { rawWhy = "a continuation frame outside a message"; going = false; break; }
                if (f.rsv1) { rawWhy = "RSV1 on a continuation frame"; going = false; break; }
                msg += f.data;
              }
              if (msg.size() > kMax) { rawWhy = "a message over 64 MB"; going = false; break; }
              if (!f.fin) break;
              if (msgDeflated) {
                std::string out;
                if (!inflater.message(msg.data(), msg.size(), out, kMax)) { rawWhy = "a compressed message that does not inflate"; going = false; break; }
                { std::lock_guard<std::mutex> l(c->m); c->stats.compressed++; }
                msg.swap(out);
              }
              { std::lock_guard<std::mutex> l(c->m); c->stats.messages++; c->stats.plain += msg.size(); }
              post(c, gen, WsMessage, msg);
              msg.clear(); msgOp = 0; msgDeflated = false;
              if (!current(c, gen)) going = false;
          }
        }
        // a frame this client cannot read: the deck is told (1002 protocol error) before the connection goes
        if (!rawWhy.empty() && !c->closeSent.load()) writeLive(c, live, wsCloseFrame(1002));
        bool cur = false;
        { std::lock_guard<std::mutex> l(c->m); cur = c->live == live; if (cur) c->live.reset(); }
        {
          std::lock_guard<std::mutex> sl(c->sendMu);
          if (cur) { c->ws.reset(); c->open = false; }
          live->close();
        }
        c->liveConns--;
        post(c, gen, WsClosed, "");
        if (!rawWhy.empty()) {
          if (c->allowFallback) { post(c, gen, WsFallback, rawWhy); break; }
          post(c, gen, WsFailed, "the exe's own socket: " + rawWhy);
        }
      } else {
        k->closeSocket();
        if (r == RawConn::RawPath && c->allowFallback && current(c, gen)) { post(c, gen, WsFallback, why); break; }
        if (!why.empty()) post(c, gen, WsFailed, why);
      }
      for (int waited = 0; waited < backoff && current(c, gen); waited += 50) Sleep(50);
      backoff = backoff * 17 / 10; if (backoff > 8000) backoff = 8000;
    }
    c->readers--;
  }

  static void writeLoop(std::shared_ptr<Core> c) {
    for (;;) {
      std::string text; bool have = false, dead = false, ping = false; unsigned gen = 0;
      {
        std::unique_lock<std::mutex> l(c->m);
        c->cv.wait_for(l, std::chrono::milliseconds(2000), [&] { return c->quit || (c->open.load() && !c->queue.empty()); });
        if (c->quit) break;
        const ULONGLONG now = GetTickCount64();
        if (c->open.load() && now - c->lastFrame.load() > WsClient::SILENCE_MS) { dead = true; gen = c->gen; }
        else if (c->open.load() && !c->queue.empty()) { text.swap(c->queue.front()); c->queue.pop_front(); have = true; }
        else if (c->open.load() && now - c->lastSent.load() > WsClient::KEEPALIVE_MS) ping = true;
      }
      if (dead) {
        // ws.hpp's watchdog: the silent connection is given up AND closed before anything new connects
        post(c, gen, WsClosed, "");
        std::shared_ptr<Live> old;
        { std::lock_guard<std::mutex> l(c->m); old = c->live; c->live.reset(); }
        {
          std::lock_guard<std::mutex> sl(c->sendMu);
          if (c->ws) c->ws->write(wsCloseFrame(1000));
          c->ws.reset(); c->open = false;
          if (old) old->close();
        }
        unsigned next = 0;
        { std::lock_guard<std::mutex> l(c->m); if (c->quit) break; next = ++c->gen; c->queue.clear(); }
        c->readers++;
        std::thread([c, next] { readLoop(c, next); }).detach();
        continue;
      }
      if (!have && !ping) continue;
      std::lock_guard<std::mutex> sl(c->sendMu);
      if (!c->ws) continue;
      c->lastSent = GetTickCount64();
      c->ws->write(have ? wsClientFrame(1, text.data(), text.size()) : wsClientFrame(9, "", 0));
    }
    c->writerDone = true;
  }
};

// What the window holds: one of the two clients, by config.json `wsClient` - "auto" (the default: the exe's own,
// WinHTTP for the rest of the run after a WsFallback), "winhttp", "raw" (no fallback: its failures are reported and
// retried like any). Every call goes to the one in use; the window thread calls fallBack() on WsFallback.
class DeckSocket {
 public:
  enum Mode { Auto, WinHttp, Raw };
  static Mode modeOf(const std::string& s) { return s == "winhttp" ? WinHttp : s == "raw" ? Raw : Auto; }
  static const char* nameOf(Mode m) { return m == WinHttp ? "winhttp" : m == Raw ? "raw" : "auto"; }
  typedef WsClient::StopInfo StopInfo;

  void setMode(Mode m) { mode_ = m; }
  Mode mode() const { return mode_; }
  void start(const DeckLink& link, HWND notify, UINT msg) {
    stop();
    link_ = link; hwnd_ = notify; msg_ = msg;
    raw_ = mode_ == Raw || (mode_ == Auto && !fellBack_);
    if (raw_) { rawc_.allowFallback = mode_ == Auto; rawc_.start(link, notify, msg); } else win_.start(link, notify, msg);
  }
  void stop() { if (raw_) rawc_.stop(); else win_.stop(); }
  // WsFallback: the exe's own client ended (its reader posts nothing more); the run goes on over WinHTTP, same link.
  // false = nothing to do (not in auto, or done already)
  bool fallBack() {
    if (!raw_ || mode_ != Auto || fellBack_) return false;
    fellBack_ = true; rawc_.stop(); raw_ = false;
    win_.start(link_, hwnd_, msg_);
    return true;
  }
  void send(const std::string& text) { if (raw_) rawc_.send(text); else win_.send(text); }
  void clearQueue() { if (raw_) rawc_.clearQueue(); else win_.clearQueue(); }
  bool open() const { return raw_ ? rawc_.open() : win_.open(); }
  StopInfo lastStop() const { return raw_ ? rawc_.lastStop() : win_.lastStop(); }
  int liveConnections() const { return raw_ ? rawc_.liveConnections() : win_.liveConnections(); }
  void testSilence() { if (raw_) rawc_.testSilence(); else win_.testSilence(); }
  bool usingRaw() const { return raw_; }
  bool fellBack() const { return fellBack_; }
  // for the log: which client, and what the deck agreed to
  std::string describe() const {
    if (!raw_) return std::string("winhttp (no compression)") + (fellBack_ ? ", after a fallback" : "");
    const RawWsClient::Stats s = rawc_.stats();
    if (!s.connected) return "raw (not connected)";
    return std::string("raw") + (s.tls ? " tls" : "") + (s.deflate ? ", permessage-deflate (" + s.extensions + ")" : ", no compression offered by the deck");
  }
  RawWsClient::Stats rawStats() const { return rawc_.stats(); }

 private:
  Mode mode_ = Auto;
  bool raw_ = false, fellBack_ = false;
  WsClient win_;
  RawWsClient rawc_;
  DeckLink link_; HWND hwnd_ = nullptr; UINT msg_ = 0;
};

}  // namespace deck
