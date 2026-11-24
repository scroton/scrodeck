// mac/Tests/bridge_test.cpp — the proof that the Swift shell has a sound thing to call: the C façade
// (mac/Sources/DeckCoreBridge/include/DeckBridge.h) driven through a whole session against the REAL core, on fake
// seams, AS A MAC WOULD CONFIGURE IT (Env::os() = "darwin", '/' paths, the login shell). No process is started, no
// socket opened, no file touched, nothing appears on screen.
//   powershell -NoProfile -ExecutionPolicy Bypass -File mac/Tests/build.ps1
// The doubles follow core/test/deck/fakes.hpp (a clock that moves when told, files in memory, a pty that records
// what it was asked, a listener that is a string per socket) with POSIX paths.
#include "DeckBridge.h"
#include "DeckBridgeHost.hpp"
#include "KeychainNames.hpp"      // mac/Sources/DeckMacSeams: the pure half of the Keychain read
#include "core/deck/ws.hpp"
#include "core/deck/edition.hpp"
#include "core/third_party/nlohmann/json.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

using nlohmann::json;
using namespace deck::core;

static int passed = 0, failed = 0;
static void check(const std::string& name, bool ok, const std::string& got = "") {
  if (ok) { ++passed; return; }
  ++failed;
  std::printf("FAIL  %s%s%s\n", name.c_str(), got.empty() ? "" : "\n      ", got.substr(0, 900).c_str());
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------- fake seams
namespace fake {
struct Clock : deck::core::Clock { double t = 1800000000000.0; double nowMs() override { return t; } };
static std::string norm(std::string p) { while (p.size() > 1 && p.back() == '/') p.pop_back(); return p; }
struct MemFiles : Files {
  std::map<std::string, std::string> files; std::set<std::string> dirs; Clock* clock = nullptr;
  void put(const std::string& p, const std::string& d) { files[norm(p)] = d; }
  bool has(const std::string& p) const { return files.count(norm(p)) > 0; }
  bool read(const std::string& p, std::string& out) override { auto it = files.find(norm(p)); if (it == files.end()) { out.clear(); return false; } out = it->second; return true; }
  bool readFrom(const std::string& p, uint64_t off, std::string& out, uint64_t* sizeNow) override {
    auto it = files.find(norm(p)); out.clear();
    if (it == files.end()) return false;
    if (sizeNow) *sizeNow = it->second.size();
    if (off < it->second.size()) out = it->second.substr((size_t)off);
    return true;
  }
  FileStat stat(const std::string& pIn) override {
    FileStat st; const std::string p = norm(pIn);
    auto it = files.find(p);
    if (it != files.end()) { st.exists = true; st.size = it->second.size(); st.mtimeMs = clock ? clock->t : 1; return st; }
    if (dirs.count(p)) { st.exists = true; st.isDir = true; return st; }
    const std::string prefix = p + "/";
    auto lb = files.lower_bound(prefix);
    if (lb != files.end() && lb->first.compare(0, prefix.size(), prefix) == 0) { st.exists = true; st.isDir = true; }
    return st;
  }
  std::vector<std::string> list(const std::string& dir) override {
    std::set<std::string> names; const std::string prefix = norm(dir) + "/";
    for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) { const std::string rest = it->first.substr(prefix.size()); names.insert(rest.substr(0, rest.find('/'))); }
    return std::vector<std::string>(names.begin(), names.end());
  }
  bool makeDirs(const std::string& p) override { dirs.insert(norm(p)); return true; }
  bool remove(const std::string& p) override { return files.erase(norm(p)) > 0; }
  bool copy(const std::string& a, const std::string& b) override { auto it = files.find(norm(a)); if (it == files.end()) return false; put(b, it->second); return true; }
  bool writeAtomic(const std::string& p, const std::string& t) override { put(p, t); return true; }
  bool write(const std::string& p, const std::string& b) override { put(p, b); return true; }
  bool rename(const std::string& a, const std::string& b) override { auto it = files.find(norm(a)); if (it == files.end()) return false; const std::string d = it->second; files.erase(it); put(b, d); return true; }
};
struct Ptys;
struct Pty : deck::core::Pty {
  Ptys* host; uint64_t token; PtySpawn spec; uint32_t pid_; int c, r; std::string typed; bool killed = false, gone = false;
  Pty(Ptys* h, uint64_t t, const PtySpawn& s, uint32_t p) : host(h), token(t), spec(s), pid_(p), c(s.cols), r(s.rows) {}
  ~Pty() override;
  uint32_t pid() const override { return pid_; }
  int cols() const override { return c; }
  int rows() const override { return r; }
  bool write(const std::string& b) override { if (gone) return false; typed += b; return true; }
  bool resize(int cols, int rows) override { if (gone || cols < 1 || rows < 1) return false; c = cols; r = rows; return true; }
  void kill() override;
  bool exited() const override { return gone; }
  std::vector<uint32_t> processTree() const override { return gone ? std::vector<uint32_t>() : std::vector<uint32_t>{ pid_ }; }
};
struct Ptys : PtyHost {
  std::shared_ptr<db::Inbox> inbox; std::map<uint64_t, Pty*> live; std::vector<PtySpawn> spawned; uint32_t nextPid = 7000; uint64_t lastToken = 0;
  std::set<uint32_t>* alive = nullptr;
  std::unique_ptr<deck::core::Pty> spawn(const PtySpawn& spec, uint64_t token, std::string*) override {
    spawned.push_back(spec); Pty* p = new Pty(this, token, spec, nextPid++); live[token] = p; lastToken = token;
    if (alive) alive->insert(p->pid_);
    return std::unique_ptr<deck::core::Pty>(p);
  }
  Pty* last() { auto it = live.find(lastToken); return it == live.end() ? nullptr : it->second; }
};
Pty::~Pty() { host->live.erase(token); }
// a kill is followed by the exit, through the inbox, exactly as a real pty's reader thread would deliver it
void Pty::kill() { if (gone || killed) return; killed = true; gone = true; if (host->alive) host->alive->erase(pid_); host->inbox->ptyExit(token, 1); }
struct Procs : ProcessList { std::set<uint32_t> alivePids; bool alive(double pid) override { return pid > 0 && alivePids.count((uint32_t)pid) > 0; } };
struct Random : deck::core::Random {
  int n = 0;
  std::string uuid() override { char b[48]; std::snprintf(b, sizeof b, "00000000-0000-4000-8000-%012d", ++n); return b; }
  std::string hex(int bytes) override { std::string s; for (int i = 0; i < bytes; i++) s += "ab"; return s; }
};
struct Env : deck::core::Env {
  EnvList env = { { "PATH", "/usr/bin:/bin" }, { "SHELL", "/bin/zsh" }, { "HOME", "/Users/t" }, { "USER", "t" } };
  EnvList environment() override { return env; }
  std::string get(const std::string& name) override { for (const auto& kv : env) if (kv.first == name) return kv.second; return std::string(); }
  std::string os() override { return "darwin"; }
  std::string home() override { return "/Users/t"; }
  uint32_t pid() override { return 4242; }
};
struct Listener : deck::core::Listener {
  bool listening = false; std::string host; int port = 0; std::map<uint64_t, std::string> sent; std::map<uint64_t, bool> closed;
  bool listen(const std::string& h, int p, std::string*) override { listening = true; host = h; port = p; return true; }
  void stop() override { listening = false; }
  void send(uint64_t conn, const std::string& bytes) override { if (!closed[conn]) sent[conn] += bytes; }
  size_t buffered(uint64_t) override { return 0; }
  void close(uint64_t conn, bool) override { closed[conn] = true; }
  std::string take(uint64_t conn) { std::string s; s.swap(sent[conn]); return s; }
};
struct Net : deck::core::Net {
  std::vector<std::string> ipv4Addresses() override { return { "192.168.1.20" }; }
  std::string tailscaleServeStatus() override { return ""; }
};
struct Host : DBHost {
  Clock clockS; MemFiles filesS; Ptys ptysS; Procs procsS; Random randomS; Env envS; Listener listenerS; Net netS;
  Host() {
    filesS.clock = &clockS; ptysS.inbox = inbox; ptysS.alive = &procsS.alivePids;
    clock = &clockS; files = &filesS; ptys = &ptysS; procs = &procsS; random = &randomS; env = &envS; listener = &listenerS; net = &netS;
  }
};
}  // namespace fake

