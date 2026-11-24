// mac/Sources/DeckMacSeams/PosixSeams.cpp — Clock, Files, Random, Env, ProcessList, Net for macOS, and the
// process-level helpers of DeckMacSeams.h (state folder, instance lock, exit watchdog, relaunch).
// UNBUILT, UNTESTED: written on Windows, never compiled. Plain POSIX + libproc; no Objective-C.
// Windows counterpart: windows/host/winseams.cpp (clock, random, env, process list), windows/host/statefile.cpp
// (files), windows/host/listener.cpp (WinNet), windows/src/lifecycle.hpp (lock, watchdog, relaunch).
// JS mirrored: lib/platform.js (killTree, processParents), lib/statefile.js (writeAtomic), lib/phone.js
// (tailscaleAddress / tailscaleServeUrl's bin list), main.js (armExitWatchdog, restartDeck).
#include "MacSeams.hpp"
#include "DeckMacSeams.h"
#include "core/deck/edition.hpp"     // the state folder's name (claude-deck-native in this copy)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace deck { namespace mac {

// ------------------------------------------------------------------------------------------------ clock
double MacClock::nowMs() { struct timeval tv; gettimeofday(&tv, nullptr); return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0; }
double MacClock::localOffsetMs(double utcMs) {
  const time_t t = (time_t)(utcMs / 1000.0); struct tm tmv;
  if (!localtime_r(&t, &tmv)) return 0;
  return (double)tmv.tm_gmtoff * 1000.0;            // seconds EAST of UTC: local = utc + offset
}

// ------------------------------------------------------------------------------------------------ files
namespace {
struct Fd { int fd; explicit Fd(int f) : fd(f) {} ~Fd() { if (fd >= 0) ::close(fd); } };
bool readAll(int fd, std::string& out, uint64_t limit) {
  char buf[65536];
  while (out.size() < limit) {
    const size_t want = (size_t)std::min<uint64_t>(sizeof buf, limit - out.size());
    const ssize_t n = ::read(fd, buf, want);
    if (n < 0) { if (errno == EINTR) continue; return false; }
    if (n == 0) break;
    out.append(buf, (size_t)n);
  }
  return true;
}
bool writeAll(int fd, const char* p, size_t n) {
  while (n) { const ssize_t w = ::write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; return false; } p += w; n -= (size_t)w; }
  return true;
}
std::string parentDir(const std::string& p) { const size_t s = p.find_last_of('/'); return s == std::string::npos ? std::string(".") : s == 0 ? std::string("/") : p.substr(0, s); }
}  // namespace

