// core/deck/hosting.hpp — the deck as a SHELL holds it, once, for every shell: the inbox other threads post to,
// the pump that hands their events to the core on the owner's thread, the server beside the deck, and the shell's
// windows as clients of the one deck, each with its own frames.
//
// This is what windows/src/deckhost.hpp + windows/host/localdeck.cpp and the Mac draft's DeckBridgeHost.hpp / DBDeck
// each wrote for themselves; both now sit on this. What stays a shell's: the seams (ConPTY / forkpty, sockets,
// files), its threads, and how it gets back to its own thread — the Inbox's WAKE callback (PostMessage,
// DispatchQueue.main.async).
//
// NO THREAD, NO OS CALL in here. The Inbox is the one thread-safe thing (a std::mutex and a deque): pty readers,
// the socket thread and finished HTTP requests append to it and never touch the deck. Everything else — Host, the
// deck, the server — belongs to ONE thread, the owner's.
//
// ---------------------------------------------------------------- frames out
// Every frame the core raises goes to exactly the client it names:
//   - one of this host's windows  -> that window's sink;
//   - the window being added      -> its sink too (the hello is raised from inside addWindow, before the shell has
//                                    the id): matched BY ID (Deck::nextClientId), never "whatever is unknown";
//   - anyone else                 -> the server (a phone, a remote desk), or nowhere.
// By default a window's frames are QUEUED while a Host call is running and delivered when the OUTERMOST call
// returns, so a reply never re-enters a window from inside its own send (the Mac's sinks call straight into the
// window). A shell whose sinks only post (Windows: PostMessage) asks for `deliverInline` and gets each frame as
// it is raised. A frame raised by a direct call on deck() (outside any Host call) is delivered at once.
//
// ---------------------------------------------------------------- windows and their saved places (R7)
// See core/deck/README.md "Hosting": which saved entry and index a window takes — before start(), after it, and
// when a new window opens after the last one closed.
#pragma once
#include "deck.hpp"
#include "options.hpp"
#include "seams.hpp"
#include "server.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace deck { namespace core {

// THE ONE THREAD-SAFE THING. A burst from one pty (or one socket) is merged into one event (up to 256 KB). The
// wake callback is called from the PUSHING thread, at most once per take() (the shell hops to its own thread
// there and calls Host::pump); it must be safe to call from any thread.
class Inbox {
 public:
  enum Kind { PtyData, PtyExit, NetAccept, NetBytes, NetClosed, Task };
  struct Event { Kind kind; uint64_t id; uint32_t code; std::string bytes; std::function<void()> task; };

  void ptyData(uint64_t token, const char* bytes, size_t n) { push(Event{ PtyData, token, 0, bytes ? std::string(bytes, n) : std::string(), nullptr }); }
  void ptyExit(uint64_t token, uint32_t code) { push(Event{ PtyExit, token, code, std::string(), nullptr }); }
  void netAccept(uint64_t conn) { push(Event{ NetAccept, conn, 0, std::string(), nullptr }); }
  void netBytes(uint64_t conn, const char* bytes, size_t n) { push(Event{ NetBytes, conn, 0, bytes ? std::string(bytes, n) : std::string(), nullptr }); }
  void netClosed(uint64_t conn) { push(Event{ NetClosed, conn, 0, std::string(), nullptr }); }
  // run `task` on the owner's thread at the next pump (an Http seam's `done`)
  void task(std::function<void()> task) { push(Event{ Task, 0, 0, std::string(), std::move(task) }); }

  void setWake(std::function<void()> wake) { std::lock_guard<std::mutex> g(m_); wake_ = std::move(wake); }
  void take(std::deque<Event>& out) { std::lock_guard<std::mutex> g(m_); out.clear(); out.swap(q_); posted_ = false; }
  bool empty() { std::lock_guard<std::mutex> g(m_); return q_.empty(); }

 private:
  void push(Event&& ev);
  std::mutex m_;
  std::deque<Event> q_;
  bool posted_ = false;
  std::function<void()> wake_;
};

// The OS side of one deck, as pointers the shell owns (they must outlive the Host).
//   required  clock, files, ptys, procs, random, env
//   optional  listener + net (both, or there is no phone / desk server); http + credentials (both, and only used
//             when the options asked for polling); notifier; lifecycle; log
struct HostSeams {
  Clock* clock = nullptr; Files* files = nullptr; PtyHost* ptys = nullptr; ProcessList* procs = nullptr; Random* random = nullptr; Env* env = nullptr;
  Listener* listener = nullptr; Net* net = nullptr;
  Http* http = nullptr; Credentials* credentials = nullptr;
  Notifier* notifier = nullptr; Lifecycle* lifecycle = nullptr;
  Log* log = nullptr;                       // optional: <state dir>/deck.log (seams.hpp Log)
};

