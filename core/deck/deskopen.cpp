// core/deck/deskopen.cpp — see deskopen.hpp. lib/deskopen.js, function for function; each one names what it ports.
#include "deskopen.hpp"
#include "util.hpp"
#include "viewer.hpp"
#include "../session/jsutil.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>

namespace deck { namespace core {

namespace js = deck::session::js;
using ojson = nlohmann::ordered_json;

const char* const kDeskOpenDir = "desk-open";

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isHex(char c) { return isDigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
inline bool isAlnum(char c) { return isDigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'z'); }

// s.length
size_t units(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) n += js::nextCodePoint(s, i) >= 0x10000 ? 2 : 1;
  return n;
}
// String(o.key || '')
std::string strOr(const json& o, const char* key) {
  const json& v = js::get(o, key);
  return js::truthy(v) ? viewerJsString(v) : std::string();
}
// `${o.key}`: a missing key prints as undefined
std::string templ(const json& o, const char* key) {
  if (!o.is_object() || o.find(key) == o.end()) return "undefined";
  return viewerJsString(o[key]);
}
// Number(text)
double numberOfText(const std::string& raw) {
  const std::string s = js::trim(raw);
  if (s.empty()) return 0;
  if (s == "Infinity" || s == "+Infinity") return std::numeric_limits<double>::infinity();
  if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
  if (s.size() > 2 && s[0] == '0' && ((s[1] | 32) == 'x' || (s[1] | 32) == 'o' || (s[1] | 32) == 'b')) {
    const int base = (s[1] | 32) == 'x' ? 16 : (s[1] | 32) == 'o' ? 8 : 2;
    double v = 0;
    for (size_t i = 2; i < s.size(); i++) {
      const char c = s[i];
      const int d = isDigit(c) ? c - '0' : ((c | 32) >= 'a' && (c | 32) <= 'f') ? (c | 32) - 'a' + 10 : 99;
      if (d >= base) return kNaN;
      v = v * base + d;
    }
    return v;
  }
  size_t i = 0; const size_t n = s.size();
  if (s[i] == '+' || s[i] == '-') i++;
  size_t whole = 0, frac = 0;
  while (i < n && isDigit(s[i])) { i++; whole++; }
  if (i < n && s[i] == '.') { i++; while (i < n && isDigit(s[i])) { i++; frac++; } }
  if (!whole && !frac) return kNaN;
  if (i < n && (s[i] | 32) == 'e') {
    i++;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    size_t e = 0;
    while (i < n && isDigit(s[i])) { i++; e++; }
    if (!e) return kNaN;
  }
  if (i != n) return kNaN;
  return std::strtod(s.c_str(), nullptr);
}
// Number(v); `missing` = undefined
double numberOf(const json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
  if (v.is_null()) return 0;
  if (v.is_string()) return numberOfText(v.get_ref<const std::string&>());
  if (v.is_array()) return numberOfText(viewerJsString(v));
  return kNaN;
}
// Number.isInteger
bool isInteger(const json& v) {
  if (v.is_number_integer() || v.is_number_unsigned()) return true;
  if (!v.is_number_float()) return false;
  const double d = v.get<double>();
  return std::isfinite(d) && d == std::floor(d);
}
// JSON.stringify(v) for the "unknown kind" words. A value that arrived as json has its keys sorted already; the
// inbox hands the request's own text in its own order (ordered_json).
template <class J> std::string stringify(const J& v) {
  if (v.is_number_float()) { const double d = v.template get<double>(); return std::isfinite(d) ? js::numberText(d) : std::string("null"); }
  if (v.is_array()) { std::string out = "["; bool first = true; for (const J& x : v) { if (!first) out += ','; first = false; out += stringify(x); } return out + "]"; }
  if (v.is_object()) {
    std::string out = "{"; bool first = true;
    for (auto it = v.begin(); it != v.end(); ++it) { if (!first) out += ','; first = false; out += json(it.key()).dump(-1, ' ', false, json::error_handler_t::replace) + ":" + stringify(it.value()); }
    return out + "}";
  }
  return v.dump(-1, ' ', false, J::error_handler_t::replace);
}
// The order JavaScript keeps an object's keys in: the ones that are array indexes first, ascending, then the rest
// as they were written. Applied to a parsed request so what is written back out (the log's desk) reads as the JS's.
void jsKeyOrder(ojson& v) {
  if (v.is_array()) { for (ojson& x : v) jsKeyOrder(x); return; }
  if (!v.is_object()) return;
  std::vector<std::pair<double, std::string>> indexed; std::vector<std::string> rest;
  for (auto it = v.begin(); it != v.end(); ++it) {
    jsKeyOrder(it.value());
    const std::string& k = it.key();
    bool index = !k.empty() && k.size() <= 10 && !(k.size() > 1 && k[0] == '0');
    double n = 0;
    for (char c : k) { if (!isDigit(c)) { index = false; break; } n = n * 10 + (c - '0'); }
    if (index && n <= 4294967294.0) indexed.emplace_back(n, k); else rest.push_back(k);
  }
  if (indexed.empty()) return;
  std::sort(indexed.begin(), indexed.end());
  ojson out = ojson::object();
  for (const auto& k : indexed) out[k.second] = v[k.second];
  for (const std::string& k : rest) out[k] = v[k];
  v = std::move(out);
}
// numbers as JSON.stringify writes them: 6.0 is 6
void jsNumbers(ojson& v) {
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (!std::isfinite(d)) v = nullptr;
    else if (d == std::floor(d) && std::fabs(d) <= 9007199254740992.0) v = (int64_t)d;
  } else if (v.is_array() || v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) jsNumbers(*it);
  }
}
std::string text(const ojson& v) { return v.dump(-1, ' ', false, ojson::error_handler_t::replace); }   // JSON.stringify(v)
json refusal(const std::string& error) { return { { "ok", false }, { "error", error } }; }
// Math.max(1, Math.min(4, Math.round(Number(grid && grid.rows) || 2)))
int gridSide(const json& grid, const char* key) {
  double v = grid.is_object() && grid.find(key) != grid.end() ? numberOf(grid[key]) : kNaN;   // a grid with no such key: Number(undefined)
  if (std::isnan(v) || v == 0) v = 2;
  v = std::floor(v + 0.5);
  return (int)std::max(1.0, std::min(4.0, v));
}
bool endsWith(const std::string& s, const char* tail) {
  const size_t n = std::char_traits<char>::length(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

}  // namespace

// ID_RE = /^[a-z0-9]{1,20}-\d{8}T\d{6}Z-[0-9a-f]{4}$/i
bool deskOpenIdOk(const std::string& id) {
  size_t i = 0; const size_t n = id.size();
  size_t k = 0;
  while (i < n && isAlnum(id[i])) { i++; k++; }
  if (k < 1 || k > 20) return false;
  if (n - i != 1 + 8 + 1 + 6 + 1 + 1 + 4) return false;
  if (id[i++] != '-') return false;
  for (int d = 0; d < 8; d++) if (!isDigit(id[i++])) return false;
  if ((id[i++] | 32) != 't') return false;
  for (int d = 0; d < 6; d++) if (!isDigit(id[i++])) return false;
  if ((id[i++] | 32) != 'z') return false;
  if (id[i++] != '-') return false;
  for (int d = 0; d < 4; d++) if (!isHex(id[i++])) return false;
  return true;
}

// ---------------------------------------------------------------- the placement (pure): pickSpot
// The order the owner gave: the MAIN window first, then each auxiliary window by its number; in each, the cells in
// reading order; the first cell no session's block covers is the one. None free anywhere -> a new auxiliary
// window with the tile alone in its top-left cell.
json deskPickSpot(const json& windows, const json& grid) {
  const int N = gridSide(grid, "rows") * gridSide(grid, "cols");
  std::vector<const json*> order;
  if (windows.is_array()) for (const json& w : windows) if (w.is_object() && isInteger(js::get(w, "index"))) order.push_back(&w);
  std::stable_sort(order.begin(), order.end(), [](const json* a, const json* b) {
    const int pa = js::truthy(js::get(*a, "primary")) ? 1 : 0, pb = js::truthy(js::get(*b, "primary")) ? 1 : 0;
    if (pa != pb) return pa > pb;
    return js::get(*a, "index").get<double>() < js::get(*b, "index").get<double>();
  });
  for (const json* w : order) {
    const json& lay = js::get(*w, "layout");             // an object (or an array: typeof says object too); else {}
    std::set<double> taken; bool any = false;             // `any`: taken.size, which also counts a NaN
    if (lay.is_object() || lay.is_array()) for (const json& set : lay) {
      if (!set.is_array()) continue;
      for (const json& k : set) { any = true; const double v = numberOf(k); if (!std::isnan(v)) taken.insert(v); }
    }
    if (!any && js::truthy(js::get(*w, "showing"))) continue;      // a session fills it full-bleed: occupied
    for (int k = 1; k <= N; k++) if (!taken.count((double)k)) return { { "window", js::get(*w, "index") }, { "cells", json::array({ k }) } };
  }
  return { { "newWindow", true }, { "cells", json::array({ 1 }) } };
}

// ---------------------------------------------------------------- the request (pure): checkRequest
// kindText: JSON.stringify(r.kind) when the caller still has the request's own key order (the inbox)
static json checkRequest(const json& r, const std::string& id, double now, const std::string* kindText) {
  if (!r.is_object()) return refusal("not a request");
  if (strOr(r, "id") != id || !deskOpenIdOk(id)) return refusal("the request names another id");
  const json& kind = js::get(r, "kind");
  if (!(kind.is_string() && kind.get_ref<const std::string&>() == "desk-open"))
    return refusal("unknown kind " + (r.find("kind") == r.end() ? std::string("undefined") : kindText ? *kindText : stringify(kind)));
  const json& deskV = js::get(r, "desk");
  const std::string desk = js::str(deskV);
  if (!deskV.is_string() || (desk != "security" && desk != "web")) return refusal("desk must be security or web");
  // Date.parse(r.expiresAt): the ISO forms (js::dateParse); anything else, and a value that is not a string, is
  // "no expiry" here (JavaScript's Date.parse also reads legacy date text)
  const json& expV = js::get(r, "expiresAt");
  const double exp = expV.is_string() ? js::dateParse(expV.get_ref<const std::string&>()) : kNaN;
  if (std::isfinite(exp) && now > exp) return refusal("expired: the on-call stopped waiting for it");
  const std::string sid = strOr(r, "sessionId");          // SID_RE = /^[0-9a-f-]{36}$/i
  bool sidOk = sid.size() == 36;
  for (char c : sid) if (!(isHex(c) || c == '-')) sidOk = false;
  if (!sidOk) return refusal("no session id");
  const std::string account = js::trim(strOr(r, "account"));
  if (account.empty()) return refusal("no account");
  const std::string cwd = js::trim(strOr(r, "cwd"));
  if (cwd.empty()) return refusal("no folder");
  std::string prompt;
  { const std::string p = strOr(r, "prompt");             // .replace(/\r\n?/g, '\n').trim()
    for (size_t i = 0; i < p.size(); i++) { if (p[i] == '\r') { prompt += '\n'; if (i + 1 < p.size() && p[i + 1] == '\n') i++; } else prompt += p[i]; }
    prompt = js::trim(prompt); }
  if (prompt.empty()) return refusal("no first message");
  const size_t len = units(prompt);
  if (len > kDeskPromptMax) return refusal("the first message is " + std::to_string(len) + " characters; " + std::to_string(kDeskPromptMax) + " at most");
  const json& titleV = js::get(r, "title");
  const json& modelV = js::get(r, "model");
  const json& bellsV = js::get(r, "bells");
  json bells = json::array();
  if (bellsV.is_array()) for (const json& b : bellsV) { if (bells.size() >= 50) break; bells.push_back(viewerJsString(b)); }
  // .slice(0, n): a cut inside a surrogate pair drops the whole character here (the JS keeps half of it)
  json req = {
    { "id", id }, { "desk", desk },
    { "title", js::sliceUnits(js::trim(js::collapseSpaces(js::truthy(titleV) ? viewerJsString(titleV) : desk + " desk")), 80) },
    { "account", account }, { "accountName", js::trim(strOr(r, "accountName")) }, { "cwd", cwd },
    { "sessionId", lowerAscii(sid) }, { "resume", js::get(r, "resume") == json(true) }, { "prompt", prompt },
    { "note", js::sliceUnits(js::trim(js::collapseSpaces(strOr(r, "note"))), 600) },
    { "model", js::truthy(modelV) ? json(js::sliceUnits(js::trim(viewerJsString(modelV)), 80)) : json() },
    { "chrome", js::get(r, "chrome") == json(true) },
    { "bells", bells },
  };
  return { { "ok", true }, { "req", req } };
}

json deskCheckRequest(const json& raw, const std::string& id, double now) { return checkRequest(raw, id, now, nullptr); }

// accountFor: a config dir as the on-call names it ("C:/Users/demo/.claude-demo") against the deck's accounts
json deskAccountFor(const json& accounts, const std::string& dir, bool win) {
  auto n = [win](const std::string& d) {
    std::string p = viewerPathNormalize(d, win);
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    return viewerLowerCase(p);
  };
  const std::string want = n(dir);
  if (accounts.is_array()) for (const json& a : accounts) if (js::truthy(a) && n(strOr(a, "dir")) == want) return a;
  return json();
}

// openPlan: what to do with a checked request (the deck's openDeskRequest carries it out)
json deskOpenPlan(const json& req, const json& tiles, const json& accounts, const std::function<bool(const std::string&)>& isDir, bool win) {
  const std::string account = templ(req, "account"), cwd = templ(req, "cwd");
  const json acct = deskAccountFor(accounts, js::truthy(js::get(req, "account")) ? viewerJsString(js::get(req, "account")) : std::string(), win);
  if (acct.is_null()) return refusal("no deck account has the folder " + account);
  if (isDir && !isDir(js::str(js::get(req, "cwd")))) return refusal("no such folder: " + cwd);
  const json* t = nullptr;
  if (tiles.is_array()) for (const json& x : tiles) {
    const json& sid = js::get(x, "sessionId");
    if (js::get(x, "kind") == json("claude") && js::truthy(sid) && json(viewerLowerCase(viewerJsString(sid))) == js::get(req, "sessionId")) { t = &x; break; }
  }
  auto put = [](json& o, const char* key, const json& from, const char* fromKey) {      // an undefined value is no key
    if (from.is_object() && from.find(fromKey) != from.end()) o[key] = from[fromKey];
  };
  if (t && js::truthy(js::get(*t, "running"))) {
    if (!js::truthy(js::get(req, "note"))) return refusal("that session already runs in a tile, and the request has no line for it");
    json out = { { "ok", true }, { "action", "type" }, { "text", js::get(req, "note") } };
    put(out, "tileId", *t, "id");
    return out;
  }
  json spec = { { "kind", "claude" } };
  if (t) put(spec, "account", *t, "account"); else put(spec, "account", acct, "id");
  put(spec, "sessionId", req, "sessionId");
  if (t && js::truthy(js::get(*t, "cwd"))) spec["cwd"] = js::get(*t, "cwd"); else put(spec, "cwd", req, "cwd");
  put(spec, "desk", req, "desk"); put(spec, "prompt", req, "prompt"); put(spec, "model", req, "model");
  if (js::truthy(js::get(req, "chrome"))) spec["browser"] = true;
  if (t) { json out = { { "ok", true }, { "action", "wake" }, { "spec", spec } }; put(out, "tileId", *t, "id"); return out; }
  return { { "ok", true }, { "action", "open" }, { "spec", spec }, { "title", "On-call " + templ(req, "title") } };
}

// placedWords: where it landed, in words (the toast and the log)
std::string deskPlacedWords(const json& p) {
  if (!js::truthy(p)) return "its tile";
  const json& cells = js::get(p, "cells");
  std::string cell = "its tile";
  if (cells.is_array() && !cells.empty()) {
    cell = "cell ";
    bool first = true;
    for (const json& k : cells) { if (!first) cell += '+'; first = false; if (!k.is_null()) cell += viewerJsString(k); }
  }
  if (js::truthy(js::get(p, "newWindow"))) return "a new auxiliary window, " + cell;
  return js::truthy(js::get(p, "primary")) ? "the main window, " + cell : "auxiliary window " + templ(p, "window") + ", " + cell;
}

// ---------------------------------------------------------------- the inbox
DeskOpenInbox::DeskOpenInbox(Files& files, Clock& clock, std::string dir, std::function<json(const json& req)> open)
  : files_(files), clock_(clock), dir_(std::move(dir)), open_(std::move(open)) {}

// start(): the folder, and what was already waiting. (The JS also sets an fs.watch and a 1500 ms poll; here the
// shell calls drain() on its own timer.)
void DeskOpenInbox::start() {
  files_.makeDirs(dir_);
  drain();
}

// log(row): one line appended to log.jsonl, best effort. There is no append in Files: read + write.
void DeskOpenInbox::log(const ojson& row) {
  const std::string file = joinPath(dir_, "log.jsonl");
  std::string all;
  if (!files_.read(file, all)) all.clear();
  files_.write(file, all + text(row) + "\n");
}

std::vector<json> DeskOpenInbox::drain() {
  std::vector<json> done;
  if (busy_) return done;                                 // one pass at a time (open() may come back round to drain())
  busy_ = true;
  struct Free { bool& b; ~Free() { b = false; } } freeIt{ busy_ };
  const std::vector<std::string> names = files_.list(dir_);
  static const char kRequest[] = ".request.json";
  for (const std::string& n : names) {
    if (!endsWith(n, kRequest)) continue;                 // /^(.+)\.request\.json$/
    const std::string id = n.substr(0, n.size() - (sizeof kRequest - 1));
    if (!deskOpenIdOk(id)) continue;
    const std::string taken = joinPath(dir_, id + ".taken.json"), ackFile = joinPath(dir_, id + ".ack.json");
    if (!files_.rename(joinPath(dir_, n), taken)) continue;        // withdrawn first, or another pass took it
    ojson raw;                                            // null when it cannot be read or parsed
    { std::string body;
      if (files_.read(taken, body)) {
        body = viewerUtf8(body);                          // fs.readFileSync(…, 'utf8')
        if (body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);
        // one BOM is taken off; JSON.parse refuses a second (nlohmann would skip it)
        if (body.compare(0, 3, "\xEF\xBB\xBF") != 0) raw = ojson::parse(body, nullptr, false);
        if (raw.is_discarded()) raw = nullptr;
        jsKeyOrder(raw);
      } }
    const double now = clock_.nowMs();
    std::string kindText;
    if (raw.is_object() && raw.find("kind") != raw.end()) kindText = stringify(raw["kind"]);
    const json c = checkRequest(json(raw), id, now, &kindText);
    const bool checked = js::truthy(js::get(c, "ok"));
    json res;
    if (!checked) res = refusal(js::str(js::get(c, "error")));
    else {
      try {
        res = open_ ? open_(c["req"]) : json();
        if (!js::truthy(res)) res = refusal("nothing opened");
      } catch (const std::exception& e) {                 // String((e && e.message) || e).slice(0, 300)
        const std::string what = e.what();
        res = refusal(js::sliceUnits(what.empty() ? std::string("Error") : what, 300));
      } catch (...) {
        res = refusal("refused");                         // something that is not a std::exception: no words to give
      }
    }
    const std::string at = isoTime(clock_.nowMs());
    auto orElse = [&](const char* key, const json& otherwise) { const json& v = js::get(res, key); return ojson(js::truthy(v) ? v : otherwise); };
    ojson ack;
    ack["id"] = id;
    if (js::truthy(js::get(res, "ok"))) {
      ack["ok"] = true;
      ack["sessionId"] = orElse("sessionId", json()); ack["tileId"] = orElse("tileId", json()); ack["pid"] = orElse("pid", json());
      ack["window"] = ojson(js::get(res, "window"));      // res.window ?? null
      ack["primary"] = js::truthy(js::get(res, "primary"));
      ack["cells"] = orElse("cells", json::array());
      ack["newWindow"] = js::truthy(js::get(res, "newWindow"));
      ack["how"] = orElse("how", json("opened"));
    } else {
      ack["ok"] = false;
      ack["error"] = orElse("error", json("refused"));
    }
    ack["at"] = at;
    jsNumbers(ack);
    // writeWhole: a temp beside it, renamed in (the on-call reads the answer as soon as it appears)
    if (files_.write(ackFile + ".tmp", text(ack))) files_.rename(ackFile + ".tmp", ackFile);
    files_.remove(taken);
    const bool ok = ack["ok"].get<bool>();
    ojson row;
    row["at"] = isoTime(clock_.nowMs());
    row["id"] = id;
    if (checked) row["desk"] = ojson(c["req"]["desk"]);
    else { const auto it = raw.is_object() ? raw.find("desk") : raw.end(); row["desk"] = raw.is_object() && it != raw.end() && js::truthy(json(*it)) ? *it : ojson(); }
    row["account"] = checked ? ojson(js::truthy(c["req"]["accountName"]) ? c["req"]["accountName"] : c["req"]["account"]) : ojson();
    row["ok"] = ok;
    if (ok) { row["how"] = ack["how"]; row["where"] = deskPlacedWords(json(ack)); row["tile"] = ack["tileId"]; row["pid"] = ack["pid"]; }
    else row["error"] = ack["error"];
    jsNumbers(row);
    log(row);
    done.push_back(json(ack));
  }
  prune(names);
  return done;
}

// prune: answers nobody read, withdrawn requests and stranded taken files older than a day
void DeskOpenInbox::prune(const std::vector<std::string>& names) {
  const double now = clock_.nowMs();
  for (const std::string& n : names) {
    if (!endsWith(n, ".ack.json") && !endsWith(n, ".withdrawn.json") && !endsWith(n, ".taken.json")) continue;
    const std::string p = joinPath(dir_, n);
    const FileStat st = files_.stat(p);
    if (st.exists && !st.isDir && now - st.mtimeMs > kDeskPruneMs) files_.remove(p);
  }
}

}}  // namespace deck::core
