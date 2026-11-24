// core/vt/terminal.cpp — deck::vt::Terminal: the escape sequence parser loop and every sequence handler.
// A port of @xterm/xterm 6.0.0 src/common/InputHandler.ts + parser/EscapeSequenceParser.ts (parse loop),
// parser/OscParser.ts, parser/DcsParser.ts, services/BufferService.ts (scroll), buffer/BufferSet.ts,
// services/CharsetService.ts and CoreService.ts. Method names follow InputHandler so the two can be read side
// by side. Known differences from xterm.js are listed at the bottom of this file.
#include "terminal.hpp"
#include "vt_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace deck { namespace vt {

using namespace detail;

namespace {
#include "charsets.inc"

const size_t PAYLOAD_LIMIT = 10000000;     // parser/Constants.ts: OSC / DCS payloads beyond this are dropped
const size_t MAX_COMBINING = 64;           // ours: code points kept per cell (xterm.js has no limit)
const size_t COMBINING_GC_AT = 1 << 16;    // ours: compact the sequence table when it grows past this

inline int clampInt(long long v, long long lo, long long hi) { return (int)(v < lo ? lo : v > hi ? hi : v); }

void appendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) s.push_back((char)cp);
  else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { s.push_back((char)(0xE0 | (cp >> 12))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
  else { s.push_back((char)(0xF0 | (cp >> 18))); s.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
}

enum OscState { OSC_START = 0, OSC_ID, OSC_PAYLOAD, OSC_ABORT };

// CSI / ESC identifiers the way the parser builds them: prefix and intermediates shifted in, final byte last
constexpr uint32_t id1(char f) { return (uint32_t)(unsigned char)f; }
constexpr uint32_t id2(char a, char f) { return (uint32_t)(unsigned char)a << 8 | (unsigned char)f; }
constexpr uint32_t id3(char a, char b, char f) { return (uint32_t)(unsigned char)a << 16 | (uint32_t)(unsigned char)b << 8 | (unsigned char)f; }
}  // namespace

struct Terminal::Impl {
  int cols, rows, scrollback;
  Buffer normal, alt;
  Buffer* buf;

  // ---- parser state (EscapeSequenceParser)
  uint8_t state = GROUND;
  Params params;
  uint32_t collect = 0;
  uint32_t precedingJoinState = 0;
  bool printRunOpen = false;      // the last parser action was a print (see print())
  Utf8Decoder utf8;
  std::vector<uint32_t> decoded;

  // ---- OSC (OscParser + OscHandler)
  int oscState = OSC_START;
  long long oscId = -1;
  bool oscHandled = false, oscHitLimit = false;
  std::string oscData;            // UTF-8
  size_t oscUnits = 0;            // payload length in UTF-16 units, what xterm's limit counts

  // ---- DCS (DcsParser + DcsHandler): only DECRQSS ($q) has a handler
  bool dcsActive = false, dcsHitLimit = false;
  std::string dcsData;

  // ---- pen
  Attr cur;
  Cell curCell, eraseCell, defCell;

  // ---- modes (CoreService, CoreMouseService, options the stream can change)
  bool insertMode = false, convertEol = false;
  bool appCursor = false, appKeypad = false, bracketedPaste = false, origin = false, reverseWraparound = false;
  bool sendFocus = false, syncOutput = false, wraparound = true;
  int cursorShape = 0, cursorBlinkMode = -1;
  bool cursorHidden = false;
  bool optCursorBlink = true;
  int mouseProtocol = 0, mouseEncoding = 0;

  // ---- charsets (CharsetService): ids into CHARSET_MAP, 0 = US ASCII / undefined
  int charset = 0, glevel = 0, gsets[4] = {0, 0, 0, 0};

  std::string title, iconName, replies;
  bool bell = false;

  // ---- combining sequences: Cell::extra indexes this (0 = none)
  std::vector<std::u32string> comb;
  std::unordered_map<std::u32string, uint32_t> combIndex;

  // ---- damage
  int dirtyLo = 0, dirtyHi = -1;

  // ---- host colours for OSC reports: 0-255 palette, 256 fg, 257 bg, 258 cursor; -1 = unknown
  std::vector<int64_t> theme;

  Impl(int c, int r, int sb) : cols(std::max(c, 2)), rows(std::max(r, 1)), scrollback(std::min(std::max(sb, 0), 100000000)), theme(259, -1) {
    defCell = toCell(Attr());
    comb.emplace_back();
    penChanged();
    params.addParam(0);
    resetBuffers();
    markAll();
  }

  // ------------------------------------------------------------------ small helpers
  void markDirty(int y) { if (y < dirtyLo) dirtyLo = y; if (y > dirtyHi) dirtyHi = y; }
  void markRange(int a, int b) { if (a > b) std::swap(a, b); markDirty(a); markDirty(b); }
  void markAll() { dirtyLo = 0; dirtyHi = rows - 1; }

  void penChanged() {
    curCell = toCell(cur);
    Attr e;
    e.bg = cur.bg & 0x03FFFFFF;          // _eraseAttrData(): the pen's background colour, none of its flags
    eraseCell = toCell(e);
  }

  void resetBuffers() {                  // BufferSet.reset()
    normal.init(true, scrollback, cols, rows);
    normal.fillViewportRows(defCell);
    alt.init(false, scrollback, cols, rows);
    buf = &normal;
  }

  Line blankLine(const Cell& fill, bool wrapped = false) { Line l; makeBlank(l, cols, fill, wrapped); return l; }
  Line* rowPtr(int vy) { return buf->hasRow(vy) ? &buf->row(vy) : nullptr; }
  // CircularList.get() with an index past the end: in a FULL list the index wraps around to the oldest
  // lines (xterm.js then really does write there); otherwise it reads nothing we can see.
  Line* cyclicLine(int abs) {
    Buffer& b = *buf;
    if (abs < 0 || b.lines.empty()) return nullptr;
    if ((size_t)abs < b.lines.size()) return &b.lines[(size_t)abs];
    if (b.lines.size() == b.maxLength) return &b.lines[(size_t)abs % b.maxLength];
    return nullptr;
  }

  void reply(const char* s) { replies += s; }
  void replyf(const char* fmt, long long a = 0, long long b = 0) {
    char tmp[96];
    snprintf(tmp, sizeof tmp, fmt, a, b);
    replies += tmp;
  }

  // ------------------------------------------------------------------ combining sequences
  uint32_t intern(std::u32string&& s) {
    auto it = combIndex.find(s);
    if (it != combIndex.end()) return it->second;
    uint32_t id = (uint32_t)comb.size();
    comb.push_back(s);
    combIndex.emplace(std::move(s), id);
    return id;
  }

  void addCodepointToCell(Line& l, int index, uint32_t cp, int width) {      // BufferLine.addCodepointToCell
    if (index < 0 || index >= lineLen(l)) return;
    Cell& c = l.cells[(size_t)index];
    if (c.extra) {
      const std::u32string& old = comb[c.extra];
      if (old.size() < MAX_COMBINING) { std::u32string s = old; s.push_back((char32_t)cp); c.extra = intern(std::move(s)); }
    } else if (c.ch) {
      std::u32string s; s.push_back(c.ch); s.push_back((char32_t)cp);
      c.extra = intern(std::move(s));
    } else {
      c.ch = (char32_t)cp;
      c.width = 1;
    }
    if (width) c.width = (uint8_t)width;
  }

  void compactCombining() {
    std::vector<std::u32string> fresh;
    fresh.emplace_back();
    std::unordered_map<uint32_t, uint32_t> remap;
    auto sweep = [&](Buffer& b) {
      for (auto& l : b.lines) for (auto& c : l.cells) if (c.extra) {
        auto it = remap.find(c.extra);
        if (it == remap.end()) { fresh.push_back(comb[c.extra]); it = remap.emplace(c.extra, (uint32_t)fresh.size() - 1).first; }
        c.extra = it->second;
      }
    };
    sweep(normal); sweep(alt);
    comb.swap(fresh);
    combIndex.clear();
    for (uint32_t i = 1; i < comb.size(); ++i) combIndex.emplace(comb[i], i);
  }

  // ------------------------------------------------------------------ BufferService.scroll
  void scroll(const Cell& fill, bool isWrapped = false) {
    Buffer& b = *buf;
    const int topRow = b.ybase + b.scrollTop;
    const int bottomRow = b.ybase + b.scrollBottom;
    if (b.scrollTop == 0) {
      // Determine whether the buffer is going to be trimmed after insertion.
      const bool willBufferBeTrimmed = b.lines.size() == b.maxLength;
      // Insert the line using the fastest method
      if (bottomRow == (int)b.lines.size() - 1) {
        if (willBufferBeTrimmed) {
          Line l = std::move(b.lines.front());       // recycle the oldest line's storage
          b.lines.pop_front();
          b.startIndex = (b.startIndex + 1) % b.maxLength;
          makeBlank(l, cols, fill, isWrapped);
          b.lines.push_back(std::move(l));
        } else {
          b.lines.push_back(blankLine(fill, isWrapped));
        }
      } else {
        b.lines.insert(b.lines.begin() + (bottomRow + 1), blankLine(fill, isWrapped));
        if (b.lines.size() > b.maxLength) { b.lines.pop_front(); b.startIndex++; }
      }
      // Only adjust ybase when the buffer is not trimmed
      if (!willBufferBeTrimmed) b.ybase++;
    } else {
      // scrollTop is non-zero which means no line will be going to the scrollback, instead we can just
      // shift them in-place.
      std::rotate(b.lines.begin() + topRow, b.lines.begin() + (topRow + 1), b.lines.begin() + (bottomRow + 1));
      makeBlank(b.lines[(size_t)bottomRow], cols, fill, isWrapped);
    }
    markAll();
  }

  // ------------------------------------------------------------------ print
  // `continues`: this run directly continues the previous print run — only a feed() boundary lies between
  // them. xterm.js repeats the "cursor sits on the right half of a wide char" fix-up at the start of every
  // print call, so its result depends on where a write was cut (visible with DECAWM off and wide chars at
  // the right edge). The engine must not depend on chunking, so a continued run skips that fix-up and
  // behaves as xterm does when the run arrives in one piece.
  void print(const uint32_t* data, int start, int end, bool continues = false) {
    Buffer& b = *buf;
    Line* bufferRow = rowPtr(b.y);
    if (!bufferRow) return;
    const char16_t* cs = charset ? CHARSET_MAP[charset] : nullptr;
    markDirty(b.y);

    // handle wide chars: reset start_cell-1 if we would overwrite the second cell of a wide char
    if (!continues && b.x && end - start > 0 && getWidth(*bufferRow, b.x - 1) == 2) setCellCp(*bufferRow, b.x - 1, 0, 1, curCell);

    uint32_t pjs = precedingJoinState;
    for (int pos = start; pos < end; ++pos) {
      uint32_t code = data[pos];
      // charsets are only defined for ASCII
      if (code < 127 && cs) {
        const char16_t ch = cs[code];
        if (ch) code = ch;
      }
      const uint32_t currentInfo = charProperties(code, pjs);
      int chWidth = (int)((currentInfo >> 1) & 3);
      const bool shouldJoin = (currentInfo & 1) != 0;
      const int oldWidth = shouldJoin ? (int)((pjs >> 1) & 3) : 0;
      pjs = currentInfo;

      // goto next line if ch would overflow
      if (b.x + chWidth - oldWidth > cols) {
        if (wraparound) {
          // autowrap - DECAWM: automatically wraps to the beginning of the next line
          int oldCol = b.x - oldWidth;
          bool sameRow = false;
          b.x = oldWidth;
          b.y++;
          if (b.y == b.scrollBottom + 1) {
            b.y--;
            scroll(eraseCell, true);
          } else {
            if (b.y >= rows) { b.y = rows - 1; sameRow = true; }
            // The line already exists (eg. the initial viewport), mark it as a wrapped line
            b.row(b.y).wrapped = true;
          }
          // row changed, get it again; the row we came from is now the one above (or, when the cursor sat
          // on the last row below the scroll region, the very same row)
          bufferRow = &b.row(b.y);
          const int oldAbs = b.ybase + b.y - 1;         // may be a scrollback line when the screen is 1 row high
          Line* oldRow = (sameRow || oldAbs < 0) ? bufferRow : &b.lines[(size_t)oldAbs];
          markDirty(b.y);
          if (oldWidth > 0) {
            // Combining character widens 1 column to 2. Move old character to next line.
            copyCellsFrom(*bufferRow, *oldRow, oldCol, 0, oldWidth, false);
          }
          // clear left over cells to the right
          while (oldCol < cols) setCellCp(*oldRow, oldCol++, 0, 1, curCell);
        } else {
          b.x = cols - 1;
          if (chWidth == 2) continue;      // a wide char that does not fit into the last cell is dropped
        }
      }

      // insert combining char at last cursor position
      if (shouldJoin && b.x) {
        const int offset = getWidth(*bufferRow, b.x - 1) ? 1 : 2;
        // if empty cell after fullwidth, need to go 2 cells back
        addCodepointToCell(*bufferRow, b.x - offset, code, chWidth);
        for (int delta = chWidth - oldWidth; --delta >= 0;) setCellCp(*bufferRow, b.x++, 0, 0, curCell);
        continue;
      }

      // insert mode: move characters to right
      if (insertMode) {
        insertCells(*bufferRow, b.x, chWidth - oldWidth, curCell);
        // a fullwidth char shifted into the last cell is lost
        if (getWidth(*bufferRow, cols - 1) == 2) setCellCp(*bufferRow, cols - 1, 0, 1, curCell);
      }

      // write current char to buffer and advance cursor
      setCellCp(*bufferRow, b.x++, (char32_t)code, chWidth, curCell);

      // fullwidth char - also set next cell to placeholder stub and advance cursor
      if (chWidth > 0) {
        while (--chWidth) setCellCp(*bufferRow, b.x++, 0, 0, curCell);
      }
    }
    precedingJoinState = pjs;

    // handle wide chars: reset cell to the right if it is second cell of a wide char
    if (b.x < cols && end - start > 0 && getWidth(*bufferRow, b.x) == 0 && !hasContent(*bufferRow, b.x)) {
      setCellCp(*bufferRow, b.x, 0, 1, curCell);
    }
    markDirty(b.y);
  }

  // ------------------------------------------------------------------ C0 / C1
  void lineFeed() {
    Buffer& b = *buf;
    markDirty(b.y);
    if (convertEol) b.x = 0;
    b.y++;
    if (b.y == b.scrollBottom + 1) {
      b.y--;
      scroll(eraseCell);
    } else if (b.y >= rows) {
      b.y = rows - 1;
    } else {
      // There was an explicit line feed (not just a carriage return), so clear the wrapped state of the
      // line. This is particularly important on conpty/Windows where revisiting lines to reprint is common.
      b.row(b.y).wrapped = false;
    }
    // If the end of the line is hit, prevent this action from wrapping around to the next line.
    if (b.x >= cols) b.x--;
    markDirty(b.y);
  }

  void backspace() {
    Buffer& b = *buf;
    if (!reverseWraparound) {
      restrictCursor();
      if (b.x > 0) b.x--;
      return;
    }
    // reverse wrap-around is enabled: the cursor may sit at x=cols to address the last cell of a row by BS
    restrictCursor(cols);
    if (b.x > 0) {
      b.x--;
    } else {
      if (b.x == 0 && b.y > b.scrollTop && b.y <= b.scrollBottom && b.hasRow(b.y) && b.row(b.y).wrapped) {
        b.row(b.y).wrapped = false;
        b.y--;
        b.x = cols - 1;
        // find last taken cell
        const Line& line = b.row(b.y);
        if (getWidth(line, b.x) && !hasContent(line, b.x)) b.x--;
      }
    }
    restrictCursor();
  }

  void tab() {
    Buffer& b = *buf;
    if (b.x >= cols) return;
    b.x = b.nextStop(b.x);
  }

  void setgLevel(int g) { glevel = g; charset = gsets[g]; }
  void setgCharset(int g, int id) { gsets[g] = id; if (glevel == g) charset = id; }

  void execute(uint32_t code) {
    switch (code) {
      case 0x07: bell = true; break;
      case 0x0a: case 0x0b: case 0x0c: lineFeed(); break;
      case 0x0d: buf->x = 0; break;
      case 0x08: backspace(); break;
      case 0x09: tab(); break;
      case 0x0e: setgLevel(1); break;
      case 0x0f: setgLevel(0); break;
      case 0x84: index(); break;
      case 0x85: nextLine(); break;
      case 0x88: tabSet(); break;
      default: break;
    }
  }

  // ------------------------------------------------------------------ cursor
  void restrictCursor() { restrictCursor(cols - 1); }
  void restrictCursor(int maxCol) {
    Buffer& b = *buf;
    b.x = std::min(maxCol, std::max(0, b.x));
    b.y = origin ? std::min(b.scrollBottom, std::max(b.scrollTop, b.y)) : std::min(rows - 1, std::max(0, b.y));
    markDirty(b.y);
  }
  void setCursor(long long x, long long y) {
    Buffer& b = *buf;
    markDirty(b.y);
    const long long BIG = 0x3FFFFFFF;
    b.x = clampInt(x, -BIG, BIG);
    b.y = clampInt(origin ? (long long)b.scrollTop + y : y, -BIG, BIG);
    restrictCursor();
    markDirty(b.y);
  }
  void moveCursor(long long x, long long y) {
    // for relative changes we have to make sure we are within 0 .. cols/rows - 1 before calculating the new position
    restrictCursor();
    setCursor(buf->x + x, buf->y + y);
  }
  long long p1(int i = 0) const { return params.params[i] ? params.params[i] : 1; }      // `params.params[i] || 1`

  void cursorUp() {
    const int diffToTop = buf->y - buf->scrollTop;
    if (diffToTop >= 0) moveCursor(0, -std::min<long long>(diffToTop, p1()));
    else moveCursor(0, -p1());
  }
  void cursorDown() {
    const int diffToBottom = buf->scrollBottom - buf->y;
    if (diffToBottom >= 0) moveCursor(0, std::min<long long>(diffToBottom, p1()));
    else moveCursor(0, p1());
  }
  void cursorPosition() { setCursor(params.length >= 2 ? p1(1) - 1 : 0, p1() - 1); }

  void cursorForwardTab() {
    Buffer& b = *buf;
    if (b.x >= cols) return;
    long long param = std::min<long long>(p1(), cols);
    while (param--) b.x = b.nextStop(b.x);
  }
  void cursorBackwardTab() {
    Buffer& b = *buf;
    if (b.x >= cols) return;
    long long param = std::min<long long>(p1(), cols);
    while (param--) b.x = b.prevStop(b.x);
  }

  // ------------------------------------------------------------------ erase
  void eraseInBufferLine(int y, int start, int end, bool clearWrap, bool respectProtect) {
    Line* line = rowPtr(y);
    if (!line) return;
    replaceCells(*line, start, end, eraseCell, respectProtect);
    if (clearWrap) line->wrapped = false;
  }
  void resetBufferLine(int y, bool respectProtect) {
    Line* line = rowPtr(y);
    if (!line) return;
    fillLine(*line, eraseCell, respectProtect);
    line->wrapped = false;
  }

  void eraseInDisplay(bool respectProtect) {
    Buffer& b = *buf;
    restrictCursor(cols);
    int j;
    switch (params.params[0]) {
      case 0:
        j = b.y;
        markDirty(j);
        eraseInBufferLine(j++, b.x, cols, b.x == 0, respectProtect);
        for (; j < rows; j++) resetBufferLine(j, respectProtect);
        markDirty(j);
        break;
      case 1:
        j = b.y;
        markDirty(j);
        // Deleted front part of line and everything before. This line will no longer be wrapped.
        eraseInBufferLine(j, 0, b.x + 1, true, respectProtect);
        if (b.x + 1 >= cols) {
          // Deleted entire previous line. This next line can no longer be wrapped.
          // (xterm indexes the list with j + 1 here, without ybase; kept. Where it would throw, we skip.)
          if (Line* l = cyclicLine(j + 1)) l->wrapped = false;
        }
        while (j--) resetBufferLine(j, respectProtect);
        markDirty(0);
        break;
      case 2:
        j = rows;
        markDirty(j - 1);
        while (j--) resetBufferLine(j, respectProtect);
        markDirty(0);
        break;
      case 3: {
        // Clear scrollback (everything not in viewport)
        const int scrollBackSize = (int)b.lines.size() - rows;
        if (scrollBackSize > 0) {
          b.trimStart((size_t)scrollBackSize);
          b.ybase = std::max(b.ybase - scrollBackSize, 0);
          markAll();
        }
        break;
      }
    }
    dirtyHi = std::min(dirtyHi, rows - 1);
  }

  void eraseInLine(bool respectProtect) {
    Buffer& b = *buf;
    restrictCursor(cols);
    switch (params.params[0]) {
      case 0: eraseInBufferLine(b.y, b.x, cols, b.x == 0, respectProtect); break;
      case 1: eraseInBufferLine(b.y, 0, b.x + 1, false, respectProtect); break;
      case 2: eraseInBufferLine(b.y, 0, cols, true, respectProtect); break;
    }
    markDirty(b.y);
  }

  // ------------------------------------------------------------------ insert / delete / scroll
  bool outsideMargins() const { return buf->y > buf->scrollBottom || buf->y < buf->scrollTop; }

  void insertLines() {
    Buffer& b = *buf;
    restrictCursor();
    if (outsideMargins()) return;
    const int row = b.ybase + b.y;
    const int bottom = b.ybase + b.scrollBottom;
    const int n = (int)std::min<long long>(p1(), bottom - row + 1);
    // n times: drop the region's bottom line, insert a blank one at the cursor row
    std::rotate(b.lines.begin() + row, b.lines.begin() + (bottom + 1 - n), b.lines.begin() + (bottom + 1));
    for (int i = 0; i < n; ++i) makeBlank(b.lines[(size_t)(row + i)], cols, eraseCell, false);
    markRange(b.y, b.scrollBottom);
    b.x = 0;
  }

  void deleteLines() {
    Buffer& b = *buf;
    restrictCursor();
    if (outsideMargins()) return;
    const int row = b.ybase + b.y;
    const int bottom = b.ybase + b.scrollBottom;
    const int n = (int)std::min<long long>(p1(), bottom - row + 1);
    std::rotate(b.lines.begin() + row, b.lines.begin() + (row + n), b.lines.begin() + (bottom + 1));
    for (int i = 0; i < n; ++i) makeBlank(b.lines[(size_t)(bottom - i)], cols, eraseCell, false);
    markRange(b.y, b.scrollBottom);
    b.x = 0;
  }

  void insertChars() {
    restrictCursor();
    if (Line* line = rowPtr(buf->y)) { insertCells(*line, buf->x, (int)p1(), eraseCell); markDirty(buf->y); }
  }
  void deleteChars() {
    restrictCursor();
    if (Line* line = rowPtr(buf->y)) { deleteCells(*line, buf->x, (int)p1(), eraseCell); markDirty(buf->y); }
  }
  void eraseChars() {
    restrictCursor();
    if (Line* line = rowPtr(buf->y)) {
      replaceCells(*line, buf->x, buf->x + (int)std::min<long long>(p1(), 0x3FFFFFFF), eraseCell);
      markDirty(buf->y);
    }
  }

  void scrollUp() {
    Buffer& b = *buf;
    const int top = b.ybase + b.scrollTop, bottom = b.ybase + b.scrollBottom;
    const int n = (int)std::min<long long>(p1(), bottom - top + 1);
    std::rotate(b.lines.begin() + top, b.lines.begin() + (top + n), b.lines.begin() + (bottom + 1));
    for (int i = 0; i < n; ++i) makeBlank(b.lines[(size_t)(bottom - i)], cols, eraseCell, false);
    markRange(b.scrollTop, b.scrollBottom);
  }
  void scrollDown() {
    Buffer& b = *buf;
    const int top = b.ybase + b.scrollTop, bottom = b.ybase + b.scrollBottom;
    const int n = (int)std::min<long long>(p1(), bottom - top + 1);
    std::rotate(b.lines.begin() + top, b.lines.begin() + (bottom + 1 - n), b.lines.begin() + (bottom + 1));
    for (int i = 0; i < n; ++i) makeBlank(b.lines[(size_t)(top + i)], cols, defCell, false);     // DEFAULT_ATTR_DATA, as xterm
    markRange(b.scrollTop, b.scrollBottom);
  }

  // SL / SR / DECIC / DECDC: one cell operation on every line of the scroll region
  void columnsOp(bool insert, int pos) {
    Buffer& b = *buf;
    if (outsideMargins()) return;
    const int param = (int)p1();
    for (int y = b.scrollTop; y <= b.scrollBottom; ++y) {
      Line* line = rowPtr(y);
      if (!line) continue;
      if (insert) insertCells(*line, pos, param, eraseCell);
      else deleteCells(*line, pos, param, eraseCell);
      line->wrapped = false;
    }
    markRange(b.scrollTop, b.scrollBottom);
  }

  void repeatPrecedingCharacter() {
    Buffer& b = *buf;
    const uint32_t joinState = precedingJoinState;
    if (!joinState) return;
    const int chWidth = (int)((joinState >> 1) & 3);
    const int x = b.x - chWidth;
    Line* bufferRow = rowPtr(b.y);
    if (!bufferRow) return;
    std::u32string text;
    if (x >= 0 && x < lineLen(*bufferRow)) {
      const Cell& c = bufferRow->cells[(size_t)x];
      if (c.extra) text = comb[c.extra];
      else if (c.ch) text.push_back(c.ch);
    }
    // xterm builds text * length in one array; anything past a full buffer of repeats cannot change the result
    const long long cap = ((long long)rows + scrollback + 2) * cols;
    const long long length = std::min<long long>(p1(), cap);
    std::vector<uint32_t> data;
    data.reserve(text.size() * (size_t)length);
    for (long long i = 0; i < length; ++i) for (char32_t ch : text) data.push_back((uint32_t)ch);
    print(data.data(), 0, (int)data.size());
  }

  // ------------------------------------------------------------------ modes
  void setMode() {
    for (int i = 0; i < params.length; i++) {
      switch (params.params[i]) {
        case 4: insertMode = true; break;
        case 20: convertEol = true; break;
      }
    }
  }
  void resetMode() {
    for (int i = 0; i < params.length; i++) {
      switch (params.params[i]) {
        case 4: insertMode = false; break;
        case 20: convertEol = false; break;
      }
    }
  }

  void activateAltBuffer() {             // BufferSet.activateAltBuffer(eraseAttr)
    if (buf == &alt) return;
    alt.fillViewportRows(eraseCell);
    alt.x = normal.x;
    alt.y = normal.y;
    buf = &alt;
  }
  void activateNormalBuffer() {          // BufferSet.activateNormalBuffer()
    if (buf == &normal) return;
    normal.x = alt.x;
    normal.y = alt.y;
    alt.clear();
    buf = &normal;
  }

  void setModePrivate() {
    for (int i = 0; i < params.length; i++) {
      switch (params.params[i]) {
        case 1: appCursor = true; break;
        case 2: for (int g = 0; g < 4; ++g) setgCharset(g, 0); break;
        case 3: break;                   // 132 columns: needs windowOptions.setWinLines, off in the deck
        case 6: origin = true; setCursor(0, 0); break;
        case 7: wraparound = true; break;
        case 12: optCursorBlink = true; break;
        case 45: reverseWraparound = true; break;
        case 66: appKeypad = true; break;
        case 9: mouseProtocol = 9; break;
        case 1000: mouseProtocol = 1000; break;
        case 1002: mouseProtocol = 1002; break;
        case 1003: mouseProtocol = 1003; break;
        case 1004: sendFocus = true; break;
        case 1005: break;                // utf8 ext mode mouse - not supported by xterm.js
        case 1006: mouseEncoding = 1006; break;
        case 1015: break;                // urxvt ext mode mouse - not supported by xterm.js
        case 1016: mouseEncoding = 1016; break;
        case 25: cursorHidden = false; break;
        case 1048: saveCursor(); break;
        case 1049: saveCursor();         // FALL-THROUGH
        case 47:
        case 1047: activateAltBuffer(); markAll(); break;
        case 2004: bracketedPaste = true; break;
        case 2026: syncOutput = true; break;
      }
    }
  }

  void resetModePrivate() {
    for (int i = 0; i < params.length; i++) {
      switch (params.params[i]) {
        case 1: appCursor = false; break;
        case 3: break;
        case 6: origin = false; setCursor(0, 0); break;
        case 7: wraparound = false; break;
        case 12: optCursorBlink = false; break;
        case 45: reverseWraparound = false; break;
        case 66: appKeypad = false; break;
        case 9: case 1000: case 1002: case 1003: mouseProtocol = 0; break;
        case 1004: sendFocus = false; break;
        case 1005: break;
        case 1006: mouseEncoding = 0; break;
        case 1015: break;
        case 1016: mouseEncoding = 0; break;
        case 25: cursorHidden = true; break;
        case 1048: restoreCursor(); break;
        case 1049:
        case 47:
        case 1047:
          // Ensure the selection manager has the correct buffer.
          activateNormalBuffer();
          if (params.params[i] == 1049) restoreCursor();
          markAll();
          break;
        case 2004: bracketedPaste = false; break;
        case 2026: syncOutput = false; markAll(); break;
      }
    }
  }

  void requestMode(bool ansi) {
    enum { NOT_RECOGNIZED = 0, SET = 1, RESET = 2, PERMANENTLY_SET = 3, PERMANENTLY_RESET = 4 };
    const long long p = params.params[0];
    auto f = [&](int v) { replyf(ansi ? "\x1b[%lld;%lld$y" : "\x1b[?%lld;%lld$y", p, v); };
    auto b2v = [](bool v) { return v ? SET : RESET; };
    if (ansi) {
      if (p == 2) return f(PERMANENTLY_RESET);
      if (p == 4) return f(b2v(insertMode));
      if (p == 12) return f(PERMANENTLY_SET);
      if (p == 20) return f(b2v(convertEol));
      return f(NOT_RECOGNIZED);
    }
    if (p == 1) return f(b2v(appCursor));
    if (p == 3) return f(NOT_RECOGNIZED);
    if (p == 6) return f(b2v(origin));
    if (p == 7) return f(b2v(wraparound));
    if (p == 8) return f(PERMANENTLY_SET);
    if (p == 9) return f(b2v(mouseProtocol == 9));
    if (p == 12) return f(b2v(optCursorBlink));
    if (p == 25) return f(b2v(!cursorHidden));
    if (p == 45) return f(b2v(reverseWraparound));
    if (p == 66) return f(b2v(appKeypad));
    if (p == 67) return f(PERMANENTLY_RESET);
    if (p == 1000) return f(b2v(mouseProtocol == 1000));
    if (p == 1002) return f(b2v(mouseProtocol == 1002));
    if (p == 1003) return f(b2v(mouseProtocol == 1003));
    if (p == 1004) return f(b2v(sendFocus));
    if (p == 1005) return f(PERMANENTLY_RESET);
    if (p == 1006) return f(b2v(mouseEncoding == 1006));
    if (p == 1015) return f(PERMANENTLY_RESET);
    if (p == 1016) return f(b2v(mouseEncoding == 1016));
    if (p == 1048) return f(SET);        // xterm always returns SET here
    if (p == 47 || p == 1047 || p == 1049) return f(b2v(buf == &alt));
    if (p == 2004) return f(b2v(bracketedPaste));
    if (p == 2026) return f(b2v(syncOutput));
    return f(NOT_RECOGNIZED);
  }

  // ------------------------------------------------------------------ SGR
  static uint32_t updateAttrColor(uint32_t color, int mode, int c1, int c2, int c3) {
    if (mode == 2) {
      color |= CM_RGB;
      color &= ~RGB_MASK;
      color |= ((uint32_t)(c1 & 255) << 16) | ((uint32_t)(c2 & 255) << 8) | (uint32_t)(c3 & 255);
    } else if (mode == 5) {
      color &= ~(CM_MASK | PCOLOR_MASK);
      color |= CM_P256 | (uint32_t)(c1 & 0xff);
    }
    return color;
  }

  // Helper to extract and apply color params/subparams. Returns advance for params index.
  int extractColor(int pos, Attr& attr) {
    // normalize params: meaning [target, CM, ign, val, val, val]; RGB [38/48, 2, ign, r, g, b]; P256 [38/48, 5, ign, v, ign, ign]
    int accu[6] = {0, 0, -1, 0, 0, 0};
    // alignment placeholder for non color space sequences
    int cSpace = 0;
    // return advance we took in params
    int advance = 0;
    do {
      accu[advance + cSpace] = params.params[pos + advance];
      if (params.hasSubParams(pos + advance)) {
        const int ss = params.subStart(pos + advance), se = params.subEnd(pos + advance);
        int i = 0;
        do {
          if (accu[1] == 5) cSpace = 1;
          if (advance + i + 1 + cSpace < 6) accu[advance + i + 1 + cSpace] = params.sub[ss + i];
        } while (++i < se - ss && i + advance + 1 + cSpace < 6);
        break;
      }
      // exit early if can decide color mode with semicolons
      if ((accu[1] == 5 && advance + cSpace >= 2) || (accu[1] == 2 && advance + cSpace >= 5)) break;
      // offset colorSpace slot for semicolon mode
      if (accu[1]) cSpace = 1;
    } while (++advance + pos < params.length && advance + cSpace < 6);
    // set default values to 0
    for (int i = 2; i < 6; ++i) if (accu[i] == -1) accu[i] = 0;
    // apply colors
    switch (accu[0]) {
      case 38: attr.fg = updateAttrColor(attr.fg, accu[1], accu[3], accu[4], accu[5]); break;
      case 48: attr.bg = updateAttrColor(attr.bg, accu[1], accu[3], accu[4], accu[5]); break;
      case 58: break;                    // underline colour: parsed, not kept
    }
    return advance;
  }

  static void processUnderline(int style, Attr& attr) {
    // treat extended attrs as immutable, thus always clone from old one
    if (style == -1 || style > 5) style = 1;
    attr.ul = (uint8_t)style;
    attr.fg |= FG_UNDERLINE;
    // 4:0 and 24 are the same
    if (style == 0) attr.fg &= ~FG_UNDERLINE;
    // update HAS_EXTENDED in BG
    attr.updateExtended();
  }
  static void processSGR0(Attr& attr) {
    attr.fg = 0;
    attr.bg = 0;
    attr.ul = 0;
    attr.updateExtended();
  }

  void charAttributes() {
    Attr& attr = cur;
    // Optimize a single SGR0.
    if (params.length == 1 && params.params[0] == 0) { processSGR0(attr); penChanged(); return; }
    const int l = params.length;
    for (int i = 0; i < l; i++) {
      const int p = params.params[i];
      if (p >= 30 && p <= 37) { attr.fg &= ~(CM_MASK | PCOLOR_MASK); attr.fg |= CM_P16 | (uint32_t)(p - 30); }
      else if (p >= 40 && p <= 47) { attr.bg &= ~(CM_MASK | PCOLOR_MASK); attr.bg |= CM_P16 | (uint32_t)(p - 40); }
      else if (p >= 90 && p <= 97) { attr.fg &= ~(CM_MASK | PCOLOR_MASK); attr.fg |= CM_P16 | (uint32_t)(p - 90) | 8; }
      else if (p >= 100 && p <= 107) { attr.bg &= ~(CM_MASK | PCOLOR_MASK); attr.bg |= CM_P16 | (uint32_t)(p - 100) | 8; }
      else if (p == 0) processSGR0(attr);
      else if (p == 1) attr.fg |= FG_BOLD;
      else if (p == 3) attr.bg |= BG_ITALIC;
      else if (p == 4) { attr.fg |= FG_UNDERLINE; processUnderline(params.hasSubParams(i) ? params.sub[params.subStart(i)] : 1, attr); }
      else if (p == 5) attr.fg |= FG_BLINK;
      else if (p == 7) attr.fg |= FG_INVERSE;
      else if (p == 8) attr.fg |= FG_INVISIBLE;
      else if (p == 9) attr.fg |= FG_STRIKE;
      else if (p == 2) attr.bg |= BG_DIM;
      else if (p == 21) processUnderline(2, attr);
      else if (p == 22) { attr.fg &= ~FG_BOLD; attr.bg &= ~BG_DIM; }
      else if (p == 23) attr.bg &= ~BG_ITALIC;
      else if (p == 24) { attr.fg &= ~FG_UNDERLINE; processUnderline(0, attr); }
      else if (p == 25) attr.fg &= ~FG_BLINK;
      else if (p == 27) attr.fg &= ~FG_INVERSE;
      else if (p == 28) attr.fg &= ~FG_INVISIBLE;
      else if (p == 29) attr.fg &= ~FG_STRIKE;
      else if (p == 39) attr.fg &= ~(CM_MASK | RGB_MASK);
      else if (p == 49) attr.bg &= ~(CM_MASK | RGB_MASK);
      else if (p == 38 || p == 48 || p == 58) i += extractColor(i, attr);
      else if (p == 53) attr.bg |= BG_OVERLINE;
      else if (p == 55) attr.bg &= ~BG_OVERLINE;
      else if (p == 59) attr.updateExtended();
      // anything else: unknown SGR attribute, ignored
    }
    penChanged();
  }

  // ------------------------------------------------------------------ reports
  void deviceStatus() {
    switch (params.params[0]) {
      case 5: reply("\x1b[0n"); break;
      case 6: replyf("\x1b[%lld;%lldR", buf->y + 1, buf->x + 1); break;
    }
  }
  void deviceStatusPrivate() {
    if (params.params[0] == 6) replyf("\x1b[?%lld;%lldR", buf->y + 1, buf->x + 1);
  }

  void softReset() {                     // DECSTR
    Buffer& b = *buf;
    cursorHidden = false;
    b.scrollTop = 0;
    b.scrollBottom = rows - 1;
    cur = Attr();
    penChanged();
    resetCoreModes();
    resetCharsets();
    // reset DECSC data
    b.savedX = 0;
    b.savedY = b.ybase;
    b.savedFg = cur.fg;
    b.savedBg = cur.bg;
    b.savedCharset = charset;
    // reset DECOM
    origin = false;
  }

  void setCursorStyle() {                // DECSCUSR
    const int param = params.length == 0 ? 1 : params.params[0];
    if (param == 0) {
      cursorShape = 0;
      cursorBlinkMode = -1;
    } else {
      switch (param) {
        case 1: case 2: cursorShape = 1; break;
        case 3: case 4: cursorShape = 2; break;
        case 5: case 6: cursorShape = 3; break;
      }
      cursorBlinkMode = (param % 2 == 1) ? 1 : 0;
    }
  }

  void setScrollRegion() {               // DECSTBM
    const long long top = p1();
    long long bottom;
    if (params.length < 2 || (bottom = params.params[1]) > rows || bottom == 0) bottom = rows;
    if (bottom > top) {
      buf->scrollTop = (int)top - 1;
      buf->scrollBottom = (int)bottom - 1;
      setCursor(0, 0);
    }
  }

  void saveCursor() {
    Buffer& b = *buf;
    b.savedX = b.x;
    b.savedY = b.ybase + b.y;
    b.savedFg = cur.fg;
    b.savedBg = cur.bg;
    b.savedCharset = charset;
  }
  void restoreCursor() {
    Buffer& b = *buf;
    b.x = b.savedX;
    b.y = std::max(b.savedY - b.ybase, 0);
    cur.fg = b.savedFg;
    cur.bg = b.savedBg;
    charset = b.savedCharset;
    penChanged();
    restrictCursor();
  }

  void nextLine() { buf->x = 0; index(); }
  void index() {
    Buffer& b = *buf;
    restrictCursor();
    b.y++;
    if (b.y == b.scrollBottom + 1) {
      b.y--;
      scroll(eraseCell);
    } else if (b.y >= rows) {
      b.y = rows - 1;
    }
    restrictCursor();
  }
  void tabSet() { buf->setTab(buf->x, true); }
  void reverseIndex() {
    Buffer& b = *buf;
    restrictCursor();
    if (b.y == b.scrollTop) {
      // possibly move the code below to term.reverseScroll(); test: echo -ne '\e[1;1H\e[44m\eM\e[0m'
      const int top = b.ybase + b.scrollTop, bottom = b.ybase + b.scrollBottom;
      std::rotate(b.lines.begin() + top, b.lines.begin() + bottom, b.lines.begin() + (bottom + 1));
      makeBlank(b.lines[(size_t)top], cols, eraseCell, false);
      markRange(b.scrollTop, b.scrollBottom);
    } else {
      b.y--;
      restrictCursor();                  // quickfix to not run out of bounds
    }
  }

  void screenAlignmentPattern() {        // DECALN
    Buffer& b = *buf;
    Attr a;
    a.fg = cur.fg;
    a.bg = cur.bg;                       // a CellData with the pen's fg/bg and fresh (empty) extended attrs
    Cell cell = toCell(a);
    cell.ch = U'E';
    cell.width = 1;
    setCursor(0, 0);
    for (int yOffset = 0; yOffset < rows; ++yOffset) {
      if (Line* line = cyclicLine(b.ybase + b.y + yOffset)) {
        std::fill(line->cells.begin(), line->cells.end(), cell);
        line->wrapped = false;
      }
    }
    markAll();
    setCursor(0, 0);
  }

  void resetCoreModes() {                // CoreService.reset()
    insertMode = false;
    appCursor = appKeypad = bracketedPaste = origin = reverseWraparound = sendFocus = syncOutput = false;
    wraparound = true;
    cursorShape = 0;
    cursorBlinkMode = -1;
  }
  void resetCharsets() { charset = 0; glevel = 0; gsets[0] = gsets[1] = gsets[2] = gsets[3] = 0; }

  void parserReset() {                   // EscapeSequenceParser.reset()
    state = GROUND;
    oscState = OSC_START; oscId = -1; oscHandled = false; oscHitLimit = false; oscData.clear(); oscUnits = 0;
    dcsActive = false; dcsHitLimit = false; dcsData.clear();
    params.reset();
    params.addParam(0);
    collect = 0;
    precedingJoinState = 0;
    printRunOpen = false;
  }

  void terminalReset() {                 // CoreTerminal.reset()
    cur = Attr();
    penChanged();
    resetBuffers();
    resetCharsets();
    resetCoreModes();
    mouseProtocol = 0;
    mouseEncoding = 0;
    comb.resize(1);
    combIndex.clear();
    markAll();
  }

  // ------------------------------------------------------------------ dispatch
  void csiDispatch(uint32_t ident) {
    Buffer& b = *buf;
    switch (ident) {
      case id1('@'): insertChars(); break;
      case id2(' ', '@'): columnsOp(false, 0); break;                    // SL
      case id1('A'): cursorUp(); break;
      case id2(' ', 'A'): columnsOp(true, 0); break;                     // SR
      case id1('B'): cursorDown(); break;
      case id1('C'): moveCursor(p1(), 0); break;
      case id1('D'): moveCursor(-p1(), 0); break;
      case id1('E'): cursorDown(); b.x = 0; break;
      case id1('F'): cursorUp(); b.x = 0; break;
      case id1('G'): setCursor(p1() - 1, b.y); break;
      case id1('H'): cursorPosition(); break;
      case id1('I'): cursorForwardTab(); break;
      case id1('J'): eraseInDisplay(false); break;
      case id2('?', 'J'): eraseInDisplay(true); break;
      case id1('K'): eraseInLine(false); break;
      case id2('?', 'K'): eraseInLine(true); break;
      case id1('L'): insertLines(); break;
      case id1('M'): deleteLines(); break;
      case id1('P'): deleteChars(); break;
      case id1('S'): scrollUp(); break;
      case id1('T'): scrollDown(); break;
      case id1('X'): eraseChars(); break;
      case id1('Z'): cursorBackwardTab(); break;
      case id1('`'): setCursor(p1() - 1, b.y); break;
      case id1('a'): moveCursor(p1(), 0); break;
      case id1('b'): repeatPrecedingCharacter(); break;
      case id1('c'): if (params.params[0] > 0) break; reply("\x1b[?1;2c"); break;
      case id2('>', 'c'): if (params.params[0] > 0) break; reply("\x1b[>0;276;0c"); break;
      case id1('d'): setCursor(b.x, p1() - 1); break;
      case id1('e'): moveCursor(0, p1()); break;
      case id1('f'): cursorPosition(); break;
      case id1('g'):
        if (params.params[0] == 0) b.setTab(b.x, false);
        else if (params.params[0] == 3) b.tabs.assign(b.tabs.size(), 0);
        break;
      case id1('h'): setMode(); break;
      case id2('?', 'h'): setModePrivate(); break;
      case id1('l'): resetMode(); break;
      case id2('?', 'l'): resetModePrivate(); break;
      case id1('m'): charAttributes(); break;
      case id1('n'): deviceStatus(); break;
      case id2('?', 'n'): deviceStatusPrivate(); break;
      case id2('!', 'p'): softReset(); break;
      case id2(' ', 'q'): setCursorStyle(); break;
      case id1('r'): setScrollRegion(); break;
      case id1('s'): saveCursor(); break;
      case id1('t'): break;                                              // window options: all off in the deck
      case id1('u'): restoreCursor(); break;
      case id2('\'', '}'): columnsOp(true, b.x); break;                  // DECIC
      case id2('\'', '~'): columnsOp(false, b.x); break;                 // DECDC
      case id2('"', 'q'):                                                // DECSCA
        if (params.params[0] == 1) cur.bg |= BG_PROTECTED;
        if (params.params[0] == 2 || params.params[0] == 0) cur.bg &= ~BG_PROTECTED;
        penChanged();
        break;
      case id2('$', 'p'): requestMode(true); break;
      case id3('?', '$', 'p'): requestMode(false); break;
      default: break;
    }
  }

  void selectCharset(int collectChar, int flag) {
    const int id = charsetIdForFinal(flag);
    if (id < 0) return;                  // no handler registered for this final
    int g;
    switch (collectChar) {
      case '(': g = 0; break;
      case ')': case '-': g = 1; break;
      case '*': case '.': g = 2; break;
      case '+': g = 3; break;
      default: return;                   // '/': not supported
    }
    setgCharset(g, id);
  }

  void escDispatch(uint32_t ident) {
    switch (ident) {
      case id1('7'): saveCursor(); return;
      case id1('8'): restoreCursor(); return;
      case id1('D'): index(); return;
      case id1('E'): nextLine(); return;
      case id1('H'): tabSet(); return;
      case id1('M'): reverseIndex(); return;
      case id1('='): appKeypad = true; return;
      case id1('>'): appKeypad = false; return;
      case id1('c'): parserReset(); terminalReset(); return;
      case id1('n'): setgLevel(2); return;
      case id1('o'): setgLevel(3); return;
      case id1('|'): setgLevel(3); return;
      case id1('}'): setgLevel(2); return;
      case id1('~'): setgLevel(1); return;
      case id2('%', '@'): case id2('%', 'G'): setgLevel(0); setgCharset(0, 0); return;
      case id2('#', '8'): screenAlignmentPattern(); return;
      default: break;
    }
    if (ident >> 16) return;
    const int c = (int)(ident >> 8), f = (int)(ident & 0xFF);
    if (c == '(' || c == ')' || c == '*' || c == '+' || c == '-' || c == '.' || c == '/') selectCharset(c, f);
  }

  // ------------------------------------------------------------------ OSC
  static bool oscHasHandler(long long id) {
    switch (id) { case 0: case 1: case 2: case 4: case 8: case 10: case 11: case 12: case 104: case 110: case 111: case 112: return true; }
    return false;
  }
  void oscReset() { oscState = OSC_START; oscId = -1; oscHandled = false; oscHitLimit = false; oscData.clear(); oscUnits = 0; }
  void oscBegin() {                      // OscParser._start(): handlers' start()
    oscHandled = oscHasHandler(oscId);
    oscData.clear(); oscUnits = 0; oscHitLimit = false;
  }
  void oscPut(const uint32_t* data, int start, int end) {
    if (oscState == OSC_ABORT) return;
    if (oscState == OSC_ID) {
      while (start < end) {
        const uint32_t code = data[start++];
        if (code == 0x3b) { oscState = OSC_PAYLOAD; oscBegin(); break; }
        if (code < 0x30 || 0x39 < code) { oscState = OSC_ABORT; return; }
        if (oscId == -1) oscId = 0;
        if (oscId < 100000000000LL) oscId = oscId * 10 + (code - 48);
      }
    }
    if (oscState == OSC_PAYLOAD && end - start > 0 && oscHandled && !oscHitLimit) {
      for (int i = start; i < end; ++i) { appendUtf8(oscData, data[i]); oscUnits += data[i] > 0xFFFF ? 2 : 1; }
      if (oscUnits > PAYLOAD_LIMIT) { oscData.clear(); oscHitLimit = true; }
    }
  }
  void oscEnd(bool success) {
    if (oscState == OSC_START) return;
    if (oscState != OSC_ABORT) {
      // payload may be empty: OSC with only an id still starts and ends its handlers
      if (oscState == OSC_ID) oscBegin();
      if (oscHandled && success && !oscHitLimit) oscDispatch((int)oscId, oscData);
    }
    oscReset();
  }

  void reportColor(const char* ident, int index, int themeIndex) {
    const int64_t c = theme[(size_t)themeIndex];
    if (c < 0) return;
    const unsigned r = (unsigned)(c >> 16) & 255, g = (unsigned)(c >> 8) & 255, bl = (unsigned)c & 255;
    char tmp[96];
    if (index >= 0) snprintf(tmp, sizeof tmp, "\x1b]%s;%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", ident, index, r, r, g, g, bl, bl);
    else snprintf(tmp, sizeof tmp, "\x1b]%s;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", ident, r, r, g, g, bl, bl);
    replies += tmp;
  }

  static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
      size_t b = s.find(sep, a);
      if (b == std::string::npos) { out.push_back(s.substr(a)); break; }
      out.push_back(s.substr(a, b - a));
      a = b + 1;
    }
    return out;
  }

  void oscDispatch(int id, const std::string& data) {
    switch (id) {
      case 0: title = data; iconName = data; break;
      case 1: iconName = data; break;
      case 2: title = data; break;
      case 4: {                          // setOrReportIndexedColor: only "?" reports are acted on
        const std::vector<std::string> slots = split(data, ';');
        for (size_t i = 0; i + 1 < slots.size(); i += 2) {
          const std::string& idx = slots[i];
          const std::string& spec = slots[i + 1];
          if (idx.empty() || idx.find_first_not_of("0123456789") != std::string::npos) continue;
          size_t nz = idx.find_first_not_of('0');
          if (nz != std::string::npos && idx.size() - nz > 3) continue;
          const int index = atoi(idx.c_str());
          if (index > 255) continue;
          if (spec == "?") reportColor("4", index, index);
        }
        break;
      }
      case 8: {                          // setHyperlink
        const size_t idx = data.find(';');
        if (idx == std::string::npos) break;
        const std::string uri = data.substr(idx + 1);
        if (!uri.empty()) {
          cur.url = true;
          cur.updateExtended();
          penChanged();
          break;
        }
        std::string idPart = data.substr(0, idx);
        if (idPart.find_first_not_of(" \t\n\v\f\r") != std::string::npos) break;     // `id.trim()` non-empty: no-op
        cur.url = false;
        cur.updateExtended();
        penChanged();
        break;
      }
      case 10: case 11: case 12: {       // _setOrReportSpecialColor
        static const char* const IDENT[3] = {"10", "11", "12"};
        const std::vector<std::string> slots = split(data, ';');
        int offset = id - 10;
        for (size_t i = 0; i < slots.size(); ++i, ++offset) {
          if (offset >= 3) break;
          if (slots[i] == "?") reportColor(IDENT[offset], -1, 256 + offset);
        }
        break;
      }
      default: break;                    // 104 / 110 / 111 / 112: colour restores, nothing to do here
    }
  }

  // ------------------------------------------------------------------ DCS
  void dcsHook(uint32_t ident) {
    dcsActive = ident == id2('$', 'q');
    dcsData.clear();
    dcsHitLimit = false;
  }
  void dcsPut(const uint32_t* data, int start, int end) {
    if (!dcsActive || dcsHitLimit) return;
    for (int i = start; i < end; ++i) appendUtf8(dcsData, data[i]);
    if (dcsData.size() > PAYLOAD_LIMIT) { dcsData.clear(); dcsHitLimit = true; }
  }
  void dcsUnhook(bool success) {
    if (dcsActive && success && !dcsHitLimit) requestStatusString(dcsData);
    dcsActive = false;
    dcsData.clear();
    dcsHitLimit = false;
  }
  void requestStatusString(const std::string& data) {      // DECRQSS
    if (data == "\"q") { replyf("\x1bP1$r%lld\"q\x1b\\", (cur.bg & BG_PROTECTED) ? 1 : 0); return; }
    if (data == "\"p") { reply("\x1bP1$r61;1\"p\x1b\\"); return; }
    if (data == "r") { replyf("\x1bP1$r%lld;%lldr\x1b\\", buf->scrollTop + 1, buf->scrollBottom + 1); return; }
    if (data == "m") { reply("\x1bP1$r0m\x1b\\"); return; }
    // xterm reads its OPTIONS here (cursorStyle 'block' in the deck), not what DECSCUSR set
    if (data == " q") { replyf("\x1bP1$r%lld q\x1b\\", 2 - (optCursorBlink ? 1 : 0)); return; }
    reply("\x1bP0$r\x1b\\");
  }

  // ------------------------------------------------------------------ the parse loop
  void parse(const uint32_t* data, int length) {
    const uint8_t* table = transitionTable();
    uint32_t code = 0;
    for (int i = 0; i < length; ++i) {
      code = data[i];
      uint8_t transition = table[(size_t)state << 8 | (code < 0xa0 ? code : 0xA0)];
      if ((transition >> 4) != A_PRINT) printRunOpen = false;
      switch (transition >> 4) {
        case A_PRINT: {
          // read ahead with loop unrolling
          int j = i + 1;
          for (; j < length; ++j) {
            const uint32_t c = data[j];
            if (c < 0x20 || (c > 0x7e && c < 0xA0)) break;
          }
          print(data, i, j, printRunOpen);
          printRunOpen = true;
          i = j - 1;
          break;
        }
        case A_EXECUTE:
          execute(code);
          precedingJoinState = 0;
          break;
        case A_IGNORE:
          break;
        case A_ERROR:
          break;                         // xterm logs and carries on in the table's next state
        case A_CSI_DISPATCH:
          csiDispatch(collect << 8 | code);
          precedingJoinState = 0;
          break;
        case A_PARAM:
          do {
            switch (code) {
              case 0x3b: params.addParam(0); break;        // ZDM
              case 0x3a: params.addSubParam(-1); break;
              default: params.addDigit((int)code - 48);    // 0x30 - 0x39
            }
          } while (++i < length && (code = data[i]) > 0x2f && code < 0x3c);
          i--;
          break;
        case A_COLLECT:
          collect <<= 8;
          collect |= code;
          break;
        case A_ESC_DISPATCH:
          escDispatch(collect << 8 | code);
          precedingJoinState = 0;
          break;
        case A_CLEAR:
          params.reset();
          params.addParam(0);            // ZDM
          collect = 0;
          break;
        case A_DCS_HOOK:
          dcsHook(collect << 8 | code);
          break;
        case A_DCS_PUT: {
          int j = i + 1;
          for (; j < length; ++j) {
            const uint32_t c = data[j];
            if (c == 0x18 || c == 0x1a || c == 0x1b || (c > 0x7f && c < 0xA0)) break;
          }
          dcsPut(data, i, j);
          i = j - 1;
          break;
        }
        case A_DCS_UNHOOK:
          dcsUnhook(code != 0x18 && code != 0x1a);
          if (code == 0x1b) transition |= ESCAPE;
          params.reset();
          params.addParam(0);            // ZDM
          collect = 0;
          precedingJoinState = 0;
          break;
        case A_OSC_START:
          oscReset();
          oscState = OSC_ID;
          break;
        case A_OSC_PUT: {
          int j = i + 1;
          for (; j < length; ++j) {
            const uint32_t c = data[j];
            if (c < 0x20 || (c > 0x7f && c < 0xA0)) break;
          }
          oscPut(data, i, j);
          i = j - 1;
          break;
        }
        case A_OSC_END:
          oscEnd(code != 0x18 && code != 0x1a);
          if (code == 0x1b) transition |= ESCAPE;
          params.reset();
          params.addParam(0);            // ZDM
          collect = 0;
          precedingJoinState = 0;
          break;
      }
      state = transition & 15;
    }
  }

  void feed(const char* bytes, size_t n) {
    if (comb.size() > COMBINING_GC_AT) compactCombining();
    const size_t CHUNK = 1 << 16;
    if (decoded.size() < CHUNK + 4) decoded.resize(CHUNK + 4);
    const unsigned char* p = (const unsigned char*)bytes;
    const Buffer* buf0 = buf;
    const int x0 = buf->x, y0 = buf->y;
    const bool hidden0 = cursorHidden;
    while (n) {
      const size_t take = n < CHUNK ? n : CHUNK;
      const size_t len = utf8.decode(p, take, decoded.data());
      if (len) parse(decoded.data(), (int)len);
      p += take;
      n -= take;
    }
    // a moved / shown / hidden cursor damages the row it left and the row it is on
    if (buf != buf0 || buf->x != x0 || buf->y != y0 || cursorHidden != hidden0) {
      markDirty(std::min(std::max(y0, 0), rows - 1));
      markDirty(std::min(std::max(buf->y, 0), rows - 1));
    }
  }

  void ensureRows() {
    // After a resize xterm.js can be left with ybase + rows reaching past the end of its line list. Its list is
    // circular, so its viewport then simply wraps around to the oldest lines and carries on. Two cases:
    //  * the list holds exactly `rows` lines (the alt buffer: it gathers a few hidden scrolled-off lines when
    //    the terminal was made shorter before the alt screen was entered, and loses them again when it grows):
    //    the wrapped viewport is the whole list rotated by ybase. Store it in that order — same picture.
    //  * the scrollback buffer: the wrapped rows would alias the oldest scrollback lines. That cannot be
    //    represented sanely (and xterm draws stale lines there); pull ybase back and pad with blank lines.
    for (Buffer* b : {&normal, &alt}) {
      if (b->lines.empty()) continue;
      if (b->ybase < 0) b->ybase = 0;
      const int size = (int)b->lines.size();
      if (b->ybase + rows > size && size == rows && b->lines.size() == b->maxLength && !b->hasScrollback) {
        std::rotate(b->lines.begin(), b->lines.begin() + (b->ybase % size), b->lines.end());
        b->ybase = 0;
      }
      while ((int)b->lines.size() < b->ybase + rows) {
        if (b->ybase > 0 && b->lines.size() >= b->maxLength) { b->ybase--; continue; }
        b->lines.push_back(blankLine(defCell));
      }
      b->y = std::min(std::max(b->y, 0), rows - 1);
    }
  }

  void resize(int c, int r) {
    c = std::max(c, 2);
    r = std::max(r, 1);
    if (c == cols && r == rows) return;  // the browser Terminal returns early here
    printRunOpen = false;
    cols = c;
    rows = r;
    normal.resize(c, r, defCell, c);
    alt.resize(c, r, defCell, c);
    normal.setupTabStops(c);
    alt.setupTabStops(c);
    ensureRows();
    markAll();
  }
};

