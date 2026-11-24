// core/deck/asks.cpp — lib/asks.js, ported (see asks.hpp). Each function names the JS function it ports.
//
// How the JS's values are held here:
//   - a session's state file (the skill's) is parsed into nlohmann::ordered_json, so the order the skill wrote its
//     keys in survives: Object.keys(inherits) (the `replaces` rows) and Object.entries(inherits[old]) (which card
//     moves first, the order of a DRAFT copy's `moves`). JS puts integer-like keys ("1", "2") first, in numeric
//     order; here they stay in file order (the skill's keys are page ids and Q-ids, never integers).
//   - the deck's own drafts / sent / log / moved are nlohmann::json (keys sorted): they arrive from callers as
//     `json`, their order already lost. So where the JS walks Object.keys(this.drafts) / Object.entries(this.sent)
//     in insertion order, this walks them by id: DRAFT copies are written, and onRead called, in id order within
//     one scan. A DRAFT copy of a page whose round is GONE lists its cards by qid, not in the order they typed them.
//   - JS `undefined` is a null POINTER here (prop() of a missing key), JSON null a pointer to a null value; a key
//     the JS would set to undefined is left out, as JSON.stringify leaves it out.
// Deliberate differences from the JS, all in input the skill never writes:
//   - a null where an object is expected (a round's item, an option) reads as "nothing there"; the JS throws a
//     TypeError out of scan().
//   - JSON.parse accepts lone surrogate escapes and Node reads ill-formed UTF-8 as U+FFFD; here such a file does
//     not parse (it is kept as last read, like a mid-write file). A file starting with a BOM fails in both.
//   - String.localeCompare (ICU collation) on the `created` stamps is a plain byte comparison: the same order for
//     the ISO stamps the skill writes (same shape, ASCII digits).
//   - Date.parse(read_at) is js::dateParse: ISO 8601 only; a stamp with no zone is read in the PROCESS's zone.
//   - errors the JS logs with console.error are swallowed.
#include "asks.hpp"
#include "edition.hpp"
#include "util.hpp"
#include "../session/jsutil.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>

