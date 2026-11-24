// core/deck/hash.hpp — the three digests the server needs, pure: SHA-1 (the websocket accept key), SHA-256 and
// HMAC-SHA-256 (the page viewer's key), base64 and hex. Checked against Node's crypto (core/test/fixtures/deck/wire.json).
#pragma once
#include <string>

namespace deck { namespace core {

std::string sha1(const std::string& bytes);                          // 20 raw bytes
std::string sha256(const std::string& bytes);                        // 32 raw bytes
std::string hmacSha256(const std::string& key, const std::string& bytes);
std::string base64(const std::string& bytes);
// Node's Buffer.from(s, 'base64'): standard and URL-safe letters, padding optional, anything else skipped
std::string unbase64(const std::string& text);
std::string hexOf(const std::string& bytes);
// crypto.timingSafeEqual over two strings of equal length; false at once when the lengths differ (lib/phone.js safeEqual)
bool safeEqual(const std::string& a, const std::string& b);

}}  // namespace deck::core
