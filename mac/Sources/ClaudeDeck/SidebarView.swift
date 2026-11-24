// mac/Sources/ClaudeDeck/SidebarView.swift — the sidebar: one tile per session (every session of the deck, in every
// window: R5), the tool row under it. AppKit views drawn from the same numbers as renderer/style.css (Theme.swift);
// the terminals are Metal, the chrome is not (mac/README.md "Chrome: AppKit, not Metal").
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain).
// Windows counterpart: windows/src/sidebar.cpp (drawn with Direct2D). JS mirrored: renderer/js/30-tiles.js
// (updateTile, the clicks, the drag), 40-new-tile.js (the account buttons), 80-sidebar.js, style.css (.tile …).
//
// DRAFTED HERE: the tile (pin mark, abbreviation — green while busy —, title wrapped, effort tag, the placed mark,
// age, line 2 with the account and the status, the last message, the session + week bars), hover / selected /
// asleep / attention looks, click = select, double-click = place, the context menu, drag (to a pane, to reorder
// pins, out of the window), the in-tile close confirm, rename in place, the new-terminal and account buttons.
// The asks tile (AsksTileView, below) and the ask mark on a tile: 49-asks.js paintAsksTile / paintAskMark.
// NOT HERE YET (mac/PARITY.md): groups, the special-prompt form, Fable / context bars, bar labels,
// the animations, the rail (collapsed) look, the settings panel's widening.
import AppKit

protocol SidebarDelegate: AnyObject {
  func sidebarSelect(_ id: String)
  func sidebarPlace(_ id: String)                              // double-click: into the pane selected before
  func sidebarMenu(for id: String) -> NSMenu?
  func sidebarClose(_ id: String)                              // confirmed in the tile
  func sidebarRename(_ id: String, to title: String)
  func sidebarReorderPinned(_ ids: [String])
  func sidebarDraggedOut(_ id: String, at screenPoint: NSPoint)   // F5: dropped outside every window
  func sidebarNewTerminal()
  func sidebarNewSession(account: String)
  func sidebarTool(_ name: String, from view: NSView)          // "palette" | "settings" | "elsewhere" | "past" | "collapse"
  func sidebarAsks(row: String?)                               // the asks tile: a row's session id, AskModel.recent, nil = the tile
  func sidebarAsksHover()                                      // the pointer rests on the asks tile: the ask pane may get ready
}

final class SidebarView: NSView {
  weak var delegate: SidebarDelegate?
  private let scroll = NSScrollView()
  let list = TileListView()
  let asksTile = AsksTileView(frame: .zero)
  private var asksHeight: NSLayoutConstraint?
  private let tools = NSStackView()
  private let accounts = NSStackView()

