// windows/src/update.hpp - the exe updates itself from the PUBLIC releases repo (2026-11-19, an ask page:
// "a separate PUBLIC repo only for releases"; the code repo stays private). Off the window thread:
//   1. the latest release of the edition's releases repo (DECK_EDITION_RELEASES_REPO, core/deck/edition.hpp; GitHub's API; or, for a test, the folder named by
//      CLAUDE_DECK_UPDATE_FROM holding the same files);
//   2. SHA256SUMS and SHA256SUMS.asc, and the signature checked with gpg (Git for Windows ships it) in a throwaway
//      keyring holding ONLY the edition's release key (KEYS, inside the exe as RCDATA 2100): it must say VALIDSIG with the
//      key's fingerprint, or nothing more happens;
//   3. this exe's own SHA-256 in that list = up to date; else the release's exe is downloaded, its SHA-256 must be the
//      list's, and it is swapped in: the running exe is renamed to <the exe>.old (Windows lets a running
//      image be renamed; the next start deletes it) and the new one takes its name. It runs from the next start.
// gpg runs hidden (CREATE_NO_WINDOW). No gpg on the machine = no update, and the check says so.
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <functional>
#include <fstream>
#include <sstream>
#include "../../core/deck/hash.hpp"
#include "../../core/deck/edition.hpp"
#include "../../core/third_party/nlohmann/json.hpp"

namespace update {

// the edition's (core/deck/edition.hpp; 2026-11-24 ask r4 Q4): each build reads its own releases repo and trusts
// only its own release key (the owner's copy and the public one have different ones)
static const char* const kLatestApi = "https://api.github.com/repos/" DECK_EDITION_RELEASES_REPO "/releases/latest";
static const char* const kFingerprint = DECK_EDITION_RELEASE_KEY;
static const char* const kExeName = DECK_EDITION_EXE;

struct Result { int state = 0; std::string why, version; };   // 1 up to date, 2 updated (restart to use it), -1 not done (why)

inline std::wstring wide(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w; }
inline std::string utf8(const std::wstring& w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s; }

// an https GET, redirects followed (a release file redirects to GitHub's storage), at most `cap` bytes
inline bool httpGet(const std::string& url, std::string& body, std::string& err, size_t cap) {
  body.clear();
  const std::wstring w = wide(url);
  URL_COMPONENTS uc; ZeroMemory(&uc, sizeof uc); uc.dwStructSize = sizeof uc;
  wchar_t host[256] = {}, path[2048] = {}; uc.lpszHostName = host; uc.dwHostNameLength = 255; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) { err = "bad address"; return false; }
  HINTERNET s = WinHttpOpen(L"ClaudeDeckNative-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) { err = "no network"; return false; }
  WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path[0] ? path : L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  bool ok = false;
  if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
      DWORD avail = 0; if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
      std::string chunk(avail, '\0'); DWORD got = 0;
      if (!WinHttpReadData(r, &chunk[0], avail, &got) || !got) break;
      body.append(chunk, 0, got);
      if (body.size() > cap) { err = "too large"; break; }
    }
    ok = status == 200 && err.empty();
    if (!ok && err.empty()) err = "HTTP " + std::to_string(status);
  } else err = "no answer";
  if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
  return ok;
}
inline bool readFile(const std::wstring& p, std::string& out) { std::ifstream f(p.c_str(), std::ios::binary); if (!f) return false; std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true; }
inline bool writeFile(const std::wstring& p, const std::string& data) { std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc); if (!f) return false; f.write(data.data(), (std::streamsize)data.size()); return (bool)f; }

inline std::wstring findGpg() {
  wchar_t buf[MAX_PATH] = {};
  if (SearchPathW(nullptr, L"gpg.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
  for (const wchar_t* p : { L"C:\\Program Files\\Git\\usr\\bin\\gpg.exe", L"C:\\Program Files (x86)\\Git\\usr\\bin\\gpg.exe", L"C:\\Program Files\\GnuPG\\bin\\gpg.exe" })
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
  return L"";
}
// a program run hidden, its stdout + stderr captured; the exit code (-1 = could not start)
inline int runHidden(const std::wstring& cmd, std::string& out) {
  SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE }; HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {}; std::wstring c = cmd;
  if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return -1; }
  CloseHandle(wr);
  char b[4096]; DWORD n = 0; while (ReadFile(rd, b, sizeof b, &n, nullptr) && n) out.append(b, n);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, 60000); DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  return (int)code;
}
// SHA256SUMS.asc must be a good signature by the project's release key, checked in a keyring that holds nothing else
inline bool verifySums(const std::string& keys, const std::string& sums, const std::string& sig, std::string& why) {
  const std::wstring gpg = findGpg();
  if (gpg.empty()) { why = "no gpg on this machine (Git for Windows has it): updates need it to check the signature"; return false; }
  wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
  const std::wstring dir = std::wstring(tmp) + L"claude-deck-update-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring home = dir + L"\\gnupg"; CreateDirectoryW(home.c_str(), nullptr);
  bool ok = writeFile(dir + L"\\KEYS", keys) && writeFile(dir + L"\\SHA256SUMS", sums) && writeFile(dir + L"\\SHA256SUMS.asc", sig);
  std::string out;
  if (ok) {
    // Git for Windows' gpg is an MSYS program: it takes /c/... paths, not C:\... (a Gpg4win gpg takes either)
    std::wstring gl = gpg; for (auto& ch : gl) ch = (wchar_t)towlower(ch);
    const bool msys = gl.find(L"\\usr\\bin\\") != std::wstring::npos;
    auto P = [msys](std::wstring w) {
      if (!msys) return w;
      for (auto& ch : w) if (ch == L'\\') ch = L'/';
      if (w.size() > 1 && w[1] == L':') w = L"/" + std::wstring(1, (wchar_t)towlower(w[0])) + w.substr(2);
      return w;
    };
    runHidden(L"\"" + gpg + L"\" --homedir \"" + P(home) + L"\" --batch --quiet --import \"" + P(dir + L"\\KEYS") + L"\"", out);
    out.clear();
    runHidden(L"\"" + gpg + L"\" --homedir \"" + P(home) + L"\" --batch --status-fd 1 --verify \"" + P(dir + L"\\SHA256SUMS.asc") + L"\" \"" + P(dir + L"\\SHA256SUMS") + L"\"", out);
    ok = out.find(std::string("[GNUPG:] VALIDSIG ") + kFingerprint) != std::string::npos;
    if (!ok) why = "the release's signature is not by its release key: not installed";
  } else why = "could not write a temp folder";
  // the throwaway keyring goes (gpg's agent may hold a lock a moment: best effort)
  runHidden(L"cmd.exe /c rmdir /s /q \"" + dir + L"\"", out);
  return ok;
}
// SHA256SUMS: "<hex>  <name>" lines
inline std::string sumFor(const std::string& sums, const std::string& name) {
  std::istringstream in(sums); std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t sp = line.find(' '); if (sp == std::string::npos || sp != 64) continue;
    std::string n = line.substr(sp); while (!n.empty() && (n[0] == ' ' || n[0] == '*')) n.erase(0, 1);
    if (n == name) return line.substr(0, 64);
  }
  return "";
}
inline std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

