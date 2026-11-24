// core/input/mouse.hpp — a mouse event → the report xterm.js sends for it. Pure: no window, no terminal.
//
// A port of @xterm/xterm 6.0.0 src/common/services/CoreMouseService.ts (protocols X10 / VT200 / DRAG / ANY =
// DECSET 9 / 1000 / 1002 / 1003; encodings DEFAULT / SGR 1006 / SGR-pixels 1016) and of the parts of
// src/browser/CoreBrowserTerminal.ts bindMouse that decide WHICH events reach it. Checked against the real
// service: core/test/input (mouse-vectors.json written by gen-vectors.js from @xterm/headless, 750 steps).
// Shared by every shell (windows/src/mouse.hpp is a forwarding header for the old path).
//
// LOCAL mode only. A remote desk never sends a mouse or focus report (R8, bridge.js): app.cpp does not call
// this in remote mode, and proto::synthesised still drops anything that looks like one before the wire.
#pragma once
#include <string>
#include <cmath>
#include <cstdio>

namespace deck { namespace mouse {

// common/Types.ts CoreMouseButton / CoreMouseAction
enum Button { LEFT = 0, MIDDLE = 1, RIGHT = 2, NONE = 3, WHEEL = 4 };
enum Action { UP = 0, DOWN = 1, WLEFT = 2, WRIGHT = 3, MOVE = 32 };

struct Event {
  int col = 0, row = 0;         // zero based cell
  int x = 0, y = 0;             // css pixels inside the grid (SGR-pixels reports these as they are)
  int button = NONE, action = MOVE;
  bool ctrl = false, alt = false, shift = false;
};

// eventCode(e, isSGR)
inline int eventCode(const Event& e, bool isSGR) {
  int code = (e.ctrl ? 16 : 0) | (e.shift ? 4 : 0) | (e.alt ? 8 : 0);
  if (e.button == WHEEL) { code |= 64; code |= e.action; }
  else {
    code |= e.button & 3;
    if (e.button & 4) code |= 64;
    if (e.button & 8) code |= 128;
    if (e.action == MOVE) code |= MOVE;
    else if (e.action == UP && !isSGR) code |= NONE;      // only SGR can name the button on release
  }
  return code;
}

// The service's state for one terminal: the last event it saw (a move equal to it is dropped).
struct Encoder {
  bool has = false; Event last;                           // _lastEvent (already 1-based, already restricted)
  void reset() { has = false; }

  // triggerMouseEvent. mode: 0 / 9 / 1000 / 1002 / 1003 (vt::Terminal::mouseMode()); encoding: 0 / 1006 / 1016.
  // Returns true when the event passed the protocol (xterm's return value); `out` is the report, one char per
  // UTF-16 unit xterm would hand to onData / onBinary (so a default-encoding byte over 127 is ONE char here):
  // wire() turns it into what reaches the pty. `out` may be empty with true (default encoding, out of range).
  bool trigger(Event e, int mode, int encoding, int cols, int rows, std::string& out) {
    out.clear();
    if (e.col < 0 || e.col >= cols || e.row < 0 || e.row >= rows) return false;
    if (e.button == WHEEL && e.action == MOVE) return false;
    if (e.button == NONE && e.action != MOVE) return false;
    if (e.button != WHEEL && (e.action == WLEFT || e.action == WRIGHT)) return false;
    e.col++; e.row++;
    const bool pixels = encoding == 1016;
    if (e.action == MOVE && has && equal(last, e, pixels)) return false;
    // the protocol's restrict()
    switch (mode) {
      case 9:    if (e.button == WHEEL || e.action != DOWN) return false; e.ctrl = e.alt = e.shift = false; break;
      case 1000: if (e.action == MOVE) return false; break;
      case 1002: if (e.action == MOVE && e.button == NONE) return false; break;
      case 1003: break;
      default:   return false;                            // NONE
    }
    char b[64];
    if (encoding == 1006 || encoding == 1016) {
      const char fin = (e.action == UP && e.button != WHEEL) ? 'm' : 'M';
      snprintf(b, sizeof b, "\x1b[<%d;%d;%d%c", eventCode(e, true), pixels ? e.x : e.col, pixels ? e.y : e.row, fin);
      out = b;
    } else {
      const int p0 = eventCode(e, false) + 32, p1 = e.col + 32, p2 = e.row + 32;
      if (!(p0 > 255 || p1 > 255 || p2 > 255)) { out = "\x1b[M"; out.push_back((char)p0); out.push_back((char)p1); out.push_back((char)p2); }
    }
    last = e; has = true;
    return true;
  }

