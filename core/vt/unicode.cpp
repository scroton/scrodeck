// core/vt/unicode.cpp — character widths (xterm.js UnicodeV6, its DEFAULT provider) and the UTF-8 decoder.
//
// xterm.js 6 ships Unicode *6* width tables unless the unicode11 / graphemes addon is loaded, and the deck loads
// neither. So, as in the Electron deck: East Asian wide = 2 cells, combining marks / ZWJ / variation selectors
// = 0 (they join the previous cell), and emoji outside the BMP (U+1F300...) = ONE cell.
#include "vt_internal.hpp"

#include <cstring>

namespace deck { namespace vt { namespace detail {

#include "unicode_v6.inc"

namespace {
struct WidthTable {
  uint8_t t[65536];
  static void fill(uint8_t* t, int v, uint32_t from, uint32_t to) { memset(t + from, v, to - from); }
  WidthTable() {
    memset(t, 1, sizeof t);
    t[0] = 0;
    fill(t, 0, 1, 32);
    fill(t, 0, 0x7f, 0xa0);
    fill(t, 2, 0x1100, 0x1160);
    t[0x2329] = 2;
    t[0x232a] = 2;
    fill(t, 2, 0x2e80, 0xa4d0);
    t[0x303f] = 1;
    fill(t, 2, 0xac00, 0xd7a4);
    fill(t, 2, 0xf900, 0xfb00);
    fill(t, 2, 0xfe10, 0xfe1a);
    fill(t, 2, 0xfe30, 0xfe70);
    fill(t, 2, 0xff00, 0xff61);
    fill(t, 2, 0xffe0, 0xffe7);
    for (const auto& r : BMP_COMBINING) fill(t, 0, r[0], r[1] + 1);
  }
};
const WidthTable& widthTable() { static const WidthTable w; return w; }
const uint8_t* const g_width = widthTable().t;

bool inHighCombining(uint32_t ucs) {
  int min = 0, max = (int)(sizeof HIGH_COMBINING / sizeof HIGH_COMBINING[0]) - 1;
  if (ucs < HIGH_COMBINING[0][0] || ucs > HIGH_COMBINING[max][1]) return false;
  while (max >= min) {
    int mid = (min + max) >> 1;
    if (ucs > HIGH_COMBINING[mid][1]) min = mid + 1;
    else if (ucs < HIGH_COMBINING[mid][0]) max = mid - 1;
    else return true;
  }
  return false;
}
}  // namespace

int wcwidthV6(uint32_t num) {
  if (num < 32) return 0;
  if (num < 127) return 1;
  if (num < 65536) return g_width[num];
  if (inHighCombining(num)) return 0;
  if ((num >= 0x20000 && num <= 0x2fffd) || (num >= 0x30000 && num <= 0x3fffd)) return 2;
  return 1;
}

size_t Utf8Decoder::decode(const unsigned char* p, size_t n, uint32_t* out) {
  size_t o = 0;
  size_t i = 0;
  while (i < n) {
    unsigned char b = p[i];
    if (needed == 0) {
      if (b < 0x80) {
        // ASCII run
        do { out[o++] = b; ++i; } while (i < n && (b = p[i]) < 0x80);
        continue;
      }
      ++i;
      if (b >= 0xC2 && b <= 0xDF) { needed = 1; cp = b & 0x1F; }
      else if (b >= 0xE0 && b <= 0xEF) {
        if (b == 0xE0) lower = 0xA0;
        if (b == 0xED) upper = 0x9F;
        needed = 2; cp = b & 0x0F;
      } else if (b >= 0xF0 && b <= 0xF4) {
        if (b == 0xF0) lower = 0x90;
        if (b == 0xF4) upper = 0x8F;
        needed = 3; cp = b & 0x07;
      } else {
        out[o++] = 0xFFFD;
      }
      continue;
    }
    if (b < lower || b > upper) {
      // ill-formed: one U+FFFD for what was collected, then look at this byte again as a lead byte
      cp = 0; needed = 0; seen = 0; lower = 0x80; upper = 0xBF;
      out[o++] = 0xFFFD;
      continue;
    }
    ++i;
    lower = 0x80; upper = 0xBF;
    cp = (cp << 6) | (b & 0x3F);
    if (++seen == needed) {
      if (cp != 0xFEFF) out[o++] = cp;
      cp = 0; needed = 0; seen = 0;
    }
  }
  return o;
}

}}}  // namespace deck::vt::detail
