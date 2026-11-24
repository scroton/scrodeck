// windows/src/termview.hpp — one terminal pane: a core VT screen (core/vt) drawn with Direct2D.
//
// The model is the core's; this file is only its picture and its keys. The look is the Electron deck's:
// renderer/js/20-terminals.js gives the theme, the font stack, line-height 1.1 and the fit rule, and
// renderer/style.css the pane's padding (a one-line band on top for the pane's name, 4 right, 8 bottom,
// 8 left). Only placed panes are ever drawn; a pane nobody shows is grid state with no draw path.
#pragma once
#include "gfx.hpp"
#include "mouse.hpp"
#include "../../core/vt/terminal.hpp"
#include "../../core/vt/selection.hpp"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace deck {

// 20-terminals.js THEME
struct Theme {
  uint32_t background = 0x0c0c0c, foreground = 0xe6e6e6, cursor = 0xe6e6e6, cursorAccent = 0x0c0c0c, selection = 0x3a3d41;
  uint32_t ansi[16] = { 0x0c0c0c, 0xc50f1f, 0x13a10e, 0xc19c00, 0x0037da, 0x881798, 0x3a96dd, 0xcccccc,
                        0x767676, 0xe74856, 0x16c60c, 0xf9f1a5, 0x3b78ff, 0xb4009e, 0x61d6d6, 0xf2f2f2 };
  uint32_t palette(int i) const {
    if (i < 16) return ansi[i];
    if (i < 232) { int n = i - 16; static const int v[6] = { 0, 95, 135, 175, 215, 255 }; return (v[n / 36] << 16) | (v[(n / 6) % 6] << 8) | v[n % 6]; }
    int g = 8 + (i - 232) * 10; return (g << 16) | (g << 8) | g;
  }
};

struct CellMetrics { float w = 8, h = 17, ascentPad = 0; float fontSize = 14; };

// xterm measures a character box and multiplies the height by lineHeight; the fit addon divides the
// pane by that box. DirectWrite gives the same numbers from the font's own metrics.
inline CellMetrics measureCells(Gfx& g, const std::wstring& family, float fontSize, float lineHeight = 1.1f) {
  CellMetrics m; m.fontSize = fontSize;
  Font f; f.family = family; f.size = fontSize;
  Com<IDWriteTextLayout> lay = g.layout(L"WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", f, 10000, 1000, false, false, 1);
  if (lay) {
    DWRITE_TEXT_METRICS tm; lay->GetMetrics(&tm);
    m.w = tm.widthIncludingTrailingWhitespace / 32.f;
    float charH = std::ceil(tm.height);
    m.h = std::floor(charH * lineHeight);
    m.ascentPad = (m.h - tm.height) / 2;
  }
  if (m.w < 1) m.w = fontSize * 0.6f;
  if (m.h < 1) m.h = std::floor(fontSize * 1.3f);
  return m;
}

// the first family of the deck's stack that this machine has: the chosen one, then "Cascadia Mono", "Cascadia
// Code", Consolas (20-terminals.js fontStack). A bundled font loaded from its file (Gfx::loadFontFolder) counts as
// had, and comes before an installed one of the same name, as an @font-face family does.
inline std::wstring resolveMonoFamily(Gfx& g, const std::wstring& wanted) {
  if (!wanted.empty() && g.hasPrivate(wanted)) return wanted;
  Com<IDWriteFontCollection> coll; g.dw->GetSystemFontCollection(coll.put(), FALSE);
  const std::wstring tries[] = { wanted, L"Cascadia Mono", L"Cascadia Code", L"Consolas", L"Courier New" };
  for (const auto& t : tries) {
    if (t.empty() || !coll) continue;
    UINT32 idx = 0; BOOL exists = FALSE;
    coll->FindFamilyName(t.c_str(), &idx, &exists);
    if (exists) return t;
  }
  return L"Consolas";
}

