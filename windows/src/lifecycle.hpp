// windows/src/lifecycle.hpp — what only the shell can do about the process itself (core/deck/deck.hpp Lifecycle),
// and the two things main.js does around it: the single-instance lock and the exit watchdog.
//
//  * RESTART (main.js restartDeck): the core has saved everything and asks; the shell closes its windows, starts
//    the deck again and leaves. Electron uses its relauncher (a helper that waits for the old process); here the
//    new process is started at once with `--wait-pid <this pid>` and does that waiting itself, before it takes the
//    single-instance lock and long before it shows a window — so the hand-over is hidden, and nothing is inherited
//    (CreateProcess with bInheritHandles FALSE: main.js's 2026-09-29 lesson about an inherited listening socket).
//    WHAT is started: `<config folder>\relaunch.json` — { "command": "...", "args": ["..."] }, main.js's own file
//    shape, in the NATIVE deck's folder — when the owner put one there (a box launcher); else this exe with the
//    arguments it was started with.
//  * THE UPDATE OFFER (main.js mainFingerprint): for one exe, "the deck's code changed on disk" is its own file's
//    size and time stamp. The core compares fingerprints and offers the restart.
//  * SINGLE INSTANCE (app.requestSingleInstanceLock / `second-instance`): a named mutex per CONFIG FOLDER (two
//    decks on one folder would both write its tiles.json); a second launch finds the first one's hub window, asks
//    it to come forward, and exits. Its own mutex name and window class — nothing of the Electron deck's.
//  * THE EXIT WATCHDOG (main.js armExitWatchdog): a thread that, 8 s after the quit began, ends the processes the
//    deck started and then this one — for a pseudoconsole that will not close, which would hang the exit in a join.
#pragma once
#include "../../core/deck/deck.hpp"
#include "../../core/deck/edition.hpp"
#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace deck {

static const wchar_t* const kHubClass = L"ClaudeDeckNativeHub";

inline std::wstring ownExePath() { wchar_t exe[32768]; const DWORD n = GetModuleFileNameW(nullptr, exe, 32768); return std::wstring(exe, n); }
// CommandLineToArgvW's inverse for one argument (the rules CreateProcess's parser reads back)
inline std::wstring quoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring o = L"\""; size_t bs = 0;
  for (wchar_t c : a) {
    if (c == L'\\') { bs++; continue; }
    if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; bs = 0; continue; }
    o.append(bs, L'\\'); bs = 0; o += c;
  }
  o.append(bs * 2, L'\\'); o += L'"';
  return o;
}
// this process's own arguments (without the exe, without an earlier --wait-pid)
inline std::vector<std::wstring> ownArgs() {
  std::vector<std::wstring> out; int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; i++) { if (std::wstring(argv[i]) == L"--wait-pid") { i++; continue; } out.push_back(argv[i]); }
  if (argv) LocalFree(argv);
  return out;
}

class SingleInstance {
 public:
  // FNV-1a of the lower-cased folder: the lock's name and the hub window's title
  static std::wstring keyFor(const std::wstring& configFolder) {
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : configFolder) { h ^= (unsigned long long)towlower(c); h *= 1099511628211ULL; }
    wchar_t b[40]; swprintf(b, 40, L"%016llx", h); return std::wstring(L"ClaudeDeckNative-") + b;
  }
  static UINT message() { static const UINT m = RegisterWindowMessageW(L"ClaudeDeckNative.SecondInstance"); return m; }
  // True = this process is the one deck on that folder. `waitMs`: how long a relaunched deck waits for the lock
  // (the old process is still on its way out).
  bool acquire(const std::wstring& key, DWORD waitMs) {
    key_ = key;
    const ULONGLONG until = GetTickCount64() + waitMs;
    for (;;) {
      mutex_ = CreateMutexW(nullptr, TRUE, (L"Local\\" + key).c_str());
      if (mutex_ && GetLastError() != ERROR_ALREADY_EXISTS) return true;
      if (mutex_) { CloseHandle(mutex_); mutex_ = nullptr; }
      if (GetTickCount64() >= until) return false;
      Sleep(100);
    }
  }
  // the first instance's hub (a message-only window titled with the key), or null
  static HWND findFirst(const std::wstring& key) { return FindWindowExW(HWND_MESSAGE, nullptr, kHubClass, key.c_str()); }
  // main.js `second-instance`: ask the first to come forward. True when it was told.
  static bool wakeFirst(const std::wstring& key) {
    HWND h = findFirst(key); if (!h) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid); if (pid) AllowSetForegroundWindow(pid);
    return PostMessageW(h, message(), 0, 0) != 0;
  }
  void release() { if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr; } }
  bool held() const { return mutex_ != nullptr; }
  const std::wstring& key() const { return key_; }
 private:
  HANDLE mutex_ = nullptr; std::wstring key_;
};

