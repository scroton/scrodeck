// core/deck/restorerefresh.hpp — lib/restorerefresh.js: refresh every restored session once the restore is done
// (2026-11-20). Header-only, like QuietRestart: the deck hands step() the tiles as they are now and refreshes
// the id it returns (D::refreshTile); settled(id) tells Pause until restart when a tile's refresh is behind it.
// The rules are the JS file's header; the two must stay the same (test/restorerefresh-unit.js, the "restore
// refresh" scenario in core/test/deck/deck_scenarios.cpp).
#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace deck { namespace core {

struct RestoreRefresh {
  struct View { std::string id, kind; bool asleep = false, exited = false, running = false, ready = false; double startedAt = 0; int background = 0; double lastKeyAt = 0; };
  double capMs = 180000, busyMs = 600000, gapMs = 2000, backMs = 90000, typingMs = 60000;
  std::string phase = "off";               // off | restore | refresh | done
  std::vector<std::string> ids, pending;
  std::map<std::string, std::string> result;   // refreshed | skipped | fresh
  std::map<std::string, double> base;
  bool hasCurrent = false; std::string currentId; double currentAt = 0;
  double startAt = 0, refreshAt = 0, nextAt = 0;

  void start(const std::vector<std::string>& list, double now) { ids = list; pending = list; phase = ids.empty() ? "done" : "restore"; startAt = now; }
  void stop() { phase = "done"; pending.clear(); hasCurrent = false; }
  bool settled(const std::string& id) const {
    if (phase == "off" || phase == "done") return true;
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) return true;
    return result.count(id) > 0;
  }
  int refreshed() const { int n = 0; for (const auto& kv : result) if (kv.second == "refreshed") n++; return n; }
  static bool nothingToRefresh(const View* v) { return !v || v->kind != "claude" || v->asleep || v->exited || !v->running; }
  bool eligible(const View& v, double now) const { return v.ready && v.background <= 0 && !(v.lastKeyAt != 0 && now - v.lastKeyAt < typingMs); }
  // the id to refresh now, or "" (restoring: the deck's restore guard holds)
  std::string step(const std::vector<View>& views, double now, bool restoring) {
    if (phase != "restore" && phase != "refresh") return "";
    auto by = [&](const std::string& id) -> const View* { for (const View& v : views) if (v.id == id) return &v; return nullptr; };
    if (phase == "restore") {
      if (!(now - startAt >= capMs)) {
        if (restoring) return "";
        for (const std::string& id : pending) { const View* v = by(id); if (!nothingToRefresh(v) && !v->ready) return ""; }
      }
      phase = "refresh"; refreshAt = now; nextAt = now;
      for (const std::string& id : pending) { const View* v = by(id); base[id] = v ? v->startedAt : 0; }
    }
    if (hasCurrent) {
      const View* v = by(currentId);
      const bool fresh = v && v->startedAt >= currentAt;
      const bool back = !v || v->kind != "claude" || v->asleep || (fresh && ((v->running && v->ready) || v->exited)) || now - currentAt >= backMs;
      if (!back) return "";
      result[currentId] = "refreshed"; hasCurrent = false; nextAt = now + gapMs;
    }
    std::vector<std::string> keep;
    for (const std::string& id : pending) {
      const View* v = by(id);
      if (nothingToRefresh(v)) { result[id] = "skipped"; continue; }
      if (v->startedAt > base[id]) { result[id] = "fresh"; continue; }
      keep.push_back(id);
    }
    pending = keep;
    if (pending.empty()) { phase = "done"; return ""; }
    if (now < nextAt) return "";
    for (size_t i = 0; i < pending.size(); i++) {
      if (!eligible(*by(pending[i]), now)) continue;
      hasCurrent = true; currentId = pending[i]; currentAt = now;
      pending.erase(pending.begin() + (long long)i);
      return currentId;
    }
    if (now - refreshAt >= busyMs) { for (const std::string& id : pending) result[id] = "skipped"; pending.clear(); phase = "done"; }
    return "";
  }
};

}}  // namespace deck::core