// ---------------------------------------------------------------------------------------------- public API

Terminal::Terminal(int cols, int rows, int scrollback) : d(new Impl(cols, rows, scrollback)) {}
Terminal::~Terminal() = default;

void Terminal::feed(const char* data, size_t n) { if (data && n) d->feed(data, n); }
void Terminal::resize(int cols, int rows) { d->resize(cols, rows); }
void Terminal::reset() { d->parserReset(); d->utf8.reset(); d->terminalReset(); }

int Terminal::cols() const { return d->cols; }
int Terminal::rows() const { return d->rows; }
const Line& Terminal::line(int y) const {
  const Buffer& b = *d->buf;
  y = std::min(std::max(y, 0), d->rows - 1);
  return b.lines[(size_t)(b.ybase + y)];
}
int Terminal::scrollbackSize() const { return d->normal.ybase; }
const Line& Terminal::scrollbackLine(int i) const {
  i = std::min(std::max(i, 0), std::max(d->normal.ybase - 1, 0));
  return d->normal.lines[(size_t)i];
}

int Terminal::cursorX() const { return std::min(d->buf->x, d->cols - 1); }
int Terminal::cursorXRaw() const { return d->buf->x; }
int Terminal::cursorY() const { return d->buf->y; }
bool Terminal::cursorVisible() const { return !d->cursorHidden; }
int Terminal::cursorStyle() const { return d->cursorShape == 0 ? 0 : d->cursorShape * 2 - (d->cursorBlinkMode == 1 ? 1 : 0); }
int Terminal::cursorShape() const { return d->cursorShape; }
int Terminal::cursorBlink() const { return d->cursorBlinkMode; }