struct Selection {
  bool active = false;
  int ax = 0, ay = 0, bx = 0, by = 0;    // anchor and head, in absolute lines (scrollback + viewport) and columns
  void ordered(int& x0, int& y0, int& x1, int& y1) const {
    if (ay < by || (ay == by && ax <= bx)) { x0 = ax; y0 = ay; x1 = bx; y1 = by; } else { x0 = bx; y0 = by; x1 = ax; y1 = ay; }
  }
  bool has(int x, int y) const {
    if (!active) return false;
    int x0, y0, x1, y1; ordered(x0, y0, x1, y1);
    if (y < y0 || y > y1) return false;
    if (y == y0 && x < x0) return false;
    if (y == y1 && x >= x1) return false;
    return true;
  }
  bool empty() const { return !active || (ax == bx && ay == by); }
};

class TermView {
 public:
  vt::Terminal term;
  int scrollOff = 0;            // lines scrolled back from the bottom (the normal buffer only)
  Selection sel;
  bool everAttached = false, attached = false;
  bool touched = false;         // a real key was pressed here (deck.userInput is sent once)
  bool repaintAll = true;       // the scene holds nothing valid of this pane (it was reset, or never drawn)
  // the top of the grid for a pane box and cell: a grid taller than the box shows its BOTTOM (see draw)
  float gridTop(const RectF& box, const CellMetrics& m) const { const float gridH = term.rows() * m.h; return box.y + (gridH > box.h + 1 ? std::round(box.h - gridH) : 0); }
  std::string oscTitle;         // the window title last told to the deck (local mode: deck.setOscTitle)
  unsigned long long syncSince = 0;   // DEC 2026: when the application began the frame the pane is holding its picture for (0 = not held)
  mouse::Encoder mouseEnc;      // LOCAL mode: xterm's CoreMouseService state for this terminal (the last event it saw)
  mouse::WheelSum wheelSum;     // CoreMouseService._wheelPartialScroll
  // xterm's DOM renderer (what the deck runs: no WebGL addon) draws box characters with the font's own
  // glyphs, gaps between rows included. false = the same; true = joined lines drawn as geometry.
  bool customBox = false;
  float fontSize = 14;          // what it is drawn at (a mirrored pane may shrink it)
  RectF lastRect;               // where it was last drawn (hit tests, mouse → cell)
  CellMetrics lastCell;
  float lastOx = 0, lastOy = 0;
  // An input method is composing here (WM_IME_COMPOSITION, GCS_COMPSTR): the text so far, drawn in the grid at the
  // cursor as xterm draws its composition view. Empty = none. Never part of the terminal's own screen.
  std::wstring composing;

  TermView() : term(80, 24, 10000) {}

  int absLines() const { return term.altScreen() ? term.rows() : term.scrollbackSize() + term.rows(); }
  int topLine() const { return term.altScreen() ? 0 : std::max(0, term.scrollbackSize() - scrollOff); }
  const vt::Line& absLine(int i) const {
    if (term.altScreen()) return term.line(i);
    int sb = term.scrollbackSize();
    return i < sb ? term.scrollbackLine(i) : term.line(i - sb);
  }
  void scrollBy(int lines) {
    if (term.altScreen()) { scrollOff = 0; return; }
    scrollOff = std::max(0, std::min(term.scrollbackSize(), scrollOff + lines));
  }

  // the text box inside a pane: style.css `.term .xterm { padding: name band, 4, 8, 8 }`
  static RectF contentBox(const RectF& pane, float paneNameSize) {
    float top = paneNameSize * 1.2f + 6;
    return RectF(pane.x + 8, pane.y + top, pane.w - 12, pane.h - top - 8);
  }
  // FitAddon.proposeDimensions: the box less the 14px scrollbar gutter, over the cell
  static void fit(const RectF& box, const CellMetrics& m, int& cols, int& rows) {
    cols = std::max(2, (int)std::floor((box.w - 14) / m.w));
    rows = std::max(1, (int)std::floor(box.h / m.h));
  }