bool PosixFiles::read(const std::string& path, std::string& out) {
  out.clear();
  Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (f.fd < 0) return false;
  struct stat st; if (fstat(f.fd, &st) != 0 || S_ISDIR(st.st_mode)) return false;
  return readAll(f.fd, out, UINT64_MAX);
}
bool PosixFiles::readFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) {
  out.clear();
  Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (f.fd < 0) return false;
  struct stat st; if (fstat(f.fd, &st) != 0 || S_ISDIR(st.st_mode)) return false;
  if (sizeNow) *sizeNow = (uint64_t)st.st_size;
  if (offset >= (uint64_t)st.st_size) return true;
  if (lseek(f.fd, (off_t)offset, SEEK_SET) < 0) return false;
  return readAll(f.fd, out, UINT64_MAX);
}
bool PosixFiles::readRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out) {
  out.clear();
  Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (f.fd < 0) return false;
  if (lseek(f.fd, (off_t)offset, SEEK_SET) < 0) return false;
  return readAll(f.fd, out, length);
}
core::FileStat PosixFiles::stat(const std::string& path) {
  core::FileStat r; struct stat st;
  if (::stat(path.c_str(), &st) != 0) return r;
  r.exists = true; r.isDir = S_ISDIR(st.st_mode); r.size = (uint64_t)st.st_size;
  r.mtimeMs = (double)st.st_mtimespec.tv_sec * 1000.0 + (double)st.st_mtimespec.tv_nsec / 1e6;   // Darwin's name for st_mtim
  return r;
}
std::vector<std::string> PosixFiles::list(const std::string& dir) {
  std::vector<std::string> names;
  DIR* d = opendir(dir.c_str());
  if (!d) return names;
  while (struct dirent* e = readdir(d)) { const std::string n = e->d_name; if (n != "." && n != "..") names.push_back(n); }
  closedir(d);
  std::sort(names.begin(), names.end());            // fs.readdirSync order on APFS is not sorted; the seam asks for sorted
  return names;
}
bool PosixFiles::makeDirs(const std::string& path) {
  if (path.empty()) return false;
  std::string cur;
  for (size_t i = 0; i <= path.size(); i++) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty() && cur != "/") { if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false; }
    }
    if (i < path.size()) cur.push_back(path[i]);
  }
  return isDir(path);
}
bool PosixFiles::remove(const std::string& path) { return ::unlink(path.c_str()) == 0 || ::rmdir(path.c_str()) == 0; }
bool PosixFiles::copy(const std::string& from, const std::string& to) { std::string data; return read(from, data) && write(to, data); }
bool PosixFiles::write(const std::string& path, const std::string& bytes) {
  Fd f(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  return f.fd >= 0 && writeAll(f.fd, bytes.data(), bytes.size());
}
// lib/statefile.js writeAtomic: a temp file beside the target, flushed to the disk, renamed over it. A crash leaves
// the old file or the new one, never half of either. (0600 for the temp: state can name sessions and folders.)
bool PosixFiles::writeAtomic(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp-" + std::to_string((long)getpid());
  { Fd f(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (f.fd < 0) return false;
    if (!writeAll(f.fd, text.data(), text.size())) { ::unlink(tmp.c_str()); return false; }
    // F_FULLFSYNC is what actually reaches the platter on macOS; fsync alone only reaches the drive's cache
    if (fcntl(f.fd, F_FULLFSYNC) != 0) fsync(f.fd); }
  if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
  Fd d(::open(parentDir(path).c_str(), O_RDONLY | O_CLOEXEC));      // make the rename itself durable
  if (d.fd >= 0) fsync(d.fd);
  return true;
}
bool PosixFiles::rename(const std::string& from, const std::string& to) { return ::rename(from.c_str(), to.c_str()) == 0; }

// ------------------------------------------------------------------------------------------------ random
namespace { std::string toHex(const unsigned char* b, size_t n) { static const char* H = "0123456789abcdef"; std::string s; s.reserve(n * 2); for (size_t i = 0; i < n; i++) { s.push_back(H[b[i] >> 4]); s.push_back(H[b[i] & 15]); } return s; } }
std::string MacRandom::uuid() {
  unsigned char b[16]; arc4random_buf(b, sizeof b);
  b[6] = (unsigned char)((b[6] & 0x0F) | 0x40); b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
  const std::string h = toHex(b, 16);
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}
std::string MacRandom::hex(int bytes) { if (bytes <= 0) return std::string(); std::vector<unsigned char> b((size_t)bytes); arc4random_buf(b.data(), b.size()); return toHex(b.data(), b.size()); }

// ------------------------------------------------------------------------------------------------ env
std::string homeDir() {
  const char* h = getenv("HOME");
  if (h && *h) return h;
  if (struct passwd* pw = getpwuid(getuid())) if (pw->pw_dir) return pw->pw_dir;
  return "/";
}
namespace {
core::EnvList processEnvironment() {
  core::EnvList out;
  for (char** e = environ; e && *e; e++) { const char* eq = strchr(*e, '='); if (eq && eq != *e) out.emplace_back(std::string(*e, (size_t)(eq - *e)), std::string(eq + 1)); }
  return out;
}
// The login shell's environment: `$SHELL -l -c 'env -0'`. NUL-separated, so values with newlines survive.
core::EnvList loginEnvironment() {
  const char* sh = getenv("SHELL");
  std::string shell = sh && *sh ? sh : "/bin/zsh";
  const std::string out = runCapture(shell, { "-l", "-c", "/usr/bin/env -0" }, 5000);
  core::EnvList env;
  for (size_t p = 0; p < out.size();) {
    const size_t q = std::min(out.find('\0', p), out.size());
    const std::string kv = out.substr(p, q - p);
    const size_t eq = kv.find('=');
    if (eq != std::string::npos && eq > 0) env.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    p = q + 1;
  }
  return env;
}
}  // namespace
MacEnv::MacEnv(bool login) {
  env_ = processEnvironment();
  if (login) {
    core::EnvList l = loginEnvironment();
    // only trust it when it looks like an environment (a shell rc that prints or fails gives nothing useful)
    bool hasPath = false; for (const auto& kv : l) if (kv.first == "PATH" && !kv.second.empty()) hasPath = true;
    if (hasPath) {
      // the login shell's values win; what only this process has (e.g. CLAUDE_DECK_* set by a launcher) is kept
      for (const auto& own : env_) { bool there = false; for (const auto& kv : l) if (kv.first == own.first) { there = true; break; } if (!there) l.push_back(own); }
      // the helper shell's own bookkeeping is not the user's environment
      l.erase(std::remove_if(l.begin(), l.end(), [](const std::pair<std::string, std::string>& kv) { return kv.first == "_" || kv.first == "SHLVL" || kv.first == "OLDPWD" || kv.first == "PWD"; }), l.end());
      env_ = std::move(l);
    }
  }
}
core::EnvList MacEnv::environment() { return env_; }
std::string MacEnv::get(const std::string& name) { for (const auto& kv : env_) if (kv.first == name) return kv.second; return std::string(); }
std::string MacEnv::home() { const std::string h = get("HOME"); return !h.empty() ? h : homeDir(); }
uint32_t MacEnv::pid() { return (uint32_t)getpid(); }
// path.resolve(p): absolute as it is (normalised), relative against the working directory; no symlink is followed
std::string MacEnv::resolvePath(const std::string& p) {
  std::string full = p;
  if (full.empty() || full[0] != '/') { char cwd[PATH_MAX]; if (getcwd(cwd, sizeof cwd)) full = std::string(cwd) + "/" + p; }
  std::vector<std::string> parts;
  for (size_t i = 0; i <= full.size();) {
    const size_t q = std::min(full.find('/', i), full.size());
    const std::string seg = full.substr(i, q - i);
    if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
    else if (!seg.empty() && seg != ".") parts.push_back(seg);
    i = q + 1;
  }
  std::string out;
  for (const std::string& s : parts) out += "/" + s;
  return out.empty() ? std::string("/") : out;
}

// ------------------------------------------------------------------------------------------------ processes
bool MacProcessList::alive(double pid) {
  if (!(pid > 0) || pid > 4194304.0) return false;
  return ::kill((pid_t)pid, 0) == 0 || errno == EPERM;      // lib/sessions.js pidAlive: EPERM = it is there, not ours
}
bool MacProcessList::parents(std::map<uint32_t, uint32_t>& out) {
  int n = proc_listallpids(nullptr, 0);                      // VERIFY: returns the COUNT of pids when the buffer is null
  if (n <= 0) return false;
  std::vector<pid_t> pids((size_t)n + 64);
  n = proc_listallpids(pids.data(), (int)(pids.size() * sizeof(pid_t)));
  if (n <= 0) return false;
  for (int i = 0; i < n && i < (int)pids.size(); i++) {
    if (pids[(size_t)i] <= 0) continue;
    struct proc_bsdinfo info;
    if (proc_pidinfo(pids[(size_t)i], PROC_PIDTBSDINFO, 0, &info, sizeof info) == (int)sizeof info) out[(uint32_t)pids[(size_t)i]] = (uint32_t)info.pbi_ppid;
    // (a process of another user answers 0 bytes: it is simply not in the map, as `ps` would still list it —
    //  VERIFY that the shell-host scan only ever asks about this user's processes)
  }
  return !out.empty();
}
// lib/platform.js killTree, POSIX branch: the child is a process-group leader (forkpty -> setsid), so -pid takes the tree
void MacProcessList::killTree(uint32_t pid) {
  if (!pid) return;
  if (::kill(-(pid_t)pid, SIGKILL) != 0) ::kill((pid_t)pid, SIGKILL);
}

// ------------------------------------------------------------------------------------------------ run + capture
std::string runCapture(const std::string& file, const std::vector<std::string>& args, int timeoutMs, const core::EnvList* env, int* exitCode) {
  if (exitCode) *exitCode = -1;
  int fds[2];
  if (pipe(fds) != 0) return std::string();
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  posix_spawnattr_t at; posix_spawnattr_init(&at);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);   // its own group (so a timeout can kill its children); no fd leaks
  posix_spawnattr_setpgroup(&at, 0);
  std::vector<char*> argv; argv.push_back(const_cast<char*>(file.c_str()));
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  std::vector<std::string> envStore; std::vector<char*> envp;
  if (env) { for (const auto& kv : *env) envStore.push_back(kv.first + "=" + kv.second); for (std::string& s : envStore) envp.push_back(&s[0]); envp.push_back(nullptr); }
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, file.c_str(), &fa, &at, argv.data(), env ? envp.data() : environ);
  posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
  ::close(fds[1]);
  if (rc != 0) { ::close(fds[0]); return std::string(); }
  std::string out; bool timedOut = false;
  MacClock clock; const double deadline = clock.nowMs() + timeoutMs;
  for (;;) {
    const double left = deadline - clock.nowMs();
    if (left <= 0) { timedOut = true; break; }
    struct pollfd p = { fds[0], POLLIN, 0 };
    const int pr = poll(&p, 1, (int)left);
    if (pr < 0) { if (errno == EINTR) continue; break; }
    if (pr == 0) { timedOut = true; break; }
    char buf[8192]; const ssize_t n = ::read(fds[0], buf, sizeof buf);
    if (n < 0) { if (errno == EINTR) continue; break; }
    if (n == 0) break;
    out.append(buf, (size_t)n);
    if (out.size() > 16u * 1024 * 1024) { timedOut = true; break; }      // execFile's maxBuffer
  }
  ::close(fds[0]);
  if (timedOut) { ::kill(-pid, SIGKILL); ::kill(pid, SIGKILL); }
  int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (timedOut) return std::string();
  if (exitCode) *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return out;
}