bool Terminal::altScreen() const { return d->buf == &d->alt; }
bool Terminal::appCursorKeys() const { return d->appCursor; }
bool Terminal::appKeypad() const { return d->appKeypad; }
bool Terminal::bracketedPaste() const { return d->bracketedPaste; }
int Terminal::mouseMode() const { return d->mouseProtocol; }
int Terminal::mouseEncoding() const { return d->mouseEncoding; }
bool Terminal::sgrMouse() const { return d->mouseEncoding == 1006; }
bool Terminal::focusReporting() const { return d->sendFocus; }
bool Terminal::synchronizedOutput() const { return d->syncOutput; }
bool Terminal::originMode() const { return d->origin; }
bool Terminal::autoWrap() const { return d->wraparound; }
bool Terminal::insertMode() const { return d->insertMode; }
int Terminal::scrollTop() const { return d->buf->scrollTop; }
int Terminal::scrollBottom() const { return d->buf->scrollBottom; }

const std::string& Terminal::title() const { return d->title; }
std::string Terminal::takeReplies() { std::string r; r.swap(d->replies); return r; }
std::u32string Terminal::cellText(const Cell& c) const {
  if (c.extra && c.extra < d->comb.size()) return d->comb[c.extra];
  if (c.ch) return std::u32string(1, c.ch);
  return std::u32string();
}
int Terminal::bufferLines() const { return d->buf->ybase + d->rows; }
int Terminal::bufferTop() const { return d->buf->ybase; }
const Line& Terminal::bufferLine(int i) const {
  const Buffer& b = *d->buf;
  i = std::min(std::max(i, 0), b.ybase + d->rows - 1);
  return b.lines[(size_t)i];
}

