// core/test/deck/fakes.hpp — the test doubles of core/deck/seams.hpp: a clock that only moves when told, files in
// memory, processes that are a record of what was asked of them, and a listener that is two strings per socket.
// Nothing here touches the machine (DiskFiles reads the repo's own pages, read-only, for the route test).
#pragma once
#include "../../deck/deck.hpp"
#include "../../deck/seams.hpp"
#include "../../deck/server.hpp"
#include "../../deck/util.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fake {

using namespace deck::core;

struct Clock : deck::core::Clock {
  double t = 1800000000000.0;              // 2027-01-15
  double offset = 0;
  double nowMs() override { return t; }
  double localOffsetMs(double) override { return offset; }
};

// ---------------------------------------------------------------- THE PATH MODE (one switch, said once per run)
// Windows mode (the default): `\` paths, a C: drive, os() == "win32" - the shape every fixture was recorded in.
// POSIX mode (`deck-test … --posix`): `/` paths, os() == "darwin", a login $SHELL - the deck as a Mac configures it,
// so the Mac branches of the ported lib/platform.js and every path the core JOINS are tested on this box.
// A test writes its paths ONCE, Windows-shaped, through P(): "C:\\home\\x" is "/home/x" in POSIX mode (another
// drive, D:\\cfg, is /D/cfg). In POSIX mode the fake files do NOT read `\` as a separator: a path the core
// builds with a hard-coded backslash misses its file, and a check fails - which is the point.
inline bool& posixFlag() { static bool on = false; return on; }
inline bool posix() { return posixFlag(); }
inline void setPosix(bool on) { posixFlag() = on; deck::core::setPathSep(on ? '/' : '\\'); }
inline char sep() { return posix() ? '/' : '\\'; }
inline std::string osName() { return posix() ? "darwin" : "win32"; }   // what Env::os() says in this mode
inline std::string P(const std::string& win) {
  if (!posix()) return win;
  std::string p = win;
  if (p.size() >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) p = (p[0] == 'C' || p[0] == 'c') ? p.substr(2) : "/" + p.substr(0, 1) + p.substr(2);
  for (char& c : p) if (c == '\\') c = '/';
  return p;
}
// …as a const char* that stays valid (the rig's ALIAS / T_DIR / U_DIR)
inline const char* pc(const char* win) { static std::map<std::string, std::string> kept; return kept.emplace(std::string(posix() ? "p:" : "w:") + win, P(win)).first->second.c_str(); }

inline std::string norm(std::string p) {
  if (posix()) { while (p.size() > 1 && p.back() == '/') p.pop_back(); return p; }
  for (char& c : p) if (c == '/') c = '\\';
  while (p.size() > 3 && p.back() == '\\') p.pop_back();
  return p;
}

struct MemFiles : Files {
  struct Entry { std::string data; double mtime = 0; };
  std::map<std::string, Entry> files;
  std::set<std::string> dirs;
  Clock* clock = nullptr;
  int atomicWrites = 0;
  void put(const std::string& path, const std::string& data, double mtime = -1) { files[norm(path)] = { data, mtime >= 0 ? mtime : (clock ? clock->t : 0) }; }
  bool has(const std::string& path) const { return files.count(norm(path)) > 0; }
  std::string get(const std::string& path) const { auto it = files.find(norm(path)); return it == files.end() ? std::string() : it->second.data; }
  json getJson(const std::string& path) const { return json::parse(get(path), nullptr, false); }

