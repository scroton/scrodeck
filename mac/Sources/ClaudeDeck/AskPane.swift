// mac/Sources/ClaudeDeck/AskPane.swift — the ask pane (renderer/js/49-asks.js) on the WebKit built into macOS.
//
// Claude sessions ask their questions on ask pages (the ask-page skill: one self-contained, sandboxed HTML page per
// round). The deck sends what waits as meta.asks and what was answered as meta.askLog. The pane is ONE WKWebView over
// the terminal area holding a host document (AskHostPage.swift: 49-asks.js's pane as JavaScript) and, in it, one
// sandboxed <iframe> per page; drafts and Done go to the deck through the window's connection (askDraftGet,
// askDraft, askClear, askDone, askLogState - the same calls the Windows pane and the Electron renderer make), so
// drafts round-trip with the Electron deck, the phone and the other windows.
//
// 2026-11-23: the Mac's ask pane is the system WebKit (light, about 40-80 MB while open), with NO functionality
// dropped against the Windows pane (windows/src/asks.hpp over WebView2). What is where:
//   * AskModel - the deck's asks state as the window needs it while NO web view exists: the asks tile, the marks on
//     the tiles, F7 (Cmd+Shift+A), F6 (Cmd+Shift+Enter) on the every-session view, the tile menu, a clicked toast.
//   * AskPane - the web view's life (made when the pane opens, closed AskPane.graceSeconds after it closes, and
//     after offscreenSeconds in a minimised window), the deck calls, the clipboard, links (never inside the pane),
//     an older page's own Done (a download), guest mode (another deck serves the pages: read-only here).
//   * AskSchemeHandler - serves `deckask://pane/host` (the host document) and `deckask://pane/ask/<id>.html` (a
//     page): from the page FILE for the in-process deck (only files the meta names, only `ask_*.html`), or fetched
//     from a REMOTE deck's /ask/ route by URLSession with the deck's cookie set on the request here. So the token
//     never enters the web view at all (the Windows pane sets it as an HttpOnly cookie in an InPrivate profile).
//     Every page is served under the deck's own sandbox CSP (core/deck/server.cpp kAskCsp) AND framed with the
//     same sandbox attribute.
//
// UNBUILT HERE (written on Windows; no Swift toolchain): the CI builds it (.github/workflows/mac.yml). Nothing of
// the pane's BEHAVIOUR has been seen on a Mac. The host document's JavaScript is run on Windows in headless Edge
// (mac/Tests/askhost-test). Lines whose API shape was not checked are marked VERIFY.
// Windows counterpart: windows/src/asks.hpp (+ windows/web/webpane.*, windows/WEB-NOTES.md). JS mirrored:
// renderer/js/49-asks.js, main.js openAskLink / hookAskDownloads.
import AppKit
import WebKit
import DeckCoreBridge

// ---------------------------------------------------------------------------------------------- the model
/// 49-asks.js's helpers at its head, over the last meta. Values, read defensively (a missing key is a default).
struct AskModel {
  static let recent = "__recent"                                   // the "recently answered" row of the asks tile
  /// lib/platform.js keyHelp for the Mac: the keys this shell answers to (Cmd+Shift+A, Cmd+Shift+Enter), whatever
  /// machine the deck runs on
  static let keyHelp: JSONObject = (JSON.parse(String(cString: db_key_help_json("darwin"))) as? JSONObject) ?? [:]
  static var asksKey: String { keyHelp.str("asks").isEmpty ? "Cmd+Shift+A" : keyHelp.str("asks") }
  static var zoomKey: String { keyHelp.str("zoom").isEmpty ? "Cmd+Shift+Enter" : keyHelp.str("zoom") }

  var asks: [JSONObject] = [], askLog: [JSONObject] = [], tiles: [JSONObject] = [], accounts: [JSONObject] = []
  var cfg: JSONObject = [:]
  var guest = false, guestWhy = ""
  var now: Double = 0

