// mac/Sources/DeckMacSeams/MacPty.cpp — one process in a pseudo-terminal on macOS: the native deck's node-pty.
// UNBUILT, UNTESTED: written on Windows, never compiled.
// Windows counterpart: windows/host/conpty.{hpp,cpp} (ConPty) + WinPtyHost in windows/host/winseams.cpp.
// JS mirrored: node-pty's unix side (src/unix/pty.cc: forkpty, the termios it sets, TIOCSWINSZ, SIGHUP on kill),
// lib/platform.js killTree (POSIX branch), main.js's use of both (`p.kill()` + `platform.killTree(pid)`).
//
// What is matched to node-pty:
//   - forkpty with node-pty's termios (UTF-8 input, ICANON / ECHO / ISIG, ONLCR, 38400 baud) and the winsize;
//   - the child is a session and process-group leader with the pty as its controlling terminal (forkpty does
//     login_tty), so kill(-pid, sig) reaches everything it started;
//   - the child's environment is spec.env AND NOTHING ELSE; its folder is spec.cwd; a bare file name is looked up
//     on the PATH of spec.env (execvp's rule, done before the fork so a miss is an error message, not a dead child);
//   - 'exit' is delivered after the last 'data'.
// Threads: ONE per pty. It polls the master (reads -> Inbox::ptyData; flushes queued writes), reaps the child, and
// pushes Inbox::ptyExit last. It never touches the deck. The Pty object is the deck thread's.
#include "MacSeams.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include <fcntl.h>
#include <libproc.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>          // forkpty (macOS; <pty.h> on Linux)

