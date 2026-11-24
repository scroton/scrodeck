// mac/Sources/ClaudeDeck/Panels.swift — the panels that open over a deck window: ONE structure for all of them,
// ONE of them fully worked (the palette), the rest named and listed so each is a file-sized job with a place to go.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain).
// Windows counterpart: windows/src/overlays.hpp (menus, the palette, Elsewhere / Past, move-all), settings.hpp,
// asks.hpp, cloud.hpp. JS mirrored: renderer/js/88-palette.js (worked here), 45-elsewhere.js, 47-past.js,
// 50-settings.js, 60-move.js, 49-asks.js, 48-cloud.js (each: mac/PARITY.md "not started").
//
// HOW A PANEL IS BUILT (follow the palette):
//   - a class conforming to DeckPanel; its view is an AppKit view styled from Theme (style.css's numbers);
//   - everything it does to the deck is a `conn.call(...)` from core/PROTOCOL.md's call table — a panel never
//     reaches into the window's state except through DeckPanelContext;
//   - the window gives it the keys first while it is open (handleKey), Esc always closes it;
//   - web content (ask pages, cloud panes) is a WKWebView inside a panel: see WebPanel at the bottom.
import AppKit
import WebKit

enum DeckPanelKind {
  case palette, settings, elsewhere, past, moveAll, asks, specialPrompt, cloud
  var title: String {
    switch self {
    case .palette: return "The palette"; case .settings: return "Settings"; case .elsewhere: return "Elsewhere"; case .past: return "Past sessions"
    case .moveAll: return "Move every session"; case .asks: return "The ask pane"; case .specialPrompt: return "Special prompts"; case .cloud: return "Cloud sessions"
    }
  }
}

struct DeckPanelContext {
  let meta: DeckMeta
  let conn: DeckConnection
  let selected: String?
  weak var controller: DeckWindowController?
}

protocol DeckPanel: AnyObject {
  var view: NSView { get }
  func preferredFrame(in bounds: CGRect) -> CGRect
  func opened(in window: NSWindow)
  /// A key-down while the panel is open: true = the panel used it (the terminal never sees keys while a panel is open)
  func handleKey(_ event: NSEvent) -> Bool
}

enum DeckPanels {
  static func make(_ kind: DeckPanelKind, context: DeckPanelContext) -> DeckPanel? {
    switch kind {
    case .palette: return PalettePanel(context: context)
    // NOT STARTED — each returns nil and the window says so in a note (mac/PARITY.md has the rows):
    //   .settings       50-settings.js: every section from `getSettings`, saved with `saveSettings` / `saveAccounts`
    //   .elsewhere      45-elsewhere.js: `externalSessions` -> rows -> `adoptSession`
    //   .past           47-past.js: `pastSessions` -> filter -> `adoptSession`
    //   .moveAll        60-move.js: `moveAll(from, to, continueIfUsage, onlyIds)`
    //   .specialPrompt  30-tiles.js renderHoldForm: `holdAdd` / `holdRemove`, `triggersList`
    //   (.asks is not a panel: the ask pane is over the terminal area - AskPane.swift; showPanel(.asks) opens it)
    //   .cloud          48-cloud.js: WebPanel per cloud pane, one WKWebsiteDataStore per account
    default: return nil
    }
  }
}

// ---------------------------------------------------------------------------------------------- the palette (worked)
/// Cmd+Shift+P (88-palette.js): every session, "new", what can be done to the selected session, the deck's own
/// actions — one list, filtered as you type, arrows + Enter, Esc closes.
final class PalettePanel: NSObject, DeckPanel, NSTextFieldDelegate, NSTableViewDataSource, NSTableViewDelegate {
  struct Row { let section: String; let title: String; let hint: String; let run: () -> Void }
  let view: NSView = NSView()
  private let field = NSTextField()
  private let table = NSTableView()
  private let scroll = NSScrollView()
  private var all: [Row] = [], rows: [Row] = []
  private let ctx: DeckPanelContext