  mutating func apply(_ meta: JSONObject) {
    asks = meta.arr("asks").compactMap { $0 as? JSONObject }
    askLog = meta.arr("askLog").compactMap { $0 as? JSONObject }
    tiles = meta.arr("tiles").compactMap { $0 as? JSONObject }
    accounts = meta.arr("accounts").compactMap { $0 as? JSONObject }
    cfg = meta.obj("settings").obj("asks")
    // meta.askFeed (the native core): `guest` = another deck is serving the ask pages; they show, read-only
    let feed = meta.obj("askFeed")
    guest = feed.str("state") == "guest"
    guestWhy = guest ? (feed.str("reason").isEmpty ? "another deck is serving the ask pages" : feed.str("reason")) : ""
    now = meta.double("now") ?? Date().timeIntervalSince1970 * 1000
  }
  // Settings -> Asks (ASK_DEFAULTS: every one on)
  var showTile: Bool { (cfg["showTile"] as? Bool) != false }
  var showMarks: Bool { (cfg["showMarks"] as? Bool) != false }

  static func rounds(_ s: JSONObject) -> [JSONObject] { s.arr("rounds").compactMap { $0 as? JSONObject } }
  /// js truthiness of a JSON value (`r.sent` is an object or null)
  static func truthy(_ v: Any?) -> Bool {
    guard let v = v, !(v is NSNull) else { return false }
    if let n = v as? NSNumber { return n.doubleValue != 0 }
    if let s = v as? String { return !s.isEmpty }
    return true
  }
  func session(_ sid: String) -> JSONObject? { sid.isEmpty ? nil : asks.first { $0.str("sessionId") == sid } }
  func tile(_ id: String) -> JSONObject? { tiles.first { $0.str("id") == id } }
  func tileOf(_ sid: String) -> JSONObject? { sid.isEmpty ? nil : tiles.first { $0.str("kind") == "claude" && $0.str("sessionId") == sid } }
  func openCount(_ s: JSONObject) -> Int { AskModel.rounds(s).reduce(0) { $0 + max(0, ($1.int("total") ?? 0) - ($1.int("answered") ?? 0)) } }
  /// the deck's drafted counts (the page's own, in progress, reach the deck with askDraft and come back in the meta)
  func drafted(_ s: JSONObject) -> Int { AskModel.rounds(s).reduce(0) { $0 + (AskModel.truthy($1["sent"]) ? 0 : ($1.int("drafted") ?? 0)) } }
  func unsent(_ s: JSONObject) -> Bool { AskModel.rounds(s).contains { !AskModel.truthy($0["sent"]) } }
  func lastLog(_ sid: String) -> JSONObject? { askLog.first { $0.str("sessionId") == sid } }
  static func tileTitle(_ t: JSONObject) -> String {          // 30-tiles.js tileTitle
    let ti = t.str("title"); if !ti.isEmpty { return ti }
    switch t.str("kind") { case "cloud": return "New cloud session"; case "shell": return "Shell"; default: return "New session" }
  }
  func name(_ s: JSONObject) -> String {
    if let t = tileOf(s.str("sessionId")) { return AskModel.tileTitle(t) }
    return s.str("title").isEmpty ? s.str("name") : s.str("title")
  }
  func abbr(ofTile t: JSONObject?) -> String {
    guard let t = t else { return "" }
    return accounts.first { $0.str("id") == t.str("account") }?.str("abbr") ?? ""
  }
  func abbr(_ s: JSONObject) -> String { abbr(ofTile: tileOf(s.str("sessionId"))) }

  /// askMarkInfo: questions waiting (hollow), answers in progress (filled), sent and not yet read (a tick)
  struct Mark: Equatable { let kind: String, text: String, title: String }
  func mark(_ sid: String) -> Mark? {
    guard let s = session(sid) else { return nil }
    let open = openCount(s), d = drafted(s)
    let kind = !unsent(s) ? "sent" : d > 0 ? "drafted" : "waiting"
    let title = kind == "sent" ? "answers sent; the session has not read them yet"
      : kind == "drafted" ? "\(d) of \(open) answered on the page, not sent yet (\(AskModel.asksKey))"
      : "\(open) question\(open == 1 ? "" : "s") waiting on an ask page (\(AskModel.asksKey))"
    return Mark(kind: kind, text: kind == "sent" ? "✓" : String(open), title: title)
  }
  /// the marks for the sidebar's tiles: tile id -> its session's mark (none when Settings -> Asks hides them)
  var tileMarks: [String: Mark] {
    guard showMarks else { return [:] }
    var out: [String: Mark] = [:]
    for t in tiles where t.str("kind") == "claude" { if let m = mark(t.str("sessionId")) { out[t.str("id")] = m } }
    return out
  }
  /// a time the meta gives as ms or as an ISO string
  static func ms(_ v: Any?) -> Double? {
    if let n = v as? NSNumber { return n.doubleValue }
    guard let s = v as? String, !s.isEmpty else { return nil }
    let f = ISO8601DateFormatter(); f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    if let d = f.date(from: s) { return d.timeIntervalSince1970 * 1000 }
    f.formatOptions = [.withInternetDateTime]
    return f.date(from: s).map { $0.timeIntervalSince1970 * 1000 }
  }
  func age(_ v: Any?) -> String { AskModel.ms(v).map { Format.age(ms: now - $0) } ?? "" }
}

