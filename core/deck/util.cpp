// core/deck/util.cpp — see util.hpp.
#include "util.hpp"
#include <cstdint>
#include <cstdio>

namespace deck { namespace core {

static char g_sep = '\\';
char pathSep() { return g_sep; }
void setPathSep(char sep) { g_sep = sep; }

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  const char last = a.back();
  return (last == '\\' || last == '/') ? a + b : a + g_sep + b;
}
std::string dirName(const std::string& path) {
  const size_t p = path.find_last_of("\\/");
  return p == std::string::npos ? std::string() : path.substr(0, p);
}
std::string baseName(const std::string& path) {
  const size_t p = path.find_last_of("\\/");
  return p == std::string::npos ? path : path.substr(p + 1);
}
std::string lowerAscii(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
std::string extName(const std::string& path) {
  const std::string b = baseName(path);
  const size_t dot = b.rfind('.');
  return dot == std::string::npos || dot == 0 ? std::string() : lowerAscii(b.substr(dot));
}

std::string isoTime(double ms) {
  const int64_t t = (int64_t)ms;
  int64_t days = t / 86400000; int64_t rem = t % 86400000;
  if (rem < 0) { rem += 86400000; days--; }
  // civil from days (Howard Hinnant)
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;
  char b[40];
  std::snprintf(b, sizeof b, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", (long long)y, m, d, (int)(rem / 3600000), (int)(rem / 60000 % 60), (int)(rem / 1000 % 60), (int)(rem % 1000));
  return b;
}
std::string isoStamp(double ms) { std::string s = isoTime(ms); for (char& c : s) if (c == ':' || c == '.') c = '-'; return s; }

json parseState(const std::string& textIn) {
  json bad = json(json::value_t::discarded);
  bool any = false;
  for (unsigned char c : textIn) if (c != 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f') { any = true; break; }
  if (!any) return bad;
  // .replace(/^﻿/, ''): one BOM is taken off; a second is an error either way
  const std::string text = textIn.compare(0, 3, "\xEF\xBB\xBF") == 0 ? textIn.substr(3) : textIn;
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) return bad;
  json v = json::parse(text, nullptr, false);
  if (v.is_discarded() || !(v.is_object() || v.is_array())) return bad;
  return v;
}

}}  // namespace deck::core
