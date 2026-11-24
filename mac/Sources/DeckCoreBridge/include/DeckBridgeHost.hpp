// mac/Sources/DeckCoreBridge/include/DeckBridgeHost.hpp — the C++ side of a DBHost: what a platform's seam
// library (mac/Sources/DeckMacSeams) or a test (mac/Tests/bridge_test.cpp) fills in so the bridge can make a deck.
// NOT part of the Swift module (include/module.modulemap lists DeckBridge.h only): Swift sees a DBHost as an
// opaque pointer it got from the seam library and hands to db_deck_create.
//
// Windows counterpart: the `Shared` queue + the seam members of windows/host/localdeck.cpp / windows/src/deckhost.hpp.
// Pure C++17 (std::mutex only). Built and tested on Windows with g++ (mac/Tests/build.ps1).
#pragma once
#include "core/deck/seams.hpp"
#include "DeckBridge.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace db {

// THE ONE THREAD-SAFE THING. Pty readers, the socket thread and finished HTTP requests append here and never
// touch the deck; the owner drains it in db_deck_pump. A burst from one pty (or one socket) is merged into one
// event, as windows/host/localdeck.cpp does. The wake callback is called from the pushing thread, at most once
// per drain (the shell hops to its own thread there: DispatchQueue.main.async / PostMessage).
class Inbox {
 public:
  enum Kind { PtyData, PtyExit, NetAccept, NetBytes, NetClosed, Task };
  struct Event { Kind kind; uint64_t id; uint32_t code; std::string bytes; std::function<void()> task; };

  void ptyData(uint64_t token, const char* bytes, size_t n) { push(Event{ PtyData, token, 0, std::string(bytes, n), nullptr }); }
  void ptyExit(uint64_t token, uint32_t code) { push(Event{ PtyExit, token, code, std::string(), nullptr }); }
  void netAccept(uint64_t conn) { push(Event{ NetAccept, conn, 0, std::string(), nullptr }); }
  void netBytes(uint64_t conn, const char* bytes, size_t n) { push(Event{ NetBytes, conn, 0, std::string(bytes, n), nullptr }); }
  void netClosed(uint64_t conn) { push(Event{ NetClosed, conn, 0, std::string(), nullptr }); }
  // run `task` on the owner's thread at the next pump (an Http seam's `done`)
  void task(std::function<void()> task) { push(Event{ Task, 0, 0, std::string(), std::move(task) }); }

  void setWake(void (*wake)(void*), void* ctx) { std::lock_guard<std::mutex> g(m_); wake_ = wake; wakeCtx_ = ctx; }
  void take(std::deque<Event>& out) { std::lock_guard<std::mutex> g(m_); out.swap(q_); posted_ = false; }
  bool empty() { std::lock_guard<std::mutex> g(m_); return q_.empty(); }

 private:
  void push(Event&& ev) {
    void (*wake)(void*) = nullptr; void* ctx = nullptr;
    { std::lock_guard<std::mutex> g(m_);
      if ((ev.kind == PtyData || ev.kind == NetBytes) && !q_.empty() && q_.back().kind == ev.kind && q_.back().id == ev.id && q_.back().bytes.size() < 256 * 1024) q_.back().bytes += ev.bytes;
      else q_.push_back(std::move(ev));
      if (!posted_) { posted_ = true; wake = wake_; ctx = wakeCtx_; } }
    if (wake) wake(ctx);
  }
  std::mutex m_;
  std::deque<Event> q_;
  bool posted_ = false;
  void (*wake_)(void*) = nullptr; void* wakeCtx_ = nullptr;
};

}  // namespace db

// The OS side of one deck. The seam pointers are owned by whoever made the host (a subclass holds them) and must
// stay valid until the host is destroyed, which must be AFTER the deck's.
//   required  clock, files, ptys, procs, random, env
//   optional  listener + net (both, or there is no phone / desk server), http + credentials (both, or no polling)
struct DBHost {
  deck::core::Clock* clock = nullptr; deck::core::Files* files = nullptr; deck::core::PtyHost* ptys = nullptr;
  deck::core::ProcessList* procs = nullptr; deck::core::Random* random = nullptr; deck::core::Env* env = nullptr;
  deck::core::Listener* listener = nullptr; deck::core::Net* net = nullptr;
  deck::core::Http* http = nullptr; deck::core::Credentials* credentials = nullptr;
  std::shared_ptr<db::Inbox> inbox = std::make_shared<db::Inbox>();
  virtual ~DBHost() {}
  // db_deck_shutdown: stop whatever threads still push (the listener is stopped by the bridge before this)
  virtual void quiesce() {}
};

// db_host_set_wake / db_host_destroy (DeckBridge.h) are what Swift calls on one.
