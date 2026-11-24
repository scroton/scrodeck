// core/deck/hash.cpp — see hash.hpp. FIPS 180-4 and RFC 2104, written out.
#include "hash.hpp"
#include <cstdint>
#include <cstring>

namespace deck { namespace core {

static inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// message + 0x80 + zeros + the bit length (big-endian, 64 bits), in 64-byte blocks
static std::string padded(const std::string& m) {
  std::string p = m;
  p += (char)0x80;
  while (p.size() % 64 != 56) p += (char)0;
  const uint64_t bits = (uint64_t)m.size() * 8;
  for (int i = 7; i >= 0; i--) p += (char)((bits >> (i * 8)) & 0xFF);
  return p;
}
static inline uint32_t be32(const unsigned char* b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static void put32(std::string& out, uint32_t v) { for (int i = 3; i >= 0; i--) out += (char)((v >> (i * 8)) & 0xFF); }

std::string sha1(const std::string& msg) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  const std::string p = padded(msg);
  for (size_t off = 0; off < p.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32((const unsigned char*)p.data() + off + i * 4);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  std::string out; put32(out, h0); put32(out, h1); put32(out, h2); put32(out, h3); put32(out, h4);
  return out;
}

std::string sha256(const std::string& msg) {
  static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
  uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  const std::string p = padded(msg);
  for (size_t off = 0; off < p.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32((const unsigned char*)p.data() + off + i * 4);
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  std::string out; for (int i = 0; i < 8; i++) put32(out, h[i]);
  return out;
}

std::string hmacSha256(const std::string& keyIn, const std::string& msg) {
  std::string key = keyIn.size() > 64 ? sha256(keyIn) : keyIn;
  key.resize(64, (char)0);
  std::string ipad(64, (char)0), opad(64, (char)0);
  for (int i = 0; i < 64; i++) { ipad[i] = (char)(key[i] ^ 0x36); opad[i] = (char)(key[i] ^ 0x5c); }
  return sha256(opad + sha256(ipad + msg));
}

std::string base64(const std::string& in) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
    out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
  }
  if (i + 1 == in.size()) { const uint32_t v = (unsigned char)in[i] << 16; out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "=="; }
  else if (i + 2 == in.size()) { const uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8); out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '='; }
  return out;
}
std::string hexOf(const std::string& in) {
  static const char d[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : in) { out += d[c >> 4]; out += d[c & 15]; }
  return out;
}
bool safeEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

std::string unbase64(const std::string& text) {
  std::string out; out.reserve(text.size() * 3 / 4);
  unsigned acc = 0; int bits = 0;
  for (unsigned char ch : text) {
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A'; else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26; else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+' || ch == '-') v = 62; else if (ch == '/' || ch == '_') v = 63; else if (ch == '=') break; else continue;
    acc = (acc << 6) | (unsigned)v; bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back((char)((acc >> bits) & 0xFF)); }
  }
  return out;
}

}}  // namespace deck::core
