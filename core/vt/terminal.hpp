// core/vt/terminal.hpp — the deck's terminal engine: a VT parser + screen model, a C++17 port of the parts of
// xterm.js 6 that decide what is on screen. Pure C++: no OS calls, no threads, no third-party code.
//
// xterm.js is the specification. The parser's transition table, the print/wrap logic, the buffer operations,
// resize + reflow and the Unicode V6 width tables are ported from @xterm/xterm 6.0.0 (src/common), and
// core/test/vt replays byte streams through both and compares cell by cell. Where xterm.js is odd, this is odd
// the same way; the few places where it deliberately is not are listed at the bottom of terminal.cpp.
//
// Options mirrored from the Electron deck (renderer/js/20-terminals.js):
//   scrollback 10000, cursorBlink true, windowsPty { backend: 'conpty', buildNumber: 26200 }
// which in xterm.js means: resize REFLOWS the normal buffer (wrapped lines re-wrap; the line under the cursor
// is left alone), never the alt buffer, and growing rows appends blank lines instead of pulling scrollback down.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace deck { namespace vt {

// Four bytes: a cell is kept for every column of every line of the 10,000-line scroll-back, per terminal, so its
// size is most of what a busy terminal costs (2026-11-24: 28 bytes a cell -> 20; 12 panes of scroll-back at 55
// columns ~185 MB -> ~132 MB). Both are plain unsigned bitfields of one type, so every ABI (MinGW's ms bitfields
// too) puts them in one 32-bit unit; `kind` reads and compares as a Kind (c.kind == Color::RGB, switch (c.kind)).
struct Color {
  enum Kind : uint8_t { Default, Palette, RGB };
  uint32_t value : 24;                              // Palette: 0-255; RGB: 0xRRGGBB; Default: 0
  uint32_t kind : 8;                                // a Kind
};
static_assert(sizeof(Color) == 4, "a cell colour is four bytes");
inline bool operator==(const Color& a, const Color& b) { return a.kind == b.kind && a.value == b.value; }
inline bool operator!=(const Color& a, const Color& b) { return !(a == b); }

enum CellFlags : uint16_t {
  Bold = 1, Dim = 2, Italic = 4, Underline = 8, Blink = 16, Inverse = 32, Invisible = 64, Strike = 128, Overline = 256,
  // bits 9-11: underline style when Underline is set — 1 single, 2 double, 3 curly, 4 dotted, 5 dashed
  // (xterm.js gives a cell inside an OSC 8 hyperlink Underline + dashed, exactly like its renderer shows it)
  UnderlineStyleShift = 9, UnderlineStyleMask = 7 << 9,
  Protected = 0x1000,    // DECSCA: survives the selective erases CSI ? J / CSI ? K
  Hyperlink = 0x2000,    // written while an OSC 8 hyperlink was open (the URI itself is not kept)
};

struct Cell {
  char32_t ch;       // base code point; 0 = empty cell
  uint8_t width;     // 1, 2, or 0 for the right half of a wide char
  uint16_t flags;    // CellFlags
  Color fg, bg;
  uint32_t extra;    // 0, or the id of the cell's full code point sequence (base + combining marks) — see cellText()
};
static_assert(sizeof(Cell) == 20, "a cell is twenty bytes (it is kept per column of every scroll-back line)");

struct Line {
  std::vector<Cell> cells;   // at least cols() cells. May be LONGER than cols() on the alt buffer after it was
                             // narrowed (xterm.js keeps those cells too): only the first cols() are on screen.
  bool wrapped;              // this row continues the row above (soft wrap)
};

class Terminal {
 public:
  Terminal(int cols, int rows, int scrollback = 10000);
  ~Terminal();
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  // Raw pty bytes, UTF-8. May be split anywhere, including mid escape sequence and mid code point.
  // Ill-formed UTF-8 becomes U+FFFD the way Node's StringDecoder does it (that is what the Electron deck feeds xterm).
  void feed(const char* data, size_t n);
  void resize(int cols, int rows);                // min 2 x 1, like xterm.js. Reflows the normal buffer (see top).
  void reset();                                   // hard reset (RIS) + parser/decoder state

  int cols() const;
  int rows() const;
  // References returned by line() / scrollbackLine(), and Cell::extra ids, are valid until the next feed(),
  // resize() or reset(). The class is not thread-safe: one thread at a time.
  const Line& line(int y) const;                  // viewport row of the ACTIVE buffer, 0 = top
  int scrollbackSize() const;                     // lines above the viewport — normal buffer only
  const Line& scrollbackLine(int i) const;        // 0 = oldest

