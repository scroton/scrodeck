// mac/Sources/DeckMacSeams/MacCredentials.cpp — core::Credentials on macOS: an account's OAuth token for the usage
// and cloud polls, READ ONLY. Nothing here writes, refreshes, copies or logs a token.
// UNBUILT, UNTESTED: written on Windows, never compiled (the service names it asks for ARE tested: KeychainNames.hpp).
// Windows counterpart: WinCredentials in windows/host/winhttp.{hpp,cpp} (<configdir>\.credentials.json + its mtime).
// JS mirrored: lib/platform.js readOAuth / credentialsStamp / keychainText / keychainPayload / keychainRead.
//
// HOW the Keychain is read, and why not SecItemCopyMatching: lib/platform.js runs
//     /usr/bin/security find-generic-password -a <account> -s <service> -w
// and this does exactly the same. Claude Code creates the item with `security`, so `security` is on the item's
// access list and reads it silently; a different program asking the Security framework directly is a NEW requester
// and macOS would raise "Claude Deck wants to use your confidential information" per item. Same command, same
// 4 s limit, same one-read-per-account-per-minute cache (a miss costs up to 4 s on the deck's thread, as in Electron).
// tools/mac-keychain-probe.sh lists what the Keychain actually holds when a Mac shows no bars.
//
// NOT WIRED BY DEFAULT: dms_host_create makes this only when DMSOptions.http_and_credentials is set, and the core
// uses it only when DBOptions.usage_polling / cloud_polling say so (the owner's word).
#include "MacSeams.hpp"
#include "KeychainNames.hpp"
#include "core/deck/hash.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <climits>
#include <map>
#include <unistd.h>

namespace deck { namespace mac {

namespace {
// String.prototype.normalize('NFC')
std::string nfc(const std::string& utf8) {
  CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)utf8.data(), (CFIndex)utf8.size(), kCFStringEncodingUTF8, false);
  if (!s) return utf8;
  CFMutableStringRef m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s);
  CFRelease(s);
  if (!m) return utf8;
  CFStringNormalize(m, kCFStringNormalizationFormC);
  const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(m), kCFStringEncodingUTF8) + 1;
  std::string out((size_t)max, '\0');
  const bool ok = CFStringGetCString(m, &out[0], max, kCFStringEncodingUTF8);
  CFRelease(m);
  if (!ok) return utf8;
  out.resize(strlen(out.c_str()));
  return out;
}
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0; while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}
// Buffer.from(text, 'base64'): ignores what is not in the alphabet, stops at padding
std::string unbase64(const std::string& in) {
  std::string out; unsigned acc = 0; int bits = 0;
  for (unsigned char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26; else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62; else if (c == '/' || c == '_') v = 63; else if (c == '=') break; else continue;
    acc = (acc << 6) | (unsigned)v; bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back((char)((acc >> bits) & 0xFF)); }
  }
  return out;
}

class KeychainCredentials : public core::Credentials {
 public:
  explicit KeychainCredentials(core::Clock& clock) : clock_(clock) {}
  core::json readOAuth(const std::string& dir) override {
    const std::string file = dir + "/.credentials.json";
    if (files_.isFile(file)) { std::string t; if (files_.read(file, t)) { core::json j = core::json::parse(t, nullptr, false); if (!j.is_discarded()) return j; } }
    std::string text; if (!keychainText(dir, text)) return core::json();
    core::json j = core::json::parse(text, nullptr, false);
    return j.is_discarded() ? core::json() : j;
  }
  std::string stamp(const std::string& dir) override {
    const std::string file = dir + "/.credentials.json";
    if (files_.isFile(file)) return std::to_string(files_.stat(file).mtimeMs);
    std::string text; if (!keychainText(dir, text)) return std::string();
    return core::hexOf(core::sha1(text));
  }

 private:
  // keychainRead: the item's secret as text, false on a miss (a non-zero exit) or an empty answer
  bool keychainRead(const std::string& service, const std::string& account, std::string& out) {
    int code = -1;
    const std::string got = trim(runCapture("/usr/bin/security", { "find-generic-password", "-a", account, "-s", service, "-w" }, 4000, nullptr, &code));
    if (code != 0 || got.empty()) return false;
    out = got; return true;
  }
  // keychainPayload: one item, or the chunked form ("#m" names the pieces, "#0".."#n-1" hold base64)
  bool keychainPayload(const std::string& service, std::string& out) {
    if (keychainRead(service, kKeychainAccount, out)) return true;
    std::string meta; if (!keychainRead(service, std::string(kKeychainAccount) + "#m", meta)) return false;
    const core::json m = core::json::parse(meta, nullptr, false);
    if (!m.is_object() || !m.contains("n") || !m["n"].is_number_integer()) return false;
    const std::vector<std::string> accounts = keychainChunkAccounts(m["n"].get<long long>());
    if (accounts.empty()) return false;
    std::string joined;
    for (const std::string& a : accounts) { std::string part; if (!keychainRead(service, a, part)) return false; joined += part; }
    if (m.contains("l") && m["l"].is_number_integer() && (long long)joined.size() != m["l"].get<long long>()) return false;
    out = unbase64(joined);
    return !out.empty();
  }
  // keychainText: one read per account per minute, remembering the service name that answered
  bool keychainText(const std::string& dir, std::string& out) {
    const double now = clock_.nowMs();
    auto hit = cache_.find(dir);
    if (hit != cache_.end() && now - hit->second.at < 60000) { out = hit->second.text; return hit->second.has; }
    char cwd[PATH_MAX]; const std::string here = getcwd(cwd, sizeof cwd) ? cwd : "/";
    std::vector<std::string> names = keychainServiceNames(nfc(dir), homeDir(), here);
    if (hit != cache_.end() && !hit->second.service.empty()) {
      std::vector<std::string> order{ hit->second.service };
      for (const std::string& s : names) if (s != hit->second.service) order.push_back(s);
      names.swap(order);
    }
    Entry e; e.at = now;
    for (const std::string& s : names) { if (keychainPayload(s, e.text)) { e.has = true; e.service = s; break; } }
    cache_[dir] = e;
    out = e.text; return e.has;
  }
  struct Entry { double at = 0; bool has = false; std::string text, service; };
  core::Clock& clock_;
  PosixFiles files_;
  std::map<std::string, Entry> cache_;
};
}  // namespace

std::unique_ptr<core::Credentials> makeKeychainCredentials(core::Clock& clock) { return std::unique_ptr<core::Credentials>(new KeychainCredentials(clock)); }

}}  // namespace deck::mac
