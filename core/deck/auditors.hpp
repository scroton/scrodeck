// core/deck/auditors.hpp — lib/auditors.js: auditor sessions and their children (2026-11-20). Header-only, like
// pause.hpp: the deck types the prompts and moves the files (deck_more.cpp D::makeAuditor / auditorTick) and hands
// step() the tiles as they are now. The rules, the prompt text and the line are the JS file's; the two must stay the
// same (test/auditors-unit.js, the "auditors" scenario in core/test/deck/deck_scenarios.cpp).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../third_party/nlohmann/json.hpp"
#include "edition.hpp"

namespace deck { namespace core {

// settings.json `auditorAutoSend`: is a file an auditor writes typed into its child without the user's Send? Said
// outright, it is the answer. Unset: yes in the private edition; in the public build NO - each file waits on the child's
// tile for the user's one-click Send (core/deck/edition.hpp).
inline bool auditorAutoSendOn(const nlohmann::json& settings) {
  if (settings.is_object()) { auto it = settings.find("auditorAutoSend"); if (it != settings.end() && it->is_boolean()) return it->get<bool>(); }
  return !edition::kPublic;
}

// {auditorName} {inbox} {toFile} {nowFile} {sentDir} {refusedDir} {logFile} {child1} {child2}; the paths in the deck's own separator.
// By edition (core/deck/edition.hpp): the private edition's text is lib/auditors.js AUDITOR_PROMPT word for word; the
// public build's names the deck scroDeck and the person "the user", and asks for whatever permissions a child's run
// needs instead of naming a skill the public build does not have.
inline const char* auditorPromptText() {
  if (edition::kPublic)
    return "AUDITOR: the user made this session an auditor in " DECK_EDITION_PRODUCT ", named \"{auditorName}\". They will make other sessions its children; the deck announces each one here in one line: CHILD <n>: session <id>, tile \"<title>\", account <account>, folder <folder>, browser <on|off|default>. Nothing reaches a child unless you write it.\n"
      "1. To give child n a prompt, write the whole prompt to {toFile}: one file = one prompt, written in one go (the deck takes the file once it has stood still for two seconds) and never appended to.\n"
      "2. When child n is idle at its prompt, the deck types that file into it as the user's own message - the child cannot tell it from the user typing it - and moves the file to {sentDir}. So write only what the user would send, plainly: the task, and whatever permissions a child needs for its run.\n"
      "3. Name the file {nowFile} instead and the deck interrupts child n at once (Esc, mid-turn) and types it then. Use that only when the child must change course before it does more work on something wrong; everything else waits for its turn as {toFile}.\n"
      "4. A file for a number that is not your child goes to {refusedDir} with a note line at its top; each delivery and refusal is one line in {logFile}. The deck renames your children {child1}, {child2}, ...\n"
      "Answer with one line: AUDITOR {auditorName} ready - {inbox}. Then wait for the CHILD lines.";
  return "AUDITOR: the user made this session an auditor in the Claude Deck, named \"{auditorName}\". They will make other sessions its children; the deck announces each one here in one line: CHILD <n>: session <id>, tile \"<title>\", account <account>, folder <folder>, browser <on|off|default>. Nothing reaches a child unless you write it.\n"
    "1. To give child n a prompt, write the whole prompt to {toFile}: one file = one prompt, written in one go (the deck takes the file once it has stood still for two seconds) and never appended to.\n"
    "2. When child n is idle at its prompt, the deck types that file into it as the user's own message - the child cannot tell it from the user typing it - and moves the file to {sentDir}. So write only what the user would send, plainly: the task, and the overnight-ask permission blocks whenever a child needs them for its run.\n"
    "3. Name the file {nowFile} instead and the deck interrupts child n at once (Esc, mid-turn) and types it then. Use that only when the child must change course before it does more work on something wrong; everything else waits for its turn as {toFile}.\n"
    "4. A file for a number that is not your child goes to {refusedDir} with a note line at its top; each delivery and refusal is one line in {logFile}. The deck renames your children {child1}, {child2}, ...\n"
    "Answer with one line: AUDITOR {auditorName} ready - {inbox}. Then wait for the CHILD lines.";
}
// "auditor 1" + 2 -> "auditor 1.2"; "Build Bot MVP S" + 2 -> "Build Bot MVP S2"
inline std::string auditorChildName(const std::string& s, int n) {
  return (!s.empty() && std::isdigit((unsigned char)s.back()) ? s + "." : s) + std::to_string(n);
}
inline std::string auditorFill(std::string text, const std::vector<std::pair<std::string, std::string>>& vars) {
  for (const auto& kv : vars) {
    const std::string key = "{" + kv.first + "}";
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + kv.second.size())) text.replace(at, key.size(), kv.second);
  }
  return text;
}
inline std::string auditorPrompt(const std::string& tmpl, const std::string& string, const std::string& inbox, const std::string& sep) {
  std::string base = inbox; while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
  const std::string s = sep.empty() ? "/" : sep;
  return auditorFill(tmpl.empty() ? auditorPromptText() : tmpl, { { "auditorName", string }, { "inbox", inbox }, { "toFile", base + s + "to-<n>.md" }, { "nowFile", base + s + "to-<n>.now.md" },
    { "sentDir", base + s + "sent" + s }, { "refusedDir", base + s + "refused" + s }, { "logFile", base + s + "log.md" }, { "child1", auditorChildName(string, 1) }, { "child2", auditorChildName(string, 2) } });
}
// the CHILD line: CHILD 2: session <id>, tile "<title>", account <account>, folder <cwd>, browser on|off|default
struct AuditorChildFacts { std::string sessionId, title, account, cwd; int browser = -1; };   // browser: 1 on, 0 off, -1 default
inline std::vector<std::pair<std::string, std::string>> auditorChildFields(const AuditorChildFacts& f) {
  std::string title = f.title; for (char& c : title) if (c == '"') c = '\'';
  return { { "session", f.sessionId.empty() ? "-" : f.sessionId }, { "tile", "\"" + title + "\"" }, { "account", f.account.empty() ? "-" : f.account },
    { "folder", f.cwd.empty() ? "-" : f.cwd }, { "browser", f.browser == 1 ? "on" : f.browser == 0 ? "off" : "default" } };
}
inline std::string auditorChildLine(int n, const AuditorChildFacts& f) {
  std::string out = "CHILD " + std::to_string(n) + ": "; bool first = true;
  for (const auto& kv : auditorChildFields(f)) { if (!first) out += ", "; first = false; out += kv.first + " " + kv.second; }
  return out;
}
// the line typed into the auditor when a child leaves it: why "un-childed" | "closed" (lib/auditors.js leftLine)
inline std::string auditorLeftLine(int n, const std::string& title, const std::string& why) {
  std::string t = title; for (char& c : t) if (c == '"') c = '\'';
  return "CHILD " + std::to_string(n) + " left: tile \"" + t + "\" (" + (why.empty() ? std::string("un-childed") : why) + ")";
}
inline std::string auditorLower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
const int AUDITOR_STRING_MAX = 40;
// "" when refused (blank after cleaning, or too long)
inline std::string auditorCleanString(const std::string& in) {
  std::string v; for (char c : in) if ((unsigned char)c >= 0x20 && c != 0x7f) v += c;
  size_t a = 0, b = v.size(); while (a < b && std::isspace((unsigned char)v[a])) a++; while (b > a && std::isspace((unsigned char)v[b - 1])) b--;
  v = v.substr(a, b - a);
  return v.size() <= (size_t)AUDITOR_STRING_MAX ? v : std::string();
}
inline std::string auditorNextString(const std::vector<std::string>& inUse) {
  std::set<std::string> used; for (const auto& s : inUse) used.insert(auditorLower(s));
  for (int k = 1; ; k++) if (!used.count("auditor " + std::to_string(k))) return "auditor " + std::to_string(k);
}
// to-<n>.now.md: URGENT (2026-11-23) - typed at once, Esc first into a running turn
inline bool auditorIsNowName(const std::string& name) { return name.size() >= 11 && auditorLower(name.substr(name.size() - 7)) == ".now.md"; }
// to-<n>.md / to-<n>.now.md (1-4 digits, any case) -> n; 0 otherwise
inline int auditorToIndex(const std::string& name) {
  if (name.size() < 7 || auditorLower(name.substr(0, 3)) != "to-" || auditorLower(name.substr(name.size() - 3)) != ".md") return 0;
  const std::string d = name.substr(3, name.size() - (auditorIsNowName(name) ? 10 : 6));
  if (d.empty() || d.size() > 4) return 0;
  for (char c : d) if (!std::isdigit((unsigned char)c)) return 0;
  return std::stoi(d);
}
inline bool auditorIsNow(const std::string& name) { return auditorToIndex(name) > 0 && auditorIsNowName(name); }
// the record of the back-and-forth: one line per delivery or refusal in <inbox>/log.md (lib/auditors.js logLine)
inline std::string auditorLogLine(const std::string& stamp, int n, const std::string& name, const std::string& outcome) {
  return stamp + " | child " + std::to_string(n) + " | " + name + " | " + outcome;
}
struct AuditorInboxPlan { std::vector<std::pair<std::string, int>> take, refuse; };
inline AuditorInboxPlan auditorInboxPlan(std::vector<std::string> names, const std::vector<int>& kids, const std::vector<std::string>& taken) {
  AuditorInboxPlan p;
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const int n = auditorToIndex(name);
    if (!n || std::find(taken.begin(), taken.end(), name) != taken.end()) continue;
    (std::find(kids.begin(), kids.end(), n) != kids.end() ? p.take : p.refuse).push_back({ name, n });
  }
  std::stable_sort(p.take.begin(), p.take.end(), [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) { return a.second < b.second; });
  return p;
}
// no BOM, LF line ends, no trailing whitespace
inline std::string auditorPromptText(const std::string& raw) {
  std::string s = raw.compare(0, 3, "\xEF\xBB\xBF") == 0 ? raw.substr(3) : raw, out;
  for (size_t i = 0; i < s.size(); i++) { if (s[i] == '\r') { out += '\n'; if (i + 1 < s.size() && s[i + 1] == '\n') i++; } else out += s[i]; }
  while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
  return out;
}
const size_t AUDITOR_TEXT_MAX = 100000;
const double AUDITOR_SETTLE_MS = 2000, AUDITOR_EMPTY_MAX_MS = 30000, AUDITOR_SETTLE_MAX_MS = 120000;
const int AUDITOR_KEEP_SENT = 5;
inline std::string auditorRefusedNote(const std::string& stamp, const std::string& string, int n, const std::string& why = std::string()) {
  return "REFUSED " + stamp + " by the Claude Deck: " + (why.empty() ? string + " has no child " + std::to_string(n) : why) + ".";
}
// "2026-11-23 15-02-10" (local time; offsetMs: local minus UTC)
inline std::string auditorFileStamp(double ms, double offsetMs) {
  const std::time_t secs = (std::time_t)std::floor((ms + offsetMs) / 1000.0);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char b[32]; std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return b;
}