// ------------------------------------------------------------------------------------------------ net
std::vector<std::string> MacNet::ipv4Addresses() {
  std::vector<std::string> out;
  if (test_) return out;
  struct ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (struct ifaddrs* a = list; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
    if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;      // os.networkInterfaces(): not `internal`
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, buf, sizeof buf)) out.push_back(buf);
  }
  freeifaddrs(list);
  return out;
}
// lib/phone.js tailscaleServeUrl's bin list, the Mac ones: on a Mac the CLI lives inside the app bundle. The first
// that answers wins; the core caches the answer a minute (Server::serveCache_), so this runs at most that often.
std::string MacNet::tailscaleServeStatus() {
  if (test_) return std::string();
  static const char* const bins[] = { "tailscale", "/Applications/Tailscale.app/Contents/MacOS/Tailscale", "/usr/local/bin/tailscale", "/opt/homebrew/bin/tailscale", "/usr/bin/tailscale" };
  for (const char* bin : bins) {
    int code = -1;
    const std::string out = runCapture(bin, { "serve", "status", "--json" }, 3000, nullptr, &code);
    if (code == 0 && !out.empty()) return out;          // execFileSync throws on a non-zero exit: "try the next"
  }
  return std::string();
}

}}  // namespace deck::mac

// ================================================================================================== DeckMacSeams.h
using deck::mac::homeDir;