/// What the window gives the pane.
protocol AskPaneHost: AnyObject {
  var askConn: DeckConnection { get }
  /// LOCAL: the ask feed's Done folders (db_deck_ask_downloads_json); a remote: none
  var askDownloadFolders: [String] { get }
  /// select a tile (20-terminals.js activate, adopt false)
  func askActivate(_ tileId: String)
  /// the pane came up (the keyboard to the page) or went (the keyboard back to the terminal area)
  func askPaneShown(_ on: Bool)
  /// a note over the terminal area (paneNote)
  func askNote(_ text: String, seconds: Double)
  /// the asks tile and the marks need painting again
  func askChanged()
}

// ---------------------------------------------------------------------------------------------- the pane
final class AskPane: NSObject, WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate {
  /// 2026-11-23: the ask web view goes 5 s after the pane closes (both native decks)
  static let graceSeconds: TimeInterval = 5
  /// open in a minimised window: nobody can see it; closed after this, made again when the window comes back
  static let offscreenSeconds: TimeInterval = 120
  /// the deck calls a page's bridge needs (49-asks.js's `deck.*`); nothing else is passed on
  private static let calls: Set<String> = ["askDraftGet", "askLogState", "askClear", "askDone"]

  weak var host: AskPaneHost?
  let view = AskPaneContainer(frame: .zero)                  // over the terminal area; the web view fills it
  private(set) var model = AskModel()
  private(set) var mode: String?                             // nil = closed, "all", a session id, "log:<page id>"
  private var web: WKWebView?
  private var hostReady = false
  private var lastState = ""                                 // the state the host document has (sent only when it changes)
  private var pendingScroll: String?
  private var idle: Timer?
  private var minimised = false
  private let schemes = AskSchemeHandler()
  private var downloads: [ObjectIdentifier: String] = [:]    // a download in progress -> its file name
  private(set) var webMade = 0, webDropped = 0               // diagnostics: how often the web view was made / closed

  /// `remote`: the deck this window is a desk of (its /ask/ route serves the pages); nil = the in-process deck
  init(remote: DeckLink?) {
    super.init()
    schemes.remote = remote
    view.isHidden = true
    view.onKey = { [weak self] e in if e.keyCode == 53 { self?.closePane() } }       // Esc with no page loaded yet
  }

  var isOpen: Bool { mode != nil }
  var hasWebView: Bool { web != nil }
  /// where the keyboard goes while the pane is open
  var keyView: NSView { if let w = web { return w }; return view }

  // ------------------------------------------------------------------ the deck's state (every meta)
  func setMeta(_ meta: JSONObject) {
    model.apply(meta)
    if schemes.remote == nil { rememberLocalPages() }
    pushState()
    host?.askChanged()
  }
  /// meta's file:// urls -> the page files the in-process deck may serve (only files named like an ask page)
  private func rememberLocalPages() {
    var pages: [String: String] = [:]
    func note(_ id: String, _ url: String) {
      guard !id.isEmpty, url.hasPrefix("file:"), let u = URL(string: url), u.isFileURL else { return }
      let base = u.lastPathComponent
      guard base.hasPrefix("ask_"), base.hasSuffix(".html"), base.count > 9 else { return }
      pages[id] = u.path
    }
    for s in model.asks { for r in AskModel.rounds(s) { note(r.str("id"), r.str("url")) } }
    for e in model.askLog { note(e.str("id"), e.str("url")) }
    schemes.localPages = pages
  }
  private var statePayload: JSONObject {
    [
      "op": "state", "asks": model.asks, "askLog": model.askLog,
      "tiles": model.tiles.map { ["id": $0.str("id"), "kind": $0.str("kind"), "sessionId": $0.str("sessionId"), "title": $0.str("title"), "account": $0.str("account"), "status": $0.str("status")] },
      "accounts": model.accounts.map { ["id": $0.str("id"), "abbr": $0.str("abbr")] },
      "cfg": model.cfg, "kh": ["asks": AskModel.asksKey, "zoom": AskModel.zoomKey],
      "guest": model.guest, "guestWhy": model.guestWhy,
      "remote": schemes.remote != nil, "base": AskSchemeHandler.origin,
    ]
  }
  /// the state, when it differs from what the host document has (a meta every 2 s mostly changes nothing here)
  private func pushState() {
    guard let w = web, hostReady,
          let d = try? JSONSerialization.data(withJSONObject: statePayload, options: [.sortedKeys, .fragmentsAllowed]) else { return }
    let t = String(decoding: d, as: UTF8.self)
    if t == lastState { return }
    lastState = t
    w.evaluateJavaScript("window.deckIn(" + t + ")", completionHandler: nil)
  }
  /// Swift -> the host document (dropped until it is ready: it is sent the whole state then)
  private func send(_ m: JSONObject) {
    guard let w = web, hostReady else { return }
    w.evaluateJavaScript("window.deckIn(" + JSON.text(m) + ")", completionHandler: nil)
  }

