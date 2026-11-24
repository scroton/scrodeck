// core/deck/startup.cpp — see startup.hpp. The regular expressions of lib/startupprompts.js, written out by
// hand (std::regex recurses per character and these run over kilobytes of screen text on every chunk).
#include "startup.hpp"
#include "../session/jsutil.hpp"
#include "../session/replay.hpp"
#include <algorithm>

namespace deck { namespace core {

namespace js = deck::session::js;
static const size_t npos = std::string::npos;
static const double QUIET_MS = 4000;
static const int MAX_ANSWERS = 3;
static const size_t KEEP = 12000;

static bool isDigit(char c) { return c >= '0' && c <= '9'; }
// skip \s* at i (JavaScript's \s)
static size_t skipSpace(const std::string& s, size_t i) {
  while (i < s.size()) { size_t j = i; if (!js::isSpace(js::nextCodePoint(s, j))) break; i = j; }
  return i;
}
// words[0] \s* words[1] \s* … at exactly `at`: the end of the match, or npos
static size_t matchWords(const std::string& s, size_t at, std::initializer_list<const char*> words) {
  size_t i = at; bool first = true;
  for (const char* w : words) {
    if (!first) i = skipSpace(s, i);
    first = false;
    const size_t n = std::char_traits<char>::length(w);
    if (s.compare(i, n, w) != 0) return npos;
    i += n;
  }
  return i;
}
static size_t findWords(const std::string& s, size_t from, std::initializer_list<const char*> words, size_t* end = nullptr) {
  const char* first = *words.begin();
  for (size_t p = s.find(first, from); p != npos; p = s.find(first, p + 1)) {
    const size_t e = matchWords(s, p, words);
    if (e != npos) { if (end) *end = e; return p; }
  }
  return npos;
}

std::string startupScreenText(const std::string& in) {
  std::string a, b;
  // 1. OSC: \x1b\][^\x07\x1b]*(?:\x07|\x1b\\)
  for (size_t i = 0; i < in.size();) {
    if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == ']') {
      size_t j = i + 2;
      while (j < in.size() && in[j] != '\x07' && in[j] != '\x1b') j++;
      if (j < in.size() && in[j] == '\x07') { i = j + 1; continue; }
      if (j + 1 < in.size() && in[j] == '\x1b' && in[j + 1] == '\\') { i = j + 2; continue; }
    }
    a += in[i++];
  }
  // 2. cursor forward = blanks: \x1b\[(\d*)C
  for (size_t i = 0; i < a.size();) {
    if (a[i] == '\x1b' && i + 1 < a.size() && a[i + 1] == '[') {
      size_t j = i + 2; long n = 0; bool any = false;
      while (j < a.size() && isDigit(a[j])) { if (n < 100000) n = n * 10 + (a[j] - '0'); any = true; j++; }
      if (j < a.size() && a[j] == 'C') { b.append((size_t)std::min<long>((any && n) ? n : 1, 200), ' '); i = j + 1; continue; }
    }
    b += a[i++];
  }
  a.clear();
  // 3. cursor position = new line: \x1b\[\d*(?:;\d*)?[Hf]
  for (size_t i = 0; i < b.size();) {
    if (b[i] == '\x1b' && i + 1 < b.size() && b[i + 1] == '[') {
      size_t j = i + 2;
      while (j < b.size() && isDigit(b[j])) j++;
      if (j < b.size() && b[j] == ';') { j++; while (j < b.size() && isDigit(b[j])) j++; }
      if (j < b.size() && (b[j] == 'H' || b[j] == 'f')) { a += '\n'; i = j + 1; continue; }
    }
    a += b[i++];
  }
  b.clear();
  // 4. every other CSI: \x1b\[[0-9;?<>=]*[ -\/]*[@-~]
  for (size_t i = 0; i < a.size();) {
    if (a[i] == '\x1b' && i + 1 < a.size() && a[i + 1] == '[') {
      size_t j = i + 2;
      while (j < a.size() && (isDigit(a[j]) || a[j] == ';' || a[j] == '?' || a[j] == '<' || a[j] == '>' || a[j] == '=')) j++;
      // [ -\/]* then [@-~]: greedy with backtracking only matters when the final could be in ' '..'/', which it cannot
      while (j < a.size() && a[j] >= ' ' && a[j] <= '/') j++;
      if (j < a.size() && a[j] >= '@' && a[j] <= '~') { i = j + 1; continue; }
    }
    b += a[i++];
  }
  a.clear();
  // 5. \x1b[=>]
  for (size_t i = 0; i < b.size();) {
    if (b[i] == '\x1b' && i + 1 < b.size() && (b[i + 1] == '=' || b[i + 1] == '>')) { i += 2; continue; }
    a += b[i++];
  }
  return a;
}

std::pair<std::string, std::string> splitIncomplete(const std::string& s) {
  const size_t last = s.rfind('\x1b');
  if (s.size() > 4096 && (last == npos || last < s.size() - 4096)) return { s, "" };
  const size_t osc = s.rfind("\x1b]");
  if (osc != npos && s.size() - osc < 4096) {
    const std::string tail = s.substr(osc + 2);
    if (tail.find('\x07') == npos && tail.find("\x1b\\") == npos) return { s.substr(0, osc), s.substr(osc) };
  }
  if (last != npos) {
    // complete when: ESC [ params intermediates final | ESC ] … (BEL | ESC \) | ESC + one char that is not [ ] ESC
    bool complete = false;
    if (last + 1 < s.size()) {
      const char c = s[last + 1];
      if (c == '[') {
        size_t j = last + 2;
        while (j < s.size() && (isDigit(s[j]) || s[j] == ';' || s[j] == '?' || s[j] == '<' || s[j] == '>' || s[j] == '=')) j++;
        while (j < s.size() && s[j] >= ' ' && s[j] <= '/') j++;
        complete = j < s.size() && s[j] >= '@' && s[j] <= '~';
      } else if (c == ']') {
        const std::string tail = s.substr(last + 2);
        complete = tail.find('\x07') != npos || tail.find("\x1b\\") != npos;
      } else complete = c != '\x1b';
    }
    if (!complete) return { s.substr(0, last), s.substr(last) };
  }
  return { s, "" };
}

static std::string prose(const std::string& s) { return js::trim(js::collapseSpaces(s)); }

static json joinWrapped(const std::vector<std::string>& lines, const ExistsFn& exists) {
  std::vector<std::string> parts;
  for (const std::string& l : lines) { const std::string t = js::trim(l); if (!t.empty()) parts.push_back(t); }
  if (parts.empty()) return json();
  const int breaks = (int)std::min<size_t>(parts.size() - 1, 6);
  std::vector<std::string> ways;
  for (int mask = 0; mask < (1 << breaks); mask++) {
    std::string p = parts[0];
    for (size_t i = 1; i < parts.size(); i++) p += (((int)i <= breaks && ((mask >> (i - 1)) & 1)) ? " " : "") + parts[i];
    ways.push_back(p);
  }
  if (exists) for (const std::string& p : ways) if (exists(p)) return p;
  return ways[0];
}

static const char* MARK = "\xE2\x9D\xAF";    // ❯
static const char* ELBOW = "\xE2\x94\x94";   // └

json classifyStartup(const std::string& s, const ExistsFn& exists) {
  if (findWords(s, 0, { "Enter", "to", "confirm" }) == npos) return json();
  size_t swEnd = 0;
  const size_t sw = findWords(s, 0, { "Settings", "Warning" }, &swEnd);
  if (sw != npos && findWords(s, 0, { "Exit", "and", "fix", "manually" }) != npos) {
    json out = { { "kind", "settings-warning" }, { "title", "settings warning" } };
    // /❯\s*1\.\s*Continue/
    bool cont = false;
    for (size_t p = s.find(MARK); p != npos && !cont; p = s.find(MARK, p + 1)) {
      size_t i = skipSpace(s, p + 3);
      if (s.compare(i, 2, "1.") == 0) { i = skipSpace(s, i + 2); cont = s.compare(i, 8, "Continue") == 0; }
    }
    out["continueSelected"] = cont;
    json rules = json::array();
    { size_t from = 0, e = 0;
      for (;;) {
        const size_t p = findWords(s, from, { "Invalid", "permission", "rule" }, &e);
        if (p == npos) break;
        size_t i = skipSpace(s, e);
        if (i < s.size() && s[i] == '"') { const size_t q = s.find('"', i + 1); if (q != npos && q > i + 1) { rules.push_back(prose(s.substr(i + 1, q - i - 1))); from = q + 1; continue; } }
        from = p + 1;
      } }
    out["rules"] = rules;
    json errors = json::array();
    for (size_t p = s.find(ELBOW); p != npos;) {
      const size_t start = p + 3;
      size_t stop = s.size();
      for (size_t k = start; k < s.size(); k++) {
        if (s.compare(k, 3, ELBOW) == 0 || s.compare(k, 3, MARK) == 0 || matchWords(s, k, { "The", "values", "listed" }) != npos || matchWords(s, k, { "Enter", "to", "confirm" }) != npos) { stop = k; break; }
      }
      errors.push_back(prose(s.substr(start, stop - start)));
      p = stop < s.size() ? s.find(ELBOW, stop) : npos;
    }
    out["errors"] = errors;
    // head: /Settings\s*Warning([\s\S]*?)└/ — the first └ after the first "Settings Warning"
    const size_t el = s.find(ELBOW, swEnd);
    if (el != npos) {
      std::vector<std::string> lines; std::string head = s.substr(swEnd, el - swEnd);
      size_t a = 0; for (;;) { const size_t nl = head.find('\n', a); if (nl == npos) { lines.push_back(head.substr(a)); break; } lines.push_back(head.substr(a, nl - a)); a = nl + 1; }
      out["file"] = joinWrapped(lines, exists);
    } else out["file"] = nullptr;
    return out;
  }
  // /Yes,\s*I\s*trust\s*this\s*folder/
  if (trustOptionSeen(s)) return { { "kind", "trust" }, { "title", "trust prompt" } };
  if (findWords(s, 0, { "MCP", "server" }) != npos) return { { "kind", "mcp" }, { "title", "MCP server prompt" } };
  return { { "kind", "other" }, { "title", "a prompt" } };
}

std::string describeWarning(const json& p) {
  if (!js::truthy(p)) return std::string();
  std::string what;
  const json& rules = js::get(p, "rules"); const json& errors = js::get(p, "errors");
  if (rules.is_array() && !rules.empty()) { for (size_t i = 0; i < rules.size(); i++) { if (i) what += ", "; what += "\"" + js::toString(rules[i]) + "\""; } }
  else if (errors.is_array() && !errors.empty() && js::truthy(errors[0])) what = js::toString(errors[0]);
  else what = "an entry it could not read";
  return what + (js::truthy(js::get(p, "file")) ? " in " + js::toString(js::get(p, "file")) : std::string());
}

void StartupWatch::feed(const std::string& d, double now) {
  if (done) return;
  auto sp = splitIncomplete(carry_ + d);
  carry_ = sp.second;
  text_ += startupScreenText(sp.first);
  const size_t units = deck::session::utf16Length(text_);
  if (units > KEEP) text_ = deck::session::sliceFromUtf16(text_, units - KEEP);
  prompt = classifyStartup(text_, exists_);
  lastDataAt = now;
}
json StartupWatch::waitingOn(double now) const {
  if (done || !js::truthy(prompt) || pending) return json();
  return now - lastDataAt >= QUIET_MS ? prompt : json();
}
bool StartupWatch::wantsContinue(bool enabled, double lastKeyAt) const {
  if (done || pending || !enabled || !js::truthy(prompt) || js::get(prompt, "kind") != "settings-warning" || !js::truthy(js::get(prompt, "continueSelected"))) return false;
  if (lastKeyAt != 0 && lastKeyAt >= since) return false;
  return answers < MAX_ANSWERS;
}

// ---------------------------------------------------------------- the trust prompt (main.js trustWatch)
std::string trustAppend(const std::string& text, const std::string& d) {
  std::string out = text;
  for (size_t i = 0; i < d.size();) {                       // d.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, '')
    if (d[i] == '\x1b' && i + 1 < d.size() && d[i + 1] == '[') {
      size_t j = i + 2;
      while (j < d.size() && (isDigit(d[j]) || d[j] == ';' || d[j] == '?')) j++;
      if (j < d.size() && ((d[j] >= 'A' && d[j] <= 'Z') || (d[j] >= 'a' && d[j] <= 'z'))) { i = j + 1; continue; }
    }
    out += d[i++];
  }
  const size_t units = deck::session::utf16Length(out);
  return units > 8000 ? deck::session::sliceFromUtf16(out, units - 8000) : out;
}
bool trustOptionSeen(const std::string& s) { return findWords(s, 0, { "Yes,", "I", "trust", "this", "folder" }) != npos; }
bool trustPromptSeen(const std::string& s) { return findWords(s, 0, { "Quick", "safety", "check" }) != npos || trustOptionSeen(s); }
std::string trustMarkerLine(const std::string& s) {
  // /\n[ \t]*[❯>][ \t]+(No,\s*exit|Yes,\s*I\s*trust)/g — the last match
  std::string found;
  for (size_t p = s.find('\n'); p != npos; p = s.find('\n', p + 1)) {
    size_t i = p + 1;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < s.size() && s[i] == '>') i++;
    else if (s.compare(i, 3, MARK) == 0) i += 3;
    else continue;
    size_t j = i;
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j == i) continue;
    if (matchWords(s, j, { "No,", "exit" }) != npos) found = "no";
    else if (matchWords(s, j, { "Yes,", "I", "trust" }) != npos) found = "yes";
  }
  return found;
}

}}  // namespace deck::core
