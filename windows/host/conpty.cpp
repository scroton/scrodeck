// windows/host/conpty.cpp — see conpty.hpp. Win32 only; no third-party code.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "conpty.hpp"

#include <windows.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace deck { namespace host {

// Declared here rather than taken from the SDK headers, so the file builds against any MinGW header set
// (node-pty does the same for SDKs older than RS5).
typedef void* HPCON_;
typedef HRESULT(WINAPI* PfnCreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON_*);
typedef HRESULT(WINAPI* PfnResizePseudoConsole)(HPCON_, COORD);
typedef void(WINAPI* PfnClosePseudoConsole)(HPCON_);
static const DWORD_PTR kAttrPseudoConsole = 0x00020016;   // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE

struct ConPtyApi {
  PfnCreatePseudoConsole create = nullptr;
  PfnResizePseudoConsole resize = nullptr;
  PfnClosePseudoConsole close = nullptr;
  ConPtyApi() {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");          // the system ConPTY, as node-pty with useConptyDll off
    if (!k) return;
    create = (PfnCreatePseudoConsole)(void*)GetProcAddress(k, "CreatePseudoConsole");
    resize = (PfnResizePseudoConsole)(void*)GetProcAddress(k, "ResizePseudoConsole");
    close = (PfnClosePseudoConsole)(void*)GetProcAddress(k, "ClosePseudoConsole");
  }
  bool ok() const { return create && resize && close; }
};
static const ConPtyApi& api() { static ConPtyApi a; return a; }

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

// node-pty src/windowsPtyAgent.ts argsToCommandLine, line for line.
std::wstring argsToCommandLine(const std::wstring& file, const std::vector<std::wstring>& args) {
  std::vector<std::wstring> argv;
  argv.push_back(file);
  for (const auto& a : args) argv.push_back(a);
  std::wstring result;
  for (size_t argIndex = 0; argIndex < argv.size(); argIndex++) {
    if (argIndex > 0) result += L' ';
    const std::wstring& arg = argv[argIndex];
    // JS: arg[0] / arg[arg.length - 1] of an empty string are undefined, which is !== '"'
    const bool firstIsQuote = !arg.empty() && arg.front() == L'"';
    const bool lastIsQuote = !arg.empty() && arg.back() == L'"';
    const bool hasLopsidedEnclosingQuote = (!firstIsQuote) != (!lastIsQuote);
    const bool hasNoEnclosingQuotes = !firstIsQuote && !lastIsQuote;
    const bool quote = arg.empty()
      || ((arg.find(L' ') != std::wstring::npos || arg.find(L'\t') != std::wstring::npos)
          && (arg.size() > 1 && (hasLopsidedEnclosingQuote || hasNoEnclosingQuotes)));
    if (quote) result += L'"';
    size_t bsCount = 0;
    for (wchar_t p : arg) {
      if (p == L'\\') bsCount++;
      else if (p == L'"') { result.append(bsCount * 2 + 1, L'\\'); result += L'"'; bsCount = 0; }
      else { result.append(bsCount, L'\\'); bsCount = 0; result += p; }
    }
    if (quote) { result.append(bsCount * 2, L'\\'); result += L'"'; }
    else result.append(bsCount, L'\\');
  }
  return result;
}

std::wstring environmentBlock(const std::vector<std::wstring>& env) {
  std::wstring block;
  if (env.empty()) return block;
  for (const auto& e : env) { block += e; block += L'\0'; }
  block += L'\0';
  return block;
}

std::vector<std::wstring> currentEnvironment() {
  std::vector<std::wstring> out;
  LPWCH block = GetEnvironmentStringsW();
  if (!block) return out;
  for (const wchar_t* p = block; *p;) {
    std::wstring e(p);
    p += e.size() + 1;
    if (!e.empty() && e[0] != L'=') out.push_back(e);
  }
  FreeEnvironmentStringsW(block);
  return out;
}

