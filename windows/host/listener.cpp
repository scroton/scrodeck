// windows/host/listener.cpp — see listener.hpp.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include "listener.hpp"
#include "conpty.hpp"      // widen / narrow

#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace deck { namespace host {

namespace {
struct ConnState {
  SOCKET s = INVALID_SOCKET;
  std::string out; size_t off = 0;         // queued and not yet written: out[off..)
  bool closeWhenFlushed = false, stalled = false;
};
bool startWinsock() { static const bool ok = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }(); return ok; }
std::string sockError(int e, const std::string& host, int port) {
  const std::string where = " " + host + ":" + std::to_string(port);
  if (e == WSAEADDRINUSE || e == WSAEACCES) return "listen EADDRINUSE: address already in use" + where;
  if (e == WSAEADDRNOTAVAIL) return "listen EADDRNOTAVAIL: address not available" + where;
  return "listen failed (winsock " + std::to_string(e) + ")" + where;
}
}  // namespace

struct WinListener::Impl {
  Post post;
  std::mutex m;
  SOCKET listenSock = INVALID_SOCKET;
  std::map<uint64_t, ConnState> conns;
  uint64_t nextId = 1;
  std::thread thread;
  std::atomic<bool> quit{ false };

  // under the lock: write what the socket takes now
  void flush(ConnState& c) {
    while (c.off < c.out.size()) {
      const int n = ::send(c.s, c.out.data() + c.off, (int)std::min<size_t>(c.out.size() - c.off, 1 << 20), 0);
      if (n <= 0) break;                    // would block, or broken (the reader finds out and closes it)
      c.off += (size_t)n;
    }
    if (c.off == c.out.size()) { c.out.clear(); c.off = 0; }
    else if (c.off > (1 << 20)) { c.out.erase(0, c.off); c.off = 0; }
  }

  void run() {
    std::vector<char> buf(64 * 1024);
    while (!quit) {
      fd_set rd, wr; FD_ZERO(&rd); FD_ZERO(&wr);
      std::vector<std::pair<uint64_t, SOCKET>> socks;
      std::vector<uint64_t> closing;
      { std::lock_guard<std::mutex> g(m);
        if (listenSock != INVALID_SOCKET) FD_SET(listenSock, &rd);
        for (auto& kv : conns) {
          ConnState& c = kv.second;
          if (c.stalled) continue;
          if (c.closeWhenFlushed && c.off == c.out.size()) { closing.push_back(kv.first); continue; }
          if (socks.size() >= FD_SETSIZE - 2) continue;
          FD_SET(c.s, &rd);
          if (c.off < c.out.size()) FD_SET(c.s, &wr);
          socks.emplace_back(kv.first, c.s);
        }
        for (uint64_t id : closing) { ConnState& c = conns[id]; ::shutdown(c.s, SD_SEND); ::closesocket(c.s); conns.erase(id); }
      }
      for (uint64_t id : closing) post(id, 2, nullptr, 0);
      timeval tv; tv.tv_sec = 0; tv.tv_usec = 20000;
      const int ready = ::select(0, &rd, &wr, nullptr, &tv);
      if (quit) break;
      if (ready <= 0) { if (ready < 0) Sleep(5); continue; }
      if (listenSock != INVALID_SOCKET && FD_ISSET(listenSock, &rd)) {
        for (;;) {
          SOCKET s = ::accept(listenSock, nullptr, nullptr);
          if (s == INVALID_SOCKET) break;
          u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
          BOOL on = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
          uint64_t id;
          { std::lock_guard<std::mutex> g(m); id = nextId++; conns[id].s = s; }
          post(id, 0, nullptr, 0);
        }
      }
      for (const auto& kv : socks) {
        if (FD_ISSET(kv.second, &wr)) { std::lock_guard<std::mutex> g(m); auto it = conns.find(kv.first); if (it != conns.end() && it->second.s == kv.second) flush(it->second); }
        if (!FD_ISSET(kv.second, &rd)) continue;
        const int n = ::recv(kv.second, buf.data(), (int)buf.size(), 0);
        if (n > 0) { post(kv.first, 1, buf.data(), (size_t)n); continue; }
        if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        bool had = false;
        { std::lock_guard<std::mutex> g(m); auto it = conns.find(kv.first); if (it != conns.end() && it->second.s == kv.second) { ::closesocket(kv.second); conns.erase(it); had = true; } }
        if (had) post(kv.first, 2, nullptr, 0);
      }
    }
  }
};

