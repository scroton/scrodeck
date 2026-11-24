// core/deck/http.hpp — just enough HTTP/1.1 for the deck's server, pure and byte-in / byte-out: an incremental
// request reader (head, Content-Length and chunked bodies, keep-alive, the bytes after an Upgrade), URL and
// cookie reading as Node's `new URL(req.url)` / lib/phone.js read them, and a response writer.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace deck { namespace core {

struct HttpRequest {
  std::string method, target, version;                     // "GET", "/desk/?t=…", "HTTP/1.1"
  std::vector<std::pair<std::string, std::string>> headers;   // names lower-cased, in order
  std::string body;
  const std::string& header(const std::string& lowerName) const;
  bool hasToken(const std::string& lowerName, const std::string& lowerToken) const;   // "connection: keep-alive, Upgrade" has `upgrade`
  bool wantsUpgrade() const;                               // Connection: upgrade + an Upgrade header
  bool keepAlive() const;
};

constexpr size_t kMaxHead = 16 * 1024;                      // Node's default header limit (431 over it)
constexpr uint64_t kMaxBody = 200ull * 1024 * 1024;         // lib/phone.js: an upload over 200 MB is cut off

class RequestReader {
 public:
  enum Result { More, Ready, Bad, TooLarge };
  // Append bytes and look for a whole request. Ready: request() is it; call next() before reading another.
  Result feed(const char* bytes, size_t n);
  Result poll();                                            // the same look, with nothing new (after next())
  const HttpRequest& request() const { return req_; }
  void next();                                              // done with request(): what followed it stays buffered
  std::string takeRest();                                   // everything buffered after request() (an upgrade: the first frames)
 private:
  enum State { Head, Body, Chunks, Done };
  State state_ = Head;
  std::string buf_;
  HttpRequest req_;
  uint64_t need_ = 0;                                       // body bytes still to come
  bool sizeLine_ = true, trailer_ = false;                  // chunked: expecting a size line / the trailer
};

// ---- the URL of a request, as `new URL(req.url, 'http://x')` gives it
struct Url {
  std::string pathname;                                     // dot segments removed, still percent-encoded
  std::vector<std::pair<std::string, std::string>> query;   // decoded (searchParams)
  bool has(const std::string& key) const;
  std::string get(const std::string& key) const;            // searchParams.get: the first, or ""
};
Url parseUrl(const std::string& target);
// decodeURIComponent; false when it would throw (a bad escape, or bytes that are not UTF-8)
bool percentDecode(const std::string& in, std::string& out);
// The path below a root that a request names, as path.join(root, path.normalize(pathname).replace(/^[\\/]+/, ''))
// resolves it: its segments. The pathname is absolute, and path.normalize stops ".." at the root, so it cannot
// climb out; false only for a segment that could name something else (a drive or stream colon, a NUL).
bool safeRelative(const std::string& decodedPath, std::vector<std::string>& parts);
// path.posix.normalize for a relative path: "a/./b/../c" -> "a/c" (a leading ".." survives)
std::string posixNormalize(const std::string& rel);
// every value a Cookie header gives one name (lib/phone.js authed), decoded; an undecodable one is left out
std::vector<std::string> cookieValues(const std::string& header, const std::string& name);
std::string encodeURIComponent(const std::string& s);

struct HttpResponse_ {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};
const char* statusText(int code);
// The bytes on the wire. `headOnly`: a HEAD request (Content-Length still says the body's length).
std::string writeResponse(int status, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, bool headOnly, bool close);

}}  // namespace deck::core