static bool fileExists(const std::wstring& f) {
  DWORD attr = GetFileAttributesW(f.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}
// node-pty src/win/path_util.cc get_shell_path, with its two oddities kept: a relative name that exists as
// given (in the current directory) answers "" and so reads as not found, and the Path entry after the last
// ';' is never looked at.
static std::wstring getShellPath(const std::wstring& filename) {
  std::wstring shellpath;
  if (fileExists(filename)) return shellpath;
  DWORD need = GetEnvironmentVariableW(L"Path", nullptr, 0);
  if (!need) return shellpath;
  std::wstring buffer((size_t)need, L'\0');
  DWORD got = GetEnvironmentVariableW(L"Path", &buffer[0], need);
  buffer.resize(got);
  size_t pos = 0;
  while ((pos = buffer.find(L';')) != std::wstring::npos) {
    std::wstring dir = buffer.substr(0, pos);
    buffer.erase(0, pos + 1);
    wchar_t searchPath[MAX_PATH];
    if (!PathCombineW(searchPath, dir.c_str(), filename.c_str())) continue;
    if (fileExists(searchPath)) { shellpath = searchPath; break; }
  }
  return shellpath;
}

bool pidAlive(uint32_t pid) {
  if (!pid) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;      // exists, not ours (process.kill(pid, 0) -> EPERM)
  DWORD code = 0;
  bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
}

static uint64_t createdAt(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return 0;
  FILETIME c, e, k, u; uint64_t t = 0;
  if (GetProcessTimes(h, &c, &e, &k, &u)) t = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
  CloseHandle(h);
  return t;
}

std::vector<uint32_t> descendantsOf(uint32_t root) {
  std::vector<uint32_t> out;
  if (!root) return out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  std::multimap<DWORD, DWORD> kids;
  PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) do { if (pe.th32ProcessID) kids.insert({ pe.th32ParentProcessID, pe.th32ProcessID }); } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
  std::vector<uint32_t> todo{ root };
  for (size_t i = 0; i < todo.size(); i++) {
    const uint64_t parentBorn = createdAt(todo[i]);
    auto range = kids.equal_range(todo[i]);
    for (auto it = range.first; it != range.second; ++it) {
      const DWORD kid = it->second;
      if (kid == root) continue;
      bool seen = false; for (uint32_t x : todo) if (x == kid) { seen = true; break; }
      if (seen) continue;
      // a parent pid can be a REUSED one: a real child is never older than its parent
      const uint64_t born = createdAt(kid);
      if (parentBorn && born && born < parentBorn) continue;
      todo.push_back(kid); out.push_back(kid);
    }
  }
  return out;
}

struct ConPty::Impl {
  std::mutex m;                       // guards hpc and hIn
  HPCON_ hpc = nullptr;
  HANDLE hIn = nullptr;               // our end: what we write is the child's input
  HANDLE hOut = nullptr;              // our end: the child's output
  HANDLE hProcess = nullptr;
  DWORD pid = 0;
  std::atomic<int> cols{ 0 }, rows{ 0 };
  std::atomic<bool> exited{ false };
  std::thread reader, waiter;
  DataFn onData;
  ExitFn onExit;

  // Closes the pseudoconsole (once). Its conhost flushes what is left and lets go of the output pipe, which
  // is what ends the reader. Never under the lock: on older Windows the call waits for conhost to exit.
  void closePseudoConsole() {
    HPCON_ pc = nullptr; HANDLE in = nullptr;
    { std::lock_guard<std::mutex> g(m); pc = hpc; hpc = nullptr; in = hIn; hIn = nullptr; }
    if (in) CloseHandle(in);
    if (pc) api().close(pc);
  }
};

ConPty::ConPty() : d(new Impl) {}

