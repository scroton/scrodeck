// core/vt/selection.hpp — what a SHELL's text selection needs from the terminal engine, once for every shell
// (header-only: no build list grows).
//
//   - its LINES: both shells number a selection's rows the same way (below);
//   - its TEXT: Terminal::selectionText / selectionText32 (xterm's SelectionService rules, checked range by range
//     against xterm.js in core/test/vt); selectionText32At() here takes selection lines;
//   - FOLLOWING its text (2026-11-19, the remote: "text selection works but doesn't move if the session
//     scrolls"): a full-screen application (Claude Code on the alternate screen) scrolls by REDRAWING, so the
//     terminal never learns that the text moved; xterm.js leaves its highlight on the same cells. A follower keeps a
//     snapshot of the rows the selection covers and, after each piece of output, looks for them: where they still
//     are, the selection stays; moved up or down, it moves with them; gone, it is cleared - never a highlight over
//     text that was not selected. (In the normal buffer the rows only move when a full scrollback trims its top.)
//     First written in windows/src/termview.hpp (main 21ce253); moved here unchanged in behaviour.
//
// SELECTION LINES: the normal buffer from its oldest scrollback line (== Terminal::bufferLine numbering), the alt
// buffer from the viewport's top row (the rows xterm keeps hidden above a shrunk alt buffer are not selectable).
// selectionLineOffset() turns a selection line into bufferLine's numbering.
#pragma once
#include "terminal.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace deck { namespace vt {

inline int selectionLines(const Terminal& t) { return t.altScreen() ? t.rows() : t.bufferLines(); }
inline int selectionLineOffset(const Terminal& t) { return t.altScreen() ? t.bufferTop() : 0; }
inline const Line& selectionLine(const Terminal& t, int i) { return t.bufferLine(i + selectionLineOffset(t)); }

// The selected text from (startCol, startLine) up to but not including (endCol, endLine), in selection lines; the
// end line is clamped to the last one (a drag below the grid).
inline std::u32string selectionText32At(const Terminal& t, int startCol, int startLine, int endCol, int endLine, const std::u32string& newline) {
  const int off = selectionLineOffset(t);
  return t.selectionText32(startCol, startLine + off, endCol, std::min(endLine, selectionLines(t) - 1) + off, newline);
}

// COPYING from the ALTERNATE screen (2026-11-24, hand test R2 in the home remote: a copy from Claude Code came
// with every row's padding, and rows ran together - "press Ctrl+V in      my tile.", a bullet glued to the line before).
// A full-screen application lays out its own lines and pads its rows with WRITTEN spaces to the edge; where it writes
// the last column and goes on, the terminal marks the next row wrapped though it is a new line of the application's.
// xterm.js copies those as they are (selectionText32At, kept exactly for the normal buffer). On the alternate screen:
//  - a wrapped row continues the one before only when that row runs to the edge with a non-space AND this row starts
//    with a non-space (a long word or URL cut by the edge); otherwise it is a new line;
//  - each line's trailing spaces are dropped (an all-blank row is an empty line).
struct CopyRow { std::u32string text; bool wrapped = false; };
inline std::u32string tidyCopy(const std::vector<CopyRow>& rows, const std::u32string& nl) {
  std::vector<std::u32string> lines;
  for (size_t k = 0; k < rows.size(); k++) {
    const CopyRow& r = rows[k];
    const bool join = k && r.wrapped && !lines.back().empty() && lines.back().back() != U' ' && !r.text.empty() && r.text.front() != U' ';
    if (join) lines.back() += r.text; else lines.push_back(r.text);
  }
  std::u32string out;
  for (size_t k = 0; k < lines.size(); k++) {
    std::u32string& l = lines[k];
    while (!l.empty() && l.back() == U' ') l.pop_back();
    if (k) out += nl;
    out += l;
  }
  return out;
}
// One selection line's text from c0 up to c1, as copied (xterm's rules for the row; "" when c1 <= c0).
inline std::u32string rowText32(const Terminal& t, int line, int c0, int c1) { return c1 > c0 ? selectionText32At(t, c0, line, c1, line, U"") : std::u32string(); }
inline bool rowWrapped(const Terminal& t, int line) { return line >= 0 && line < selectionLines(t) && selectionLine(t, line).wrapped; }
// What a COPY of the selection gives: the normal buffer exactly as xterm.js (selectionText32At); the alternate screen
// row by row through tidyCopy.
inline std::u32string copyText32At(const Terminal& t, int startCol, int startLine, int endCol, int endLine, const std::u32string& newline) {
  if (!t.altScreen()) return selectionText32At(t, startCol, startLine, endCol, endLine, newline);
  if (startLine > endLine || (startLine == endLine && startCol >= endCol)) return std::u32string();
  endLine = std::min(endLine, selectionLines(t) - 1);
  std::vector<CopyRow> rows;
  for (int y = startLine; y <= endLine; y++) rows.push_back({ rowText32(t, y, y == startLine ? startCol : 0, y == endLine ? endCol : t.cols()), rowWrapped(t, y) });
  return tidyCopy(rows, newline);
}