namespace deck { namespace mac {

namespace {
std::mutex gLiveM; std::set<int> gLive;                 // children not yet reaped (the exit watchdog's list)

struct State {                                          // shared by the Pty (deck thread) and its reader thread
  std::shared_ptr<db::Inbox> inbox;
  uint64_t token = 0;
  int master = -1; pid_t pid = 0;
  std::mutex m; std::string pending;                    // bytes write() could not put on the pty at once
  int wake[2] = { -1, -1 };                             // the deck thread writes a byte: "there is something to flush"
  std::atomic<bool> exited{ false };
  std::atomic<int> cols{ 120 }, rows{ 30 };
};

void readerThread(std::shared_ptr<State> s) {
  bool eof = false, reaped = false; int status = 0; int quietAfterExit = 0;
  char buf[65536];
  while (!(reaped && (eof || quietAfterExit >= 2))) {
    struct pollfd p[2] = { { s->master, POLLIN, 0 }, { s->wake[0], POLLIN, 0 } };
    bool wantWrite; { std::lock_guard<std::mutex> g(s->m); wantWrite = !s->pending.empty(); }
    if (wantWrite) p[0].events |= POLLOUT;
    if (eof) p[0].fd = -1;
    const int pr = poll(p, 2, reaped ? 50 : 100);
    if (pr < 0 && errno != EINTR) break;
    if (pr > 0 && (p[1].revents & POLLIN)) { char junk[64]; while (::read(s->wake[0], junk, sizeof junk) > 0) {} }
    bool readSomething = false;
    if (pr > 0 && !eof && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      const ssize_t n = ::read(s->master, buf, sizeof buf);
      if (n > 0) { s->inbox->ptyData(s->token, buf, (size_t)n); readSomething = true; }
      else if (n == 0 || (errno != EAGAIN && errno != EINTR)) eof = true;       // EIO: every slave end is closed
    }
    if (pr > 0 && !eof && (p[0].revents & POLLOUT)) {
      std::lock_guard<std::mutex> g(s->m);
      while (!s->pending.empty()) {
        const ssize_t w = ::write(s->master, s->pending.data(), s->pending.size());
        if (w > 0) s->pending.erase(0, (size_t)w); else break;
      }
    }
    if (!reaped) {
      const pid_t r = waitpid(s->pid, &status, WNOHANG);
      if (r == s->pid || (r < 0 && errno == ECHILD)) { reaped = true; std::lock_guard<std::mutex> g(gLiveM); gLive.erase((int)s->pid); }
    } else if (!readSomething) quietAfterExit++;        // the child is gone and the pty is quiet: a grandchild may
                                                        // still hold the slave open — do not wait for its EOF
  }
  s->exited.store(true);
  // node-pty: exitCode is the status of a normal exit and 0 for a death by signal (the signal is reported apart,
  // and main.js reads only exitCode). VERIFY against node-pty's pty.cc before relying on the 0.
  const uint32_t code = WIFEXITED(status) ? (uint32_t)WEXITSTATUS(status) : 0;
  s->inbox->ptyExit(s->token, code);
  ::close(s->master); ::close(s->wake[0]); ::close(s->wake[1]);
}

class MacPty : public core::Pty {
 public:
  explicit MacPty(std::shared_ptr<State> s) : s_(std::move(s)) {}
  ~MacPty() override { kill(); }                        // the reader thread owns the fds and ends by itself
  uint32_t pid() const override { return (uint32_t)s_->pid; }
  int cols() const override { return s_->cols.load(); }
  int rows() const override { return s_->rows.load(); }
  bool write(const std::string& bytes) override {
    if (s_->exited.load()) return false;
    std::lock_guard<std::mutex> g(s_->m);
    size_t done = 0;
    if (s_->pending.empty()) {                          // straight to the pty when nothing is queued ahead of it
      while (done < bytes.size()) {
        const ssize_t w = ::write(s_->master, bytes.data() + done, bytes.size() - done);
        if (w > 0) done += (size_t)w; else if (errno == EINTR) continue; else break;      // EAGAIN: the child is not reading
      }
    }
    if (done < bytes.size()) { s_->pending.append(bytes, done, std::string::npos); const char c = 1; const ssize_t w = ::write(s_->wake[1], &c, 1); (void)w; }
    return true;
  }
  bool resize(int cols, int rows) override {
    if (s_->exited.load() || cols < 1 || rows < 1) return false;
    struct winsize ws; std::memset(&ws, 0, sizeof ws); ws.ws_col = (unsigned short)cols; ws.ws_row = (unsigned short)rows;
    if (ioctl(s_->master, TIOCSWINSZ, &ws) != 0) return false;      // the kernel sends SIGWINCH to the foreground group
    s_->cols.store(cols); s_->rows.store(rows);
    return true;
  }
  // main.js: `p.kill()` (node-pty: SIGHUP to the child) and `platform.killTree(pid)` (SIGKILL to its group, then
  // to the pid when there is no such group). Returns at once; the exit follows through the inbox.
  void kill() override {
    if (s_->exited.load() || killed_) return;
    killed_ = true;
    ::kill(s_->pid, SIGHUP);
    if (::kill(-s_->pid, SIGKILL) != 0) ::kill(s_->pid, SIGKILL);
  }
  bool exited() const override { return s_->exited.load(); }
  std::vector<uint32_t> processTree() const override {
    std::vector<uint32_t> out;
    if (s_->exited.load()) return out;
    out.push_back((uint32_t)s_->pid);
    for (size_t i = 0; i < out.size() && out.size() < 512; i++) {       // breadth first over the children
      pid_t kids[256];
      const int n = proc_listchildpids((pid_t)out[i], kids, (int)sizeof kids);   // VERIFY: returns a COUNT on current SDKs (it was bytes on old ones)
      for (int k = 0; k < n && k < 256; k++) if (kids[k] > 0) out.push_back((uint32_t)kids[k]);
    }
    return out;
  }
 private:
  std::shared_ptr<State> s_;
  bool killed_ = false;
};

std::string envGet(const core::EnvList& env, const char* name) { for (const auto& kv : env) if (kv.first == name) return kv.second; return std::string(); }
bool isExecutable(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0; }
// execvp's lookup, against the CHILD's PATH: a name with a slash is used as it is
std::string resolveFile(const std::string& file, const core::EnvList& env, const std::string& cwd) {
  if (file.find('/') != std::string::npos) {
    if (file[0] == '/' || cwd.empty()) return isExecutable(file) ? file : std::string();
    const std::string rel = cwd + "/" + file; return isExecutable(rel) ? rel : std::string();
  }
  std::string path = envGet(env, "PATH"); if (path.empty()) path = "/usr/bin:/bin:/usr/sbin:/sbin";
  for (size_t p = 0; p <= path.size();) {
    const size_t q = std::min(path.find(':', p), path.size());
    const std::string dir = q > p ? path.substr(p, q - p) : std::string(".");
    const std::string full = dir + "/" + file;
    if (isExecutable(full)) return full;
    p = q + 1;
  }
  return std::string();
}
}  // namespace

MacPtyHost::MacPtyHost(std::shared_ptr<db::Inbox> inbox) : inbox_(std::move(inbox)) {}
MacPtyHost::~MacPtyHost() {}
std::vector<int> MacPtyHost::livePids() { std::lock_guard<std::mutex> g(gLiveM); return std::vector<int>(gLive.begin(), gLive.end()); }

std::unique_ptr<core::Pty> MacPtyHost::spawn(const core::PtySpawn& spec, uint64_t token, std::string* error) {
  auto fail = [&](const std::string& what) -> std::unique_ptr<core::Pty> { if (error) *error = what; return nullptr; };
  const std::string file = resolveFile(spec.file, spec.env, spec.cwd);
  if (file.empty()) return fail("File not found: " + spec.file);       // node-pty's wording (windows/host/conpty.hpp uses it too)
  if (!spec.cwd.empty()) { struct stat st; if (::stat(spec.cwd.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return fail("chdir() failed: " + spec.cwd); }

  // everything the child needs, built BEFORE the fork: between fork and exec only async-signal-safe calls are made
  std::vector<std::string> argStore; argStore.push_back(spec.file);
  for (const std::string& a : spec.args) argStore.push_back(a);
  std::vector<char*> argv; for (std::string& s : argStore) argv.push_back(&s[0]); argv.push_back(nullptr);
  std::vector<std::string> envStore; for (const auto& kv : spec.env) envStore.push_back(kv.first + "=" + kv.second);
  std::vector<char*> envp; for (std::string& s : envStore) envp.push_back(&s[0]); envp.push_back(nullptr);

  // node-pty's termios (src/unix/pty.cc PtyFork)
  struct termios t; std::memset(&t, 0, sizeof t);
  t.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT | IUTF8;
  t.c_oflag = OPOST | ONLCR;
  t.c_cflag = CREAD | CS8 | HUPCL;
  t.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;
  t.c_cc[VEOF] = 4; t.c_cc[VEOL] = 255; t.c_cc[VEOL2] = 255; t.c_cc[VERASE] = 0x7f; t.c_cc[VWERASE] = 23; t.c_cc[VKILL] = 21;
  t.c_cc[VREPRINT] = 18; t.c_cc[VINTR] = 3; t.c_cc[VQUIT] = 0x1c; t.c_cc[VSUSP] = 26; t.c_cc[VSTART] = 17; t.c_cc[VSTOP] = 19;
  t.c_cc[VLNEXT] = 22; t.c_cc[VDISCARD] = 15; t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
#ifdef VDSUSP
  t.c_cc[VDSUSP] = 25; t.c_cc[VSTATUS] = 20;
#endif
  cfsetispeed(&t, B38400); cfsetospeed(&t, B38400);
  struct winsize ws; std::memset(&ws, 0, sizeof ws);
  ws.ws_col = (unsigned short)std::max(1, spec.cols); ws.ws_row = (unsigned short)std::max(1, spec.rows);

  int errPipe[2];                                       // CLOEXEC: closes by itself on a successful exec
  if (pipe(errPipe) != 0) return fail("pipe() failed");
  fcntl(errPipe[0], F_SETFD, FD_CLOEXEC); fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);
  const long maxFd = sysconf(_SC_OPEN_MAX);

  int master = -1;
  const pid_t pid = forkpty(&master, nullptr, &t, &ws);
  if (pid < 0) { ::close(errPipe[0]); ::close(errPipe[1]); return fail(std::string("forkpty(3) failed: ") + strerror(errno)); }
  if (pid == 0) {
    // ---- the child. async-signal-safe only.
    struct sigaction sa; std::memset(&sa, 0, sizeof sa); sa.sa_handler = SIG_DFL;
    for (int sig = 1; sig < NSIG; sig++) sigaction(sig, &sa, nullptr);          // the deck ignores SIGPIPE; the child must not
    sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, nullptr);
    for (long fd = 3; fd < maxFd && fd < 4096; fd++) if (fd != errPipe[1]) ::close((int)fd);   // nothing of the deck's leaks in
    if (!spec.cwd.empty() && chdir(spec.cwd.c_str()) != 0) { const int e = errno; const ssize_t w = ::write(errPipe[1], &e, sizeof e); (void)w; _exit(127); }
    execve(file.c_str(), argv.data(), envp.data());
    const int e = errno; const ssize_t w = ::write(errPipe[1], &e, sizeof e); (void)w;
    _exit(127);
  }
  // ---- the parent
  ::close(errPipe[1]);
  int childErr = 0; ssize_t got;
  while ((got = ::read(errPipe[0], &childErr, sizeof childErr)) < 0 && errno == EINTR) {}
  ::close(errPipe[0]);
  if (got == (ssize_t)sizeof childErr) {                // chdir or exec failed in the child
    ::close(master); int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return fail(std::string("posix_spawnp failed: ") + strerror(childErr));     // node-pty 1.x's wording on macOS
  }
  fcntl(master, F_SETFD, FD_CLOEXEC);                   // the next child must not inherit this pty
  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

  std::shared_ptr<State> s = std::make_shared<State>();
  s->inbox = inbox_; s->token = token; s->master = master; s->pid = pid; s->cols.store(ws.ws_col); s->rows.store(ws.ws_row);
  if (pipe(s->wake) != 0) { ::kill(-pid, SIGKILL); ::close(master); return fail("pipe() failed"); }
  for (int i = 0; i < 2; i++) { fcntl(s->wake[i], F_SETFD, FD_CLOEXEC); fcntl(s->wake[i], F_SETFL, fcntl(s->wake[i], F_GETFL) | O_NONBLOCK); }
  { std::lock_guard<std::mutex> g(gLiveM); gLive.insert((int)pid); }
  std::thread(readerThread, s).detach();
  return std::unique_ptr<core::Pty>(new MacPty(s));
}

}}  // namespace deck::mac
