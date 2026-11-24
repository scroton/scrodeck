// core/deck/viewer.cpp — see viewer.hpp. lib/viewer.js, function for function; each one names what it ports.
#include "viewer.hpp"
#include "hash.hpp"
#include "../session/jsutil.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>

namespace deck { namespace core {

namespace js = deck::session::js;
using ojson = nlohmann::ordered_json;

namespace {

const size_t npos = std::string::npos;
const size_t kMaxDirs = 300;                              // MAX_DIRS
const size_t kMaxList = 40;                               // MAX_LIST
const double kPrintedBeforeMs = 15 * 60000.0;             // PRINTED_BEFORE_MS
const double kPrintedAfterMs = 2 * 60000.0;               // PRINTED_AFTER_MS

inline bool isSep(char c) { return c == '/' || c == '\\'; }
inline bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool lineTerm(uint32_t c) { return c == 10 || c == 13 || c == 0x2028 || c == 0x2029; }
void putUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// HTML = /\.html?$/i; the extension's length through *ext
bool endsHtml(const std::string& p, size_t* ext = nullptr) {
  auto ends = [&](const char* e, size_t n) {
    if (p.size() < n) return false;
    for (size_t k = 0; k < n; k++) { char c = p[p.size() - n + k]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); if (c != e[k]) return false; }
    return true;
  };
  if (ends(".html", 5)) { if (ext) *ext = 5; return true; }
  if (ends(".htm", 4)) { if (ext) *ext = 4; return true; }
  return false;
}

// ---------------------------------------------------------------- Node's lib/path.js
// normalizeString(path, allowAboveRoot, separator, isPathSeparator). Its lengths are UTF-16 units in Node and
// bytes here; the only place a length decides (res.length > 2) gives the same result either way, because a `res`
// of one or two characters holds no separator.
std::string normalizeString(const std::string& path, bool allowAboveRoot, char separator, bool win) {
  auto sep = [&](char c) { return win ? isSep(c) : c == '/'; };
  std::string res; long lastSegmentLength = 0, lastSlash = -1; int dots = 0; char code = 0;
  const long n = (long)path.size();
  for (long i = 0; i <= n; ++i) {
    if (i < n) code = path[(size_t)i];
    else if (sep(code)) break;
    else code = '/';
    if (sep(code)) {
      if (lastSlash == i - 1 || dots == 1) {
        // NOOP
      } else if (dots == 2) {
        if (res.size() < 2 || lastSegmentLength != 2 || res[res.size() - 1] != '.' || res[res.size() - 2] != '.') {
          if (res.size() > 2) {
            const size_t lastSlashIndex = res.rfind(separator);
            if (lastSlashIndex == npos) { res.clear(); lastSegmentLength = 0; }
            else { res.resize(lastSlashIndex); const size_t prev = res.rfind(separator); lastSegmentLength = (long)res.size() - 1 - (prev == npos ? -1 : (long)prev); }
            lastSlash = i; dots = 0; continue;
          } else if (!res.empty()) {
            res.clear(); lastSegmentLength = 0; lastSlash = i; dots = 0; continue;
          }
        }
        if (allowAboveRoot) { res += res.empty() ? std::string("..") : std::string(1, separator) + ".."; lastSegmentLength = 2; }
      } else {
        if (!res.empty()) res += separator;
        res.append(path, (size_t)(lastSlash + 1), (size_t)(i - lastSlash - 1));
        lastSegmentLength = i - lastSlash - 1;
      }
      lastSlash = i; dots = 0;
    } else if (code == '.' && dots != -1) ++dots;
    else dots = -1;
  }
  return res;
}

// path.win32.normalize
std::string winNormalize(const std::string& path) {
  const size_t len = path.size();
  if (len == 0) return ".";
  size_t rootEnd = 0; std::string device; bool hasDevice = false, isAbsolute = false;
  const char code = path[0];
  if (len == 1) return code == '/' ? std::string("\\") : path;
  if (isSep(code)) {
    isAbsolute = true;                                    // possible UNC root
    if (isSep(path[1])) {
      size_t j = 2, last = j;
      while (j < len && !isSep(path[j])) j++;
      if (j < len && j != last) {
        const std::string firstPart = path.substr(last, j - last);
        last = j;
        while (j < len && isSep(path[j])) j++;
        if (j < len && j != last) {
          last = j;
          while (j < len && !isSep(path[j])) j++;
          if (j == len) return "\\\\" + firstPart + "\\" + path.substr(last) + "\\";   // a UNC root only
          if (j != last) { device = "\\\\" + firstPart + "\\" + path.substr(last, j - last); hasDevice = true; rootEnd = j; }
        }
      }
    } else rootEnd = 1;
  } else if (isAlpha(code) && path[1] == ':') {
    device = path.substr(0, 2); hasDevice = true; rootEnd = 2;
    if (len > 2 && isSep(path[2])) { isAbsolute = true; rootEnd = 3; }
  }
  std::string tail = rootEnd < len ? normalizeString(path.substr(rootEnd), !isAbsolute, '\\', true) : std::string();
  if (tail.empty() && !isAbsolute) tail = ".";
  if (!tail.empty() && isSep(path[len - 1])) tail += '\\';
  if (!isAbsolute && !hasDevice && path.find(':') != npos) {
    // a relative path must not become something Windows reads as a drive (Node's CVE-2024-36139 rule)
    if (tail.size() >= 2 && isAlpha(tail[0]) && tail[1] == ':') return ".\\" + tail;
    for (size_t index = path.find(':'); index != npos; index = path.find(':', index + 1))
      if (index == len - 1 || isSep(path[index + 1])) return ".\\" + tail;
  }
  if (!hasDevice) return isAbsolute ? "\\" + tail : tail;
  return isAbsolute ? device + "\\" + tail : device + tail;
}
// path.posix.normalize
std::string posixNormalize(const std::string& pathIn) {
  if (pathIn.empty()) return ".";
  const bool isAbsolute = pathIn[0] == '/', trailing = pathIn.back() == '/';
  std::string path = normalizeString(pathIn, !isAbsolute, '/', false);
  if (path.empty()) { if (isAbsolute) return "/"; return trailing ? "./" : "."; }
  if (trailing) path += '/';
  return isAbsolute ? "/" + path : path;
}
// path.win32.join
std::string winJoin(const std::vector<std::string>& parts) {
  std::string joined, firstPart; bool any = false;
  for (const std::string& a : parts) {
    if (a.empty()) continue;
    if (!any) { joined = firstPart = a; any = true; } else { joined += '\\'; joined += a; }
  }
  if (!any) return ".";
  // the joined path must not start with two slashes unless the first part was a UNC path
  bool needsReplace = true; size_t slashCount = 0;
  if (isSep(firstPart[0])) {
    ++slashCount;
    const size_t firstLen = firstPart.size();
    if (firstLen > 1 && isSep(firstPart[1])) {
      ++slashCount;
      if (firstLen > 2) { if (isSep(firstPart[2])) ++slashCount; else needsReplace = false; }
    }
  }
  if (needsReplace) {
    while (slashCount < joined.size() && isSep(joined[slashCount])) slashCount++;
    if (slashCount >= 2) joined = "\\" + joined.substr(slashCount);
  }
  return winNormalize(joined);
}
std::string posixJoin(const std::vector<std::string>& parts) {
  std::string joined; bool any = false;
  for (const std::string& a : parts) {
    if (a.empty()) continue;
    if (any) joined += '/';
    joined += a; any = true;
  }
  return any ? posixNormalize(joined) : std::string(".");
}
// path.win32.dirname
std::string winDirname(const std::string& path) {
  const long len = (long)path.size();
  if (len == 0) return ".";
  long rootEnd = -1, offset = 0;
  const char code = path[0];
  if (len == 1) return isSep(code) ? path : std::string(".");
  if (isSep(code)) {
    rootEnd = offset = 1;
    if (isSep(path[1])) {
      long j = 2, last = j;
      while (j < len && !isSep(path[(size_t)j])) j++;
      if (j < len && j != last) {
        last = j;
        while (j < len && isSep(path[(size_t)j])) j++;
        if (j < len && j != last) {
          last = j;
          while (j < len && !isSep(path[(size_t)j])) j++;
          if (j == len) return path;                      // a UNC root only
          if (j != last) rootEnd = offset = j + 1;
        }
      }
    }
  } else if (isAlpha(code) && path[1] == ':') {
    rootEnd = len > 2 && isSep(path[2]) ? 3 : 2;
    offset = rootEnd;
  }
  long end = -1; bool matchedSlash = true;
  for (long i = len - 1; i >= offset; --i) {
    if (isSep(path[(size_t)i])) { if (!matchedSlash) { end = i; break; } }
    else matchedSlash = false;
  }
  if (end == -1) { if (rootEnd == -1) return "."; end = rootEnd; }
  return path.substr(0, (size_t)end);
}
std::string posixDirname(const std::string& path) {
  if (path.empty()) return ".";
  const bool hasRoot = path[0] == '/';
  long end = -1; bool matchedSlash = true;
  for (long i = (long)path.size() - 1; i >= 1; --i) {
    if (path[(size_t)i] == '/') { if (!matchedSlash) { end = i; break; } }
    else matchedSlash = false;
  }
  if (end == -1) return hasRoot ? "/" : ".";
  if (hasRoot && end == 1) return "//";
  return path.substr(0, (size_t)end);
}
// path.basename(path), no suffix
std::string baseOf(const std::string& path, bool win) {
  long start = 0, end = -1; bool matchedSlash = true;
  if (win && path.size() >= 2 && isAlpha(path[0]) && path[1] == ':') start = 2;
  for (long i = (long)path.size() - 1; i >= start; --i) {
    const char c = path[(size_t)i];
    if (win ? isSep(c) : c == '/') { if (!matchedSlash) { start = i + 1; break; } }
    else if (end == -1) { matchedSlash = false; end = i + 1; }
  }
  if (end == -1) return std::string();
  return path.substr((size_t)start, (size_t)(end - start));
}
// path.extname
std::string extOf(const std::string& path, bool win) {
  long start = 0, startDot = -1, startPart = 0, end = -1; bool matchedSlash = true; int preDotState = 0;
  if (win && path.size() >= 2 && path[1] == ':' && isAlpha(path[0])) start = startPart = 2;
  for (long i = (long)path.size() - 1; i >= start; --i) {
    const char c = path[(size_t)i];
    if (win ? isSep(c) : c == '/') { if (!matchedSlash) { startPart = i + 1; break; } continue; }
    if (end == -1) { matchedSlash = false; end = i + 1; }
    if (c == '.') { if (startDot == -1) startDot = i; else if (preDotState != 1) preDotState = 1; }
    else if (startDot != -1) preDotState = -1;
  }
  if (startDot == -1 || end == -1 || preDotState == 0 || (preDotState == 1 && startDot == end - 1 && startDot == startPart + 1)) return std::string();
  return path.substr((size_t)startDot, (size_t)(end - startDot));
}

// sameKey(p, win)
std::string sameKey(const std::string& p, bool win) { return win ? viewerLowerCase(p) : p; }

// ---------------------------------------------------------------- PATH_RE, written out
// /(^|[\s(\[`'"=;:,>])((?:[A-Za-z]:[\\/]|~[\\/]|\/(?!\/))[^\r\n`'"<>|*?]{0,300}?\.html?)(?=$|[\s)\]`'",;:!?&<]|\.(?:\s|$))/gm
// std::regex would count bytes and has no JavaScript \s; this is the same automaton by hand, in code points, the
// {0,300} counted in UTF-16 units.
bool leadIn(uint32_t c) {
  return js::isSpace(c) || c == '(' || c == '[' || c == '`' || c == '\'' || c == '"' || c == '=' || c == ';' || c == ':' || c == ',' || c == '>';
}
// (?=$|[\s)\]`'",;:!?&<]|\.(?:\s|$)) at q; $ has the m flag: the end, or before a line terminator
bool pathEnds(const std::string& s, size_t q) {
  if (q >= s.size()) return true;
  size_t j = q; const uint32_t c = js::nextCodePoint(s, j);
  if (lineTerm(c) || js::isSpace(c)) return true;
  if (c == ')' || c == ']' || c == '`' || c == '\'' || c == '"' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '&' || c == '<') return true;
  if (c == '.') {
    if (j >= s.size()) return true;
    size_t k = j; return js::isSpace(js::nextCodePoint(s, k));     // every line terminator is \s too
  }
  return false;
}
// group 2 at p: where it ends, or npos
size_t matchPath(const std::string& s, size_t p) {
  const size_t n = s.size();
  size_t i = p;
  if (i >= n) return npos;
  if (isAlpha(s[i]) && i + 2 < n && s[i + 1] == ':' && isSep(s[i + 2])) i += 3;
  else if (s[i] == '~' && i + 1 < n && isSep(s[i + 1])) i += 2;
  else if (s[i] == '/' && !(i + 1 < n && s[i + 1] == '/')) i += 1;
  else return npos;
  size_t units = 0;
  for (;;) {                                              // the lazy body: the shortest run that ends the path
    if (s.compare(i, 4, ".htm") == 0) {
      if (i + 4 < n && s[i + 4] == 'l' && pathEnds(s, i + 5)) return i + 5;
      if (pathEnds(s, i + 4)) return i + 4;
    }
    if (i >= n) return npos;
    size_t j = i; const uint32_t c = js::nextCodePoint(s, j);
    if (c == '\r' || c == '\n' || c == '`' || c == '\'' || c == '"' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?') return npos;
    const size_t u = c >= 0x10000 ? 2 : 1;
    if (units + u > 300) return npos;
    units += u; i = j;
  }
}

// ---------------------------------------------------------------- JSON values the way JavaScript reads them
template <class J> const J* field(const J& o, const char* key) {
  if (!o.is_object()) return nullptr;
  auto it = o.find(key);
  return it == o.end() ? nullptr : &*it;
}
template <class J> bool truthyJ(const J& v) {
  if (v.is_null() || v.is_discarded()) return false;
  if (v.is_boolean()) return v.template get<bool>();
  if (v.is_string()) return !v.template get_ref<const std::string&>().empty();
  if (v.is_number()) { const double d = v.template get<double>(); return d != 0 && !std::isnan(d); }
  return true;
}
template <class J> bool isStrEq(const J* v, const char* want) { return v && v->is_string() && v->template get_ref<const std::string&>() == want; }
// an "array index" key: Object.entries lists these first, in ascending order
bool arrayIndex(const std::string& k, double* out) {
  if (k.empty() || k.size() > 10) return false;
  if (k.size() > 1 && k[0] == '0') return false;
  double v = 0;
  for (char c : k) { if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
  if (v > 4294967294.0) return false;
  *out = v; return true;
}

}  // namespace

// ---------------------------------------------------------------- the exported path helpers
std::string viewerPathNormalize(const std::string& p, bool win) { return win ? winNormalize(p) : posixNormalize(p); }
std::string viewerPathJoin(const std::vector<std::string>& parts, bool win) { return win ? winJoin(parts) : posixJoin(parts); }
std::string viewerPathDirname(const std::string& p, bool win) { return win ? winDirname(p) : posixDirname(p); }
std::string viewerPathBasename(const std::string& p, bool win) { return baseOf(p, win); }
std::string viewerPathExtname(const std::string& p, bool win) { return extOf(p, win); }
bool viewerPathIsAbsolute(const std::string& p, bool win) {
  if (p.empty()) return false;
  if (!win) return p[0] == '/';
  if (isSep(p[0])) return true;
  return p.size() > 2 && isAlpha(p[0]) && p[1] == ':' && isSep(p[2]);
}

std::string viewerLowerCase(const std::string& s) {
  std::string out; out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char b = (unsigned char)s[i];
    if (b < 0x80) { out += (b >= 'A' && b <= 'Z') ? (char)(b + 32) : (char)b; i++; continue; }
    size_t j = i; const uint32_t cp = js::nextCodePoint(s, j);
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) putUtf8(out, cp + 0x20);
    else if (cp >= 0x400 && cp <= 0x40F) putUtf8(out, cp + 0x50);
    else if (cp >= 0x410 && cp <= 0x42F) putUtf8(out, cp + 0x20);
    else out.append(s, i, j - i);
    i = j;
  }
  return out;
}

std::string viewerUtf8(const std::string& in) {
  const size_t n = in.size();
  std::string out; out.reserve(n);
  for (size_t i = 0; i < n;) {
    const unsigned char c = (unsigned char)in[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    int need = 0; unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
    else { out += "\xEF\xBF\xBD"; i++; continue; }
    size_t j = i + 1; int got = 0;
    while (got < need && j < n) {
      const unsigned char d = (unsigned char)in[j];
      if (d < lo || d > hi) break;
      lo = 0x80; hi = 0xBF; j++; got++;
    }
    if (got == need) out.append(in, i, j - i); else out += "\xEF\xBF\xBD";   // the maximal ill-formed part is one U+FFFD
    i = j;
  }
  return out;
}

std::string viewerJsString(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null() || v.is_discarded()) return "null";
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number_float()) return js::numberText(v.get<double>());
  if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
  if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
  if (v.is_array()) {                                     // Array.prototype.join: null prints as nothing
    std::string out; bool first = true;
    for (const json& x : v) { if (!first) out += ','; first = false; if (!x.is_null()) out += viewerJsString(x); }
    return out;
  }
  return "[object Object]";
}

