// windows/test/fakeclaude.cpp - a STAND-IN for claude.exe, for the exe UI tests that need a tile the deck takes for a
// Claude session (shell.ps1 `auditors`). It is never Claude: it runs no model, reads no account, sends nothing anywhere.
//
// The test copies it to <fake home>\.local\bin\claude.exe and names it in CLAUDE_DECK_CLAUDE as well, so the deck under
// test (--allow-claude, a scratch --state-dir and --home) starts THIS for each restored Claude tile, never the real one.
// What it does, as far as the deck can tell a Claude Code session apart:
//   - `--session-id <id>` / `--resume <id>`: its session id;
//   - the live record <home>\.claude\sessions\<pid>.json (home = two folders above this exe), as Claude Code writes it:
//     status idle at start; busy when an Enter arrives, idle again 3 s after the last input (longer than the deck's
//     2.5 s second-Enter check); busy as long as <home>\.claude\fake-typed\<session id>.busy exists (a test holds a
//     turn open);
//   - every byte typed into it (the console input, VT) appended to <home>\.claude\fake-typed\<session id>.txt, and a
//     marker <home>\.claude\fake-typed\<session id>.started, so a test can prove the stand-in ran, not Claude;
//   - a line on its screen per input, so a shot shows something.
// It ends by itself after 180 s (a test never leaves it behind), or when its console goes.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

static std::string g_dir, g_sid, g_cwd;
static std::atomic<long long> g_lastInput{ 0 };
static std::atomic<bool> g_enter{ false };
static long long nowMs() { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL; }
static std::string esc(const std::string& s) { std::string o; for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o; }
static void writeFile(const std::string& path, const std::string& text, bool append) {
  HANDLE h = CreateFileA(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD n = 0; WriteFile(h, text.data(), (DWORD)text.size(), &n, nullptr); CloseHandle(h);
}
static void record(const char* status, long long startedAt) {
  const DWORD pid = GetCurrentProcessId();
  const std::string tmp = g_dir + "\\sessions\\" + std::to_string(pid) + ".json.tmp", dst = g_dir + "\\sessions\\" + std::to_string(pid) + ".json";
  writeFile(tmp, "{\"pid\":" + std::to_string(pid) + ",\"sessionId\":\"" + esc(g_sid) + "\",\"cwd\":\"" + esc(g_cwd) + "\",\"status\":\"" + status +
    "\",\"statusUpdatedAt\":" + std::to_string(nowMs()) + ",\"startedAt\":" + std::to_string(startedAt) + ",\"kind\":\"interactive\"}", false);
  writeFile(g_dir + "\\fake-typed\\" + g_sid + ".status", std::to_string(nowMs()) + " " + status + "\n", true);
  for (int k = 0; k < 50 && !MoveFileExA(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING); k++) Sleep(10);   // the deck may be reading it
}
static void say(const std::string& s) { DWORD n = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &n, nullptr); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--session-id") || !strcmp(argv[i], "--resume")) g_sid = argv[i + 1];
  if (g_sid.empty()) g_sid = "no-session";
  char self[MAX_PATH] = { 0 }; GetModuleFileNameA(nullptr, self, MAX_PATH);
  std::string p = self;                                  // <home>\.local\bin\claude.exe -> <home>
  for (int k = 0; k < 3; k++) { const size_t sl = p.find_last_of('\\'); if (sl == std::string::npos) return 2; p = p.substr(0, sl); }
  g_dir = p + "\\.claude";
  char cwd[MAX_PATH] = { 0 }; GetCurrentDirectoryA(MAX_PATH, cwd); g_cwd = cwd;
  CreateDirectoryA(g_dir.c_str(), nullptr); CreateDirectoryA((g_dir + "\\sessions").c_str(), nullptr); CreateDirectoryA((g_dir + "\\fake-typed").c_str(), nullptr);
  const std::string typed = g_dir + "\\fake-typed\\" + g_sid + ".txt";
  writeFile(g_dir + "\\fake-typed\\" + g_sid + ".started", std::to_string(GetCurrentProcessId()) + "\n", false);
  const long long started = nowMs();
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  SetConsoleMode(in, ENABLE_VIRTUAL_TERMINAL_INPUT);     // raw: no line input, no echo, Ctrl+C as a byte
  SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
  say("fake claude (a test stand-in, not Claude) - session " + g_sid + "\r\n");
  Sleep(300);
  record("idle", started);
  std::thread([in, typed] {
    wchar_t buf[2048];
    for (;;) {
      DWORD n = 0;
      if (!ReadConsoleW(in, buf, 2048, &n, nullptr)) ExitProcess(0);
      if (!n) continue;
      const int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
      std::string s((size_t)len, '\0'); WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, &s[0], len, nullptr, nullptr);
      writeFile(typed, s, true); writeFile(g_dir + "\\fake-typed\\" + g_sid + ".status", std::to_string(nowMs()) + " input " + std::to_string(s.size()) + (s.find('\r') != std::string::npos ? " CR" : "") + "\n", true);
      if (s.find('\r') != std::string::npos) g_enter = true;
      g_lastInput = nowMs();
      say("fake claude: got " + std::to_string(s.size()) + " bytes\r\n");
    }
  }).detach();
  bool busy = false;
  const std::string hold = g_dir + "\\fake-typed\\" + g_sid + ".busy";   // while this file exists the turn runs on
  for (;;) {
    Sleep(100);
    const long long t = nowMs();
    if (t - started > 180000) ExitProcess(0);
    const bool held = GetFileAttributesA(hold.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (g_enter.exchange(false) || held) { if (!busy) { busy = true; record("busy", started); } }
    else if (busy && t - g_lastInput.load() > 3000) { busy = false; record("idle", started); }
  }
}