  // ------------------------------------------------------------------ open / close (openAskPane / closeAskPane)
  func openPane(_ mode: String, scrollTo: String? = nil) {
    if mode != "all" && !mode.hasPrefix("log:") && model.session(mode) == nil { host?.askNote("No questions from this session.", seconds: 2.5); return }
    self.mode = mode
    idle?.invalidate(); idle = nil
    view.isHidden = false
    if web == nil { ensureWeb() }
    if hostReady { var m: JSONObject = ["op": "open", "mode": mode]; if let s = scrollTo { m["scrollTo"] = s }; send(m) } else { pendingScroll = scrollTo }
    host?.askPaneShown(true)
    host?.askChanged()
  }
  /// `toTile`: the session to go to (Answer in the terminal, Done); `fromPage`: the host document closed itself
  func closePane(toTile: String? = nil, fromPage: Bool = false) {
    guard mode != nil else { return }
    mode = nil; pendingScroll = nil
    if !fromPage { send(["op": "close"]) }
    view.isHidden = true
    armIdle()
    if let t = toTile, !t.isEmpty { host?.askActivate(t) }
    host?.askPaneShown(false)
    host?.askChanged()
  }

  // ------------------------------------------------------------------ keys and clicks (asksZoom, askOpenFor, asksKey …)
  /// F6 (Cmd+Shift+Enter): true = the asks had it (25-layout.js asks them first)
  func asksZoom() -> Bool { if mode == "all" { closePane(); return true }; return false }
  /// a session's own: its waiting ask; once that is answered, its last answers read-only, until it asks again
  @discardableResult func openFor(tileId: String?) -> Bool {
    guard let id = tileId, let t = model.tile(id), t.str("kind") == "claude" else { return false }
    let sid = t.str("sessionId")
    if model.session(sid) != nil { openPane(sid); return true }
    if let last = model.lastLog(sid) { openPane("log:" + last.str("id")); return true }
    return false
  }
  /// F7 / Cmd+Shift+A (activeTileId = the window's selection)
  func asksKey(activeTileId: String?) {
    if isOpen { closePane(); return }
    if openFor(tileId: activeTileId) { return }
    let n = model.asks.count
    host?.askNote(n > 0 ? "This session has no questions waiting. \(n) other session\(n == 1 ? " has" : "s have"): the asks tile at the top of the sidebar." : "No questions waiting.", seconds: 2.5)
  }
  /// a new-ask toast was clicked (main.js notifyNewAsks -> asks:open): that session's tile, then its questions
  func onAskOpen(sessionId: String, tileId: String) {
    if !tileId.isEmpty, model.tile(tileId) != nil { host?.askActivate(tileId) }
    openPane(model.session(sessionId) != nil ? sessionId : "all")
  }
  /// a session tile's right-click (35-groups.js askMenuItem): its questions, or its last answers
  func menuItem(tileId: String) -> (label: String, run: () -> Void)? {
    guard let t = model.tile(tileId), t.str("kind") == "claude" else { return nil }
    let sid = t.str("sessionId"), key = AskModel.asksKey
    if let s = model.session(sid) {
      let n = model.openCount(s)
      return ("Answer its question\(n == 1 ? "" : "s") (\(n))  \(key)", { [weak self] in self?.host?.askActivate(tileId); self?.openPane(sid) })
    }
    if let last = model.lastLog(sid) {
      let lid = last.str("id")
      return ("Its last answers (read-only)  \(key)", { [weak self] in self?.host?.askActivate(tileId); self?.openPane("log:" + lid) })
    }
    return nil
  }
  /// a click on the asks tile: `row` = a session id, AskModel.recent, or nil for the tile itself. Every session's
  /// asks at once, scrolled to the row clicked; the same click on the tile while that view is open closes it.
  func tileClick(row: String?) {
    if row == nil && mode == "all" { closePane(); return }
    openPane("all", scrollTo: row)
  }
  /// Esc while the window (not the page) has the keyboard: the pane's own keydown. true = taken
  func key(_ e: NSEvent) -> Bool { if isOpen && e.keyCode == 53 { closePane(); return true }; return false }