  override init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true; layer?.backgroundColor = Theme.sidebar.cgColor
    scroll.drawsBackground = false; scroll.hasVerticalScroller = true; scroll.scrollerStyle = .overlay; scroll.documentView = list
    for v in [asksTile, scroll, accounts, tools] as [NSView] { v.translatesAutoresizingMaskIntoConstraints = false; addSubview(v) }
    asksTile.onClick = { [weak self] row in self?.delegate?.sidebarAsks(row: row) }
    asksTile.onHover = { [weak self] in self?.delegate?.sidebarAsksHover() }
    let ah = asksTile.heightAnchor.constraint(equalToConstant: 0); asksHeight = ah
    accounts.orientation = .horizontal; accounts.spacing = 4; accounts.alignment = .centerY
    tools.orientation = .horizontal; tools.spacing = 4; tools.alignment = .centerY
    for (title, name) in [(">_", "terminal"), ("Elsewhere", "elsewhere"), ("Past", "past"), ("⌘⇧P", "palette"), ("Settings", "settings")] {
      tools.addArrangedSubview(SidebarView.button(title, name, target: self, action: #selector(tool(_:))))
    }
    NSLayoutConstraint.activate([
      asksTile.topAnchor.constraint(equalTo: topAnchor, constant: 8), asksTile.leadingAnchor.constraint(equalTo: leadingAnchor, constant: Theme.tilePadX),
      asksTile.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -Theme.tilePadX), ah,
      scroll.topAnchor.constraint(equalTo: asksTile.bottomAnchor, constant: 4), scroll.leadingAnchor.constraint(equalTo: leadingAnchor), scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
      scroll.bottomAnchor.constraint(equalTo: accounts.topAnchor, constant: -4),
      accounts.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4), accounts.bottomAnchor.constraint(equalTo: tools.topAnchor, constant: -4),
      tools.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4), tools.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4),
    ])
  }
  required init?(coder: NSCoder) { fatalError("not used") }

  static func button(_ title: String, _ name: String, target: AnyObject, action: Selector) -> NSButton {
    let b = NSButton(title: title, target: target, action: action)
    b.identifier = NSUserInterfaceItemIdentifier(name)
    b.isBordered = false; b.wantsLayer = true
    b.layer?.backgroundColor = Theme.btn.cgColor; b.layer?.borderColor = Theme.btnBorder.cgColor; b.layer?.borderWidth = 1
    b.attributedTitle = NSAttributedString(string: " \(title) ", attributes: [.font: Theme.menuFont, .foregroundColor: Theme.fg])
    return b
  }
  @objc private func tool(_ sender: NSButton) {
    let name = sender.identifier?.rawValue ?? ""
    if name == "terminal" { delegate?.sidebarNewTerminal() } else { delegate?.sidebarTool(name, from: sender) }
  }
  @objc private func account(_ sender: NSButton) { delegate?.sidebarNewSession(account: sender.identifier?.rawValue ?? "") }

  /// A meta arrived (or the selection / layout changed): the tiles and the account buttons as they are now.
  func update(meta: DeckMeta, selected: String?, placed: Set<String>, windowIndex: Int, claudeAllowed: Bool) {
    list.delegate = delegate
    list.set(meta: meta, selected: selected, placed: placed, windowIndex: windowIndex)
    let want = meta.accounts.map { $0.id }
    if accounts.arrangedSubviews.compactMap({ $0.identifier?.rawValue }) != want {
      for v in accounts.arrangedSubviews { accounts.removeArrangedSubview(v); v.removeFromSuperview() }
      for a in meta.accounts {
        let b = SidebarView.button(a.abbr.isEmpty ? a.label : a.abbr, a.id, target: self, action: #selector(account(_:)))
        b.toolTip = claudeAllowed ? "New \(a.label) session" : "Claude sessions are off in this deck"
        accounts.addArrangedSubview(b)
      }
    }
  }
  /// The asks (every meta, and when the pane opens or closes): the asks tile and the marks on the session tiles
  func updateAsks(_ m: AskModel, paneAll: Bool) {
    asksTile.set(m, paneAll: paneAll)
    asksHeight?.constant = asksTile.isHidden ? 0 : asksTile.wantedHeight
    list.askMarks = m.tileMarks
  }
  override func layout() { super.layout(); list.setFrameSize(NSSize(width: scroll.contentSize.width, height: list.frame.height)); list.relayout() }
}

/// The tiles, drawn in one view (a tile is geometry + text, not a view: 30 tiles repaint as one rectangle list).
final class TileListView: NSView, NSDraggingSource, NSTextFieldDelegate {
  weak var delegate: SidebarDelegate?
  private var meta = DeckMeta()
  private var selected: String?
  private var placed: Set<String> = []
  private var windowIndex = 0
  private var rects: [(id: String, rect: CGRect)] = []
  private var hover: String?
  private var confirming: String?                              // the tile showing "Close this session?  [Close] [Cancel]"
  private var renaming: String?
  private var renameField: NSTextField?
  private var tracking: NSTrackingArea?
  private var mouseDownAt = CGPoint.zero, mouseDownId: String?
  /// tile id -> its session's ask mark (49-asks.js paintAskMark on .t-ask); empty when Settings -> Asks hides them
  var askMarks: [String: AskModel.Mark] = [:] { didSet { if askMarks != oldValue { relayout() } } }

  override var isFlipped: Bool { true }

  func set(meta: DeckMeta, selected: String?, placed: Set<String>, windowIndex: Int) {
    self.meta = meta; self.selected = selected; self.placed = placed; self.windowIndex = windowIndex
    if let c = confirming, meta.tile(c) == nil { confirming = nil }
    relayout()
  }

