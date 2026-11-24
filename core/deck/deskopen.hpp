// core/deck/deskopen.hpp — lib/deskopen.js, ported: A DESK THE ON-CALL OPENS AS A TILE (2026-11-16).
//
// The Claude on-call starts a "desk" session when a bell goes unanswered. While the deck runs, it asks the deck:
//   1. it writes <userData>/desk-open/<id>.request.json (whole: a temp file renamed in);
//   2. the deck TAKES it - renames it to <id>.taken.json; whoever renames first owns it, so a request the on-call
//      has already withdrawn (<id>.withdrawn.json) is never opened - checks it (deskCheckRequest), opens the session
//      as a tile (the deck's openDeskRequest, handed in as `open`; deskOpenPlan says what to do, deskPickSpot where)
//      and makes it noticeable;
//   3. it answers <id>.ack.json: { id, ok: true, sessionId, tileId, pid, window, primary, cells, newWindow, how, at }
//      or { id, ok: false, error, at }. The on-call reads and removes it.
// One line per request goes to <userData>/desk-open/log.jsonl (never the prompt). Old answers, withdrawn and taken
// files are pruned after a day.
//
// PURE over two seams: Files and Clock. There is no watcher and no timer here: the shell calls start() once and
// drain() every kDeskOpenPollMs. Checked against the REAL lib/deskopen.js (core/test/deck/gen-deskopen.js).
// Needs core/deck/viewer.cpp (Node's path.normalize, String(x)) and core/deck/util.cpp.
#pragma once
#include "seams.hpp"
#include <functional>
#include <string>
#include <vector>

namespace deck { namespace core {

extern const char* const kDeskOpenDir;                                    // DIR: "desk-open"
constexpr size_t kDeskPromptMax = 24000;                                  // PROMPT_MAX (UTF-16 units)
constexpr double kDeskOpenPollMs = 1500;                                  // start(pollMs = 1500)
constexpr double kDeskPruneMs = 24 * 3600e3;                              // PRUNE_MS

bool deskOpenIdOk(const std::string& id);                                 // ID_RE

// pickSpot. windows: [{ index, primary, layout: { tileId: [cells] }, showing }]; grid: { rows, cols }
// -> { window: <index>, cells: [k] } | { newWindow: true, cells: [1] }
json deskPickSpot(const json& windows, const json& grid);
// checkRequest -> { ok: true, req } | { ok: false, error }
json deskCheckRequest(const json& raw, const std::string& id, double now);
// accountFor. accounts: [{ id, dir }] -> the account object, or null. `win`: which path.normalize (the deck's own
// platform; path.win32 on this box)
json deskAccountFor(const json& accounts, const std::string& dir, bool win = true);
// openPlan. tiles: [{ id, kind, account, sessionId, cwd, running, userTitle }]
// -> { ok: false, error } | { ok: true, action: "type", tileId, text } | { ok: true, action: "wake", tileId, spec }
//    | { ok: true, action: "open", spec, title };  spec = { kind: "claude", account, sessionId, cwd, desk, prompt, model, browser? }
json deskOpenPlan(const json& req, const json& tiles, const json& accounts, const std::function<bool(const std::string&)>& isDir, bool win = true);
std::string deskPlacedWords(const json& place);                           // placedWords

class DeskOpenInbox {
 public:
  // open(req) -> result json ({ ok: true, sessionId, tileId, pid, window, primary, cells, newWindow, how } |
  // { ok: false, error }); it may throw std::exception: its what() is the refusal, cut to 300
  DeskOpenInbox(Files& files, Clock& clock, std::string dir, std::function<json(const json& req)> open);
  void start();                 // mkdir + the first drain (the deck then calls drain() every kDeskOpenPollMs)
  std::vector<json> drain();    // the acks written this pass
  const std::string& dir() const { return dir_; }
 private:
  void log(const nlohmann::ordered_json& row);
  void prune(const std::vector<std::string>& names);
  Files& files_; Clock& clock_;
  std::string dir_;
  std::function<json(const json&)> open_;
  bool busy_ = false;
};

}}  // namespace deck::core