  init(context: DeckPanelContext) {
    ctx = context
    super.init()
    view.wantsLayer = true
    view.layer?.backgroundColor = Theme.menuBg.cgColor; view.layer?.borderColor = Theme.menuBorder.cgColor; view.layer?.borderWidth = 1
    view.shadow = { let s = NSShadow(); s.shadowColor = NSColor.black.withAlphaComponent(0.66); s.shadowBlurRadius = 24; s.shadowOffset = NSSize(width: 0, height: -8); return s }()
    field.placeholderString = "Type to filter…"; field.font = NSFont.systemFont(ofSize: 14); field.isBordered = false; field.focusRingType = .none
    field.backgroundColor = Theme.sidebar; field.textColor = Theme.fg; field.drawsBackground = true; field.delegate = self
    let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("row")); col.resizingMask = .autoresizingMask
    table.addTableColumn(col); table.headerView = nil; table.backgroundColor = .clear; table.rowHeight = 24; table.intercellSpacing = .zero
    table.dataSource = self; table.delegate = self; table.target = self; table.doubleAction = #selector(runSelected); table.action = #selector(runSelected)
    table.selectionHighlightStyle = .regular
    scroll.documentView = table; scroll.drawsBackground = false; scroll.hasVerticalScroller = true
    for v in [field, scroll] as [NSView] { v.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(v) }
    NSLayoutConstraint.activate([
      field.topAnchor.constraint(equalTo: view.topAnchor, constant: 6), field.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 6),
      field.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -6), field.heightAnchor.constraint(equalToConstant: 28),
      scroll.topAnchor.constraint(equalTo: field.bottomAnchor, constant: 4), scroll.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 3),
      scroll.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -3), scroll.bottomAnchor.constraint(equalTo: view.bottomAnchor, constant: -3),
    ])
    build()
    filter("")
  }

  private func build() {
    let c = ctx.controller, conn = ctx.conn, meta = ctx.meta
    let close: () -> Void = { [weak c] in c?.closePanel() }
    func row(_ section: String, _ title: String, _ hint: String = "", _ run: @escaping () -> Void) { all.append(Row(section: section, title: title, hint: hint, run: { close(); run() })) }
    for t in meta.ordered {
      let acct = t.isShell ? "terminal" : (meta.account(t.account)?.label ?? t.account)
      row("Sessions", t.displayTitle, acct + (t.asleep ? " · asleep" : "")) { [weak c] in c?.activate(t.id) }
    }
    row("New", "New terminal") { [weak c] in c?.sidebarNewTerminal() }
    for a in meta.accounts { row("New", "New \(a.label) session") { [weak c] in c?.sidebarNewSession(account: a.id) } }
    if let id = ctx.selected, let t = meta.tile(id) {
      row("This session", "Rename") { [weak c] in c?.beginRename(id) }
      row("This session", t.pinned ? "Unpin" : "Pin") { conn.call("pin", [id, !t.pinned]) }
      if !t.isShell {
        row("This session", t.asleep ? "Wake" : "Sleep") { conn.call(t.asleep ? "wakeTile" : "sleepTile", [id]) }
        row("This session", "Refresh") { conn.call("refreshTile", [id]) }
      }
      row("This session", "Fill the window") { [weak c] in c?.sidebarPlace(id) }
      row("This session", "Open in a new window") { [weak c] in c?.owner?.openWindow(holding: id, at: nil) }
    }
    if let wc = c {                                           // the ask pane's entries (88-palette.js)
      let am = wc.asks
      let curSid = ctx.selected.flatMap { am.tile($0) }.map { $0.str("kind") == "claude" ? $0.str("sessionId") : "" } ?? ""
      if !am.asks.isEmpty { row("Asks", "Answer every session's questions (\(am.asks.count) waiting)", "the asks tile") { [weak c] in c?.openAsks("all") } }
      if am.session(curSid) != nil { row("Asks", "Answer this session's questions", AskModel.asksKey) { [weak c] in c?.openAsks(curSid) } }
      for s in am.asks where s.str("sessionId") != curSid {
        let sid = s.str("sessionId")
        row("Asks", "Answer \(am.name(s))", "\(am.openCount(s)) waiting") { [weak c] in c?.openAsks(sid) }
      }
      if !am.askLog.isEmpty { row("Asks", "Recently answered", "the last \(am.askLog.count), read-only") { [weak c] in c?.openAsks("all", scrollTo: AskModel.recent) } }
    }
    let help = meta.raw.obj("platform").obj("keyHelp")       // the deck's own key names, when the meta carries them
    row("Deck", "Settings", help.str("settings").isEmpty ? "Cmd+," : help.str("settings")) { [weak c] in c?.showPanel(.settings) }
    row("Deck", "Elsewhere…") { [weak c] in c?.showPanel(.elsewhere) }
    row("Deck", "Past sessions…") { [weak c] in c?.showPanel(.past) }
    if !Edition.isPublic { row("Deck", "Move every session…") { [weak c] in c?.showPanel(.moveAll) } }   // the public build has no move-all
    row("Deck", "Restart the deck when quiet") { conn.call("restartWhenQuiet", [true, "manual"]) }
    row("Deck", "Refresh usage") { conn.call("refreshUsage", []) }
  }
  private func filter(_ q: String) {
    let needle = q.lowercased().trimmingCharacters(in: .whitespaces)
    rows = needle.isEmpty ? all : all.filter { ($0.title + " " + $0.hint + " " + $0.section).lowercased().contains(needle) }
    table.reloadData()
    if !rows.isEmpty { table.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false) }
  }

  func preferredFrame(in b: CGRect) -> CGRect {            // css .palette: 560 px wide, centred, a little down from the top
    let w = min(560, b.width - 40), h = min(420, b.height - 80)
    return CGRect(x: (b.width - w) / 2, y: b.height - h - 60, width: w, height: h)
  }
  func opened(in window: NSWindow) { window.makeFirstResponder(field) }
  func handleKey(_ event: NSEvent) -> Bool { false }       // the field is first responder: the keys arrive as its commands, below

  func controlTextDidChange(_ obj: Notification) { filter(field.stringValue) }
  func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
    switch selector {
    case #selector(NSResponder.moveDown(_:)): move(1); return true
    case #selector(NSResponder.moveUp(_:)): move(-1); return true
    case #selector(NSResponder.insertNewline(_:)): runSelected(); return true
    case #selector(NSResponder.cancelOperation(_:)): ctx.controller?.closePanel(); return true
    default: return false
    }
  }
  private func move(_ d: Int) {
    guard !rows.isEmpty else { return }
    let i = max(0, min(rows.count - 1, table.selectedRow + d))
    table.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false); table.scrollRowToVisible(i)
  }
  @objc private func runSelected() { let i = table.selectedRow; if i >= 0 && i < rows.count { rows[i].run() } }

  func numberOfRows(in tableView: NSTableView) -> Int { rows.count }
  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    let r = rows[row]
    let s = NSMutableAttributedString(string: r.section + "  ", attributes: [.font: Theme.subFont, .foregroundColor: Theme.fgFaint])
    s.append(NSAttributedString(string: r.title, attributes: [.font: Theme.menuFont, .foregroundColor: Theme.fg]))
    if !r.hint.isEmpty { s.append(NSAttributedString(string: "   " + r.hint, attributes: [.font: Theme.subFont, .foregroundColor: Theme.fgDim])) }
    let label = NSTextField(labelWithAttributedString: s); label.lineBreakMode = .byTruncatingTail
    return label
  }
}