class Host {
 public:
  using WindowSink = std::function<void(const json& frame)>;

  // Throws std::invalid_argument when a required seam is missing. `deliverInline`: see "frames out" above.
  Host(const DeckBuild& build, const HostSeams& seams, std::shared_ptr<Inbox> inbox, bool deliverInline = false);
  ~Host();                                  // shutdown()
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Load settings + state, restore the saved tiles. Windows added BEFORE this take their saved places now.
  void start();

  // ---- windows of the shell, as clients of the deck
  // A window. Its hello (`init`) reaches `sink` (a window ignores it and calls deskInit). The client id.
  ClientId addWindow(const json& bounds, WindowSink sink);
  // F5 `window:open`: a new window holding `tileId` alone (it leaves wherever it was). 0 = no such tile.
  ClientId openWindow(const std::string& tileId, const json& bounds, WindowSink sink);
  // A window for a client the CORE registered itself (Lifecycle::openWindow: the on-call's desk), or one the
  // shell made by calling deck().openWindow directly. False = the id is 0 or already has a window here.
  bool adoptWindow(ClientId client, WindowSink sink);
  // A window closed: its tiles drop out of that layout and keep running. Nothing more is delivered to its sink.
  void removeWindow(ClientId client);
  bool hasWindow(ClientId client) const { return wins_.count(client) > 0; }
  size_t windowCount() const { return wins_.size(); }

  // ---- the pump
  // One frame from a window (core/PROTOCOL.md "Shell -> core"). Ignored when `window` is not one of this host's.
  void handle(ClientId window, const json& frame);
  // Drain the inbox into the core (pty bytes and exits, sockets, finished requests), run the timers that are due.
  void pump();
  // Milliseconds until a timer is due (0 = now); never more than 2000, and 1000 while the server runs (its heartbeat).
  int nextDueMs() const;
  // window-all-closed: the server stopped, the listener stopped, every pty killed. The shell then joins whatever
  // threads of its own still push. Nothing is delivered afterwards (deferred sinks: nothing once it began; inline
  // sinks still get what the shutdown itself raises).
  void shutdown();
  bool running() const { return !down_; }
  // Run `fn` as one Host call: frames it causes are queued and delivered when it returns (for a shell that calls
  // deck() directly and wants the same no-re-entry promise).
  void enter(const std::function<void()>& fn);

  Deck& deck() { return *deck_; }
  Server* server() { return server_.get(); }             // null without listener + net
  Inbox& inbox() { return *inbox_; }
  const std::string& stateDir() const { return stateDir_; }
  // main.js phoneStatus() (+ `assets`, `assetRoot`); null without a server
  json phoneStatus();

 private:
  struct Win { ClientId id = 0; WindowSink sink; std::deque<json> out; bool closed = false; };
  struct Scope;
  bool route(ClientId c, const json& frame);
  void flush();
  void drain();

  std::shared_ptr<Inbox> inbox_;
  HostSeams seams_;
  std::string stateDir_;
  std::unique_ptr<Server> server_;                        // declared before the deck: the deck is destroyed first
  std::unique_ptr<Deck> deck_;
  std::map<ClientId, std::shared_ptr<Win>> wins_;
  std::shared_ptr<Win> pending_;                          // a window being added…
  ClientId pendingId_ = 0;                                // …and the id the deck is about to give it
  int depth_ = 0;
  bool flushing_ = false, down_ = false, inline_ = false;
};

// (tests) A listener that binds nothing but loopback on the test ports: whatever a test saves into Settings ->
// Phone, no test ever listens on the tailnet, the LAN, or the live deck's port.
class TestListener : public Listener {
 public:
  explicit TestListener(Listener& real) : real_(real) {}
  bool listen(const std::string& host, int port, std::string* error) override {
    if (host != "127.0.0.1" || port < 8811 || port > 8819) { if (error) *error = "a test binds 127.0.0.1:8811-8819 only (asked: " + host + ":" + std::to_string(port) + ")"; refused = host + ":" + std::to_string(port); return false; }
    return real_.listen(host, port, error);
  }
  void stop() override { real_.stop(); }
  void send(uint64_t c, const std::string& b) override { real_.send(c, b); }
  size_t buffered(uint64_t c) override { return real_.buffered(c); }
  void close(uint64_t c, bool af) override { real_.close(c, af); }
  std::string refused;
 private:
  Listener& real_;
};
// (tests) No network is looked at: no address but loopback exists, and `tailscale` is never run.
class TestNet : public Net {
 public:
  std::vector<std::string> ipv4Addresses() override { return {}; }
  std::string tailscaleServeStatus() override { return ""; }
};

}}  // namespace deck::core
