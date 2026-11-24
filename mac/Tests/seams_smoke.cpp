// mac/Tests/seams_smoke.cpp — the FIRST thing to run on a Mac after the core's own tests: every macOS seam of
// mac/Sources/DeckMacSeams once, for real, in a temp folder and on loopback only — then the whole deck on them
// (a shell tile running `/bin/echo`), through the same C façade the Swift app uses.
// UNBUILT, UNRUN: written on Windows. mac/README.md "First-build checklist" step 3 has the command line.
// It starts /bin/sh and /bin/echo, binds 127.0.0.1:8811, writes under $TMPDIR. It never starts Claude, never
// touches ~/Library, never reads the Keychain, never looks at the network beyond listing this machine's addresses.
#include "DeckBridge.h"
#include "DeckBridgeHost.hpp"
#include "DeckMacSeams.h"
#include "MacSeams.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace deck;
static int passed = 0, failed = 0;
static void check(const char* name, bool ok, const std::string& got = "") { if (ok) { passed++; return; } failed++; std::printf("FAIL  %s  %s\n", name, got.c_str()); }
static void sleepMs(int ms) { usleep((useconds_t)ms * 1000); }

int main() {
  char tmpl[] = "/tmp/deck-seams-XXXXXX";
  const std::string dir = mkdtemp(tmpl) ? tmpl : "/tmp";
  mac::MacClock clock; mac::PosixFiles files; mac::MacRandom random; mac::MacProcessList procs; mac::MacEnv env(false); mac::MacNet net(false);

  // ---- clock, random, env
  check("clock: after 2026", clock.nowMs() > 1767225600000.0);
  check("random: a version-4 uuid", random.uuid().size() == 36 && random.uuid()[14] == '4', random.uuid());
  check("random: hex(8) is 16 characters", random.hex(8).size() == 16);
  check("env: darwin, a home, a PATH, this pid", env.os() == "darwin" && !env.home().empty() && !env.get("PATH").empty() && env.pid() == (uint32_t)getpid());
  check("env: resolvePath normalises", env.resolvePath("/a/./b/../c") == "/a/c", env.resolvePath("/a/./b/../c"));
  { mac::MacEnv login(true); check("env: the login shell's environment has a PATH and a HOME", !login.get("PATH").empty() && !login.get("HOME").empty(), login.get("PATH")); }

  // ---- files
  const std::string f = dir + "/sub/dir/state.json";
  check("files: makeDirs", files.makeDirs(dir + "/sub/dir") && files.isDir(dir + "/sub/dir"));
  check("files: writeAtomic, read", files.writeAtomic(f, "{\"a\":1}") && files.isFile(f));
  { std::string s; check("files: read back", files.read(f, s) && s == "{\"a\":1}", s); }
  { std::string s; uint64_t size = 0; check("files: readFrom an offset, and the size", files.readFrom(f, 2, s, &size) && s == "a\":1}" && size == 7, s); }
  { std::string s; check("files: readRange", files.readRange(f, 1, 3, s) && s == "\"a\"", s); }
  check("files: stat (size, a recent mtime)", files.stat(f).size == 7 && files.stat(f).mtimeMs > clock.nowMs() - 60000);
  check("files: copy, list sorted", files.copy(f, dir + "/sub/dir/a.json") && files.list(dir + "/sub/dir") == std::vector<std::string>{ "a.json", "state.json" });
  check("files: rename replaces, and fails when the source is gone", files.rename(dir + "/sub/dir/a.json", f) && !files.rename(dir + "/sub/dir/a.json", f));
  check("files: no temp file is left behind", files.list(dir + "/sub/dir").size() == 1);
  check("files: remove", files.remove(f) && !files.stat(f).exists);

  // ---- processes
  { std::map<uint32_t, uint32_t> parents; check("procs: the parent map has this process under its parent", procs.parents(parents) && parents.count((uint32_t)getpid()) && parents[(uint32_t)getpid()] == (uint32_t)getppid()); }
  check("procs: alive(self), not alive(a pid that cannot be)", procs.alive(getpid()) && !procs.alive(3999999));
  check("net: addresses are listed without failing", (net.ipv4Addresses(), true));
  { int code = -1; check("runCapture: stdout and the exit code", mac::runCapture("/bin/echo", { "hi" }, 3000, nullptr, &code) == "hi\n" && code == 0);
    check("runCapture: a timeout kills it and answers nothing", mac::runCapture("/bin/sleep", { "5" }, 300).empty()); }

  // ---- a pty, alone
  { auto inbox = std::make_shared<db::Inbox>(); mac::MacPtyHost host(inbox);
    core::PtySpawn spec; spec.file = "sh"; spec.args = { "-c", "printf 'cols:'; stty size; exit 7" }; spec.env = env.environment(); spec.cwd = dir; spec.cols = 100; spec.rows = 40;
    std::string err; auto pty = host.spawn(spec, 1, &err);
    check("pty: spawned by a bare name on the PATH", pty != nullptr, err);
    std::string out; bool exited = false; uint32_t code = 99; bool dataAfterExit = false;
    for (int i = 0; i < 100 && !exited; i++) { sleepMs(50); std::deque<db::Inbox::Event> q; inbox->take(q);
      for (auto& e : q) { if (e.kind == db::Inbox::PtyData) { if (exited) dataAfterExit = true; out += e.bytes; } if (e.kind == db::Inbox::PtyExit) { exited = true; code = e.code; } } }
    check("pty: the child saw the size it was spawned at (rows cols)", out.find("cols:40 100") != std::string::npos, out);
    check("pty: the exit code, after the last byte", exited && code == 7 && !dataAfterExit, std::to_string(code));
    check("pty: a missing program is an error, not a dead child", (spec.file = "no-such-program-xyz", host.spawn(spec, 2, &err) == nullptr) && err.find("File not found") == 0, err);
    spec.file = "/bin/cat"; spec.args.clear(); auto cat = host.spawn(spec, 3, &err);
    check("pty: cat", cat != nullptr, err);
    if (cat) {
      cat->write("echo me\r"); check("pty: resize", cat->resize(80, 24) && cat->cols() == 80);
      std::string seen; for (int i = 0; i < 20 && seen.find("echo me") == std::string::npos; i++) { sleepMs(50); std::deque<db::Inbox::Event> q; inbox->take(q); for (auto& e : q) if (e.kind == db::Inbox::PtyData) seen += e.bytes; }
      check("pty: what was written came back (the line discipline's echo)", seen.find("echo me") != std::string::npos, seen);
      check("pty: the process tree names the child", !cat->processTree().empty() && cat->processTree()[0] == cat->pid());
      cat->kill(); bool gone = false; for (int i = 0; i < 40 && !gone; i++) { sleepMs(50); std::deque<db::Inbox::Event> q; inbox->take(q); for (auto& e : q) if (e.kind == db::Inbox::PtyExit && e.id == 3) gone = true; }
      check("pty: kill is followed by the exit", gone && cat->exited());
    } }

  // ---- the listener, alone: accept, bytes in, bytes out, close
  { auto inbox = std::make_shared<db::Inbox>(); mac::MacListener lis(inbox, 8811, 8819); std::string err;
    check("listener: a test listener refuses anything but loopback 8811-8819", !lis.listen("0.0.0.0", 8811, &err) && !lis.listen("127.0.0.1", 9000, &err), err);
    check("listener: listens on 127.0.0.1:8811", lis.listen("127.0.0.1", 8811, &err), err);
    const int c = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(8811); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    check("listener: a client connects", connect(c, (struct sockaddr*)&a, sizeof a) == 0);
    ssize_t w = write(c, "ping", 4); (void)w;
    uint64_t conn = 0; std::string got; for (int i = 0; i < 20 && got.empty(); i++) { sleepMs(50); std::deque<db::Inbox::Event> q; inbox->take(q); for (auto& e : q) { if (e.kind == db::Inbox::NetAccept) conn = e.id; if (e.kind == db::Inbox::NetBytes) got += e.bytes; } }
    check("listener: accept, then the bytes", conn != 0 && got == "ping", got);
    lis.send(conn, "pong"); char buf[8] = {}; const ssize_t n = read(c, buf, 4);
    check("listener: bytes out, nothing left buffered", n == 4 && std::string(buf, 4) == "pong" && lis.buffered(conn) == 0);
    close(c); bool closed = false; for (int i = 0; i < 20 && !closed; i++) { sleepMs(50); std::deque<db::Inbox::Event> q; inbox->take(q); for (auto& e : q) if (e.kind == db::Inbox::NetClosed && e.id == conn) closed = true; }
    check("listener: the peer's close is reported", closed);
    lis.stop(); }

  // ---- the whole deck on the real seams, through the façade
  { DMSOptions mo{}; mo.server = 1; mo.test_port_low = 8811; mo.test_port_high = 8819;
    DBHost* host = dms_host_create(&mo);
    static int wakes = 0; db_host_set_wake(host, [](void*) { wakes++; }, nullptr);
    DBOptions o; db_options_init(&o); const std::string state = dir + "/state"; o.state_dir = state.c_str(); o.serve_port = 8812;
    char err[256]; DBDeck* deck = db_deck_create(&o, host, nullptr, err, sizeof err);
    check("deck: made on the real seams", deck != nullptr, err);
    if (deck) {
      static std::string data; static std::string newId; static bool exited = false;
      DBSink sink{ nullptr, [](void*, const DBFrame* f) {
        if (f->kind == DB_FRAME_DATA) data.append(f->bytes, f->bytes_len);
        if (f->kind == DB_FRAME_OK && f->rid == 2 && f->json_len > 2) newId.assign(f->json + 1, f->json_len - 2);
        if (f->kind == DB_FRAME_EXIT) exited = true; } };
      DBConn* conn = db_deck_add_window(deck, nullptr, &sink);
      db_deck_start(deck);
      db_conn_call(conn, 1, "deskInit", nullptr);
      db_conn_call(conn, 2, "newTile", "[{\"kind\":\"shell\"}]");
      for (int i = 0; i < 20 && newId.empty(); i++) { sleepMs(50); db_deck_pump(deck); }
      check("deck: a shell tile", !newId.empty());
      db_conn_attach(conn, newId.c_str(), 0);
      const std::string cmd = "echo deck-$((40+2))\r";
      for (int i = 0; i < 40; i++) { sleepMs(50); db_deck_pump(deck); if (i == 10) db_conn_input(conn, newId.c_str(), cmd.data(), cmd.size()); if (data.find("deck-42") != std::string::npos) break; }
      check("deck: typed into the user's login shell, and its answer came back", data.find("deck-42") != std::string::npos, data.substr(0, 400));
      check("deck: the pty threads woke the owner", wakes > 0);
      check("deck: one live process", db_deck_live_count(deck) >= 1);
      check("deck: the server is up on the forced port", std::string(db_deck_phone_status_json(deck)).find("\"running\":true") != std::string::npos, db_deck_phone_status_json(deck));
      db_deck_shutdown(deck);
      check("deck: state was written under the temp folder", files.isFile(state + "/settings.json") || files.isFile(state + "/tiles.json"));
      db_deck_destroy(deck);
    }
    db_host_destroy(host); }

  std::printf("%d passed, %d failed   (scratch folder: %s)\n", passed, failed, dir.c_str());
  return failed ? 1 : 0;
}
