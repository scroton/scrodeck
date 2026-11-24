// core/vt/vt_internal.hpp — shared between the core/vt translation units. Not a public header.
// Each piece names the xterm.js 6.0.0 source it ports (paths under @xterm/xterm/src/common).
#pragma once
#include "terminal.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace deck { namespace vt { namespace detail {

// ---------------------------------------------------------------- attributes (buffer/Constants.ts, AttributeData.ts)
// The pen is kept in xterm's own bit layout so SGR / save / restore / erase are literal ports; it is converted
// to the public Cell form once per pen change, not per character.
enum : uint32_t {
  CM_MASK = 0x3000000, CM_P16 = 0x1000000, CM_P256 = 0x2000000, CM_RGB = 0x3000000,
  RGB_MASK = 0xFFFFFF, PCOLOR_MASK = 0xFF,
  FG_INVERSE = 0x4000000, FG_BOLD = 0x8000000, FG_UNDERLINE = 0x10000000, FG_BLINK = 0x20000000,
  FG_INVISIBLE = 0x40000000, FG_STRIKE = 0x80000000u,
  BG_ITALIC = 0x4000000, BG_DIM = 0x8000000, BG_HAS_EXT = 0x10000000, BG_PROTECTED = 0x20000000, BG_OVERLINE = 0x40000000,
};

struct Attr {
  uint32_t fg = 0, bg = 0;
  uint8_t ul = 0;        // ExtendedAttrs underline style as stored (0-5)
  bool url = false;      // ExtendedAttrs.urlId != 0 (an OSC 8 link is open)
  int ulEffective() const { return url ? 5 : ul; }               // ExtendedAttrs.underlineStyle getter
  void updateExtended() { if (ulEffective() == 0 && !url) bg &= ~BG_HAS_EXT; else bg |= BG_HAS_EXT; }
};
Cell toCell(const Attr& a);          // an empty width-1 cell carrying the attribute

// ---------------------------------------------------------------- unicode (input/UnicodeV6.ts, services/UnicodeService.ts)
int wcwidthV6(uint32_t cp);
// charProperties(): bit 0 = shouldJoin, bits 1-2 = width
inline uint32_t charProperties(uint32_t cp, uint32_t preceding) {
  int width = wcwidthV6(cp);
  bool join = width == 0 && preceding != 0;
  if (join) {
    int oldWidth = (preceding >> 1) & 3;
    if (oldWidth == 0) join = false;
    else if (oldWidth > width) width = oldWidth;
  }
  return (uint32_t)((width & 3) << 1) | (join ? 1u : 0u);
}

// Streaming UTF-8 -> UTF-32, WHATWG/Node semantics: every maximal ill-formed subsequence becomes one U+FFFD.
// Restartable at any byte. U+FEFF is dropped (xterm's StringToUtf32 skips it).
struct Utf8Decoder {
  uint32_t cp = 0;
  int needed = 0, seen = 0;
  uint8_t lower = 0x80, upper = 0xBF;
  void reset() { cp = 0; needed = 0; seen = 0; lower = 0x80; upper = 0xBF; }
  // Decodes [p, p+n) into out (capacity must be >= n + 1). Returns the number of code points written.
  size_t decode(const unsigned char* p, size_t n, uint32_t* out);
};

// ---------------------------------------------------------------- parser (parser/EscapeSequenceParser.ts, Params.ts)
enum ParserState : uint8_t {
  GROUND = 0, ESCAPE, ESCAPE_INTERMEDIATE, CSI_ENTRY, CSI_PARAM, CSI_INTERMEDIATE, CSI_IGNORE, SOS_PM_APC_STRING,
  OSC_STRING, DCS_ENTRY, DCS_PARAM, DCS_IGNORE, DCS_INTERMEDIATE, DCS_PASSTHROUGH
};
enum ParserAction : uint8_t {
  A_IGNORE = 0, A_ERROR, A_PRINT, A_EXECUTE, A_OSC_START, A_OSC_PUT, A_OSC_END, A_CSI_DISPATCH, A_PARAM, A_COLLECT,
  A_ESC_DISPATCH, A_CLEAR, A_DCS_HOOK, A_DCS_PUT, A_DCS_UNHOOK
};
// table[state << 8 | code] = action << 4 | nextState, code < 0xA0 (everything above uses the 0xA0 entry)
const uint8_t* transitionTable();

struct Params {
  enum { MAX = 32, MAX_SUB = 32 };
  int32_t params[MAX];
  int length = 0;
  int32_t sub[MAX_SUB];
  int subLength = 0;
  uint16_t subIdx[MAX];
  bool rejectDigits = false, rejectSubDigits = false, digitIsSub = false;