  // ------------------------------------------------------------------ the web view's life
  /// The window is minimised: nobody can see the page; it is closed after offscreenSeconds and made again (the
  /// drafts are the deck's: nothing typed is lost) when the window comes back.
  func setMinimised(_ on: Bool) {
    guard on != minimised else { return }
    minimised = on
    if on { armIdle(); return }
    idle?.invalidate(); idle = nil
    if isOpen && web == nil { ensureWeb() }
  }
  /// the window is closing / the app is going
  func shutdown() { mode = nil; dropWeb() }
  /// The web view made a moment ahead of an open, on a real sign of intent - the pointer resting on the asks tile
  /// (windows/src/asks.hpp prewarm): the page process starts and the host document loads, nothing shows. Not opened
  /// within the grace, it goes again. Never because a session happens to be asking.
  func prewarm() {
    guard web == nil, !isOpen, !minimised else { return }
    ensureWeb()
    armIdle()
  }

  private func ensureWeb() {
    guard web == nil else { return }
    let cfg = WKWebViewConfiguration()
    cfg.websiteDataStore = .nonPersistent()                  // nothing of the pane reaches disk (the pages' origins are opaque anyway)
    cfg.setURLSchemeHandler(schemes, forURLScheme: AskSchemeHandler.scheme)
    cfg.userContentController.add(WeakScriptHandler(self), name: "deck")
    cfg.preferences.javaScriptCanOpenWindowsAutomatically = false
    let w = WKWebView(frame: view.bounds, configuration: cfg)
    w.autoresizingMask = [.width, .height]
    w.setValue(false, forKey: "drawsBackground")             // VERIFY: the container's dark colour shows until the page paints (no white flash)
    w.allowsBackForwardNavigationGestures = false
    w.allowsMagnification = false
    w.navigationDelegate = self; w.uiDelegate = self
    view.addSubview(w)
    web = w; hostReady = false; lastState = ""; webMade += 1
    w.load(URLRequest(url: AskSchemeHandler.hostURL))
  }
  private func dropWeb() {
    idle?.invalidate(); idle = nil
    guard let w = web else { return }
    web = nil; hostReady = false; lastState = ""; webDropped += 1
    w.stopLoading()
    w.configuration.userContentController.removeScriptMessageHandler(forName: "deck")
    w.navigationDelegate = nil; w.uiDelegate = nil
    w.removeFromSuperview()                                  // with the last web view of the process, its WebContent process goes
  }
  /// nobody can see the web view (the pane is closed, or open in a minimised window): closed after the grace
  private func armIdle() {
    idle?.invalidate(); idle = nil
    guard web != nil, !(isOpen && !minimised) else { return }
    idle = Timer.scheduledTimer(withTimeInterval: isOpen ? AskPane.offscreenSeconds : AskPane.graceSeconds, repeats: false) { [weak self] _ in
      guard let me = self, !(me.isOpen && !me.minimised) else { return }
      me.dropWeb()
    }
  }

