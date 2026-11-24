// core/session/jsutil.hpp — the handful of JavaScript behaviours the session ports lean on, so that
// `a || b`, `String(x)`, `s.trim()`, `s.slice(0, 80)` and `Date.parse(iso)` mean here what they mean in the
// JS they were ported from. Header-only, pure.
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>

namespace deck { namespace session { namespace js {

using json = nlohmann::json;

inline const json& null() { static const json n; return n; }
// obj.key for anything: a missing key, or a non-object, reads as null (JS undefined)
inline const json& get(const json& o, const char* key) {
  if (!o.is_object()) return null();
  auto it = o.find(key);
  return it == o.end() ? null() : *it;
}
inline bool truthy(const json& v) {
  switch (v.type()) {
    case json::value_t::null: case json::value_t::discarded: return false;
    case json::value_t::boolean: return v.get<bool>();
    case json::value_t::string: return !v.get_ref<const std::string&>().empty();
    case json::value_t::number_integer: return v.get<int64_t>() != 0;
    case json::value_t::number_unsigned: return v.get<uint64_t>() != 0;
    case json::value_t::number_float: { double d = v.get<double>(); return d != 0 && !std::isnan(d); }
    default: return true;                 // objects and arrays
  }
}
// a || b
inline const json& orElse(const json& a, const json& b) { return truthy(a) ? a : b; }
inline bool isStr(const json& v) { return v.is_string(); }
inline const std::string& str(const json& v) { static const std::string e; return v.is_string() ? v.get_ref<const std::string&>() : e; }

// Number -> the text JavaScript prints for it (integers plainly; the rest by shortest round-trip)
inline std::string numberText(double d) {
  if (std::isnan(d)) return "NaN";
  if (std::isinf(d)) return d < 0 ? "-Infinity" : "Infinity";
  if (d == std::floor(d) && std::fabs(d) < 1e21) { char b[40]; std::snprintf(b, sizeof b, "%.0f", d); return b; }
  return json(d).dump();
}
// String(x) for the values a transcript field can hold. Objects and arrays do not occur where this is used;
// they print as their JSON, which is not what JS prints ("[object Object]").
inline std::string toString(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return "null";
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number()) return numberText(v.get<double>());
  return v.dump();
}
// +x / Number(x): NaN for what is not a number
inline double toNumber(const json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
  if (v.is_null()) return 0;                                   // +null is 0 (undefined would be NaN: see finiteNumber)
  if (v.is_string()) {
    const std::string& s = v.get_ref<const std::string&>();
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    if (a == b) return 0;
    char* end = nullptr; const std::string t = s.substr(a, b - a);
    double d = std::strtod(t.c_str(), &end);
    return end && *end == 0 ? d : std::numeric_limits<double>::quiet_NaN();
  }
  return std::numeric_limits<double>::quiet_NaN();
}
// Number.isFinite(+o.key), where a missing key is undefined (NaN) and an explicit null is 0
inline bool finiteNumber(const json& o, const char* key, double* out) {
  if (!o.is_object() || o.find(key) == o.end()) return false;
  double d = toNumber(o[key]);
  if (!std::isfinite(d)) return false;
  if (out) *out = d;
  return true;
}
inline double round(double x) { return std::floor(x + 0.5); }   // Math.round

// ---- strings (UTF-8 in, JavaScript semantics)
// one code point at s[i]; advances i. Ill-formed bytes come back one at a time as themselves.
inline uint32_t nextCodePoint(const std::string& s, size_t& i) {
  const unsigned char c = (unsigned char)s[i];
  int len = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 1;
  if (i + len > s.size()) len = 1;
  uint32_t cp = c;
  if (len == 2) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
  else if (len == 3) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
  else if (len == 4) cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
  i += len;
  return cp;
}
// \s in a JavaScript regular expression, and what trim() removes
inline bool isSpace(uint32_t cp) {
  return cp == 9 || cp == 10 || cp == 11 || cp == 12 || cp == 13 || cp == 32 || cp == 0xA0 || cp == 0x1680
    || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}
inline std::string trim(const std::string& s) {
  size_t a = 0;
  while (a < s.size()) { size_t j = a; if (!isSpace(nextCodePoint(s, j))) break; a = j; }
  size_t b = s.size(), i = a, lastNonSpaceEnd = a;
  while (i < b) { size_t j = i; const bool sp = isSpace(nextCodePoint(s, j)); if (!sp) lastNonSpaceEnd = j; i = j; }
  return s.substr(a, lastNonSpaceEnd - a);
}
// s.replace(/\s+/g, ' ')
inline std::string collapseSpaces(const std::string& s) {
  std::string out; bool in = false;
  for (size_t i = 0; i < s.size();) {
    size_t j = i; const bool sp = isSpace(nextCodePoint(s, j));
    if (sp) { if (!in) out += ' '; in = true; } else { out.append(s, i, j - i); in = false; }
    i = j;
  }
  return out;
}
// s.slice(0, n) with n in UTF-16 units (a cut inside a surrogate pair drops the whole character here)
inline std::string sliceUnits(const std::string& s, size_t n) {
  size_t i = 0, units = 0;
  while (i < s.size()) {
    size_t j = i; const uint32_t cp = nextCodePoint(s, j);
    const size_t u = cp >= 0x10000 ? 2 : 1;
    if (units + u > n) break;
    units += u; i = j;
  }
  return s.substr(0, i);
}
inline bool isWordChar(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
inline bool startsWith(const std::string& s, const char* p, size_t at = 0) { const size_t n = std::char_traits<char>::length(p); return s.size() >= at + n && s.compare(at, n, p) == 0; }

// ---- time
inline int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
// local wall-clock fields -> ms epoch (new Date(y, mon, d, h, mi, s).getTime())
inline double localToEpochMs(int y, int monthIndex, int d, int h, int mi, int s) {
  std::tm tm = {};
  tm.tm_year = y - 1900; tm.tm_mon = monthIndex; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s; tm.tm_isdst = -1;
  const std::time_t t = std::mktime(&tm);
  return t == (std::time_t)-1 ? std::numeric_limits<double>::quiet_NaN() : (double)t * 1000.0;
}
inline int localYear(double epochMs) {
  const std::time_t t = (std::time_t)std::floor(epochMs / 1000.0);
  std::tm* tm = std::localtime(&t);
  return tm ? tm->tm_year + 1900 : 1970;
}
// Date.parse for the ISO 8601 forms a transcript carries: YYYY-MM-DD, and YYYY-MM-DDTHH:MM[:SS[.fff…]] with
// Z, ±HH:MM, or no zone (then local time, as JavaScript reads it). Anything else is NaN.
inline double dateParse(const std::string& s) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto digits = [&](size_t at, size_t n, int& out) {
    if (at + n > s.size()) return false;
    int v = 0;
    for (size_t k = 0; k < n; k++) { const char c = s[at + k]; if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
    out = v; return true;
  };
  int y, mo, d, h = 0, mi = 0, sec = 0; double frac = 0;
  if (!digits(0, 4, y) || s.size() < 10 || s[4] != '-' || !digits(5, 2, mo) || s[7] != '-' || !digits(8, 2, d)) return nan;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return nan;
  size_t i = 10;
  if (i == s.size()) return (double)daysFromCivil(y, mo, d) * 86400000.0;          // date only: UTC
  if (s[i] != 'T' && s[i] != 't' && s[i] != ' ') return nan;
  i++;
  if (!digits(i, 2, h) || i + 2 >= s.size() || s[i + 2] != ':' || !digits(i + 3, 2, mi)) return nan;
  i += 5;
  if (i < s.size() && s[i] == ':') { if (!digits(i + 1, 2, sec)) return nan; i += 3; }
  if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
    i++; double scale = 100; int n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { if (n < 3) { frac += (s[i] - '0') * scale; scale /= 10; } n++; i++; }
    if (!n) return nan;
  }
  if (h > 24 || mi > 59 || sec > 59) return nan;
  const double wall = (double)daysFromCivil(y, mo, d) * 86400000.0 + h * 3600000.0 + mi * 60000.0 + sec * 1000.0 + frac;
  if (i == s.size()) {                                                              // no zone: local time
    const double base = localToEpochMs(y, mo - 1, d, h, mi, sec);
    return std::isnan(base) ? nan : base + frac;
  }
  if (s[i] == 'Z' || s[i] == 'z') return i + 1 == s.size() ? wall : nan;
  if (s[i] == '+' || s[i] == '-') {
    int oh, om = 0; const int sign = s[i] == '-' ? -1 : 1;
    if (!digits(i + 1, 2, oh)) return nan;
    size_t j = i + 3;
    if (j < s.size() && s[j] == ':') j++;
    if (j < s.size()) { if (!digits(j, 2, om)) return nan; j += 2; }
    if (j != s.size()) return nan;
    return wall - sign * (oh * 3600000.0 + om * 60000.0);
  }
  return nan;
}

}}}  // namespace deck::session::js