// ---------------------------------------------------------------- pathsIn
// The JS returns early when /\.htm/i finds nothing; the pattern itself needs ".htm", so that test is only a
// short cut and is not repeated here.
std::vector<std::string> viewerPathsIn(const std::string& text) {
  std::vector<std::string> out;
  if (text.find(".htm") == npos) return out;
  const size_t n = text.size();
  size_t at = 0; bool lineStart = true;                   // exec resumes at lastIndex: the lead-in is consumed, the lookahead is not
  while (at <= n) {
    size_t end = npos, from = 0;
    if (lineStart) { end = matchPath(text, at); from = at; }                         // (^|…): ^ first
    size_t next = at; uint32_t c = 0;
    if (at < n) c = js::nextCodePoint(text, next);
    if (end == npos && at < n && leadIn(c)) { end = matchPath(text, next); from = next; }
    if (end != npos) { out.push_back(text.substr(from, end - from)); at = end; lineStart = false; continue; }
    if (at >= n) break;
    lineStart = lineTerm(c); at = next;
  }
  return out;
}

// ---------------------------------------------------------------- resolvePath / htmlFile
std::string viewerResolvePath(const std::string& raw, const std::string& home, bool win) {
  std::string p = js::trim(raw);
  auto quote = [](char c) { return c == '`' || c == '\'' || c == '"'; };
  size_t a = 0, b = p.size();
  while (a < b && quote(p[a])) a++;                       // .replace(/^[`'"]+|[`'"]+$/g, '')
  while (b > a && quote(p[b - 1])) b--;
  p = p.substr(a, b - a);
  if (p.empty() || p.find('\0') != npos) return std::string();
  if (p.size() >= 2 && p[0] == '~' && isSep(p[1])) p = viewerPathJoin({ home, p.substr(2) }, win);
  else if (win && p.size() >= 3 && p[0] == '/' && isAlpha(p[1]) && p[2] == '/') {
    const char drive = p[1] >= 'a' ? (char)(p[1] - 32) : p[1];
    p = std::string(1, drive) + ":\\" + p.substr(3);
  }
  if (win ? !(p.size() >= 3 && isAlpha(p[0]) && p[1] == ':' && isSep(p[2])) : !(!p.empty() && p[0] == '/')) return std::string();
  return viewerPathNormalize(p, win);
}