  // ---- layout: every tile's rectangle from its text (title wraps: `overflow-wrap: anywhere`)
  private func titleAttr(_ t: Tile) -> NSAttributedString {
    let p = NSMutableParagraphStyle(); p.lineBreakMode = .byCharWrapping; p.lineHeightMultiple = 1.0
    var attrs: [NSAttributedString.Key: Any] = [.font: Theme.titleFont, .foregroundColor: t.asleep ? Theme.fgDim : Theme.fg, .paragraphStyle: p]
    if t.title.isEmpty { attrs[.foregroundColor] = Theme.fgDim; attrs[.obliqueness] = 0.2 }          // .t-title.untitled: dim, italic
    return NSAttributedString(string: t.displayTitle, attributes: attrs)
  }
  private func messageAttr(_ t: Tile) -> NSAttributedString {
    let p = NSMutableParagraphStyle(); p.lineBreakMode = .byWordWrapping
    return NSAttributedString(string: t.lastText, attributes: [.font: Theme.subFont, .foregroundColor: Theme.fgDim.withAlphaComponent(t.asleep ? 0.45 : 1), .paragraphStyle: p])
  }
  private func abbr(_ t: Tile) -> String { t.isShell ? "" : (meta.account(t.account)?.abbr ?? "") }
  private func ageText(_ t: Tile) -> String { Format.age(ms: meta.now - (t.lastAt ?? meta.now)) }
  private func titleWidth(_ t: Tile, _ width: CGFloat) -> CGFloat {
    let a = abbr(t)
    let abbrW = a.isEmpty ? 0 : NSAttributedString(string: a, attributes: [.font: Theme.abbrFont]).size().width + 6
    let markW = askMarks[t.id].map { AsksTileView.markSize($0).width + 6 } ?? 0
    let ageW = NSAttributedString(string: ageText(t), attributes: [.font: Theme.ageFont]).size().width + 6
    let effortW: CGFloat = t.effort.isEmpty ? 0 : NSAttributedString(string: t.effort, attributes: [.font: Theme.effortFont]).size().width + 14
    return max(40, width - 2 * Theme.tilePadX - 2 - abbrW - markW - ageW - effortW - (placed.contains(t.id) ? 17 : 0))
  }
  private func height(_ t: Tile, width: CGFloat) -> CGFloat {
    let title = titleAttr(t).boundingRect(with: CGSize(width: titleWidth(t, width), height: 400), options: [.usesLineFragmentOrigin]).height
    var h = Theme.tilePadY * 2 + 2 + ceil(max(title, 14 * 1.25)) + 1 + 12            // line 1, the gap, line 2 (.t-sub: 12 px)
    if !t.isShell { h += 2 + 4 + 3 + 4 + 1 }                                         // .t-bars: two 4 px bars, 3 px apart
    if !t.lastText.isEmpty {                                                         // --msg-lines: 3
      let m = messageAttr(t).boundingRect(with: CGSize(width: width - 2 * Theme.tilePadX - 2, height: 400), options: [.usesLineFragmentOrigin]).height
      h += 1 + min(ceil(m), ceil(Theme.subFont.pointSize * 1.3) * 3)
    }
    if confirming == t.id { h += 26 }
    return h
  }
  func relayout() {
    var y: CGFloat = 0; rects.removeAll(keepingCapacity: true)
    let w = bounds.width
    for t in meta.ordered { let h = height(t, width: w); rects.append((t.id, CGRect(x: 0, y: y, width: w, height: h))); y += h + Theme.tileGap }
    if frame.height != max(y, 1) { setFrameSize(NSSize(width: w, height: max(y, 1))) }
    needsDisplay = true
  }