// ---------------------------------------------------------------------------------------------- a recording window
struct Rec {
  struct F { int kind, rid, exitCode, replay; std::string id, name, json, bytes; };
  std::vector<F> frames;
  DBSink sink() { return DBSink{ this, [](void* ctx, const DBFrame* f) {
    static_cast<Rec*>(ctx)->frames.push_back({ f->kind, f->rid, f->exit_code, f->replay, f->id, f->name, std::string(f->json, f->json_len), std::string(f->bytes, f->bytes_len) }); } }; }
  const F* reply(int rid) const { for (size_t i = frames.size(); i-- > 0;) if ((frames[i].kind == DB_FRAME_OK || frames[i].kind == DB_FRAME_ERR) && frames[i].rid == rid) return &frames[i]; return nullptr; }
  json result(int rid) const { const F* f = reply(rid); return f && f->kind == DB_FRAME_OK ? json::parse(f->json, nullptr, false) : json(); }
  std::string data(const std::string& id, size_t from = 0) const { std::string s; for (size_t i = from; i < frames.size(); i++) if (frames[i].kind == DB_FRAME_DATA && frames[i].id == id) s += frames[i].bytes; return s; }
  const F* last(int kind) const { for (size_t i = frames.size(); i-- > 0;) if (frames[i].kind == kind) return &frames[i]; return nullptr; }
  size_t count(int kind) const { size_t n = 0; for (const F& f : frames) if (f.kind == kind) n++; return n; }
};