// The whole check, on a worker thread. `from` = "" for GitHub, else a folder (tests). `log` gets one line per step.
inline Result check(const std::wstring& exePath, const std::string& keys, const std::wstring& from, const std::function<void(const std::string&)>& log) {
  Result res; std::string err;
  auto fail = [&](const std::string& w) { res.state = -1; res.why = w; if (log) log("update: " + w); return res; };
  std::string sums, sig, exeUrl;
  if (from.empty()) {
    std::string api;
    if (!httpGet(kLatestApi, api, err, 1u << 20)) return fail("the releases page did not answer (" + err + ")");
    const nlohmann::json j = nlohmann::json::parse(api, nullptr, false);
    if (!j.is_object()) return fail("the releases page's answer was not understood");
    res.version = j.value("tag_name", std::string());
    if (log) log("update: the latest release is " + (res.version.empty() ? std::string("(no tag)") : res.version));
    std::string sumsUrl, sigUrl;
    if (j.contains("assets") && j["assets"].is_array()) for (const auto& a : j["assets"]) {
      const std::string n = a.value("name", std::string()), u = a.value("browser_download_url", std::string());
      if (n == "SHA256SUMS") sumsUrl = u; else if (n == "SHA256SUMS.asc") sigUrl = u; else if (n == kExeName) exeUrl = u;
    }
    if (sumsUrl.empty() || sigUrl.empty() || exeUrl.empty()) return fail("the latest release has no signed exe");
    if (!httpGet(sumsUrl, sums, err, 1u << 16) || !httpGet(sigUrl, sig, err, 1u << 16)) return fail("the release's hash list did not download (" + err + ")");
    if (log) log("update: downloaded SHA256SUMS and its signature");
  } else {
    res.version = "(test folder)";
    if (!readFile(from + L"\\SHA256SUMS", sums) || !readFile(from + L"\\SHA256SUMS.asc", sig)) return fail("the test folder has no SHA256SUMS / .asc");
  }
  std::string why;
  if (!verifySums(keys, sums, sig, why)) return fail("signature check FAILED: " + why);
  if (log) log("update: SHA256SUMS is signed by the release key (" + std::string(kFingerprint) + ")");
  const std::string want = lower(sumFor(sums, kExeName));
  if (want.size() != 64) return fail("the signed list has no line for " + std::string(kExeName));
  std::string mine; if (!readFile(exePath, mine)) return fail("could not read this exe");
  if (deck::core::hexOf(deck::core::sha256(mine)) == want) { res.state = 1; if (log) log("update: up to date (" + want.substr(0, 12) + ")"); return res; }
  std::string fresh;
  if (log) log("update: downloading the new exe (" + want.substr(0, 12) + ")");
  if (from.empty()) { if (!httpGet(exeUrl, fresh, err, 80u << 20)) return fail("the new exe did not download (" + err + ")"); }
  else if (!readFile(from + L"\\" + wide(kExeName), fresh)) return fail("the test folder has no exe");
  if (deck::core::hexOf(deck::core::sha256(fresh)) != want) return fail("the downloaded exe is not the one the signed list names: not installed");
  if (log) log("update: downloaded, " + std::to_string(fresh.size()) + " bytes, its hash is the signed one");
  const std::wstring next = exePath + L".new", old = exePath + L".old";
  if (!writeFile(next, fresh)) return fail("could not write beside the exe (" + utf8(next) + ")");
  DeleteFileW(old.c_str());
  if (!MoveFileExW(exePath.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(next.c_str()); return fail("could not move the running exe aside"); }
  if (!MoveFileExW(next.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING)) { MoveFileExW(old.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING); DeleteFileW(next.c_str()); return fail("could not put the new exe in place"); }
  res.state = 2; if (log) log("update: staged - the new exe (" + want.substr(0, 12) + ") is in place; it runs from the next start");
  return res;
}

}  // namespace update