  int cursorX() const;                            // 0..cols-1 (clamped: where a renderer draws it)
  int cursorXRaw() const;                         // xterm's buffer.x: equals cols() while a wrap is pending
  int cursorY() const;
  bool cursorVisible() const;                     // DECTCEM
  int cursorStyle() const;                        // DECSCUSR number: 0 = terminal default, 1/2 block, 3/4 underline, 5/6 bar (odd = blinking)
  int cursorShape() const;                        // 0 default, 1 block, 2 underline, 3 bar
  int cursorBlink() const;                        // -1 default, 0 steady, 1 blinking

  bool altScreen() const;
  bool appCursorKeys() const;                     // DECCKM
  bool appKeypad() const;                         // DECKPAM / DECKPNM
  bool bracketedPaste() const;                    // 2004
  int mouseMode() const;                          // 0 none, 9 x10, 1000, 1002, 1003
  int mouseEncoding() const;                      // 0 default (X10 bytes), 1006 SGR, 1016 SGR-pixels
  bool sgrMouse() const;                          // mouseEncoding() == 1006
  bool focusReporting() const;                    // 1004
  bool synchronizedOutput() const;                // 2026: the app is mid-frame; a renderer may hold the paint
  bool originMode() const;
  bool autoWrap() const;
  bool insertMode() const;
  int scrollTop() const;                          // DECSTBM, 0-based, inclusive
  int scrollBottom() const;

  const std::string& title() const;               // OSC 0 / OSC 2, UTF-8
  std::string takeReplies();                      // bytes to write back to the pty (DSR, DA1/DA2, DECRQM, DECRQSS, OSC colour reports)
  std::u32string cellText(const Cell&) const;     // base char + combining marks; empty for an empty cell

  // ---- the ACTIVE buffer by absolute line, as a selection addresses it (xterm's buffer.lines): 0 = its oldest
  // line, bufferTop() = the viewport's top row. Normal buffer: bufferTop() == scrollbackSize(). The alt buffer has
  // no scrollback, so its bufferTop() is 0 - except after it was made shorter, when xterm keeps the rows that
  // left the top hidden above the viewport (and so does this).
  int bufferLines() const;                        // bufferTop() + rows()
  int bufferTop() const;
  const Line& bufferLine(int i) const;            // clamped like line()
  // The text a selection copies, EXACTLY as xterm.js builds it (SelectionService.selectionText over
  // BufferLine.translateToString(trimRight = true)): from cell (startCol, startLine) up to but NOT including
  // (endCol, endLine), lines absolute as above.
  //   - every row loses its trailing EMPTY cells (never-written or erased ones; a space the app wrote is text);
  //   - a row that continues the row above (soft wrap) is joined to it with nothing between, any other starts
  //     after `newline` (xterm: CRLF on Windows, LF elsewhere - the shell says which);
  //   - an empty cell inside the text is a space, a wide character is taken once, a no-break space becomes a space.
  // Empty when the range is empty or backwards. Columns and lines outside the buffer are clamped.
  std::u32string selectionText32(int startCol, int startLine, int endCol, int endLine, const std::u32string& newline = U"\n") const;
  std::string selectionText(int startCol, int startLine, int endCol, int endLine, const std::string& newline = "\n") const;   // UTF-8
  bool takeBell();                                // true when BEL arrived since the last take

  // Damage: the span of viewport rows whose cells may have changed since the last take, plus the rows the
  // cursor left and entered. Scrolls, buffer switches and resizes report all rows. Title, modes and the
  // cursor's style are not damage — read them after each feed.
  bool takeDamage(int& firstRow, int& lastRow);   // false when nothing changed

  // Host colours for OSC 4 / 10 / 11 / 12 "?" queries. index 0-255 = palette, 256 = foreground, 257 = background,
  // 258 = cursor. A query for a colour that was never set gets no reply (as headless xterm.js gives none).
  void setThemeColor(int index, uint32_t rgb);
  // xterm's `cursorBlink` OPTION (the deck passes true): only read back by DECRQM ?12 and DECRQSS " q".
  void setCursorBlinkOption(bool on);

  // One line of internal counters for both buffers (line count, capacity, ybase, cursor, margins) — for logs
  // and for comparing against xterm.js when a parity test fails. Not a stable format.
  std::string debugState() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}}  // namespace deck::vt