std::unique_ptr<ConPty> ConPty::spawn(const PtySpec& spec, DataFn onData, ExitFn onExit, std::string* error) {
  auto fail = [&](const std::string& why) { if (error) *error = why; return std::unique_ptr<ConPty>(); };
  auto failCode = [&](const char* what) { return fail(std::string(what) + ", error code: " + std::to_string(GetLastError())); };
  if (!api().ok()) return fail("Cannot launch conpty");

  // node-pty conpty.cc PtyStartProcess: the lookup that decides "File not found"
  std::wstring shellpath = PathIsRelativeW(spec.file.c_str()) ? getShellPath(spec.file) : spec.file;
  if (shellpath.empty() || !fileExists(shellpath)) return fail("File not found: " + narrow(shellpath));

  HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
  if (!CreatePipe(&inRead, &inWrite, nullptr, 0)) return failCode("Cannot launch conpty");
  if (!CreatePipe(&outRead, &outWrite, nullptr, 128 * 1024)) { CloseHandle(inRead); CloseHandle(inWrite); return failCode("Cannot launch conpty"); }

  const int cols = spec.cols > 0 ? spec.cols : 80, rows = spec.rows > 0 ? spec.rows : 24;   // node-pty DEFAULT_COLS / ROWS
  HPCON_ hpc = nullptr;
  COORD size; size.X = (SHORT)cols; size.Y = (SHORT)rows;
  HRESULT hr = api().create(size, inRead, outWrite, 0, &hpc);
  if (FAILED(hr)) { CloseHandle(inRead); CloseHandle(inWrite); CloseHandle(outRead); CloseHandle(outWrite); return fail("Cannot launch conpty"); }

  auto closeAll = [&] { api().close(hpc); CloseHandle(inRead); CloseHandle(inWrite); CloseHandle(outRead); CloseHandle(outWrite); };

  std::wstring cmdline = argsToCommandLine(spec.file, spec.args);
  std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end()); mutableCmd.push_back(L'\0');
  std::wstring envBlock = environmentBlock(spec.env);

  STARTUPINFOEXW si; ZeroMemory(&si, sizeof(si));
  si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;            // three null handles: nothing of ours is inherited
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<BYTE> attrs(attrSize);
  si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrs.data();
  if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize)) { auto r = failCode("InitializeProcThreadAttributeList failed"); closeAll(); return r; }
  if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, kAttrPseudoConsole, hpc, sizeof(HPCON_), nullptr, nullptr)) {
    auto r = failCode("UpdateProcThreadAttribute failed"); DeleteProcThreadAttributeList(si.lpAttributeList); closeAll(); return r;
  }

  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
  BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
    envBlock.empty() ? nullptr : (LPVOID)envBlock.data(),
    spec.cwd.empty() ? nullptr : spec.cwd.c_str(), &si.StartupInfo, &pi);
  DWORD createErr = GetLastError();
  DeleteProcThreadAttributeList(si.lpAttributeList);
  if (!ok) { SetLastError(createErr); auto r = failCode("Cannot create process"); closeAll(); return r; }
  CloseHandle(pi.hThread);
  // the pseudoconsole holds its own copies of these two ends now
  CloseHandle(inRead); CloseHandle(outWrite);

  std::unique_ptr<ConPty> p(new ConPty());
  Impl* d = p->d.get();
  d->hpc = hpc; d->hIn = inWrite; d->hOut = outRead; d->hProcess = pi.hProcess; d->pid = pi.dwProcessId;
  d->cols = cols; d->rows = rows;
  d->onData = std::move(onData); d->onExit = std::move(onExit);

  d->reader = std::thread([d] {
    std::vector<char> buf(64 * 1024);
    for (;;) {
      DWORD n = 0;
      if (!ReadFile(d->hOut, buf.data(), (DWORD)buf.size(), &n, nullptr) || n == 0) break;   // ERROR_BROKEN_PIPE: the console is gone
      if (d->onData) d->onData(buf.data(), n);
    }
  });
  d->waiter = std::thread([d] {
    WaitForSingleObject(d->hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(d->hProcess, &code);
    d->exited = true;
    d->closePseudoConsole();
    if (d->reader.joinable()) d->reader.join();
    if (d->onExit) d->onExit((uint32_t)code);
  });
  return p;
}

ConPty::~ConPty() {
  if (!d) return;
  if (d->hProcess) kill();
  if (d->waiter.joinable()) d->waiter.join();
  if (d->reader.joinable()) d->reader.join();
  d->closePseudoConsole();
  if (d->hOut) CloseHandle(d->hOut);
  if (d->hProcess) CloseHandle(d->hProcess);
}

uint32_t ConPty::pid() const { return d->pid; }
int ConPty::cols() const { return d->cols; }
int ConPty::rows() const { return d->rows; }
bool ConPty::exited() const { return d->exited; }

bool ConPty::write(const std::string& bytes) {
  if (bytes.empty()) return true;
  std::lock_guard<std::mutex> g(d->m);
  if (!d->hIn) return false;
  size_t off = 0;
  while (off < bytes.size()) {
    DWORD n = 0;
    if (!WriteFile(d->hIn, bytes.data() + off, (DWORD)(bytes.size() - off), &n, nullptr)) return false;
    off += n;
  }
  return true;
}

bool ConPty::resize(int cols, int rows) {
  // node-pty: "resizing must be done using positive cols and rows"
  if (cols <= 0 || rows <= 0) return false;
  std::lock_guard<std::mutex> g(d->m);
  if (!d->hpc) return false;
  COORD size; size.X = (SHORT)cols; size.Y = (SHORT)rows;
  api().resize(d->hpc, size);
  d->cols = cols; d->rows = rows;       // node-pty records the size whatever the call answered
  return true;
}

std::vector<uint32_t> ConPty::processTree() const {
  std::vector<uint32_t> out;
  if (!d->pid) return out;
  if (!d->exited) out.push_back(d->pid);
  for (uint32_t k : descendantsOf(d->pid)) if (pidAlive(k)) out.push_back(k);
  return out;
}

void ConPty::kill() {
  // The tree first, while the parent links still exist. (node-pty asks a helper process for the console's
  // process list and main.js then runs `taskkill /PID <pid> /T /F`; a snapshot walk reaches the same
  // processes without starting one — and closing the pseudoconsole ends anything else attached to it.)
  std::vector<uint32_t> tree = d->exited ? std::vector<uint32_t>() : descendantsOf(d->pid);
  d->closePseudoConsole();
  if (!d->exited && d->hProcess) TerminateProcess(d->hProcess, 1);
  for (uint32_t pid : tree) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) { TerminateProcess(h, 1); CloseHandle(h); }
  }
}

}}  // namespace deck::host