  uint32_t rgbOf(const vt::Color& c, const Theme& th, bool fg) const {
    if (c.kind == vt::Color::Palette) return th.palette((int)c.value & 255);
    if (c.kind == vt::Color::RGB) return c.value & 0xffffff;
    return fg ? th.foreground : th.background;
  }

  // Draws the grid into `box` (already the padded content box). `focused` = this pane has the keys.
  // A grid taller than the box shows its BOTTOM (applyMirror: the prompt stays in view).
  // `pane` is the whole pane: xterm's viewport (and so its scrollbar) spans it, padding included.
  // `rowFrom`..`rowTo` (inclusive): only these viewport rows are drawn — the caller repaints a band of the pane
  // under a clip of its own (app.cpp paintPaneRows); the cursor and the scrollbar are drawn either way, and the
  // clip keeps what lies outside the band as it was.
  void draw(Gfx& g, const RectF& box, const std::wstring& family, const CellMetrics& m, const Theme& th, bool focused, bool blinkOn, const RectF* pane = nullptr,
            int rowFrom = 0, int rowTo = 1 << 30) {
    lastRect = box; lastCell = m;
    const int cols = term.cols(), rows = term.rows();
    float gridH = rows * m.h;
    float oy = box.y + (gridH > box.h + 1 ? std::round(box.h - gridH) : 0);
    float ox = box.x;
    lastOx = ox; lastOy = oy;
    g.pushClip(box);
    Font base; base.family = family; base.size = m.fontSize;
    const int top = topLine();
    std::wstring run;
    for (int y = std::max(0, rowFrom); y < rows && y <= rowTo; y++) {
      float py = oy + y * m.h;
      if (py + m.h < box.y || py > box.b()) continue;
      const vt::Line& ln = absLine(top + y);
      const int n = std::min(cols, (int)ln.cells.size());
      // backgrounds: runs of one colour
      for (int x = 0; x < n;) {
        const vt::Cell& c = ln.cells[x];
        bool inv = (c.flags & vt::Inverse) != 0;
        bool selHere = sel.has(x, top + y);
        uint32_t bg = selHere ? th.selection : (inv ? rgbOf(c.fg, th, true) : rgbOf(c.bg, th, false));
        int x2 = x + 1;
        while (x2 < n) {
          const vt::Cell& d = ln.cells[x2];
          bool inv2 = (d.flags & vt::Inverse) != 0;
          uint32_t bg2 = sel.has(x2, top + y) ? th.selection : (inv2 ? rgbOf(d.fg, th, true) : rgbOf(d.bg, th, false));
          if (bg2 != bg) break;
          x2++;
        }
        if (bg != th.background) g.fill(RectF(ox + x * m.w, py, (x2 - x) * m.w + 0.5f, m.h), Rgba::hex(bg));
        x = x2;
      }
      // text: runs of plain ASCII in one style go out as one string; anything else cell by cell
      for (int x = 0; x < n;) {
        const vt::Cell& c = ln.cells[x];
        if (c.width == 0 || c.ch == 0 || c.ch == U' ' || (c.flags & vt::Invisible)) {
          if (c.ch == U' ' && (c.flags & (vt::Underline | vt::Strike))) decorate(g, c, ox + x * m.w, py, m.w, m, th);
          x++; continue;
        }
        bool inv = (c.flags & vt::Inverse) != 0;
        uint32_t fg = inv ? rgbOf(c.bg, th, false) : rgbOf(c.fg, th, true);
        // xterm draws bold in the bright colour for the low 8 (drawBoldTextInBrightColors, on by default)
        if ((c.flags & vt::Bold) && !inv && c.fg.kind == vt::Color::Palette && c.fg.value < 8) fg = th.palette((int)c.fg.value + 8);
        Rgba col = Rgba::hex(fg, (c.flags & vt::Dim) ? 0.5f : 1.f);
        Font f = base; f.weight = (c.flags & vt::Bold) ? 700 : 400; f.italic = (c.flags & vt::Italic) != 0;
        bool ascii = c.ch < 0x80 && c.extra == 0 && c.width == 1;
        if (ascii) {
          run.clear(); run.push_back((wchar_t)c.ch);
          int x2 = x + 1;
          while (x2 < n) {
            const vt::Cell& d = ln.cells[x2];
            if (!(d.ch >= 0x20 && d.ch < 0x80) || d.extra || d.width != 1 || d.flags != c.flags || !sameColor(d.fg, c.fg) || !sameColor(d.bg, c.bg)) break;
            run.push_back((wchar_t)d.ch); x2++;
          }
          drawRun(g, run, ox + x * m.w, py, (x2 - x) * m.w, m, f, col);
          if (c.flags & (vt::Underline | vt::Strike | vt::Overline)) decorate(g, c, ox + x * m.w, py, (x2 - x) * m.w, m, th);
          x = x2;
        } else {
          int w = c.width == 2 ? 2 : 1;
          if (!drawBox(g, c.ch, ox + x * m.w, py, m, col)) {
            std::u32string u = term.cellText(c);
            std::wstring ws = utf16(u);
            drawRun(g, ws, ox + x * m.w, py, w * m.w, m, f, col);
          }
          if (c.flags & (vt::Underline | vt::Strike | vt::Overline)) decorate(g, c, ox + x * m.w, py, w * m.w, m, th);
          x += w;
        }
      }
    }
    // The cursor, as xterm's DOM renderer draws it. With the focus: the shape the application asked for
    // (DECSCUSR: block, underline, bar; the deck's default is the block), blinking 600 ms on / 600 ms off unless
    // the application asked for a steady one. Without it: a 1px outline whatever the shape (cursorInactiveStyle).
    if (term.cursorVisible() && scrollOff == 0) {
      int cx = std::min(term.cursorX(), cols - 1), cy = term.cursorY();
      RectF cr(ox + cx * m.w, oy + cy * m.h, m.w, m.h);
      const int shape = term.cursorShape();
      const bool on = blinkOn || term.cursorBlink() == 0;
      if (!focused) g.stroke(cr, Rgba::hex(th.cursor), 1);
      else if (on) {
        if (shape == 2) g.fill(RectF(cr.x, cr.b() - 2, cr.w, 1), Rgba::hex(th.cursor));           // underline: a 1px line, one row up from the cell's foot
        else if (shape == 3) g.fill(RectF(cr.x, cr.y, 1, cr.h), Rgba::hex(th.cursor));            // bar: cursorWidth 1
        else {
          g.fill(cr, Rgba::hex(th.cursor));
          const vt::Line& ln = absLine(top + cy);
          if (cx < (int)ln.cells.size() && ln.cells[cx].ch > U' ') {
            Font f = base;
            drawRun(g, utf16(term.cellText(ln.cells[cx])), cr.x, cr.y, cr.w * (ln.cells[cx].width == 2 ? 2 : 1), m, f, Rgba::hex(th.cursorAccent));
          }
        }
      }
    }
    // xterm's composition view (xterm.css `.xterm .composition-view { background: #000; color: #FFF; position:
    // absolute; white-space: nowrap }`, CompositionHelper.updateCompositionElements: at the cursor's cell, one cell
    // high, in the terminal's own font): the composing text over the grid, one line, running on past the cursor.
    if (!composing.empty() && scrollOff == 0) {
      const int cx = std::min(term.cursorX(), cols - 1), cy = term.cursorY();
      Font f = base;
      IDWriteTextLayout* lay = g.runLayout(composing, f);
      DWRITE_TEXT_METRICS tm{}; if (lay) lay->GetMetrics(&tm);
      const float w = std::max(m.w, std::ceil(tm.widthIncludingTrailingWhitespace));
      const RectF r(ox + cx * m.w, oy + cy * m.h, w, m.h);
      g.fill(r, Rgba::hex(0x000000));
      drawRun(g, composing, r.x, r.y, w, m, f, Rgba::hex(0xffffff));
    }
    g.popClip();
    // The viewport's scrollbar (css .xterm-viewport: scrollbar-width thin, #333 on nothing): there whenever the
    // normal buffer has scrollback, down the pane's right edge, the thumb where the view is.
    if (!term.altScreen() && term.scrollbackSize() > 0) {
      const RectF pr = pane ? *pane : box;
      const float total = (float)(term.scrollbackSize() + rows);
      const float track = pr.h - 2, th_ = std::max(17.f, track * rows / total);
      const float ty = pr.y + 1 + (track - th_) * (float)top / (float)term.scrollbackSize();
      g.fill(RectF(pr.r() - 1 - 8, ty, 6, th_), Rgba::hex(0x333333));
    }
  }
  // The scrollbar as drawn above, for the mouse: false when there is none. `thumb` is the slider, `track` the
  // strip a press may land in (the viewport's 14px gutter: FitAddon leaves it free of cells).
  bool scrollbarRects(const RectF& pr, RectF& track, RectF& thumb) const {
    if (term.altScreen() || term.scrollbackSize() <= 0) return false;
    const float total = (float)(term.scrollbackSize() + term.rows());
    const float tr = pr.h - 2, th_ = std::max(17.f, tr * term.rows() / total);
    const float ty = pr.y + 1 + (tr - th_) * (float)topLine() / (float)term.scrollbackSize();
    track = RectF(pr.r() - 14, pr.y + 1, 14, tr); thumb = RectF(pr.r() - 14, ty, 14, th_);
    return true;
  }
  // the slider's top edge put at `y` (window coordinates): the view follows, as a dragged scrollbar does
  void scrollThumbTo(const RectF& pr, float y) {
    RectF track, thumb; if (!scrollbarRects(pr, track, thumb)) return;
    const float room = track.h - thumb.h; if (room <= 0) return;
    const float f = std::max(0.f, std::min(1.f, (y - track.y) / room));
    const int top = (int)std::lround(f * term.scrollbackSize());
    scrollOff = std::max(0, std::min(term.scrollbackSize(), term.scrollbackSize() - top));
  }