namespace deck { namespace core {

namespace {

using OJ = nlohmann::ordered_json;
namespace js = deck::session::js;

const double KEEP_SENT_MS = 6 * 3600 * 1000.0;   // a Done the session never read is shown as sent this long
const size_t DRAFT_MAX = 200000;                 // UTF-16 units of one page's draft state, as JSON
const size_t LOG_MAX = 50;                       // answered pages kept for the read-only look back
const double LOG_DAYS = 14;                      // a page read longer ago than this is not added to the log on its own
const double HOLD_REPLACED_MS = 10 * 60000.0;    // a replaced page's drafts wait this long for the page that replaces it

const auto kReplace = nlohmann::detail::error_handler_t::replace;
template <class J> std::string dumpCompact(const J& v) { return v.dump(-1, ' ', false, kReplace); }   // JSON.stringify(v)
template <class J> std::string dumpOne(const J& v) { return v.dump(1, ' ', false, kReplace); }        // JSON.stringify(v, null, 1)

// ---- JavaScript values
// o.key: null pointer = undefined (a missing key, or `o` is not an object)
template <class J> const J* prop(const J* o, const std::string& key) {
  if (!o || !o->is_object()) return nullptr;
  auto it = o->find(key);
  return it == o->end() ? nullptr : &*it;
}
template <class J> bool truthy(const J* v) {
  if (!v) return false;
  if (v->is_null() || v->is_discarded()) return false;
  if (v->is_boolean()) return v->template get<bool>();
  if (v->is_string()) return !v->template get_ref<const std::string&>().empty();
  if (v->is_number()) { const double d = v->template get<double>(); return d != 0 && !std::isnan(d); }
  return true;
}
// String(v), `${v}`
template <class J> std::string tpl(const J* v) {
  if (!v) return "undefined";
  if (v->is_string()) return v->template get<std::string>();
  if (v->is_null()) return "null";
  if (v->is_boolean()) return v->template get<bool>() ? "true" : "false";
  if (v->is_number_unsigned()) return std::to_string(v->template get<uint64_t>());
  if (v->is_number_integer()) return std::to_string(v->template get<int64_t>());
  if (v->is_number()) return js::numberText(v->template get<double>());
  if (v->is_array()) {                                      // Array.prototype.join(','): null prints as nothing
    std::string s; bool first = true;
    for (const J& e : *v) { if (!first) s += ','; first = false; if (!e.is_null()) s += tpl(&e); }
    return s;
  }
  return "[object Object]";
}
template <class J> std::string strOr(const J* v) { return truthy(v) ? tpl(v) : std::string(); }   // String(v || '')
template <class J> bool isStr(const J* v, const char* s) { return v && v->is_string() && v->template get_ref<const std::string&>() == s; }
// +v: NaN for undefined and for what is not a number
template <class J> double num(const J* v) {
  if (!v) return std::numeric_limits<double>::quiet_NaN();
  if (v->is_number()) return v->template get<double>();
  if (v->is_boolean()) return v->template get<bool>() ? 1 : 0;
  if (v->is_null()) return 0;
  if (v->is_string()) return js::toNumber(json(v->template get<std::string>()));
  return std::numeric_limits<double>::quiet_NaN();
}
// a === b for JSON values: two objects or arrays are never the same one
template <class A, class B> bool strictEq(const A* a, const B* b) {
  if (!a || !b) return !a && !b;
  if (a->is_string() && b->is_string()) return a->template get_ref<const std::string&>() == b->template get_ref<const std::string&>();
  if (a->is_number() && b->is_number()) return a->template get<double>() == b->template get<double>();
  if (a->is_boolean() && b->is_boolean()) return a->template get<bool>() == b->template get<bool>();
  return a->is_null() && b->is_null();
}
// a number the deck made, as JSON.stringify prints it: whole numbers without ".0"
json numJson(double d) {
  if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9007199254740992.0) return json((int64_t)d);
  return json(d);
}
size_t units(const std::string& s) {                         // s.length
  size_t n = 0;
  for (size_t i = 0; i < s.size();) n += js::nextCodePoint(s, i) >= 0x10000 ? 2 : 1;
  return n;
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
bool endsWith(const std::string& s, const char* p) { const size_t n = std::strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }
// JSON.parse(fs.readFileSync(f, 'utf8')): false where it throws. nlohmann skips a BOM; JSON.parse does not.
template <class J> bool parseJs(const std::string& text, J& out) {
  if (startsWith(text, "\xEF\xBB\xBF")) return false;
  out = J::parse(text, nullptr, false);
  return !out.is_discarded();
}

// ---- paths (path.win32 when `win`, else path.posix)
bool isSep(char c, bool win) { return c == '/' || (win && c == '\\'); }
// the root of a path: its text as path.resolve writes it, where the rest starts, and whether it is absolute
void parseRoot(const std::string& p, bool win, std::string& root, size_t& rest, bool& abs) {
  const size_t n = p.size();
  root.clear(); rest = 0; abs = false;
  if (!win) { if (n && p[0] == '/') { root = "/"; rest = 1; abs = true; } return; }
  const bool letter = n >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
  if (letter) {
    root = p.substr(0, 2); rest = 2;
    if (n > 2 && isSep(p[2], true)) { root += '\\'; rest = 3; abs = true; }
    return;
  }
  if (n >= 2 && isSep(p[0], true) && isSep(p[1], true)) {     // \\server\share
    size_t a = 2;
    while (a < n && !isSep(p[a], true)) a++;
    if (a < n && a > 2) {
      size_t b = a;
      while (b < n && isSep(p[b], true)) b++;
      if (b < n) {
        size_t c = b;
        while (c < n && !isSep(p[c], true)) c++;
        root = "\\\\" + p.substr(2, a - 2) + "\\" + p.substr(b, c - b) + "\\"; rest = c; abs = true;
        return;
      }
    }
  }
  if (n && isSep(p[0], true)) { root = "\\"; rest = 1; abs = true; }   // rooted on the current drive, which is not known here
}
// path.resolve(p) for an ABSOLUTE path (separators made the platform's, `.` and `..` and doubled separators
// gone, no trailing separator). A relative path is normalised and stays relative: the JS resolves it against
// process.cwd(), which the core does not have (the skill writes absolute paths).
std::string normPath(const std::string& p, bool win) {
  std::string root; size_t i; bool abs;
  parseRoot(p, win, root, i, abs);
  std::vector<std::string> segs;
  while (i <= p.size()) {
    size_t j = i;
    while (j < p.size() && !isSep(p[j], win)) j++;
    const std::string seg = p.substr(i, j - i);
    if (seg == "..") { if (!segs.empty() && segs.back() != "..") segs.pop_back(); else if (!abs) segs.push_back(seg); }
    else if (!seg.empty() && seg != ".") segs.push_back(seg);
    i = j + 1;
  }
  std::string out = root;
  for (size_t k = 0; k < segs.size(); k++) { if (k) out += win ? '\\' : '/'; out += segs[k]; }
  return out;
}
std::string dirnameP(const std::string& p, bool win) {       // path.dirname of a normalised path
  std::string root; size_t rest; bool abs;
  parseRoot(p, win, root, rest, abs);
  size_t idx = std::string::npos;
  for (size_t k = p.size(); k-- > 0;) if (isSep(p[k], win)) { idx = k; break; }
  if (idx == std::string::npos) return root.empty() ? "." : root;
  if (idx < root.size()) return p.substr(0, root.size());
  return p.substr(0, idx);
}
std::string basenameP(const std::string& p, bool win) {      // path.basename
  size_t end = p.size();
  while (end > 0 && isSep(p[end - 1], win)) end--;
  size_t start = 0;
  if (win && end >= 2 && p[1] == ':') start = 2;
  for (size_t k = end; k-- > start;) if (isSep(p[k], win)) { start = k + 1; break; }
  return p.substr(start, end - start);
}
std::string joinP(const std::string& dir, const std::string& name, bool win) {   // path.join(dir, name)
  const std::string d = normPath(dir, win);
  if (d.empty()) return name;
  return isSep(d.back(), win) ? d + name : d + (win ? '\\' : '/') + name;
}
// expand: String(p || '').replace(/^~(?=[\\/]|$)/, home)
std::string expandHome(const std::string& p, const std::string& home) {
  if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '\\' || p[1] == '/')) return home + p.substr(1);
  return p;
}

// ---- pathToFileURL
// What Node 24 percent-encodes in a file URL's path: controls, space, " # % < > ? [ ] ^ ` { | } ~, DEL and every
// byte of a non-ASCII character (UTF-8); a backslash too on posix (on Windows it is a separator).
void encodeUrlPath(const std::string& p, bool win, std::string& out) {
  static const char* hex = "0123456789ABCDEF";
  for (unsigned char c : p) {
    if (win && c == '\\') { out += '/'; continue; }
    const bool enc = c <= 0x20 || c >= 0x7f || std::strchr("\"#%<>?[]^`{|}~", c) != nullptr || c == '\\';
    if (enc) { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; } else out += (char)c;
  }
}
// The URL parser's host for a UNC server name. Covered: ASCII names (lower-cased; tab / CR / LF dropped; cut at
// / ? #); a forbidden character is Node's throw (false). NOT covered: a non-ASCII name (Node writes punycode;
// here it is refused, so the view's url is null) and a name the parser reads as an IPv4 address.
bool urlHost(const std::string& name, std::string& host) {
  host.clear();
  for (unsigned char c : name) {
    if (c == '\t' || c == '\n' || c == '\r') continue;
    if (c == '/' || c == '?' || c == '#') break;
    if (c <= 0x20 || c >= 0x7f || std::strchr("%:<>@[]^|\\", c) != nullptr) return false;
    host += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (char)c;
  }
  return true;
}
// url.pathname = <path>: the URL parser's `.` and `..` segments (a `%2e` cannot arrive: `%` was encoded first)
std::string urlDotSegments(const std::string& path) {
  std::vector<std::string> segs;
  size_t i = !path.empty() && path[0] == '/' ? 1 : 0;
  const auto driveLetter = [](const std::string& s) { return s.size() == 2 && s[1] == ':' && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')); };
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    const bool last = j == std::string::npos;
    if (last) j = path.size();
    const std::string seg = path.substr(i, j - i);
    if (seg == "..") {
      if (!segs.empty() && !(segs.size() == 1 && driveLetter(segs[0]))) segs.pop_back();
      if (last) segs.push_back("");
    } else if (seg == ".") { if (last) segs.push_back(""); }
    else segs.push_back(seg);
    i = j + 1;
  }
  std::string out;
  for (const std::string& s : segs) { out += '/'; out += s; }
  return out.empty() ? "/" : out;
}

// ---- ID_RE = /^ask_[A-Za-z0-9-]+_[0-9a-f]{8}_r\d+_\d{4}-\d{4}_[0-9a-f]{4}$/
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHexLower(char c) { return isDigit(c) || (c >= 'a' && c <= 'f'); }

// ---- the cards
// entered: a card's draft has something they entered: a pick (an option or a reply button), words, or "later"
template <class J> bool entered(const J* s) {
  if (!truthy(s)) return false;
  return truthy(prop(s, "pick")) || !js::trim(strOr(prop(s, "text"))).empty() || truthy(prop(s, "later"));
}
// REPLY_FILL: the reply buttons' words and statuses (the skill's REPLIES)
struct Reply { const char* code; const char* words; const char* status; };
const Reply kReplies[] = {
  { "explain", "explain more", "later" }, { "later", "later", "later" }, { "check", "check first", "answered" }, { "both", "both", "answered" },
  { "all", "all of them", "answered" }, { "drop", "drop it", "dropped" }, { "yourcall", "your call", "answered" }, { "remind", "remind me later", "later" },
  { "workaround", "work around it for now", "answered" }, { "after", "after the others", "later" }, { "none", "", "answered" } };
struct Ans { bool ok = false; std::string status, answer; };
// draftAnswer: one card's draft -> { status, answer } or nothing (the skill's answer_from_state)
template <class I, class S> Ans draftAnswer(const I* it, const S* s) {
  Ans none;
  if (!truthy(s)) return none;
  const std::string text = js::trim(strOr(prop(s, "text")));
  const S* pick = prop(s, "pick");
  if (pick && pick->is_array() && pick->empty()) pick = nullptr;
  if (truthy(prop(s, "later")) && !truthy(pick)) return { true, "later", text.empty() ? "later" : text };
  if (pick && pick->is_string() && startsWith(pick->template get_ref<const std::string&>(), "r:")) {
    const std::string code = pick->template get_ref<const std::string&>().substr(2);
    const Reply* r = nullptr;
    for (const Reply& k : kReplies) if (code == k.code) r = &k;
    if (!r || (code == "none" && text.empty())) return text.empty() ? none : Ans{ true, "answered", text };
    return { true, r->status, !text.empty() ? text : (r->words[0] ? std::string(r->words) : code) };
  }
  if (truthy(pick)) {
    if (!text.empty()) return { true, "answered", text };
    const I* options = prop(it, "options");
    std::string out; bool first = true;
    const auto one = [&](const S& k) {
      const std::string key = tpl(&k);
      const I* label = nullptr;                              // new Map(...): the last option with that key
      if (options && options->is_array()) for (const I& o : *options) if (o.is_object() && tpl(prop(&o, "key")) == key) label = prop(&o, "label");
      if (!first) out += ", ";
      first = false;
      out += truthy(label) ? tpl(label) : key;
    };
    if (pick->is_array()) { for (const S& k : *pick) one(k); } else one(*pick);
    return { true, "answered", out };
  }
  return text.empty() ? none : Ans{ true, "answered", text };
}
// lib/asks.js titleKey: String(i.title || '') with spaces collapsed, trimmed, lower case (ASCII letters only here)
template <class J> std::string titleKey(const J* i) {
  const J* t = i ? prop(i, "title") : nullptr;
  std::string s = js::trim(js::collapseSpaces(truthy(t) ? tpl(t) : std::string()));
  for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
  return s;
}
// sameOptions: the same options on two cards: same keys, same labels, same order (a carried card, not a rewritten one)
template <class J> bool sameOptions(const J* a, const J* b) {
  const J* oa = prop(a, "options"); const J* ob = prop(b, "options");
  if (!oa || !oa->is_array() || !ob || !ob->is_array() || oa->size() != ob->size()) return false;
  if (truthy(prop(a, "multi")) != truthy(prop(b, "multi"))) return false;
  for (size_t i = 0; i < oa->size(); i++) {
    const J* o = &(*oa)[i]; const J* p = &(*ob)[i];
    if (!truthy(o) || !truthy(p)) return false;
    if (!strictEq(prop(o, "key"), prop(p, "key")) || strOr(prop(o, "label")) != strOr(prop(p, "label"))) return false;
  }
  return true;
}

// stripAnsi: .replace(/\x1b\[[0-9;?<=>]*[A-Za-z]/g, ' ').replace(/\x1b\][^\x07]*\x07/g, '').replace(/\s+/g, ' ')
std::string stripAnsi(const std::string& s) {
  std::string a;
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < s.size() && (isDigit(s[j]) || (s[j] && std::strchr(";?<=>", s[j]) != nullptr))) j++;
      if (j < s.size() && ((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z'))) { a += ' '; i = j + 1; continue; }
    }
    a += s[i++];
  }
  std::string b;
  for (size_t i = 0; i < a.size();) {
    if (a[i] == '\x1b' && i + 1 < a.size() && a[i + 1] == ']') {
      const size_t bel = a.find('\x07', i + 2);
      if (bel != std::string::npos) { i = bel + 1; continue; }
    }
    b += a[i++];
  }
  return js::collapseSpaces(b);
}

}  // namespace