  // ------------------------------------------------------------------ the host document's messages
  func userContentController(_ c: WKUserContentController, didReceive message: WKScriptMessage) {
    // ONLY the host document: the main frame at deckask://pane. A framed page is sandboxed (an opaque origin, not the
    // main frame) and reaches the deck only through the document's relay, which checks it.
    let origin = message.frameInfo.securityOrigin
    guard message.webView === web, message.frameInfo.isMainFrame, origin.protocol == AskSchemeHandler.scheme, origin.host == AskSchemeHandler.hostName,
          let m = message.body as? [String: Any] else { return }
    switch m.str("ev") {
    case "ready":
      hostReady = true; lastState = ""
      pushState()
      if let mode = mode { var o: JSONObject = ["op": "open", "mode": mode]; if let s = pendingScroll { o["scrollTo"] = s }; send(o) }
      pendingScroll = nil
    case "call":
      deckCall(m)
    case "fire":
      // askDraft only (49-asks.js: a send with no answer), and never as a guest (the deck refuses it there)
      guard m.str("fn") == "askDraft", !model.guest else { return }
      host?.askConn.fire("askDraft", m.arr("args"))
    case "closed":
      closePane(toTile: m.str("toTile"), fromPage: true)
    case "mode":                                             // the page it showed was replaced, or read: what it shows now
      let md = m.str("mode")
      if !md.isEmpty, mode != nil { mode = md; host?.askChanged() }
    case "note":
      host?.askNote(m.str("text"), seconds: (m.double("ms") ?? 2500) / 1000)
    case "clipboard":                                        // Copy all (deck.writeClipboard)
      NSPasteboard.general.clearContents(); NSPasteboard.general.setString(m.str("text"), forType: .string)
    default: break
    }
  }
  private func deckCall(_ m: JSONObject) {
    guard let rid = m.int("rid") else { return }
    let fn = m.str("fn")
    guard AskPane.calls.contains(fn), let conn = host?.askConn else { send(["op": "reply", "rid": rid, "ok": false, "error": "not an ask pane call"]); return }
    let asked = web
    conn.call(fn, m.arr("args")) { [weak self] r in
      guard let me = self, me.web === asked else { return }  // the answer of a web view that has gone since
      switch r {
      case .success(let v): me.send(["op": "reply", "rid": rid, "ok": true, "value": v ?? NSNull()])
      case .failure(let e): me.send(["op": "reply", "rid": rid, "ok": false, "error": e.message])
      }
    }
  }

  // ------------------------------------------------------------------ what may load: the host page, the deck's pages; links go out
  func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
    if action.shouldPerformDownload { decisionHandler(.download); return }   // an older page's own Done (a blob with `download`)
    guard let url = action.request.url else { decisionHandler(.cancel); return }
    let main = action.targetFrame?.isMainFrame ?? false
    if url.scheme == AskSchemeHandler.scheme && url.host == AskSchemeHandler.hostName {
      // the top document is the host page and nothing else; a frame is an ask page of this deck
      decisionHandler((main ? url.path == "/host" : AskSchemeHandler.pageId(url) != nil) ? .allow : .cancel)
      return
    }
    if !main && url.scheme == "about" { decisionHandler(.allow); return }     // a frame's about:blank before its page
    // anything else is a link out (main.js openAskLink): never inside the pane
    if action.navigationType == .linkActivated || action.targetFrame == nil { linkClicked(url) }
    decisionHandler(.cancel)
  }
  func webView(_ webView: WKWebView, decidePolicyFor response: WKNavigationResponse, decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
    decisionHandler(response.canShowMIMEType ? .allow : .download)
  }
  /// a link with target=_blank / window.open on a page
  func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
    if let u = action.request.url { linkClicked(u) }
    return nil
  }
  /// the page's renderer died: made again while the pane is open (the drafts are the deck's)
  func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
    guard webView === web else { return }
    dropWeb()
    if isOpen && !minimised { ensureWeb() }
  }
  /// main.js openAskLink. LOCAL: the DECK decides (`askOpenLink`: http(s) in the default browser, a file in its own
  /// program, something that would RUN only shown in its folder). A REMOTE: http(s) in this Mac's default browser and
  /// nothing else (a file: link names a file on the deck's machine).
  private func linkClicked(_ url: URL) {
    guard let conn = host?.askConn else { return }
    if conn.isLocal {
      conn.call("askOpenLink", [url.absoluteString]) { [weak self] r in if case .failure(let e) = r { self?.host?.askNote(e.message, seconds: 3) } }
      return
    }
    if let s = url.scheme?.lowercased(), s == "http" || s == "https" { NSWorkspace.shared.open(url) }
  }

  // ------------------------------------------------------------------ an older page's own Done (main.js hookAskDownloads)
  func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) { download.delegate = self }
  func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) { download.delegate = self }
  func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String, completionHandler: @escaping (URL?) -> Void) {
    guard let to = downloadDestination(suggestedFilename) else { completionHandler(nil); return }   // nil = cancelled
    try? FileManager.default.removeItem(at: to)              // the same answers file again: replaced, as Electron's setSavePath does
    downloads[ObjectIdentifier(download)] = suggestedFilename
    completionHandler(to)
  }
  func downloadDidFinish(_ download: WKDownload) {
    if let n = downloads.removeValue(forKey: ObjectIdentifier(download)) { host?.askNote("Answers saved: \(n)", seconds: 2.5) }
  }
  func download(_ download: WKDownload, didFailWithError error: Error, resumeData: Data?) { downloads.removeValue(forKey: ObjectIdentifier(download)) }
  /// `ask_<id>.answers.json` (or .txt) is saved, with no dialog, in the first of the ask feed's Done folders that
  /// exists - where the skill's `wait` looks. LOCAL only: on a remote the file would land on the wrong machine (the
  /// page's own Copy all works). Anything else a page downloads is cancelled (Electron would show a Save dialog).
  func downloadDestination(_ name: String) -> URL? {
    guard let h = host, h.askConn.isLocal, AskPane.isAnswersFile(name) else { return nil }
    for dir in h.askDownloadFolders {
      var isDir: ObjCBool = false
      if FileManager.default.fileExists(atPath: dir, isDirectory: &isDir), isDir.boolValue { return URL(fileURLWithPath: dir).appendingPathComponent(name) }
    }
    return nil
  }
  static func isAnswersFile(_ n: String) -> Bool { n.range(of: "^ask_[A-Za-z0-9_-]+\\.answers(\\.json|\\.txt)$", options: .regularExpression) != nil }
}

