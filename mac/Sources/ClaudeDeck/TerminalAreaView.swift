// mac/Sources/ClaudeDeck/TerminalAreaView.swift — the terminal area of one window (#main in the Electron deck): an
// MTKView that draws every placed pane (TerminalRenderer), an AppKit overlay for what is chrome (the panes' name
// bands, borders, the drop preview, an input method's composition), and ALL of the terminal's input: keys through
// NSTextInputClient (IME, dead keys), Option as Meta, the mouse (selection, or reports to the application in local
// mode), the wheel, copy / paste, and tiles dropped from the sidebar.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain). VERIFY marks what to check on the first build.
// Windows counterpart: the WM_KEYDOWN / WM_CHAR / WM_MOUSE* / WM_IME_* handlers and `layoutPanes` in
// windows/src/app.cpp, the pane chrome in windows/src/termview.hpp, windows/src/mouse.hpp.
// JS mirrored: renderer/js/20-terminals.js (attachCustomKeyEventHandler, contextmenu, fitOne, paintPaneName),
// renderer/js/25-layout.js (applyLayout, zoneAt), xterm.js's own keyboard / mouse / selection services.
import AppKit
import MetalKit
import DeckCoreBridge

let kTilePasteboardType = NSPasteboard.PasteboardType("app.claude-deck.native.tile")

protocol TerminalAreaDelegate: AnyObject {
  var areaIsLocal: Bool { get }
  func areaTile(_ id: String) -> Tile?
  func areaSelected() -> String?
  /// One of the window's own keys (Cmd+B, Cmd+Shift+P, a chord digit …): true = consumed, the terminal never sees it
  func areaWindowKey(_ event: NSEvent) -> Bool
  func areaKeyUp(_ event: NSEvent)                             // the chord places when its last key comes up
  /// Bytes for the tile's pty. `realKey`: the user pressed a key (fires `userInput`, wakes nothing synthesised)
  func areaSend(_ id: String, _ bytes: [UInt8], realKey: Bool)
  func areaWheelReport(_ id: String, _ report: [UInt8])
  func areaSelect(_ id: String)
  func areaPaneName(_ id: String, doubleClick: Bool)
  func areaResized()
  func areaDrop(tileId: String, cells: [Int])
  func areaContextMenu(for id: String, event: NSEvent) -> NSMenu?
  func areaNote(_ text: String)
}

struct PaneFrame { let pane: TerminalPane; let frame: CGRect; let title: String; let abbr: String; let busy: Bool; let attention: Bool }

final class TerminalAreaView: NSView, NSTextInputClient {
  weak var delegate: TerminalAreaDelegate?
  let metal = MTKView()
  private(set) var renderer: TerminalRenderer?
  private let chrome = PaneChromeView()
  private var frames: [PaneFrame] = []
  private var placements: [String: PanePlacement] = [:]
  private var blink: Timer?
  private var dragSelecting = false, clickCount = 0
  var optionIsMeta = true                                   // 20-terminals.js: macOptionIsMeta (+ Option-click forces selection)
  var layoutModel: GridLayout?                              // for the drop preview's hit zones

  override var isFlipped: Bool { true }                     // y grows down, like the layout's fractions and the grid
  override var acceptsFirstResponder: Bool { true }

