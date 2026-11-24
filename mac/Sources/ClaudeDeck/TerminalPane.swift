// mac/Sources/ClaudeDeck/TerminalPane.swift — one terminal of one window: the core's VT screen (core/vt through the
// bridge) plus what only a VIEW of it has — where it is scrolled to, what is selected, which rows changed since the
// renderer last copied them, its mouse-report state. No drawing here (TerminalRenderer) and no AppKit.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain).
// Windows counterpart: `struct TermView` in windows/src/termview.hpp (the model half) + `struct Selection`.
// JS mirrored: renderer/js/20-terminals.js (createTerm, the xterm.js instance) and xterm's SelectionService.
import Foundation
import DeckCoreBridge

struct TermSelection: Equatable {
  /// anchor and head as (column, absolute line): line 0 = the oldest scrollback line
  var ax = 0, ay = 0, bx = 0, by = 0
  var ordered: (x0: Int, y0: Int, x1: Int, y1: Int) { (ay < by || (ay == by && ax <= bx)) ? (ax, ay, bx, by) : (bx, by, ax, ay) }
  func contains(x: Int, line: Int) -> Bool {
    let o = ordered
    if line < o.y0 || line > o.y1 { return false }
    if line == o.y0 && x < o.x0 { return false }
    if line == o.y1 && x > o.x1 { return false }
    return true
  }
}

final class TerminalPane {
  let id: String
  let term: OpaquePointer                       // DBTerm*
  let mouse: OpaquePointer                      // DBMouse*
  private(set) var state = DBTermState()
  /// lines scrolled up from the bottom (0 = following the output). Only the normal screen has scrollback.
  var scrollOffset = 0 { didSet { if scrollOffset != oldValue { markAllDirty() } } }
  var selection: TermSelection? { didSet { if selection != oldValue { markAllDirty() } } }
  /// viewport rows the renderer must copy again (inclusive); nil = none
  private(set) var dirty: ClosedRange<Int>?
  var markedText = ""                           // an input method's composition, drawn at the cursor
  var exitLine: String?                         // "[process exited with code n]" (20-terminals.js deck.onExit)
  var touched = false                           // a real key was pressed here: `userInput` was fired once
  var cursorOn = true                           // the 600 ms blink's phase
  var title = ""                                // the OSC title (becomes the tile's title on a local deck)
  var bell = false

  init(id: String, cols: Int, rows: Int) {
    self.id = id
    term = db_term_create(Int32(max(2, cols)), Int32(max(1, rows)), Theme.scrollback)
    mouse = db_mouse_create()
    // the host colours xterm answers OSC 4 / 10 / 11 / 12 queries with (20-terminals.js THEME)
    for i in 0..<16 { db_term_set_theme_color(term, Int32(i), Theme.ansi[i]) }
    db_term_set_theme_color(term, 256, Theme.termForeground); db_term_set_theme_color(term, 257, Theme.termBackground); db_term_set_theme_color(term, 258, Theme.termCursor)
    refresh()
    markAllDirty()
  }
  deinit { db_term_destroy(term); db_mouse_destroy(mouse) }

  var cols: Int { Int(state.cols) }
  var rows: Int { Int(state.rows) }
  var scrollback: Int { Int(state.scrollback) }
  /// the absolute line shown on viewport row 0
  var topLine: Int { scrollback - scrollOffset }

  private func refresh() { db_term_state(term, &state) }
  func markAllDirty() { dirty = 0...max(0, rows - 1) }
  func markDirty(_ r: ClosedRange<Int>) { dirty = dirty.map { min($0.lowerBound, r.lowerBound)...max($0.upperBound, r.upperBound) } ?? r }
  func takeDirty() -> ClosedRange<Int>? { let d = dirty; dirty = nil; return d }

  /// Pty bytes (a `data` frame). Returns the bytes the terminal answers with (DSR, DA …): LOCAL mode writes them
  /// back as input; a REMOTE drops them (R8: a remote sends real keystrokes only).
  func feed(_ bytes: UnsafePointer<CChar>, count: Int) -> [UInt8] {
    let before = scrollback
    db_term_feed(term, bytes, count)
    refresh()
    var a: Int32 = 0, b: Int32 = 0
    if db_term_take_damage(term, &a, &b) != 0 {
      if scrollOffset > 0 {
        // scrolled up: keep showing the same lines while output arrives below (xterm keeps the viewport where it is)
        let grew = scrollback - before
        if grew > 0 { scrollOffset = min(scrollback, scrollOffset + grew) }
        markAllDirty()
      } else { markDirty(Int(a)...Int(b)) }
    }
    if db_term_take_bell(term) != 0 { bell = true }
    title = String(cString: db_term_title(term))
    var n = 0
    guard let r = db_term_take_replies(term, &n), n > 0 else { return [] }
    return Array(UnsafeRawBufferPointer(start: r, count: n))
  }
  func resize(cols: Int, rows: Int) {
    if cols == self.cols && rows == self.rows { return }
    db_term_resize(term, Int32(max(2, cols)), Int32(max(1, rows)))
    refresh()
    scrollOffset = min(scrollOffset, scrollback)
    var a: Int32 = 0, b: Int32 = 0; _ = db_term_take_damage(term, &a, &b)
    markAllDirty()
  }
  /// `reset` frame (a wake, a refresh, a move) and a redraw's local reset
  func reset() {
    db_term_reset(term); db_mouse_reset(mouse); refresh()
    scrollOffset = 0; selection = nil; exitLine = nil
    markAllDirty()
  }
  func scroll(lines: Int) { scrollOffset = max(0, min(scrollback, scrollOffset + lines)) }

