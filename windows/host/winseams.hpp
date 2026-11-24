// windows/host/winseams.hpp — the core deck's seams (core/deck/seams.hpp) as Windows implements them:
//   WinPtyHost / a ConPty per process (conpty.hpp)   WinProcessList (a Toolhelp snapshot, no helper process)
//   WinRandom (std::random_device)                    WinEnv (the process environment, %USERPROFILE%)
//   WinFileLog (<state dir>\deck.log: 1 MB, then deck.log.1)
// Files and Clock are in statefile.hpp. The Listener (Winsock) is in listener.hpp.
#pragma once
#include "conpty.hpp"
#include "../../core/deck/seams.hpp"
#include <functional>
#include <string>

namespace deck { namespace host {

class WinRandom : public core::Random {
 public:
  std::string uuid() override;
  std::string hex(int bytes) override;
};

class WinEnv : public core::Env {
 public:
  explicit WinEnv(std::string home = "") : home_(std::move(home)) {}
  core::EnvList environment() override;
  std::string get(const std::string& name) override;
  std::string os() override { return "win32"; }
  std::string home() override;
  std::string resolvePath(const std::string& p) override;
  uint32_t pid() override;
 private:
  std::string home_;
};

class WinProcessList : public core::ProcessList {
 public:
  bool alive(double pid) override;
  bool parents(std::map<uint32_t, uint32_t>& out) override;
  bool consoleHosts(const std::vector<uint32_t>& pids, std::map<uint32_t, int64_t>& out) override;
  void killTree(uint32_t pid) override;
  bool commit(double& used, double& limit) override;
  bool snapshot(std::vector<core::ProcInfo>& out) override;
};

// Starts each process in a pseudoconsole. Its two threads (conpty.hpp) hand bytes and the exit to `post`, which
// must be safe to call from any thread: the shell queues them and delivers them to the deck on its own thread.
class WinPtyHost : public core::PtyHost {
 public:
  using Post = std::function<void(uint64_t token, bool exit, uint32_t code, const char* bytes, size_t n)>;
  // Diagnostics only (LocalDeckOptions::ptyTrace): everything the deck does TO a process, as it does it, on the
  // deck's own thread — what: "spawn" (detail = the file), "write" (the bytes), "resize" ("cols x rows"), "kill".
  using Trace = std::function<void(uint64_t token, const char* what, const std::string& detail)>;
  explicit WinPtyHost(Post post, Trace trace = nullptr) : post_(std::move(post)), trace_(std::move(trace)) {}
  std::unique_ptr<core::Pty> spawn(const core::PtySpawn& spec, uint64_t token, std::string* error) override;
 private:
  Post post_;
  Trace trace_;
};

// The deck's log (seams.hpp Log): each line appended to `path` (UTF-8, CRLF); once the file is past `cap` bytes it
// becomes <path>.1 (the older .1 is replaced) and a new file starts. A line that cannot be written is dropped.
class WinFileLog : public core::Log {
 public:
  explicit WinFileLog(std::string path, uint64_t cap = 1024 * 1024) : path_(std::move(path)), cap_(cap) {}
  void line(const std::string& text) override;
  const std::string& path() const { return path_; }
 private:
  std::string path_;
  uint64_t cap_;
};

std::string envVar(const wchar_t* name);

}}  // namespace deck::host