  bool read(const std::string& path, std::string& out) override { auto it = files.find(norm(path)); if (it == files.end()) { out.clear(); return false; } out = it->second.data; return true; }
  bool readFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) override {
    auto it = files.find(norm(path)); out.clear();
    if (it == files.end()) return false;
    if (sizeNow) *sizeNow = it->second.data.size();
    if (offset < it->second.data.size()) out = it->second.data.substr((size_t)offset);
    return true;
  }
  FileStat stat(const std::string& pathIn) override {
    FileStat st; const std::string path = norm(pathIn);
    auto it = files.find(path);
    if (it != files.end()) { st.exists = true; st.size = it->second.data.size(); st.mtimeMs = it->second.mtime; return st; }
    if (dirs.count(path)) { st.exists = true; st.isDir = true; return st; }
    const std::string prefix = path == "/" ? path : path + sep();
    auto lb = files.lower_bound(prefix);
    if (lb != files.end() && lb->first.compare(0, prefix.size(), prefix) == 0) { st.exists = true; st.isDir = true; }
    return st;
  }
  std::vector<std::string> list(const std::string& dirIn) override {
    std::set<std::string> names; const std::string prefix = norm(dirIn) == "/" ? std::string("/") : norm(dirIn) + sep();
    for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
      const std::string rest = it->first.substr(prefix.size());
      names.insert(rest.substr(0, rest.find(sep())));
    }
    for (const std::string& d : dirs) if (d.compare(0, prefix.size(), prefix) == 0 && d.find(sep(), prefix.size()) == std::string::npos) names.insert(d.substr(prefix.size()));
    return std::vector<std::string>(names.begin(), names.end());
  }
  bool makeDirs(const std::string& path) override { dirs.insert(norm(path)); return true; }
  bool remove(const std::string& path) override { return files.erase(norm(path)) > 0; }
  bool copy(const std::string& from, const std::string& to) override { auto it = files.find(norm(from)); if (it == files.end()) return false; put(to, it->second.data); return true; }
  bool writeAtomic(const std::string& path, const std::string& text) override { atomicWrites++; put(path, text); return true; }
  bool write(const std::string& path, const std::string& bytes) override { put(path, bytes); return true; }
  bool rename(const std::string& from, const std::string& to) override { auto it = files.find(norm(from)); if (it == files.end()) return false; Entry e = it->second; files.erase(it); files[norm(to)] = e; return true; }
};

// the repo's own files, read-only (the pages the server serves)
struct DiskFiles : Files {
  // The deck joins its paths with the MODE's separator: in the default (win32) mode the pages are asked for as
  // `<repo>\renderer\phone\index.html`, which a Mac or Linux reads as ONE name with backslashes in it (not found:
  // the 113 serve / route / extras / http failures of the Windows-fakes pass on the Mac). Windows takes `/` as
  // well as `\`, so every host opens the repo's files through `/`: the same code path everywhere.
  static std::string onDisk(std::string path) { std::replace(path.begin(), path.end(), '\\', '/'); return path; }
  bool read(const std::string& path, std::string& out) override {
    std::ifstream f(onDisk(path), std::ios::binary); out.clear();
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
  }
  bool readFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) override {
    std::string all; if (!read(path, all)) return false;
    if (sizeNow) *sizeNow = all.size();
    out = offset < all.size() ? all.substr((size_t)offset) : std::string();
    return true;
  }
  FileStat stat(const std::string& path) override {
    FileStat st; std::ifstream f(onDisk(path), std::ios::binary | std::ios::ate);
    if (!f) return st;
    const std::streamoff n = f.tellg();
    if (n < 0) return st;                                  // a folder
    st.exists = true; st.size = (uint64_t)n; st.mtimeMs = 1;
    return st;
  }
  std::vector<std::string> list(const std::string&) override { return {}; }
  bool makeDirs(const std::string&) override { return false; }
  bool remove(const std::string&) override { return false; }
  bool copy(const std::string&, const std::string&) override { return false; }
  bool writeAtomic(const std::string&, const std::string&) override { return false; }
  bool write(const std::string&, const std::string&) override { return false; }
};