static void advance(DBDeck* deck, fake::Host& h, double ms) {
  const double end = h.clockS.t + ms;
  for (;;) {
    db_deck_pump(deck);
    if (h.clockS.t >= end) break;
    const int due = db_deck_next_due_ms(deck);
    h.clockS.t += std::min(end - h.clockS.t, std::max(1.0, (double)due));
  }
  db_deck_pump(deck);
}
static std::string lineText(DBTerm* t, int absLine) {
  DBCell cells[400]; int wrapped = 0; std::string s;
  const int n = db_term_read_line(t, absLine, cells, 400, &wrapped);
  for (int i = 0; i < n; i++) if (cells[i].width) s.push_back(cells[i].ch ? (char)cells[i].ch : ' ');
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

static int wakes = 0;
struct ShellRec { std::vector<std::string> opened; std::vector<std::string> toasts; int flashes = 0; };

int main(int argc, char** argv) {
  const std::string testsDir = argc > 1 ? argv[1] : "mac/Tests";
  check("abi version", db_abi_version() == 1);
  check("core version is the core's own", std::string(db_core_version()).find("native") != std::string::npos, db_core_version());

  // ============================================================== the deck on fake seams, as a Mac configures it
  fake::Host host;
  const std::string STATE = "/Users/t/Library/Application Support/" DECK_EDITION_STATE_DIR "/local";   // the edition's folder
  host.filesS.put("/app/renderer/phone/index.html", "<html><title>phone</title></html>");
  host.filesS.put("/app/renderer/index.html", "<html><head><title>desk</title></head></html>");
  host.filesS.put("/app/renderer/phone/manifest.webmanifest", "{\"name\":\"Claude Deck\"}");
  db_host_set_wake(&host, [](void*) { wakes++; }, nullptr);

  ShellRec shellRec;
  DBShellCallbacks shell; std::memset(&shell, 0, sizeof shell);
  shell.ctx = &shellRec;
  shell.open_external = [](void* ctx, const char* url) { static_cast<ShellRec*>(ctx)->opened.push_back(url); return 1; };
  shell.toast = [](void* ctx, const DBToast* t) { static_cast<ShellRec*>(ctx)->toasts.push_back(t->title); };
  shell.flash = [](void* ctx, int) { static_cast<ShellRec*>(ctx)->flashes++; };

  DBOptions opt; db_options_init(&opt);
  char err[256];
  check("create: refused without a state dir", db_deck_create(&opt, &host, &shell, err, sizeof err) == nullptr && std::string(err).find("state_dir") != std::string::npos, err);
  opt.state_dir = STATE.c_str(); opt.asset_root = "/app"; opt.serve_port = 8815;
  DBDeck* deck = db_deck_create(&opt, &host, &shell, err, sizeof err);
  check("create: a deck", deck != nullptr, err);
  if (!deck) { std::printf("cannot go on\n"); return 1; }
  check("create: Claude is off unless the shell says so", db_deck_allow_claude(deck) == 0);
  check("create: the state dir as given", std::string(db_deck_state_dir(deck)) == STATE);

  Rec win;
  const DBSink sink = win.sink();
  DBConn* conn = db_deck_add_window(deck, "{\"x\":10,\"y\":20,\"width\":1600,\"height\":960}", &sink);
  check("window: a connection", conn != nullptr);
  check("window: local, client id, index 0", db_conn_is_local(conn) == 1 && db_conn_client(conn) > 0 && db_conn_window_index(conn) == 0);
  check("window: frames were delivered when the call returned, OPEN first", !win.frames.empty() && win.frames[0].kind == DB_FRAME_OPEN, std::to_string(win.frames.size()));
  db_deck_start(deck);
  check("server: --serve listens on loopback", host.listenerS.listening && host.listenerS.host == "127.0.0.1" && host.listenerS.port == 8815, host.listenerS.host + ":" + std::to_string(host.listenerS.port));

  // ---- deskInit: what a window is told (the Mac's platform block comes from Env::os())
  db_conn_call(conn, 1, "deskInit", "[{\"name\":\"test-mac\",\"kind\":\"native\"}]");
  json init = win.result(1);
  check("deskInit: answered", init.is_object(), win.reply(1) ? win.reply(1)->json + win.reply(1)->name : "no reply");
  check("deskInit: platform is darwin", init.is_object() && init["platform"].value("os", "") == "darwin" && init["platform"].value("isMac", false), init.is_object() ? init["platform"].dump() : "");
  check("deskInit: the Mac key help (Cmd+B)", init.is_object() && init["platform"]["keyHelp"].value("sidebar", "") == "Cmd+B");
  check("deskInit: home, grid, window index", init.is_object() && init.value("home", "") == "/Users/t" && init["grid"].value("rows", 0) == 2 && init.value("windowIndex", -1) == 0, init.is_object() ? init["grid"].dump() : "");
  advance(deck, host, 2100);

  // ---- a shell tile on the scripted pty
  db_conn_call(conn, 2, "newTile", "[{\"kind\":\"shell\"}]");
  advance(deck, host, 500);
  const json idj = win.result(2);
  const std::string id = idj.is_string() ? idj.get<std::string>() : "";
  check("newTile: answered with the tile's id", !id.empty(), win.reply(2) ? win.reply(2)->json + win.reply(2)->name : "no reply");
  check("newTile: one process, the login shell (lib/platform.js shellCommand on a Mac)", host.ptysS.spawned.size() == 1 && host.ptysS.spawned[0].file == "/bin/zsh" && host.ptysS.spawned[0].args == std::vector<std::string>{ "-l" },
    host.ptysS.spawned.empty() ? "none" : host.ptysS.spawned[0].file);
  check("newTile: spawned at 120 x 30 in the home folder", !host.ptysS.spawned.empty() && host.ptysS.spawned[0].cols == 120 && host.ptysS.spawned[0].rows == 30 && host.ptysS.spawned[0].cwd == "/Users/t", host.ptysS.spawned.empty() ? "" : host.ptysS.spawned[0].cwd);
  { bool tileEnv = false, home = false; if (!host.ptysS.spawned.empty()) for (const auto& kv : host.ptysS.spawned[0].env) { if (kv.first == "CLAUDE_DECK_TILE") tileEnv = kv.second == id; if (kv.first == "HOME") home = true; }
    check("newTile: the child's environment carries CLAUDE_DECK_TILE and the parent's HOME", tileEnv && home); }
  fake::Pty* pty = host.ptysS.last();
  check("newTile: the pty is live", pty != nullptr);
  if (!pty) { std::printf("cannot go on\n"); return 1; }

  // ---- layout + shown + attach
  DBLayout* lay = db_layout_create(2, 2);
  const int full[] = { 1, 2, 3, 4 };
  check("layout: place", db_layout_place(lay, id.c_str(), full, 4) == 1 && db_layout_count(lay) == 1);
  db_conn_layout(conn, lay);
  const char* ids[] = { id.c_str() };
  db_conn_shown(conn, ids, 1, 0);
  db_conn_window_state(conn, 1, 1);
  db_conn_attach(conn, id.c_str(), 0);
  db_deck_pump(deck);

  // ---- data: pty bytes -> the inbox (as a reader thread pushes them) -> pump -> a DATA frame -> a terminal
  const int wakesBefore = wakes;
  const size_t dataFrom = win.frames.size();
  host.inbox->ptyData(pty->token, "hello from zsh\r\n", 16);
  host.inbox->ptyData(pty->token, "t@mac ~ % ", 10);
  check("inbox: one wake for a burst", wakes == wakesBefore + 1, std::to_string(wakes - wakesBefore));
  check("inbox: nothing reaches the window before the pump", win.data(id, dataFrom).empty());
  db_deck_pump(deck);
  check("data: the bytes, in order, as one burst", win.data(id, dataFrom) == "hello from zsh\r\nt@mac ~ % ", win.data(id, dataFrom));
  DBTerm* term = db_term_create(120, 30, 10000);
  { const std::string d = win.data(id, dataFrom); db_term_feed(term, d.data(), d.size()); }
  DBTermState ts; db_term_state(term, &ts);
  check("term: size and cursor after the prompt", ts.cols == 120 && ts.rows == 30 && ts.cursor_y == 1 && ts.cursor_x == 10, std::to_string(ts.cursor_x) + "," + std::to_string(ts.cursor_y));
  check("term: line 0 reads back", lineText(term, ts.scrollback + 0) == "hello from zsh", lineText(term, ts.scrollback));
  check("term: line 1 reads back", lineText(term, ts.scrollback + 1) == "t@mac ~ %", lineText(term, ts.scrollback + 1));
  int d0 = -1, d1 = -1;
  check("term: damage covers the two rows, then is taken", db_term_take_damage(term, &d0, &d1) == 1 && d0 == 0 && d1 >= 1 && db_term_take_damage(term, &d0, &d1) == 0, std::to_string(d0) + ".." + std::to_string(d1));
  { size_t n = 0; const char* t = db_term_text(term, 6, ts.scrollback, 8, ts.scrollback + 1, &n);
    check("term: a selection across two lines", std::string(t, n) == "from zsh\nt@mac ~ %", std::string(t, n)); }
  { const char sgr[] = "\x1b[1;31mX\x1b[0m\x1b[38;2;1;2;3mY\x1b[48;5;200mZ"; db_term_feed(term, sgr, sizeof sgr - 1);
    DBCell c[120]; db_term_read_line(term, ts.scrollback + 1, c, 120, nullptr);
    check("term: SGR — bold red X (palette 1)", c[10].ch == 'X' && (c[10].flags & DB_BOLD) && c[10].fg == (DB_COLOR_PALETTE | 1) && c[10].bg == DB_COLOR_DEFAULT);
    check("term: SGR — truecolour Y", c[11].ch == 'Y' && c[11].fg == 0x010203 && !(c[11].flags & DB_BOLD));
    check("term: SGR — palette background Z", c[12].ch == 'Z' && c[12].bg == (DB_COLOR_PALETTE | 200)); }
  { const char q[] = "\x1b[6n"; db_term_feed(term, q, 4); size_t n = 0; const char* r = db_term_take_replies(term, &n);
    check("term: a cursor report is a reply (local mode writes it back)", std::string(r, n) == "\x1b[2;14R", std::string(r + 1, n ? n - 1 : 0)); }
  { const char u[] = "\r\ne\xcc\x81 \xe6\x97\xa5"; db_term_feed(term, u, sizeof u - 1);
    DBCell c[120]; db_term_read_line(term, ts.scrollback + 2, c, 120, nullptr); uint32_t cp[8];
    check("term: a combining mark is a cluster", c[0].ch == 'e' && c[0].cluster == 1 && db_term_cell_cluster(term, ts.scrollback + 2, 0, cp, 8) == 2 && cp[1] == 0x301);
    check("term: a wide character takes two cells", c[2].ch == 0x65E5 && c[2].width == 2 && c[3].width == 0); }
  { const char m[] = "\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[?2004h\x1b[?1h"; db_term_feed(term, m, sizeof m - 1); DBTermState s2; db_term_state(term, &s2);
    check("term: modes — alt screen, mouse 1000 + SGR, bracketed paste, app cursor keys", s2.alt_screen && s2.mouse_mode == 1000 && s2.mouse_encoding == 1006 && s2.bracketed_paste && s2.app_cursor_keys); }
  { const char o[] = "\x1b]0;my title\x07"; db_term_feed(term, o, sizeof o - 1); check("term: the OSC title", std::string(db_term_title(term)) == "my title" && db_term_take_bell(term) == 0, db_term_title(term)); }
  db_term_resize(term, 80, 24); db_term_state(term, &ts);
  check("term: resize", ts.cols == 80 && ts.rows == 24);
  db_term_destroy(term);

  // ---- input: a real keystroke reaches the process
  db_conn_input(conn, id.c_str(), "ls -la\r", 7);
  check("input: typed into the pty", pty->typed == "ls -la\r", pty->typed);
  check("input: an empty input sends nothing", (db_conn_input(conn, id.c_str(), "", 0), pty->typed == "ls -la\r"));

  // ---- resize: the window drives its own pane (the 150 ms debounce runs in the pump)
  db_conn_resize(conn, id.c_str(), 100, 40);
  advance(deck, host, 400);
  int pc = 0, pr = 0; db_deck_pty_size(deck, id.c_str(), &pc, &pr);
  check("resize: the pty was resized", pty->c == 100 && pty->r == 40 && pc == 100 && pr == 40, std::to_string(pty->c) + "x" + std::to_string(pty->r));

  // ---- meta: the 2 s snapshot names the tile
  { const size_t before = win.count(DB_FRAME_META);
    advance(deck, host, 2100);
    const Rec::F* m = win.last(DB_FRAME_META);
    check("meta: a snapshot arrived on the tick", win.count(DB_FRAME_META) > before && m != nullptr);
    const json mj = m ? json::parse(m->json, nullptr, false) : json();
    bool found = false; std::string kind;
    if (mj.is_object() && mj["meta"].is_object()) for (const json& t : mj["meta"]["tiles"]) if (t.value("id", "") == id) { found = true; kind = t.value("kind", ""); }
    check("meta: the shell tile is in it", found && kind == "shell", kind); }
  check("deck: one live process", db_deck_live_count(deck) == 1, std::to_string(db_deck_live_count(deck)));

  // ---- a call that goes through the shell's Lifecycle callback
  db_conn_call(conn, 3, "openExternal", "[\"https://claude.ai/code\"]");
  db_conn_call(conn, 31, "openExternal", "[\"https://example.com/\"]");
  check("lifecycle: the core decides which links open (an err frame names why)", win.reply(31) && win.reply(31)->kind == DB_FRAME_ERR && win.reply(31)->name.find("claude.ai") != std::string::npos && shellRec.opened.size() == 1);
  check("lifecycle: openExternal reached the shell's callback", shellRec.opened.size() == 1 && shellRec.opened[0] == "https://claude.ai/code", shellRec.opened.empty() ? "none" : shellRec.opened[0]);

  // ---- a Claude tile: built, shown — and NOT started (allow_claude is 0)
  { const size_t before = host.ptysS.spawned.size();
    db_conn_call(conn, 4, "newTile", "[{\"kind\":\"claude\",\"cwd\":\"/Users/t\"}]");
    advance(deck, host, 1500);
    bool claude = false; for (size_t i = before; i < host.ptysS.spawned.size(); i++) if (host.ptysS.spawned[i].file.find("claude") != std::string::npos) claude = true;
    check("claude: no Claude process is started with the switch off", !claude && host.ptysS.spawned.size() == before, std::to_string(host.ptysS.spawned.size() - before)); }

  // ---- re-entrancy: a call made from inside a frame callback is answered after the callback returned
  { struct Re { DBConn* conn = nullptr; int depth = 0, maxDepth = 0, oks = 0; } re;
    DBSink s{ &re, [](void* ctx, const DBFrame* f) {
      Re* r = static_cast<Re*>(ctx); r->depth++; if (r->depth > r->maxDepth) r->maxDepth = r->depth;
      if (f->kind == DB_FRAME_OPEN) db_conn_call(r->conn, 71, "getSettings", nullptr);
      if (f->kind == DB_FRAME_OK && f->rid == 71) { r->oks++; db_conn_call(r->conn, 72, "getSettings", nullptr); }
      if (f->kind == DB_FRAME_OK && f->rid == 72) r->oks++;
      r->depth--; } };
    // the sink needs the connection it is called for: adopt-style two-step is not needed — OPEN is queued, not sent inline
    DBConn* c2 = nullptr;
    struct Late { Re* re; DBConn** out; };
    c2 = db_deck_add_window(deck, nullptr, &s);
    check("re-entrancy: (precondition) OPEN was delivered before add_window returned, with no connection known yet", re.maxDepth == 1 && re.oks == 0);
    re.conn = c2;
    db_conn_call(c2, 71, "getSettings", nullptr);
    check("re-entrancy: nested calls are answered, never inside a callback", re.oks == 2 && re.maxDepth == 1, std::to_string(re.oks) + " oks, depth " + std::to_string(re.maxDepth));
    check("window: the second window has index 1", db_conn_window_index(c2) == 1, std::to_string(db_conn_window_index(c2)));
    db_conn_close(c2);
    check("window: closing a window leaves the tile's process running", db_deck_live_count(deck) == 1); }

  // ============================================================== the server, in memory (the fake listener)
  const json status = json::parse(db_deck_phone_status_json(deck), nullptr, false);
  check("server: status says running on 8815, pages found", status.is_object() && status.value("running", false) && status.value("port", 0) == 8815 && status.value("assets", "") == "ok", status.dump().substr(0, 300));
  const std::string token = status.is_object() && status["token"].is_string() ? status["token"].get<std::string>() : "";
  check("server: a token", !token.empty());
  auto http = [&](uint64_t c, const std::string& req) { host.inbox->netAccept(c); host.inbox->netBytes(c, req.data(), req.size()); db_deck_pump(deck); return host.listenerS.take(c); };
  { const std::string r = http(11, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    check("server: the page without the token is 401", r.compare(0, 12, "HTTP/1.1 401") == 0, r.substr(0, 60)); }
  { const std::string r = http(12, "GET /manifest.webmanifest HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    check("server: the manifest is public", r.compare(0, 12, "HTTP/1.1 200") == 0 && r.find("Claude Deck") != std::string::npos, r.substr(0, 60)); }
  { const std::string r = http(13, "GET /?t=" + token + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    check("server: the phone page with the token, and the cookie is set", r.compare(0, 12, "HTTP/1.1 200") == 0 && r.find("<title>phone</title>") != std::string::npos && r.find("set-cookie: deck_phone=") != std::string::npos, r.substr(0, 200)); }

  // ---- a REMOTE connection of the bridge talking to the bridge's own server over a websocket made of strings:
  //      exactly what the Swift shell's remote mode does with URLSessionWebSocketTask in the middle
  struct Wire { fake::Host* host; uint64_t sock; uint32_t key = 0x01020304; std::vector<std::string> sentText; };
  Wire wire; wire.host = &host; wire.sock = 21;
  Rec remote;
  const DBSink rsink = remote.sink();
  DBConn* rc = db_conn_remote(&rsink, [](void* ctx, const char* text, size_t n) {
    Wire* w = static_cast<Wire*>(ctx); w->sentText.push_back(std::string(text, n));
    const std::string f = wsFrame(WsText, std::string(text, n), true, true, w->key++);     // a client masks
    w->host->inbox->netBytes(w->sock, f.data(), f.size()); }, &wire);
  check("remote: not local, no client id", db_conn_is_local(rc) == 0 && db_conn_client(rc) == 0 && db_conn_window_index(rc) == -1);
  WsDecoder fromServer(false);
  auto pumpRemote = [&]() {
    db_deck_pump(deck);
    const std::string bytes = host.listenerS.take(wire.sock);
    std::vector<WsEvent> evs; fromServer.feed(bytes.data(), bytes.size(), evs);
    for (const WsEvent& e : evs) if (e.kind == WsEvent::Message) db_conn_receive_text(rc, e.data.data(), e.data.size());
  };
  { const std::string up = http(wire.sock, "GET /desk/ws?t=" + token + " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    const size_t end = up.find("\r\n\r\n");
    check("remote: the upgrade is 101 with RFC 6455's accept key", up.compare(0, 12, "HTTP/1.1 101") == 0 && up.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos, up.substr(0, 80));
    if (end != std::string::npos) { std::vector<WsEvent> evs; fromServer.feed(up.data() + end + 4, up.size() - end - 4, evs); for (const WsEvent& e : evs) if (e.kind == WsEvent::Message) db_conn_receive_text(rc, e.data.data(), e.data.size()); } }
  check("remote: the hello arrived as an INIT frame", remote.count(DB_FRAME_INIT) == 1, std::to_string(remote.frames.size()));
  db_conn_call(rc, 1, "deskInit", "[{\"name\":\"other-mac\",\"kind\":\"native\"}]");
  check("remote: the frame on the wire is the codec's", !wire.sentText.empty() && json::parse(wire.sentText[0], nullptr, false) == json{ { "t", "call" }, { "rid", 1 }, { "fn", "deskInit" }, { "args", json::array({ { { "name", "other-mac" }, { "kind", "native" } } }) } }, wire.sentText.empty() ? "" : wire.sentText[0]);
  pumpRemote();
  { const json r = remote.result(1);
    check("remote: deskInit answered over the socket", r.is_object() && r["platform"].value("os", "") == "darwin", remote.reply(1) ? remote.reply(1)->name : "no reply");
    bool has = false; if (r.is_object() && r["meta"].is_object()) for (const json& t : r["meta"]["tiles"]) if (t.value("id", "") == id) has = true;
    check("remote: its meta has the tile", has); }
  { const json st = json::parse(db_deck_phone_status_json(deck), nullptr, false);
    check("server: one desk connected", st.is_object() && st.value("desks", 0) == 1, st.dump().substr(0, 200)); }
  db_conn_attach(rc, id.c_str(), 0);
  pumpRemote();
  { const size_t from = remote.frames.size();
    host.inbox->ptyData(pty->token, "remote sees this\r\n", 18);
    pumpRemote();
    check("remote: live data reaches the remote desk", remote.data(id, from) == "remote sees this\r\n", remote.data(id, from));
    check("remote: and the local window, which also carries the tile", win.data(id).find("remote sees this") != std::string::npos); }
  { const std::string before = pty->typed;
    db_conn_input(rc, id.c_str(), "x", 1); pumpRemote();
    check("remote: a keystroke reaches the pty", pty->typed == before + "x", pty->typed); }
  check("remote: wheel — one SGR report is built, anything else refused", db_conn_wheel(rc, id.c_str(), "\x1b[<64;10;5M", 11) == 1 && db_conn_wheel(rc, id.c_str(), "\x1b[A", 3) == 0 && db_conn_wheel(rc, id.c_str(), "\x1b[<0;10;5M", 10) == 0);
  check("remote: R8 — mouse and focus reports are recognised as synthesised", db_frame_synthesised("\x1b[<0;1;1M\x1b[I", 12) == 1 && db_frame_synthesised("a", 1) == 0);
  { wire.sentText.clear(); db_conn_layout(rc, lay); db_conn_leaving(rc, id.c_str(), 1); db_conn_detach(rc, nullptr);
    check("remote: layout / leaving / detach-all frames", wire.sentText.size() == 3 && json::parse(wire.sentText[0], nullptr, false) == json{ { "t", "layout" }, { "layout", { { id, { 1, 2, 3, 4 } } } } }
      && json::parse(wire.sentText[1], nullptr, false) == json{ { "t", "leaving" }, { "id", id }, { "on", true } } && json::parse(wire.sentText[2], nullptr, false) == json{ { "t", "detach" } }, wire.sentText.empty() ? "" : wire.sentText[0]); }
  pumpRemote();
  db_conn_close(rc);
  host.inbox->netClosed(wire.sock); db_deck_pump(deck);
  { const json st = json::parse(db_deck_phone_status_json(deck), nullptr, false);
    check("server: the desk is gone after its socket closed", st.is_object() && st.value("desks", 1) == 0, st.dump().substr(0, 200)); }

  // ============================================================== exit, close, shutdown
  db_conn_call(conn, 5, "newTile", "[{\"kind\":\"shell\"}]");
  advance(deck, host, 500);
  const json id2j = win.result(5);
  const std::string id2 = id2j.is_string() ? id2j.get<std::string>() : "";
  fake::Pty* pty2 = host.ptysS.last();
  check("second tile: its own process", !id2.empty() && pty2 && pty2 != pty && db_deck_live_count(deck) == 2);
  if (pty2) {
    pty2->gone = true; host.procsS.alivePids.erase(pty2->pid_);
    host.inbox->ptyData(pty2->token, "bye\r\n", 5); host.inbox->ptyExit(pty2->token, 3);
    db_conn_attach(conn, id2.c_str(), 0);
    db_deck_pump(deck);
    const Rec::F* ex = win.last(DB_FRAME_EXIT);
    check("exit: an EXIT frame with the tile and the code", ex && ex->id == id2 && ex->exitCode == 3, ex ? ex->id + " " + std::to_string(ex->exitCode) : "none");
  }
  db_conn_call(conn, 6, "closeTile", (std::string("[\"") + id + "\"]").c_str());
  advance(deck, host, 3000);
  check("close: the process was killed and its exit taken", host.ptysS.live.empty() && db_deck_live_count(deck) == 0, std::to_string(db_deck_live_count(deck)));
  { const Rec::F* m = (advance(deck, host, 2100), win.last(DB_FRAME_META)); const json mj = m ? json::parse(m->json, nullptr, false) : json();
    bool still = false; if (mj.is_object() && mj["meta"].is_object()) for (const json& t : mj["meta"]["tiles"]) if (t.value("id", "") == id) still = true;
    check("close: the tile is gone from the meta", !still); }
  check("state: settings.json was written under the state dir, with POSIX separators", host.filesS.has(STATE + "/settings.json") || host.filesS.has(STATE + "/tiles.json"),
    [&] { std::string s; for (const auto& kv : host.filesS.files) if (kv.first.find(DECK_EDITION_STATE_DIR) != std::string::npos) s += kv.first + " "; return s; }());
  { bool backslash = false; for (const auto& kv : host.filesS.files) if (kv.first.find('\\') != std::string::npos) backslash = true;
    check("state: no path the core wrote has a backslash in it (setPathSep('/'))", !backslash); }
  { const json saved = json::parse(db_deck_saved_windows_json(deck), nullptr, false); check("state: saved windows is an array", saved.is_array(), saved.dump().substr(0, 100)); }
  check("shell: no toast and no flash were raised by this script", shellRec.toasts.empty() && shellRec.flashes == 0);
  db_conn_close(conn);
  db_deck_shutdown(deck);
  check("shutdown: the listener is stopped, pumping is inert", !host.listenerS.listening && (db_deck_pump(deck), db_deck_next_due_ms(deck) == 2000));
  check("ask downloads: [] while the ask feed is off", std::string(db_deck_ask_downloads_json(deck)) == "[]", db_deck_ask_downloads_json(deck));
  db_deck_destroy(deck);
  db_layout_destroy(lay);
  check("ask downloads: no deck reads as []", std::string(db_deck_ask_downloads_json(nullptr)) == "[]");

  // ---- the ask feed's Done folders (an older page's own Done is saved in the first that exists: AskPane.swift)
  { fake::Host h2;
    DBOptions o2; db_options_init(&o2);
    const std::string st2 = "/Users/t/scratch-asks-deck";
    o2.state_dir = st2.c_str(); o2.ask_state_dir = "/Users/t/scratch-ask-state"; o2.ask_downloads = "/Users/t/Downloads\n/Users/t/Desktop";
    char e2[256];
    DBDeck* d2 = db_deck_create(&o2, &h2, nullptr, e2, sizeof e2);
    check("ask downloads: a deck with the feed on a named folder", d2 != nullptr, e2);
    if (d2) {
      const json dl = json::parse(db_deck_ask_downloads_json(d2), nullptr, false);
      check("ask downloads: the named folders, in order", dl == json::array({ "/Users/t/Downloads", "/Users/t/Desktop" }), dl.dump());
      db_deck_destroy(d2);
    } }

  // ============================================================== the layout rules through the C surface
  { DBLayout* l = db_layout_create(2, 2);
    const int c1[] = { 1 }, c12[] = { 1, 2 }, c14[] = { 1, 4 }, c3[] = { 3 };
    db_layout_place(l, "a", c1, 1); db_layout_place(l, "b", c3, 1);
    db_layout_place(l, "c", c12, 2);
    check("layout: R6 — the covered tile is evicted", db_layout_evicted_count(l) == 1 && std::string(db_layout_evicted_at(l, 0)) == "a" && db_layout_count(l) == 2);
    double x, y, w, h;
    check("layout: geometry of the top half", db_layout_rect(l, "c", &x, &y, &w, &h) == 1 && x == 0 && y == 0 && w == 1 && h == 0.5);
    check("layout: an unplaced tile has no rectangle", db_layout_rect(l, "a", &x, &y, &w, &h) == 0);
    db_layout_place(l, "d", c14, 2); int out[DB_MAX_CELLS];
    check("layout: R1 — a diagonal is promoted to the whole grid, everyone else evicted", db_layout_cells(l, "d", out, DB_MAX_CELLS) == 4 && db_layout_count(l) == 1 && db_layout_evicted_count(l) == 2);
    db_layout_clear(l);
    check("layout: a drop on an empty layout takes the whole grid", db_layout_drop_target(l, 0.2, 0.2, out, DB_MAX_CELLS) == 4);
    db_layout_place(l, "a", c1, 1); db_layout_place(l, "b", c3, 1);
    check("layout: json out and back", std::string(db_layout_json(l)) == "{\"a\":[1],\"b\":[3]}" && db_layout_load_json(l, "{\"z\":[2,4],\"bad\":[1,4]}") == 1 && db_layout_count(l) == 1 && std::string(db_layout_id_at(l, 0)) == "z", db_layout_json(l));
    db_layout_regrid(l, 3, 3); int r = 0, c = 0; db_layout_grid(l, &r, &c);
    check("layout: regrid keeps the block's rows and columns", r == 3 && c == 3 && db_layout_cells(l, "z", out, DB_MAX_CELLS) == 2 && out[0] == 2 && out[1] == 5, std::to_string(out[0]) + "," + std::to_string(out[1]));
    const char* live[] = { "nobody" }; db_layout_prune(l, live, 1);
    check("layout: prune drops what is not live", db_layout_count(l) == 0);
    db_layout_destroy(l); }

  // ============================================================== keys (windows/src/keys.hpp, row for row)
  { char b[16];
    auto key = [&](int k, int mods, int app) { const int n = db_key_bytes(k, mods, app, b, 16); return n < 0 ? std::string("<none>") : std::string(b, (size_t)n); };
    check("keys: arrows, plain and application mode", key(DB_KEY_UP, 0, 0) == "\x1b[A" && key(DB_KEY_UP, 0, 1) == "\x1bOA" && key(DB_KEY_LEFT, 0, 0) == "\x1b[D");
    check("keys: modified arrows (Shift 2, Alt 3, Ctrl 5)", key(DB_KEY_RIGHT, DB_MOD_SHIFT, 0) == "\x1b[1;2C" && key(DB_KEY_RIGHT, DB_MOD_ALT, 1) == "\x1b[1;3C" && key(DB_KEY_DOWN, DB_MOD_CTRL | DB_MOD_SHIFT, 0) == "\x1b[1;6B");
    check("keys: Cmd is never part of a sequence", key(DB_KEY_UP, DB_MOD_META, 0) == "\x1b[A");
    check("keys: Home / End / PageUp / Delete", key(DB_KEY_HOME, 0, 0) == "\x1b[H" && key(DB_KEY_END, 0, 1) == "\x1bOF" && key(DB_KEY_PAGE_UP, 0, 0) == "\x1b[5~" && key(DB_KEY_PAGE_UP, DB_MOD_SHIFT, 0) == "\x1b[5;2~" && key(DB_KEY_DELETE, 0, 0) == "\x1b[3~");
    check("keys: F1-F4 SS3, F5-F12 tilde", key(DB_KEY_F1, 0, 0) == "\x1bOP" && key(DB_KEY_F4, DB_MOD_SHIFT, 0) == "\x1b[1;2S" && key(DB_KEY_F5, 0, 0) == "\x1b[15~" && key(DB_KEY_F12, 0, 0) == "\x1b[24~" && key(DB_KEY_F11, DB_MOD_CTRL, 0) == "\x1b[23;5~");
    check("keys: Enter, Shift+Enter (the deck's newline), Alt+Enter", key(DB_KEY_ENTER, 0, 0) == "\r" && key(DB_KEY_ENTER, DB_MOD_SHIFT, 0) == "\x1b\r" && key(DB_KEY_ENTER, DB_MOD_ALT, 0) == "\x1b\r");
    check("keys: Backspace, Ctrl+Backspace, Alt+Backspace", key(DB_KEY_BACKSPACE, 0, 0) == "\x7f" && key(DB_KEY_BACKSPACE, DB_MOD_CTRL, 0) == "\x08" && key(DB_KEY_BACKSPACE, DB_MOD_ALT, 0) == "\x1b\x7f");
    check("keys: Tab, Shift+Tab; Ctrl+Tab is the window's", key(DB_KEY_TAB, 0, 0) == "\t" && key(DB_KEY_TAB, DB_MOD_SHIFT, 0) == "\x1b[Z" && key(DB_KEY_TAB, DB_MOD_CTRL, 0) == "<none>");
    check("keys: Esc, Alt+Esc, Ctrl+Space = NUL, Space is text", key(DB_KEY_ESCAPE, 0, 0) == "\x1b" && key(DB_KEY_ESCAPE, DB_MOD_ALT, 0) == "\x1b\x1b" && key(DB_KEY_SPACE, DB_MOD_CTRL, 0) == std::string(1, '\0') && key(DB_KEY_SPACE, 0, 0) == "<none>");
    check("keys: Insert, and Shift+Insert left to the shell (a paste)", key(DB_KEY_INSERT, 0, 0) == "\x1b[2~" && key(DB_KEY_INSERT, DB_MOD_SHIFT, 0) == "<none>");
    check("keys: Option as Meta", db_meta_text("b", 1, b, 16) == 2 && b[0] == 0x1b && b[1] == 'b');
    size_t n = 0; const char* p = db_paste_bytes("a\r\nb\nc", 6, 0, &n);
    check("keys: paste — line ends become CR", std::string(p, n) == "a\rb\rc", std::string(p, n));
    p = db_paste_bytes("x\x1b[201~y", 8, 1, &n);
    check("keys: paste — bracketed, and it cannot end its own bracket", std::string(p, n) == "\x1b[200~xy\x1b[201~");
    check("keys: the Mac window keys (lib/platform.js keyAction)", std::string(db_key_action("darwin", "b", "", DB_MOD_META)) == "sidebar" && std::string(db_key_action("darwin", "P", "", DB_MOD_META | DB_MOD_SHIFT)) == "palette"
      && std::string(db_key_action("darwin", "]", "", DB_MOD_META | DB_MOD_SHIFT)) == "next" && std::string(db_key_action("darwin", "f", "", DB_MOD_META | DB_MOD_CTRL)) == "fullscreen"
      && std::string(db_key_action("darwin", ",", "", DB_MOD_META)) == "settings" && std::string(db_key_action("darwin", "r", "", DB_MOD_META)) == "redraw"
      && std::string(db_key_action("darwin", "Enter", "", DB_MOD_META | DB_MOD_SHIFT)) == "zoom" && std::string(db_key_action("darwin", "a", "", DB_MOD_META | DB_MOD_SHIFT)) == "asks"
      && std::string(db_key_action("darwin", "b", "", 0)) == "" && std::string(db_key_action("darwin", "F9", "", 0)) == "");
    check("keys: the Windows window keys through the same function", std::string(db_key_action("win32", "F9", "", 0)) == "sidebar" && std::string(db_key_action("win32", "`", "Backquote", DB_MOD_CTRL)) == "sidebar");
    check("keys: key help and the font stack for a Mac", std::string(db_key_help_json("darwin")).find("Cmd+Shift+P") != std::string::npos && std::string(db_default_font_stack("darwin")).find("SF Mono") != std::string::npos); }

  // ============================================================== mouse (windows/src/mouse.hpp behind the C surface)
  { DBMouse* m = db_mouse_create(); char b[64];
    DBMouseEvent e; std::memset(&e, 0, sizeof e);
    db_mouse_coords(85.0, 40.0, 8.0, 17.0, 120, 30, &e);
    check("mouse: the cell under a point", e.col == 10 && e.row == 2 && e.x == 85 && e.y == 40);
    e.button = DB_BTN_LEFT; e.action = DB_ACT_DOWN;
    int n = db_mouse_report(m, &e, 1000, 1006, 120, 30, b, 64);
    check("mouse: SGR press", n > 0 && std::string(b, (size_t)n) == "\x1b[<0;11;3M", n > 0 ? std::string(b + 1, (size_t)n - 1) : "none");
    e.action = DB_ACT_UP; n = db_mouse_report(m, &e, 1000, 1006, 120, 30, b, 64);
    check("mouse: SGR release", n > 0 && std::string(b, (size_t)n) == "\x1b[<0;11;3m");
    e.action = DB_ACT_MOVE; check("mouse: a move is not for mode 1000", db_mouse_report(m, &e, 1000, 1006, 120, 30, b, 64) == -1);
    e.action = DB_ACT_DOWN; check("mouse: nothing with tracking off", db_mouse_report(m, &e, 0, 0, 120, 30, b, 64) == -1);
    n = db_mouse_report(m, &e, 1000, 0, 120, 30, b, 64);
    check("mouse: X10 bytes", n == 6 && std::string(b, 3) == "\x1b[M" && b[3] == 32 && b[4] == 43 && b[5] == 35);
    check("mouse: the wheel sums pixels into lines", db_mouse_wheel_lines(m, 100, 17, 0) == 5 && db_mouse_wheel_lines(m, 100, 17, 1) == 0);
    db_mouse_reset(m); db_mouse_destroy(m); }

  // ============================================================== the Keychain item names (lib/platform.js, by vectors)
  { std::ifstream f(testsDir + "/keychain-names.json", std::ios::binary); std::stringstream ss; ss << f.rdbuf();
    const json v = json::parse(ss.str(), nullptr, false);
    check("keychain: the vectors written by lib/platform.js are there (node mac/Tests/gen-keychain-names.js)", v.is_object() && v["cases"].is_array() && v["cases"].size() >= 10, testsDir);
    if (v.is_object()) for (const json& c : v["cases"]) {
      const std::vector<std::string> got = deck::mac::keychainServiceNames(c["dir"].get<std::string>(), v["home"].get<std::string>(), v["cwd"].get<std::string>());
      check("keychain: service names for '" + c["dir"].get<std::string>() + "'", json(got) == c["names"], json(got).dump());
    }
    check("keychain: the chunk accounts, and the limits keychainPayload enforces", deck::mac::keychainChunkAccounts(2) == std::vector<std::string>{ "claude-code-user#0", "claude-code-user#1" }
      && deck::mac::keychainChunkAccounts(0).empty() && deck::mac::keychainChunkAccounts(257).empty() && deck::mac::keychainChunkAccounts(256).size() == 256); }

  std::printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