// xterm.js BufferLine.translateToString(trimRight = true, startCol, endCol), appended to `out`
static void translateLine(const Terminal& t, const Line& l, int startCol, int endCol, std::u32string& out) {
  const int len = (int)l.cells.size();
  // getTrimmedLength(): one past the last cell that has content (a wide character counts both its cells)
  int trimmed = 0;
  for (int i = len - 1; i >= 0; --i) { const Cell& c = l.cells[(size_t)i]; if (c.ch || c.extra) { trimmed = i + c.width; break; } }
  if (endCol < 0 || endCol > len) endCol = len;
  endCol = std::min(endCol, trimmed);
  for (int x = std::max(startCol, 0); x < endCol;) {
    const Cell& c = l.cells[(size_t)x];
    const std::u32string text = t.cellText(c);
    if (text.empty()) out.push_back(U' '); else out += text;
    x += c.width ? c.width : 1;                   // always advance by at least 1
  }
}
std::u32string Terminal::selectionText32(int startCol, int startLine, int endCol, int endLine, const std::u32string& newline) const {
  std::u32string out;
  if (startLine > endLine || (startLine == endLine && startCol >= endCol)) return out;
  const int n = bufferLines();
  // xterm: the first row, the middle rows, the final row - each trimmed; a wrapped row continues the one before
  bool first = true;
  for (int y = startLine; y <= endLine; ++y) {
    const bool inside = y >= 0 && y < n;
    const Line* l = inside ? &bufferLine(y) : nullptr;    // (a row outside the buffer is "" in xterm, and not wrapped)
    if (!first && !(l && l->wrapped)) out += newline;
    if (l) translateLine(*this, *l, y == startLine ? startCol : 0, y == endLine ? endCol : -1, out);
    first = false;
  }
  for (char32_t& c : out) if (c == 0xA0) c = U' ';       // ALL_NON_BREAKING_SPACE_REGEX
  return out;
}
std::string Terminal::selectionText(int startCol, int startLine, int endCol, int endLine, const std::string& newline) const {
  std::u32string nl; for (unsigned char c : newline) nl.push_back((char32_t)c);       // (a newline is ASCII)
  const std::u32string u = selectionText32(startCol, startLine, endCol, endLine, nl);
  std::string s; s.reserve(u.size());
  for (char32_t c : u) {
    if (c < 0x80) s.push_back((char)c);
    else if (c < 0x800) { s.push_back((char)(0xC0 | (c >> 6))); s.push_back((char)(0x80 | (c & 0x3F))); }
    else if (c < 0x10000) { s.push_back((char)(0xE0 | (c >> 12))); s.push_back((char)(0x80 | ((c >> 6) & 0x3F))); s.push_back((char)(0x80 | (c & 0x3F))); }
    else { s.push_back((char)(0xF0 | (c >> 18))); s.push_back((char)(0x80 | ((c >> 12) & 0x3F))); s.push_back((char)(0x80 | ((c >> 6) & 0x3F))); s.push_back((char)(0x80 | (c & 0x3F))); }
  }
  return s;
}
bool Terminal::takeBell() { const bool b = d->bell; d->bell = false; return b; }

