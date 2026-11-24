// mac/Sources/DeckMacSeams/KeychainNames.hpp — the PURE half of the Mac credentials read: which Keychain items
// hold one config dir's sign-in, exactly as lib/platform.js keychainServiceNames / keychainPayload decide it
// (the scheme was read out of Claude Code 2.1.259, lib/platform.js says where). No OS call: the Keychain read
// itself is MacHttpKeychain.mm.
// THIS FILE IS BUILT AND TESTED ON WINDOWS (mac/Tests/bridge_test.cpp checks it against vectors written by the
// real lib/platform.js: mac/Tests/gen-keychain-names.js). Windows counterpart: none (Windows reads a file).
#pragma once
#include "core/deck/hash.hpp"

#include <string>
#include <vector>

namespace deck { namespace mac {

constexpr const char* kKeychainAccount = "claude-code-user";

// path.resolve(p) on POSIX: absolute and normalised, against `cwd` when relative; "" resolves to cwd
inline std::string posixResolve(const std::string& p, const std::string& cwd) {
  const std::string full = !p.empty() && p[0] == '/' ? p : cwd + "/" + p;
  std::vector<std::string> parts;
  for (size_t i = 0; i <= full.size();) {
    size_t q = full.find('/', i); if (q == std::string::npos) q = full.size();
    const std::string seg = full.substr(i, q - i);
    if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
    else if (!seg.empty() && seg != ".") parts.push_back(seg);
    i = q + 1;
  }
  std::string out;
  for (const std::string& s : parts) out += "/" + s;
  return out.empty() ? std::string("/") : out;
}

// keychainServiceNames(dir): `dirNFC` is the config dir, ALREADY NFC-normalised by the caller (String.normalize
// is the one step this header cannot do). In order, without repeats; the unsuffixed name last.
inline std::vector<std::string> keychainServiceNames(const std::string& dirNFC, const std::string& home, const std::string& cwd) {
  const std::string& raw = dirNFC;
  std::string stripped = raw; while (!stripped.empty() && stripped.back() == '/') stripped.pop_back();
  std::string tilde = raw; if (!tilde.empty() && tilde[0] == '~') tilde = home + tilde.substr(1);
  std::vector<std::string> variants;
  auto add = [](std::vector<std::string>& v, const std::string& s) { if (s.empty()) return; for (const std::string& x : v) if (x == s) return; v.push_back(s); };
  add(variants, raw); add(variants, stripped); add(variants, posixResolve(raw, cwd)); add(variants, tilde);
  std::vector<std::string> names;
  for (const std::string& v : variants) add(names, "Claude Code-credentials-" + core::hexOf(core::sha256(v)).substr(0, 8));
  add(names, "Claude Code-credentials");            // CLAUDE_CONFIG_DIR unset
  return names;
}

// The chunked form's meta item ("<account>#m" holds {"n": chunks, "l": total length}): the piece account names,
// or empty when n is not an integer in 1..256 (keychainPayload refuses those).
inline std::vector<std::string> keychainChunkAccounts(long long n) {
  std::vector<std::string> out;
  if (n < 1 || n > 256) return out;
  for (long long i = 0; i < n; i++) out.push_back(std::string(kKeychainAccount) + "#" + std::to_string(i));
  return out;
}

}}  // namespace deck::mac