struct Ptys;
struct Pty : deck::core::Pty {
  Ptys* host; uint64_t token; PtySpawn spec; uint32_t pid_;
  int c = 120, r = 30;
  std::vector<std::string> writes; std::vector<std::pair<int, int>> resizes;
  bool killed = false, gone = false;
  Pty(Ptys* h, uint64_t t, const PtySpawn& s, uint32_t p) : host(h), token(t), spec(s), pid_(p) {}
  ~Pty() override;
  uint32_t pid() const override { return pid_; }
  int cols() const override { return c; }
  int rows() const override { return r; }
  bool write(const std::string& bytes) override { if (gone) return false; writes.push_back(bytes); return true; }
  bool resize(int cols, int rows) override { if (gone || cols < 1 || rows < 1) return false; c = cols; r = rows; resizes.emplace_back(cols, rows); return true; }
  void kill() override;
  bool exited() const override { return gone; }
  std::vector<uint32_t> processTree() const override { return gone ? std::vector<uint32_t>() : std::vector<uint32_t>{ pid_ }; }
  std::string typed() const { std::string s; for (const std::string& w : writes) s += w; return s; }
  // A freed fake Pty is zeroed, as macOS's allocator zeroes a freed block: a check that reads one the deck has
  // already deleted (its `killed` flag after a shutdown, say) fails HERE as it does on the Mac, instead of passing
  // on whatever the Windows heap left behind. Ask `Ptys::wasKilled(token)` about a pty that may be gone.
  static void operator delete(void* at, std::size_t size) { std::memset(at, 0, size); ::operator delete(at); }
};
struct Ptys : PtyHost {
  std::map<uint64_t, Pty*> live;                         // by token, while the deck holds it
  std::vector<PtySpawn> spawned;
  std::deque<std::pair<uint64_t, uint32_t>> exits;       // to deliver: token, code
  std::string failNext;
  bool exitOnKill = true;                                // a kill is followed by the exit (code 1) at the next pump
  uint32_t nextPid = 5000;
  std::set<uint32_t>* alivePids = nullptr;
  uint64_t lastToken = 0;
  std::set<uint64_t> killedTokens;                       // every pty kill() reached, kept after the deck deletes it
  bool wasKilled(uint64_t token) const { return killedTokens.count(token) > 0; }
  std::unique_ptr<deck::core::Pty> spawn(const PtySpawn& spec, uint64_t token, std::string* error) override {
    if (!failNext.empty()) { if (error) *error = failNext; failNext.clear(); return nullptr; }
    spawned.push_back(spec);
    Pty* p = new Pty(this, token, spec, nextPid++);
    live[token] = p; lastToken = token;
    if (alivePids) alivePids->insert(p->pid_);
    return std::unique_ptr<deck::core::Pty>(p);
  }
  Pty* last() { auto it = live.find(lastToken); return it == live.end() ? nullptr : it->second; }
  Pty* byToken(uint64_t t) { auto it = live.find(t); return it == live.end() ? nullptr : it->second; }
};
inline Pty::~Pty() { host->live.erase(token); }
inline void Pty::kill() { if (gone || killed) return; killed = true; host->killedTokens.insert(token); if (host->exitOnKill) host->exits.emplace_back(token, 1); }

struct Procs : ProcessList {
  std::set<uint32_t> alivePids;
  std::map<uint32_t, uint32_t> parentOf; bool parentsOk = true;
  std::map<uint32_t, int64_t> hostOf; bool hostsOk = true;
  std::vector<uint32_t> killedTrees;
  bool killTreeEnds = true;
  double used = 0, limit = 0;
  bool alive(double pid) override { return pid > 0 && alivePids.count((uint32_t)pid) > 0; }
  bool parents(std::map<uint32_t, uint32_t>& out) override { if (!parentsOk) return false; out = parentOf; return true; }
  bool consoleHosts(const std::vector<uint32_t>& pids, std::map<uint32_t, int64_t>& out) override { if (!hostsOk) return false; for (uint32_t p : pids) out[p] = hostOf.count(p) ? hostOf[p] : -1; return true; }
  void killTree(uint32_t pid) override { killedTrees.push_back(pid); if (killTreeEnds) alivePids.erase(pid); }
  bool commit(double& u, double& l) override { if (!(limit > 0)) return false; u = used; l = limit; return true; }
  std::vector<ProcInfo> snap; bool snapOk = true;                // the one-deck guard's snapshot (onedeck.hpp)
  bool snapshot(std::vector<ProcInfo>& out) override { if (!snapOk) return false; out = snap; return true; }
};