bool Terminal::takeDamage(int& firstRow, int& lastRow) {
  if (d->dirtyHi < d->dirtyLo) return false;
  firstRow = std::max(d->dirtyLo, 0);
  lastRow = std::min(d->dirtyHi, d->rows - 1);
  d->dirtyLo = d->rows;
  d->dirtyHi = -1;
  return firstRow <= lastRow;
}

void Terminal::setThemeColor(int index, uint32_t rgb) {
  if (index >= 0 && index < (int)d->theme.size()) d->theme[(size_t)index] = rgb & 0xFFFFFF;
}
void Terminal::setCursorBlinkOption(bool on) { d->optCursorBlink = on; }

std::string Terminal::debugState() const {
  char tmp[400];
  auto f = [&](const Buffer& b, char* out, size_t n) {
    snprintf(out, n, "len=%d max=%d ybase=%d y=%d x=%d st=%d sb=%d start=%d", (int)b.lines.size(), (int)b.maxLength, b.ybase, b.y, b.x,
             b.scrollTop, b.scrollBottom, (int)b.startIndex);
  };
  char n[160], a[160];
  f(d->normal, n, sizeof n);
  f(d->alt, a, sizeof a);
  snprintf(tmp, sizeof tmp, "active=%s | N %s | A %s", d->buf == &d->alt ? "alt" : "normal", n, a);
  return tmp;
}

}}  // namespace deck::vt