const char* dms_config_dir(void) { static thread_local std::string s; s = homeDir() + "/Library/Application Support/" DECK_EDITION_STATE_DIR; return s.c_str(); }
const char* dms_default_state_dir(void) { static thread_local std::string s; s = homeDir() + "/Library/Application Support/" DECK_EDITION_STATE_DIR "/local"; return s.c_str(); }

int dms_single_instance_lock(const char* config_dir) {
  static int held = -1;
  if (held >= 0) return 1;
  const std::string dir = config_dir && *config_dir ? config_dir : dms_config_dir();
  deck::mac::PosixFiles().makeDirs(dir);
  const int fd = ::open((dir + "/instance.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return 1;                               // cannot tell: do not lock the user out of their own deck
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return 0; }
  char pid[32]; const int n = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());
  if (ftruncate(fd, 0) == 0 && n > 0) { const ssize_t w = ::write(fd, pid, (size_t)n); (void)w; }
  held = fd;                                          // kept open (and locked) until the process ends
  return 1;
}

void dms_arm_exit_watchdog(double ms) {
  static std::atomic<bool> armed{ false };
  if (armed.exchange(true)) return;
  const long wait = ms > 0 ? (long)ms : 0;
  std::thread([wait] {
    struct timespec ts = { (time_t)(wait / 1000), (long)(wait % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    for (int pid : deck::mac::MacPtyHost::livePids()) { if (::kill(-pid, SIGKILL) != 0) ::kill(pid, SIGKILL); }
    _exit(0);
  }).detach();
}

int dms_relaunch(const char* file, const char* const* args, int append_wait_pid) {
  if (!file || !*file) return 0;
  std::vector<std::string> store;
  for (const char* const* a = args; a && *a; a++) store.push_back(*a);
  if (append_wait_pid) { store.push_back("--wait-pid"); store.push_back(std::to_string((long)getpid())); }
  std::vector<char*> argv; argv.push_back(const_cast<char*>(file));
  for (std::string& s : store) argv.push_back(&s[0]);
  argv.push_back(nullptr);
  posix_spawnattr_t at; posix_spawnattr_init(&at);
  // POSIX_SPAWN_SETSID (macOS 10.15+): the new deck is in its own session, so it outlives this one.
  // VERIFY: the flag's name and availability in <spawn.h> of the SDK in use.
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
  posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, file, &fa, &at, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
  return rc == 0 ? 1 : 0;
}

void dms_wait_for_pid_exit(int pid, int timeout_ms) {
  if (pid <= 0) return;
  for (int waited = 0; waited < timeout_ms; waited += 50) {
    if (::kill((pid_t)pid, 0) != 0 && errno == ESRCH) return;
    struct timespec ts = { 0, 50 * 1000000L }; nanosleep(&ts, nullptr);
  }
}
