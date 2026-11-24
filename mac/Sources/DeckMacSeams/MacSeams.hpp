// mac/Sources/DeckMacSeams/MacSeams.hpp — the macOS implementations of core/deck/seams.hpp, declared in one place.
// UNBUILT, UNTESTED (written on Windows; never compiled).
// Windows counterpart: windows/host/winseams.hpp (Clock, Random, Env, ProcessList, PtyHost), statefile.hpp (Files),
// listener.hpp (Listener, Net), winhttp.hpp (Http, Credentials). JS mirrored: lib/platform.js, node-pty,
// lib/statefile.js, lib/phone.js.
//
// THREADS (the same contract as windows/HOST-NOTES.md "Threads"): every method of every class here is called on
// the deck's thread, except where a class says it owns a thread. Those threads never call the core: they push to
// the db::Inbox they were given and nothing else.
#pragma once
#include "DeckBridgeHost.hpp"
#include "core/deck/seams.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace deck { namespace mac {

// ---- PosixSeams.cpp
class MacClock : public core::Clock {
 public:
  double nowMs() override;                          // gettimeofday
  double localOffsetMs(double utcMs) override;      // localtime_r(...).tm_gmtoff
};
class PosixFiles : public core::Files {
 public:
  bool read(const std::string& path, std::string& out) override;
  bool readFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) override;
  bool readRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out) override;
  core::FileStat stat(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;
  bool makeDirs(const std::string& path) override;
  bool remove(const std::string& path) override;
  bool copy(const std::string& from, const std::string& to) override;
  bool writeAtomic(const std::string& path, const std::string& text) override;   // temp file, fsync, rename
  bool write(const std::string& path, const std::string& bytes) override;
  bool rename(const std::string& from, const std::string& to) override;          // rename(2): atomic
};
class MacRandom : public core::Random {
 public:
  std::string uuid() override;                      // arc4random_buf, RFC 4122 version 4
  std::string hex(int bytes) override;
};
class MacEnv : public core::Env {
 public:
  explicit MacEnv(bool loginEnvironment);
  core::EnvList environment() override;
  std::string get(const std::string& name) override;
  std::string os() override { return "darwin"; }
  std::string home() override;
  uint32_t pid() override;
  std::string resolvePath(const std::string& p) override;
 private:
  core::EnvList env_;
};
class MacProcessList : public core::ProcessList {
 public:
  bool alive(double pid) override;
  bool parents(std::map<uint32_t, uint32_t>& out) override;                     // libproc: every pid's pbi_ppid
  // No console hosts on a Mac: lib/platform.js answers an empty Map (not a failure), so nothing retries.
  bool consoleHosts(const std::vector<uint32_t>&, std::map<uint32_t, int64_t>&) override { return true; }
  void killTree(uint32_t pid) override;                                         // kill(-pid, SIGKILL), else kill(pid)
  // commit(): not overridden. The memory guard reads Windows' commit charge; macOS has no equal figure (it
  // compresses and swaps without a fixed limit), and lib/hibernate.js's guard is fed by a Windows-only reading in
  // main.js. VERIFY against main.js on the Mac build before inventing one.
};
class MacNet : public core::Net {
 public:
  explicit MacNet(bool test) : test_(test) {}
  std::vector<std::string> ipv4Addresses() override;                            // getifaddrs: AF_INET, up, not loopback
  std::string tailscaleServeStatus() override;                                  // `tailscale serve status --json`, 3 s at most
 private:
  bool test_;
};
// Run a program and capture its stdout (no shell, no window — there are none to make on a Mac). "" on failure or
// when it ran past `timeoutMs` (it is then killed). `env` null = this process's environment.
std::string runCapture(const std::string& file, const std::vector<std::string>& args, int timeoutMs, const core::EnvList* env = nullptr, int* exitCode = nullptr);
std::string homeDir();

// ---- MacPty.cpp
// One reader thread per live pty (it also reaps the child): pushes ptyData, then ptyExit after the last byte.
class MacPtyHost : public core::PtyHost {
 public:
  explicit MacPtyHost(std::shared_ptr<db::Inbox> inbox);
  ~MacPtyHost() override;
  std::unique_ptr<core::Pty> spawn(const core::PtySpawn& spec, uint64_t token, std::string* error) override;
  // every child this host started that has not been reaped (the exit watchdog kills these process groups)
  static std::vector<int> livePids();
 private:
  std::shared_ptr<db::Inbox> inbox_;
};

// ---- MacListener.cpp
// One thread (started by listen, joined by stop) owns every socket: accepts, reads, flushes what send() queued.
class MacListener : public core::Listener {
 public:
  MacListener(std::shared_ptr<db::Inbox> inbox, int testPortLow, int testPortHigh);
  ~MacListener() override;
  bool listen(const std::string& host, int port, std::string* error) override;
  void stop() override;
  void send(uint64_t conn, const std::string& bytes) override;
  size_t buffered(uint64_t conn) override;
  void close(uint64_t conn, bool afterFlush) override;
 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

// ---- MacHttpKeychain.mm
// core::Http on URLSession (its own delegate queue); `done` is handed to the inbox and so runs on the deck's thread.
std::unique_ptr<core::Http> makeUrlSessionHttp(std::shared_ptr<db::Inbox> inbox);
// core::Credentials: <dir>/.credentials.json when it is a file, else the login Keychain exactly as lib/platform.js
// documents it (service "Claude Code-credentials[-sha256(dir)[0:8]]", account "claude-code-user", chunked "#m" /
// "#0".."#n-1"). READ ONLY. One Keychain read per account per minute (lib/platform.js KEYCHAIN_TTL_MS).
std::unique_ptr<core::Credentials> makeKeychainCredentials(core::Clock& clock);

}}  // namespace deck::mac
