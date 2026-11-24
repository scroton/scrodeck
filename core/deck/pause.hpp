// core/deck/pause.hpp — lib/pause.js: Pause / Resume / Pause until restart (2026-11-20). Header-only, like
// QuietRestart and RestoreRefresh: the deck types the prompts (deck_more.cpp D::pauseTile / pauseTick) and hands
// step() the tiles as they are now. The rules, the prompt texts and the line tests are the JS file's; the two must stay
// the same (test/pause-unit.js, the "pause" scenario in core/test/deck/deck_scenarios.cpp).
// A PAUSE prompt the deck did not type (pasted or typed by hand) is read off the session's transcript (the tail's
// lastPromptKind) and marks the tile `by: prompt`; any pause ends when the session gets another prompt after it; a seen
// one also when the tile moves to a new session id (2026-11-24; the "pause mark" scenario). ANY pause - the deck's
// Pause, Pause until restart with its queued Resume, a seen one - ends when the tile moves to a new session id (/clear);
// the pause file stays as the session wrote it (2026-11-24; the "pause + clear" scenario).
#pragma once
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../third_party/nlohmann/json.hpp"
#include "edition.hpp"

namespace deck { namespace core {

inline const char* pausePromptText() {
  return "PAUSE: " DECK_EDITION_THE " is about to restart. Stop what you are doing now; start nothing new.\n"
    "1. Stop every background process you started (background shell tasks, watchers, Monitors, loops, scheduled wakeups). Touch nothing another session or the deck started.\n"
    "2. If an outward step was cut off (push, merge, deploy, migration, Slack or email send), check whether it went through. Do not redo it now.\n"
    "3. Write {pauseFile}: your task and its next step; each process you stopped, with the exact command to start it again; each role or claim you hold (on-call or security desk, planner, driver claim, browser hold, build lock, oncall awaits); for each Slack channel or DM you were watching, the newest message timestamp (ts) you had read; uncommitted repo changes, left as they are; anything you were waiting on.\n"
    "4. Send no messages, push nothing, commit nothing new. Answer with one line: PAUSED <your session name> - {pauseFile}. Then stop and wait for RESUME.";
}
inline const char* resumePromptText() {
  return "RESUME: " DECK_EDITION_THE " restarted at {restartedAt}. Read {pauseFile}. If it does not exist, answer \"not paused\" and carry on as you were. If you are already working again, do only the steps below that are not done yet.\n"
    "1. Restart the processes listed there that your task still needs, each hidden, with the command recorded.\n"
    "2. Re-take each role or claim listed, the way you took it before. If another session holds one now, leave it and tell the user in one line.\n"
    "3. Slack: for each channel listed, read only messages after the recorded ts, and handle them as you would have.\n"
    "4. Rename {pauseFile} to .resumed, answer RESUMED <your session name>, and continue from the recorded next step.";
}
inline std::string pauseFill(std::string text, const std::string& pauseFile, const std::string& restartedAt) {
  for (const auto& kv : std::vector<std::pair<std::string, std::string>>{ { "{pauseFile}", pauseFile }, { "{restartedAt}", restartedAt } }) {
    if (kv.first == "{restartedAt}" && restartedAt.empty()) continue;
    for (size_t at = text.find(kv.first); at != std::string::npos; at = text.find(kv.first, at + kv.second.size())) text.replace(at, kv.first.size(), kv.second);
  }
  return text;
}
// "2026-11-20 17:42:10 -05:00" (offsetMs: local minus UTC)
inline std::string pauseLocalStamp(double ms, double offsetMs) {
  const std::time_t secs = (std::time_t)std::floor((ms + offsetMs) / 1000.0);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  const long off = std::lround(offsetMs / 60000.0), a = off < 0 ? -off : off;
  char b[48];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d %c%02ld:%02ld", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, off < 0 ? '-' : '+', a / 60, a % 60);
  return b;
}
// a line of the answer that starts with the word and a name (`**PAUSED** x` and `PAUSED x` alike)
inline bool pauseHasLine(const std::string& text, const std::string& word) {
  size_t i = 0;
  while (i <= text.size()) {
    size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
    std::string l; for (size_t k = i; k < e; k++) if (text[k] != '*' && text[k] != '`') l += text[k];
    size_t s = 0; while (s < l.size() && (l[s] == ' ' || l[s] == '\t' || l[s] == '\r' || l[s] == '>' || l[s] == '_')) s++;
    l = l.substr(s);
    if (l.compare(0, word.size() + 1, word + " ") == 0) { for (size_t k = word.size(); k < l.size(); k++) if (l[k] != ' ' && l[k] != '\t' && l[k] != '\r') return true; }
    i = e + 1;
  }
  return false;
}
inline bool pausePausedLine(const std::string& t) { return pauseHasLine(t, "PAUSED"); }
inline bool pauseResumedLine(const std::string& t) {
  if (pauseHasLine(t, "RESUMED")) return true;
  std::string low; for (char c : t) low += (char)std::tolower((unsigned char)c);
  return low.find("not paused") != std::string::npos;
}

struct Pauses {
  // hasPrompt / promptKind / promptAt: the newest prompt a person sent it (TailFacts lastPromptKind / lastPromptAt); sid: its session id now
  struct View { std::string id; bool running = false, ready = false; std::string status; double statusAt = 0; bool hasText = false; std::string lastText; double lastAssistantAt = 0;
    bool hasPrompt = false; std::string promptKind; double promptAt = 0; std::string sid; };
  // file: the pause file the PAUSE prompt named - Resume names the same one, even after a /clear moved the tile to a
  // new session id (tilefollow.hpp); empty: none recorded (a pause saved before this field), the caller names <sid>.md
  // by: "prompt" for a pause read off the transcript (its sid: the session id it was seen in), "" for the deck's own
  struct Rec { std::string state; double at = 0; bool untilRestart = false, want = false, restored = false; bool hasBefore = false; std::string textBefore; double resumeAt = 0; std::string file;
    std::string by, sid; };
  std::map<std::string, Rec> m;
  std::vector<std::string> seen, cleared;   // the last step's: marked by their own PAUSE prompt; ended by another prompt or a /clear