  static bool equal(const Event& a, const Event& b, bool pixels) {
    if (pixels) { if (a.x != b.x || a.y != b.y) return false; }
    else { if (a.col != b.col || a.row != b.row) return false; }
    return a.button == b.button && a.action == b.action && a.ctrl == b.ctrl && a.alt == b.alt && a.shift == b.shift;
  }
};

// What reaches the pty. The Electron deck hands the report to node-pty as a JavaScript string
// (20-terminals.js: term.onData / term.onBinary -> deck.input), which writes it as UTF-8: a default-encoding
// byte over 127 goes out as the two bytes of U+0080..U+00FF. The same here.
inline std::string wire(const std::string& units) {
  std::string o; o.reserve(units.size() + 4);
  for (unsigned char c : units) { if (c < 0x80) o.push_back((char)c); else { o.push_back((char)(0xC0 | (c >> 6))); o.push_back((char)(0x80 | (c & 0x3F))); } }
  return o;
}

// Which events a protocol listens for (CoreMouseEventType of DEFAULT_PROTOCOLS; bindMouse binds only these).
inline bool wantsUp(int mode) { return mode == 1000 || mode == 1002 || mode == 1003; }
inline bool wantsWheel(int mode) { return mode == 1000 || mode == 1002 || mode == 1003; }
inline bool wantsDrag(int mode) { return mode == 1002 || mode == 1003; }
inline bool wantsMove(int mode) { return mode == 1003; }

// browser/services/MouseService.ts getMouseReportCoords: the point clamped into the canvas (cols*cellW x
// rows*cellH css px), then the cell under it and the whole css pixel.
inline void reportCoords(float px, float py, float cellW, float cellH, int cols, int rows, Event& e) {
  const float cw = cols * cellW, ch = rows * cellH;
  float cx = px < 0 ? 0 : px > cw - 1 ? cw - 1 : px, cy = py < 0 ? 0 : py > ch - 1 ? ch - 1 : py;
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  e.col = (int)(cx / (cellW > 0 ? cellW : 1)); e.row = (int)(cy / (cellH > 0 ? cellH : 1));
  e.x = (int)cx; e.y = (int)cy;
}

// CoreMouseService.consumeWheelEvent for a pixel-mode wheel (what Chromium gives on Windows: deltaY is 100 css px a
// notch, scrollSensitivity 1): whole lines out of a running sum. A report (or the alt-screen arrow key) is sent
// for an event only when this is not 0 — ONE report per wheel event, whatever the count.
struct WheelSum {
  double partial = 0;
  int consume(double deltaYpx, float cellH, bool shift) {
    if (deltaYpx == 0 || shift || cellH <= 0) return 0;
    double amount = deltaYpx / cellH;
    if (std::abs(deltaYpx) < 50) amount *= 0.3;           // "likely a trackpad"
    partial += amount;
    const double whole = std::floor(std::abs(partial)) * (partial > 0 ? 1 : -1);
    partial = std::fmod(partial, 1.0);
    return (int)whole;
  }
};

// browser/services/SelectionService.ts _getMouseEventScrollAmount: how many lines a 50 ms tick scrolls while a
// selection drag is held above (negative) or below (positive) the grid. offset = css px from the grid's top.
inline int dragScrollAmount(float offset, float gridHeight) {
  if (offset >= 0 && offset <= gridHeight) return 0;
  if (offset > gridHeight) offset -= gridHeight;
  offset = offset < -50 ? -50 : offset > 50 ? 50 : offset;
  offset /= 50;
  return (int)((offset > 0 ? 1 : -1) + std::round(offset * 14));
}
const int kDragScrollIntervalMs = 50;

} }  // namespace deck::mouse