  // a point in the window → the cell under it (clamped), in absolute lines
  void cellAt(float px, float py, int& col, int& absRow) const {
    int c = (int)std::floor((px - lastOx) / std::max(1.f, lastCell.w));
    int r = (int)std::floor((py - lastOy) / std::max(1.f, lastCell.h));
    col = std::max(0, std::min(term.cols(), c));
    absRow = topLine() + std::max(0, std::min(term.rows() - 1, r));
  }

  // xterm's double click: the word under the pointer (its default separators: space ( ) [ ] { } ' , " `);
  // a triple click: the whole line
  static bool wordSep(char32_t c) { return c == 0 || c == U' ' || c == U'(' || c == U')' || c == U'[' || c == U']' || c == U'{' || c == U'}' || c == U'\'' || c == U',' || c == U'"' || c == U'`'; }
  void selectWord(int col, int absRow) {
    if (absRow < 0 || absRow >= absLines()) return;
    const vt::Line& ln = absLine(absRow);
    const int n = std::min(term.cols(), (int)ln.cells.size());
    if (n <= 0) return;
    int c = std::max(0, std::min(n - 1, col));
    auto sepAt = [&](int x) { return wordSep(ln.cells[x].width == 0 && x > 0 ? ln.cells[x - 1].ch : ln.cells[x].ch); };
    int a = c, b = c;
    const bool onSep = sepAt(c);
    while (a > 0 && sepAt(a - 1) == onSep && (!onSep || ln.cells[a - 1].ch == ln.cells[c].ch)) a--;
    while (b + 1 < n && sepAt(b + 1) == onSep && (!onSep || ln.cells[b + 1].ch == ln.cells[c].ch)) b++;
    sel.active = true; sel.ax = a; sel.ay = absRow; sel.bx = b + 1; sel.by = absRow;
  }
  void selectLine(int absRow) {
    if (absRow < 0 || absRow >= absLines()) return;
    int a = absRow, b = absRow;
    while (a > 0 && absLine(a).wrapped) a--;                                  // a wrapped line is one line
    while (b + 1 < absLines() && absLine(b + 1).wrapped) b++;
    sel.active = true; sel.ax = 0; sel.ay = a; sel.bx = term.cols(); sel.by = b;
  }