// `--wait-pid <pid>`: a relaunched deck waits for the one that started it to be gone (20 s at most).
inline bool waitForPid(DWORD pid, DWORD ms) {
  if (!pid) return true;
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid); if (!h) return true;      // gone already
  const DWORD r = WaitForSingleObject(h, ms); CloseHandle(h);
  return r == WAIT_OBJECT_0;
}

// main.js armExitWatchdog. `pids`: every process the deck started, as known when the quit began.
inline void armExitWatchdog(std::vector<uint32_t> pids, DWORD afterMs = 8000) {
  static bool armed = false; if (armed) return; armed = true;
  std::thread([pids, afterMs] {
    Sleep(afterMs);
    for (uint32_t p : pids) { HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p); if (h) { TerminateProcess(h, 1); CloseHandle(h); } }
    TerminateProcess(GetCurrentProcess(), 0);
  }).detach();
}

// A SWITCH to another deck (App::switchDeck), not a quit: the local deck's processes still must not hang its stop (a
// pseudoconsole that will not close), so whatever of them is still running after `afterMs` is ended - but this
// process lives on. The handles are opened NOW, so a pid reused after its process ended is never touched.
inline void armChildWatchdog(const std::vector<uint32_t>& pids, DWORD afterMs = 8000) {
  std::vector<HANDLE> hs;
  for (uint32_t p : pids) { HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p); if (h) hs.push_back(h); }
  if (hs.empty()) return;
  std::thread([hs, afterMs] {
    Sleep(afterMs);
    for (HANDLE h : hs) { if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) TerminateProcess(h, 1); CloseHandle(h); }
  }).detach();
}

class ShellLifecycle : public core::Lifecycle {
 public:
  // ---- set by the shell
  std::wstring configFolder;                      // where relaunch.json is looked for
  bool test = false;                              // a test run: see relaunch()
  std::function<bool(const std::string& why)> onRestart;   // close every window and leave; false = it cannot
  std::function<void(const std::string&)> log;
  std::string salt;                               // (tests) `lifecycle bump`: as if the exe had been rebuilt