// ============================================================================================ the pure functions

bool askIdOk(const std::string& s) {
  size_t i = 0; const size_t n = s.size();
  const auto lit = [&](const char* p) { const size_t k = std::strlen(p); if (s.compare(i, k, p) != 0) return false; i += k; return true; };
  const auto run = [&](bool (*ok)(char), size_t min, size_t max) { size_t k = 0; while (i < n && k < max && ok(s[i])) { i++; k++; } return k >= min; };
  const auto name = [](char c) { return isDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-'; };
  const size_t any = std::string::npos;
  return lit("ask_") && run(name, 1, any) && lit("_") && run(isHexLower, 8, 8) && lit("_r") && run(isDigit, 1, any) && lit("_")
    && run(isDigit, 4, 4) && lit("-") && run(isDigit, 4, 4) && lit("_") && run(isHexLower, 4, 4) && i == n;
}

// composeDone: each page's Copy-all lines, headed by the page id on a line of its own; one block per page
std::string composeDone(const json& pages) {
  std::string out; bool first = true;
  if (!pages.is_array()) return out;
  for (const json& p : pages) {
    const json* lines = prop(&p, "lines");
    if (!truthy(&p) || !lines || !lines->is_array()) continue;
    bool some = false;
    for (const json& l : *lines) if (!js::trim(tpl(&l)).empty()) { some = true; break; }
    if (!some) continue;
    if (!first) out += '\n';
    first = false;
    const json* id = prop(&p, "id");
    if (id && !id->is_null()) out += tpl(id);                // [p.id, ...].join('\n'): null and undefined print as nothing
    for (const json& l : *lines) { out += '\n'; out += tpl(&l); }
  }
  return out;
}

// composePointer: the ONE line Done sends into a session, naming the answers file(s)
std::string composePointer(const json& pages) {
  std::vector<const json*> withFile;
  if (pages.is_array()) for (const json& p : pages) if (truthy(&p) && truthy(prop(&p, "id")) && truthy(prop(&p, "file"))) withFile.push_back(&p);
  if (withFile.empty()) return "";
  const size_t n = withFile.size();
  std::string out = "My answers are saved in " + (n == 1 ? std::string("a file") : std::to_string(n) + " files") + " - read " + (n == 1 ? "it" : "them")
    + " with ask_page.py read <page id>. ";
  for (size_t i = 0; i < n; i++) {
    const json* p = withFile[i];
    if (i) out += "; ";
    out += tpl(prop(p, "id"));
    if (truthy(prop(p, "total"))) {
      out += " (" + (truthy(prop(p, "answered")) ? tpl(prop(p, "answered")) : std::string("0")) + " of " + tpl(prop(p, "total")) + " answered";
      if (truthy(prop(p, "later"))) out += ", " + tpl(prop(p, "later")) + " later";
      out += ")";
    }
    out += ": " + tpl(prop(p, "file"));
  }
  return out;
}

// pickerOnScreen: is the session's own question picker on its screen right now?
// recent.slice(-65536) and head.slice(0, 40) are in UTF-16 units; a cut inside a surrogate pair drops the whole
// character here (the JS keeps half of it).
bool pickerOnScreen(const std::string& recent, const std::vector<std::string>& questions) {
  size_t start = 0;
  const size_t total = units(recent);
  if (total > 65536) {
    size_t skip = total - 65536, i = 0, u = 0;
    while (i < recent.size() && u < skip) u += js::nextCodePoint(recent, i) >= 0x10000 ? 2 : 1;
    start = i;
  }
  const std::string screen = stripAnsi(start ? recent.substr(start) : recent);
  for (const std::string& q : questions) {
    const std::string head = js::sliceUnits(js::trim(stripAnsi(q)), 40);
    if (units(head) >= 8 && screen.find(head) != std::string::npos) return true;
  }
  return false;
}

// newAskRows: which ask pages are NEW since the last look
json newAskRows(const json& seen, const json& view) {
  json ids = json::array(), fresh = json::array();
  const bool first = !seen.is_array();                       // the JS's `seen` is a Set, or null on the first call
  const auto has = [](const json& list, const json& id) { for (const json& x : list) if (x == id) return true; return false; };
  if (view.is_array()) for (const json& s : view) {
    json rows = json::array();
    const json* rounds = prop(&s, "rounds");
    if (rounds && rounds->is_array()) for (const json& r : *rounds) {
      const json* idp = prop(&r, "id");
      const json id = idp ? *idp : json();
      if (!has(ids, id)) ids.push_back(id);
      if (!first && !has(seen, id) && !truthy(prop(&r, "background")) && !truthy(prop(&r, "sent"))) rows.push_back(r);
    }
    if (!rows.empty()) fresh.push_back({ { "session", s }, { "rounds", rows } });
  }
  return { { "seen", ids }, { "fresh", fresh } };
}

// url.pathToFileURL(path).href
std::string pathToFileUrl(const std::string& path, bool win) {
  std::string out = "file://";
  if (win && startsWith(path, "\\\\")) {
    // UNC: \\server\share\resource; the "\\?\UNC\" prefix is ignored
    const size_t prefix = startsWith(path, "\\\\?\\UNC\\") ? 8 : 2;
    const size_t end = path.find('\\', prefix);
    if (end == std::string::npos || end == 2) return "";    // "Missing UNC resource path" / "Empty UNC servername"
    std::string host;
    if (!urlHost(path.substr(prefix, end - prefix), host)) return "";
    std::string enc;
    encodeUrlPath(path.substr(end), true, enc);
    return out + host + urlDotSegments(enc);
  }
  std::string resolved = normPath(path, win);
  const bool unc = win && startsWith(resolved, "\\\\");
  // path.resolve strips trailing slashes so they are added back (Node 24 loses the one of a //server/share/x/)
  const char lastCh = path.empty() ? 0 : path.back();
  if (!unc && isSep(lastCh, win) && !resolved.empty() && resolved.back() != (win ? '\\' : '/')) resolved += '/';
  if (unc) {                                                 // //server/share/x, resolved to a UNC path
    const size_t end = resolved.find('\\', 2);
    std::string host;
    if (end == std::string::npos || !urlHost(resolved.substr(2, end - 2), host)) return "";
    encodeUrlPath(resolved.substr(end), true, out += host);
    return out;
  }
  std::string enc;
  encodeUrlPath(resolved, win, enc);
  if (enc.empty() || enc[0] != '/') out += '/';
  return out + enc;
}

// locations() + skillSettings()
AskLocations askLocations(Files& files, Env& env) {
  const std::string home = env.home();
  const bool win = env.os() == "win32";
  json s = json::object();
  // the account folders (core/deck/edition.hpp): the private edition's named ones, or ~/.claude then every ~/.claude-* folder
  std::vector<std::string> homes(std::begin(edition::kAskSkillHomes), std::end(edition::kAskSkillHomes));
  if (edition::kAskScanAccounts)
    for (const std::string& n : files.list(home))
      if (n.compare(0, 8, ".claude-") == 0 && n.size() > 8 && std::find(homes.begin(), homes.end(), n) == homes.end()) homes.push_back(n);
  for (const std::string& d : homes) {
    std::string text; json j;
    const std::string f = joinP(joinP(joinP(joinP(home, d, win), "skills", win), "ask-page", win), "settings.json", win);
    if (files.read(f, text) && parseJs(text, j)) { s = j; break; }
  }
  const auto resolve = [&](const std::string& p) { const std::string r = env.resolvePath(expandHome(p, home)); return r.empty() ? r : normPath(r, win); };
  AskLocations out;
  const std::string envState = env.get("ASK_PAGE_STATE_DIR"), envDl = env.get("ASK_PAGE_DOWNLOADS");
  const json* sd = prop(&s, "state_dir");
  out.stateDir = resolve(!envState.empty() ? envState : truthy(sd) ? tpl(sd) : std::string("~/.local/state/ask-pages"));
  std::vector<std::string> dl;
  const json* sdl = prop(&s, "downloads");
  if (!envDl.empty()) {
    const char delim = win ? ';' : ':';
    size_t i = 0;
    for (;;) { const size_t j = envDl.find(delim, i); dl.push_back(envDl.substr(i, j == std::string::npos ? j : j - i)); if (j == std::string::npos) break; i = j + 1; }
  } else if (sdl && sdl->is_array() && !sdl->empty()) {
    for (const json& d : *sdl) dl.push_back(strOr(&d));
  } else dl.assign(std::begin(edition::kAskDownloads), std::end(edition::kAskDownloads));
  for (const std::string& d : dl) out.downloads.push_back(resolve(d));
  return out;
}

// ============================================================================================ AskFeed

namespace {
struct Sess { std::string sid; double mtimeMs = 0; uint64_t size = 0; OJ data; };
// one of the skill's rounds, as rounds() lists it. The pointers are into a Sess's data: good until the next scan.
struct RX {
  std::string sid; const OJ* state = nullptr; const OJ* round = nullptr; std::vector<const OJ*> items;
  int answered = 0, later = 0; bool waiting = false; std::string id;
};
struct Card { std::string oq; OJ to; bool moved = false; bool hasWhy = false; std::string why; };
struct Report { std::string to; std::vector<Card> cards; };
const RX* findRound(const std::vector<RX>& all, const std::string& id) { for (const RX& x : all) if (x.id == id) return &x; return nullptr; }
using ById = std::map<std::string, const RX*>;
ById indexRounds(const std::vector<RX>& all) { ById m; for (const RX& x : all) m[x.id] = &x; return m; }   // new Map(...): the last one wins
const RX* byIdGet(const ById& m, const OJ* id) {
  if (!id || !id->is_string()) return nullptr;
  auto it = m.find(id->get_ref<const std::string&>());
  return it == m.end() ? nullptr : it->second;
}
json sessionIdOf(const RX& x) { const OJ* v = prop(x.state, "session_id"); return truthy(v) ? json(*v) : json(x.sid); }   // x.state.session_id || x.sid
json sessionNameOf(const RX& x) { const OJ* v = prop(x.state, "session_name"); return truthy(v) ? json(*v) : json("session"); }
}  // namespace

struct AskFeed::Impl {
  AskDeps deps;
  std::vector<Sess> sessions;                  // this.sessions: in the order each session was first seen
  json drafts = json::object();                // page id -> { state, drafted, answered, later, total, at }
  json sent = json::object();                  // page id -> { at, how, tileId?, file?, heldId? }
  json log = json::array();                    // newest first
  json moved = json::object();                 // newer page id -> { older page id: { n, at } }
  bool liveKnown = false; std::set<std::string> live; double liveAt = 0;
  std::map<std::string, double> orphanAt;      // replaced page id -> when it was first seen with nowhere to move its drafts
  std::map<std::string, Report> report;        // replaced page id -> what migrate did with each card
  std::string sig;
  bool pending = false; uint64_t gen = 0; int depth = 0;
  bool guest = false;                          // another deck's fresh heartbeat is in deck.json: this deck writes nothing shared
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  double now() { return deps.clock ? deps.clock->nowMs() : 0; }
  Files& files() { return *deps.files; }

  // a public call that may change what ask-drafts.json holds: with no `later`, the file is written as it returns
  struct Call {
    Impl* f;
    explicit Call(Impl* impl) : f(impl) { f->depth++; }
    ~Call() { if (--f->depth == 0 && !f->deps.later && f->pending) f->writeDrafts(); }
  };

  // ---- drafts file
  void loadDrafts() {
    std::string text; json j;
    if (!deps.files || deps.draftsFile.empty() || !files().read(deps.draftsFile, text) || !parseJs(text, j)) return;
    const json* v;
    if ((v = prop(&j, "drafts")) && v->is_object()) drafts = *v;
    if ((v = prop(&j, "sent")) && v->is_object()) sent = *v;
    if ((v = prop(&j, "log")) && v->is_array()) for (const json& e : *v) {
      if (log.size() >= LOG_MAX) break;
      const json* id = prop(&e, "id");
      if (id && id->is_string() && askIdOk(id->get_ref<const std::string&>())) log.push_back(e);
    }
    if ((v = prop(&j, "moved")) && v->is_object()) moved = *v;
  }
  void writeDrafts() {
    pending = false;
    if (deps.draftsFile.empty()) return;
    files().writeAtomic(deps.draftsFile, dumpCompact(json{ { "drafts", drafts }, { "sent", sent }, { "log", log }, { "moved", moved } }));
  }
  void saveDrafts() {
    if (deps.draftsFile.empty()) return;
    pending = true;
    const uint64_t mine = ++gen;                             // clearTimeout: an older timer finds a newer save and does nothing
    if (!deps.later) return;                                 // written when the outermost call returns (Call)
    std::shared_ptr<bool> a = alive;
    deps.later(300, [this, a, mine]() { if (*a && mine == gen && pending) writeDrafts(); });
  }

  // ---- reading the skill's state files
  std::vector<RX> rounds() const {
    std::vector<RX> out;
    for (const Sess& s : sessions) {
      const OJ* d = &s.data;
      const OJ* rs = prop(d, "rounds");
      if (!rs || !rs->is_array()) continue;
      const OJ* answers = prop(d, "answers");
      for (const OJ& r : *rs) {
        const OJ* id = prop(&r, "id");
        if (!id || !id->is_string() || !askIdOk(id->get_ref<const std::string&>())) continue;
        RX x; x.sid = s.sid; x.state = d; x.round = &r; x.id = id->get<std::string>();
        const OJ* items = prop(&r, "items");
        if (items && items->is_array()) for (const OJ& it : *items) {
          x.items.push_back(&it);
          const OJ* a = truthy(answers) ? prop(answers, tpl(prop(&it, "key"))) : nullptr;
          const OJ* st = truthy(a) ? prop(a, "status") : nullptr;
          if (isStr(st, "answered") || isStr(st, "dropped")) x.answered++;
          else if (isStr(st, "later")) x.later++;
        }
        // waiting on them: open, or partly answered and not yet read
        const OJ* status = prop(&r, "status");
        x.waiting = isStr(status, "open") || (isStr(status, "partial") && !truthy(prop(&r, "read_at")));
        out.push_back(std::move(x));
      }
    }
    return out;
  }

  const std::set<std::string>* liveSet() {
    if (!deps.liveSessions) return nullptr;
    const double t = now();
    if (!liveKnown || t - liveAt > 15000) {
      live.clear();
      liveKnown = deps.liveSessions(live);
      if (!liveKnown) live.clear();
      liveAt = t;
    }
    return liveKnown ? &live : nullptr;
  }

  void scan() {
    Call call(this);
    std::set<std::string> seen;
    for (const std::string& n : files().list(deps.stateDir)) {
      if (!endsWith(n, ".json") || n == "deck.json" || n[0] == '.') continue;
      const std::string f = joinP(deps.stateDir, n, deps.win);
      const FileStat st = files().stat(f);
      if (!st.exists) continue;
      const std::string sid = n.substr(0, n.size() - 5);
      seen.insert(sid);
      Sess* had = nullptr;
      for (Sess& s : sessions) if (s.sid == sid) had = &s;
      if (had && had->mtimeMs == st.mtimeMs && had->size == st.size) continue;
      std::string text; OJ j;
      if (st.isDir || !files().read(f, text) || !parseJs(text, j)) continue;   // mid-write: the next scan reads it
      const OJ* rs = prop(&j, "rounds");
      if (!rs || !rs->is_array()) continue;
      if (had) { had->mtimeMs = st.mtimeMs; had->size = st.size; had->data = std::move(j); }
      else { Sess s; s.sid = sid; s.mtimeMs = st.mtimeMs; s.size = st.size; s.data = std::move(j); sessions.push_back(std::move(s)); }
    }
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [&](const Sess& s) { return !seen.count(s.sid); }), sessions.end());
    if (!guest) prune();                       // a guest reads; the deck that serves the pages prunes, migrates and saves DRAFT copies
    // the signature: what a view shows of the asks (any text that changes when the JS's does)
    json parts = json::array();
    for (const json& s : view()) {
      json rs = json::array();
      for (const json& r : s["rounds"]) {
        json mv = json::array();
        for (const json& p : r["replaces"]) mv.push_back(p.value("moved", json()));
        rs.push_back({ r["id"], r.value("status", json()), r["answered"], r["later"], r["drafted"], truthy(&r["sent"]), mv });
      }
      parts.push_back({ s["sessionId"], s["ended"], rs });
    }
    json logSig = json::array();
    for (const json& e : log) logSig.push_back(tpl(prop(&e, "id")) + tpl(prop(&e, "at")));
    const std::string next = dumpCompact(json::array({ parts, logSig }));
    if (next != sig) { sig = next; if (deps.onChange) deps.onChange(); }
  }

  // prune: drafts and sent marks of pages that no longer wait (answered, carried, gone) are dropped — a REPLACED
  // page's drafts first move onto the page that replaced it (migrate)
  void prune() {
    logRead();
    const std::vector<RX> all = rounds();
    std::set<std::string> waiting, readAt;
    for (const RX& x : all) { if (x.waiting) waiting.insert(x.id); if (truthy(prop(x.round, "read_at"))) readAt.insert(x.id); }
    bool dirty = migrate(all);
    // a page marked replaced whose newer page (or its inherits map) is not there yet: another state file still
    // being written. Its drafts wait up to HOLD_REPLACED_MS for somewhere to go before they are dropped.
    const ById byId = indexRounds(all);
    std::set<std::string> hold;
    for (const RX& x : all) {
      const OJ* by = prop(x.round, "superseded_by");
      if (x.waiting || !truthy(by) || !truthy(prop(&drafts, x.id))) continue;
      const RX* to = byIdGet(byId, by);
      const OJ* inh = to ? prop(to->round, "inherits") : nullptr;
      if (to && truthy(inh) && truthy(prop(inh, x.id))) continue;          // moved (or nothing of their to move)
      double& since = orphanAt[x.id];
      if (!since) since = now();
      if (now() - since < HOLD_REPLACED_MS) hold.insert(x.id);
    }
    for (auto it = orphanAt.begin(); it != orphanAt.end();) { if (!truthy(prop(&drafts, it->first)) || waiting.count(it->first)) it = orphanAt.erase(it); else ++it; }
    std::vector<std::string> ids;
    for (auto it = drafts.begin(); it != drafts.end(); ++it) ids.push_back(it.key());
    for (const std::string& id : ids) {
      if (waiting.count(id) || hold.count(id)) continue;
      // what they entered and never sent is saved as a DRAFT copy next to its page before it goes
      if (!truthy(prop(&sent, id))) {
        auto x = byId.find(id); auto rep = report.find(id);
        saveDraftCopy(id, x == byId.end() ? nullptr : x->second, rep == report.end() ? nullptr : &rep->second);
      }
      drafts.erase(id); report.erase(id); dirty = true;
    }
    ids.clear();
    for (auto it = moved.begin(); it != moved.end(); ++it) ids.push_back(it.key());
    for (const std::string& id : ids) if (!waiting.count(id)) { moved.erase(id); dirty = true; }
    ids.clear();
    for (auto it = sent.begin(); it != sent.end(); ++it) ids.push_back(it.key());
    for (const std::string& id : ids) {
      const json s = sent[id];
      if (waiting.count(id) && now() - num(prop(&s, "at")) <= KEEP_SENT_MS) continue;
      sent.erase(id);
      // the session READ these answers before Done's one line reached it: the line still queued would only repeat them
      if (readAt.count(id) && truthy(prop(&s, "heldId")) && deps.onRead) deps.onRead(id, s);
    }
    if (dirty) saveDrafts();
  }

  // migrate: before a replaced page's drafts are dropped, what they entered moves onto the newer page's cards
  // (the skill's `superseded_by` and the newer round's `inherits`). A card that already has a draft keeps it. The
  // text and reply buttons always move; an option pick only when the two cards' options are the same.
  // Oldest first, so a chain (r1 -> r2 -> r3, pushed out at once) ends on the newest page.
  bool migrate(const std::vector<RX>& all) {
    const ById byId = indexRounds(all);
    std::vector<const RX*> olds;
    for (const RX& x : all) if (!x.waiting && truthy(prop(x.round, "superseded_by"))) olds.push_back(&x);
    std::stable_sort(olds.begin(), olds.end(), [](const RX* a, const RX* b) { return strOr(prop(a->round, "created")) < strOr(prop(b->round, "created")); });
    bool any = false;
    for (const RX* x : olds) {
      const json* from = prop(&drafts, x->id);
      const json* fromState = truthy(from) ? prop(from, "state") : nullptr;
      if (!truthy(fromState)) continue;
      const RX* to = byIdGet(byId, prop(x->round, "superseded_by"));
      const OJ* inh = to ? prop(to->round, "inherits") : nullptr;
      const OJ* map = truthy(inh) ? prop(inh, x->id) : nullptr;
      if (!to || !truthy(map) || !map->is_object()) continue;
      const json* had = prop(&drafts, to->id);
      json dst = truthy(had) ? *had
        : json{ { "state", json::object() }, { "drafted", 0 }, { "answered", 0 }, { "later", 0 }, { "total", to->items.size() }, { "at", numJson(now()) } };
      if (!dst.is_object()) dst = json::object();
      json& st = dst["state"];
      if (!st.is_object()) st = json::object();
      const auto itemOf = [](const RX* r, const OJ* qid) { const OJ* hit = nullptr; for (const OJ* i : r->items) if (strictEq(prop(i, "qid"), qid)) hit = i; return hit; };
      const auto hasItem = [](const RX* r, const OJ* qid) { for (const OJ* i : r->items) if (strictEq(prop(i, "qid"), qid)) return true; return false; };
      Report rep; rep.to = to->id;
      int n = 0;
      for (auto e = map->begin(); e != map->end(); ++e) {
        const std::string oq = e.key(); const OJ* nq = &e.value();
        const json* o = prop(fromState, oq);
        if (!entered(o)) continue;
        Card c; c.oq = oq; c.to = *nq; c.hasWhy = true;
        if (!hasItem(to, nq)) { c.why = "that card is not on the newer page"; rep.cards.push_back(c); continue; }
        // lib/asks.js retarget (fix 8, 2026-11-20): the map's target has ANOTHER title -> the card with the old card's
        // own title (the first one with no answer yet); the target is another old card's -> not moved; else the map's
        std::string aimWhy;
        { const OJ oq0 = oq;
          const OJ* oldItem = itemOf(x, &oq0); const OJ* atItem = itemOf(to, nq);
          const std::string want = titleKey(oldItem);
          if (!want.empty() && atItem && titleKey(atItem) != want) {
            const OJ* twin = nullptr;
            for (const OJ* i : to->items) if (titleKey(i) == want && !entered(prop(&st, tpl(prop(i, "qid"))))) { twin = i; break; }
            if (twin) {
              aimWhy = "the newer page's map said " + tpl(nq) + " (" + tpl(prop(atItem, "title")) + "); moved to " + tpl(prop(twin, "qid")) + ", the card with this title";
              nq = prop(twin, "qid"); c.to = *nq;
            } else {
              const OJ* other = nullptr;
              for (const OJ* i : x->items) if (i != oldItem && titleKey(i) == titleKey(atItem)) { other = i; break; }
              if (other) { c.why = "the newer page's map said " + tpl(nq) + " (" + tpl(prop(atItem, "title")) + "), which is " + tpl(prop(other, "qid")) + "'s card: not moved"; rep.cards.push_back(c); continue; }
            }
          } }
        const std::string nqKey = tpl(nq);
        if (entered(prop(&st, nqKey))) { c.why = "the newer card already had his answer"; rep.cards.push_back(c); continue; }
        const json* op = prop(o, "pick");
        const bool reply = op && op->is_string() && startsWith(op->get_ref<const std::string&>(), "r:");
        const OJ oqv = oq;
        const bool same = sameOptions(itemOf(x, &oqv), itemOf(to, nq));
        const bool hasPick = op && !op->is_null();           // o.pick != null
        json movedCard = { { "text", strOr(prop(o, "text")) }, { "pick", reply ? *op : (hasPick && same ? *op : json()) } };
        if (truthy(prop(o, "later"))) movedCard["later"] = true;
        if (!entered(&movedCard)) { c.why = "only an option pick, and the options changed"; rep.cards.push_back(c); continue; }
        c.moved = true;
        { std::string w = aimWhy;
          if (hasPick && !reply && !same) w += std::string(w.empty() ? "" : "; ") + "his words moved; the option pick did not (the options changed)";
          c.hasWhy = !w.empty(); if (c.hasWhy) c.why = w; }
        rep.cards.push_back(c);
        st[nqKey] = movedCard;
        n++;
      }
      report[x->id] = rep;
      if (!n) continue;
      int drafted = 0;
      for (const OJ* i : to->items) if (entered(prop(&st, tpl(prop(i, "qid"))))) drafted++;
      dst["drafted"] = drafted;
      dst["at"] = numJson(now());
      drafts[to->id] = dst;
      json& mv = moved[to->id];
      if (!mv.is_object()) mv = json::object();
      mv[x->id] = { { "n", n }, { "at", numJson(now()) } };
      any = true;
    }
    return any;
  }

  // answersDir: next to the page; the first Downloads folder that exists when the page's folder is gone
  std::string answersDirOf(const RX* x) {
    const OJ* page = x ? prop(x->round, "page") : nullptr;
    if (truthy(page)) {
      const std::string dir = dirnameP(normPath(expandHome(tpl(page), deps.home), deps.win), deps.win);
      if (files().isDir(dir)) return dir;
    }
    for (const std::string& d : deps.downloads) if (files().isDir(d)) return d;
    return "";
  }

  // saveDraftCopy: a DRAFT copy of what they entered on a page and never sent, next to the page, in the skill's own
  // save-drafts format (kind ask-page-drafts): <page folder>/<page id>.drafts-<HHMMSS>.json + .txt. For a
  // replaced page: which card went where, and which did not and why. "" when nothing of their was on it.
  std::string saveDraftCopy(const std::string& pageId, const RX* x, const Report* rep) {
    const json* d = prop(&drafts, pageId);
    const json* state = truthy(d) ? prop(d, "state") : nullptr;
    if (!truthy(state) || !(state->is_object() || state->is_array())) return "";
    bool some = false;
    for (const json& v : *state) if (entered(&v)) { some = true; break; }
    if (!some) return "";
    std::vector<std::string> lines; int answered = 0, later = 0;
    const auto card = [&](const std::string& qidText, const std::string& title, const Ans& a) {
      if (!a.ok) return;
      if (a.status == "answered" || a.status == "dropped") answered++; else if (a.status == "later") later++;
      std::string ans;                                       // a.answer.replace(/\n+/g, ' / ')
      for (size_t i = 0; i < a.answer.size(); i++) {
        if (a.answer[i] != '\n') { ans += a.answer[i]; continue; }
        while (i + 1 < a.answer.size() && a.answer[i + 1] == '\n') i++;
        ans += " / ";
      }
      lines.push_back(qidText + (title.empty() ? std::string() : " (" + title + ")") + ": " + ans);
    };
    if (x) {
      for (const OJ* it : x->items) {
        const OJ* qid = prop(it, "qid"); const OJ* title = prop(it, "title");
        card(tpl(qid), strOr(title), draftAnswer(it, prop(state, tpl(qid))));
      }
    } else if (state->is_object()) {
      const OJ* noItem = nullptr;
      for (auto it = state->begin(); it != state->end(); ++it) card(it.key(), "", draftAnswer(noItem, &it.value()));
    }
    const OJ* byv = x ? prop(x->round, "superseded_by") : nullptr;
    const bool by = truthy(byv);
    const OJ* status = x ? prop(x->round, "status") : nullptr;
    const std::string why = by ? "replaced by " + tpl(byv)
      : x && truthy(prop(x->round, "read_at")) ? "the session read its answers (given elsewhere) before these were sent"
      : x ? "its round is " + (truthy(status) ? tpl(status) : std::string("closed")) : "its round is gone";
    const std::string dir = answersDirOf(x);
    if (dir.empty()) return "";
    // local time with its offset, as the skill's own copies write it (2026-11-10T23:19:04-05:00)
    const double t = now(), offset = deps.clock ? deps.clock->localOffsetMs(t) : 0;
    const std::string iso = isoTime(t + offset);             // the local wall clock, read off as if it were UTC
    const int off = (int)std::floor(offset / 60000.0 + 0.5), mag = off < 0 ? -off : off;
    char zone[16];
    std::snprintf(zone, sizeof zone, "%c%02d:%02d", off >= 0 ? '+' : '-', mag / 60, mag % 60);
    const std::string local = iso.substr(0, 19) + zone;
    const std::string base = joinP(dir, pageId + ".drafts-" + iso.substr(11, 2) + iso.substr(14, 2) + iso.substr(17, 2), deps.win);
    OJ moves = OJ::array();
    std::vector<std::string> went, stay;
    if (rep) for (const Card& c : rep->cards) {
      const std::string to = rep->to + ":" + tpl(&c.to);
      OJ m = OJ::object();
      m["qid"] = c.oq; m["to"] = to; m["moved"] = c.moved; m["why"] = c.hasWhy ? OJ(c.why) : OJ();
      moves.push_back(m);
      if (c.moved) went.push_back(c.oq + " -> " + to + (c.hasWhy ? " (" + c.why + ")" : std::string()));
      else stay.push_back(c.oq + " (" + c.why + ")");
    }
    OJ doc = OJ::object();
    doc["kind"] = "ask-page-drafts"; doc["draft"] = true; doc["id"] = pageId; doc["saved_at"] = local;
    doc["source"] = "claude-deck: " + why;
    const OJ* sname = x ? prop(x->state, "session_name") : nullptr;
    const OJ* rnd = x ? prop(x->round, "round") : nullptr;
    doc["session_id"] = x ? OJ(sessionIdOf(*x)) : OJ();
    doc["session_name"] = truthy(sname) ? *sname : OJ();
    if (!x) doc["round"] = OJ(); else if (rnd) doc["round"] = *rnd;
    doc["answered"] = answered; doc["later"] = later;
    const json* dTotal = prop(d, "total");
    json total = x ? json(x->items.size()) : truthy(dTotal) ? *dTotal : json();
    doc["total"] = OJ(total);
    doc["state"] = OJ(*state);
    doc["lines"] = lines;
    doc["replaced_by"] = by ? *byv : OJ();
    doc["moves"] = moves;
    std::string txt = "DRAFT - not sent. " + pageId + (truthy(sname) ? " (" + tpl(sname) + ", round " + tpl(rnd) + ")" : std::string()) + "\n";
    txt += "saved " + local + " by the Claude Deck: " + why + ". " + std::to_string(answered) + " of " + (truthy(&total) ? tpl(&total) : std::string("?"))
      + " answered, " + std::to_string(later) + " later.\n";
    const auto joined = [](const std::vector<std::string>& v) { std::string s; for (size_t i = 0; i < v.size(); i++) { if (i) s += "; "; s += v[i]; } return s; };
    if (!went.empty()) txt += "moved to the newer page: " + joined(went) + "\n";
    if (!stay.empty()) txt += "NOT moved: " + joined(stay) + "\n";
    txt += "\n";
    for (const std::string& l : lines) txt += l + "\n";
    if (!files().writeAtomic(base + ".json", dumpOne(doc))) return "";
    if (!files().writeAtomic(base + ".txt", txt)) return "";
    return base + ".json";
  }

  // What the views get (meta.asks): every session with a page waiting on them, newest ask first
  json view() {
    const std::vector<RX> all = rounds();
    const ById byId = indexRounds(all);
    const std::set<std::string>* liveNow = liveSet();
    std::vector<std::pair<std::string, json>> list;          // bySid, in insertion order
    for (const RX& x : all) {
      if (!x.waiting) continue;
      const OJ* r = x.round;
      const json* dr = prop(&drafts, x.id);
      const json* snt = prop(&sent, x.id);
      const json* mvAll = prop(&moved, x.id);
      json row = json::object();
      row["id"] = x.id;
      if (const OJ* v = prop(r, "round")) row["round"] = json(*v);
      const OJ* v;
      row["created"] = truthy(v = prop(r, "created")) ? json(*v) : json();
      row["title"] = truthy(v = prop(r, "title")) ? json(*v) : json("");
      row["source"] = truthy(v = prop(r, "source")) ? json(*v) : json("skill");
      row["background"] = truthy(prop(r, "background"));
      if ((v = prop(r, "status"))) row["status"] = json(*v);
      row["total"] = x.items.size(); row["answered"] = x.answered; row["later"] = x.later;
      const json* drafted = truthy(dr) ? prop(dr, "drafted") : nullptr;
      row["drafted"] = truthy(drafted) ? *drafted : json(0);
      row["sent"] = truthy(snt) ? *snt : json();
      const OJ* page = prop(r, "page");
      const std::string url = truthy(page) && page->is_string() ? pathToFileUrl(page->get_ref<const std::string&>(), deps.win) : std::string();
      row["url"] = url.empty() ? json() : json(url);
      // the older pages this one replaced (the skill's inherits), and how many of their answers came over from each
      json replaces = json::array();
      const OJ* inh = prop(r, "inherits");
      if (truthy(inh) && inh->is_object()) for (auto e = inh->begin(); e != inh->end(); ++e) {
        if (!askIdOk(e.key())) continue;
        auto oit = byId.find(e.key());
        const RX* o = oit == byId.end() ? nullptr : oit->second;
        const json* m = truthy(mvAll) ? prop(mvAll, e.key()) : nullptr;
        json p = json::object();
        p["id"] = e.key();
        p["sessionId"] = o ? sessionIdOf(*o) : json();
        p["name"] = o ? sessionNameOf(*o) : json();
        const OJ* ornd = o ? prop(o->round, "round") : nullptr;
        if (!o) p["round"] = json(); else if (ornd) p["round"] = json(*ornd);
        const OJ& cards = e.value();                         // Object.keys(inh[oid] || {}).length
        p["cards"] = cards.is_object() || cards.is_array() ? cards.size() : cards.is_string() ? units(cards.get_ref<const std::string&>()) : 0;
        if (!truthy(m)) p["moved"] = 0; else if (const json* nn = prop(m, "n")) p["moved"] = *nn;
        replaces.push_back(p);
      }
      row["replaces"] = replaces;
      json items = json::array();
      for (const OJ* it : x.items) {
        json i = json::object();
        if (const OJ* q = prop(it, "qid")) i["qid"] = json(*q);
        if (const OJ* t = prop(it, "title")) i["title"] = json(*t);
        items.push_back(i);
      }
      row["items"] = items;
      json* s = nullptr;
      for (auto& kv : list) if (kv.first == x.sid) s = &kv.second;
      if (!s) {
        const json sessionId = sessionIdOf(x);
        const OJ* account = prop(x.state, "account");
        // ended: nobody left to read their answers (until it is resumed)
        const bool ended = liveNow ? !(sessionId.is_string() && liveNow->count(sessionId.get_ref<const std::string&>())) : false;
        list.push_back({ x.sid, json{ { "sessionId", sessionId }, { "name", sessionNameOf(x) }, { "account", truthy(account) ? json(*account) : json("") },
          { "rounds", json::array() }, { "ended", ended } } });
        s = &list.back().second;
      }
      (*s)["rounds"].push_back(row);
    }
    const auto created = [](const json& row) { return tpl(prop(&row, "created")); };   // String(r.created)
    std::vector<json> out;
    for (auto& kv : list) {
      json& s = kv.second;
      std::vector<json> rows(s["rounds"].begin(), s["rounds"].end());
      std::stable_sort(rows.begin(), rows.end(), [&](const json& a, const json& b) { return created(a) < created(b); });
      double total = 0, drafted = 0; bool allSent = true;
      for (const json& r : rows) { total += num(&r["total"]); drafted += num(&r["drafted"]); if (!truthy(&r["sent"])) allSent = false; }
      s["latest"] = rows.empty() ? json() : rows.back()["created"];
      s["rounds"] = rows;
      s["total"] = numJson(total); s["drafted"] = numJson(drafted); s["sent"] = allSent;
      out.push_back(std::move(s));
    }
    const auto latest = [](const json& s) { return tpl(prop(&s, "latest")); };
    std::stable_sort(out.begin(), out.end(), [&](const json& a, const json& b) { return latest(b) < latest(a); });
    return json(out);
  }

  // ---- the log of answered pages
  // logRead: a round the session has READ (its answers recorded) goes into the log if it is not there yet:
  // answered in the terminal, in its picker, or on a browser page. What they picked comes from the session's record.
  void logRead() {
    std::set<std::string> have;
    for (const json& e : log) { const json* id = prop(&e, "id"); if (id && id->is_string()) have.insert(id->get<std::string>()); }
    const double cutoff = now() - LOG_DAYS * 86400000.0;
    bool added = false;
    for (const RX& x : rounds()) {
      const OJ* r = x.round;
      const OJ* readAt = prop(r, "read_at");
      if (!truthy(readAt) || have.count(x.id)) continue;
      double at = js::dateParse(tpl(readAt));
      if (std::isnan(at)) at = 0;
      if (at < cutoff) continue;
      if (log.size() >= LOG_MAX) { const double last = num(prop(&log.back(), "at")); if (at <= (std::isnan(last) ? 0 : last)) continue; }
      const OJ* answers = prop(x.state, "answers");
      if (!truthy(answers)) answers = nullptr;
      json lines = json::array(), state = json::object();
      int answered = 0; bool picker = false, paste = false, file = false;
      for (const OJ* it : x.items) {
        const OJ* a = prop(answers, tpl(prop(it, "key")));
        const OJ* via = truthy(a) ? prop(a, "via") : nullptr;
        if (isStr(via, "picker")) picker = true; else if (isStr(via, "paste")) paste = true; else if (isStr(via, "file")) file = true;
        const OJ* status = truthy(a) ? prop(a, "status") : nullptr;
        if (!truthy(status) || isStr(status, "skipped")) continue;
        if (isStr(status, "answered") || isStr(status, "dropped")) answered++;
        const OJ* answer = prop(a, "answer"); const OJ* reply = prop(a, "reply"); const OJ* choice = prop(a, "choice");
        const std::string qid = tpl(prop(it, "qid"));
        lines.push_back(qid + " (" + tpl(prop(it, "title")) + "): " + (truthy(answer) ? tpl(answer) : isStr(status, "later") ? std::string("later") : std::string()));
        state[qid] = { { "text", truthy(answer) ? json(*answer) : json("") },
          { "pick", truthy(reply) ? json("r:" + tpl(reply)) : (!choice || choice->is_null() ? json() : json(*choice)) } };
      }
      if (lines.empty()) continue;
      const char* how = picker ? "answered in its picker" : paste ? "answered in its terminal" : file ? "answered on the page" : "answered";
      const OJ* account = prop(x.state, "account"); const OJ* page = prop(r, "page"); const OJ* rnd = prop(r, "round");
      json e = { { "id", x.id }, { "sessionId", sessionIdOf(x) }, { "name", sessionNameOf(x) }, { "account", truthy(account) ? json(*account) : json("") },
        { "title", nullptr }, { "at", numJson(at) }, { "how", how }, { "lines", lines }, { "state", state }, { "total", x.items.size() },
        { "answered", answered }, { "page", truthy(page) ? json(*page) : json() } };
      if (rnd) e["round"] = json(*rnd);
      log.push_back(e);
      added = true;
    }
    if (!added) return;
    std::vector<json> v(log.begin(), log.end());
    const auto at = [](const json& e) { const double d = num(prop(&e, "at")); return std::isnan(d) ? 0 : d; };   // b.at - a.at
    std::stable_sort(v.begin(), v.end(), [&](const json& a, const json& b) { return at(a) > at(b); });
    if (v.size() > LOG_MAX) v.resize(LOG_MAX);
    log = v;
    saveDrafts();
  }

  json logView() {
    const std::vector<RX> all = rounds();
    json out = json::array();
    for (const json& e : log) {
      const json* page = prop(&e, "page");
      std::string url;
      if (truthy(page) && page->is_string() && files().stat(page->get_ref<const std::string&>()).exists) url = pathToFileUrl(page->get_ref<const std::string&>(), deps.win);
      const json* idv = prop(&e, "id");
      const RX* x = idv && idv->is_string() ? findRound(all, idv->get_ref<const std::string&>()) : nullptr;
      const OJ* byv = x ? prop(x->round, "superseded_by") : nullptr;
      const RX* by = truthy(byv) && byv->is_string() ? findRound(all, byv->get_ref<const std::string&>()) : nullptr;
      json row = json::object();
      for (const char* k : { "id", "sessionId", "name", "account", "title", "round", "at", "how", "total", "answered" }) if (const json* v = prop(&e, k)) row[k] = *v;
      json lines = json::array();
      const json* el = prop(&e, "lines");
      if (el && el->is_array()) for (const json& l : *el) { if (lines.size() >= 40) break; lines.push_back(js::sliceUnits(tpl(&l), 400)); }
      row["lines"] = lines;
      row["url"] = url.empty() ? json() : json(url);
      // pushed out by a newer page after they answered part of it: what is left is asked there
      if (truthy(byv)) {
        json rb = { { "id", json(*byv) }, { "name", by ? sessionNameOf(*by) : json() }, { "sessionId", by ? sessionIdOf(*by) : json() } };
        const OJ* rnd = by ? prop(by->round, "round") : nullptr;
        if (!by) rb["round"] = json(); else if (rnd) rb["round"] = json(*rnd);
        row["replacedBy"] = rb;
      } else row["replacedBy"] = json();
      out.push_back(row);
    }
    return out;
  }
};

