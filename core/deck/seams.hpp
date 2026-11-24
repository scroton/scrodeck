// core/deck/seams.hpp — everything the deck (core/deck/deck.hpp) and its server (core/deck/server.hpp) need from
// an operating system, as small abstract interfaces. The core is pure C++17: it never opens a file, starts a
// process, reads a clock, sleeps or makes a thread. A shell implements these once (windows/host/ for Windows;
// the Swift shell supplies its own) and PUMPS the core: frames in, pty bytes in, tick(); frames out through a sink.
//
//   Clock        Date.now()
//   Files        read / write / atomic replace / list / stat — state, transcripts, session records, served assets
//   PtyHost/Pty  a process in a pseudo-terminal (node-pty): spawn, write, resize, kill; output and the exit come
//                back through Deck::onPtyData / onPtyExit, called by the shell ON THE DECK'S THREAD
//   ProcessList  is a pid running; pid -> parent pid (the shell-host scan); a pid's console host
//   Random       crypto.randomUUID / randomBytes
//   Env          the parent environment, home, the platform's name
//   Notifier     a toast, a taskbar flash
//   Http         one GET (the usage endpoint). Written against fakes only; no shell implements it yet
//   Credentials  an account's OAuth token for the usage poll. Fakes only; NO shell implements it (the owner's word)
//   Listener     a listening TCP socket and its connections (the server's bytes in and out)
// Every callback into the core happens on the one thread that owns it. Paths are UTF-8.
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace deck { namespace core {

using json = nlohmann::json;
using EnvList = std::vector<std::pair<std::string, std::string>>;   // NAME, value — in order, names exact-case

class Clock {
 public:
  virtual ~Clock() {}
  virtual double nowMs() = 0;                       // ms since the epoch
  // minutes west of UTC subtracted: local time = utc + offset (ms). Only the special-prompt windows read it.
  virtual double localOffsetMs(double /*utcMs*/) { return 0; }
};

struct FileStat { bool exists = false, isDir = false; uint64_t size = 0; double mtimeMs = 0; };
class Files {
 public:
  virtual ~Files() {}
  virtual bool read(const std::string& path, std::string& out) = 0;                    // the whole file
  virtual bool readFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) = 0;   // [offset, end)
  // [offset, offset + length): a shell overrides this to read only that much
  virtual bool readRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out) {
    if (!readFrom(path, offset, out, nullptr)) return false;
    if (out.size() > length) out.resize((size_t)length);
    return true;
  }
  virtual FileStat stat(const std::string& path) = 0;
  virtual std::vector<std::string> list(const std::string& dir) = 0;                  // names, sorted; empty when it cannot be read
  virtual bool makeDirs(const std::string& path) = 0;
  virtual bool remove(const std::string& path) = 0;
  virtual bool copy(const std::string& from, const std::string& to) = 0;
  // through a temp file: flushed, then renamed over `path` (lib/statefile.js writeAtomic)
  virtual bool writeAtomic(const std::string& path, const std::string& text) = 0;
  virtual bool write(const std::string& path, const std::string& bytes) = 0;          // fs.writeFileSync
  // fs.renameSync: `from` becomes `to`, replacing a file already there; false when `from` is gone (the desk-open
  // inbox's "whoever renames first owns it"). The default is NOT atomic: a shell overrides it with a real rename.
  virtual bool rename(const std::string& from, const std::string& to) { if (!isFile(from) || !copy(from, to)) return false; return remove(from); }
  bool isFile(const std::string& p) { const FileStat s = stat(p); return s.exists && !s.isDir; }
  bool isDir(const std::string& p) { const FileStat s = stat(p); return s.exists && s.isDir; }
};

struct PtySpawn {
  std::string file;                                 // looked up on the Path when it is a bare name
  std::vector<std::string> args;
  EnvList env;                                      // the child's WHOLE environment
  std::string cwd;
  int cols = 120, rows = 30;                        // main.js spawns every tile at 120 x 30
};
class Pty {
 public:
  virtual ~Pty() {}
  virtual uint32_t pid() const = 0;
  virtual int cols() const = 0;                     // the size last given to it
  virtual int rows() const = 0;
  virtual bool write(const std::string& bytes) = 0; // false once it is gone
  virtual bool resize(int cols, int rows) = 0;
  virtual void kill() = 0;                          // p.kill() + platform.killTree(pid): returns at once, the exit follows
  virtual bool exited() const = 0;
  virtual std::vector<uint32_t> processTree() const { return {}; }   // diagnostics: the pids still alive under it
};
class PtyHost {
 public:
  virtual ~PtyHost() {}
  // Start a process. `token` names it in Deck::onPtyData(token, …) / onPtyExit(token, code); the exit is
  // delivered after its last byte. Null on failure, with `error` set (node-pty's wording where it has one).
  virtual std::unique_ptr<Pty> spawn(const PtySpawn& spec, uint64_t token, std::string* error) = 0;
};