  // ---- drawing (style.css .tile and its parts)
  override func draw(_ dirtyRect: NSRect) {
    for (id, r) in rects where r.intersects(dirtyRect) { if let t = meta.tile(id) { drawTile(t, r) } }
  }
  private func drawTile(_ t: Tile, _ r: CGRect) {
    let active = t.id == selected
    (active ? Theme.tileActive : (hover == t.id ? Theme.tileHover : Theme.tile)).setFill(); r.fill()
    if active || t.attention { (t.attention ? Theme.attention : Theme.tileActiveBorder).setStroke(); NSBezierPath(rect: r.insetBy(dx: 0.5, dy: 0.5)).stroke() }
    let inner = r.insetBy(dx: Theme.tilePadX + 1, dy: Theme.tilePadY + 1)
    var x = inner.minX; var y = inner.minY
    if t.pinned { Theme.fgDim.setFill(); NSBezierPath(ovalIn: CGRect(x: r.minX + 1, y: r.minY + 1, width: 5, height: 5)).fill() }      // .t-pin (the glyph itself: PARITY)
    let dim: CGFloat = t.asleep ? 0.45 : 1
    let a = abbr(t)
    if !a.isEmpty {
      let s = NSAttributedString(string: a, attributes: [.font: Theme.abbrFont, .foregroundColor: (t.busy ? Theme.green : Theme.fgDim).withAlphaComponent(dim)])
      s.draw(at: CGPoint(x: x, y: y + 1)); x += s.size().width + 6
    }
    if let m = askMarks[t.id] { x += AsksTileView.drawMark(m, at: CGPoint(x: x, y: y + 3)) + 6 }   // .t-ask: questions on an ask page
    // right side of line 1, from the edge inwards: age, the placed mark, the effort tag
    var right = inner.maxX
    let age = NSAttributedString(string: ageText(t), attributes: [.font: Theme.ageFont, .foregroundColor: Theme.fgDim])
    right -= age.size().width; age.draw(at: CGPoint(x: right, y: y + 2)); right -= 6
    if placed.contains(t.id) {                                 // .t-quad: where it sits (drawn as one 11 px mark in this draft)
      Theme.fgDim.setStroke(); NSBezierPath(rect: CGRect(x: right - 11, y: y + 4, width: 10, height: 10)).stroke(); right -= 17
    }
    if !t.effort.isEmpty {
      let e = NSAttributedString(string: t.effort, attributes: [.font: Theme.effortFont, .foregroundColor: effortColor(t)])
      let w = e.size().width + 8
      Theme.rgb(0x2e2e2e).setStroke(); NSBezierPath(rect: CGRect(x: right - w + 0.5, y: y + 2.5, width: w - 1, height: 13)).stroke()
      e.draw(at: CGPoint(x: right - w + 4, y: y + 2)); right -= w + 6
    }
    let title = titleAttr(t)
    let tw = titleWidth(t, r.width)
    let th = ceil(max(title.boundingRect(with: CGSize(width: tw, height: 400), options: [.usesLineFragmentOrigin]).height, 14 * 1.25))
    if renaming != t.id { title.draw(with: CGRect(x: x, y: y, width: tw, height: th), options: [.usesLineFragmentOrigin]) }
    y += th + 1
    // line 2 (.t-sub): the account's name, the status words, which window holds it (R5)
    var sub = t.isShell ? "terminal" : (meta.account(t.account)?.label ?? t.account)
    if t.asleep { sub += " · asleep" } else if t.exited { sub += " · exited" } else if t.waiting { sub += " · waiting" }
    if let w = t.inWindow, w != windowIndex { sub += " · in window \(w + 1)" }
    NSAttributedString(string: sub, attributes: [.font: Theme.subFont, .foregroundColor: Theme.fgDim]).draw(at: CGPoint(x: inner.minX, y: y - 1))
    y += 12
    if !t.isShell {                                            // .t-bars: session and week (lib/usage.js: used 0-100)
      let u = meta.usage.obj(t.account)
      y += 2
      for key in ["session", "week"] {
        let track = CGRect(x: inner.minX, y: y, width: inner.width, height: 4)
        Theme.track.withAlphaComponent(dim).setFill(); track.fill()
        if let used = u.obj(key).double("used") {
          let f = max(0, min(100, used)) / 100
          ramp(f).withAlphaComponent(dim).setFill(); CGRect(x: track.minX, y: track.minY, width: track.width * CGFloat(f), height: 4).fill()
        }
        y += 4 + 3
      }
      y -= 3; y += 1
    }
    if !t.lastText.isEmpty {
      let m = messageAttr(t), mh = min(ceil(m.boundingRect(with: CGSize(width: inner.width, height: 400), options: [.usesLineFragmentOrigin]).height), ceil(Theme.subFont.pointSize * 1.3) * 3)
      m.draw(with: CGRect(x: inner.minX, y: y + 1, width: inner.width, height: mh), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
      y += 1 + mh
    }
    if confirming == t.id {                                    // the confirm row: the danger button, Cancel, the question
      let row = confirmRects(r)
      Theme.danger.setFill(); row.yes.fill(); Theme.btn.setFill(); row.no.fill()
      NSAttributedString(string: "Close", attributes: [.font: Theme.menuFont, .foregroundColor: NSColor.white]).draw(at: CGPoint(x: row.yes.minX + 8, y: row.yes.minY + 3))
      NSAttributedString(string: "Cancel", attributes: [.font: Theme.menuFont, .foregroundColor: Theme.fg]).draw(at: CGPoint(x: row.no.minX + 8, y: row.no.minY + 3))
      NSAttributedString(string: t.isShell ? "Close this terminal?" : "Close this session?", attributes: [.font: Theme.menuFont, .foregroundColor: Theme.fgDim]).draw(at: CGPoint(x: row.no.maxX + 8, y: row.no.minY + 3))
    }
  }
  private func confirmRects(_ r: CGRect) -> (yes: CGRect, no: CGRect) {
    let y = r.maxY - Theme.tilePadY - 22
    return (CGRect(x: r.minX + Theme.tilePadX + 1, y: y, width: 52, height: 20), CGRect(x: r.minX + Theme.tilePadX + 57, y: y, width: 58, height: 20))
  }
  private func effortColor(_ t: Tile) -> NSColor {            // .tile.effort-* .t-effort
    if t.ultracode { return Theme.rgb(0xd8b4fe) }
    switch t.effort { case "low": return Theme.rgb(0x7a7a7a); case "medium": return Theme.rgb(0xa8a8a8); case "high": return Theme.rgb(0xd8d4cc); default: return .white }
  }
  /// The bar's colour by how full it is. VERIFY against 30-tiles.js rampColor and the per-bar colours in settings
  /// (the user can set them): this draft uses green / amber / red at 70 % and 90 %.
  private func ramp(_ f: Double) -> NSColor { f < 0.7 ? Theme.green : (f < 0.9 ? Theme.attention : Theme.danger) }

  // ---- the mouse
  private func tile(at p: CGPoint) -> (id: String, rect: CGRect)? { rects.first { $0.rect.contains(p) } }
  override func updateTrackingAreas() {
    super.updateTrackingAreas()
    if let t = tracking { removeTrackingArea(t) }
    let t = NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil)
    addTrackingArea(t); tracking = t
  }
  override func mouseMoved(with event: NSEvent) { let h = tile(at: convert(event.locationInWindow, from: nil))?.id; if h != hover { hover = h; needsDisplay = true } }
  override func mouseExited(with event: NSEvent) { if hover != nil { hover = nil; needsDisplay = true } }
  override func mouseDown(with event: NSEvent) {
    let p = convert(event.locationInWindow, from: nil)
    guard let hit = tile(at: p) else { return }
    if confirming == hit.id {
      let row = confirmRects(hit.rect)
      if row.yes.contains(p) { confirming = nil; delegate?.sidebarClose(hit.id); relayout(); return }
      if row.no.contains(p) { confirming = nil; relayout(); return }
    }
    mouseDownAt = p; mouseDownId = hit.id
    if event.clickCount == 2 { delegate?.sidebarPlace(hit.id) }      // double-click places in the pane selected before
    else { delegate?.sidebarSelect(hit.id) }                         // a click selects, never places
  }
  override func rightMouseDown(with event: NSEvent) {
    guard let hit = tile(at: convert(event.locationInWindow, from: nil)) else { return }
    delegate?.sidebarSelect(hit.id)
    if let menu = delegate?.sidebarMenu(for: hit.id) { NSMenu.popUpContextMenu(menu, with: event, for: self) }
  }
  /// The tile's menu asks for these
  func askClose(_ id: String) { confirming = id; relayout() }
  func beginRename(_ id: String) {
    guard let hit = rects.first(where: { $0.id == id }), let t = meta.tile(id) else { return }
    endRename(save: false)
    let f = NSTextField(frame: CGRect(x: hit.rect.minX + Theme.tilePadX, y: hit.rect.minY + Theme.tilePadY, width: hit.rect.width - 2 * Theme.tilePadX, height: 28))
    f.stringValue = t.title; f.font = NSFont.systemFont(ofSize: 13.5); f.delegate = self; f.focusRingType = .none
    f.backgroundColor = Theme.sidebar; f.textColor = Theme.fg; f.isBordered = true
    addSubview(f); window?.makeFirstResponder(f)
    renaming = id; renameField = f; needsDisplay = true
  }
  private func endRename(save: Bool) {
    guard let f = renameField, let id = renaming else { return }
    renameField = nil; renaming = nil
    let text = f.stringValue
    f.removeFromSuperview(); needsDisplay = true
    if save { delegate?.sidebarRename(id, to: text) }
  }
  /// Enter saves, Esc cancels, leaving the box saves (30-tiles.js renameFns)
  func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
    if selector == #selector(NSResponder.insertNewline(_:)) { endRename(save: true); return true }
    if selector == #selector(NSResponder.cancelOperation(_:)) { endRename(save: false); return true }
    return false
  }
  func controlTextDidEndEditing(_ obj: Notification) { endRename(save: true) }

  // ---- drag: onto a pane (F4), among the pinned tiles (reorder), out of the window (F5)
  override func mouseDragged(with event: NSEvent) {
    let p = convert(event.locationInWindow, from: nil)
    guard let id = mouseDownId, hypot(p.x - mouseDownAt.x, p.y - mouseDownAt.y) > 5, let hit = rects.first(where: { $0.id == id }) else { return }
    mouseDownId = nil
    let item = NSPasteboardItem(); item.setString(id, forType: kTilePasteboardType)
    let drag = NSDraggingItem(pasteboardWriter: item)
    // the dragged tile's picture under the pointer
    if let rep = bitmapImageRepForCachingDisplay(in: hit.rect) {
      cacheDisplay(in: hit.rect, to: rep)
      let img = NSImage(size: hit.rect.size); img.addRepresentation(rep)
      drag.setDraggingFrame(hit.rect, contents: img)
    } else { drag.setDraggingFrame(hit.rect, contents: nil) }
    beginDraggingSession(with: [drag], event: event, source: self)
  }
  func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation { .move }
  func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint, operation: NSDragOperation) {
    guard let id = session.draggingPasteboard.string(forType: kTilePasteboardType) else { return }
    if operation != [] { return }                               // a pane took it
    // not taken: inside this list = a reorder among the pinned tiles; outside EVERY window of the app = a new window
    if let w = window {
      let local = convert(w.convertPoint(fromScreen: screenPoint), from: nil)
      if bounds.contains(local) {
        guard meta.tile(id)?.pinned == true else { return }
        var pinned = meta.ordered.filter { $0.pinned }.map { $0.id }.filter { $0 != id }
        let target = rects.first { $0.rect.contains(local) }?.id
        let at = target.flatMap { pinned.firstIndex(of: $0) } ?? pinned.count
        pinned.insert(id, at: at)
        delegate?.sidebarReorderPinned(pinned)
        return
      }
    }
    if !NSApp.windows.contains(where: { $0.isVisible && $0.frame.contains(screenPoint) }) { delegate?.sidebarDraggedOut(id, at: screenPoint) }
  }
}