AskFeed::AskFeed(AskDeps deps) : d_(new Impl()) { d_->deps = std::move(deps); d_->loadDrafts(); }
AskFeed::~AskFeed() { *d_->alive = false; }

void AskFeed::scan() { d_->scan(); }
json AskFeed::view() { return d_->view(); }
json AskFeed::logView() { return d_->logView(); }
const std::vector<std::string>& AskFeed::downloads() const { return d_->deps.downloads; }
const std::string& AskFeed::stateDir() const { return d_->deps.stateDir; }
void AskFeed::flushDrafts() { if (d_->pending) d_->writeDrafts(); }

// draft(pageId): d ? d.state : null
json AskFeed::draft(const std::string& pageId) {
  const json* d = prop(&d_->drafts, pageId);
  const json* s = truthy(d) ? prop(d, "state") : nullptr;
  return s ? *s : json();
}

bool AskFeed::setDraft(const std::string& pageId, const json& p) {
  if (!askIdOk(pageId) || !(p.is_object() || p.is_array())) return false;
  Impl::Call call(d_.get());
  const json* ps = prop(&p, "state");
  const json state = ps && (ps->is_object() || ps->is_array()) ? *ps : json::object();
  // JSON.stringify(state).length: UTF-16 units of the compact text (non-ASCII left as it is, as in the JS)
  if (units(dumpCompact(state)) > DRAFT_MAX) return false;
  const auto n = [&](const char* key) {                      // Number.isFinite(+v) ? Math.max(0, Math.min(999, Math.round(+v))) : 0
    const double v = num(prop(&p, key));
    return std::isfinite(v) ? (int)std::max(0.0, std::min(999.0, js::round(v))) : 0;
  };
  const json* before = prop(&d_->drafts, pageId);
  const bool had = truthy(before);
  const json beforeDrafted = had && prop(before, "drafted") ? *prop(before, "drafted") : json();
  const bool beforeHasDrafted = had && prop(before, "drafted");
  const int drafted = n("drafted");
  if (!drafted) d_->drafts.erase(pageId);
  else d_->drafts[pageId] = { { "state", state }, { "drafted", drafted }, { "answered", n("answered") }, { "later", n("later") }, { "total", n("total") }, { "at", numJson(d_->now()) } };
  d_->saveDrafts();
  const bool has = drafted != 0;
  const bool moved = had != has || (had && !(beforeHasDrafted && beforeDrafted.is_number() && beforeDrafted.get<double>() == drafted));
  if (moved) d_->scan();                                     // the marks change: tell the views (scan compares and calls onChange)
  return true;
}

