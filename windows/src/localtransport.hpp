// windows/src/localtransport.hpp — local mode's transport: the same frames App sends over the socket, handed
// to an in-process deck (windows/host/localdeck.hpp) instead. core/PROTOCOL.md "Local vs remote": the local
// shell gets a direct dispatch of the identical message types.
//
// It has WsClient's shape (start / stop / send / clearQueue) and answers the same way: every frame the deck
// emits reaches the window as a POSTED WM_WS message (wParam = WsEvent, lParam = a heap std::string the
// receiver deletes). Posted, never delivered inline, so a reply cannot re-enter App from inside its own send().
// One thread: everything here runs on the window thread (the deck's rule, HOST-NOTES.md "Threads").
#pragma once
#include "ws.hpp"                              // WsEvent
#include "../host/localdeck.hpp"
#include "../../core/proto/frames.hpp"
#include <memory>
#include <shellapi.h>                           // CommandLineToArgvW

namespace deck {

class LocalTransport {
 public:
  ~LocalTransport() { stop(); }
  // wsMsg: App's WM_WS.  pumpMsg: a second message of App's own that means "call pump()".
  void start(HWND notify, UINT wsMsg, UINT pumpMsg, host::LocalDeckOptions o = host::LocalDeckOptions()) {
    stop();
    hwnd_ = notify; msg_ = wsMsg;
    o.notifyHwnd = notify; o.notifyMsg = pumpMsg;
    // `--serve <port>` and `--assets <folder>`: the local deck also serves a phone / a remote desk on
    // 127.0.0.1:<port> for this run (windows/HOST-NOTES.md "The server"). Read here, from the process's own
    // command line, so the switch works without a change to app.cpp's option parser; the clean form (an Options
    // field set where --local is read) is written out in HOST-NOTES.md.
    if (!o.servePort) {
      int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
      for (int i = 1; argv && i + 1 < argc; i++) {
        const std::wstring a = argv[i];
        if (a == L"--serve") { const int p = _wtoi(argv[i + 1]); if (p > 0 && p < 65536) o.servePort = p; }
        else if (a == L"--assets" && o.assetRoot.empty()) o.assetRoot = host::narrow(argv[i + 1]);
      }
      if (argv) LocalFree(argv);
    }
    deck_.reset(new host::LocalDeck(o, [this](const nlohmann::json& f) { post(WsMessage, proto::encode(f)); }));
    post(WsOpen, "");                          // App::onWs(WsOpen) calls deskInit, exactly as on a socket
    deck_->start();                            // its `init` hello follows; a desk ignores it
  }
  // What window-all-closed does in main.js: every pty killed, their threads joined. Nothing of ours runs after.
  void stop() { if (deck_) { deck_->shutdown(); deck_.reset(); } }
  void send(const std::string& text) {
    if (!deck_) return;
    nlohmann::json m = nlohmann::json::parse(text, nullptr, false);
    if (m.is_object()) deck_->handle(m);
  }
  void clearQueue() {}
  void pump() { if (deck_) deck_->pump(); }
  int nextDueMs() const { return deck_ ? deck_->nextDueMs() : 2000; }
  bool running() const { return !!deck_; }
  // tests: every live process the deck started
  size_t liveCount() const { return deck_ ? deck_->livePids().size() : 0; }

 private:
  void post(WsEvent ev, const std::string& s) {
    std::string* p = new std::string(s);
    if (!PostMessageW(hwnd_, msg_, ev, (LPARAM)p)) delete p;
  }
  HWND hwnd_ = nullptr; UINT msg_ = 0;
  std::unique_ptr<host::LocalDeck> deck_;
};

}  // namespace deck
