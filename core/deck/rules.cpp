// core/deck/rules.cpp — see rules.hpp. Ported from main.js, lib/accounts.js, lib/hibernate.js.
#include "rules.hpp"
#include "viewer.hpp"
#include "../session/jsutil.hpp"

#include <algorithm>
#include <stdexcept>

namespace deck { namespace core {

using namespace deck::session::js;

// ---------------------------------------------------------------- environment
static void envSet(EnvList& env, const std::string& k, const std::string& v) {
  for (auto& kv : env) if (kv.first == k) { kv.second = v; return; }
  env.emplace_back(k, v);
}
static void envDelete(EnvList& env, const std::string& k) {
  env.erase(std::remove_if(env.begin(), env.end(), [&](const std::pair<std::string, std::string>& kv) { return kv.first == k; }), env.end());
}
static std::string upper(std::string s) { for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A'); return s; }

EnvList baseEnv(const EnvList& parent) {
  EnvList env;
  for (const auto& kv : parent) {
    const std::string u = upper(kv.first);
    // /^CLAUDE(CODE|_)/i.test(k) || k === 'AI_AGENT'
    if (u.compare(0, 10, "CLAUDECODE") == 0 || u.compare(0, 7, "CLAUDE_") == 0 || kv.first == "AI_AGENT") continue;
    if (kv.first == "NO_COLOR") continue;
    env.push_back(kv);
  }
  envSet(env, "CLAUDE_CODE_FORCE_SESSION_PERSISTENCE", "1");
  envSet(env, "TERM_PROGRAM", "claude-deck");
  envSet(env, "COLORTERM", "truecolor");
  return env;
}

EnvList envFor(const EnvList& parent, const Account& acct) {
  EnvList env = baseEnv(parent);
  if (acct.isDefault) envDelete(env, "CLAUDE_CONFIG_DIR"); else envSet(env, "CLAUDE_CONFIG_DIR", acct.dir);
  return env;
}

// ---------------------------------------------------------------- accounts
std::string normalizeWinPath(const std::string& in) {
  if (in.empty()) return ".";
  std::string p = in;
  for (char& c : p) if (c == '/') c = '\\';
  std::string root; size_t i = 0;
  if (p.size() >= 2 && p[1] == ':') { root = p.substr(0, 2); i = 2; if (i < p.size() && p[i] == '\\') { root += '\\'; while (i < p.size() && p[i] == '\\') i++; } }
  else if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') { root = "\\\\"; while (i < p.size() && p[i] == '\\') i++; }
  else if (p[0] == '\\') { root = "\\"; while (i < p.size() && p[i] == '\\') i++; }
  const bool absolute = !root.empty() && root.back() == '\\';
  const bool trailing = p.size() > i && p.back() == '\\';
  std::vector<std::string> parts;
  while (i < p.size()) {
    size_t j = p.find('\\', i); if (j == std::string::npos) j = p.size();
    const std::string seg = p.substr(i, j - i);
    if (seg.empty() || seg == ".") { /* skip */ }
    else if (seg == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else if (!absolute) parts.push_back(".."); }
    else parts.push_back(seg);
    i = j + 1;
  }
  std::string out = root;
  for (size_t k = 0; k < parts.size(); k++) { if (k) out += '\\'; out += parts[k]; }
  if (trailing && !parts.empty()) out += '\\';
  if (out.empty()) out = ".";
  return out;
}

static bool isAliasChar(char c) { return isWordChar((unsigned char)c); }
static bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }

std::string normalizePath(const std::string& p, bool win) { return win ? normalizeWinPath(p) : viewerPathNormalize(p, false); }

std::vector<Account> parseAccounts(const std::string& text, const std::string& home, bool win) {
  // path.normalize(path.join(HOME, '.claude'))
  const std::string defaultDir = normalizePath(home + (win ? "\\" : "/") + ".claude", win);
  std::vector<Account> byDir;
  { Account a; a.id = "claude"; a.aliases = { "claude" }; a.dir = defaultDir; a.isDefault = true; byDir.push_back(a); }
  size_t at = 0;
  while (at <= text.size()) {
    size_t nl = text.find('\n', at); if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(at, nl - at);
    at = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // /^\s*alias\s+([A-Za-z0-9_]+)=(['"])(.*)\2\s*$/
    size_t i = 0;
    while (i < line.size() && isAsciiSpace(line[i])) i++;
    if (line.compare(i, 5, "alias") != 0) continue;
    i += 5;
    const size_t ws = i; while (i < line.size() && isAsciiSpace(line[i])) i++;
    if (i == ws) continue;
    const size_t n0 = i; while (i < line.size() && isAliasChar(line[i])) i++;
    if (i == n0 || i >= line.size() || line[i] != '=') continue;
    const std::string name = line.substr(n0, i - n0);
    i++;
    if (i >= line.size() || (line[i] != '\'' && line[i] != '"')) continue;
    const char q = line[i++];
    size_t end = line.size(); while (end > i && isAsciiSpace(line[end - 1])) end--;
    if (end <= i || line[end - 1] != q) continue;
    const std::string rhs = line.substr(i, end - 1 - i);
    std::string dir = defaultDir;
    // /CLAUDE_CONFIG_DIR=(?:"([^"]*)"|'([^']*)'|(\S+))/
    static const char kVar[] = "CLAUDE_CONFIG_DIR=";
    for (size_t p = rhs.find(kVar); p != std::string::npos; p = rhs.find(kVar, p + 1)) {
      const size_t v = p + sizeof(kVar) - 1;
      std::string found; bool ok = false;
      if (v < rhs.size() && (rhs[v] == '"' || rhs[v] == '\'')) {
        const size_t close = rhs.find(rhs[v], v + 1);
        if (close != std::string::npos) { found = rhs.substr(v + 1, close - v - 1); ok = true; }
      }
      if (!ok) { size_t e = v; while (e < rhs.size() && !isAsciiSpace(rhs[e]) && rhs[e] != '\n') e++; if (e > v) { found = rhs.substr(v, e - v); ok = true; } }
      if (!ok) continue;
      // .replace(/^\$HOME|^\$\{HOME\}|^~/, HOME)
      if (found.compare(0, 5, "$HOME") == 0) found = home + found.substr(5);
      else if (found.compare(0, 7, "${HOME}") == 0) found = home + found.substr(7);
      else if (found.compare(0, 1, "~") == 0) found = home + found.substr(1);
      dir = found;
      break;
    }
    dir = normalizePath(dir, win);
    Account* acct = nullptr;
    for (auto& a : byDir) if (a.dir == dir) acct = &a;
    if (!acct) { Account a; a.id = name; a.dir = dir; byDir.push_back(a); acct = &byDir.back(); }
    acct->aliases.push_back(name);
    if (!acct->isDefault && name.size() < acct->id.size()) acct->id = name;     // the shortest alias is the display name
  }
  if (byDir.size() > 1) byDir.erase(std::remove_if(byDir.begin(), byDir.end(), [](const Account& a) { return a.isDefault; }), byDir.end());
  for (auto& a : byDir) { a.key = a.id; a.label = a.id; }
  return byDir;
}

const Account* accountById(const std::vector<Account>& accounts, const std::string& id) {
  for (const auto& a : accounts) {
    if (a.id == id) return &a;
    for (const auto& al : a.aliases) if (al == id) return &a;
  }
  return nullptr;
}

json publicAccounts(const std::vector<Account>& accounts) {
  json out = json::array();
  for (const auto& a : accounts) out.push_back({ { "id", a.id }, { "label", a.label }, { "abbr", a.abbr }, { "email", a.email }, { "key", a.key }, { "isDefault", a.isDefault } });
  return out;
}
json accountRows(const std::vector<Account>& accounts) {
  json out = json::array();
  for (const auto& a : accounts) {
    json kw = json::array();
    for (const auto& k : a.aliases) if (k != "claude") kw.push_back(k);
    out.push_back({ { "id", a.id }, { "keywords", kw }, { "dir", a.dir }, { "email", truthy(a.email) ? a.email : json() }, { "label", a.label }, { "isDefault", a.isDefault } });
  }
  return out;
}

// ---------------------------------------------------------------- newTileSpec
json TileSpec::toJson() const {
  json o = { { "kind", kind } };
  if (!account.is_null()) o["account"] = account;
  if (!sessionId.is_null()) o["sessionId"] = sessionId;
  if (!cwd.is_null()) o["cwd"] = cwd;
  if (hasBrowser) o["browser"] = browser;
  return o;
}
TileSpec newTileSpec(const json& spec) {
  static const json empty = json::object();
  const json& s = truthy(spec) ? spec : empty;
  if (get(s, "kind") == "cloud") throw std::runtime_error("not in the native local deck yet: cloud sessions");
  TileSpec t;
  t.kind = get(s, "kind") == "shell" ? "shell" : "claude";
  t.account = get(s, "account"); t.sessionId = get(s, "sessionId"); t.cwd = get(s, "cwd");
  // browserSpec: only true, false or an explicit null count as naming it
  if (s.is_object() && s.find("browser") != s.end()) {
    const json& b = s["browser"];
    if (b.is_boolean() || b.is_null()) { t.hasBrowser = true; t.browser = b; }
  }
  return t;
}

// ---------------------------------------------------------------- the Claude spawn
ClaudeLaunch claudeLaunch(const ClaudeLaunchInput& in) {
  ClaudeLaunch out;
  out.file = in.claudeExe;
  out.sessionId = in.sessionId;
  // A session that never got a message has no transcript, and `--resume` of it exits at once ("No
  // conversation found"): such a tile starts afresh under the same id instead.
  if (!out.sessionId.empty() && in.transcriptExists) out.args = { "--resume", out.sessionId };
  else { if (out.sessionId.empty()) out.sessionId = in.freshUuid; out.args = { "--session-id", out.sessionId }; }
  if (in.browser) out.args.push_back(*in.browser ? "--chrome" : "--no-chrome");
  if (!in.model.empty()) { out.args.push_back("--model"); out.args.push_back(in.model); }
  if (!in.prompt.empty()) out.args.push_back(in.prompt);
  out.env = envFor(in.parentEnv, in.account);
  if (in.desk && !in.desk->empty()) envSet(out.env, "CLAUDE_ONCALL_KIND", *in.desk + "-desk");
  out.cwd = in.cwd;
  return out;
}

// ---------------------------------------------------------------- settings
json hibernateConfigJson(const json& v) {
  auto clampInt = [&](const char* k, double lo, double hi, int dflt) {
    double d = 0;
    if (!finiteNumber(v, k, &d)) return dflt;
    return (int)std::max(lo, std::min(hi, deck::session::js::round(d)));
  };
  return { { "enabled", get(v, "enabled").is_boolean() ? get(v, "enabled").get<bool>() : true }, { "idleMinutes", clampInt("idleMinutes", 1, 24 * 60, 30) },
    { "keepPinned", get(v, "keepPinned").is_boolean() ? get(v, "keepPinned").get<bool>() : true }, { "maxPerTick", clampInt("maxPerTick", 1, 50, 3) } };
}

json defaultSettings(const std::string& home, const std::string& cwdOverride) {
  auto mk = [](const char* name, const char* from, const char* to) { return json{ { "show", true }, { "name", name }, { "from", from }, { "to", to } }; };
  return {
    { "aliases", json::object() },
    { "fontSize", 14 },
    { "zoomFontSize", 0 },
    { "fontFamily", "" },
    { "cwd", cwdOverride.empty() ? home : cwdOverride },
    { "autoTrust", true },
    { "resumeChoice", "ask" },
    { "animations", true },
    { "effortStyle", "tag" },
    { "sidebarSide", "left" },
    { "pinSide", "edge" },
    { "bars", { { "session", mk("Session (5 h)", "#22c55e", "#ef4444") }, { "week", mk("Week", "#22c55e", "#ef4444") },
                { "fable", mk("Fable (if applicable)", "#22d3ee", "#d946ef") }, { "context", mk("Context", "#fbbf24", "#f97316") } } },
    { "barLabels", { { "time", true }, { "day", true } } },
    { "barLabelMode", "reset" },
    { "msgLines", { { "normal", 3 }, { "wide", 10 } } },
    { "notify", { { "waiting", true }, { "done", true }, { "asks", true }, { "minTurnSec", 20 }, { "sound", true } } },
    { "phone", { { "enabled", false }, { "port", 8788 }, { "bind", "tailscale" }, { "token", "" } } },
    { "pages", json::array() },
    { "hibernate", { { "enabled", true }, { "idleMinutes", 30 }, { "keepPinned", true }, { "maxPerTick", 3 } } },
    { "grid", { { "rows", 2 }, { "cols", 2 } } },
    { "oneWindow", true },
    { "stepEscape", true },
    { "driver", "auto" },
    { "asks", { { "deckThreshold", nullptr }, { "picker", "alongside" }, { "usePages", true }, { "background", true }, { "showTile", true }, { "showMarks", true }, { "closeOnDone", true } } },
    { "deskOpen", true },
  };
}

static bool isAbsoluteWin(const std::string& p) {
  if (p.empty()) return false;
  if (p[0] == '\\' || p[0] == '/') return true;
  return p.size() >= 3 && isWordChar((unsigned char)p[0]) && p[0] != '_' && !(p[0] >= '0' && p[0] <= '9') && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}
static bool endsHtml(const std::string& f, size_t* extLen = nullptr) {
  std::string l = f; for (char& c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  if (l.size() >= 5 && l.compare(l.size() - 5, 5, ".html") == 0) { if (extLen) *extLen = 5; return true; }
  if (l.size() >= 4 && l.compare(l.size() - 4, 4, ".htm") == 0) { if (extLen) *extLen = 4; return true; }
  return false;
}
json cleanPages(const json& list) {
  json out = json::array();
  if (!list.is_array()) return out;
  for (const json& p : list) {
    if (!p.is_object()) continue;
    const json& fv = get(p, "file");
    std::string file = trim(truthy(fv) ? toString(fv) : std::string());
    if (file.size() >= 2 && file.front() == '"' && file.back() == '"') file = file.substr(1, file.size() - 2);
    size_t ext = 0;
    if (file.empty() || !isAbsoluteWin(file) || !endsHtml(file, &ext)) continue;
    const json& lv = get(p, "label");
    std::string label = sliceUnits(trim(truthy(lv) ? toString(lv) : std::string()), 24);
    if (label.empty()) { const size_t s = file.find_last_of("\\/"); const std::string base = s == std::string::npos ? file : file.substr(s + 1); label = base.substr(0, base.size() - ext); }
    out.push_back({ { "label", label }, { "file", file } });
    if (out.size() >= 12) break;
  }
  return out;
}

static json spread(const json& a, const json& b) {         // { ...a, ...b }
  json o = a.is_object() ? a : json::object();
  if (b.is_object()) for (auto it = b.begin(); it != b.end(); ++it) o[it.key()] = it.value();
  return o;
}

json mergeSettings(const json& d, const json& sIn) {
  const json s = sIn.is_object() ? sIn : json::object();
  json out = spread(d, s);
  out["aliases"] = spread(json::object(), truthy(get(s, "aliases")) ? get(s, "aliases") : d["aliases"]);
  out["pages"] = cleanPages(get(s, "pages").is_array() ? get(s, "pages") : json::array());
  json bars = json::object();
  for (auto it = d["bars"].begin(); it != d["bars"].end(); ++it) bars[it.key()] = spread(it.value(), get(get(s, "bars"), it.key().c_str()));
  out["bars"] = bars;
  out["barLabels"] = spread(d["barLabels"], get(s, "barLabels"));
  out["msgLines"] = spread(d["msgLines"], get(s, "msgLines"));
  out["phone"] = spread(d["phone"], get(s, "phone"));
  return out;
}

static bool oneOf(const json& v, std::initializer_list<const char*> options) {
  if (!v.is_string()) return false;
  for (const char* o : options) if (v == o) return true;
  return false;
}
static bool isHex(const json& v) {
  if (!v.is_string()) return false;
  const std::string& s = str(v);
  if (s.size() != 7 || s[0] != '#') return false;
  for (size_t i = 1; i < 7; i++) { const char c = s[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false; }
  return true;
}
static double clampRound(double v, double lo, double hi) { return std::max(lo, std::min(hi, deck::session::js::round(v))); }

json applySettings(const json& D, const json& settings, const json& s, const std::string& newPhoneToken) {
  json next = settings;
  if (!truthy(s)) return next;
  double n = 0;
  const json& al = get(s, "aliases");
  if (al.is_object() && (!al.empty() || truthy(get(s, "aliasesExplicit")))) {
    json a = json::object();
    for (auto it = al.begin(); it != al.end(); ++it) { const std::string val = trim(truthy(it.value()) ? toString(it.value()) : std::string()); if (!it.key().empty() && !val.empty()) a[it.key()] = val; }
    next["aliases"] = a;
  }
  if (finiteNumber(s, "fontSize", &n)) next["fontSize"] = (int)clampRound(n, 9, 28);
  if (finiteNumber(s, "zoomFontSize", &n)) { const double z = deck::session::js::round(n); next["zoomFontSize"] = z <= 0 ? 0 : (int)std::max(9.0, std::min(40.0, z)); }
  if (get(s, "cwd").is_string() && !trim(str(get(s, "cwd"))).empty()) next["cwd"] = trim(str(get(s, "cwd")));
  if (get(s, "autoTrust").is_boolean()) next["autoTrust"] = get(s, "autoTrust");
  if (get(s, "fontFamily").is_string()) next["fontFamily"] = sliceUnits(trim(str(get(s, "fontFamily"))), 80);
  if (oneOf(get(s, "effortStyle"), { "tag", "color", "none" })) next["effortStyle"] = get(s, "effortStyle");
  if (oneOf(get(s, "sidebarSide"), { "left", "right" })) next["sidebarSide"] = get(s, "sidebarSide");
  if (oneOf(get(s, "pinSide"), { "edge", "centre" })) next["pinSide"] = get(s, "pinSide");
  if (oneOf(get(s, "driver"), { "auto", "phone", "desk" })) next["driver"] = get(s, "driver");
  if (oneOf(get(s, "resumeChoice"), { "ask", "full", "summary", "never" })) next["resumeChoice"] = get(s, "resumeChoice");
  if (get(s, "animations").is_boolean()) next["animations"] = get(s, "animations");
  // auditors (auditors.hpp): false = each file from an auditor waits on its child's tile for Send; a template over the AUDITOR prompt
  if (get(s, "auditorAutoSend").is_boolean()) next["auditorAutoSend"] = get(s, "auditorAutoSend");
  // the public build's opt-in for the live usage bars (core/deck/edition.hpp; the private edition polls whatever it says)
  if (get(s, "usageLive").is_boolean()) next["usageLive"] = get(s, "usageLive");
  // and its opt-in for the cloud session list (the same token): ignored by the owner's deck, which always lists them
  if (get(s, "cloudLive").is_boolean()) next["cloudLive"] = get(s, "cloudLive");
  // Pause (pause.hpp): false = after a restart only "Pause until restart" tiles get RESUME by themselves (default: every paused tile)
  if (get(s, "resumeAfterRestart").is_boolean()) next["resumeAfterRestart"] = get(s, "resumeAfterRestart");
  if (get(s, "auditorPrompt").is_string()) { std::string v = get(s, "auditorPrompt").get<std::string>(); if (v.size() > 20000) v.resize(20000); next["auditorPrompt"] = v; }
  const json& bars = get(s, "bars");
  if (bars.is_object() || bars.is_array()) {
    json nb = json::object();
    for (auto it = D["bars"].begin(); it != D["bars"].end(); ++it) {
      const json& d = it.value(); const json& v = get(bars, it.key().c_str());
      const json& name = get(v, "name");
      nb[it.key()] = { { "show", get(v, "show").is_boolean() ? get(v, "show") : d["show"] },
        { "name", (name.is_string() && !trim(str(name)).empty()) ? json(sliceUnits(trim(str(name)), 40)) : d["name"] },
        { "from", isHex(get(v, "from")) ? get(v, "from") : d["from"] }, { "to", isHex(get(v, "to")) ? get(v, "to") : d["to"] } };
    }
    next["bars"] = nb;
  }
  const json& bl = get(s, "barLabels");
  if (bl.is_object() || bl.is_array()) next["barLabels"] = { { "time", !(get(bl, "time") == false) }, { "day", !(get(bl, "day") == false) } };
  if (oneOf(get(s, "barLabelMode"), { "reset", "countdown" })) next["barLabelMode"] = get(s, "barLabelMode");
  const json& nt = get(s, "notify");
  if (nt.is_object() || nt.is_array()) {
    const json& d = D["notify"];
    auto b = [&](const char* k) { return get(nt, k).is_boolean() ? get(nt, k) : d[k]; };
    next["notify"] = { { "waiting", b("waiting") }, { "done", b("done") }, { "asks", b("asks") }, { "sound", b("sound") },
      { "minTurnSec", finiteNumber(nt, "minTurnSec", &n) ? json((int)clampRound(n, 0, 3600)) : d["minTurnSec"] } };
  }
  const json& ml = get(s, "msgLines");
  if (ml.is_object() || ml.is_array()) {
    auto c = [&](const char* k) { return finiteNumber(ml, k, &n) ? json((int)clampRound(n, 1, 40)) : D["msgLines"][k]; };
    next["msgLines"] = { { "normal", c("normal") }, { "wide", c("wide") } };
  }
  const json& ph = get(s, "phone");
  if (ph.is_object() || ph.is_array()) {
    const json& d = truthy(get(settings, "phone")) ? get(settings, "phone") : D["phone"];
    next["phone"] = { { "enabled", get(ph, "enabled").is_boolean() ? get(ph, "enabled") : get(d, "enabled") },
      { "port", finiteNumber(ph, "port", &n) ? json((int)clampRound(n, 1024, 65535)) : get(d, "port") },
      { "bind", oneOf(get(ph, "bind"), { "tailscale", "all", "localhost" }) ? get(ph, "bind") : get(d, "bind") },
      { "token", get(ph, "newToken") == true ? json(newPhoneToken) : get(d, "token") } };
  }
  if (get(s, "pages").is_array()) next["pages"] = cleanPages(get(s, "pages"));
  const json& hb = get(s, "hibernate");
  if (hb.is_object() || hb.is_array()) next["hibernate"] = hibernateConfigJson(spread(get(settings, "hibernate"), hb));
  if (get(s, "oneWindow").is_boolean()) next["oneWindow"] = get(s, "oneWindow");
  const json& ak = get(s, "asks");
  if (ak.is_object() || ak.is_array()) {
    const json& d = truthy(get(settings, "asks")) ? get(settings, "asks") : D["asks"];
    const json& dt = get(ak, "deckThreshold");
    const bool given = ak.is_object() && ak.find("deckThreshold") != ak.end();
    json th;
    if (dt.is_null() || dt == "") th = nullptr;
    else { const double v = toNumber(dt); th = std::isfinite(v) ? json((int)clampRound(v, 0, 999)) : get(d, "deckThreshold"); }
    auto b = [&](const char* k) { return get(ak, k).is_boolean() ? get(ak, k) : (get(d, k).is_boolean() ? get(d, k) : D["asks"][k]); };
    next["asks"] = { { "deckThreshold", given ? th : get(d, "deckThreshold") }, { "picker", oneOf(get(ak, "picker"), { "alongside", "pane" }) ? get(ak, "picker") : get(d, "picker") },
      { "usePages", b("usePages") }, { "background", b("background") }, { "showTile", b("showTile") }, { "showMarks", b("showMarks") }, { "closeOnDone", b("closeOnDone") } };
  }
  if (get(s, "stepEscape").is_boolean()) next["stepEscape"] = get(s, "stepEscape");
  const json& gr = get(s, "grid");
  if (gr.is_object() || gr.is_array()) {
    auto one = [&](const char* k, int& out) {              // Number(v): a missing key is NaN, null is 0 -> clamps to 1
      if (!gr.is_object() || gr.find(k) == gr.end()) return false;
      const double v = toNumber(gr[k]);
      if (!std::isfinite(v)) return false;
      out = (int)clampRound(v, 1, 4);                      // Layout.MAX
      return true;
    };
    int rows = 0, cols = 0;
    const bool hr = one("rows", rows), hc = one("cols", cols);
    next["grid"] = { { "rows", hr ? json(rows) : get(get(settings, "grid"), "rows") }, { "cols", hc ? json(cols) : get(get(settings, "grid"), "cols") } };
  }
  return next;
}

}}  // namespace deck::core
