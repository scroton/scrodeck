// windows/host/conpty.hpp — one process in a Windows pseudoconsole: the native deck's node-pty.
//
// What the Electron deck gets from `pty.spawn(file, args, { name, cols, rows, cwd, env, useConpty: true })`
// (node-pty 1.1.0, src/win/conpty.cc + src/windowsPtyAgent.ts), matched here:
//   - the SYSTEM ConPTY: kernel32 CreatePseudoConsole / ResizePseudoConsole / ClosePseudoConsole. main.js
//     never passes `useConptyDll`, so node-pty does not load its bundled conpty.dll + OpenConsole.exe;
//   - dwFlags 0: `conptyInheritCursor` is not passed either (PSEUDOCONSOLE_INHERIT_CURSOR stays off);
//   - the child: CreateProcessW(nullptr, <command line>, …, bInheritHandles FALSE,
//     EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, <env block>, <cwd>) with
//     PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE and STARTF_USESTDHANDLES over three null handles;
//   - the command line: node-pty's argsToCommandLine (the CommandLineToArgvW quoting rule), ported below;
//   - "File not found: <path>" before anything is created, by node-pty's own lookup (path_util.cc).
// The child's console is the pseudoconsole's headless conhost: no window is ever created, whatever the
// parent is (a GUI exe with no console, or a console test run from a hidden shell).
//
// Threads. spawn() starts two: a READER (ReadFile on the pseudoconsole's output until the pipe breaks) that
// calls `onData`, and a WAITER (WaitForSingleObject on the child) that closes the pseudoconsole, lets the
// reader drain to the end, and then calls `onExit` — so the exit always comes after the last byte, as
// node-pty's 'exit' comes after its socket's last 'data'. Both callbacks run on those threads, never on the
// caller's: hand the bytes to a queue (LocalDeck does) and return. Do not call into this object's kill()
// or destructor from inside a callback.
// write(), resize(), kill() may be called from any one thread at a time (the deck thread).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace deck { namespace host {

struct PtySpec {
  std::wstring file;                    // "powershell.exe" (looked up on Path) or a full path
  std::vector<std::wstring> args;
  std::vector<std::wstring> env;        // "NAME=value" entries: the child's WHOLE environment, in this order
  std::wstring cwd;
  int cols = 120, rows = 30;            // main.js spawns every tile at 120 x 30
};

// node-pty's argsToCommandLine(file, args): quote an argument when it is empty or holds a space or a tab
// (and is not already wrapped in quotes), doubling backslashes before a quote. Pure.
std::wstring argsToCommandLine(const std::wstring& file, const std::vector<std::wstring>& args);

// "K=V\0K=V\0\0" for CreateProcessW. Empty input -> empty string (the caller passes nullptr = inherit).
std::wstring environmentBlock(const std::vector<std::wstring>& env);

// This process's environment as "NAME=value" entries (the `{ ...process.env }` main.js starts from).
// The hidden "=C:=C:\dir" drive entries are left out, as Node leaves them out.
std::vector<std::wstring> currentEnvironment();

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);

class ConPty {
 public:
  using DataFn = std::function<void(const char* bytes, size_t n)>;   // reader thread
  using ExitFn = std::function<void(uint32_t exitCode)>;             // waiter thread, after the last onData

  // Starts the process. Null on failure, with `error` set to node-pty's wording where it has one.
  static std::unique_ptr<ConPty> spawn(const PtySpec& spec, DataFn onData, ExitFn onExit, std::string* error);

  ~ConPty();                            // kill() + joins both threads; onExit has run (or will not) when it returns
  ConPty(const ConPty&) = delete;
  ConPty& operator=(const ConPty&) = delete;

  uint32_t pid() const;                 // the child's pid (node-pty's `pid`)
  int cols() const;                     // the size last given to the pseudoconsole (node-pty's `cols` / `rows`)
  int rows() const;
  bool write(const std::string& bytes); // false once the pseudoconsole is gone
  bool resize(int cols, int rows);      // false when either is < 1 or the pseudoconsole is gone
  bool exited() const;

  // What main.js does to end a tile: `p.kill()` then `platform.killTree(pid)` — every process on the
  // pseudoconsole and the child's whole tree ends, the pseudoconsole closes. Returns at once; onExit follows.
  void kill();

  // The pids attached to this pseudoconsole's console plus the child's descendants, right now (for a
  // caller that wants to check nothing was left behind). Empty once everything is gone.
  std::vector<uint32_t> processTree() const;

 private:
  ConPty();
  struct Impl;
  std::unique_ptr<Impl> d;
};

// Is this pid running? (lib/sessions.js pidAlive: exists, whoever owns it.)
bool pidAlive(uint32_t pid);
// Every descendant of `pid` in a process snapshot (children first-seen order), not including `pid`.
std::vector<uint32_t> descendantsOf(uint32_t pid);

}}  // namespace deck::host