  void reset() { length = 0; subLength = 0; rejectDigits = rejectSubDigits = digitIsSub = false; }
  void addParam(int32_t v) {
    digitIsSub = false;
    if (length >= MAX) { rejectDigits = true; return; }
    subIdx[length] = (uint16_t)(subLength << 8 | subLength);
    params[length++] = v;
  }
  void addSubParam(int32_t v) {
    digitIsSub = true;
    if (!length) return;
    if (rejectDigits || subLength >= MAX_SUB) { rejectSubDigits = true; return; }
    sub[subLength++] = v;
    subIdx[length - 1]++;
  }
  void addDigit(int v) {
    int len;
    if (rejectDigits || !(len = digitIsSub ? subLength : length) || (digitIsSub && rejectSubDigits)) return;
    int32_t* store = digitIsSub ? sub : params;
    int32_t cur = store[len - 1];
    if (cur == -1) store[len - 1] = v;
    else {
      int64_t nv = (int64_t)cur * 10 + v;
      store[len - 1] = nv > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)nv;
    }
  }
  bool hasSubParams(int i) const { return (subIdx[i] & 0xFF) - (subIdx[i] >> 8) > 0; }
  int subStart(int i) const { return subIdx[i] >> 8; }
  int subEnd(int i) const { return subIdx[i] & 0xFF; }
};

// ---------------------------------------------------------------- lines (buffer/BufferLine.ts)
// JS typed arrays read 0 out of range and ignore writes out of range; these helpers keep that.
inline int lineLen(const Line& l) { return (int)l.cells.size(); }
inline int getWidth(const Line& l, int i) { return (i >= 0 && i < lineLen(l)) ? l.cells[i].width : 0; }
inline bool hasContent(const Line& l, int i) { return i >= 0 && i < lineLen(l) && (l.cells[i].ch != 0 || l.cells[i].extra != 0); }
inline bool isProtected(const Line& l, int i) { return i >= 0 && i < lineLen(l) && (l.cells[i].flags & Protected); }
inline void setCell(Line& l, int i, const Cell& c) { if (i >= 0 && i < lineLen(l)) l.cells[i] = c; }
inline void setCellCp(Line& l, int i, char32_t cp, int width, const Cell& attrs) {
  if (i < 0 || i >= lineLen(l)) return;
  Cell& c = l.cells[i];
  c = attrs; c.ch = cp; c.width = (uint8_t)width; c.extra = 0;
}
void insertCells(Line& l, int pos, int n, const Cell& fill);
void deleteCells(Line& l, int pos, int n, const Cell& fill);
void replaceCells(Line& l, int start, int end, const Cell& fill, bool respectProtect = false);
void fillLine(Line& l, const Cell& fill, bool respectProtect = false);
void resizeLine(Line& l, int cols, const Cell& fill);
int trimmedLength(const Line& l);
void copyCellsFrom(Line& dst, const Line& src, int srcCol, int destCol, int length, bool reverse);
void makeBlank(Line& l, int cols, const Cell& fill, bool wrapped);

// ---------------------------------------------------------------- buffer (buffer/Buffer.ts, BufferReflow.ts, CircularList.ts)
struct Buffer {
  std::deque<Line> lines;       // CircularList: index 0 = oldest. lines[ybase + y] is viewport row y.
  size_t maxLength = 0;         // rows + scrollback (normal) or rows (alt)
  size_t startIndex = 0;        // CircularList._startIndex — kept only because one reflow quirk depends on it
  void setMaxLength(size_t n) { if (n != maxLength) { maxLength = n; startIndex = 0; } }
  bool hasScrollback = false;
  int scrollback = 0;
  int cols = 0, rows = 0;       // the size this buffer was last laid out for
  int ybase = 0, y = 0, x = 0;
  int scrollTop = 0, scrollBottom = 0;
  std::vector<uint8_t> tabs;    // tab stop per column; never shrinks (xterm keeps stops past the right edge)
  int savedX = 0, savedY = 0;
  uint32_t savedFg = 0, savedBg = 0;
  int savedCharset = 0;

  void init(bool hasScrollback, int scrollback, int cols, int rows);
  size_t correctBufferLength(int r) const { return hasScrollback ? (size_t)r + (size_t)scrollback : (size_t)r; }
  void fillViewportRows(const Cell& fill);
  void clear();                                  // Buffer.clear(): drop all lines, home, reset margins + tabs
  void push(Line&& l);                           // CircularList.push: drops the oldest line when full
  void trimStart(size_t n);
  void resize(int newCols, int newRows, const Cell& nullCell, int bufferServiceCols);
  void setupTabStops(int from = -1);             // -1 = clear and lay out every 8
  int prevStop(int x) const;
  int nextStop(int x) const;
  bool tabAt(int i) const { return i >= 0 && i < (int)tabs.size() && tabs[i]; }
  void setTab(int i, bool v);
  Line& row(int vy) { return lines[(size_t)(ybase + vy)]; }
  bool hasRow(int vy) const { int i = ybase + vy; return i >= 0 && (size_t)i < lines.size(); }
};

}}}  // namespace deck::vt::detail