/// The pane's place over the terminal area: dark until the page paints; nothing under it takes a click or a wheel
/// turn (49-asks.js: the pane is over the terminals, the terminals keep running behind it).
final class AskPaneContainer: NSView {
  var onKey: ((NSEvent) -> Void)?
  override init(frame: NSRect) { super.init(frame: frame); wantsLayer = true; layer?.backgroundColor = Theme.bg.cgColor }
  required init?(coder: NSCoder) { fatalError("not used") }
  override var isFlipped: Bool { true }
  override var acceptsFirstResponder: Bool { true }
  override func keyDown(with event: NSEvent) { onKey?(event) }           // the pane has the keyboard before its page is up
  override func mouseDown(with event: NSEvent) {}
  override func mouseUp(with event: NSEvent) {}
  override func mouseDragged(with event: NSEvent) {}
  override func rightMouseDown(with event: NSEvent) {}
  override func otherMouseDown(with event: NSEvent) {}
  override func scrollWheel(with event: NSEvent) {}
}

/// WKUserContentController keeps its handlers strongly: this breaks the pane <-> web view cycle.
private final class WeakScriptHandler: NSObject, WKScriptMessageHandler {
  weak var target: WKScriptMessageHandler?
  init(_ t: WKScriptMessageHandler) { target = t }
  func userContentController(_ c: WKUserContentController, didReceive m: WKScriptMessage) { target?.userContentController(c, didReceive: m) }
}

// ---------------------------------------------------------------------------------------------- what the web view loads
/// `deckask://pane/host` = the host document; `deckask://pane/ask/<id>.html` = an ask page, from its file (the
/// in-process deck has no /ask/ route: windows/src/asks.hpp localPage does the same) or from a remote deck's /ask/
/// route (lib/phone.js / core/deck/server.cpp), the deck's cookie set on the request HERE - never in the web view.
final class AskSchemeHandler: NSObject, WKURLSchemeHandler {
  static let scheme = "deckask", hostName = "pane"
  static let origin = "deckask://pane"
  static let hostURL = URL(string: "deckask://pane/host")!
  /// core/deck/server.cpp kAskCsp (lib/phone.js): what every ask page is served under
  static let pageCsp = "sandbox allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads; default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:"
  /// the host document's own (windows/src/asks.hpp: the same, with this scheme for the frames)
  static let hostCsp = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-src 'self' deckask:; img-src data:; base-uri 'none'; form-action 'none'"   // VERIFY: WebKit applies a scheme handler's CSP header, and 'self' / deckask: match the frames
  var localPages: [String: String] = [:]                     // page id -> its file (LOCAL; from the meta's file:// urls)
  var remote: DeckLink?
  private var live = Set<ObjectIdentifier>()                 // tasks started and neither finished nor stopped
  private lazy var session: URLSession = {
    let c = URLSessionConfiguration.ephemeral
    c.httpShouldSetCookies = false; c.httpCookieAcceptPolicy = .never; c.urlCache = nil; c.requestCachePolicy = .reloadIgnoringLocalCacheData
    return URLSession(configuration: c)
  }()