WinListener::WinListener(Post post) : d(new Impl) { d->post = std::move(post); }
WinListener::~WinListener() { stop(); }

bool WinListener::listen(const std::string& host, int port, std::string* error) {
  stop();
  if (!startWinsock()) { if (error) *error = "winsock is not available"; return false; }
  sockaddr_in addr; ZeroMemory(&addr, sizeof addr);
  addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
  if (port <= 0 || port > 65535 || InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1) { if (error) *error = "listen: bad address " + host + ":" + std::to_string(port); return false; }
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) { if (error) *error = sockError(WSAGetLastError(), host, port); return false; }
  BOOL on = TRUE; setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof on);
  if (::bind(s, (const sockaddr*)&addr, sizeof addr) != 0 || ::listen(s, 64) != 0) {
    const int e = WSAGetLastError(); ::closesocket(s);
    if (error) *error = sockError(e, host, port);
    return false;
  }
  u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
  d->listenSock = s; d->quit = false;
  Impl* self = d.get();
  d->thread = std::thread([self] { self->run(); });
  return true;
}
void WinListener::stop() {
  d->quit = true;
  if (d->thread.joinable()) d->thread.join();
  std::lock_guard<std::mutex> g(d->m);
  for (auto& kv : d->conns) { ::shutdown(kv.second.s, SD_SEND); ::closesocket(kv.second.s); }
  d->conns.clear();
  if (d->listenSock != INVALID_SOCKET) { ::closesocket(d->listenSock); d->listenSock = INVALID_SOCKET; }
}
void WinListener::send(uint64_t conn, const std::string& bytes) {
  std::lock_guard<std::mutex> g(d->m);
  auto it = d->conns.find(conn);
  if (it == d->conns.end() || it->second.closeWhenFlushed) return;
  ConnState& c = it->second;
  c.out += bytes;
  if (!c.stalled) d->flush(c);
}
size_t WinListener::buffered(uint64_t conn) {
  std::lock_guard<std::mutex> g(d->m);
  auto it = d->conns.find(conn);
  return it == d->conns.end() ? 0 : it->second.out.size() - it->second.off;
}
void WinListener::close(uint64_t conn, bool afterFlush) {
  std::lock_guard<std::mutex> g(d->m);
  auto it = d->conns.find(conn);
  if (it == d->conns.end()) return;
  if (!afterFlush) { it->second.out.clear(); it->second.off = 0; }
  it->second.closeWhenFlushed = true; it->second.stalled = false;
}
void WinListener::stall(uint64_t conn, bool on) {
  std::lock_guard<std::mutex> g(d->m);
  auto it = d->conns.find(conn);
  if (it != d->conns.end()) it->second.stalled = on;
}

// ---------------------------------------------------------------- Net
std::vector<std::string> WinNet::ipv4Addresses() {
  std::vector<std::string> out;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf(size);
  ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
  if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(size); rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size); }
  if (rc != NO_ERROR) return out;
  for (IP_ADAPTER_ADDRESSES* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
      char text[64] = {};
      if (InetNtopA(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, text, sizeof text)) out.push_back(text);
    }
  }
  return out;
}

// One hidden child, its stdout read through a pipe; "" on any failure or after 3 s.
static std::string runHidden(const std::wstring& exe, const std::wstring& args) {
  SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return std::string();
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr; si.hStdError = nul; si.hStdInput = nul;
  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  std::string out;
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(wr); wr = nullptr;
    const DWORD until = GetTickCount() + 3000;
    for (;;) {
      DWORD avail = 0;
      if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;         // the child closed its end
      if (avail) { char b[4096]; DWORD n = 0; if (!ReadFile(rd, b, sizeof b, &n, nullptr) || !n) break; out.append(b, n); if (out.size() > (1 << 20)) break; continue; }
      if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) { if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || !avail) break; continue; }
      if ((int)(GetTickCount() - until) > 0) { TerminateProcess(pi.hProcess, 1); out.clear(); break; }
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  }
  if (wr) CloseHandle(wr);
  CloseHandle(rd); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  return out;
}
std::string WinNet::tailscaleServeStatus() {
  for (const wchar_t* exe : { L"tailscale.exe", L"C:\\Program Files\\Tailscale\\tailscale.exe" }) {
    const std::string out = runHidden(exe, L"serve status --json");
    if (!out.empty()) return out;
  }
  return std::string();
}

}}  // namespace deck::host
