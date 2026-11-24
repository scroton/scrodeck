// windows/src/deckhost.hpp — local mode's deck, as the Windows SHELL holds it: core/deck/hosting.hpp (the inbox, the
// pump, windows as clients, exact routing — shared with the Mac shell) over the Windows seams of windows/host, with
// the two Windows things on top: the wake is a PostMessage to the hub window, and every frame for a window is
// POSTED to it (WM_WS with a heap std::string the receiver deletes) — never delivered inline, so a reply cannot
// re-enter a window from inside its own send().
//
// What is NOT here any more (it was, until the Mac draft wrote the same thing a second time): the queue the pty
// and socket threads post to, the drain, the per-window routing, the options builder. Those are core/deck/hosting
// and core/deck/options; this file is the seams' ownership and the HWNDs.
//
// THREADS, as HOST-NOTES.md "Threads": one thread owns it (the window thread). The pty and socket threads only
// append to the core's Inbox, which posts ONE wake message to the hub window; pump() runs there and on a timer.
#pragma once
#include "ws.hpp"                               // WsEvent
#include "../host/localdeck.hpp"                // LocalDeckOptions: the option set (and its defaults: allowClaude, usagePolling)
#include "../host/listener.hpp"
#include "../host/statefile.hpp"
#include "../host/winhttp.hpp"
#include "../host/winseams.hpp"
#include "../../core/deck/deck.hpp"
#include "../../core/deck/hosting.hpp"
#include "../../core/deck/platform.hpp"
#include "../../core/deck/server.hpp"
#include "../../core/proto/frames.hpp"
#include "../../core/session/jsutil.hpp"
#include <memory>

namespace deck {

using core::TestListener;                       // (tests) loopback on 8811-8819 only: core/deck/hosting.hpp
using core::TestNet;                            // (tests) no network is looked at

class DeckHost {
 public:
  struct Extra {
    std::string version;                          // DeckOptions::version (deck-state.json `deck.version`)
    core::Notifier* notifier = nullptr;
    core::Lifecycle* lifecycle = nullptr;
    bool test = false;                            // a test run: TestListener + TestNet
  };
  ~DeckHost() { stop(); host_.reset(); }
  bool running() const { return host_ && host_->running(); }

  // hub: the window that gets `pumpMsg` when pty / socket work is waiting (at most one outstanding).
  void create(host::LocalDeckOptions o, const Extra& x, HWND hub, UINT pumpMsg) {
    stop();
    host_.reset();                                // (before the seams it points at are replaced)
    inbox_ = std::make_shared<core::Inbox>();
    inbox_->setWake([hub, pumpMsg] { if (hub) PostMessageW(hub, pumpMsg, 0, 0); });
    env_.reset(new host::WinEnv(o.home));
    std::shared_ptr<core::Inbox> in = inbox_;
    ptys_.reset(new host::WinPtyHost([in](uint64_t token, bool exit, uint32_t code, const char* b, size_t n) {
      if (exit) in->ptyExit(token, code); else in->ptyData(token, b, n);
    }));

    // ONE entry point (core/deck/options.hpp, through windows/host/localdeck.hpp): a LocalDeckOptions becomes the
    // core's DeckOptions + ServerOptions in one place, so whatever the core grows reaches the app without this
    // file knowing its name. HOST-NOTES.md "one entry point".
    o.version = x.version;
    // the account labels (vv / ms / ts): a deck with none of its own shows the Electron deck's, read-only from its
    // settings.json `aliases` (office check 2026-11-20); never in a test
    if (!x.test) {
      std::string text; const std::string appdata = env_->get("APPDATA");
      if (!appdata.empty() && host::winFiles().read(appdata + "\\claude-deck\\settings.json", text)) {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object() && j.contains("aliases") && j["aliases"].is_object()) o.labelFallback = j["aliases"];
      }
    }
    opt_ = o;
    const core::DeckBuild built = host::buildDeckOptions(o, *env_);
    askDownloads_ = built.options.askDownloads;          // where an older ask page's own Done saves (main.js hookAskDownloads)

    core::HostSeams sm;
    sm.clock = &host::winClock(); sm.files = &host::winFiles(); sm.ptys = ptys_.get(); sm.procs = &procs_; sm.random = &random_; sm.env = env_.get();
    sm.notifier = x.notifier; sm.lifecycle = x.lifecycle;
    log_.reset(built.stateDir.empty() ? nullptr : new host::WinFileLog(built.stateDir + "\\deck.log"));   // the deck's event log
    sm.log = log_.get();

