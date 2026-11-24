// core/deck/hosting.cpp — see hosting.hpp.
#include "hosting.hpp"
#include "util.hpp"
#include "../session/jsutil.hpp"

#include <algorithm>
#include <stdexcept>

namespace deck { namespace core {

void Inbox::push(Event&& ev) {
  std::function<void()> wake;
  { std::lock_guard<std::mutex> g(m_);
    // output of one pty (or one socket) arriving in a burst is one event (the owner sees it as one chunk)
    if ((ev.kind == PtyData || ev.kind == NetBytes) && !q_.empty() && q_.back().kind == ev.kind && q_.back().id == ev.id && q_.back().bytes.size() < 256 * 1024) q_.back().bytes += ev.bytes;
    else q_.push_back(std::move(ev));
    if (!posted_) { posted_ = true; wake = wake_; } }
  if (wake) wake();
}

// Every public entry that can make the core speak holds one of these: frames queue while it lives and are
// delivered when the OUTERMOST one goes (never from inside a sink's own nested call).
struct Host::Scope {
  Host& h;
  explicit Scope(Host& host) : h(host) { h.depth_++; }
  ~Scope() { if (--h.depth_ == 0 && !h.flushing_) h.flush(); }
};

Host::Host(const DeckBuild& build, const HostSeams& seams, std::shared_ptr<Inbox> inbox, bool deliverInline)
  : inbox_(inbox ? std::move(inbox) : std::make_shared<Inbox>()), seams_(seams), stateDir_(build.stateDir), inline_(deliverInline) {
  if (!seams.clock || !seams.files || !seams.ptys || !seams.procs || !seams.random || !seams.env)
    throw std::invalid_argument("the host is missing a required seam (clock, files, ptys, procs, random, env)");
  // the separator join() writes (core/deck/util.hpp): '\\' on Windows, '/' everywhere else - said once, here, before
  // the deck is made, so no shell has to remember it (the Mac draft's bridge did it by hand)
  setPathSep(seams.env->os() == "win32" ? '\\' : '/');
  Seams sm;
  sm.clock = seams.clock; sm.files = seams.files; sm.ptys = seams.ptys; sm.procs = seams.procs; sm.random = seams.random; sm.env = seams.env;
  sm.notifier = seams.notifier; sm.lifecycle = seams.lifecycle; sm.log = seams.log;
  if (seams.listener && seams.net) {
    server_.reset(new Server(build.server, *seams.files, *seams.clock, *seams.listener, *seams.net));
    sm.phone = server_.get();
  }
  // Usage / cloud polling read the accounts' sign-in tokens: only when the options asked AND the shell gave both seams
  if (build.wantsHttp && seams.http && seams.credentials) { sm.http = seams.http; sm.credentials = seams.credentials; }
  deck_.reset(new Deck(build.options, sm, [this](ClientId c, const json& frame) { return route(c, frame); }));
  if (server_) server_->attach(deck_.get());
}
Host::~Host() { shutdown(); }

bool Host::route(ClientId c, const json& frame) {
  std::shared_ptr<Win> w;
  auto it = wins_.find(c);
  if (it != wins_.end()) w = it->second;
  else if (pending_ && c == pendingId_) w = pending_;      // EXACTLY the window being added, by its id
  if (!w) return server_ ? server_->deliver(c, frame) : false;
  if (w->closed) return false;
  if (inline_ || depth_ == 0) { if (w->sink) w->sink(frame); }
  else w->out.push_back(frame);
  return true;
}

void Host::flush() {
  flushing_ = true;
  for (bool progress = true; progress;) {
    progress = false;
    std::vector<std::shared_ptr<Win>> all;                 // (a sink may add or close windows)
    all.reserve(wins_.size());
    for (auto& kv : wins_) all.push_back(kv.second);
    for (auto& w : all) {
      while (!w->closed && !w->out.empty()) {
        const json f = std::move(w->out.front()); w->out.pop_front();
        if (w->sink) { try { w->sink(f); } catch (...) {} }
        progress = true;
      }
    }
  }
  flushing_ = false;
}

void Host::drain() {
  std::deque<Inbox::Event> q;
  inbox_->take(q);
  for (Inbox::Event& ev : q) {
    if (down_) return;
    switch (ev.kind) {
      case Inbox::PtyData: deck_->onPtyData(ev.id, ev.bytes.data(), ev.bytes.size()); break;
      case Inbox::PtyExit: deck_->onPtyExit(ev.id, ev.code); break;
      case Inbox::NetAccept: if (server_) server_->onAccept(ev.id); break;
      case Inbox::NetBytes: if (server_) server_->onBytes(ev.id, ev.bytes.data(), ev.bytes.size()); break;
      case Inbox::NetClosed: if (server_) server_->onClosed(ev.id); break;
      case Inbox::Task: if (ev.task) ev.task(); break;
    }
  }
}

void Host::start() { if (down_) return; Scope s(*this); deck_->start(); }

ClientId Host::addWindow(const json& bounds, WindowSink sink) {
  if (down_) return 0;
  Scope s(*this);
  std::shared_ptr<Win> w = std::make_shared<Win>(); w->sink = std::move(sink);
  pending_ = w; pendingId_ = deck_->nextClientId();
  ClientId c = 0;
  try { c = deck_->addWindow(bounds); } catch (...) { pending_.reset(); pendingId_ = 0; throw; }
  pending_.reset(); pendingId_ = 0;
  w->id = c; wins_[c] = w;
  return c;
}
ClientId Host::openWindow(const std::string& tileId, const json& bounds, WindowSink sink) {
  if (down_) return 0;
  Scope s(*this);
  std::shared_ptr<Win> w = std::make_shared<Win>(); w->sink = std::move(sink);
  pending_ = w; pendingId_ = deck_->nextClientId();
  ClientId c = 0;
  try { c = deck_->openWindow(tileId, bounds); } catch (...) { pending_.reset(); pendingId_ = 0; throw; }
  pending_.reset(); pendingId_ = 0;
  if (!c) return 0;
  w->id = c; wins_[c] = w;
  return c;
}
bool Host::adoptWindow(ClientId client, WindowSink sink) {
  if (down_ || client <= 0 || wins_.count(client)) return false;
  std::shared_ptr<Win> w = std::make_shared<Win>(); w->id = client; w->sink = std::move(sink);
  wins_[client] = w;
  return true;
}
void Host::removeWindow(ClientId client) {
  auto it = wins_.find(client);
  if (it == wins_.end()) return;
  it->second->closed = true; it->second->out.clear();
  wins_.erase(it);
  if (down_) return;
  Scope s(*this);
  deck_->removeClient(client);
}

void Host::handle(ClientId window, const json& frame) {
  if (down_ || !wins_.count(window)) return;
  Scope s(*this);
  deck_->handle(window, frame);
}
void Host::pump() {
  if (down_) return;
  Scope s(*this);
  drain(); deck_->tick();
  if (server_) server_->tick();
  drain();
}
int Host::nextDueMs() const {
  if (down_) return 2000;
  int ms = deck_->nextDueMs();
  if (server_ && server_->running()) ms = std::min(ms, 1000);       // the heartbeat's clock
  return ms;
}
void Host::shutdown() {
  if (down_) return;
  down_ = true;
  // Deferred sinks call straight into windows that are being torn down: nothing reaches them once this began.
  // Inline sinks (they post) still get what the shutdown itself raises, as they always did.
  if (!inline_) for (auto& kv : wins_) { kv.second->closed = true; kv.second->out.clear(); }
  if (server_) server_->stop();
  if (seams_.listener) seams_.listener->stop();          // (a real one joins its socket thread: nothing is listening after this)
  deck_->shutdown();
  for (auto& kv : wins_) { kv.second->closed = true; kv.second->out.clear(); }
  wins_.clear();
}
void Host::enter(const std::function<void()>& fn) { if (down_ || !fn) return; Scope s(*this); fn(); }
json Host::phoneStatus() { return server_ ? server_->status(session::js::get(deck_->settings(), "phone")) : json(); }

}}  // namespace deck::core