std::string viewerHtmlFile(Files& files, const std::string& raw, const std::string& home, bool win) {
  const std::string p = viewerResolvePath(raw, home, win);
  // .slice(0, 120): a cut inside a surrogate pair drops the whole character here (the JS keeps half of it)
  if (p.empty()) throw std::runtime_error("not a full path: " + js::sliceUnits(raw, 120));
  if (!endsHtml(p)) throw std::runtime_error("not an .html file: " + viewerPathBasename(p, win));
  const FileStat st = files.stat(p);
  if (!st.exists) throw std::runtime_error("not on this PC: " + p);
  if (st.isDir) throw std::runtime_error("not a file: " + p);
  return p;
}

// ---------------------------------------------------------------- Viewer
Viewer::Viewer(bool win) : win_(win) {}

std::string Viewer::idFor(const std::string& dir) const {
  return hexOf(sha256(sameKey(viewerPathNormalize(dir, win_), win_))).substr(0, 16);
}
bool Viewer::has(const std::string& id) const {
  for (const auto& d : dirs_) if (d.first == id) return true;
  return false;
}
std::vector<std::string> Viewer::ids() const {
  std::vector<std::string> out;
  for (const auto& d : dirs_) out.push_back(d.first);
  return out;
}
json Viewer::show(const std::string& file) {
  const std::string dir = viewerPathDirname(viewerPathNormalize(file, win_), win_);
  const std::string id = idFor(dir);
  for (size_t i = 0; i < dirs_.size(); i++) if (dirs_[i].first == id) { dirs_.erase(dirs_.begin() + (long)i); break; }
  dirs_.emplace_back(id, dir);
  while (dirs_.size() > kMaxDirs) dirs_.erase(dirs_.begin());
  return { { "dir", id }, { "name", viewerPathBasename(file, win_) } };
}
std::string Viewer::fileFor(Files& files, const std::string& id, const std::vector<std::string>& parts) const {
  const std::string* dirp = nullptr;
  for (const auto& d : dirs_) if (d.first == id) { dirp = &d.second; break; }
  if (!dirp || dirp->empty() || parts.empty() || parts.size() > 8) return std::string();
  const std::string& dir = *dirp;
  for (const std::string& s : parts) {
    if (s.empty() || s[0] == '.') return std::string();
    for (char c : s) if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\0') return std::string();
  }
  // ASSETS. toLowerCase: ASCII is exact here (no other character lower-cases into one of these extensions)
  static const char* const assets[] = { ".html", ".htm", ".css", ".js", ".mjs", ".svg", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".bmp", ".ico", ".woff", ".woff2", ".ttf", ".otf" };
  std::string ext = viewerPathExtname(parts.back(), win_);
  for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  bool asset = false;
  for (const char* a : assets) if (ext == a) { asset = true; break; }
  if (!asset) return std::string();
  std::vector<std::string> all; all.push_back(dir); all.insert(all.end(), parts.begin(), parts.end());
  const std::string f = viewerPathJoin(all, win_);
  const char sep = win_ ? '\\' : '/';
  const std::string base = sameKey(dir.back() == sep ? dir : dir + sep, win_);
  if (sameKey(f, win_).compare(0, base.size(), base) != 0) return std::string();
  return files.isFile(f) ? f : std::string();
}