// One row as the follower compares it: every cell (an empty one is a space; the second half of a wide character
// is skipped), trailing spaces dropped - written or empty alike, so a row redrawn with spaces where it was erased
// still matches. "" outside the selection lines.
inline std::u32string followRowText(const Terminal& t, int line) {
  std::u32string s;
  if (line < 0 || line >= selectionLines(t)) return s;
  const Line& ln = selectionLine(t, line);
  const int n = std::min(t.cols(), (int)ln.cells.size());
  for (int x = 0; x < n; x++) { const Cell& c = ln.cells[(size_t)x]; if (c.width == 0) continue; if (c.ch == 0) s.push_back(U' '); else s += t.cellText(c); }
  while (!s.empty() && s.back() == U' ') s.pop_back();
  return s;
}

class SelectionFollow {
 public:
  enum Result { Stayed = 0, Moved = 1, Gone = 2 };
  static constexpr int kMaxRows = 2000;          // a huge selection is not followed (copy it while it is there)

  // A selection was made or changed (mouse up, a double / triple click, Shift+click): remember the rows
  // [line0, line1] (selection lines, either order) it covers.
  void snap(const Terminal& t, int line0, int line1) {
    rows_.clear();
    if (line1 < line0) std::swap(line0, line1);
    if (line1 - line0 + 1 > kMaxRows) return;
    for (int y = line0; y <= line1; y++) rows_.push_back(followRowText(t, y));
    top_ = line0;
  }
  void clear() { rows_.clear(); }
  bool active() const { return !rows_.empty(); }
  int top() const { return top_; }

  // After output: Stayed (nothing to follow counts as staying), Moved by `shift` lines (add it to the selection's
  // lines), or Gone (clear the selection; the follower is cleared too). Cheap when nothing moved: the first and the
  // last remembered row with text are compared where they were. Else the rows are looked for nearest first, up to
  // the viewport's height away on the alt screen and max(2 x rows, 500) lines in the normal buffer.
  Result follow(const Terminal& t, int& shift) {
    shift = 0;
    if (rows_.empty()) return Stayed;
    const int n = (int)rows_.size();
    int first = -1, last = -1;
    for (int i = 0; i < n; i++) if (!rows_[(size_t)i].empty()) { if (first < 0) first = i; last = i; }
    if (first < 0) { rows_.clear(); return Gone; }                  // nothing but blank rows: nothing to recognise
    if (followRowText(t, top_ + first) == rows_[(size_t)first] && followRowText(t, top_ + last) == rows_[(size_t)last]) return Stayed;
    const int lines = selectionLines(t);
    auto matchAt = [&](int top) {
      if (top < 0 || top + n > lines) return false;
      for (int i = 0; i < n; i++) if (followRowText(t, top + i) != rows_[(size_t)i]) return false;
      return true;
    };
    const int range = t.altScreen() ? t.rows() : std::max(t.rows() * 2, 500);
    for (int d = 1; d <= range; d++) {
      for (int top : { top_ - d, top_ + d }) {
        if (top + first < 0 || top + first >= lines || followRowText(t, top + first) != rows_[(size_t)first]) continue;
        if (!matchAt(top)) continue;
        shift = top - top_; top_ = top;
        return Moved;
      }
    }
    rows_.clear();
    return Gone;
  }

 private:
  std::vector<std::u32string> rows_;   // the covered rows' text when the selection was made
  int top_ = 0;                        // the selection line of rows_[0]
};

// ---- A DRAG on the ALTERNATE screen while the application scrolls (2026-11-24, the home remote: "still can't
// scroll down while selecting text"; the office session's ruling the same night: the deck alone, R8 untouched, and the
// copy EXACT or cut where it cannot be vouched for). A full-screen application (Claude Code) scrolls by redrawing:
// the rows that leave the pane exist nowhere in the terminal any more, so a selection could never reach past the
// screen. While the button is down, a DragSpill watches each piece of output:
//   - it finds how far the rows moved (content up by `s`: now[i] == before[i + s]; the shift most non-empty rows
//     agree on; the moving block is the run of rows that agree, so a footer that stays put is not part of it);
//   - the ANCHOR follows its row; when it leaves the pane through the block's top (bottom), the rows from it to
//     that edge are KEPT (`above` / `below`, exactly as they were drawn), and every row that leaves after it too;
//   - rows that come back are checked against the kept ones, and leave the store; the anchor comes back with its row;
//   - anything it cannot match - the anchor's row rewritten in place, the row next to the kept ones changed, a clear,
//     a resize - CUTS the drag: no more rows are kept, and the shell copies what was selected just before
//     (the text as it stood), saying so, instead of guessing.
// The head is the pointer's, as ever (the shell's). Selection lines here are screen rows (the alt screen).
class DragSpill {
 public:
  enum Result { Same = 0, Moved = 1, Cut = 2 };
  struct Row { std::u32string key; std::u32string text; bool wrapped = false; };