// ---------------------------------------------------------------------------------------------------------
// Known differences from xterm.js 6.0.0. Everything else is meant to match, and core/test/vt checks it
// (hand-written streams per feature, real ConPTY captures, a synthesized Claude Code session, seeded fuzz).
//
// Input
//  * Bytes are decoded as Node's StringDecoder + term.write(string) would (U+FFFD for ill-formed UTF-8, U+FEFF
//    dropped), which is the Electron deck's pipeline. xterm's own Uint8Array path drops bad bytes instead and
//    mis-joins a sequence cut after a 0x80 continuation byte.
//  * The result never depends on where feed() calls are cut. xterm.js has one place where it does: with DECAWM
//    off, a wide char that does not fit at the right edge, then another print call (see print()). The engine
//    always gives xterm's answer for the uncut stream.
//
// Deliberately not copied (xterm.js bugs that garble content; each found by the fuzz, each small)
//  * Reflow + combining marks: xterm's BufferLine.copyCellsFrom copies the combined-string map entries of the
//    whole source line, so cells next to a reflowed run can end up showing another cell's accents / ZWJ
//    sequence. Here every cell keeps its own text. (Fixtures that resize carry no combining marks.)
//  * Alt buffer grown while it holds hidden scrolled-off lines (it collects those when the terminal was made
//    shorter before the alt screen was entered): xterm's viewport ends up wrapped around its circular line
//    list and IL/DL/region scrolls then corrupt or throw. The engine shows the same picture at the resize
//    (ensureRows) and stays sound afterwards.
//  * Scrollback 0 with reflow, and any other case where xterm's ybase + rows passes the end of its list:
//    xterm aliases the oldest lines into the viewport; here ybase is pulled back.
//  * DECALN (ESC # 8) in origin mode after a reflow, ED 1 at the last column: xterm reaches past the end of its
//    list into slots that still alias live lines (or throws). Here: wraps only in a genuinely full list.
//  * Loops xterm runs "param" times (IL/DL/SU/SD/CHT/CBT/REP) are clamped to where the result stops changing;
//    xterm spins or throws on huge counts. Combining sequences are capped at 64 code points per cell.
//  * Same-size resize is a no-op (the browser Terminal's early return); headless xterm would reset the margins.
//
// Not implemented because the deck's xterm.js does not do them either
//  * Unicode 11+/grapheme widths (emoji are 1 cell wide, as in the Electron deck — no unicode11 addon there),
//    XTVERSION (CSI > q: no reply), window ops (CSI t, incl. title stack), DECCOLM (?3), mouse encodings
//    1005/1015, OSC 52 clipboard, OSC 7 / 133 / 9 and every other OSC without a core handler, sixel/DCS other
//    than DECRQSS, kitty keyboard protocol (CSI u forms other than restore-cursor are ignored).
//
// Kept smaller than xterm.js
//  * OSC 8: cells get the Hyperlink flag and the dashed underline; the URI is not stored.
//  * SGR 58/59 underline colour is parsed and dropped. OSC 1 icon name is stored, not exposed.
//  * OSC 4/10/11/12 "?" are answered only for colours given via setThemeColor(); setting colours through OSC
//    (and OSC 104/110/111/112) is swallowed.
//  * No viewport scroll position (ydisp), selection, markers or decorations: renderer / host business.
