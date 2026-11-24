// windows/web/webview2_loader.cpp — see webview2_loader.hpp. Ported from webview/webview's loader.hh (MIT).
#include "webview2_loader.hpp"
#include <objbase.h>
#include "../third_party/webview2/WebView2.h"
#include <vector>

namespace deck { namespace web {

namespace {

const wchar_t* kStableGuid = L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

bool regString(HKEY root, const std::wstring& sub, const wchar_t* name, std::wstring& out) {
  HKEY k = nullptr;
  if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) return false;
  DWORD type = 0, bytes = 0;
  bool ok = false;
  if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(k, name, nullptr, nullptr, (LPBYTE)buf.data(), &bytes) == ERROR_SUCCESS) { out = buf.data(); ok = !out.empty(); }
  }
  RegCloseKey(k);
  return ok;
}

bool isFile(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

std::wstring lastPart(const std::wstring& p) {
  std::wstring s = p; while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  size_t i = s.find_last_of(L"\\/");
  return i == std::wstring::npos ? s : s.substr(i + 1);
}

std::wstring dllIn(const std::wstring& dir) {
  std::wstring d = dir; if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
#if defined(_M_ARM64) || defined(__aarch64__)
  return d + L"EBWebView\\arm64\\EmbeddedBrowserWebView.dll";
#elif defined(_WIN64)
  return d + L"EBWebView\\x64\\EmbeddedBrowserWebView.dll";
#else
  return d + L"EBWebView\\x86\\EmbeddedBrowserWebView.dll";
#endif
}

// a.b.c.d >= 86.0.616.0, the first runtime with the WebView2 API (the loader's own floor)
bool versionOk(const std::wstring& v) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (swscanf(v.c_str(), L"%u.%u.%u.%u", &a, &b, &c, &d) < 3) return false;
  if (a != 86) return a > 86;
  if (b != 0) return true;
  return c >= 616;
}

bool tryDir(const std::wstring& dir, RuntimeInfo& out) {
  if (dir.empty()) return false;
  const std::wstring dll = dllIn(dir);
  if (!isFile(dll)) return false;
  const std::wstring ver = lastPart(dir);
  if (!versionOk(ver)) return false;
  out.found = true; out.dir = dir; out.dll = dll; out.version = ver;
  return true;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateInternalFn)(bool, int /* 0 = installed, 1 = embedded (fixed) */, PCWSTR, IUnknown*,
                                                     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

}  // namespace

RuntimeInfo findRuntime(const std::wstring& fixedDir) {
  RuntimeInfo r;
  if (!fixedDir.empty()) {
    // a fixed-version folder is named after anything; its version is not in the path
    const std::wstring dll = dllIn(fixedDir);
    if (isFile(dll)) { r.found = true; r.fixed = true; r.dir = fixedDir; r.dll = dll; r.version = lastPart(fixedDir); }
    return r;
  }
  for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
    std::wstring v;
    if (regString(root, std::wstring(L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\") + kStableGuid, L"EBWebView", v) && tryDir(v, r)) return r;
    std::wstring loc, pv;
    const std::wstring clients = std::wstring(L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\") + kStableGuid;
    if (regString(root, clients, L"pv", pv) && pv != L"0.0.0.0") {
      if (regString(root, clients, L"location", loc)) { if (loc.back() != L'\\') loc += L'\\'; if (tryDir(loc + pv, r)) return r; }
      // no `location`: where the per-machine installer puts it
      wchar_t pf[MAX_PATH] = L"";
      if (GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH) && tryDir(std::wstring(pf) + L"\\Microsoft\\EdgeWebView\\Application\\" + pv, r)) return r;
      if (root == HKEY_CURRENT_USER && GetEnvironmentVariableW(L"LOCALAPPDATA", pf, MAX_PATH) && tryDir(std::wstring(pf) + L"\\Microsoft\\EdgeWebView\\Application\\" + pv, r)) return r;
    }
  }
  return r;
}

HRESULT createEnvironment(const RuntimeInfo& rt, const std::wstring& userDataDir, ICoreWebView2EnvironmentOptions* options,
                          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) {
  if (!rt.found) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  // never unloaded: the environment's objects live in it for the life of the process
  HMODULE mod = LoadLibraryExW(rt.dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!mod) return HRESULT_FROM_WIN32(GetLastError());
  CreateInternalFn fn = (CreateInternalFn)(void*)GetProcAddress(mod, "CreateWebViewEnvironmentWithOptionsInternal");
  if (!fn) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  return fn(true, rt.fixed ? 1 : 0, userDataDir.empty() ? nullptr : userDataDir.c_str(), (IUnknown*)options, handler);
}

} }  // namespace deck::web