void AskFeed::clearDraft(const std::string& pageId) {
  if (!truthy(prop(&d_->drafts, pageId))) return;
  Impl::Call call(d_.get());
  d_->drafts.erase(pageId);
  d_->saveDrafts();
  d_->scan();
}

// logState(pageId): e ? e.state || {} : null
json AskFeed::logState(const std::string& pageId) {
  for (const json& e : d_->log) {
    if (!isStr(prop(&e, "id"), pageId.c_str())) continue;
    const json* s = prop(&e, "state");
    return truthy(s) ? *s : json::object();
  }
  return json();
}

AskRound AskFeed::round(const std::string& pageId) {
  AskRound out;
  const std::vector<RX> all = d_->rounds();
  const RX* x = findRound(all, pageId);
  if (!x) return out;
  out.found = true; out.sid = x->sid; out.state = json(*x->state); out.round = json(*x->round);
  out.items = json::array();
  for (const OJ* it : x->items) out.items.push_back(json(*it));
  out.answered = x->answered; out.later = x->later; out.waiting = x->waiting;
  return out;
}

// pagePath: a page the deck may show: one of the skill's rounds, an ask_*.html file that exists
std::string AskFeed::pagePath(const std::string& pageId) {
  const std::vector<RX> all = d_->rounds();
  const RX* x = findRound(all, pageId);
  if (!x) return "";
  const std::string p = strOr(prop(x->round, "page"));
  const std::string b = lowerAscii(basenameP(p, d_->deps.win));   // /^ask_.*\.html$/i
  if (b.size() < 9 || !startsWith(b, "ask_") || !endsWith(b, ".html")) return "";
  if (b.find('\n') != std::string::npos || b.find('\r') != std::string::npos || b.find("\xE2\x80\xA8") != std::string::npos || b.find("\xE2\x80\xA9") != std::string::npos) return "";
  if (!d_->files().isFile(p)) return "";
  return p;
}