  override init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true
    metal.frame = bounds; metal.autoresizingMask = [.width, .height]
    addSubview(metal)
    chrome.frame = bounds; chrome.autoresizingMask = [.width, .height]
    addSubview(chrome)                                      // above the Metal layer: AppKit composites it
    renderer = TerminalRenderer(view: metal)
    registerForDraggedTypes([kTilePasteboardType])
    // xterm's cursorBlink: 600 ms on, 600 ms off
    blink = Timer.scheduledTimer(withTimeInterval: 0.6, repeats: true) { [weak self] _ in self?.blinkTick() }
  }
  required init?(coder: NSCoder) { fatalError("not used") }
  deinit { blink?.invalidate() }

  var scale: CGFloat { window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2 }

  // ------------------------------------------------------------------ geometry
  func configureFont(family: String, size: CGFloat) {
    renderer?.atlas.configure(fontFamily: family, size: size, scale: scale)
  }
  /// cols x rows that fit a pane's frame (points): the frame less the name band and the pane's padding, divided by
  /// the cell (renderer/js/20-terminals.js fitOne; xterm's fit addon)
  func fit(_ frame: CGRect) -> (cols: Int, rows: Int) {
    guard let r = renderer else { return (80, 24) }
    let s = scale
    let w = (frame.width - Theme.panePadLeft - Theme.panePadRight) * s, h = (frame.height - Theme.paneNameBand - Theme.panePadBottom) * s
    return (max(2, Int(w / r.cellSize.width)), max(1, Int(h / r.cellSize.height)))
  }
  /// The window decided what is on screen: these panes at these frames (points, this view's coordinates).
  func setPanes(_ f: [PaneFrame]) {
    frames = f
    var list: [PanePlacement] = []
    var keep: [String: PanePlacement] = [:]
    let s = scale, selected = delegate?.areaSelected()
    for pf in f {
      let pl = placements[pf.pane.id] ?? PanePlacement(pane: pf.pane)
      pl.gridOrigin = CGPoint(x: ((pf.frame.minX + Theme.panePadLeft) * s).rounded(), y: ((pf.frame.minY + Theme.paneNameBand) * s).rounded())
      pl.focused = pf.pane.id == selected && window?.firstResponder === self && window?.isKeyWindow == true
      keep[pf.pane.id] = pl; list.append(pl)
    }
    placements = keep
    renderer?.setPlacements(list)
    chrome.frames = f; chrome.selected = selected
    chrome.needsDisplay = true
    refresh()
  }
  /// After a feed, a scroll, a selection: copy what changed and ask for a frame if anything did.
  func refresh() {
    if renderer?.update() == true { metal.needsDisplay = true }
  }
  func paneChanged(_ id: String) { refresh() }
  private func blinkTick() {
    guard let sel = delegate?.areaSelected(), let pl = placements[sel] else { return }
    pl.pane.cursorOn.toggle()
    refresh()
  }
  private func restartBlink(_ pane: TerminalPane) { pane.cursorOn = true }

  override func setFrameSize(_ newSize: NSSize) { super.setFrameSize(newSize); delegate?.areaResized() }
  override func viewDidChangeBackingProperties() {
    super.viewDidChangeBackingProperties()
    // moved to a display with another scale: the atlas is per device pixel
    if let a = renderer?.atlas { a.configure(fontFamily: a.fontName, size: a.fontSize, scale: scale) }
    delegate?.areaResized()
  }
  override func becomeFirstResponder() -> Bool { focusChanged(true); return true }
  override func resignFirstResponder() -> Bool { focusChanged(false); return true }
  func focusChanged(_ on: Bool) {
    let sel = delegate?.areaSelected()
    for (id, pl) in placements { pl.focused = on && id == sel }
    // xterm's focus reports (DECSET 1004) are the terminal's own bytes: LOCAL mode only (R8)
    if delegate?.areaIsLocal == true, let id = sel, let pl = placements[id], pl.pane.state.focus_reporting != 0 {
      delegate?.areaSend(id, Array((on ? "\u{1b}[I" : "\u{1b}[O").utf8), realKey: false)
    }
    refresh()
  }

  private func frame(at p: CGPoint) -> PaneFrame? { frames.first { $0.frame.contains(p) } }
  /// The cell under a point of this view, clamped into the pane's grid; and the point inside the grid in device pixels
  private func cell(_ p: CGPoint, in pf: PaneFrame) -> (col: Int, row: Int, px: CGFloat, py: CGFloat) {
    guard let r = renderer else { return (0, 0, 0, 0) }
    let s = scale
    let px = (p.x - pf.frame.minX - Theme.panePadLeft) * s, py = (p.y - pf.frame.minY - Theme.paneNameBand) * s
    let col = max(0, min(pf.pane.cols - 1, Int(px / r.cellSize.width))), row = max(0, min(pf.pane.rows - 1, Int(py / r.cellSize.height)))
    return (col, row, px, py)
  }

  // ------------------------------------------------------------------ keys
  private func mods(_ e: NSEvent) -> Int32 {
    var m: Int32 = 0
    if e.modifierFlags.contains(.shift) { m |= DB_MOD_SHIFT }
    if e.modifierFlags.contains(.option) { m |= DB_MOD_ALT }
    if e.modifierFlags.contains(.control) { m |= DB_MOD_CTRL }
    if e.modifierFlags.contains(.command) { m |= DB_MOD_META }
    return m
  }

  override func keyDown(with event: NSEvent) {
    // 1. the window's own keys (lib/platform.js keyAction's Mac branch, the chords, the walk) come first
    if delegate?.areaWindowKey(event) == true { return }
    guard let id = delegate?.areaSelected(), let pl = placements[id] else { return }
    let pane = pl.pane, m = mods(event)
    if m & DB_MOD_META != 0 { super.keyDown(with: event); return }      // Cmd is never the terminal's: the menu bar's
    restartBlink(pane)
    // 2. a composition in progress owns every key (Return commits, Esc cancels, arrows move in the candidates)
    if hasMarkedText() { interpretKeyEvents([event]); return }
    // 3. the keys that are not text: one table for both shells
    let key = TerminalKeys.key(forKeyCode: event.keyCode)
    if key != DB_KEY_NONE, let bytes = TerminalKeys.bytes(key: key, mods: m, appCursor: pane.state.app_cursor_keys != 0) {
      send(id, bytes, key: true); return
    }
    // 4. Ctrl+letter: the control character itself (AppKit already computed it: Ctrl+C is "\u{03}"), ESC first with Option
    if m & DB_MOD_CTRL != 0, let chars = event.characters, !chars.isEmpty {
      var bytes = Array(chars.utf8)
      if m & DB_MOD_ALT != 0 { bytes.insert(0x1b, at: 0) }
      send(id, bytes, key: true); return
    }
    // 5. Option as Meta: ESC + the key WITHOUT Option's own character (Option+B is ESC b, not "∫")
    if optionIsMeta, m & DB_MOD_ALT != 0, let chars = event.charactersIgnoringModifiers, !chars.isEmpty {
      send(id, [0x1b] + Array(chars.utf8), key: true); return
    }
    // 6. text: through the input system, so dead keys, the press-and-hold accent menu and input methods work
    interpretKeyEvents([event])
  }
  override func keyUp(with event: NSEvent) { delegate?.areaKeyUp(event) }
  private func send(_ id: String, _ bytes: [UInt8], key: Bool) {
    guard !bytes.isEmpty else { return }
    if let pl = placements[id] {
      if pl.pane.scrollOffset != 0 { pl.pane.scrollOffset = 0 }       // typing snaps to the bottom (xterm scrollOnUserInput)
      if pl.pane.selection != nil { pl.pane.selection = nil }
    }
    delegate?.areaSend(id, bytes, realKey: key)
    refresh()
  }

  // ---- NSTextInputClient (what makes this view a text input: IME, dead keys, marked text)
  private var marked = NSMutableAttributedString()
  private var markedSel = NSRange(location: 0, length: 0)
  func insertText(_ string: Any, replacementRange: NSRange) {
    let text = (string as? NSAttributedString)?.string ?? (string as? String) ?? ""
    marked = NSMutableAttributedString(); setComposition("")
    if let id = delegate?.areaSelected() { send(id, Array(text.utf8), key: true) }
  }
  func setMarkedText(_ string: Any, selectedRange: NSRange, replacementRange: NSRange) {
    let text = (string as? NSAttributedString)?.string ?? (string as? String) ?? ""
    marked = NSMutableAttributedString(string: text); markedSel = selectedRange
    setComposition(text)
  }
  func unmarkText() { marked = NSMutableAttributedString(); setComposition("") }
  func selectedRange() -> NSRange { NSRange(location: NSNotFound, length: 0) }
  func markedRange() -> NSRange { marked.length > 0 ? NSRange(location: 0, length: marked.length) : NSRange(location: NSNotFound, length: 0) }
  func hasMarkedText() -> Bool { marked.length > 0 }
  func attributedSubstring(forProposedRange range: NSRange, actualRange: NSRangePointer?) -> NSAttributedString? { nil }
  func validAttributesForMarkedText() -> [NSAttributedString.Key] { [] }
  /// Where the candidate window goes: the cursor's cell, in screen coordinates
  func firstRect(forCharacterRange range: NSRange, actualRange: NSRangePointer?) -> NSRect {
    guard let id = delegate?.areaSelected(), let pf = frames.first(where: { $0.pane.id == id }), let r = renderer, let w = window else { return .zero }
    let s = scale, st = pf.pane.state
    let x = pf.frame.minX + Theme.panePadLeft + CGFloat(st.cursor_x) * r.cellSize.width / s
    let y = pf.frame.minY + Theme.paneNameBand + CGFloat(st.cursor_y) * r.cellSize.height / s
    let inWindow = convert(NSRect(x: x, y: y, width: r.cellSize.width / s, height: r.cellSize.height / s), to: nil)
    return w.convertToScreen(inWindow)
  }
  func characterIndex(for point: NSPoint) -> Int { NSNotFound }
  /// Keys the input system turned into commands (moveLeft:, insertNewline: …) were all answered in keyDown already;
  /// what reaches here has no terminal meaning. Not calling super keeps AppKit from beeping.
  override func doCommand(by selector: Selector) {}
  private func setComposition(_ text: String) {
    guard let id = delegate?.areaSelected(), let pf = frames.first(where: { $0.pane.id == id }), let r = renderer else { return }
    pf.pane.markedText = text
    let s = scale, st = pf.pane.state
    chrome.composition = text
    chrome.compositionOrigin = CGPoint(x: pf.frame.minX + Theme.panePadLeft + CGFloat(st.cursor_x) * r.cellSize.width / s, y: pf.frame.minY + Theme.paneNameBand + CGFloat(st.cursor_y) * r.cellSize.height / s)
    chrome.compositionFont = NSFont(name: r.atlas.fontName, size: r.atlas.fontSize) ?? NSFont.monospacedSystemFont(ofSize: r.atlas.fontSize, weight: .regular)
    chrome.cellHeight = r.cellSize.height / s
    chrome.needsDisplay = true
  }

  // ---- the Edit menu's actions (Cmd+C / Cmd+V / Cmd+A arrive here through the responder chain)
  @objc func copy(_ sender: Any?) {
    guard let id = delegate?.areaSelected(), let pl = placements[id] else { return }
    let text = pl.pane.selectedText
    if text.isEmpty { return }
    NSPasteboard.general.clearContents(); NSPasteboard.general.setString(text, forType: .string)
  }
  @objc func paste(_ sender: Any?) {
    guard let id = delegate?.areaSelected(), let pl = placements[id] else { return }
    if let text = NSPasteboard.general.string(forType: .string), !text.isEmpty {
      send(id, TerminalKeys.paste(text, bracketed: pl.pane.state.bracketed_paste != 0), key: true)
    } else if delegate?.areaIsLocal == true {
      // no text on the clipboard: send the key itself (0x16) so Claude Code reads the clipboard (an image) on its own
      send(id, [0x16], key: true)
    } else {
      // a REMOTE never sends 0x16: the session would read the DECK's machine's clipboard, not this one's (windows/PARITY.md)
      delegate?.areaNote("Nothing to paste: the clipboard holds no text.")
    }
  }
  @objc override func selectAll(_ sender: Any?) {
    guard let id = delegate?.areaSelected(), let pl = placements[id] else { return }
    let p = pl.pane
    p.selection = TermSelection(ax: 0, ay: 0, bx: max(0, p.cols - 1), by: p.scrollback + p.rows - 1)
    refresh()
  }
  // VERIFY: NSView does not declare copy(_:) / paste(_:), so these are plain @objc methods found by the responder
  // chain (no `override`); selectAll(_:) IS declared on NSResponder (hence `override`).

  // ------------------------------------------------------------------ mouse
  private func appTracksMouse(_ pane: TerminalPane, _ e: NSEvent) -> Bool {
    // LOCAL only (a remote never sends a mouse report: R8); Option forces a selection (macOptionClickForcesSelection)
    delegate?.areaIsLocal == true && pane.state.mouse_mode != 0 && !e.modifierFlags.contains(.option)
  }
  private func report(_ pf: PaneFrame, _ e: NSEvent, button: Int32, action: Int32) {
    let c = cell(convert(e.locationInWindow, from: nil), in: pf), pane = pf.pane
    var ev = DBMouseEvent(col: Int32(c.col), row: Int32(c.row), x: Int32(max(0, c.px)), y: Int32(max(0, c.py)), button: button, action: action,
                          ctrl: e.modifierFlags.contains(.control) ? 1 : 0, alt: 0, shift: e.modifierFlags.contains(.shift) ? 1 : 0)
    var out = [CChar](repeating: 0, count: 64)
    let n = db_mouse_report(pane.mouse, &ev, pane.state.mouse_mode, pane.state.mouse_encoding, Int32(pane.cols), Int32(pane.rows), &out, 64)
    if n > 0 { delegate?.areaSend(pane.id, out.prefix(Int(n)).map { UInt8(bitPattern: $0) }, realKey: false) }
  }

  override func mouseDown(with event: NSEvent) {
    window?.makeFirstResponder(self)
    let p = convert(event.locationInWindow, from: nil)
    guard let pf = frame(at: p) else { return }
    delegate?.areaSelect(pf.pane.id)
    if p.y < pf.frame.minY + Theme.paneNameBand {            // the name band: click = redraw, double-click = zoom
      delegate?.areaPaneName(pf.pane.id, doubleClick: event.clickCount >= 2); return
    }
    let pane = pf.pane
    if appTracksMouse(pane, event) { report(pf, event, button: DB_BTN_LEFT, action: DB_ACT_DOWN); return }
    let c = cell(p, in: pf), line = pane.topLine + c.row
    clickCount = event.clickCount
    if event.clickCount == 2 { pane.selectWord(col: c.col, line: line) }
    else if event.clickCount >= 3 { pane.selectLine(line) }
    else if event.modifierFlags.contains(.shift), var s = pane.selection { s.bx = c.col; s.by = line; pane.selection = s }
    else { pane.selection = nil; dragSelecting = true; dragAnchor = (c.col, line) }
    refresh()
  }
  private var dragAnchor = (col: 0, line: 0)
  override func mouseDragged(with event: NSEvent) {
    let p = convert(event.locationInWindow, from: nil)
    guard let id = delegate?.areaSelected(), let pf = frames.first(where: { $0.pane.id == id }) else { return }
    let pane = pf.pane
    if appTracksMouse(pane, event) { if pane.state.mouse_mode >= 1002 { report(pf, event, button: DB_BTN_LEFT, action: DB_ACT_MOVE) }; return }
    guard dragSelecting else { return }
    let c = cell(p, in: pf)
    // dragging above / below the grid scrolls the scrollback (xterm's drag scroll; the 50 ms repeat is not in this draft)
    if p.y < pf.frame.minY + Theme.paneNameBand { pane.scroll(lines: 1) } else if p.y > pf.frame.maxY - Theme.panePadBottom { pane.scroll(lines: -1) }
    pane.selection = TermSelection(ax: dragAnchor.col, ay: dragAnchor.line, bx: c.col, by: pane.topLine + c.row)
    refresh()
  }
  override func mouseUp(with event: NSEvent) {
    dragSelecting = false
    guard let id = delegate?.areaSelected(), let pf = frames.first(where: { $0.pane.id == id }) else { return }
    if appTracksMouse(pf.pane, event) { report(pf, event, button: DB_BTN_LEFT, action: DB_ACT_UP) }
  }
  /// Right click (20-terminals.js contextmenu): copy the selection, else paste — unless the application tracks the
  /// mouse (it gets the click and pastes by itself). A REMOTE always pastes (it sends no click report).
  override func rightMouseDown(with event: NSEvent) {
    let p = convert(event.locationInWindow, from: nil)
    guard let pf = frame(at: p) else { return }
    delegate?.areaSelect(pf.pane.id)
    if p.y < pf.frame.minY + Theme.paneNameBand {
      if let menu = delegate?.areaContextMenu(for: pf.pane.id, event: event) { NSMenu.popUpContextMenu(menu, with: event, for: self) }
      return
    }
    let pane = pf.pane
    if pane.selection != nil { copy(nil); pane.selection = nil; refresh(); return }
    if appTracksMouse(pane, event) { report(pf, event, button: DB_BTN_RIGHT, action: DB_ACT_DOWN); return }
    paste(nil)
  }
  override func rightMouseUp(with event: NSEvent) {
    guard let pf = frame(at: convert(event.locationInWindow, from: nil)) else { return }
    if appTracksMouse(pf.pane, event) { report(pf, event, button: DB_BTN_RIGHT, action: DB_ACT_UP) }
  }

  override func scrollWheel(with event: NSEvent) {
    let p = convert(event.locationInWindow, from: nil)
    guard let pf = frame(at: p), let r = renderer else { return }
    let pane = pf.pane, st = pane.state, cellH = r.cellSize.height / scale
    // AppKit: positive deltaY = content moves down = scrolling UP. xterm's deltaY is the opposite sign, in pixels.
    let dyPx = event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY : -event.scrollingDeltaY * cellH * 3     // a wheel notch: three lines
    let lines = Int(db_mouse_wheel_lines(pane.mouse, Double(dyPx), Double(cellH), event.modifierFlags.contains(.shift) ? 1 : 0))
    if lines == 0 { return }
    let tile = delegate?.areaTile(pane.id)
    let local = delegate?.areaIsLocal == true
    let tracks = st.mouse_mode == 1000 || st.mouse_mode == 1002 || st.mouse_mode == 1003
    if tracks && (local || (tile?.mouseOn == true && tile?.sgrMouse == true && st.mouse_encoding == 1006 && tile?.asleep != true)) {
      // ONE report per wheel event, whatever the count (CoreMouseService). A remote sends exactly this and nothing
      // else synthesised (R8 narrowed for the wheel), through the codec that refuses any other shape.
      let c = cell(p, in: pf)
      var ev = DBMouseEvent(col: Int32(c.col), row: Int32(c.row), x: Int32(max(0, c.px)), y: Int32(max(0, c.py)), button: DB_BTN_WHEEL,
                            action: lines < 0 ? DB_ACT_UP : DB_ACT_DOWN, ctrl: event.modifierFlags.contains(.control) ? 1 : 0, alt: 0, shift: 0)
      var out = [CChar](repeating: 0, count: 64)
      let n = db_mouse_report(pane.mouse, &ev, st.mouse_mode, st.mouse_encoding, Int32(pane.cols), Int32(pane.rows), &out, 64)
      if n > 0 {
        let bytes = out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
        if local { delegate?.areaSend(pane.id, bytes, realKey: false) } else { delegate?.areaWheelReport(pane.id, bytes) }
      }
      return
    }
    if st.alt_screen != 0 {
      // the alternate screen has no scrollback: an arrow key a notch (xterm), which IS a key and may go from a remote
      if let b = TerminalKeys.bytes(key: lines < 0 ? DB_KEY_UP : DB_KEY_DOWN, mods: 0, appCursor: st.app_cursor_keys != 0) { delegate?.areaSend(pane.id, b, realKey: true) }
      return
    }
    pane.scroll(lines: -lines)                                 // this pane's own scrollback
    refresh()
  }

  // ------------------------------------------------------------------ a tile dragged in from the sidebar (F4)
  private func dropCells(_ sender: NSDraggingInfo) -> (id: String, cells: [Int])? {
    guard let id = sender.draggingPasteboard.string(forType: kTilePasteboardType), let l = layoutModel, bounds.width > 0, bounds.height > 0 else { return nil }
    let p = convert(sender.draggingLocation, from: nil)
    let cells = l.dropTarget(px: Double(p.x / bounds.width), py: Double(p.y / bounds.height), moving: id)
    return cells.isEmpty ? nil : (id, cells)
  }
  override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { draggingUpdated(sender) }
  override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
    guard let d = dropCells(sender), let r = layoutModel?.rect(ofCells: d.cells) else { chrome.dropPreview = nil; chrome.needsDisplay = true; return [] }
    chrome.dropPreview = CGRect(x: r.minX * bounds.width, y: r.minY * bounds.height, width: r.width * bounds.width, height: r.height * bounds.height)
    chrome.needsDisplay = true
    return .move
  }
  override func draggingExited(_ sender: NSDraggingInfo?) { chrome.dropPreview = nil; chrome.needsDisplay = true }
  override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
    chrome.dropPreview = nil; chrome.needsDisplay = true
    guard let d = dropCells(sender) else { return false }
    delegate?.areaDrop(tileId: d.id, cells: d.cells)
    return true
  }
  // (NSView itself adopts NSDraggingDestination, so these four are overrides and the class names no conformance.)

  func setNote(_ text: String?) { chrome.note = text; chrome.needsDisplay = true }
  func setLeaving(_ ids: Set<String>) { chrome.leaving = ids; chrome.needsDisplay = true }
}

