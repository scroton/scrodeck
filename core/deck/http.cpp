// core/deck/http.cpp — see http.hpp.
#include "http.hpp"
#include "util.hpp"
#include <algorithm>

namespace deck { namespace core {

static const size_t npos = std::string::npos;
static int hexVal(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static std::string trimOws(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
  return s.substr(a, b - a);
}

const std::string& HttpRequest::header(const std::string& name) const {
  static const std::string none;
  for (const auto& kv : headers) if (kv.first == name) return kv.second;
  return none;
}
bool HttpRequest::hasToken(const std::string& name, const std::string& token) const {
  for (const auto& kv : headers) {
    if (kv.first != name) continue;
    const std::string v = lowerAscii(kv.second);
    size_t a = 0;
    while (a <= v.size()) {
      size_t c = v.find(',', a); if (c == npos) c = v.size();
      if (trimOws(v.substr(a, c - a)) == token) return true;
      a = c + 1;
    }
  }
  return false;
}
bool HttpRequest::wantsUpgrade() const { return hasToken("connection", "upgrade") && !header("upgrade").empty(); }
bool HttpRequest::keepAlive() const {
  if (hasToken("connection", "close")) return false;
  if (version == "HTTP/1.0") return hasToken("connection", "keep-alive");
  return true;
}

RequestReader::Result RequestReader::feed(const char* bytes, size_t n) { buf_.append(bytes, n); return poll(); }
void RequestReader::next() { state_ = Head; req_ = HttpRequest(); need_ = 0; sizeLine_ = true; trailer_ = false; }
std::string RequestReader::takeRest() { std::string r; r.swap(buf_); return r; }

RequestReader::Result RequestReader::poll() {
  if (state_ == Done) return Ready;
  if (state_ == Head) {
    // a stray CRLF before the request line is allowed (RFC 9112 2.2)
    while (buf_.compare(0, 2, "\r\n") == 0) buf_.erase(0, 2);
    const size_t end = buf_.find("\r\n\r\n");
    if (end == npos) return buf_.size() > kMaxHead ? TooLarge : More;
    if (end > kMaxHead) return TooLarge;
    const std::string head = buf_.substr(0, end);
    buf_.erase(0, end + 4);
    size_t lineEnd = head.find("\r\n"); if (lineEnd == npos) lineEnd = head.size();
    const std::string line = head.substr(0, lineEnd);
    const size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == npos || s2 == s1) return Bad;
    req_.method = line.substr(0, s1); req_.target = line.substr(s1 + 1, s2 - s1 - 1); req_.version = line.substr(s2 + 1);
    if (req_.method.empty() || req_.target.empty() || (req_.version != "HTTP/1.1" && req_.version != "HTTP/1.0")) return Bad;
    for (char c : req_.method) if (!(c >= 'A' && c <= 'Z')) return Bad;
    for (char c : req_.target) if ((unsigned char)c <= ' ' || c == 0x7f) return Bad;
    size_t at = lineEnd;
    while (at < head.size()) {
      at += 2;
      size_t e = head.find("\r\n", at); if (e == npos) e = head.size();
      const std::string h = head.substr(at, e - at);
      at = e;
      const size_t colon = h.find(':');
      if (colon == npos || colon == 0 || h[0] == ' ' || h[0] == '\t') return Bad;
      std::string name = lowerAscii(h.substr(0, colon));
      for (char c : name) if ((unsigned char)c <= ' ' || c == 0x7f) return Bad;
      req_.headers.emplace_back(name, trimOws(h.substr(colon + 1)));
    }
    if (req_.hasToken("transfer-encoding", "chunked")) { state_ = Chunks; sizeLine_ = true; trailer_ = false; }
    else {
      const std::string& cl = req_.header("content-length");
      uint64_t n = 0;
      if (!cl.empty()) { for (char c : cl) { if (c < '0' || c > '9') return Bad; n = n * 10 + (uint64_t)(c - '0'); if (n > kMaxBody) return TooLarge; } }
      need_ = n; state_ = Body;
    }
  }
  if (state_ == Body) {
    if (buf_.size() < need_) return More;
    req_.body = buf_.substr(0, (size_t)need_);
    buf_.erase(0, (size_t)need_);
    state_ = Done;
    return Ready;
  }
  // chunked
  for (;;) {
    if (trailer_) {
      const size_t e = buf_.find("\r\n");
      if (e == npos) return buf_.size() > kMaxHead ? Bad : More;
      const bool blank = e == 0;
      buf_.erase(0, e + 2);
      if (blank) { state_ = Done; return Ready; }
      continue;
    }
    if (sizeLine_) {
      const size_t e = buf_.find("\r\n");
      if (e == npos) return buf_.size() > 1024 ? Bad : More;
      uint64_t n = 0; size_t k = 0;
      for (; k < e && hexVal(buf_[k]) >= 0; k++) { n = n * 16 + (uint64_t)hexVal(buf_[k]); if (n > kMaxBody) return TooLarge; }
      if (k == 0 || (k < e && buf_[k] != ';' && buf_[k] != ' ')) return Bad;
      buf_.erase(0, e + 2);
      if (n == 0) { trailer_ = true; continue; }
      if (req_.body.size() + n > kMaxBody) return TooLarge;
      need_ = n; sizeLine_ = false;
    }
    if (buf_.size() < need_ + 2) return More;
    if (buf_.compare((size_t)need_, 2, "\r\n") != 0) return Bad;
    req_.body.append(buf_, 0, (size_t)need_);
    buf_.erase(0, (size_t)need_ + 2);
    sizeLine_ = true;
  }
}

// ---------------------------------------------------------------- URLs
static bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    int n; uint32_t cp;
    if (c < 0x80) { i++; continue; }
    else if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
    else return false;
    for (int k = 1; k <= n; k++) { if (i + k >= s.size()) return false; const unsigned char cc = (unsigned char)s[i + k]; if ((cc & 0xC0) != 0x80) return false; cp = (cp << 6) | (cc & 0x3F); }
    if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += n + 1;
  }
  return true;
}
bool percentDecode(const std::string& in, std::string& out) {
  out.clear();
  // decodeURIComponent decodes each run of %XX as UTF-8 and throws on a run that is not; literal text is kept
  for (size_t i = 0; i < in.size();) {
    if (in[i] != '%') { out += in[i++]; continue; }
    std::string run;
    while (i < in.size() && in[i] == '%') {
      if (i + 2 >= in.size() || hexVal(in[i + 1]) < 0 || hexVal(in[i + 2]) < 0) return false;
      run += (char)(hexVal(in[i + 1]) * 16 + hexVal(in[i + 2]));
      i += 3;
    }
    if (!validUtf8(run)) return false;
    out += run;
  }
  return true;
}
static std::string lenientDecode(const std::string& in) {      // URLSearchParams: '+' is a space, a bad escape stays as it is
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '+') out += ' ';
    else if (in[i] == '%' && i + 2 < in.size() && hexVal(in[i + 1]) >= 0 && hexVal(in[i + 2]) >= 0) { out += (char)(hexVal(in[i + 1]) * 16 + hexVal(in[i + 2])); i += 2; }
    else out += in[i];
  }
  return out;
}
static bool isDot(const std::string& seg) { const std::string s = lowerAscii(seg); return s == "." || s == "%2e"; }
static bool isDotDot(const std::string& seg) { const std::string s = lowerAscii(seg); return s == ".." || s == ".%2e" || s == "%2e." || s == "%2e%2e"; }