  // ---- the selection follows its text when a full-screen session redraws (2026-11-19). THE RULES ARE THE
  // TERMINAL ENGINE'S (core/vt/selection.hpp SelectionFollow, shared with the Mac shell and tested in core/test/vt);
  // this view's lines are the engine's "selection lines" (the alt screen from the viewport's top).
  vt::SelectionFollow selFollow;         // the covered rows' text when the selection was made; inactive = nothing to follow
  // call when a selection is made or changed (mouse up, double / triple click, Shift+click)
  void snapSelection() {
    selFollow.clear();
    if (!sel.active || sel.empty()) return;
    int x0, y0, x1, y1; sel.ordered(x0, y0, x1, y1);
    selFollow.snap(term, y0, y1);
  }
  // after output: 0 = the selection's text is where it was, 1 = it moved and the selection with it, 2 = it is gone
  // and the selection was cleared
  int followSelection() {
    if (!sel.active || sel.empty() || !selFollow.active()) return 0;
    int shift = 0;
    const vt::SelectionFollow::Result r = selFollow.follow(term, shift);
    if (r == vt::SelectionFollow::Moved) { const bool held = holds(); sel.ay += shift; sel.by += shift; if (held) heldSel = sel; }
    else if (r == vt::SelectionFollow::Gone) { sel = Selection(); heldText.clear(); }
    return (int)r;
  }