  /// "/ask/ask_x.html" -> "ask_x" (a page id, never a path)
  static func pageId(_ url: URL) -> String? {
    let p = url.path
    guard p.hasPrefix("/ask/"), p.hasSuffix(".html") else { return nil }
    let id = String(p.dropFirst(5).dropLast(5))
    return id.range(of: "^ask_[A-Za-z0-9_-]+$", options: .regularExpression) != nil ? id : nil
  }

  func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
    live.insert(ObjectIdentifier(task as AnyObject))
    let text = ["Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store"]
    guard let url = task.request.url, url.host == AskSchemeHandler.hostName else { finish(task, 404, text, Data("not found".utf8)); return }
    if url.path == "/host" {
      finish(task, 200, ["Content-Type": "text/html; charset=utf-8", "Content-Security-Policy": AskSchemeHandler.hostCsp, "Cache-Control": "no-store",
                         "Referrer-Policy": "no-referrer", "X-Content-Type-Options": "nosniff"], Data(AskHostPage.html.utf8))
      return
    }
    guard let id = AskSchemeHandler.pageId(url) else { finish(task, 404, text, Data("not found".utf8)); return }
    let page = ["Content-Type": "text/html; charset=utf-8", "Content-Security-Policy": AskSchemeHandler.pageCsp, "Cache-Control": "no-store"]
    if let link = remote { fetch(task, id: id, link: link, headers: page); return }
    guard let path = localPages[id], let data = AskSchemeHandler.readPage(path) else { finish(task, 404, text, Data("no such ask page".utf8)); return }
    finish(task, 200, page, data)
  }
  func webView(_ webView: WKWebView, stop task: WKURLSchemeTask) { live.remove(ObjectIdentifier(task as AnyObject)) }

  private static func readPage(_ path: String) -> Data? {
    guard let a = try? FileManager.default.attributesOfItem(atPath: path), let size = a[.size] as? NSNumber, size.intValue <= 32 << 20 else { return nil }
    return try? Data(contentsOf: URL(fileURLWithPath: path))
  }
  /// a remote deck's /ask/<id>.html (?embed=deck&theme=dark, as the browser desk frames it), authorised by its
  /// cookie on this request (`deck_phone=<encodeURIComponent(token)>`); the deck's answer is passed on under the
  /// deck's own sandbox CSP, never with a Set-Cookie
  private func fetch(_ task: WKURLSchemeTask, id: String, link: DeckLink, headers: [String: String]) {
    var c = URLComponents(); c.scheme = link.scheme == "ws" ? "http" : "https"; c.host = link.host; c.port = link.port
    c.path = "/ask/\(id).html"; c.query = "embed=deck&theme=dark"
    guard let url = c.url else { finish(task, 404, [:], Data()); return }
    var req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 20)
    req.httpShouldHandleCookies = false
    var allowed = CharacterSet.alphanumerics; allowed.insert(charactersIn: "-_.!~*'()")
    req.setValue("deck_phone=" + (link.token.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""), forHTTPHeaderField: "Cookie")
    session.dataTask(with: req) { [weak self] data, response, error in
      DispatchQueue.main.async {
        guard let me = self else { return }
        guard error == nil, let http = response as? HTTPURLResponse else {
          me.finish(task, 502, ["Content-Type": "text/plain; charset=utf-8"], Data("the deck did not answer".utf8)); return
        }
        me.finish(task, http.statusCode, headers, data ?? Data())
      }
    }.resume()
  }
  private func finish(_ task: WKURLSchemeTask, _ status: Int, _ headers: [String: String], _ body: Data) {
    // a task WebKit has stopped must not be answered (it raises)
    guard live.remove(ObjectIdentifier(task as AnyObject)) != nil, let url = task.request.url,
          let r = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers) else { return }
    task.didReceive(r)
    if !body.isEmpty { task.didReceive(body) }
    task.didFinish()
  }
}