  // the drag began at (col, line) of the alternate screen
  void start(const Terminal& t, int col, int line) {
    clear();
    if (!t.altScreen() || line < 0 || line >= t.rows()) return;
    on_ = true; col_ = std::max(0, std::min(col, t.cols())); a_ = line;
    snap(t, prev_);
    from_ = text(t, line, col_, t.cols()); to_ = text(t, line, 0, col_);
  }
  void clear() { on_ = cut_ = false; where_ = 0; above_.clear(); below_.clear(); prev_.clear(); }
  bool active() const { return on_ && !cut_; }
  bool cut() const { return cut_; }
  int where() const { return where_; }               // 0 the anchor is on screen; -1 it left through the top; +1 the bottom
  int anchorLine() const { return a_; }              // where 0: its row; else the junction (the block's top / bottom row)
  int anchorCol() const { return col_; }
  size_t kept() const { return above_.size() + below_.size(); }

  // after output. Moved: the anchor's row moved, or it left / came back (anchorLine and where say how it stands).
  Result afterFeed(const Terminal& t) {
    if (!active()) return Same;
    if (t.synchronizedOutput()) return Same;           // mid-frame (DEC 2026): judged when the frame is complete, never half drawn
    std::vector<Row> cur; snap(t, cur);
    const int R = (int)cur.size();
    if (!t.altScreen() || (int)prev_.size() != R) return doCut();          // the screen was left, or resized
    bool same = true;
    for (int i = 0; i < R && same; i++) same = cur[(size_t)i].key == prev_[(size_t)i].key;
    if (same) { prev_.swap(cur); return Same; }
    // the shift: the most non-empty rows in agreement, the nearest first on a tie; fewer than 2 = nothing moved
    int s = 0, best = votes(cur, 0);
    for (int d = 1; d < R; d++) for (int c : { d, -d }) { const int n = votes(cur, c); if (n > best) { best = n; s = c; } }
    if (s != 0 && best < 2) s = 0;
    int lo = -1, hi = -1;                                                   // the moving block, in today's rows
    if (s != 0) {
      for (int i = 0; i < R; i++) if (agrees(cur, i, s) && !cur[(size_t)i].key.empty()) { if (lo < 0) lo = i; hi = i; }
      while (lo > 0 && agrees(cur, lo - 1, s)) lo--;
      while (hi + 1 < R && agrees(cur, hi + 1, s)) hi++;
      for (int i = lo; i <= hi; i++) if (!agrees(cur, i, s)) return doCut();   // a block with a hole: not a plain scroll
    }
    Result r = Same;
    if (where_ == 0) r = anchorOnScreen(cur, s, lo, hi);
    else if (where_ < 0) r = anchorAbove(cur, s, lo, hi);
    else r = anchorBelow(cur, s, lo, hi);
    if (r != Cut) prev_.swap(cur);
    return r;
  }

  // The selection's text while the anchor is OFF the screen: the kept rows and the screen's rows between the
  // junction and the head, joined as Terminal::selectionText32 joins rows (a wrapped row continues the one before).
  std::u32string spilledText(const Terminal& t, int headCol, int headLine, const std::u32string& nl) const {
    // row by row, then as a copy from the alternate screen is (tidyCopy: an application's padding and the wrap marks
    // it makes are not taken for text and joins)
    std::vector<CopyRow> rows;
    if (where_ < 0) {
      for (const Row& r : above_) rows.push_back({ r.text, r.wrapped });
      if (headLine >= a_ && !(headLine == a_ && headCol <= 0)) {
        const int last = std::min(headLine, selectionLines(t) - 1);
        for (int y = a_; y <= last; y++) rows.push_back({ rowText32(t, y, 0, y == headLine ? headCol : t.cols()), rowWrapped(t, y) });
      }
    } else if (where_ > 0) {
      if (headLine <= a_ && !(headLine == a_ && headCol >= t.cols()))
        for (int y = std::max(headLine, 0); y <= a_; y++) rows.push_back({ rowText32(t, y, y == headLine ? headCol : 0, t.cols()), rowWrapped(t, y) });
      for (const Row& r : below_) rows.push_back({ r.text, r.wrapped });
    }
    return tidyCopy(rows, nl);
  }