std::string AskFeed::answersDir(const std::string& pageId) {
  const std::vector<RX> all = d_->rounds();
  return d_->answersDirOf(findRound(all, pageId));
}

// writeAnswersFile: the page's own answers file, <page folder>/<page id>.answers.json, written atomically
std::string AskFeed::writeAnswersFile(const std::string& pageId, const json& answers) {
  if (!askIdOk(pageId)) throw std::runtime_error("not an ask page id");
  if (!truthy(&answers) || !isStr(prop(&answers, "kind"), "ask-page-answers") || !isStr(prop(&answers, "id"), pageId.c_str()))
    throw std::runtime_error("these answers are not that page's");
  const std::string dir = answersDir(pageId);
  if (dir.empty()) throw std::runtime_error("no folder to save the answers in (the page's folder is gone and there is no Downloads folder)");
  const std::string file = joinP(dir, pageId + ".answers.json", d_->deps.win);
  // the JS lets fs's own error out here (ENOENT / EACCES ...); the seam has no error text
  if (!d_->files().writeAtomic(file, dumpOne(answers))) throw std::runtime_error("could not write " + file);
  return file;
}

void AskFeed::markSent(const std::vector<std::string>& ids, const json& info) {
  Impl::Call call(d_.get());
  for (const std::string& id : ids) {
    json s = { { "at", numJson(d_->now()) } };               // Object.assign({ at: this.now() }, info || {})
    if (info.is_object()) for (auto it = info.begin(); it != info.end(); ++it) s[it.key()] = it.value();
    d_->sent[id] = s;
  }
  d_->saveDrafts();
  d_->scan();
}