  // ---- a DRAG on the alternate screen while the application scrolls by redrawing (2026-11-24): THE RULES ARE
  // THE ENGINE'S (core/vt/selection.hpp DragSpill). While the anchor is on screen the selection is as ever; once its
  // row has left the pane, the highlight starts at the junction row and the text is the kept rows + the screen's.
  vt::DragSpill drag;
  std::wstring cutText;                 // a drag that was CUT (a redraw it could not match): the selection as it stood before
  // A selection that reached past the screen, after the release: its text (the kept rows are nowhere else), for as long
  // as `sel` is still that selection (the follower moves both; anything else that changes `sel` ends it).
  std::wstring heldText; Selection heldSel;
  bool holds() const { return !heldText.empty() && sel.active && sel.ax == heldSel.ax && sel.ay == heldSel.ay && sel.bx == heldSel.bx && sel.by == heldSel.by; }
  // the drag's anchor as the highlight shows it: on screen, its own cell; kept above / below, the junction row's start / end
  void dragAnchorToSel() {
    if (drag.where() == 0) { sel.ax = drag.anchorCol(); sel.ay = drag.anchorLine(); }
    else if (drag.where() < 0) { sel.ax = 0; sel.ay = drag.anchorLine(); }
    else { sel.ax = term.cols(); sel.ay = drag.anchorLine(); }
  }

  // The selected text. THE RULES ARE THE TERMINAL ENGINE'S (vt::Terminal::selectionText32, checked range by range
  // against xterm.js in core/test/vt): a wrapped line continues, each row loses its trailing EMPTY cells (a space
  // the app wrote is text, as in xterm), CRLF between rows as xterm joins them on Windows. On the ALTERNATE screen
  // a copy is tidied (vt::copyText32At: an application's padding and its wrap marks are not text; 2026-11-24).
  std::wstring selectionText() const {
    if (holds()) return heldText;                                      // it reached past the screen (above)
    if (drag.active() && drag.where() != 0) return utf16(drag.spilledText(term, sel.bx, sel.by, U"\r\n"));
    if (sel.empty()) return L"";
    int x0, y0, x1, y1; sel.ordered(x0, y0, x1, y1);
    return utf16(vt::copyText32At(term, x0, y0, x1, y1, U"\r\n"));
  }