 private:
  bool on_ = false, cut_ = false;
  int where_ = 0, a_ = 0, col_ = 0;
  std::u32string from_, to_;           // the anchor's row from the anchor on / up to it (as drawn when the drag began)
  std::vector<Row> prev_;              // the screen after the last piece of output
  std::vector<Row> above_;             // rows that left through the top, in reading order; [0] is the anchor's row
  std::vector<Row> below_;             // rows that left through the bottom, in reading order; the last is the anchor's

  static std::u32string text(const Terminal& t, int line, int c0, int c1) { return c1 > c0 ? selectionText32At(t, c0, line, c1, line, U"") : std::u32string(); }
  static void snap(const Terminal& t, std::vector<Row>& v) {
    v.assign((size_t)t.rows(), Row());
    for (int i = 0; i < t.rows(); i++) { Row& r = v[(size_t)i]; r.key = followRowText(t, i); r.text = text(t, i, 0, t.cols()); r.wrapped = selectionLine(t, i).wrapped; }
  }
  bool agrees(const std::vector<Row>& cur, int i, int s) const { const int j = i + s; return i >= 0 && i < (int)cur.size() && j >= 0 && j < (int)prev_.size() && cur[(size_t)i].key == prev_[(size_t)j].key; }
  int votes(const std::vector<Row>& cur, int s) const { int n = 0; for (int i = 0; i < (int)cur.size(); i++) if (!cur[(size_t)i].key.empty() && agrees(cur, i, s)) n++; return n; }
  Result doCut() { cut_ = true; above_.clear(); below_.clear(); return Cut; }
  Row anchorRow(bool upTo) const { Row r = prev_[(size_t)a_]; r.text = upTo ? to_ : from_; return r; }

  Result anchorOnScreen(const std::vector<Row>& cur, int s, int lo, int hi) {
    if (s == 0) return cur[(size_t)a_].key == prev_[(size_t)a_].key ? Same : doCut();
    if (a_ >= lo + s && a_ <= hi + s) { a_ -= s; return Moved; }          // within the block: it moved with it
    if (s > 0 && a_ >= lo && a_ < lo + s) {                                 // it left through the block's top
      above_.push_back(anchorRow(false));
      for (int i = a_ + 1; i < lo + s; i++) above_.push_back(prev_[(size_t)i]);
      where_ = -1; a_ = lo; return Moved;
    }
    if (s < 0 && a_ > hi + s && a_ <= hi) {                                 // it left through the block's bottom
      for (int i = hi + s + 1; i < a_; i++) below_.push_back(prev_[(size_t)i]);
      below_.push_back(anchorRow(true));
      where_ = 1; a_ = hi; return Moved;
    }
    return cur[(size_t)a_].key == prev_[(size_t)a_].key ? Same : doCut();  // outside the block: it must not have changed
  }
  Result anchorAbove(const std::vector<Row>& cur, int s, int lo, int hi) {
    const int jt = a_, R = (int)cur.size();
    if (s == 0) return cur[(size_t)jt].key == prev_[(size_t)jt].key ? Same : doCut();
    if (s > 0) {                                                            // more rows leave through the top
      if (!(jt >= lo && jt <= hi)) return doCut();
      for (int i = jt; i < jt + s; i++) above_.push_back(prev_[(size_t)i]);
      return Moved;
    }
    const int d = -s, n = (int)above_.size();                              // rows come back from the top
    if (jt + d >= R || !(jt + d >= lo && jt + d <= hi)) return doCut();
    const int back = std::min(d, n);
    for (int k = 0; k < back; k++) if (cur[(size_t)(jt + d - back + k)].key != above_[(size_t)(n - back + k)].key) return doCut();
    above_.resize((size_t)(n - back));
    if (above_.empty()) { where_ = 0; a_ = jt + d - n; }                    // the anchor's row is back on screen
    return Moved;
  }
  Result anchorBelow(const std::vector<Row>& cur, int s, int lo, int hi) {
    const int jb = a_;
    if (s == 0) return cur[(size_t)jb].key == prev_[(size_t)jb].key ? Same : doCut();
    if (s < 0) {                                                            // more rows leave through the bottom
      if (!(jb >= lo && jb <= hi)) return doCut();
      std::vector<Row> left; for (int i = jb + s + 1; i <= jb; i++) left.push_back(prev_[(size_t)i]);
      below_.insert(below_.begin(), left.begin(), left.end());
      return Moved;
    }
    const int n = (int)below_.size();                                      // rows come back from the bottom
    if (jb - s < 0 || !(jb - s >= lo && jb - s <= hi)) return doCut();
    const int back = std::min(s, n);
    for (int k = 0; k < back; k++) if (cur[(size_t)(jb - s + 1 + k)].key != below_[(size_t)k].key) return doCut();
    below_.erase(below_.begin(), below_.begin() + back);
    if (below_.empty()) { where_ = 0; a_ = jb - s + n; }
    return Moved;
  }
};

}}  // namespace deck::vt