struct Auditors {
  struct Aud { std::string string; int last = 0; std::string prevTitle; bool hasPrev = false; };
  struct Kid { std::string parent; int n = 0; std::string prevTitle; bool hasPrev = false; uint64_t seq = 0; };
  struct Result { bool ok = false; std::string string, why; int n = 0; bool again = false; std::string left; int leftN = 0; };   // left / leftN: the auditor it left, its number there
  struct Ended { std::string id; int n = 0; std::string prevTitle; bool hasPrev = false; };
  std::map<std::string, Aud> a;
  std::map<std::string, Kid> c;
  uint64_t seq = 0;

  bool isAuditor(const std::string& id) const { return a.count(id) > 0; }
  const Aud* auditor(const std::string& id) const { auto it = a.find(id); return it == a.end() ? nullptr : &it->second; }
  const Kid* child(const std::string& id) const { auto it = c.find(id); return it == c.end() ? nullptr : &it->second; }
  std::vector<std::string> strings() const { std::vector<std::string> v; for (const auto& kv : a) v.push_back(kv.second.string); return v; }
  std::string nextString() const { return auditorNextString(strings()); }
  std::vector<std::pair<std::string, int>> children(const std::string& parent) const {
    std::vector<std::pair<std::string, int>> v; for (const auto& kv : c) if (kv.second.parent == parent) v.push_back({ kv.first, kv.second.n });
    std::sort(v.begin(), v.end(), [](const std::pair<std::string, int>& x, const std::pair<std::string, int>& y) { return x.second < y.second; });
    return v;
  }
  std::string childAt(const std::string& parent, int n) const {
    const std::string* best = nullptr; uint64_t bs = 0;
    for (const auto& kv : c) if (kv.second.parent == parent && kv.second.n == n && (!best || kv.second.seq < bs)) { best = &kv.first; bs = kv.second.seq; }
    return best ? *best : std::string();
  }
  // "" = not ours
  std::string nameOf(const std::string& id) const {
    if (const Aud* x = auditor(id)) return x->string;
    const Kid* k = child(id); const Aud* p = k ? auditor(k->parent) : nullptr;
    return p ? auditorChildName(p->string, k->n) : std::string();
  }
  bool taken(const std::string& s, const std::string& except) const {
    for (const auto& kv : a) if (kv.first != except && auditorLower(kv.second.string) == auditorLower(s)) return true;
    return false;
  }
  Result make(const std::string& id, const std::string& string, bool hasPrev, const std::string& prevTitle) {
    Result r;
    if (c.count(id)) { r.why = "a child cannot be an auditor: un-child it first"; return r; }
    std::string blank = string; blank.erase(std::remove_if(blank.begin(), blank.end(), [](char ch) { return std::isspace((unsigned char)ch); }), blank.end());
    const std::string s = blank.empty() ? nextString() : auditorCleanString(string);
    if (s.empty()) { r.why = "an auditor's name is 1 to " + std::to_string(AUDITOR_STRING_MAX) + " characters"; return r; }
    if (taken(s, id)) { r.why = "\"" + s + "\" is taken by another auditor"; return r; }
    auto it = a.find(id);
    if (it != a.end()) { it->second.string = s; r.ok = true; r.string = s; r.again = true; return r; }
    Aud x; x.string = s; x.hasPrev = hasPrev && !prevTitle.empty(); x.prevTitle = x.hasPrev ? prevTitle : std::string(); a[id] = x;
    r.ok = true; r.string = s; return r;
  }
  Result setChild(const std::string& id, const std::string& parent) {
    Result r;
    auto p = a.find(parent);
    if (p == a.end()) { r.why = "not an auditor"; return r; }
    if (id == parent) { r.why = "an auditor cannot be its own child"; return r; }
    if (a.count(id)) { r.why = "an auditor cannot be a child: un-make it first"; return r; }
    auto was = c.find(id);
    if (was != c.end() && was->second.parent == parent) { r.ok = true; r.n = was->second.n; r.again = true; return r; }
    p->second.last += 1;
    Kid k; k.parent = parent; k.n = p->second.last; k.seq = ++seq;
    if (was != c.end()) { k.prevTitle = was->second.prevTitle; k.hasPrev = was->second.hasPrev; r.left = was->second.parent; r.leftN = was->second.n; }
    c[id] = k;
    r.ok = true; r.n = k.n; return r;
  }
  void setPrevTitle(const std::string& id, bool has, const std::string& title) { auto it = c.find(id); if (it != c.end() && !it->second.hasPrev) { it->second.hasPrev = has && !title.empty(); it->second.prevTitle = it->second.hasPrev ? title : std::string(); } }
  bool unchild(const std::string& id, Ended* out = nullptr) {
    auto it = c.find(id); if (it == c.end()) return false;
    if (out) { out->id = id; out->n = it->second.n; out->prevTitle = it->second.prevTitle; out->hasPrev = it->second.hasPrev; }
    c.erase(it); return true;
  }
  // -> false when not an auditor; prev: its name before; children: each child ended with it, in index order
  bool unmake(const std::string& id, Ended& prev, std::vector<Ended>& children) {
    auto it = a.find(id); if (it == a.end()) return false;
    prev.id = id; prev.prevTitle = it->second.prevTitle; prev.hasPrev = it->second.hasPrev;
    for (auto k = c.begin(); k != c.end();) {
      if (k->second.parent == id) { Ended e; e.id = k->first; e.n = k->second.n; e.prevTitle = k->second.prevTitle; e.hasPrev = k->second.hasPrev; children.push_back(e); k = c.erase(k); }
      else ++k;
    }
    std::sort(children.begin(), children.end(), [](const Ended& x, const Ended& y) { return x.n < y.n; });
    a.erase(it); return true;
  }
  nlohmann::json saved(const std::string& id) const {
    if (const Aud* x = auditor(id)) { nlohmann::json o = { { "string", x->string }, { "last", x->last } }; if (x->hasPrev) o["prevTitle"] = x->prevTitle; return { { "auditor", o } }; }
    if (const Kid* k = child(id)) { nlohmann::json o = { { "parent", k->parent }, { "n", k->n } }; if (k->hasPrev) o["prevTitle"] = k->prevTitle; return { { "child", o } }; }
    return nullptr;
  }
  void load(const std::string& id, const nlohmann::json& s) {
    if (!s.is_object()) return;
    if (s.contains("auditor") && s["auditor"].is_object()) {
      const nlohmann::json& x = s["auditor"];
      const std::string str = x.contains("string") && x["string"].is_string() ? auditorCleanString(x["string"].get<std::string>()) : std::string();
      if (str.empty() || taken(str, id)) return;
      Aud r; r.string = str; r.last = x.contains("last") && x["last"].is_number() ? std::max(0, (int)std::floor(x["last"].get<double>())) : 0;
      if (x.contains("prevTitle") && x["prevTitle"].is_string()) { r.prevTitle = x["prevTitle"].get<std::string>(); r.hasPrev = !r.prevTitle.empty(); }
      a[id] = r;
    } else if (s.contains("child") && s["child"].is_object()) {
      const nlohmann::json& x = s["child"];
      if (!x.contains("parent") || !x["parent"].is_string() || !x.contains("n") || !x["n"].is_number() || x["n"].get<double>() < 1) return;
      Kid k; k.parent = x["parent"].get<std::string>(); k.n = (int)std::floor(x["n"].get<double>()); k.seq = ++seq;
      if (x.contains("prevTitle") && x["prevTitle"].is_string()) { k.prevTitle = x["prevTitle"].get<std::string>(); k.hasPrev = !k.prevTitle.empty(); }
      c[id] = k;
    }
  }
  // after every tile's load: a child whose auditor did not come back ends; no two children of one auditor share an n
  void settle() {
    std::vector<std::string> drop;
    for (const auto& kv : c) { auto p = a.find(kv.second.parent); if (p == a.end() || kv.second.parent == kv.first || childAt(kv.second.parent, kv.second.n) != kv.first) drop.push_back(kv.first); }
    for (const auto& id : drop) c.erase(id);
    for (const auto& kv : c) { Aud& p = a[kv.second.parent]; if (kv.second.n > p.last) p.last = kv.second.n; }
  }
  // the tile's meta fields: { auditor: { string, children: [{ id, n }] } } | { child: { parent, n, auditorName } } | null
  nlohmann::json view(const std::string& id) const {
    if (const Aud* x = auditor(id)) { nlohmann::json ch = nlohmann::json::array(); for (const auto& kv : children(id)) ch.push_back({ { "id", kv.first }, { "n", kv.second } }); return { { "auditor", { { "string", x->string }, { "children", ch } } } }; }
    const Kid* k = child(id); const Aud* p = k ? auditor(k->parent) : nullptr;
    if (p) return { { "child", { { "parent", k->parent }, { "n", k->n }, { "auditorName", p->string } } } };
    return nullptr;
  }
};