Url parseUrl(const std::string& targetIn) {
  Url u;
  std::string target = targetIn;
  // absolute-form ("http://host/path"): the path starts at the first '/' after the authority
  const size_t scheme = target.find("://");
  if (scheme != npos && target.find('/') > scheme) { const size_t p = target.find_first_of("/?#", scheme + 3); target = p == npos ? "/" : target.substr(p); if (target.empty() || target[0] != '/') target = "/" + target; }
  // "//host/path": a scheme-relative reference — what follows the two slashes is an authority, not a path
  // (`new URL('//app.js', 'http://x').pathname` is "/"; a backslash counts as a slash here)
  if (target.size() >= 2 && (target[0] == '/' || target[0] == '\\') && (target[1] == '/' || target[1] == '\\')) {
    const size_t p = target.find_first_of("/\\?#", 2);
    target = p == npos ? std::string("/") : target.substr(p);
    if (target[0] != '/' && target[0] != '\\') target = "/" + target;
  }
  const size_t hash = target.find('#'); if (hash != npos) target.resize(hash);
  const size_t q = target.find('?');
  std::string path = q == npos ? target : target.substr(0, q);
  const std::string query = q == npos ? std::string() : target.substr(q + 1);
  for (char& c : path) if (c == '\\') c = '/';                // a special scheme reads a backslash as a slash
  std::vector<std::string> segs; bool endsDir = false;
  size_t a = 0;
  if (!path.empty() && path[0] == '/') a = 1;
  for (;;) {
    size_t b = path.find('/', a);
    const bool last = b == npos;
    const std::string seg = path.substr(a, last ? npos : b - a);
    if (isDotDot(seg)) { if (!segs.empty()) segs.pop_back(); endsDir = last; }
    else if (isDot(seg)) endsDir = last;
    else { segs.push_back(seg); endsDir = false; }
    if (last) break;
    a = b + 1;
  }
  u.pathname = "/";
  for (size_t i = 0; i < segs.size(); i++) { if (i) u.pathname += "/"; u.pathname += segs[i]; }
  if (endsDir && !segs.empty()) u.pathname += "/";
  for (size_t s = 0; s <= query.size();) {
    size_t e = query.find('&', s); if (e == npos) e = query.size();
    const std::string pair = query.substr(s, e - s);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      u.query.emplace_back(lenientDecode(eq == npos ? pair : pair.substr(0, eq)), eq == npos ? std::string() : lenientDecode(pair.substr(eq + 1)));
    }
    s = e + 1;
  }
  return u;
}
bool Url::has(const std::string& key) const { for (const auto& kv : query) if (kv.first == key) return true; return false; }
std::string Url::get(const std::string& key) const { for (const auto& kv : query) if (kv.first == key) return kv.second; return std::string(); }

