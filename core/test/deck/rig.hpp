// core/test/deck/rig.hpp — the deck on fake seams, as every scenario file makes it: one Deck, a clock that moves
// only when told, files in memory, processes that are a record of what was asked of them, and each client's frames.
#pragma once
#include "t.hpp"
#include "fakes.hpp"
#include "../../deck/deck.hpp"
#include "../../session/jsutil.hpp"

namespace {

using namespace deck::core;

// (in the run's path mode: fakes.hpp "THE PATH MODE")
#define ALIAS (fake::pc("C:\\home\\.claude-code-account-aliases"))
#define T_DIR (fake::pc("C:\\home\\.claude-t"))
#define U_DIR (fake::pc("C:\\home\\.claude-u"))
using fake::P;

struct Rig {
  fake::Clock clock; fake::MemFiles files; fake::Ptys ptys; fake::Procs procs; fake::Random random; fake::Env env; fake::Notifier notes; fake::Log log; fake::Life life; fake::Http http; fake::Creds creds;
  DeckOptions opt;
  std::unique_ptr<Deck> deck;
  std::map<ClientId, std::vector<json>> out;
  std::set<ClientId> busy;
  bool usage = false, cloud = false;
  PhoneHost* phone = nullptr;
  int rid = 100;

  Rig() {
    files.clock = &clock; ptys.alivePids = &procs.alivePids;
    opt.stateDir = P("C:\\state"); opt.home = P("C:\\home"); opt.aliasFile = ALIAS;
    opt.shellFile = "cmd.exe"; opt.shellArgs = { "/d" }; opt.shellName = "cmd";
    opt.claudeExe = P("C:\\bin\\claude.exe"); opt.bash = P("C:\\Git\\bin\\bash.exe"); opt.userswitch = P("C:\\home\\.local\\bin\\userswitch");
    files.put(ALIAS, "alias claudet='CLAUDE_CONFIG_DIR=\"$HOME/.claude-t\" claude'\nalias claudeu='CLAUDE_CONFIG_DIR=\"$HOME/.claude-u\" claude'\n");
    files.put(std::string(T_DIR) + P("\\.claude.json"), "{\"oauthAccount\":{\"emailAddress\":\"t@example.com\"}}");
  }
  void make() {
    Seams sm; sm.clock = &clock; sm.files = &files; sm.ptys = &ptys; sm.procs = &procs; sm.random = &random; sm.env = &env; sm.notifier = &notes; sm.lifecycle = &life; sm.log = &log;
    if (usage || cloud) { sm.http = &http; sm.credentials = &creds; }
    opt.usagePolling = usage; opt.cloudPolling = cloud;
    sm.phone = phone;
    deck.reset(new Deck(opt, sm, [this](ClientId c, const json& f) { if (busy.count(c)) return false; out[c].push_back(f); return true; }));
  }
  // a deck with one window, started, past the two seconds in which nothing is saved
  ClientId boot() { make(); const ClientId w = deck->addWindow(); deck->start(); send(w, { { "t", "call" }, { "rid", 1 }, { "fn", "deskInit" }, { "args", json::array() } }); advance(2100); return w; }
  void pump() {
    while (!ptys.exits.empty()) {
      const auto e = ptys.exits.front(); ptys.exits.pop_front();
      if (fake::Pty* p = ptys.byToken(e.first)) { p->gone = true; procs.alivePids.erase(p->pid_); }
      deck->onPtyExit(e.first, e.second);
    }
    deck->tick();
  }
  void advance(double ms) {
    const double end = clock.t + ms;
    for (;;) {
      pump();
      if (clock.t >= end) break;
      clock.t += std::min(end - clock.t, std::max(1.0, (double)deck->nextDueMs()));
    }
    pump();
  }
  void send(ClientId c, const json& frame) { deck->handle(c, frame); }
  json call(ClientId c, const std::string& fn, json args = json::array(), double waitMs = 0) {
    const int id = ++rid;
    const size_t from = out[c].size();
    send(c, { { "t", "call" }, { "rid", id }, { "fn", fn }, { "args", args } });
    auto find = [&]() -> json { for (size_t i = from; i < out[c].size(); i++) { const json& f = out[c][i]; if ((f["t"] == "ok" || f["t"] == "err") && f.value("rid", json()) == id) return f; } return json(); };
    json r = find();
    if (r.is_null() && waitMs > 0) { advance(waitMs); r = find(); }
    return r;
  }
  json res(ClientId c, const std::string& fn, json args = json::array()) { return call(c, fn, args).value("result", json()); }
  json lastOf(ClientId c, const char* t) { auto& v = out[c]; for (size_t i = v.size(); i-- > 0;) if (v[i]["t"] == t) return v[i]; return json(); }
  json meta(ClientId c) { deck->tick(); return lastOf(c, "meta").value("meta", json()); }
  // a fresh pass, as a window sees it
  json fresh() { return deck->collectMeta(); }
  json tile(const std::string& id) { const json m = fresh(); for (const json& t : m["tiles"]) if (t["id"] == id) return t; return json(); }
  static json tileIn(const json& meta, const std::string& id) { if (meta.is_object()) for (const json& t : meta["tiles"]) if (t["id"] == id) return t; return json(); }
  size_t count(ClientId c, const char* t, const std::string& id = "", size_t from = 0) { size_t n = 0; auto& v = out[c]; for (size_t i = from; i < v.size(); i++) if (v[i]["t"] == t && (id.empty() || v[i].value("id", "") == id)) n++; return n; }
  std::string data(ClientId c, const std::string& id, size_t from = 0, bool replayOnly = false) {
    std::string s; auto& v = out[c];
    for (size_t i = from; i < v.size(); i++) if (v[i]["t"] == "data" && v[i]["id"] == id && (!replayOnly || v[i].value("replay", false))) s += v[i]["d"].get<std::string>();
    return s;
  }
  std::vector<json> events(ClientId c, const std::string& name, size_t from = 0) { std::vector<json> e; auto& v = out[c]; for (size_t i = from; i < v.size(); i++) if (v[i]["t"] == "event" && v[i]["name"] == name) e.push_back(v[i]["payload"]); return e; }
  void feed(fake::Pty* p, const std::string& bytes) { deck->onPtyData(p->token, bytes.data(), bytes.size()); }
  std::string shell(ClientId w, const std::string& cwd = "") { json spec = { { "kind", "shell" } }; if (!cwd.empty()) spec["cwd"] = cwd; return str(res(w, "newTile", json::array({ spec }))); }
  std::string claude(ClientId w, const std::string& account = "claudet", const std::string& cwd = P("C:\\work")) { return str(res(w, "newTile", json::array({ { { "kind", "claude" }, { "account", account }, { "cwd", cwd } } }))); }
  json tilesFile() { return files.getJson(P("C:\\state\\tiles.json")); }
  // Claude Code's own record of a session: <config>/sessions/<pid>.json
  void record(const char* dir, uint32_t pid, const std::string& sessionId, const std::string& status, json more = json::object()) {
    json j = { { "pid", pid }, { "sessionId", sessionId }, { "status", status }, { "statusUpdatedAt", clock.t }, { "kind", "interactive" } };
    for (auto it = more.begin(); it != more.end(); ++it) j[it.key()] = it.value();
    files.put(std::string(dir) + P("\\sessions\\") + std::to_string(pid) + ".json", j.dump());
  }
  std::string transcriptPath(const char* dir, const std::string& sessionId) { return std::string(dir) + P("\\projects\\C--work\\") + sessionId + ".jsonl"; }
  void transcript(const char* dir, const std::string& sessionId, const std::string& title, const std::string& lastText, size_t pad = 0) {
    // (Claude Code writes `type` first; main.js quickTitle's pattern relies on it, so this line is spelled out)
    std::string t = "{\"type\":\"custom-title\",\"customTitle\":" + json(title).dump() + "}\n"
      + json{ { "type", "user" }, { "cwd", P("C:\\work") }, { "timestamp", "2027-01-15T08:00:00.000Z" }, { "message", { { "role", "user" }, { "content", "hello" } } } }.dump() + "\n"
      + json{ { "type", "assistant" }, { "timestamp", "2027-01-15T08:00:05.000Z" }, { "effort", "high" }, { "message", { { "role", "assistant" }, { "model", "claude-fable-5-1" },
          { "content", json::array({ { { "type", "text" }, { "text", lastText } } }) }, { "usage", { { "input_tokens", 1000 }, { "cache_read_input_tokens", 99000 }, { "output_tokens", 10 } } } } } }.dump() + "\n";
    if (pad) t += json{ { "type", "summary" }, { "pad", std::string(pad, 'x') } }.dump() + "\n";
    files.put(transcriptPath(dir, sessionId), t);
  }
};

inline bool hasEnv(const EnvList& env, const std::string& k, const std::string& v) { for (const auto& kv : env) if (kv.first == k) return kv.second == v; return false; }
inline bool noEnv(const EnvList& env, const std::string& k) { for (const auto& kv : env) if (kv.first == k) return false; return true; }

}  // namespace