  bool has(const std::string& id) const { return m.count(id) > 0; }
  void forget(const std::string& id) { m.erase(id); }
  void pause(const std::string& id, double now, bool untilRestart, bool hasBefore, const std::string& textBefore, const std::string& file = std::string(), const std::string& sid = std::string()) {
    Rec r; r.state = "pausing"; r.at = now; r.untilRestart = untilRestart; r.hasBefore = hasBefore; r.textBefore = textBefore; r.file = file; r.sid = sid; m[id] = r;
  }
  std::string fileOf(const std::string& id) const { auto it = m.find(id); return it == m.end() ? std::string() : it->second.file; }
  bool canResume(const std::string& id) const { auto it = m.find(id); return it != m.end() && (it->second.state == "pausing" || it->second.state == "paused"); }
  bool resume(const std::string& id) { if (!canResume(id)) return false; m[id].want = true; return true; }
  bool talking(const std::string& id) const { auto it = m.find(id); return it != m.end() && (it->second.state == "resuming" || (it->second.state == "pausing" && !it->second.restored && it->second.by != "prompt")); }
  nlohmann::json saved(const std::string& id) const {
    auto it = m.find(id);
    if (it == m.end() || it->second.state == "resuming") return nullptr;
    const Rec& r = it->second;
    nlohmann::json o = { { "state", r.state }, { "at", r.at == std::floor(r.at) ? nlohmann::json((int64_t)r.at) : nlohmann::json(r.at) }, { "untilRestart", r.untilRestart }, { "want", r.want } };
    if (!r.file.empty()) o["file"] = r.file;
    if (r.by == "prompt") o["by"] = "prompt";
    if (!r.sid.empty()) o["sid"] = r.sid;
    return o;
  }
  // resumeAll: settings.resumeAfterRestart is on (the default) - every pause read back gets its Resume, not only "until
  // restart" (2026-11-24: "have the 'send resume on restart after pause' as a setting, default on")
  void load(const std::string& id, const nlohmann::json& s, bool resumeAll = false) {
    if (!s.is_object() || !s.contains("state") || !s["state"].is_string()) return;
    const std::string st = s["state"].get<std::string>();
    if (st != "pausing" && st != "paused") return;
    Rec r; r.state = st; r.at = s.contains("at") && s["at"].is_number() ? s["at"].get<double>() : 0;
    const bool seenBy = s.contains("by") && s["by"] == "prompt";
    r.untilRestart = !seenBy && s.value("untilRestart", false) == true; r.want = s.value("want", false) == true || r.untilRestart || resumeAll; r.restored = true;
    if (s.contains("file") && s["file"].is_string()) r.file = s["file"].get<std::string>();
    if (seenBy) r.by = "prompt";
    if (s.contains("sid") && s["sid"].is_string()) r.sid = s["sid"].get<std::string>();
    m[id] = r;
  }
  nlohmann::json view(const std::string& id) const {
    auto it = m.find(id);
    if (it == m.end()) return nullptr;
    nlohmann::json o = { { "state", it->second.state }, { "untilRestart", it->second.untilRestart }, { "want", it->second.want } };
    if (it->second.by == "prompt") o["by"] = "prompt";
    return o;
  }
  static bool turnEndedAfter(const View& v, double at) { return v.status == "idle" && v.statusAt > at && v.lastAssistantAt > at; }
  static bool differs(const View& v, const Rec& r) { return v.hasText != r.hasBefore || (v.hasText && v.lastText != r.textBefore); }
  // the ids to type the RESUME prompt into now; changed: what tiles.json keeps or the tiles show changed
  std::vector<std::string> step(const std::vector<View>& views, double now, const std::function<bool(const std::string&)>& settled, bool& changed) {
    std::vector<std::string> resume; changed = false; seen.clear(); cleared.clear();
    // the prompts the session got: a PAUSE marks it, any other prompt after a pause ends it (lib/pause.js, the same order)
    std::vector<const View*> uniq;   // by id, the LAST view of an id (a JS Map built from the list)
    for (const View& x : views) { bool dup = false; for (auto& u : uniq) if (u->id == x.id) { u = &x; dup = true; break; } if (!dup) uniq.push_back(&x); }
    for (const View* v : uniq) {
      auto it = m.find(v->id);
      if (it != m.end() && it->second.sid.empty() && !v->sid.empty()) it->second.sid = v->sid;   // saved before it kept its session id
      if (it != m.end() && ((it->second.state != "resuming" && v->hasPrompt && v->promptKind != "pause" && v->promptAt > it->second.at)
          || (!it->second.sid.empty() && !v->sid.empty() && v->sid != it->second.sid))) {
        m.erase(it); cleared.push_back(v->id); changed = true; continue;
      }
      if (v->hasPrompt && v->promptKind == "pause" && (it == m.end() || (it->second.state == "resuming" && v->promptAt > it->second.resumeAt))) {
        Rec r; r.state = "pausing"; r.at = v->promptAt; r.by = "prompt"; r.sid = v->sid;
        if (!(v->lastAssistantAt > v->promptAt)) { r.hasBefore = v->hasText; r.textBefore = v->lastText; }   // else the answer there is that turn's
        m[v->id] = r; seen.push_back(v->id); changed = true;
      }
    }
    for (auto it = m.begin(); it != m.end();) {
      const View* v = nullptr; for (const View& x : views) if (x.id == it->first) { v = &x; break; }
      if (!v) { it = m.erase(it); changed = true; continue; }
      Rec& r = it->second;
      if (r.state == "pausing" && v->hasText && pausePausedLine(v->lastText)
          && (r.by == "prompt" ? v->lastAssistantAt > r.at && (!r.hasBefore || v->lastText != r.textBefore || turnEndedAfter(*v, r.at)) : (differs(*v, r) || turnEndedAfter(*v, r.at)))) { r.state = "paused"; changed = true; }
      if (r.want && (r.state == "pausing" || r.state == "paused")) {
        if (v->running && v->ready && (!settled || settled(it->first))) {
          r.state = "resuming"; r.want = false; r.untilRestart = false; r.resumeAt = now; r.hasBefore = v->hasText; r.textBefore = v->lastText;
          resume.push_back(it->first); changed = true;
        }
        ++it; continue;
      }
      if (r.state == "resuming" && (turnEndedAfter(*v, r.resumeAt) || (v->hasText && pauseResumedLine(v->lastText) && differs(*v, r)))) { it = m.erase(it); changed = true; continue; }
      ++it;
    }
    return resume;
  }
};

}}  // namespace deck::core
