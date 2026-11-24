// windows/host/winseams.cpp — see winseams.hpp.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "winseams.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <random>

namespace deck { namespace host {

void WinFileLog::line(const std::string& text) {
  const std::wstring w = widen(path_);
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fa) && ((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) >= cap_)
    MoveFileExW(w.c_str(), widen(path_ + ".1").c_str(), MOVEFILE_REPLACE_EXISTING);
  HANDLE h = CreateFileW(w.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  const std::string out = text + "\r\n";
  DWORD n = 0; WriteFile(h, out.data(), (DWORD)out.size(), &n, nullptr);
  CloseHandle(h);
}

std::string envVar(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (!n) return std::string();
  std::wstring w((size_t)n, L'\0');
  n = GetEnvironmentVariableW(name, &w[0], n);
  w.resize(n);
  return narrow(w);
}

// ---------------------------------------------------------------- Random
std::string WinRandom::hex(int bytes) {
  static std::random_device rd;
  static const char d[] = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < bytes; i++) { const unsigned v = rd() & 0xFF; s += d[v >> 4]; s += d[v & 15]; }
  return s;
}
std::string WinRandom::uuid() {            // crypto.randomUUID(): version 4, variant 1
  std::string h = hex(16);
  h[12] = '4';
  h[16] = "89ab"[(h[16] >= 'a' ? h[16] - 'a' + 10 : h[16] - '0') & 3];
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

// ---------------------------------------------------------------- Env
core::EnvList WinEnv::environment() {
  core::EnvList out;
  for (const std::wstring& e : currentEnvironment()) {
    const size_t eq = e.find(L'=', 1);
    if (eq == std::wstring::npos) continue;
    out.emplace_back(narrow(e.substr(0, eq)), narrow(e.substr(eq + 1)));
  }
  return out;
}
std::string WinEnv::get(const std::string& name) { return envVar(widen(name).c_str()); }
std::string WinEnv::home() { return !home_.empty() ? home_ : envVar(L"USERPROFILE"); }
uint32_t WinEnv::pid() { return GetCurrentProcessId(); }
std::string WinEnv::resolvePath(const std::string& p) {
  const std::wstring wc = widen(p); wchar_t full[32768];
  const DWORD n = GetFullPathNameW(wc.c_str(), 32768, full, nullptr);
  return n && n < 32768 ? narrow(std::wstring(full, n)) : p;
}

// ---------------------------------------------------------------- ProcessList
bool WinProcessList::alive(double pid) { return pid > 0 && pidAlive((uint32_t)pid); }
bool WinProcessList::parents(std::map<uint32_t, uint32_t>& out) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
  if (Process32FirstW(snap, &pe)) do { if (pe.th32ProcessID) out[pe.th32ProcessID] = pe.th32ParentProcessID; } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
  return !out.empty();
}
// One deck at a time (core/deck/onedeck.hpp): every process, its parent, its file's name and full path ("" where
// Windows will not tell: another user's process, an elevated one). Taken once at the deck's start.
bool WinProcessList::snapshot(std::vector<core::ProcInfo>& out) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
  if (Process32FirstW(snap, &pe)) do {
    if (!pe.th32ProcessID) continue;
    core::ProcInfo p; p.pid = pe.th32ProcessID; p.ppid = pe.th32ParentProcessID; p.name = narrow(pe.szExeFile);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
    if (h) { wchar_t buf[32768]; DWORD n = 32768; if (QueryFullProcessImageNameW(h, 0, buf, &n)) p.path = narrow(std::wstring(buf, n)); CloseHandle(h); }
    out.push_back(std::move(p));
  } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
  return !out.empty();
}
// Every process on one pseudoconsole reports the same console host pid (NtQueryInformationProcess class 49 =
// ProcessConsoleHostProcess; the low two bits are flags). lib/platform.js asks a hidden PowerShell; here it is
// the same call made directly.
bool WinProcessList::consoleHosts(const std::vector<uint32_t>& pids, std::map<uint32_t, int64_t>& out) {
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  if (!nt) return false;
  typedef LONG(WINAPI* Query)(HANDLE, int, void*, ULONG, ULONG*);
  const Query query = (Query)(void*)GetProcAddress(nt, "NtQueryInformationProcess");
  if (!query) return false;
  for (uint32_t pid : pids) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { out[pid] = -1; continue; }
    ULONG_PTR v = 0; ULONG ret = 0;
    out[pid] = query(h, 49, &v, sizeof v, &ret) == 0 ? (int64_t)(v & ~(ULONG_PTR)3) : -2;
    CloseHandle(h);
  }
  return !out.empty();
}
void WinProcessList::killTree(uint32_t pid) {                 // taskkill /PID <pid> /T /F, without starting a process
  if (!pid) return;
  std::vector<uint32_t> all = descendantsOf(pid);
  all.push_back(pid);
  for (uint32_t p : all) { HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p); if (h) { TerminateProcess(h, 1); CloseHandle(h); } }
}
bool WinProcessList::commit(double& used, double& limit) {    // Win32_OperatingSystem TotalVirtualMemorySize / FreeVirtualMemory
  MEMORYSTATUSEX m; m.dwLength = sizeof m;
  if (!GlobalMemoryStatusEx(&m) || !m.ullTotalPageFile) return false;
  limit = (double)m.ullTotalPageFile; used = (double)(m.ullTotalPageFile - m.ullAvailPageFile);
  return true;
}

// ---------------------------------------------------------------- PtyHost
namespace {
class WinPty : public core::Pty {
 public:
  WinPty(std::unique_ptr<ConPty> p, uint64_t token, WinPtyHost::Trace trace) : p_(std::move(p)), token_(token), trace_(std::move(trace)) {}
  uint32_t pid() const override { return p_->pid(); }
  int cols() const override { return p_->cols(); }
  int rows() const override { return p_->rows(); }
  bool write(const std::string& bytes) override { if (trace_) trace_(token_, "write", bytes); return p_->write(bytes); }
  bool resize(int cols, int rows) override { if (trace_) trace_(token_, "resize", std::to_string(cols) + " x " + std::to_string(rows)); return p_->resize(cols, rows); }
  void kill() override { if (trace_) trace_(token_, "kill", p_->exited() ? "already exited" : ""); p_->kill(); }
  bool exited() const override { return p_->exited(); }
  std::vector<uint32_t> processTree() const override { return p_->processTree(); }
 private:
  std::unique_ptr<ConPty> p_;
  uint64_t token_;
  WinPtyHost::Trace trace_;
};
}  // namespace

std::unique_ptr<core::Pty> WinPtyHost::spawn(const core::PtySpawn& spec, uint64_t token, std::string* error) {
  PtySpec ps;
  ps.file = widen(spec.file);
  for (const std::string& a : spec.args) ps.args.push_back(widen(a));
  for (const auto& kv : spec.env) ps.env.push_back(widen(kv.first + "=" + kv.second));
  ps.cwd = widen(spec.cwd); ps.cols = spec.cols; ps.rows = spec.rows;
  Post post = post_;
  std::unique_ptr<ConPty> p = ConPty::spawn(ps,
    [post, token](const char* b, size_t n) { post(token, false, 0, b, n); },
    [post, token](uint32_t code) { post(token, true, code, nullptr, 0); }, error);
  if (trace_) trace_(token, p ? "spawn" : "spawn failed", spec.file + (p ? " pid " + std::to_string(p->pid()) : std::string(error ? ": " + *error : std::string())));
  if (!p) return nullptr;
  return std::unique_ptr<core::Pty>(new WinPty(std::move(p), token, trace_));
}

}}  // namespace deck::host