bool safeRelative(const std::string& p, std::vector<std::string>& parts) {
  parts.clear();
  size_t a = 0;
  for (;;) {
    const size_t b = p.find_first_of("/\\", a);
    const std::string seg = p.substr(a, b == npos ? npos : b - a);
    // the path is absolute ("/…"), and path.normalize stops a ".." at the root: it can never climb out
    if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
    else if (!seg.empty() && seg != ".") {
      for (char c : seg) if (c == ':' || c == 0) return false;   // a drive, a stream, a cut-off name: never below the root
      parts.push_back(seg);
    }
    if (b == npos) break;
    a = b + 1;
  }
  return true;
}
std::string posixNormalize(const std::string& rel) {
  std::vector<std::string> out; int up = 0;
  size_t a = 0;
  for (;;) {
    const size_t b = rel.find('/', a);
    const std::string seg = rel.substr(a, b == npos ? npos : b - a);
    if (seg == "..") { if (!out.empty()) out.pop_back(); else up++; }
    else if (!seg.empty() && seg != ".") out.push_back(seg);
    if (b == npos) break;
    a = b + 1;
  }
  std::string s;
  for (int i = 0; i < up; i++) s += s.empty() ? ".." : "/..";
  for (const std::string& seg : out) { if (!s.empty()) s += "/"; s += seg; }
  if (!s.empty() && !rel.empty() && rel.back() == '/') s += "/";
  return s.empty() ? std::string(".") : s;
}
std::vector<std::string> cookieValues(const std::string& header, const std::string& name) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a <= header.size()) {
    size_t e = header.find(';', a); if (e == npos) e = header.size();
    std::string part = header.substr(a, e - a);
    // String.prototype.trim: ASCII white space is all a cookie header carries
    size_t x = 0, y = part.size();
    while (x < y && (unsigned char)part[x] <= ' ') x++;
    while (y > x && (unsigned char)part[y - 1] <= ' ') y--;
    part = part.substr(x, y - x);
    const size_t eq = part.find('=');
    if ((eq == npos ? part : part.substr(0, eq)) == name) { std::string v; if (percentDecode(eq == npos ? std::string() : part.substr(eq + 1), v)) out.push_back(v); }
    a = e + 1;
  }
  return out;
}
std::string encodeURIComponent(const std::string& s) {
  static const char d[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') out += (char)c;
    else { out += '%'; out += d[c >> 4]; out += d[c & 15]; }
  }
  return out;
}

const char* statusText(int code) {
  switch (code) {
    case 101: return "Switching Protocols"; case 200: return "OK"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 413: return "Payload Too Large";
    case 426: return "Upgrade Required"; case 431: return "Request Header Fields Too Large"; case 500: return "Internal Server Error";
    case 501: return "Not Implemented"; default: return "Unknown";
  }
}
std::string writeResponse(int status, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, bool headOnly, bool close) {
  std::string out = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n";
  for (const auto& kv : headers) out += kv.first + ": " + kv.second + "\r\n";
  out += "content-length: " + std::to_string(body.size()) + "\r\n";
  out += close ? "connection: close\r\n" : "connection: keep-alive\r\n";
  out += "\r\n";
  if (!headOnly) out += body;
  return out;
}

}}  // namespace deck::core
