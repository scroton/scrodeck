// mac/Sources/ClaudeDeck/DeckWindowController.swift — one deck window: the sidebar, the terminal area, and the
// rules that join them — what is selected, what is placed where (the grid layout, the digit chord, a dropped
// tile), which panes are on screen (attached, shown, sized), how a meta is applied, the window's own keys.
// The same class is the window of a LOCAL deck and the desk of a REMOTE one; `conn.isLocal` is the only switch.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain).
// Windows counterpart: `struct App` in windows/src/app.cpp (its blocks name the same JS).
// JS mirrored: renderer/js/25-layout.js (visibleIds, applyLayout, placeTile, the chord, zoom), 70-meta.js
// (applyMeta), 20-terminals.js (activate, redrawTile, fitOne), 85-focus.js, 90-boot.js, renderer/desk/bridge.js.
import AppKit
import DeckCoreBridge

protocol DeckWindowOwner: AnyObject {
  func windowClosed(_ w: DeckWindowController)
  func openWindow(holding tileId: String, at screenPoint: NSPoint?)
  func newSession(account: String, inNewWindow: Bool)
  var localHost: DeckHost? { get }
}

final class DeckWindowController: NSObject, NSWindowDelegate, DeckConnectionDelegate, TerminalAreaDelegate, SidebarDelegate, NSSplitViewDelegate, AskPaneHost {
  let window: NSWindow
  let conn: DeckConnection
  weak var owner: DeckWindowOwner?
  private let split = NSSplitView()
  private let sidebar = SidebarView(frame: .zero)
  private let area = TerminalAreaView(frame: .zero)
  private var meta = DeckMeta()
  private var panes: [String: TerminalPane] = [:]           // only tiles this window has shown; an unshown tile costs nothing here
  private var attached: Set<String> = []
  private var layout = GridLayout(rows: 2, cols: 2)
  private var selected: String?
  private var zoomed: String?
  private var windowIndex = 0
  private var home = "", platformOS = "darwin"
  private var ready = false                                  // deskInit answered
  private var chordHeld: Set<UInt16> = [], chordCells: Set<Int> = []
  private var lastSize: [String: (Int, Int)] = [:]
  private var away: NSTextField?
  private var panel: DeckPanel?
  private var reconnect: Timer?
  /// the ask pane over the terminal area (AskPane.swift: a WKWebView, made only while it is of use)
  private let askPane: AskPane
  private var noteTimer: Timer?