struct ProcInfo { uint32_t pid = 0, ppid = 0; std::string name, path; };   // a process: its file's name and full path ("" = not told)
class ProcessList {
 public:
  virtual ~ProcessList() {}
  virtual bool alive(double pid) = 0;               // lib/sessions.js pidAlive
  // platform.processParents: pid -> parent pid for every process; false when the snapshot failed
  virtual bool parents(std::map<uint32_t, uint32_t>& /*out*/) { return false; }
  // platform.consoleHosts: pid -> its console host's pid, for the pids asked; false when it cannot be asked
  virtual bool consoleHosts(const std::vector<uint32_t>& /*pids*/, std::map<uint32_t, int64_t>& /*out*/) { return false; }
  // platform.killTree(pid) for a process the deck did not start (adoptSession)
  virtual void killTree(uint32_t /*pid*/) {}
  // the commit charge: used / limit bytes (memory guard); false when it cannot be read
  virtual bool commit(double& /*used*/, double& /*limit*/) { return false; }
  // every process with its parent, name and file (one deck at a time: the other deck's main process, the marker's pid
  // checked against its exe); false when the snapshot failed
  virtual bool snapshot(std::vector<ProcInfo>& /*out*/) { return false; }
};

class Random {
 public:
  virtual ~Random() {}
  virtual std::string uuid() = 0;                   // crypto.randomUUID()
  virtual std::string hex(int bytes) = 0;           // crypto.randomBytes(n).toString('hex')
};

class Env {
 public:
  virtual ~Env() {}
  virtual EnvList environment() = 0;                // { ...process.env }
  virtual std::string get(const std::string& name) = 0;   // "" when unset
  virtual std::string os() = 0;                     // process.platform: "win32" | "darwin" | "linux"
  virtual std::string home() = 0;                   // os.homedir()
  virtual uint32_t pid() { return 0; }              // process.pid (running.json)
  // path.resolve(cwd) as node-pty applies it to a spawn's folder
  virtual std::string resolvePath(const std::string& p) { return p; }
};

// The deck's own log: what the Electron deck writes with console.log ([follow], [pause], [restore-refresh], ...), one
// finished, timestamped line per event. The shell appends it to <state dir>/deck.log (windows/host/winseams: 1 MB,
// then deck.log.1). Optional: none = no log. Called on the deck's thread.
class Log {
 public:
  virtual ~Log() {}
  virtual void line(const std::string& text) = 0;
};

struct Toast { std::string title, body; bool silent = false; std::string kind; std::string tileId; std::string sessionId; };
class Notifier {
 public:
  virtual ~Notifier() {}
  virtual void toast(const Toast&) {}
  virtual void flash(int /*windowIndex*/) {}        // win.flashFrame(true) / dock.bounce
};

struct HttpResponse { int status = 0; std::map<std::string, std::string> headers; std::string body; std::string error; };
class Http {
 public:
  virtual ~Http() {}
  // One GET; `done` is called later, on the deck's thread (from the shell's pump).
  virtual void get(const std::string& url, const std::map<std::string, std::string>& headers, std::function<void(const HttpResponse&)> done) = 0;
};
class Credentials {
 public:
  virtual ~Credentials() {}
  // platform.readOAuth(dir): the parsed { claudeAiOauth: {…} } of one config dir, or null
  virtual json readOAuth(const std::string& configDir) = 0;
  // platform.credentialsStamp(dir): changes when the token does; "" when there is none
  virtual std::string stamp(const std::string& configDir) = 0;
};

// The server's socket side. Connections are numbered by the listener; bytes arrive through
// Server::onAccept / onBytes / onClosed, called by the shell on the deck's thread.
class Listener {
 public:
  virtual ~Listener() {}
  // Bind `host` (an IPv4 address: "127.0.0.1", a Tailscale address, "0.0.0.0") and accept. False with `error`
  // set when it cannot (the port is taken, the address is not this machine's).
  virtual bool listen(const std::string& host, int port, std::string* error) = 0;
  virtual void stop() = 0;                          // every connection closed, the port free
  virtual void send(uint64_t conn, const std::string& bytes) = 0;
  virtual size_t buffered(uint64_t conn) = 0;       // bytes accepted by send() and not yet on the wire (ws.bufferedAmount)
  virtual void close(uint64_t conn, bool afterFlush) = 0;   // afterFlush: what was sent goes out first
};
// What the server asks of the network around it (lib/phone.js tailscaleAddress / tailscaleServeUrl).
class Net {
 public:
  virtual ~Net() {}
  // os.networkInterfaces(): every IPv4 address that is not internal (loopback)
  virtual std::vector<std::string> ipv4Addresses() = 0;
  // the output of `tailscale serve status --json` (run hidden), "" when it cannot be had
  virtual std::string tailscaleServeStatus() = 0;
};

}}  // namespace deck::core