  bool restart(const std::string& why) override { return onRestart ? onRestart(why) : false; }
  // ---- The rest of core::Lifecycle (deck.hpp), live since 30f68cf: the exit watchdog at shutdown() and at a restart,
  // the three "open" calls for `askOpenLink` / `openExternal`, and a window for a client the core registered itself.
  std::function<std::vector<uint32_t>()> livePids;          // every process the deck started, for the watchdog
  std::function<bool(int client, const core::json& bounds, bool inactive)> onOpenWindow;
  // the quit has begun: gone within `ms` whatever holds the process (main.js armExitWatchdog)
  void armExitWatchdog(double ms) override { if (switching) return; deck::armExitWatchdog(livePids ? livePids() : std::vector<uint32_t>(), (DWORD)(ms > 0 ? ms : 8000)); }
  // set by App::stopTransport(false) around the local deck's stop for a SWITCH to another deck: the core's shutdown
  // arms the exit watchdog (a quit's), which would end this process 8 s later; the shell armed armChildWatchdog instead
  bool switching = false;
  // shell.openExternal / openPath / showItemInFolder. A test opens nothing: it logs `OPEN …`.
  bool openExternal(const std::string& url) override { return shellOpen("OPEN ", widenUtf8(url), L""); }
  bool openPath(const std::string& path) override { return shellOpen("OPEN PATH ", widenUtf8(path), L""); }
  bool showInFolder(const std::string& path) override { return shellOpen("SHOW IN FOLDER ", L"explorer.exe", L"/select,\"" + widenUtf8(path) + L"\""); }
  // a new window of this deck for a client the core has already registered (the on-call's desk with no free cell)
  bool openWindow(int client, const core::json& bounds, bool inactive) override { return onOpenWindow ? onOpenWindow(client, bounds, inactive) : false; }
  bool shellOpen(const char* what, const std::wstring& file, const std::wstring& args) {
    if (test) { if (log) log(what + narrowUtf8(file) + (args.empty() ? "" : " " + narrowUtf8(args))); return true; }
    return (INT_PTR)ShellExecuteW(nullptr, L"open", file.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL) > 32;
  }
  // { "<the edition's exe, ClaudeDeckNative.exe>": "<size>:<last write time>" } — what main.js's mainFingerprint is for its files
  core::json fingerprint() override {
    const std::wstring exe = ownExePath();
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &d)) return core::json();
    ULARGE_INTEGER size, time; size.LowPart = d.nFileSizeLow; size.HighPart = d.nFileSizeHigh; time.LowPart = d.ftLastWriteTime.dwLowDateTime; time.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return core::json{ { DECK_EDITION_EXE, std::to_string(size.QuadPart) + ":" + std::to_string(time.QuadPart) + salt } };
  }
  // The command line a restart starts: relaunch.json's, else this exe with its own arguments; `--wait-pid` added
  // only for this exe (a launcher of the owner's does its own waiting, as it does for the Electron deck).
  struct Spec { std::wstring exe, commandLine; bool fromFile = false; };
  Spec spec() const {
    Spec s; const std::wstring self = ownExePath();
    std::vector<std::wstring> args; std::wstring exe = self;
    { std::ifstream f((configFolder + L"\\relaunch.json").c_str(), std::ios::binary);
      if (f) { std::stringstream ss; ss << f.rdbuf(); std::string t = ss.str(); if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t = t.substr(3);
        const core::json r = core::json::parse(t, nullptr, false);
        if (r.is_object() && r.contains("command") && r["command"].is_string() && !r["command"].get<std::string>().empty()) {
          exe = widenUtf8(r["command"].get<std::string>()); s.fromFile = true;
          if (r.contains("args") && r["args"].is_array()) for (const auto& a : r["args"]) args.push_back(widenUtf8(a.is_string() ? a.get<std::string>() : a.dump()));
        } } }
    if (!s.fromFile) args = ownArgs();
    const bool own = CompareStringOrdinal(exe.c_str(), -1, self.c_str(), -1, TRUE) == CSTR_EQUAL;
    s.exe = exe; s.commandLine = quoteArg(exe);
    for (const auto& a : args) s.commandLine += L" " + quoteArg(a);
    if (own) s.commandLine += L" --wait-pid " + std::to_wstring(GetCurrentProcessId());
    return s;
  }
  // Start it. Hidden whatever it is (CREATE_NO_WINDOW: a console program named in relaunch.json gets no console
  // window; this exe has none anyway), nothing inherited. A TEST starts nothing but this very exe, and only when
  // its own relaunch.json (in its --state-dir) says with what — a test that relaunched itself with its own
  // arguments would run its script again, for ever; without the file it only logs what a real run would start.
  bool relaunch(std::string* said = nullptr) {
    const Spec s = spec();
    const std::wstring self = ownExePath();
    const bool own = CompareStringOrdinal(s.exe.c_str(), -1, self.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (said) *said = narrowUtf8(s.commandLine);
    if (test && !(s.fromFile && own)) { if (log) log("relaunch (not started in a test): " + narrowUtf8(s.commandLine)); return true; }
    std::wstring cl = s.commandLine; cl.push_back(L'\0');
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    // a bare name (wscript.exe) is found on the Path: no application name is given
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
      if (log) log("relaunch FAILED (" + std::to_string(GetLastError()) + "): " + narrowUtf8(s.commandLine));
      return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (log) log("relaunch started: " + narrowUtf8(s.commandLine));
    return true;
  }
  static std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
  }
  static std::string narrowUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
  }
};

}  // namespace deck