// ---------------------------------------------------------------- sessionHtml
json viewerSessionHtml(Files& files, const std::string& jsonlIn, const std::string& home, bool win) {
  // the JS is handed the transcript as fs.readFileSync(file, 'utf8') made it: ill-formed bytes are U+FFFD already
  const std::string jsonl = viewerUtf8(jsonlIn);
  struct Seen { std::string p; long n; };
  std::map<std::string, Seen> seen;
  long n = 0;
  auto note = [&](const std::string& raw) {
    const std::string p = viewerResolvePath(raw, home, win);
    if (p.empty() || !endsHtml(p)) return;
    seen[sameKey(p, win)] = Seen{ p, n++ };               // seen.delete(k); seen.set(k, …): only n orders them
  };
  // SKIP_KEYS: a file's body; its hrefs are not pages it made
  auto skipKey = [](const std::string& k) { return k == "content" || k == "new_string" || k == "old_string" || k == "edits" || k == "new_source"; };
  std::function<void(const ojson&, int)> walk = [&](const ojson& v, int depth) {
    if (depth > 4000) return;                             // (the JS would overflow its stack and throw long before this)
    if (v.is_string()) {
      const std::string& s = v.get_ref<const std::string&>();
      if (endsHtml(js::trim(s)) && !viewerResolvePath(s, home, win).empty()) note(s);
      else for (const std::string& p : viewerPathsIn(s)) note(p);
    } else if (v.is_array()) {
      for (const ojson& x : v) walk(x, depth + 1);
    } else if (v.is_object()) {
      // Object.entries: the keys that are array indexes first, ascending; then the rest as they were written
      std::vector<std::pair<double, const ojson*>> indexed; std::vector<const ojson*> rest;
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (skipKey(it.key())) continue;
        double ix = 0;
        if (arrayIndex(it.key(), &ix)) indexed.emplace_back(ix, &it.value()); else rest.push_back(&it.value());
      }
      std::stable_sort(indexed.begin(), indexed.end(), [](const std::pair<double, const ojson*>& a, const std::pair<double, const ojson*>& b) { return a.first < b.first; });
      for (const auto& x : indexed) walk(*x.second, depth + 1);
      for (const ojson* x : rest) walk(*x, depth + 1);
    }
  };
  // a path a tool printed counts when the file was written close to that output: made by it, not just listed
  auto printed = [&](const std::string& raw, double at) {
    const std::string p = viewerResolvePath(raw, home, win);
    if (p.empty() || !endsHtml(p) || !std::isfinite(at)) return;
    const FileStat st = files.stat(p);
    if (!st.exists) return;
    if (st.mtimeMs >= at - kPrintedBeforeMs && st.mtimeMs <= at + kPrintedAfterMs) note(p);
  };
  auto resultText = [](const ojson* c) -> std::string {
    if (!c) return std::string();
    if (c->is_string()) return c->get<std::string>();
    if (!c->is_array()) return std::string();
    std::string out; bool first = true;
    for (const ojson& x : *c) {
      if (!first) out += '\n';
      first = false;
      const ojson* t = field(x, "text");
      if (t && t->is_string()) out += t->get_ref<const std::string&>();
    }
    return out;
  };
  auto hasHtm = [](const std::string& s, size_t a, size_t b) {             // /\.htm/i
    for (size_t i = a; i + 4 <= b; i++)
      if (s[i] == '.' && (s[i + 1] | 32) == 'h' && (s[i + 2] | 32) == 't' && (s[i + 3] | 32) == 'm') return true;
    return false;
  };
  for (size_t a = 0; a <= jsonl.size();) {
    size_t b = jsonl.find('\n', a);
    if (b == npos) b = jsonl.size();
    const size_t lineStart = a; a = b + 1;
    if (!hasHtm(jsonl, lineStart, b)) continue;           // most lines: no page named, not parsed
    if (jsonl.compare(lineStart, 3, "\xEF\xBB\xBF") == 0) continue;   // JSON.parse refuses a byte order mark (nlohmann would skip it)
    const ojson o = ojson::parse(jsonl.begin() + (long)lineStart, jsonl.begin() + (long)b, nullptr, false);
    if (o.is_discarded()) continue;
    const ojson* message = field(o, "message");
    const ojson* content = message ? field(*message, "content") : nullptr;
    if (!content || !content->is_array()) continue;
    const ojson* type = field(o, "type");
    if (isStrEq(type, "assistant")) {
      for (const ojson& blk : *content) {
        const ojson* bt = field(blk, "type");
        if (isStrEq(bt, "text")) {
          const ojson* text = field(blk, "text");
          if (text && text->is_string()) for (const std::string& p : viewerPathsIn(text->get_ref<const std::string&>())) note(p);
        } else if (isStrEq(bt, "tool_use")) {
          const ojson* input = field(blk, "input");
          if (input && truthyJ(*input)) walk(*input, 0);
        }
      }
    } else if (isStrEq(type, "user")) {
      // Date.parse(o.timestamp): the ISO forms a transcript carries (js::dateParse); anything else, and a
      // timestamp that is not a string, is NaN here (JavaScript's Date.parse also reads legacy date text)
      const ojson* ts = field(o, "timestamp");
      const double at = ts && ts->is_string() ? js::dateParse(ts->get_ref<const std::string&>()) : std::numeric_limits<double>::quiet_NaN();
      for (const ojson& blk : *content) {
        if (!isStrEq(field(blk, "type"), "tool_result")) continue;
        for (const std::string& p : viewerPathsIn(resultText(field(blk, "content")))) printed(p, at);
      }
    }
  }
  std::vector<const Seen*> order;
  for (const auto& kv : seen) order.push_back(&kv.second);
  std::sort(order.begin(), order.end(), [](const Seen* x, const Seen* y) { return x->n > y->n; });
  json out = json::array();
  for (const Seen* s : order) {
    const FileStat st = files.stat(s->p);
    if (!st.exists || st.isDir) continue;
    out.push_back({ { "path", s->p }, { "name", viewerPathBasename(s->p, win) }, { "folder", viewerPathDirname(s->p, win) }, { "mtime", st.mtimeMs }, { "size", st.size } });
    if (out.size() >= kMaxList) break;
  }
  return out;
}