// ---------------------------------------------------------------------------------------------- the asks tile
/// 49-asks.js paintAsksTile: at the top of the sidebar, every session's waiting asks, one row each (sessions outside
/// the deck included), and a "recently answered" row. A click opens the ask pane with every session's asks, scrolled
/// to the row clicked; the same click on the tile while that view is open closes it (AskPane.tileClick).
/// Drawn, not views (as the tiles are). Not here yet: the pulse while a question waits (the border is yellow, still),
/// hover looks, a scroll past AsksTileView.maxRows rows, a hover title per row (the tile has one title).
final class AsksTileView: NSView {
  var onClick: ((String?) -> Void)?
  /// the pointer has rested on the tile for AsksTileView.hoverSeconds (windows/src/asks.hpp ASK_WARM_HOVER_MS)
  var onHover: (() -> Void)?
  static let hoverSeconds: TimeInterval = 0.15
  private var hoverTimer: Timer?
  private var tracking: NSTrackingArea?
  private var model = AskModel()
  private var paneAll = false
  private var rows: [(sid: String, rect: CGRect)] = []
  static let topH: CGFloat = 19, rowH: CGFloat = 17, pad: CGFloat = 5, maxRows = 7
  override var isFlipped: Bool { true }

  func set(_ m: AskModel, paneAll: Bool) {
    model = m; self.paneAll = paneAll
    isHidden = !m.showTile
    toolTip = m.guest ? "Another deck is serving the ask pages: they show here read-only. Answer on that deck, or in the session's terminal." : "Questions from Claude sessions on ask pages. Click to answer them (a row: that session first); \(AskModel.zoomKey) or \(AskModel.asksKey) closes. \(AskModel.asksKey) on a session shows just its own."
    needsDisplay = true
  }
  private var rowCount: Int { min(model.asks.count, AsksTileView.maxRows) + (model.askLog.isEmpty ? 0 : 1) }
  var wantedHeight: CGFloat { AsksTileView.pad * 2 + AsksTileView.topH + CGFloat(rowCount) * AsksTileView.rowH }