  static std::wstring utf16(const std::u32string& u) {
    std::wstring w; w.reserve(u.size());
    for (char32_t c : u) {
      if (c < 0x10000) w.push_back((wchar_t)c);
      else { c -= 0x10000; w.push_back((wchar_t)(0xD800 + (c >> 10))); w.push_back((wchar_t)(0xDC00 + (c & 0x3FF))); }
    }
    return w;
  }

 private:
  static bool sameColor(const vt::Color& a, const vt::Color& b) { return a.kind == b.kind && a.value == b.value; }

  void drawRun(Gfx& g, const std::wstring& s, float x, float y, float w, const CellMetrics& m, const Font& f, const Rgba& col) {
    (void)w;
    IDWriteTextLayout* lay = g.runLayout(s, f);         // kept by text + font: a repaint lays nothing out again
    if (!lay) return;
    g.rt->DrawTextLayout(D2D1::Point2F(x, y + m.ascentPad), lay, g.b(col), (D2D1_DRAW_TEXT_OPTIONS)4);
  }
  void decorate(Gfx& g, const vt::Cell& c, float x, float y, float w, const CellMetrics& m, const Theme& th) {
    bool inv = (c.flags & vt::Inverse) != 0;
    Rgba col = Rgba::hex(inv ? rgbOf(c.bg, th, false) : rgbOf(c.fg, th, true));
    if (c.flags & vt::Underline) g.fill(RectF(x, std::floor(y + m.h - 2), w, 1), col);
    if (c.flags & vt::Strike) g.fill(RectF(x, std::floor(y + m.h / 2), w, 1), col);
    if (c.flags & vt::Overline) g.fill(RectF(x, y, w, 1), col);
  }
  // Box drawing and block elements, drawn as geometry so lines join across the 1.1 line height
  // (a font glyph leaves a gap between rows). Returns false for anything it does not draw.
  // BLOCK ELEMENTS (U+2580-259F) are ALWAYS drawn here, whatever customBox says (2026-11-19, the remote:
  // Claude Code's context bar "[bar] 77% context" was hard to read - in Reddit Mono at 10 the font's full block and
  // its shades came out as similar light-grey hatching, where Windows Terminal shows a solid filled part and a dark
  // empty one). As Windows Terminal and xterm's canvas / WebGL renderers (customGlyphs) do: the block fills its part
  // of the cell edge to edge in the cell's colour (dim included), a shade is that colour at 25 / 50 / 75 % over the
  // cell's background, every edge on a whole pixel so neighbours join with no seam. A deliberate difference from the
  // Electron deck's DOM renderer, which uses the font's glyphs. Box-drawing LINES stay the font's unless customBox.
  bool drawBox(Gfx& g, char32_t ch, float x, float y, const CellMetrics& m, const Rgba& col) {
    if (ch < 0x2500 || ch > 0x259F) return false;
    if (ch >= 0x2580) return drawBlock(g, ch, x, y, m, col);
    if (!customBox) return false;
    const float w = m.w, h = m.h, cx = std::floor(x + w / 2), cy = std::floor(y + h / 2);
    auto H = [&](float x0, float x1, float t) { g.fill(RectF(x0, cy - std::floor(t / 2), x1 - x0, t), col); };
    auto V = [&](float y0, float y1, float t) { g.fill(RectF(cx - std::floor(t / 2), y0, t, y1 - y0), col); };
    const float L = 1, B = 2, xr = x + w, yb = y + h;
    // l r u d: 0 none, 1 light, 2 heavy
    struct S { char32_t c; int l, r, u, d; };
    static const S tbl[] = {
      {0x2500,1,1,0,0},{0x2501,2,2,0,0},{0x2502,0,0,1,1},{0x2503,0,0,2,2},
      {0x250C,0,1,0,1},{0x250F,0,2,0,2},{0x2510,1,0,0,1},{0x2513,2,0,0,2},
      {0x2514,0,1,1,0},{0x2517,0,2,2,0},{0x2518,1,0,1,0},{0x251B,2,0,2,0},
      {0x251C,0,1,1,1},{0x2523,0,2,2,2},{0x2524,1,0,1,1},{0x252B,2,0,2,2},
      {0x252C,1,1,0,1},{0x2533,2,2,0,2},{0x2534,1,1,1,0},{0x253B,2,2,2,0},
      {0x253C,1,1,1,1},{0x254B,2,2,2,2},
      {0x256D,0,1,0,1},{0x256E,1,0,0,1},{0x256F,1,0,1,0},{0x2570,0,1,1,0},   // rounded corners, drawn square
      {0x2574,1,0,0,0},{0x2575,0,0,1,0},{0x2576,0,1,0,0},{0x2577,0,0,0,1},
      {0x2578,2,0,0,0},{0x2579,0,0,2,0},{0x257A,0,2,0,0},{0x257B,0,0,0,2},
    };
    for (const S& s : tbl) if (s.c == ch) {
      if (s.l) H(x, cx + 1, s.l == 2 ? B : L);
      if (s.r) H(cx, xr, s.r == 2 ? B : L);
      if (s.u) V(y, cy + 1, s.u == 2 ? B : L);
      if (s.d) V(cy, yb, s.d == 2 ? B : L);
      return true;
    }
    return false;
  }
  // U+2580-259F, every one of them. The cell is split on whole pixels: x0 | xm | x1 across, y0 | ym | y1 down, and
  // an eighth k at round(k * size), so a run of cells (a bar) is one solid strip.
  bool drawBlock(Gfx& g, char32_t ch, float x, float y, const CellMetrics& m, const Rgba& col) {
    const float x0 = std::round(x), x1 = std::round(x + m.w), y0 = std::round(y), y1 = std::round(y + m.h);
    const float w = x1 - x0, h = y1 - y0, xm = std::round(x0 + w / 2), ym = std::round(y0 + h / 2);
    auto box = [&](float a, float b, float c, float d, const Rgba& k) { if (c > a && d > b) g.fill(RectF(a, b, c - a, d - b), k); };
    auto eighth = [](float size, int k) { return std::round(size * k / 8.f); };
    if (ch == 0x2580) { box(x0, y0, x1, ym, col); return true; }                                            // ▀ upper half
    if (ch >= 0x2581 && ch <= 0x2588) { box(x0, y1 - eighth(h, (int)(ch - 0x2580)), x1, y1, col); return true; }   // ▁..█ lower eighths
    if (ch >= 0x2589 && ch <= 0x258F) { box(x0, y0, x0 + eighth(w, (int)(0x2590 - ch)), y1, col); return true; }  // ▉..▏ left eighths
    if (ch == 0x2590) { box(xm, y0, x1, y1, col); return true; }                                            // ▐ right half
    if (ch >= 0x2591 && ch <= 0x2593) { box(x0, y0, x1, y1, col.alpha(0.25f * (float)(ch - 0x2590))); return true; }   // ░ ▒ ▓
    if (ch == 0x2594) { box(x0, y0, x1, y0 + eighth(h, 1), col); return true; }                             // ▔ upper eighth
    if (ch == 0x2595) { box(x1 - eighth(w, 1), y0, x1, y1, col); return true; }                             // ▕ right eighth
    // quadrants ▖▗▘▙▚▛▜▝▞▟: bits 1 upper-left, 2 upper-right, 4 lower-left, 8 lower-right
    static const unsigned char quad[10] = { 4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8 };
    if (ch >= 0x2596 && ch <= 0x259F) {
      const unsigned q = quad[ch - 0x2596];
      if (q & 1) box(x0, y0, xm, ym, col);
      if (q & 2) box(xm, y0, x1, ym, col);
      if (q & 4) box(x0, ym, xm, y1, col);
      if (q & 8) box(xm, ym, x1, y1, col);
      return true;
    }
    return false;
  }
};

}  // namespace deck
