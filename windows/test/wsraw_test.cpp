// windows/test/wsraw_test.cpp — the exe's own WebSocket client (windows/src/wsraw.hpp) on its own, no window shown
// (a message-only window receives the client's events). Built and run by windows/test/wsraw.ps1.
//   wsraw_test unit                          the frame parser, the client frames, the extension answer (no network)
//   wsraw_test inflate <cases.json>          PmdInflater against Node's zlib (windows/test/wsraw-inflate.js)
//   wsraw_test deck <link> [--mode M]        connect, deskInit, attach `big`, the replay's length + sha256, the goodbye
//   wsraw_test echo <link> [--insecure] [--mode raw]   echo every message until the server closes (wsraw-deck.js frames/tls)
// --mode auto|raw|winhttp (DeckSocket; default raw for echo, auto for deck). Prints `key value` lines; exit 0.
#include "../src/wsraw.hpp"
#include "../../core/third_party/nlohmann/json.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace deck;
static int fails = 0, passes = 0;
static void check(bool ok, const std::string& what) { (ok ? passes : fails)++; printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str()); fflush(stdout); }
static std::string hexSha(const std::string& s) { return core::hexOf(core::sha256(s)); }

static std::string serverFrame(int op, const std::string& p, bool fin = true, bool rsv1 = false) {
  std::string f; const size_t n = p.size();
  f.push_back((char)((fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | op));
  if (n < 126) f.push_back((char)n);
  else if (n < 65536) { f.push_back((char)126); f.push_back((char)(n >> 8)); f.push_back((char)n); }
  else { f.push_back((char)127); for (int i = 7; i >= 0; i--) f.push_back((char)((uint64_t)n >> (8 * i))); }
  return f + p;
}

static int unit() {
  const uint64_t kMax = 64u << 20;
  // every edge of the three length forms, whole and fed a byte at a time
  for (size_t n : { (size_t)0, (size_t)1, (size_t)125, (size_t)126, (size_t)127, (size_t)65535, (size_t)65536, (size_t)200000 }) {
    std::string p(n, 'x'); for (size_t i = 0; i < n; i++) p[i] = (char)('a' + i % 26);
    const std::string f = serverFrame(1, p);
    std::string buf; size_t at = 0; WsFrame fr; std::string why; int r = 0, calls = 0;
    for (size_t i = 0; i < f.size() && r == 0; i++) { buf.push_back(f[i]); r = wsParseFrame(buf, at, fr, why, kMax); calls++; }
    check(r == 1 && fr.op == 1 && fr.fin && fr.data == p && at == f.size() && (size_t)calls == f.size(), "a " + std::to_string(n) + "-byte frame, fed a byte at a time, parses only when whole");
  }
  // two frames in one buffer, and the second split
  { std::string b = serverFrame(1, "one") + serverFrame(2, std::string(300, 'z')); b.resize(b.size() - 1);
    size_t at = 0; WsFrame f; std::string why;
    const int r1 = wsParseFrame(b, at, f, why, kMax); const std::string first = f.data;
    const int r2 = wsParseFrame(b, at, f, why, kMax);
    check(r1 == 1 && first == "one" && r2 == 0 && at == 5, "two frames in one read: the first parses, the second waits for its last byte"); }
  // what a server must not send
  struct Bad { std::string bytes, what; };
  const std::vector<Bad> bad = {
    { std::string("\xa1\x00", 2), "RSV2 set" }, { std::string("\x91\x00", 2), "RSV3 set" },
    { std::string("\x81\x80\x01\x02\x03\x04", 6), "a masked frame" }, { std::string("\x83\x00", 2), "opcode 3" }, { std::string("\x8b\x00", 2), "opcode 11" },
    { std::string("\x09\x00", 2), "a ping without FIN" }, { std::string("\x89\x7e\x00\x7e", 4) + std::string(126, 'p'), "a 126-byte ping" },
    { std::string("\xc9\x00", 2), "RSV1 on a ping" }, { std::string("\x82\x7f\x80\x00\x00\x00\x00\x00\x00\x00", 10), "a 64-bit length with the top bit" },
    { std::string("\x82\x7f\x00\x00\x00\x00\x04\x00\x00\x01", 10), "a 64 MB + 1 frame" },
  };
  for (const auto& b : bad) { size_t at = 0; WsFrame f; std::string why; const int r = wsParseFrame(b.bytes, at, f, why, kMax); check(r == -1 && !why.empty(), "refused: " + b.what + " (" + why + ")"); }
  // client frames: masked, FIN, the right length form, the payload behind the mask
  for (size_t n : { (size_t)0, (size_t)125, (size_t)126, (size_t)65535, (size_t)65536 }) {
    const std::string p(n, 'k'); const std::string f = wsClientFrame(1, p.data(), p.size());
    const unsigned char* u = (const unsigned char*)f.data();
    const size_t h = n < 126 ? 2 : n < 65536 ? 4 : 10;
    bool same = f.size() == h + 4 + n;
    for (size_t i = 0; same && i < n; i++) same = (char)(f[h + 4 + i] ^ f[h + (i & 3)]) == p[i];
    check(u[0] == 0x81 && (u[1] & 0x80) && same && ((u[1] & 0x7f) == (n < 126 ? n : n < 65536 ? 126 : 127)), "a client frame of " + std::to_string(n) + " bytes: FIN, masked, unmasks to the payload");
  }
  { const std::string a = wsClientFrame(1, "abcd", 4), b = wsClientFrame(1, "abcd", 4); check(a.substr(2, 4) != b.substr(2, 4), "a fresh mask per frame"); }
  { const std::string c = wsCloseFrame(1000); check((unsigned char)c[0] == 0x88 && (unsigned char)c[1] == 0x82 && (char)(c[6] ^ c[2]) == 0x03 && (char)(c[7] ^ c[3]) == (char)0xe8, "the close frame carries 1000"); }
  // the server's extension answer
  struct Ext { std::string v; bool ok, deflate, noCtx; };
  const std::vector<Ext> ext = {
    { "permessage-deflate", true, true, false }, { "permessage-deflate; server_max_window_bits=13", true, true, false },
    { "permessage-deflate; client_max_window_bits=15; server_max_window_bits=13", true, true, false },
    { "Permessage-Deflate; Server_No_Context_Takeover", true, true, true }, { "permessage-deflate; client_no_context_takeover", true, true, false },
    { "x-webkit-deflate-frame", false, false, false }, { "permessage-deflate; server_max_window_bits=7", false, true, false },
    { "permessage-deflate; bogus=1", false, true, false }, { "permessage-deflate, permessage-deflate", false, true, false },
  };
  for (const auto& e : ext) { bool d = false, n = false; std::string why; const bool ok = wsParseExtensions(e.v, d, n, why); check(ok == e.ok && (!ok || (d == e.deflate && n == e.noCtx)), "extensions '" + e.v + "' -> " + (ok ? std::string(d ? "deflate" : "none") + (n ? " no-context" : "") : "refused: " + why)); }
  printf("unit passed %d failed %d\n", passes, fails);
  return fails ? 1 : 0;
}

static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static int inflate(const std::string& file) {
  const auto j = nlohmann::json::parse(readFile(file), nullptr, false);
  if (!j.is_object()) { printf("FAIL no cases in %s\n", file.c_str()); return 1; }
  for (const auto& c : j["cases"]) {
    PmdInflater inf; inf.noContext = c.value("noContext", false);
    size_t i = 0; bool all = true; std::string firstBad;
    for (const auto& m : c["messages"]) {
      const std::string in = core::unbase64(m["z"].get<std::string>()); std::string out;
      const bool ok = inf.message(in.data(), in.size(), out, 64u << 20);
      const bool same = ok && m.contains("len") && out.size() == m["len"].get<size_t>() && hexSha(out) == m["sha"].get<std::string>();
      const bool good = m.value("bad", false) ? !ok : m.value("maybe", false) ? !same : same;   // maybe: a corrupted stream - fails, or comes out different, and never crashes
      if (!good && all) { all = false; firstBad = "message " + std::to_string(i) + (ok ? " len " + std::to_string(out.size()) : " failed"); }
      i++;
    }
    check(all, c["name"].get<std::string>() + " (" + std::to_string(i) + " messages)" + (all ? "" : ": " + firstBad));
  }
  printf("inflate passed %d failed %d\n", passes, fails);
  return fails ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------- over the network
static const UINT WM_WSX = WM_APP + 7;
static int net(const std::string& what, const std::wstring& url, DeckSocket::Mode mode, bool insecure, int seconds) {
  DeckLink link;
  if (!parseDeckLink(url, link)) { printf("FAIL not a link\n"); return 1; }
  WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"wsraw_test";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  DeckSocket ws; ws.setMode(mode);
  // the test-only knob: through a raw client the DeckSocket does not expose, so echo/tls runs use RawWsClient itself
  RawWsClient raw; raw.insecure = insecure; raw.allowFallback = false;
  const bool direct = insecure;               // --insecure: the TLS run, on RawWsClient directly
  if (direct) raw.start(link, hwnd, WM_WSX); else ws.start(link, hwnd, WM_WSX);
  auto sendIt = [&](const std::string& s) { if (direct) raw.send(s); else ws.send(s); };
  SetTimer(hwnd, 1, seconds * 1000, nullptr);
  int opens = 0, closes = 0, fallbacks = 0; std::string replaySha; size_t replayLen = 0; bool inited = false, done = false;
  const ULONGLONG t0 = GetTickCount64();
  MSG m;
  while (!done && GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (m.message == WM_TIMER) { printf("timeout after %d s\n", seconds); break; }
    if (m.message != WM_WSX) { DispatchMessageW(&m); continue; }
    std::string* p = (std::string*)m.lParam; const std::string text = p ? *p : std::string(); delete p;
    const WPARAM ev = m.wParam;
    if (ev == WsOpen) { opens++; printf("open %d via %s at %llu ms\n", opens, direct ? "raw (direct)" : ws.describe().c_str(), (unsigned long long)(GetTickCount64() - t0));
      if (what == "deck") sendIt("{\"t\":\"call\",\"fn\":\"deskInit\",\"rid\":1,\"args\":[{\"kind\":\"native\",\"name\":\"wsraw-test\"}]}"); }
    else if (ev == WsFailed) { printf("failed: %s\n", text.c_str()); if (what != "deck" || mode == DeckSocket::Raw) done = true; }
    else if (ev == WsClosed) { closes++; printf("closed %d\n", closes); if (what == "echo") done = true; }
    else if (ev == WsFallback) { fallbacks++; printf("fallback: %s\n", text.c_str()); printf("fallBack() %d\n", (int)ws.fallBack()); }
    else if (ev == WsMessage) {
      if (what == "echo") { sendIt(text); continue; }
      const auto j = nlohmann::json::parse(text, nullptr, false);
      if (!j.is_object()) continue;
      const std::string t = j.value("t", std::string());
      if (t == "ok" && j.value("rid", 0) == 1) { inited = true; printf("deskInit ok\n"); sendIt("{\"t\":\"attach\",\"id\":\"big\"}"); }
      else if (t == "data" && j.value("id", std::string()) == "big") { const std::string d = j.value("d", std::string()); replayLen = d.size(); replaySha = hexSha(d); printf("replay bytes %zu sha256 %s\n", replayLen, replaySha.c_str()); done = true; }
      else if (t == "err") printf("err %s\n", text.c_str());
    }
  }
  if (direct) {
    raw.stop(); const auto s = raw.lastStop(); const auto st = raw.stats();
    printf("stop: close frame sent %d, closed by the reader after the deck's answer %d, closed hard %d, %d ms\n", s.closeSent, s.readerDone && !s.closedHard, s.closedHard, s.ms);
    printf("stats: tls %d deflate %d extensions '%s' messages %llu compressed %llu wire %llu plain %llu\n", st.tls, st.deflate, st.extensions.c_str(), (unsigned long long)st.messages, (unsigned long long)st.compressed, (unsigned long long)st.wire, (unsigned long long)st.plain);
  } else {
    const std::string how = ws.describe(); const bool usedRaw = ws.usingRaw(); const auto st = ws.rawStats();
    ws.stop(); const auto s = ws.lastStop();
    printf("via %s\n", how.c_str());
    printf("stop: close frame sent %d, closed by the reader after the deck's answer %d, closed hard %d, %d ms\n", s.closeSent, s.readerDone && !s.closedHard, s.closedHard, s.ms);
    if (usedRaw) printf("stats: tls %d deflate %d extensions '%s' messages %llu compressed %llu wire %llu plain %llu\n", st.tls, st.deflate, st.extensions.c_str(), (unsigned long long)st.messages, (unsigned long long)st.compressed, (unsigned long long)st.wire, (unsigned long long)st.plain);
  }
  printf("summary opens %d closes %d fallbacks %d inited %d replay %zu\n", opens, closes, fallbacks, (int)inited, replayLen);
  DestroyWindow(hwnd);
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) { printf("usage: wsraw_test unit | inflate <cases.json> | deck <link> [--mode M] | echo <link> [--insecure] [--mode M]\n"); return 2; }
  const std::wstring cmd = argv[1];
  if (cmd == L"unit") return unit();
  if (cmd == L"inflate" && argc > 2) return inflate(narrow(argv[2]));
  if ((cmd == L"deck" || cmd == L"echo") && argc > 2) {
    DeckSocket::Mode mode = cmd == L"deck" ? DeckSocket::Auto : DeckSocket::Raw; bool insecure = false; int seconds = 30;
    for (int i = 3; i < argc; i++) {
      const std::wstring a = argv[i];
      if (a == L"--mode" && i + 1 < argc) mode = DeckSocket::modeOf(narrow(argv[++i]));
      else if (a == L"--insecure") insecure = true;
      else if (a == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
    }
    return net(narrow(cmd), argv[2], mode, insecure, seconds);
  }
  printf("unknown command\n"); return 2;
}