  /// One viewport row's cells, as the renderer copies them. `buffer` must hold `cols` cells.
  func readRow(_ row: Int, into buffer: UnsafeMutablePointer<DBCell>) -> Int {
    Int(db_term_read_line(term, Int32(topLine + row), buffer, Int32(cols), nil))
  }
  func cluster(row: Int, col: Int) -> [UInt32] {
    var out = [UInt32](repeating: 0, count: 16)
    let n = db_term_cell_cluster(term, Int32(topLine + row), Int32(col), &out, 16)
    return Array(out.prefix(Int(n)))
  }
  var selectedText: String {
    guard let s = selection else { return "" }
    let o = s.ordered; var n = 0
    guard let t = db_term_text(term, Int32(o.x0), Int32(o.y0), Int32(o.x1), Int32(o.y1), &n), n > 0 else { return "" }
    return String(decoding: UnsafeRawBufferPointer(start: t, count: n), as: UTF8.self)
  }
  /// xterm's word selection on a double click: the run of non-blank cells around the column
  func selectWord(col: Int, line: Int) {
    var cells = [DBCell](repeating: DBCell(), count: max(1, cols))
    let n = Int(db_term_read_line(term, Int32(line), &cells, Int32(cells.count), nil))
    guard col < n else { return }
    func blank(_ i: Int) -> Bool { cells[i].ch == 0 || cells[i].ch == 32 }
    if blank(col) { selection = TermSelection(ax: col, ay: line, bx: col, by: line); return }
    var a = col, b = col
    while a > 0 && !blank(a - 1) { a -= 1 }
    while b + 1 < n && !blank(b + 1) { b += 1 }
    selection = TermSelection(ax: a, ay: line, bx: b, by: line)
  }
  func selectLine(_ line: Int) { selection = TermSelection(ax: 0, ay: line, bx: max(0, cols - 1), by: line) }
}

// ---------------------------------------------------------------------------------------------- keys
/// A key press -> the bytes xterm.js would send, through the ONE table both native shells use
/// (DeckCoreBridge db_key_bytes; windows/src/keys.hpp is the same table keyed by VK_ codes).
enum TerminalKeys {
  /// macOS virtual key codes (Carbon's kVK_* — hardware positions, stable since the first Macs)
  static func key(forKeyCode code: UInt16) -> Int32 {
    switch code {
    case 126: return Int32(DB_KEY_UP)
    case 125: return Int32(DB_KEY_DOWN)
    case 124: return Int32(DB_KEY_RIGHT)
    case 123: return Int32(DB_KEY_LEFT)
    case 115: return Int32(DB_KEY_HOME)
    case 119: return Int32(DB_KEY_END)
    case 114: return Int32(DB_KEY_INSERT)       // the Help key's position on an extended keyboard
    case 117: return Int32(DB_KEY_DELETE)       // forward delete
    case 116: return Int32(DB_KEY_PAGE_UP)
    case 121: return Int32(DB_KEY_PAGE_DOWN)
    case 122: return Int32(DB_KEY_F1)
    case 120: return Int32(DB_KEY_F2)
    case 99: return Int32(DB_KEY_F3)
    case 118: return Int32(DB_KEY_F4)
    case 96: return Int32(DB_KEY_F5)
    case 97: return Int32(DB_KEY_F6)
    case 98: return Int32(DB_KEY_F7)
    case 100: return Int32(DB_KEY_F8)
    case 101: return Int32(DB_KEY_F9)
    case 109: return Int32(DB_KEY_F10)
    case 103: return Int32(DB_KEY_F11)
    case 111: return Int32(DB_KEY_F12)
    case 36, 76: return Int32(DB_KEY_ENTER)     // Return, keypad Enter
    case 51: return Int32(DB_KEY_BACKSPACE)     // the key labelled "delete"
    case 48: return Int32(DB_KEY_TAB)
    case 53: return Int32(DB_KEY_ESCAPE)
    case 49: return Int32(DB_KEY_SPACE)
    default: return Int32(DB_KEY_NONE)
    }
  }
  /// nil = the table does not answer this key (let the text system type it)
  static func bytes(key: Int32, mods: Int32, appCursor: Bool) -> [UInt8]? {
    var out = [CChar](repeating: 0, count: 16)
    let n = db_key_bytes(key, mods, appCursor ? 1 : 0, &out, 16)
    return n < 0 ? nil : out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
  }
  /// xterm's paste: line ends to CR, bracketed when the application asked
  static func paste(_ text: String, bracketed: Bool) -> [UInt8] {
    var t = text; var n = 0
    return t.withUTF8 { buf -> [UInt8] in
      buf.withMemoryRebound(to: CChar.self) { c in
        guard let p = db_paste_bytes(c.baseAddress, c.count, bracketed ? 1 : 0, &n), n > 0 else { return [] }
        return Array(UnsafeRawBufferPointer(start: p, count: n))
      }
    }
  }
}