  init(conn: DeckConnection, bounds: CGRect?, inactive: Bool = false) {
    self.conn = conn
    askPane = AskPane(remote: conn.remoteLink)
    let frame = bounds ?? CGRect(x: 120, y: 120, width: 1600, height: 960)          // main.js createWindow: 1600 x 960
    window = NSWindow(contentRect: frame, styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
    super.init()
    window.title = Edition.productName; window.minSize = NSSize(width: 800, height: 500)
    window.backgroundColor = Theme.bg; window.isReleasedWhenClosed = false
    window.delegate = self
    window.appearance = NSAppearance(named: .darkAqua)
    split.isVertical = true; split.dividerStyle = .thin; split.delegate = self
    split.addArrangedSubview(sidebar); split.addArrangedSubview(area)
    sidebar.setFrameSize(NSSize(width: CGFloat(UserDefaults.standard.double(forKey: "sidebarWidth")).clamped(Theme.sidebarMinWidth, 900, or: Theme.sidebarWidth), height: frame.height))
    window.contentView = split
    sidebar.delegate = self; sidebar.list.delegate = self; area.delegate = self; area.layoutModel = layout
    askPane.host = self
    askPane.view.frame = area.bounds; askPane.view.autoresizingMask = [.width, .height]
    area.addSubview(askPane.view)                              // over the terminals and their chrome; hidden until it opens
    conn.delegate = self
    if inactive { window.orderFrontRegardless() } else { window.makeKeyAndOrderFront(nil) }   // a desk the on-call opened never takes the keys
    window.makeFirstResponder(area)
    if !conn.isLocal { conn.connect() }
  }

  // ------------------------------------------------------------------ the connection
  func connectionOpened(_ c: DeckConnection) {
    setAway(nil)
    let name = Host.current().localizedName ?? "Mac"
    c.call("deskInit", [["name": name, "kind": "native"]]) { [weak self] r in
      guard let me = self else { return }
      switch r {
      case .failure(let e): me.setAway(e.message)             // "one desk at a time" …
      case .success(let v):
        guard let o = v as? JSONObject else { return }
        me.home = o.str("home"); me.platformOS = o.obj("platform").str("os")
        me.windowIndex = o.int("windowIndex") ?? 0
        let g = o.obj("grid")
        me.layout.regrid(rows: g.int("rows") ?? 2, cols: g.int("cols") ?? 2)
        me.layout.load(json: JSON.text(o.obj("layout")))      // R7: the saved layout of this window
        me.attached.removeAll(); me.lastSize.removeAll()     // a reconnect is a fresh window: everything attaches again
        me.ready = true
        me.apply(meta: o.obj("meta"))
        me.reportWindowState()
      }
    }
  }
  func connectionClosed(_ c: DeckConnection, reason: String) {
    ready = false
    setAway(reason.hasPrefix("refused") ? "This deck refused the link (\(reason)). Check the link in Settings → Phone on the deck." : "The deck is away — reconnecting…")
    reconnect?.invalidate()
    if !reason.hasPrefix("refused") { reconnect = Timer.scheduledTimer(withTimeInterval: 2, repeats: false) { [weak self] _ in self?.conn.connect() } }
  }
  func connection(_ c: DeckConnection, data id: String, bytes: UnsafePointer<CChar>, count: Int, replay: Bool) {
    guard let pane = panes[id] else { return }
    let replies = pane.feed(bytes, count: count)
    // the terminal's own answers (cursor position, device attributes): the deck's window writes them back; a
    // remote desk never does (R8) — the deck's own window, if one shows the tile, answers
    if conn.isLocal, !replay, !replies.isEmpty { c.input(id, replies) }
    if conn.isLocal, !pane.title.isEmpty, pane.title != meta.tile(id)?.title { c.fire("setOscTitle", [id, pane.title]) }
    area.paneChanged(id)
  }
  func connection(_ c: DeckConnection, meta m: JSONObject) { apply(meta: m) }
  func connection(_ c: DeckConnection, exit id: String, code: Int) {
    guard let pane = panes[id] else { return }
    let line = "\r\n[process exited with code \(code)]\r\n"   // 20-terminals.js deck.onExit
    _ = line.withCString { pane.feed($0, count: line.utf8.count) }
    pane.exitLine = line
    area.paneChanged(id)
  }
  func connection(_ c: DeckConnection, reset id: String) { panes[id]?.reset(); area.paneChanged(id) }
  func connection(_ c: DeckConnection, event name: String, payload: Any?) {
    switch name {
    case "layout:evict":                                       // another window of the deck took these
      for id in payload as? [String] ?? [] { layout.unplace(id) }
      relayout()
    case "grid:changed":                                       // every layout was cleared by the deck
      let g = payload as? JSONObject ?? [:]
      layout.clear(); layout.regrid(rows: g.int("rows") ?? 2, cols: g.int("cols") ?? 2); zoomed = nil
      relayout()
    case "tiles:activate":                                     // a clicked toast
      if let id = payload as? String { activate(id) }
    case "layout:leaving":
      let o = payload as? JSONObject ?? [:]
      var s = leavingIds; if o.bool("on") { s.insert(o.str("id")) } else { s.remove(o.str("id")) }
      leavingIds = s; area.setLeaving(s)
    case "asks:open":                                          // a clicked new-ask toast (core/deck/deck_parity.cpp)
      let o = payload as? JSONObject ?? [:]
      askPane.onAskOpen(sessionId: o.str("sessionId"), tileId: o.str("tileId"))
    default: break
    }
  }
  private var leavingIds: Set<String> = []

  private func setAway(_ text: String?) {
    away?.removeFromSuperview(); away = nil
    guard let text = text, let content = window.contentView else { return }
    let f = NSTextField(labelWithString: text)
    f.textColor = Theme.fg; f.backgroundColor = Theme.rgb(0x3a2a12); f.drawsBackground = true; f.alignment = .center
    f.frame = CGRect(x: 0, y: content.bounds.height - 26, width: content.bounds.width, height: 26); f.autoresizingMask = [.width, .minYMargin]
    content.addSubview(f); away = f
  }

  // ------------------------------------------------------------------ meta (70-meta.js applyMeta)
  private func apply(meta m: JSONObject) {
    let change = meta.apply(m)
    for id in change.removed { panes[id] = nil; attached.remove(id); lastSize[id] = nil; if zoomed == id { zoomed = nil } }
    layout.prune(live: meta.tiles.map { $0.id })
    let g = meta.grid
    if g != layout.grid { layout.regrid(rows: g.rows, cols: g.cols) }
    if selected == nil || meta.tile(selected!) == nil {        // the selection falls back: remembered, else the first tile
      let remembered = UserDefaults.standard.string(forKey: "selected.\(windowIndex)")
      selected = meta.tile(remembered ?? "") != nil ? remembered : meta.ordered.first?.id
    }
    area.configureFont(family: meta.fontFamily, size: zoomed != nil ? CGFloat(meta.settings.double("zoomFontSize") ?? Double(meta.fontSize)) : meta.fontSize)
    relayout()
    askPane.setMeta(m)                                         // 49-asks.js paintAsks: the asks tile, the marks, the pane
  }

  // ------------------------------------------------------------------ what is on screen (25-layout.js visibleIds / applyLayout)
  private var visibleIds: [String] {
    if let z = zoomed, meta.tile(z) != nil { return [z] }
    if layout.isEmpty {
      // no layout: the selected tile fills the area — unless another window of the deck holds it (the empty notice)
      guard let s = selected, let t = meta.tile(s) else { return [] }
      if let w = t.inWindow, w != windowIndex { return [] }
      return [s]
    }
    return layout.ids.filter { meta.tile($0) != nil }
  }
  private func frame(of id: String) -> CGRect {
    let b = area.bounds
    if zoomed == id || layout.isEmpty { return b }
    guard let r = layout.rect(of: id) else { return .zero }
    return CGRect(x: (r.minX * b.width).rounded(), y: (r.minY * b.height).rounded(), width: (r.width * b.width).rounded(), height: (r.height * b.height).rounded())
  }
  /// Everything that follows from "the layout, the selection, the window size or a meta changed".
  func relayout() {
    guard ready else { return }
    let ids = visibleIds
    let hidden = window.isMiniaturized || !window.isVisible
    var frames: [PaneFrame] = []
    for id in ids {
      guard let t = meta.tile(id) else { continue }
      let f = frame(of: id)
      var size = area.fit(f)
      if let m = t.mirror { size = (m.cols, m.rows) }          // another client owns the size: render at exactly its size
      let pane = panes[id] ?? { let p = TerminalPane(id: id, cols: size.cols, rows: size.rows); panes[id] = p; return p }()
      pane.resize(cols: size.cols, rows: size.rows)
      // only a visible pane this window drives pushes its size (20-terminals.js fitOne)
      if t.mirror == nil, !hidden, !t.asleep, lastSize[id].map({ $0 != (size.cols, size.rows) }) ?? true { conn.resize(id, cols: size.cols, rows: size.rows); lastSize[id] = (size.cols, size.rows) }
      if !attached.contains(id) { pane.reset(); conn.attach(id); attached.insert(id) }      // a pane coming on screen is reset locally, then replayed
      let acct = meta.account(t.account)
      frames.append(PaneFrame(pane: pane, frame: f, title: t.displayTitle + (zoomed == id ? " · zoomed" : ""), abbr: t.isShell ? "" : (acct?.abbr ?? ""), busy: t.busy, attention: t.attention))
    }
    for id in attached.subtracting(ids) { conn.detach(id); attached.remove(id); lastSize[id] = nil }   // attach only what is on screen
    conn.shown(ids, hidden: hidden)
    area.setPanes(frames)
    sidebar.update(meta: meta, selected: selected, placed: Set(layout.ids), windowIndex: windowIndex, claudeAllowed: true)
    // the window's title: the one session that fills it, else `Claude Deck [n]` (25-layout.js paintWindowTitle)
    if ids.count == 1, let t = meta.tile(ids[0]) { window.title = t.displayTitle } else { window.title = windowIndex == 0 ? Edition.productName : "\(Edition.productName) [\(windowIndex + 1)]" }
    area.setNote(ids.isEmpty && selected != nil ? "This session is shown in another window of the deck." : nil)
  }
  private func sendLayout() { conn.layout(layout); relayout() }

  // ------------------------------------------------------------------ selecting and placing
  /// 20-terminals.js activate: select; an unplaced tile takes the pane selected before (layoutAdopt); the zoom follows
  func activate(_ id: String) {
    guard meta.tile(id) != nil else { return }
    let previous = selected
    selected = id
    UserDefaults.standard.set(id, forKey: "selected.\(windowIndex)")
    if zoomed != nil { zoomed = id }
    if !layout.isEmpty, layout.cells(of: id).isEmpty, let p = previous, p != id {
      let cells = layout.cells(of: p)
      if !cells.isEmpty { layout.place(id, cells: cells); conn.layout(layout) }
    }
    relayout()
  }
  /// F2 / F3 / F4: place a tile on cells (legalised: R1; evictions: R6; shrinks: R3)
  func place(_ id: String, cells: [Int]) {
    guard meta.tile(id) != nil, !cells.isEmpty else { return }
    zoomed = nil
    layout.place(id, cells: cells)
    selected = id
    sendLayout()
  }
  func areaSelected() -> String? { selected }
  func areaSelect(_ id: String) { if id != selected { activate(id) } }
  func areaDrop(tileId: String, cells: [Int]) { place(tileId, cells: cells) }
  func sidebarSelect(_ id: String) { activate(id) }
  func sidebarPlace(_ id: String) {
    // double-click: into the pane selected before, else the whole grid
    let cells = selected.flatMap { layout.cells(of: $0) }.flatMap { $0.isEmpty ? nil : $0 } ?? Array(1...(layout.grid.rows * layout.grid.cols))
    place(id, cells: cells)
    window.makeFirstResponder(area)
  }

  // ---- the digit chord (F2): Option+digit from anywhere, a bare digit while the sidebar has the keys (R2).
  //      Keys by PHYSICAL position (with Option down a Mac reports the Option glyph, not the digit).
  private static let digitKeyCodes: [UInt16: Int] = [18: 1, 19: 2, 20: 3, 21: 4, 23: 5, 22: 6, 26: 7, 28: 8, 25: 9]
  private func chordDown(_ e: NSEvent) -> Bool {
    guard let d = DeckWindowController.digitKeyCodes[e.keyCode], d <= layout.grid.rows * layout.grid.cols, selected != nil else { return false }
    chordHeld.insert(e.keyCode); chordCells.insert(d)
    return true
  }
  func areaKeyUp(_ event: NSEvent) { chordUp(event) }
  func chordUp(_ e: NSEvent) {
    guard chordHeld.remove(e.keyCode) != nil, chordHeld.isEmpty else { return }
    let cells = Array(chordCells).sorted(); chordCells.removeAll()
    if let s = selected { place(s, cells: cells) }             // the diagonal {1,4} is promoted by the model (R1)
  }
  func chordAbandon() { chordHeld.removeAll(); chordCells.removeAll() }

  // ------------------------------------------------------------------ the window's own keys
  /// lib/platform.js keyAction (the Mac branch, through the core) + the chord. Called for every key-down in the
  /// terminal area before the terminal sees it; the app's local key monitor calls it for the chrome too.
  func areaWindowKey(_ event: NSEvent) -> Bool {
    conn.windowKey()                                           // "the user is at this machine" (local decks: hibernation, the driver)
    if panel != nil { if event.keyCode == 53 { closePanel(); return true }; return panel?.handleKey(event) ?? false }
    if askPane.key(event) { return true }                     // Esc on the ask pane while the window (not its page) has the keys
    var mods: Int32 = 0
    let f = event.modifierFlags
    if f.contains(.shift) { mods |= DB_MOD_SHIFT }; if f.contains(.option) { mods |= DB_MOD_ALT }
    if f.contains(.control) { mods |= DB_MOD_CTRL }; if f.contains(.command) { mods |= DB_MOD_META }
    if f.contains(.option) && !f.contains(.command) && !f.contains(.control) && chordDown(event) { return true }
    var key = event.charactersIgnoringModifiers ?? ""
    if event.keyCode == 36 || event.keyCode == 76 { key = "Enter" }
    let action = String(cString: db_key_action("darwin", key, "", mods))
    switch action {
    case "sidebar": toggleSidebar()
    case "palette": showPanel(.palette)
    case "fullscreen": window.toggleFullScreen(nil)
    case "prev": step(-1)
    case "next": step(1)
    case "settings": showPanel(.settings)
    case "redraw": if let s = selected { redraw(s) }
    case "zoom": if !askPane.asksZoom() { toggleZoom() }       // the asks first (25-layout.js): it closes the every-session view
    case "asks": askPane.asksKey(activeTileId: selected)
    case "devtools": break                                     // Electron's; nothing to open here
    default:
      // while the ask pane is open nothing typed reaches a terminal behind it (49-asks.js); the page's own keys
      // (a text box, Cmd+C / Cmd+V in it) are the web view's: it is the first responder then, not the area. Cmd
      // keys go on to the menu bar (Cmd+Q, Cmd+W); a paste into the terminal is refused in areaSend.
      if askPane.isOpen && window.firstResponder === area && !f.contains(.command) { return true }
      return false
    }
    return true
  }
  /// F11 / F12's walk, as a plain step in the sidebar's order (the preview / commit / Esc of 25-layout.js: PARITY)
  private func step(_ d: Int) {
    let ids = meta.ordered.map { $0.id }
    guard !ids.isEmpty else { return }
    let i = selected.flatMap { ids.firstIndex(of: $0) } ?? 0
    activate(ids[(i + d + ids.count) % ids.count])
  }
  private func toggleZoom() { zoomed = zoomed == nil ? selected : nil; apply(meta: meta.raw) }
  private func toggleSidebar() { sidebar.isHidden.toggle(); split.adjustSubviews(); relayout() }
  /// 20-terminals.js redrawTile: reset, re-attach with only what was written at this size, nudge the pty
  private func redraw(_ id: String) {
    guard let pane = panes[id], let t = meta.tile(id) else { return }
    pane.reset(); conn.attach(id, sinceResize: true)
    if t.mirror == nil, pane.cols > 2, pane.rows > 1 { conn.fire("nudge", [id, pane.cols, pane.rows]) }
    area.paneChanged(id)
  }

  // ------------------------------------------------------------------ the terminal area's other asks
  var areaIsLocal: Bool { conn.isLocal }
  func areaTile(_ id: String) -> Tile? { meta.tile(id) }
  func areaSend(_ id: String, _ bytes: [UInt8], realKey: Bool) {
    guard let t = meta.tile(id) else { return }
    if realKey && askPane.isOpen { return }                    // nothing typed (or pasted) reaches a terminal behind the ask pane
    // R8: a remote sends real keystrokes and nothing else. The view only calls this with realKey false for the
    // terminal's own reports in LOCAL mode; this is the second lock, and the codec's `synthesised` the third.
    if !conn.isLocal {
      if !realKey { return }
      if bytes.withUnsafeBufferPointer({ b in b.withMemoryRebound(to: CChar.self) { db_frame_synthesised($0.baseAddress, $0.count) != 0 } }) { return }
    }
    if t.asleep { return }                                     // a sleeping tile is not woken by its own terminal
    conn.input(id, bytes)
    if realKey, let p = panes[id], !p.touched { p.touched = true; conn.fire("userInput", [id]) }   // a real key: the tile is no longer fresh
  }
  func areaWheelReport(_ id: String, _ report: [UInt8]) { conn.wheel(id, report) }
  func areaPaneName(_ id: String, doubleClick: Bool) { if doubleClick { toggleZoom() } else { redraw(id) } }
  func areaResized() { relayout() }
  func areaNote(_ text: String) { note(text, seconds: 3) }
  func note(_ text: String, seconds: Double) {
    area.setNote(text)
    noteTimer?.invalidate()
    noteTimer = Timer.scheduledTimer(withTimeInterval: max(0.5, seconds), repeats: false) { [weak self] _ in self?.area.setNote(nil) }
  }
  func areaContextMenu(for id: String, event: NSEvent) -> NSMenu? { sidebarMenu(for: id) }

  // ------------------------------------------------------------------ the sidebar's asks (30-tiles.js, 35-groups.js openTileMenu)
  func sidebarMenu(for id: String) -> NSMenu? {
    guard let t = meta.tile(id) else { return nil }
    let m = NSMenu(); m.autoenablesItems = false
    func add(_ title: String, _ action: Selector, on: Bool = false, enabled: Bool = true) {
      let i = NSMenuItem(title: title, action: action, keyEquivalent: ""); i.target = self; i.representedObject = id; i.state = on ? .on : .off; i.isEnabled = enabled; m.addItem(i)
    }
    // the first entry (35-groups.js + 49-asks.js askMenuItem): its questions, or its last answers read-only
    if let a = askPane.menuItem(tileId: id) {
      let i = NSMenuItem(title: a.label, action: #selector(menuAsk(_:)), keyEquivalent: ""); i.target = self; i.representedObject = AskMenuAction(a.run)
      m.addItem(i); m.addItem(.separator())
    }
    add("Rename", #selector(menuRename(_:)))
    add(t.pinned ? "Unpin" : "Pin", #selector(menuPin(_:)))
    m.addItem(.separator())
    let all = layout.grid.rows * layout.grid.cols
    add("Fill the window", #selector(menuFill(_:)), on: layout.cells(of: id).count == all)
    add("Remove from layout", #selector(menuUnplace(_:)), enabled: !layout.cells(of: id).isEmpty)
    add("Open in a new window", #selector(menuNewWindow(_:)), enabled: conn.isLocal)        // a second remote desk is unsupported (PROTOCOL.md)
    m.addItem(.separator())
    if !t.isShell {
      add(t.asleep ? "Wake" : "Sleep", #selector(menuSleep(_:)))
      add("Refresh", #selector(menuRefresh(_:)))
    }
    add(t.isShell ? "Close" : "Close session…", #selector(menuClose(_:)))
    return m
  }
  private func id(_ sender: Any?) -> String { ((sender as? NSMenuItem)?.representedObject as? String) ?? "" }
  private func report(_ r: Result<Any?, DeckError>) { if case .failure(let e) = r { areaNote(e.message) } }    // a call the deck refuses is said, never swallowed
  func beginRename(_ id: String) { sidebar.list.beginRename(id) }
  @objc private func menuAsk(_ s: Any?) { ((s as? NSMenuItem)?.representedObject as? AskMenuAction)?.run() }
  @objc private func menuRename(_ s: Any?) { beginRename(id(s)) }
  @objc private func menuPin(_ s: Any?) { let i = id(s); conn.call("pin", [i, !(meta.tile(i)?.pinned ?? false)]) { [weak self] in self?.report($0) } }
  @objc private func menuFill(_ s: Any?) { place(id(s), cells: Array(1...(layout.grid.rows * layout.grid.cols))) }
  @objc private func menuUnplace(_ s: Any?) { layout.unplace(id(s)); sendLayout() }
  @objc private func menuNewWindow(_ s: Any?) { owner?.openWindow(holding: id(s), at: nil) }
  @objc private func menuSleep(_ s: Any?) { let i = id(s); conn.call(meta.tile(i)?.asleep == true ? "wakeTile" : "sleepTile", [i]) { [weak self] in self?.report($0) } }
  @objc private func menuRefresh(_ s: Any?) { conn.call("refreshTile", [id(s)]) { [weak self] in self?.report($0) } }
  @objc private func menuClose(_ s: Any?) { sidebar.list.askClose(id(s)) }
  func sidebarClose(_ id: String) { conn.call("closeTile", [id]) { [weak self] in self?.report($0) } }
  func sidebarRename(_ id: String, to title: String) { conn.call("renameTile", [id, title]) { [weak self] in self?.report($0) } }
  func sidebarReorderPinned(_ ids: [String]) { conn.call("reorder", [ids]) { [weak self] in self?.report($0) } }
  func sidebarDraggedOut(_ id: String, at screenPoint: NSPoint) { if conn.isLocal { owner?.openWindow(holding: id, at: screenPoint) } }
  func sidebarNewTerminal() { newTile(["kind": "shell"]) }
  func sidebarNewSession(account: String) { newTile(["kind": "claude", "account": account]) }
  func newTile(_ spec: JSONObject) {
    conn.call("newTile", [spec]) { [weak self] r in
      guard let me = self else { return }
      me.report(r)
      // the new tile is in the NEXT meta; select it when it arrives (25-layout.js placeNew: the first empty region — PARITY)
      if case .success(let v) = r, let id = v as? String { me.pendingSelect = id; DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { if me.meta.tile(id) != nil { me.activate(id); me.window.makeFirstResponder(me.area) } } }
    }
  }
  private var pendingSelect: String?
  func sidebarTool(_ name: String, from view: NSView) {
    switch name {
    case "palette": showPanel(.palette)
    case "settings": showPanel(.settings)
    case "elsewhere": showPanel(.elsewhere)
    case "past": showPanel(.past)
    default: break
    }
  }

  // ------------------------------------------------------------------ the ask pane (AskPane.swift)
  func sidebarAsks(row: String?) { askPane.tileClick(row: row) }
  func sidebarAsksHover() { askPane.prewarm() }
  /// the palette's ask entries (88-palette.js)
  var asks: AskModel { askPane.model }
  func openAsks(_ mode: String, scrollTo: String? = nil) { askPane.openPane(mode, scrollTo: scrollTo) }
  var askConn: DeckConnection { conn }
  var askDownloadFolders: [String] { conn.isLocal ? (owner?.localHost?.askDownloads ?? []) : [] }
  func askActivate(_ tileId: String) { activate(tileId) }
  func askPaneShown(_ on: Bool) { window.makeFirstResponder(on ? askPane.keyView : area) }
  func askNote(_ text: String, seconds: Double) { note(text, seconds: seconds) }
  func askChanged() { sidebar.updateAsks(askPane.model, paneAll: askPane.mode == "all") }

  // ------------------------------------------------------------------ panels (Panels.swift)
  func showPanel(_ kind: DeckPanelKind) {
    if kind == .asks { askPane.openPane("all"); return }      // the ask pane is not a panel: it is over the terminal area
    closePanel()
    guard let content = window.contentView else { return }
    let ctx = DeckPanelContext(meta: meta, conn: conn, selected: selected, controller: self)
    guard let p = DeckPanels.make(kind, context: ctx) else { areaNote("\(kind.title) is not in the Mac shell yet (mac/PARITY.md)."); return }
    p.view.frame = p.preferredFrame(in: content.bounds); p.view.autoresizingMask = [.minXMargin, .maxXMargin, .minYMargin]
    content.addSubview(p.view)
    panel = p
    p.opened(in: window)
  }
  func closePanel() { panel?.view.removeFromSuperview(); panel = nil; window.makeFirstResponder(askPane.isOpen ? askPane.keyView : area) }

  // ------------------------------------------------------------------ the window
  private func reportWindowState() {
    conn.windowState(visible: window.isVisible && !window.isMiniaturized, focused: window.isKeyWindow)
    conn.windowBounds(window.frame)
  }
  func windowDidBecomeKey(_ n: Notification) { reportWindowState(); area.focusChanged(window.firstResponder === area) }
  func windowDidResignKey(_ n: Notification) { chordAbandon(); reportWindowState(); area.focusChanged(false) }
  func windowDidMiniaturize(_ n: Notification) { reportWindowState(); relayout(); askPane.setMinimised(true) }       // shows nothing, drives nothing
  func windowDidDeminiaturize(_ n: Notification) { reportWindowState(); relayout(); askPane.setMinimised(false) }
  func windowDidMove(_ n: Notification) { conn.windowBounds(window.frame) }
  func windowDidEndLiveResize(_ n: Notification) { conn.windowBounds(window.frame) }
  func splitViewDidResizeSubviews(_ n: Notification) { if !sidebar.isHidden { UserDefaults.standard.set(Double(sidebar.frame.width), forKey: "sidebarWidth") } }
  func splitView(_ s: NSSplitView, constrainMinCoordinate p: CGFloat, ofSubviewAt i: Int) -> CGFloat { Theme.sidebarMinWidth }
  func splitView(_ s: NSSplitView, constrainMaxCoordinate p: CGFloat, ofSubviewAt i: Int) -> CGFloat { s.bounds.width * Theme.sidebarMaxFraction }
  /// Cmd+W / the red button. A LOCAL deck's window closing touches no session (its tiles drop out of that layout and
  /// run on); the LAST one closing leaves the deck in the Dock with every pty alive (main.js on a Mac) — the app
  /// delegate decides that, not this window.
  func windowWillClose(_ n: Notification) {
    reconnect?.invalidate()
    askPane.shutdown()                                         // its web view (and, with the last one, the WebContent process) goes
    conn.close()
    owner?.windowClosed(self)
  }
}

/// a tile menu entry that runs a closure (the ask pane's)
private final class AskMenuAction: NSObject {
  let run: () -> Void
  init(_ run: @escaping () -> Void) { self.run = run }
}

private extension CGFloat {
  func clamped(_ lo: CGFloat, _ hi: CGFloat, or fallback: CGFloat) -> CGFloat { self <= 0 ? fallback : Swift.min(hi, Swift.max(lo, self)) }
}