struct Random : deck::core::Random {
  int n = 0;
  std::string uuid() override { char b[48]; std::snprintf(b, sizeof b, "00000000-0000-4000-8000-%012d", ++n); return b; }
  std::string hex(int bytes) override { std::string s; for (int i = 0; i < bytes; i++) s += "ab"; return s; }
};
struct Env : deck::core::Env {
  // POSIX mode: the same variables with POSIX values, and the login shell a Mac user has
  EnvList env = { { "Path", P("C:\\Windows") }, { "CLAUDECODE", "1" }, { "CLAUDE_CODE_ENTRYPOINT", "cli" }, { "AI_AGENT", "x" }, { "NO_COLOR", "1" }, { "USERNAME", "tester" } };
  std::string homeDir = P("C:\\home");
  Env() { if (posix()) env.emplace_back("SHELL", "/bin/zsh"); }
  EnvList environment() override { return env; }
  std::string get(const std::string& name) override { for (const auto& kv : env) if (kv.first == name) return kv.second; return std::string(); }
  std::string os() override { return posix() ? "darwin" : "win32"; }
  std::string home() override { return homeDir; }
  uint32_t pidV = 0;                                     // process.pid (0, as before, unless a test names one)
  uint32_t pid() override { return pidV; }
};
struct Log : deck::core::Log {                   // the deck's log lines, collected
  std::vector<std::string> lines;
  void line(const std::string& text) override { lines.push_back(text); }
  // the lines that hold `part` (all of them, joined, for a failure's message)
  int count(const std::string& part) const { int n = 0; for (const auto& l : lines) if (l.find(part) != std::string::npos) n++; return n; }
  std::string all() const { std::string o; for (const auto& l : lines) o += l + "\n"; return o; }
};
struct Notifier : deck::core::Notifier {
  std::vector<Toast> toasts; std::vector<int> flashes;
  void toast(const Toast& t) override { toasts.push_back(t); }
  void flash(int w) override { flashes.push_back(w); }
};
struct Life : Lifecycle {
  std::vector<std::string> restarts; bool can = true; json print = json::object();
  bool restart(const std::string& why) override { restarts.push_back(why); return can; }
  json fingerprint() override { return print; }
  std::vector<double> watchdogs; std::vector<std::string> opened, openedPaths, shownInFolder;
  struct Win { int client; json bounds; bool inactive; };
  std::vector<Win> windows; bool canOpenWindow = false;
  void armExitWatchdog(double ms) override { watchdogs.push_back(ms); }
  bool openExternal(const std::string& url) override { opened.push_back(url); return true; }
  bool openPath(const std::string& path) override { openedPaths.push_back(path); return true; }
  bool showInFolder(const std::string& path) override { shownInFolder.push_back(path); return true; }
  bool openWindow(int client, const json& bounds, bool inactive) override { if (!canOpenWindow) return false; windows.push_back({ client, bounds, inactive }); return true; }
};
struct Http : deck::core::Http {
  struct Asked { std::string url; std::map<std::string, std::string> headers; std::function<void(const HttpResponse&)> done; };
  std::vector<Asked> asked;
  std::map<std::string, HttpResponse> byToken;       // "Bearer <token>" -> the answer; a missing one is a network error
  bool answerAtOnce = true;
  void get(const std::string& url, const std::map<std::string, std::string>& headers, std::function<void(const HttpResponse&)> done) override {
    asked.push_back({ url, headers, done });
    if (answerAtOnce) answer(asked.size() - 1);
  }
  void answer(size_t i) {
    const auto h = asked[i].headers.find("Authorization");
    const std::string token = h == asked[i].headers.end() ? std::string() : h->second.substr(7);
    auto it = byToken.find(token);
    if (it == byToken.end()) { HttpResponse r; r.error = "fetch failed"; asked[i].done(r); } else asked[i].done(it->second);
  }
};
struct Creds : Credentials {
  std::map<std::string, json> oauth; std::map<std::string, std::string> stamps;
  int reads = 0;
  json readOAuth(const std::string& dir) override { reads++; auto it = oauth.find(dir); return it == oauth.end() ? json() : it->second; }
  std::string stamp(const std::string& dir) override { auto it = stamps.find(dir); return it == stamps.end() ? std::string() : it->second; }
};

struct Listener : deck::core::Listener {
  bool listening = false; std::string host; int port = 0; std::string failWith; int listens = 0, stops = 0;
  std::map<uint64_t, std::string> sent; std::map<uint64_t, size_t> pending; std::map<uint64_t, bool> closed;
  bool listen(const std::string& h, int p, std::string* error) override { listens++; if (!failWith.empty()) { if (error) *error = failWith; return false; } listening = true; host = h; port = p; return true; }
  void stop() override { if (listening) stops++; listening = false; }
  void send(uint64_t conn, const std::string& bytes) override { if (!closed[conn]) sent[conn] += bytes; }
  size_t buffered(uint64_t conn) override { return pending[conn]; }
  void close(uint64_t conn, bool) override { closed[conn] = true; }
  std::string take(uint64_t conn) { std::string s; s.swap(sent[conn]); return s; }
};
struct Net : deck::core::Net {
  std::vector<std::string> addrs; std::string serveStatus; int serveAsked = 0;
  std::vector<std::string> ipv4Addresses() override { return addrs; }
  std::string tailscaleServeStatus() override { serveAsked++; return serveStatus; }
};

}  // namespace fake