// ---------------------------------------------------------------------------------------------- web content (structure only)
/// A WKWebView in a panel or over a pane, for the cloud panes (claude.ai). STRUCTURE ONLY — nothing constructs one yet.
/// (The ask pane is built: AskPane.swift, its own WKWebView with a scheme handler for the host page and the pages.)
/// The Windows shell's rules that must hold here too (windows/WEB-NOTES.md, windows/src/asks.hpp, cloud.hpp):
///   - no web view process unless an ask or cloud pane is ON SCREEN; closed after a grace period off screen;
///   - cloud: one persistent WKWebsiteDataStore per account (one login per account, kept), a navigation policy
///     that only lets claude.ai / anthropic.com load (main.js guardCloudView);
///   - asks: a NON-persistent store, the page sandboxed by the server's CSP (core/deck/server.hpp kAskCsp), a
///     WKScriptMessageHandler for the pages' bridge (ready, change, height, answers);
///   - while the page has the keyboard the window's own keys still work (DeckWindowController.areaWindowKey from a
///     local key monitor), and Esc with the keyboard on the pane itself closes the pane.
final class WebPanel: NSObject, WKNavigationDelegate {
  let web: WKWebView
  private let allowedHosts: [String]
  /// `persistentStore`: an account's identifier for a cloud pane (macOS 14+: WKWebsiteDataStore(forIdentifier:));
  /// nil = non-persistent (ask pages).
  init(allowedHosts: [String], persistentStore: UUID?) {
    self.allowedHosts = allowedHosts
    let cfg = WKWebViewConfiguration()
    if let id = persistentStore, #available(macOS 14.0, *) { cfg.websiteDataStore = WKWebsiteDataStore(forIdentifier: id) }    // VERIFY: the initialiser's name
    else { cfg.websiteDataStore = .nonPersistent() }
    web = WKWebView(frame: .zero, configuration: cfg)
    super.init()
    web.navigationDelegate = self
  }
  func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
    let host = navigationAction.request.url?.host ?? ""
    let ok = allowedHosts.contains { host == $0 || host.hasSuffix("." + $0) }
    if !ok, navigationAction.navigationType == .linkActivated, let u = navigationAction.request.url { NSWorkspace.shared.open(u) }    // a link out: the default browser
    decisionHandler(ok ? .allow : .cancel)
  }
}