// the deliveries: what the deck types, one at a time per target, only into a tile running, idle after its own start
// and not paused; after one, the target is left alone until its turn has ended (a status written after the typing)
// or AUDITOR_SETTLE_MAX_MS passed. kind "auditor" | "line" | "file" (a file is read at the moment it is typed).
struct Deliveries {
  // about: the child a CHILD line is about; urgent: a to-<n>.now.md file; esc: set by step() when it is typed into a running turn
  struct Item { std::string id, state, kind, target, text, from, fromName, name, file, firstLine, about; int n = 0; double at = 0, sentAt = 0; bool urgent = false, esc = false; };
  // ready: idle after its own start; live: up after its own start, idle or not (what an urgent item needs)
  struct View { std::string id; bool running = false, ready = false, live = false, asleep = false, paused = false; double statusAt = 0; };
  std::vector<Item> items; uint64_t seqNo = 0; std::map<std::string, double> last;

  Item& add(Item it, bool hold, double now) { it.id = "m" + std::to_string(++seqNo); it.state = hold ? "hold" : "queued"; it.at = now; it.sentAt = 0; items.push_back(it); return items.back(); }
  Item* get(const std::string& id) { for (auto& i : items) if (i.id == id) return &i; return nullptr; }
  bool send(const std::string& id) { Item* i = get(id); if (!i || i->state != "hold") return false; i->state = "queued"; return true; }
  std::vector<std::string> pendingFiles(const std::string& from) const { std::vector<std::string> v; for (const auto& i : items) if (i.kind == "file" && i.from == from && (i.state == "hold" || i.state == "queued")) v.push_back(i.name); return v; }
  template <class P> size_t drop(P pred) { const size_t n0 = items.size(); items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return (i.state == "hold" || i.state == "queued") && pred(i); }), items.end()); return n0 - items.size(); }
  size_t dropFor(const std::string& target) { return drop([&](const Item& i) { return i.target == target; }); }
  size_t dropFrom(const std::string& from) { return drop([&](const Item& i) { return i.from == from; }); }
  void remove(const std::string& id) { items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return i.id == id; }), items.end()); }
  nlohmann::json forTile(const std::string& target) const {
    std::vector<const Item*> waiting, sent;
    for (const auto& i : items) if (i.kind == "file" && i.target == target) (i.state == "sent" ? sent : waiting).push_back(&i);
    if (sent.size() > (size_t)AUDITOR_KEEP_SENT) sent.erase(sent.begin(), sent.end() - AUDITOR_KEEP_SENT);
    nlohmann::json out = nlohmann::json::array();
    auto num = [](double v) { return v == std::floor(v) ? nlohmann::json((int64_t)v) : nlohmann::json(v); };
    for (const auto* list : { &waiting, &sent }) for (const Item* i : *list)
      out.push_back({ { "id", i->id }, { "state", i->state }, { "at", num(i->at) }, { "sentAt", num(i->sentAt) }, { "from", i->fromName }, { "n", i->n }, { "name", i->name }, { "firstLine", i->firstLine }, { "urgent", i->urgent } });
    return out;
  }
  // -> the items to type now (copies; their state is "sent" here already); wake: asleep targets with something queued
  std::vector<Item> step(const std::vector<View>& views, double now, std::vector<std::string>& wake) {
    std::vector<Item> type; std::set<std::string> seen;
    std::vector<Item*> order;                      // the queued items, an urgent one first (each target takes its first)
    for (auto& it : items) if (it.state == "queued" && it.urgent) order.push_back(&it);
    for (auto& it : items) if (it.state == "queued" && !it.urgent) order.push_back(&it);
    for (Item* p : order) {
      Item& it = *p;
      if (seen.count(it.target)) continue;
      seen.insert(it.target);
      const View* v = nullptr; for (const View& x : views) if (x.id == it.target) { v = &x; break; }
      if (!v) continue;
      if (v->asleep) { wake.push_back(it.target); continue; }
      auto l = last.find(it.target);
      const double at = l == last.end() ? 0 : l->second;
      const bool settled = !at || v->statusAt > at || now - at >= AUDITOR_SETTLE_MAX_MS;
      if (it.urgent) {
        if (!(v->running && (v->ready || v->live) && !v->paused && settled)) continue;
        it.esc = !v->ready;                        // a running turn (or a question) is interrupted; an idle prompt needs no Esc
      } else if (!(v->running && v->ready && !v->paused && settled)) continue;
      it.state = "sent"; it.sentAt = now; last[it.target] = now;
      type.push_back(it);
    }
    items.erase(std::remove_if(items.begin(), items.end(), [](const Item& i) { return i.state == "sent" && i.kind != "file"; }), items.end());
    std::map<std::string, int> keep;
    for (auto i = items.rbegin(); i != items.rend(); ++i) if (i->state == "sent") { if (++keep[i->target] > AUDITOR_KEEP_SENT) i->state = "gone"; }
    items.erase(std::remove_if(items.begin(), items.end(), [](const Item& i) { return i.state == "gone"; }), items.end());
    return type;
  }
};

}}  // namespace deck::core
