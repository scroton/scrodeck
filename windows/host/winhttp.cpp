// windows/host/winhttp.cpp — see winhttp.hpp.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "winhttp.hpp"
#include "conpty.hpp"      // widen / narrow
#include "statefile.hpp"

#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <thread>
#include <vector>

namespace deck { namespace host {

struct WinHttpGet::Impl {
  Post post;
  std::mutex m;
  std::vector<std::thread> threads;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

static core::HttpResponse fetch(const std::string& url, const std::map<std::string, std::string>& headers) {
  core::HttpResponse r;
  const std::wstring wurl = widen(url);
  URL_COMPONENTS uc; ZeroMemory(&uc, sizeof uc); uc.dwStructSize = sizeof uc;
  wchar_t hostName[256] = {}, path[2048] = {};
  uc.lpszHostName = hostName; uc.dwHostNameLength = 255; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "bad url"; return r; }
  HINTERNET session = WinHttpOpen(L"claude-deck-native", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) { r.error = "fetch failed"; return r; }
  WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);
  HINTERNET conn = WinHttpConnect(session, hostName, uc.nPort, 0);
  HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path[0] ? path : L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  if (req) {
    std::wstring head;
    for (const auto& kv : headers) head += widen(kv.first + ": " + kv.second) + L"\r\n";
    const bool sent = WinHttpSendRequest(req, head.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : head.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr);
    if (sent) {
      DWORD status = 0, size = sizeof status;
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
      r.status = (int)status;
      wchar_t retry[64] = {}; size = sizeof retry;
      if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &size, WINHTTP_NO_HEADER_INDEX)) r.headers["retry-after"] = narrow(retry);
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
        std::string chunk((size_t)avail, '\0'); DWORD got = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &got) || !got) break;
        r.body.append(chunk, 0, got);
        if (r.body.size() > (4u << 20)) break;                 // a usage answer is a few hundred bytes
      }
    } else r.error = "fetch failed";
  } else r.error = "fetch failed";
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return r;
}

WinHttpGet::WinHttpGet(Post post) : d(new Impl) { d->post = std::move(post); }
WinHttpGet::~WinHttpGet() {
  std::vector<std::thread> threads;
  { std::lock_guard<std::mutex> g(d->m); *d->alive = false; threads.swap(d->threads); }
  for (std::thread& t : threads) if (t.joinable()) t.join();
}
void WinHttpGet::get(const std::string& url, const std::map<std::string, std::string>& headers, std::function<void(const core::HttpResponse&)> done) {
  Post post = d->post; std::shared_ptr<bool> alive = d->alive;
  std::lock_guard<std::mutex> g(d->m);
  d->threads.emplace_back([url, headers, done, post, alive] {
    const core::HttpResponse r = fetch(url, headers);
    post([r, done, alive] { if (*alive) done(r); });
  });
}

// lib/platform.js readOAuth on Windows: <dir>\.credentials.json, parsed; null when there is none or it will not parse
core::json WinCredentials::readOAuth(const std::string& configDir) {
  std::string text;
  if (!readFile(joinPath(configDir, ".credentials.json"), text)) return core::json();
  const core::json j = core::json::parse(text, nullptr, false);
  return j.is_discarded() ? core::json() : j;
}
// credentialsStamp: the file's modification time, as text
std::string WinCredentials::stamp(const std::string& configDir) {
  const core::FileStat st = winFiles().stat(joinPath(configDir, ".credentials.json"));
  if (!st.exists || st.isDir) return std::string();
  return core::json(st.mtimeMs).dump();
}

}}  // namespace deck::host