// logDone: Done here: the page's own answers (its Copy-all lines) and what was on it (the draft, for the read-only view)
void AskFeed::logDone(const std::string& pageId, const json& answers, const std::string& how, const json& title) {
  const std::vector<RX> all = d_->rounds();
  const RX* x = findRound(all, pageId);
  if (!x) return;
  Impl::Call call(d_.get());
  const json* dr = prop(&d_->drafts, pageId);
  const json* state = truthy(dr) ? prop(dr, "state") : nullptr;
  json lines = json::array();
  const json* al = truthy(&answers) ? prop(&answers, "lines") : nullptr;
  if (al && al->is_array()) for (const json& l : *al) lines.push_back(tpl(&l));
  const double answered = truthy(&answers) ? num(prop(&answers, "answered")) : 0;   // +answers.answered || 0
  const OJ* account = prop(x->state, "account"); const OJ* page = prop(x->round, "page"); const OJ* rnd = prop(x->round, "round");
  json e = { { "id", pageId }, { "sessionId", sessionIdOf(*x) }, { "name", sessionNameOf(*x) }, { "account", truthy(account) ? json(*account) : json("") },
    { "title", truthy(&title) ? title : json() }, { "at", numJson(d_->now()) }, { "how", how.empty() ? std::string("sent") : how }, { "lines", lines },
    { "state", truthy(dr) ? (state ? *state : json()) : json::object() }, { "total", x->items.size() },
    { "answered", std::isnan(answered) ? json(0) : numJson(answered) }, { "page", truthy(page) ? json(*page) : json() } };
  if (rnd) e["round"] = json(*rnd);
  json next = json::array();
  next.push_back(e);
  for (const json& l : d_->log) { if (next.size() >= LOG_MAX) break; if (!isStr(prop(&l, "id"), pageId.c_str())) next.push_back(l); }
  d_->log = next;
  d_->saveDrafts();
}