/// The panes' chrome, drawn by AppKit over the Metal layer: the name band (abbreviation — green while busy —, the
/// title, the selected pane's border, the attention frame), the drop preview, a note, an input method's composition.
/// Numbers from renderer/style.css (.term, .pane-name). It never takes the mouse (hitTest is nil).
final class PaneChromeView: NSView {
  var frames: [PaneFrame] = []
  var selected: String?
  var dropPreview: CGRect?
  var note: String?
  var leaving: Set<String> = []
  var composition = "", compositionOrigin = CGPoint.zero, compositionFont = NSFont.monospacedSystemFont(ofSize: 14, weight: .regular), cellHeight: CGFloat = 17
  override var isFlipped: Bool { true }
  override func hitTest(_ point: NSPoint) -> NSView? { nil }

  override func draw(_ dirtyRect: NSRect) {
    for pf in frames {
      let f = pf.frame
      // cell lines between panes, and the selected pane's border (25-layout.js applyLayout)
      (pf.pane.id == selected && frames.count > 1 ? Theme.tileActiveBorder : Theme.tileBorder).setStroke()
      let border = NSBezierPath(rect: f.insetBy(dx: 0.5, dy: 0.5)); border.lineWidth = 1; border.stroke()
      if pf.attention { Theme.attention.setStroke(); let a = NSBezierPath(rect: f.insetBy(dx: 1, dy: 1)); a.lineWidth = 2; a.stroke() }
      if leaving.contains(pf.pane.id) {                      // the walk's leaving mark: a dashed amber frame
        Theme.attention.setStroke(); let d = NSBezierPath(rect: f.insetBy(dx: 2, dy: 2)); d.lineWidth = 2; d.setLineDash([6, 4], count: 2, phase: 0); d.stroke()
      }
      // the name band
      var x = f.minX + Theme.panePadLeft
      let y = f.minY + 3
      let nameFont = NSFont.systemFont(ofSize: Theme.paneNameSize)
      if !pf.abbr.isEmpty {
        let a = NSAttributedString(string: pf.abbr, attributes: [.font: NSFont.systemFont(ofSize: Theme.paneNameSize, weight: .semibold), .foregroundColor: pf.busy ? Theme.green : Theme.fgDim])
        a.draw(at: CGPoint(x: x, y: y)); x += a.size().width + 6
      }
      let t = NSAttributedString(string: pf.title, attributes: [.font: nameFont, .foregroundColor: Theme.fgDim])
      t.draw(with: CGRect(x: x, y: y, width: max(0, f.maxX - x - Theme.panePadRight), height: Theme.paneNameSize * 1.3), options: [.truncatesLastVisibleLine, .usesLineFragmentOrigin])
    }
    if let d = dropPreview {                                  // F4: the preview frame, the rest shaded
      NSColor.black.withAlphaComponent(0.35).setFill(); bounds.fill(using: .sourceOver)
      Theme.fg.withAlphaComponent(0.12).setFill(); d.fill(using: .sourceOver)
      Theme.fg.setStroke(); let p = NSBezierPath(rect: d.insetBy(dx: 1, dy: 1)); p.lineWidth = 2; p.stroke()
    }
    if !composition.isEmpty {                                 // marked text: on the cursor, underlined
      let a = NSAttributedString(string: composition, attributes: [.font: compositionFont, .foregroundColor: Theme.fg, .backgroundColor: Theme.box, .underlineStyle: NSUnderlineStyle.single.rawValue])
      a.draw(at: compositionOrigin)
    }
    if let n = note, !n.isEmpty {                             // a pane note ("Nothing to paste…", "The phone is driving")
      let a = NSAttributedString(string: n, attributes: [.font: NSFont.systemFont(ofSize: 12.5), .foregroundColor: Theme.fg])
      let s = a.size(), box = CGRect(x: (bounds.width - s.width) / 2 - 10, y: 14, width: s.width + 20, height: s.height + 12)
      Theme.box.setFill(); box.fill(); Theme.btnBorder.setStroke(); NSBezierPath(rect: box.insetBy(dx: 0.5, dy: 0.5)).stroke()
      a.draw(at: CGPoint(x: box.minX + 10, y: box.minY + 6))
    }
  }
}