// ---------------------------------------------------------------- main.js cleanPages
json cleanPages(const json& list, bool win) {
  json out = json::array();
  if (!list.is_array()) return out;
  for (const json& p : list) {
    if (!p.is_object()) continue;                         // !p || typeof p !== 'object' (an array has no .file: dropped below in the JS, here)
    const json* fv = field(p, "file");
    std::string file = js::trim(fv && truthyJ(*fv) ? viewerJsString(*fv) : std::string());
    // .replace(/^"(.*)"$/, '$1'): the dot stops at a line terminator
    if (file.size() >= 2 && file.front() == '"' && file.back() == '"') {
      bool oneLine = true;
      for (size_t i = 1; i + 1 < file.size();) { size_t j = i; if (lineTerm(js::nextCodePoint(file, j))) { oneLine = false; break; } i = j; }
      if (oneLine) file = file.substr(1, file.size() - 2);
    }
    size_t ext = 0;
    if (file.empty() || !viewerPathIsAbsolute(file, win) || !endsHtml(file, &ext)) continue;
    const json* lv = field(p, "label");
    std::string label = js::sliceUnits(js::trim(lv && truthyJ(*lv) ? viewerJsString(*lv) : std::string()), 24);
    if (label.empty()) { label = viewerPathBasename(file, win); size_t e = 0; if (endsHtml(label, &e)) label.resize(label.size() - e); }
    out.push_back({ { "label", label }, { "file", file } });
    if (out.size() >= 12) break;
  }
  return out;
}