// writeDeckFile: deck.json, the deck's side of the skill's settings, plus a heartbeat
bool AskFeed::guest() const { return d_->guest; }
json AskFeed::writeDeckFile() {
  { // the guard: a fresh heartbeat that is not ours
    std::string text; json j; bool foreign = false;
    if (d_->files().read(joinP(d_->deps.stateDir, "deck.json", d_->deps.win), text) && parseJs(text, j)) {
      const json* pid = prop(&j, "pid"); const json* at = prop(&j, "at");
      const bool mine = pid && pid->is_number() && pid->get<double>() == (double)d_->deps.pid;
      const double when = at && at->is_string() ? deck::session::js::dateParse(at->get<std::string>()) : std::nan("");
      foreign = !mine && std::isfinite(when) && d_->now() - when < kAskForeignFreshMs;
    }
    d_->guest = foreign;
    if (foreign) return json();
  }
  json s = d_->deps.settings ? d_->deps.settings() : json();
  if (!truthy(&s)) s = json::object();
  const json* th = prop(&s, "deckThreshold");
  OJ threshold;                                              // Number.isInteger(th) && th >= 0 ? th : null
  if (th && th->is_number()) { const double v = th->get<double>(); if (std::isfinite(v) && v == std::floor(v) && v >= 0) threshold = OJ(numJson(v)); }
  const json* usePages = prop(&s, "usePages"); const json* background = prop(&s, "background");
  OJ body = OJ::object();
  body["kind"] = "claude-deck-asks";
  body["pid"] = d_->deps.pid;
  body["at"] = isoTime(d_->now());
  body["beat_seconds"] = kAskBeatSeconds;
  body["deck_threshold"] = threshold;
  body["picker"] = isStr(prop(&s, "picker"), "pane") ? "pane" : "alongside";
  body["use_pages"] = !(usePages && usePages->is_boolean() && !usePages->get<bool>());        // off: sessions in the deck ask in the terminal
  body["background"] = !(background && background->is_boolean() && !background->get<bool>());  // on: terminal questions also get a page
  body["shows_pages"] = true;
  // a page pushed out by a newer one hands its drafts over (migrate): the skill's build need not refuse (exit 6)
  body["migrates_drafts"] = true;
  d_->files().makeDirs(d_->deps.stateDir);
  d_->files().writeAtomic(joinP(d_->deps.stateDir, "deck.json", d_->deps.win), dumpOne(body));
  return json(body);
}

void AskFeed::removeDeckFile() {
  const std::string f = joinP(d_->deps.stateDir, "deck.json", d_->deps.win);
  std::string text; json j;
  if (!d_->files().read(f, text) || !parseJs(text, j)) return;   // not ours, or gone
  const json* pid = prop(&j, "pid");
  if (pid && pid->is_number() && pid->get<double>() == (double)d_->deps.pid) d_->files().remove(f);
}

}}  // namespace deck::core