  // style.css .t-ask: min-width 15, height 13, padding 0 3, a 1 px border, 10 px bold; "?" before the count (not on a tick)
  private static let markFont = NSFont.systemFont(ofSize: 10, weight: .bold)
  private static func markText(_ m: AskModel.Mark) -> String { m.kind == "sent" ? m.text : "?" + m.text }
  static func markSize(_ m: AskModel.Mark) -> CGSize {
    let w = NSAttributedString(string: markText(m), attributes: [.font: markFont]).size().width + 6 + 2
    return CGSize(width: max(15, ceil(w)), height: 13)
  }
  /// draws the mark with its top-left at `p`; returns its width
  @discardableResult static func drawMark(_ m: AskModel.Mark, at p: CGPoint) -> CGFloat {
    let size = markSize(m), r = CGRect(origin: p, size: size)
    let line = m.kind == "sent" ? Theme.askDim : Theme.ask
    if m.kind == "drafted" { Theme.ask.setFill(); r.fill() }
    line.setStroke(); NSBezierPath(rect: r.insetBy(dx: 0.5, dy: 0.5)).stroke()
    let ink = m.kind == "drafted" ? Theme.askInk : line
    let s = NSAttributedString(string: markText(m), attributes: [.font: markFont, .foregroundColor: ink])
    s.draw(at: CGPoint(x: r.minX + (size.width - s.size().width) / 2, y: r.minY - 1))
    return size.width
  }