// ---------------------------------------------------------------- lib/phone.js: what the /view/ route serves with
const char* const kPageLinks = "<script>/* claude-deck: the page viewer */document.addEventListener(\"click\",function(e){"
  "var a=e.target&&e.target.closest?e.target.closest(\"a[href]\"):null;"
  "if(!a||e.defaultPrevented||e.button!==0||e.metaKey||e.ctrlKey||e.shiftKey||a.hasAttribute(\"download\"))return;"
  "var u;try{u=new URL(a.getAttribute(\"href\"),location.href);}catch(x){return;}"
  "if(!/^https?:$/.test(u.protocol))return;"
  "e.preventDefault();"
  "if(u.origin===location.origin)location.replace(u.href);else window.open(u.href,\"_blank\",\"noopener\");"
  "});</script>";
const char* const kViewCsp = "sandbox allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads allow-modals; "
  "default-src 'self' data: blob: https:; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob: https:; "
  "style-src 'self' 'unsafe-inline' https:; img-src 'self' data: blob: https:; font-src 'self' data: https:; "
  "connect-src 'self' https:; media-src 'self' data: blob: https:; frame-src 'self' data: blob: https:; form-action 'none'";

const char* viewContentType(const std::string& ext) {
  static const char* const types[][2] = {
    { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
    { ".js", "text/javascript; charset=utf-8" }, { ".mjs", "text/javascript; charset=utf-8" }, { ".svg", "image/svg+xml" }, { ".png", "image/png" },
    { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".webp", "image/webp" }, { ".avif", "image/avif" }, { ".bmp", "image/bmp" },
    { ".ico", "image/x-icon" }, { ".woff", "font/woff" }, { ".woff2", "font/woff2" }, { ".ttf", "font/ttf" }, { ".otf", "font/otf" } };
  for (const auto& t : types) if (ext == t[0]) return t[1];
  return "application/octet-stream";
}

// withPageLinks(buf). The JS decodes the file as UTF-8 first (ill-formed bytes become U+FFFD); here the page's
// bytes go out as they are, with the script put in at the same place.
std::string withPageLinks(const std::string& html) {
  static const char tag[] = "</body";
  const size_t n = html.size();
  for (size_t i = 0; i + 7 <= n; i++) {
    if (html[i] != '<') continue;
    bool hit = true;
    for (size_t k = 1; k < 6; k++) { char c = html[i + k]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != tag[k]) { hit = false; break; } }
    if (!hit) continue;
    size_t j = i + 6;
    while (j < n) { size_t k = j; if (!js::isSpace(js::nextCodePoint(html, k))) break; j = k; }
    if (j < n && html[j] == '>') return html.substr(0, i) + kPageLinks + html.substr(i);
  }
  return html + kPageLinks;
}

}}  // namespace deck::core