    listener_.reset(new host::WinListener([in](uint64_t conn, int what, const char* b, size_t n) {
      if (what == 0) in->netAccept(conn); else if (what == 1) in->netBytes(conn, b, n); else in->netClosed(conn);
    }));
    testListener_.reset(); testNet_.reset(); net_.reset(); http_.reset();
    if (x.test) { testListener_.reset(new TestListener(*listener_)); sm.listener = testListener_.get(); testNet_.reset(new TestNet()); sm.net = testNet_.get(); }
    else { sm.listener = listener_.get(); net_.reset(new host::WinNet()); sm.net = net_.get(); }
    if (built.wantsHttp) {                               // usage and / or cloud polling
      http_.reset(new host::WinHttpGet([in](std::function<void()> task) { in->task(std::move(task)); }));
      sm.http = http_.get(); sm.credentials = &credentials_;
    }
    // deliverInline: these sinks only post, so each frame is posted as the core raises it
    host_.reset(new core::Host(built, sm, inbox_, true));
  }
  // Load settings + state, restore the saved tiles (the 400 ms spawn queue). Windows added BEFORE this take their
  // saved place and layout (R7); the hello goes to them.
  void start() { if (host_) host_->start(); }

  // A window of the app becomes a client. It is posted WsOpen first (the window then calls deskInit, exactly as on
  // a socket), then whatever the deck says to it.
  core::ClientId addWindow(HWND h, UINT wsMsg, const nlohmann::json& bounds) {
    if (!host_) return 0;
    const Win w{ h, wsMsg }; post(w, WsOpen, "");
    return host_->addWindow(bounds, sinkOf(w));
  }
  // F5 `window:open`: a new window holding `tileId` alone (it leaves wherever it was). 0 = no such tile.
  core::ClientId openWindow(HWND h, UINT wsMsg, const std::string& tileId, const nlohmann::json& bounds) {
    if (!host_) return 0;
    const Win w{ h, wsMsg };
    const core::ClientId c = host_->openWindow(tileId, bounds, sinkOf(w));
    if (!c) return 0;
    post(w, WsOpen, ""); return c;                    // (the hello before it is ignored by a window; deskInit answers the layout)
  }
  // A window for a client the CORE registered itself (Lifecycle::openWindow: the on-call's desk): only the HWND is new.
  void adoptWindow(core::ClientId c, HWND h, UINT wsMsg) { if (!host_) return; const Win w{ h, wsMsg }; host_->adoptWindow(c, sinkOf(w)); post(w, WsOpen, ""); }
  // A window closed: its tiles drop out of that layout and keep running (main.js `closed`).
  void removeWindow(core::ClientId c) { if (host_) host_->removeWindow(c); }
  size_t windowCount() const { return host_ ? host_->windowCount() : 0; }
  void send(core::ClientId c, const std::string& text) {
    if (!running()) return;
    nlohmann::json m = nlohmann::json::parse(text, nullptr, false);
    if (m.is_object()) host_->handle(c, m);
  }
  void pump() { if (running()) host_->pump(); }
  int nextDueMs() const { return host_ ? host_->nextDueMs() : 2000; }
  // window-all-closed: every pty killed, the listener stopped, the threads joined. Nothing of ours runs after.
  void stop() {
    if (!running()) return;
    host_->shutdown();
    http_.reset();
  }
  core::Deck& core() { return host_->deck(); }
  const std::vector<std::string>& askDownloads() const { return askDownloads_; }
  const host::LocalDeckOptions& options() const { return opt_; }
  const std::string& stateDir() const { static const std::string none; return host_ ? host_->stateDir() : none; }
  nlohmann::json phoneStatus() { return host_ ? host_->phoneStatus() : nlohmann::json(); }
  std::string testBindRefused() const { return testListener_ ? testListener_->refused : std::string(); }
  size_t liveCount() const { return host_ ? host_->deck().livePids().size() : 0; }

 private:
  struct Win { HWND hwnd = nullptr; UINT msg = 0; };
  static void post(const Win& w, WsEvent ev, const std::string& s) {
    std::string* p = new std::string(s);
    if (!w.hwnd || !PostMessageW(w.hwnd, w.msg, ev, (LPARAM)p)) delete p;
  }
  static core::Host::WindowSink sinkOf(const Win& w) { return [w](const nlohmann::json& frame) { post(w, WsMessage, proto::encode(frame)); }; }

  host::LocalDeckOptions opt_;
  std::vector<std::string> askDownloads_;
  std::shared_ptr<core::Inbox> inbox_;
  std::unique_ptr<host::WinEnv> env_;
  host::WinRandom random_;
  host::WinProcessList procs_;
  std::unique_ptr<host::WinPtyHost> ptys_;
  std::unique_ptr<host::WinFileLog> log_;          // <state dir>\deck.log (winseams.hpp)
  std::unique_ptr<host::WinListener> listener_;
  std::unique_ptr<TestListener> testListener_;
  std::unique_ptr<host::WinNet> net_;
  std::unique_ptr<TestNet> testNet_;
  std::unique_ptr<host::WinHttpGet> http_;
  host::WinCredentials credentials_;
  std::unique_ptr<core::Host> host_;                   // last: destroyed first (the deck, then its server)
};

}  // namespace deck