  override func draw(_ dirtyRect: NSRect) {
    let n = model.asks.count, asking = model.asks.contains { model.unsent($0) }
    let b = bounds
    (paneAll ? Theme.asksTileActive : Theme.asksTileBg).setFill(); b.fill()
    (asking || paneAll ? Theme.attention : Theme.asksTileBorder).setStroke(); NSBezierPath(rect: b.insetBy(dx: 0.5, dy: 0.5)).stroke()
    let x0 = AsksTileView.pad + 1, right = b.width - AsksTileView.pad - 1
    var y = AsksTileView.pad
    // .t-top: the glyph, "Asks", the count
    let glyph = CGRect(x: x0, y: y + 1, width: 15, height: 15)
    let gc = n > 0 ? Theme.attention : Theme.fgFaint
    if asking { gc.setFill(); glyph.fill() }
    gc.setStroke(); NSBezierPath(rect: glyph.insetBy(dx: 0.5, dy: 0.5)).stroke()
    let q = NSAttributedString(string: "?", attributes: [.font: NSFont.systemFont(ofSize: 11, weight: .bold), .foregroundColor: asking ? Theme.rgb(0x1a1408) : gc])
    q.draw(at: CGPoint(x: glyph.midX - q.size().width / 2, y: glyph.minY))
    NSAttributedString(string: "Asks", attributes: [.font: NSFont.systemFont(ofSize: 13, weight: n > 0 ? .semibold : .medium), .foregroundColor: n > 0 ? Theme.fg : Theme.fgDim])
      .draw(at: CGPoint(x: glyph.maxX + 6, y: y))
    // a guest (another deck serves the pages): they show read-only (windows/src/sidebar.cpp, the same words)
    let countText = model.guest ? (n > 0 ? "\(n) waiting · read-only" : "read-only") : (n > 0 ? "\(n) waiting" : "nothing waiting")
    let count = NSAttributedString(string: countText,
                                   attributes: [.font: NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .semibold), .foregroundColor: n > 0 ? Theme.attention : Theme.fgFaint])
    count.draw(at: CGPoint(x: right - count.size().width, y: y + 1))
    y += AsksTileView.topH
    rows.removeAll(keepingCapacity: true)
    let small = NSFont.systemFont(ofSize: 12)
    for s in model.asks.prefix(AsksTileView.maxRows) {
      let sid = s.str("sessionId"), r = CGRect(x: 0, y: y, width: b.width, height: AsksTileView.rowH)
      rows.append((sid, r))
      var x = x0 + 2
      if let m = model.mark(sid) { x += AsksTileView.drawMark(m, at: CGPoint(x: x, y: y + 2)) + 6 }
      let ab = model.abbr(s)
      if !ab.isEmpty {
        let a = NSAttributedString(string: ab, attributes: [.font: NSFont.systemFont(ofSize: 12, weight: .semibold), .foregroundColor: Theme.fgDim])
        a.draw(at: CGPoint(x: x, y: y)); x += a.size().width + 6
      }
      let age = NSAttributedString(string: model.age(s["latest"]), attributes: [.font: Theme.ageFont, .foregroundColor: Theme.fgFaint])
      age.draw(at: CGPoint(x: right - age.size().width, y: y))
      let ended = AskModel.truthy(s["ended"])
      let whereText = ended ? "session ended" : model.tileOf(sid) == nil ? "outside the deck" : ""
      let wh = NSAttributedString(string: whereText, attributes: [.font: Theme.subFont, .foregroundColor: Theme.fgFaint])
      let nameW = max(20, right - age.size().width - 6 - x - (whereText.isEmpty ? 0 : wh.size().width + 6))
      let p = NSMutableParagraphStyle(); p.lineBreakMode = .byTruncatingTail
      let name = NSAttributedString(string: model.name(s), attributes: [.font: small, .foregroundColor: ended ? Theme.fgDim : Theme.fg, .paragraphStyle: p])
      name.draw(with: CGRect(x: x, y: y, width: nameW, height: AsksTileView.rowH), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
      if !whereText.isEmpty { wh.draw(at: CGPoint(x: x + min(nameW, name.size().width) + 6, y: y + 1)) }
      y += AsksTileView.rowH
    }
    if let l = model.askLog.first {
      rows.append((AskModel.recent, CGRect(x: 0, y: y, width: b.width, height: AsksTileView.rowH)))
      NSAttributedString(string: "Recently answered (\(model.askLog.count))", attributes: [.font: small, .foregroundColor: Theme.fgDim]).draw(at: CGPoint(x: x0 + 2, y: y))
      let age = NSAttributedString(string: model.age(l["at"]), attributes: [.font: Theme.ageFont, .foregroundColor: Theme.fgFaint])
      age.draw(at: CGPoint(x: right - age.size().width, y: y))
    }
  }
  override func mouseDown(with event: NSEvent) {
    hoverTimer?.invalidate(); hoverTimer = nil
    let p = convert(event.locationInWindow, from: nil)
    onClick?(rows.first { $0.rect.contains(p) }?.sid)
  }
  override func updateTrackingAreas() {
    super.updateTrackingAreas()
    if let t = tracking { removeTrackingArea(t) }
    let t = NSTrackingArea(rect: bounds, options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil)
    addTrackingArea(t); tracking = t
  }
  override func mouseEntered(with event: NSEvent) {
    hoverTimer?.invalidate()
    hoverTimer = Timer.scheduledTimer(withTimeInterval: AsksTileView.hoverSeconds, repeats: false) { [weak self] _ in self?.onHover?() }
  }
  override func mouseExited(with event: NSEvent) { hoverTimer?.invalidate(); hoverTimer = nil }
}
