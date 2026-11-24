// mac/Sources/ClaudeDeck/DeckConnection.swift — one window's connection to a deck. The window's code is the same
// in both modes; only the transport differs:
//   LOCAL   a client of the in-process deck (DeckHost): frames are direct calls into core/deck, never serialised
//           on the way in; `data` arrives as raw bytes
//   REMOTE  a desk of a deck on another machine: the same frames as websocket text (URLSessionWebSocketTask) to
//           wss://<deck>/desk/ws?t=<token>, exactly what renderer/desk/bridge.js speaks
// Either way every frame is BUILT by core/proto/frames.hpp (through the bridge) and arrives as one DBFrame.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain).
// Windows counterpart: windows/src/ws.hpp (the WinHTTP websocket) + the WM_WS handler and `call` / `fire` in
// windows/src/app.cpp; local mode's side is windows/src/deckhost.hpp. JS mirrored: renderer/desk/bridge.js, preload.js.
import AppKit
import DeckCoreBridge

protocol DeckConnectionDelegate: AnyObject {
  /// The transport is up: call `deskInit` now (a socket's `open`; a local window gets it at once).
  func connectionOpened(_ c: DeckConnection)
  /// REMOTE only: the socket closed or failed. The window shows the "deck is away" banner and may reconnect.
  func connectionClosed(_ c: DeckConnection, reason: String)
  func connection(_ c: DeckConnection, data id: String, bytes: UnsafePointer<CChar>, count: Int, replay: Bool)
  func connection(_ c: DeckConnection, meta: JSONObject)
  func connection(_ c: DeckConnection, exit id: String, code: Int)
  func connection(_ c: DeckConnection, reset id: String)
  func connection(_ c: DeckConnection, event name: String, payload: Any?)
}

struct DeckError: Error { let message: String }

/// A deck link as the phone page shows it: https://host[:port]/?t=<token> (or http://100.x.y.z:port/?t=…).
struct DeckLink {
  var scheme = "wss", host = "", port: Int?, token = ""
  init?(_ text: String) {
    guard let u = URLComponents(string: text.trimmingCharacters(in: .whitespacesAndNewlines)), let h = u.host, !h.isEmpty else { return nil }
    host = h; port = u.port
    scheme = (u.scheme == "http" || u.scheme == "ws") ? "ws" : "wss"
    token = u.queryItems?.first(where: { $0.name == "t" })?.value ?? ""
    if token.isEmpty { return nil }
  }
  /// windows/src/ws.hpp: the desk's socket path, the token in the query
  var socketURL: URL? {
    var c = URLComponents(); c.scheme = scheme; c.host = host; c.port = port; c.path = "/desk/ws"
    c.queryItems = [URLQueryItem(name: "t", value: token)]
    return c.url
  }
}

final class DeckConnection: NSObject, URLSessionWebSocketDelegate {
  weak var delegate: DeckConnectionDelegate?
  private var conn: OpaquePointer?                       // DBConn*
  private var nextRid: Int32 = 1
  private var pending: [Int32: (Result<Any?, DeckError>) -> Void] = [:]
  let isLocal: Bool

  // remote
  private var session: URLSession?
  private var task: URLSessionWebSocketTask?
  private var closed = false

  private var sink: DBSink {
    DBSink(ctx: Unmanaged.passUnretained(self).toOpaque(), frame: { ctx, f in
      guard let ctx = ctx, let f = f else { return }
      Unmanaged<DeckConnection>.fromOpaque(ctx).takeUnretainedValue().onFrame(f.pointee)
    })
  }

  // ------------------------------------------------------------------ making one
  /// A window of the local deck. `tileId` non-nil = F5 `window:open` (a new window holding that tile alone).
  /// `adoptClient` non-zero = the window for a client the core registered itself (the on-call's desk).
  init?(local host: DeckHost, boundsJSON: String?, tileId: String? = nil, adoptClient: Int32 = 0) {
    isLocal = true
    super.init()
    guard let deck = host.deck else { return nil }
    var s = sink
    if adoptClient != 0 { conn = db_deck_adopt_window(deck, adoptClient, &s) }
    else if let tile = tileId { conn = db_deck_open_window(deck, tile, boundsJSON, &s) }
    else { conn = db_deck_add_window(deck, boundsJSON, &s) }
    if conn == nil { return nil }
  }

  /// A desk of a remote deck. Nothing is on the wire until `connect()`.
  init(remote link: DeckLink) {
    isLocal = false
    self.link = link
    super.init()
    var s = sink
    conn = db_conn_remote(&s, { ctx, text, n in
      guard let ctx = ctx, let text = text else { return }
      let me = Unmanaged<DeckConnection>.fromOpaque(ctx).takeUnretainedValue()
      let message = String(decoding: UnsafeRawBufferPointer(start: text, count: n), as: UTF8.self)
      me.task?.send(.string(message)) { [weak me] error in
        if let error = error { DispatchQueue.main.async { me?.socketEnded("send: \(error.localizedDescription)") } }
      }
    }, Unmanaged.passUnretained(self).toOpaque())
  }
  private var link: DeckLink?
  /// REMOTE: the deck this window is a desk of (the ask pane fetches its pages from it); nil = the in-process deck
  var remoteLink: DeckLink? { link }

  func connect() {
    guard !isLocal, let url = link?.socketURL else { return }
    closed = false
    let cfg = URLSessionConfiguration.ephemeral            // system TLS and proxy, no cookies kept
    cfg.waitsForConnectivity = false
    let s = URLSession(configuration: cfg, delegate: self, delegateQueue: .main)      // delegate calls on the MAIN queue
    session = s
    let t = s.webSocketTask(with: url)
    t.maximumMessageSize = 100 * 1024 * 1024               // core/deck/ws.hpp kWsMaxPayload (a replay can be megabytes)
    task = t
    t.resume()
    receiveNext()
    // The deck pings every 15 s and drops a client that leaves two unanswered (core/PROTOCOL.md "Heartbeat").
    // VERIFY: URLSessionWebSocketTask answers a server ping with a pong by itself (it is documented to).
  }

  private func receiveNext() {
    task?.receive { [weak self] result in
      // (URLSession's own queue) -> main: the bridge and everything behind it are main-thread only
      DispatchQueue.main.async {
        guard let me = self, !me.closed else { return }
        switch result {
        case .failure(let error): me.socketEnded(error.localizedDescription)
        case .success(let message):
          if case .string(let text) = message, let c = me.conn {
            var t = text
            t.withUTF8 { buf in buf.withMemoryRebound(to: CChar.self) { db_conn_receive_text(c, $0.baseAddress, $0.count) } }
          }
          me.receiveNext()
        }
      }
    }
  }
  func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?) {
    delegate?.connectionOpened(self)
  }
  func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
    socketEnded(closeCode == .policyViolation ? "refused" : "closed")
  }
  func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    // a failed handshake lands here: 401 = the token is wrong (lib/phone.js answers the upgrade with it)
    // VERIFY: that a refused upgrade leaves its HTTPURLResponse on `task.response` (else every refusal reads as "away")
    if let http = task.response as? HTTPURLResponse, http.statusCode == 401 { socketEnded("refused: the link's token was not accepted"); return }
    if let error = error { socketEnded(error.localizedDescription) }
  }
  private func socketEnded(_ reason: String) {
    if closed { return }
    closed = true
    task?.cancel(with: .goingAway, reason: nil); task = nil
    session?.invalidateAndCancel(); session = nil
    failPending("the deck is away")
    delegate?.connectionClosed(self, reason: reason)
  }

  /// The window closed (local: its tiles drop out of that layout and keep running) / leave the remote deck.
  func close() {
    closed = true
    task?.cancel(with: .goingAway, reason: nil); task = nil
    session?.invalidateAndCancel(); session = nil
    if let c = conn { db_conn_close(c); conn = nil }
    failPending("closed")
  }
  private func failPending(_ why: String) { let p = pending; pending.removeAll(); for (_, done) in p { done(.failure(DeckError(message: why))) } }

  // ------------------------------------------------------------------ frames in (core -> this window)
  private func onFrame(_ f: DBFrame) {
    let id = f.id.map { String(cString: $0) } ?? ""
    switch Int(f.kind) {
    case Int(DB_FRAME_OPEN):
      // LOCAL: delivered from inside db_deck_add_window, before `conn` is even assigned — tell the window on the next turn
      DispatchQueue.main.async { [weak self] in if let me = self, me.conn != nil { me.delegate?.connectionOpened(me) } }
    case Int(DB_FRAME_DATA):
      if let b = f.bytes, f.bytes_len > 0 { delegate?.connection(self, data: id, bytes: b, count: f.bytes_len, replay: f.replay != 0) }
    case Int(DB_FRAME_META), Int(DB_FRAME_INIT):
      // (`init` is the phone's hello; a window ignores it and takes deskInit's answer — but its meta is as good as any)
      if Int(f.kind) == Int(DB_FRAME_META), let o = JSON.parse(f.json, length: f.json_len) as? JSONObject { delegate?.connection(self, meta: o.obj("meta")) }
    case Int(DB_FRAME_EXIT): delegate?.connection(self, exit: id, code: Int(f.exit_code))
    case Int(DB_FRAME_RESET): delegate?.connection(self, reset: id)
    case Int(DB_FRAME_OK):
      if let done = pending.removeValue(forKey: f.rid) { done(.success(JSON.parse(f.json, length: f.json_len))) }
    case Int(DB_FRAME_ERR):
      if let done = pending.removeValue(forKey: f.rid) { done(.failure(DeckError(message: f.name.map { String(cString: $0) } ?? "failed"))) }
    case Int(DB_FRAME_EVENT):
      delegate?.connection(self, event: f.name.map { String(cString: $0) } ?? "", payload: JSON.parse(f.json, length: f.json_len))
    default: break
    }
  }

  // ------------------------------------------------------------------ frames out (this window -> core)
  /// preload.js's invoke: answered once with the result or the deck's error message.
  func call(_ fn: String, _ args: [Any] = [], done: ((Result<Any?, DeckError>) -> Void)? = nil) {
    guard let c = conn else { done?(.failure(DeckError(message: "not connected"))); return }
    var rid: Int32 = 0
    if let done = done { rid = nextRid; nextRid = nextRid == Int32.max ? 1 : nextRid + 1; pending[rid] = done }
    db_conn_call(c, rid, fn, JSON.text(args))
  }
  /// fire and forget (`rid` 0): userInput, nudge, screenProbeReply
  func fire(_ fn: String, _ args: [Any] = []) { call(fn, args, done: nil) }

  func attach(_ id: String, sinceResize: Bool = false) { if let c = conn { db_conn_attach(c, id, sinceResize ? 1 : 0) } }
  func detach(_ id: String) { if let c = conn { db_conn_detach(c, id) } }
  /// A REAL keystroke (or, in local mode, anything the terminal sends). R8 is the caller's: see TerminalPane.send.
  func input(_ id: String, _ bytes: [UInt8]) {
    guard let c = conn, !bytes.isEmpty else { return }
    bytes.withUnsafeBufferPointer { b in b.withMemoryRebound(to: CChar.self) { db_conn_input(c, id, $0.baseAddress, $0.count) } }
  }
  /// The one synthesised thing a remote may send (exactly one SGR wheel report); false = the codec refused it.
  @discardableResult func wheel(_ id: String, _ report: [UInt8]) -> Bool {
    guard let c = conn else { return false }
    return report.withUnsafeBufferPointer { b in b.withMemoryRebound(to: CChar.self) { db_conn_wheel(c, id, $0.baseAddress, $0.count) != 0 } }
  }
  func resize(_ id: String, cols: Int, rows: Int) { if let c = conn { db_conn_resize(c, id, Int32(cols), Int32(rows)) } }
  func shown(_ ids: [String], hidden: Bool) {
    guard let c = conn else { return }
    // an array of C strings for the call's duration
    var cs: [UnsafePointer<CChar>?] = ids.map { UnsafePointer(strdup($0)) }
    db_conn_shown(c, &cs, Int32(cs.count), hidden ? 1 : 0)
    for p in cs { free(UnsafeMutablePointer(mutating: p)) }
  }
  func layout(_ l: GridLayout) { if let c = conn { db_conn_layout(c, l.handle) } }
  func leaving(_ id: String, on: Bool) { if let c = conn { db_conn_leaving(c, id, on ? 1 : 0) } }

  // LOCAL only (no-ops on a remote): the window's own facts, for hibernation's "on screen" and "the user is here"
  func windowState(visible: Bool, focused: Bool) { if let c = conn { db_conn_window_state(c, visible ? 1 : 0, focused ? 1 : 0) } }
  /// `r` is the window's AppKit frame (origin bottom-left). The deck keeps bounds as Electron does: from the TOP-left
  /// of the primary display (AppDelegate.rect(fromDeck:) is the way back).
  func windowBounds(_ r: CGRect) {
    guard let c = conn else { return }
    let top = (NSScreen.screens.first?.frame.height ?? r.maxY) - r.maxY
    db_conn_window_bounds(c, JSON.text(["x": Int(r.minX), "y": Int(top), "width": Int(r.width), "height": Int(r.height)]))
  }
  func windowKey() { if let c = conn { db_conn_window_key(c) } }
  var windowIndex: Int { conn.map { Int(db_conn_window_index($0)) } ?? -1 }
  var client: Int32 { conn.map { db_conn_client($0) } ?? 0 }
}

/// core/model/layout through the bridge: one window's grid layout (tile -> a rectangle of cells).
final class GridLayout {
  let handle: OpaquePointer                    // DBLayout*
  init(rows: Int, cols: Int) { handle = db_layout_create(Int32(rows), Int32(cols)) }
  deinit { db_layout_destroy(handle) }
  var grid: (rows: Int, cols: Int) { var r: Int32 = 0, c: Int32 = 0; db_layout_grid(handle, &r, &c); return (Int(r), Int(c)) }
  func regrid(rows: Int, cols: Int) { db_layout_regrid(handle, Int32(rows), Int32(cols)) }
  func load(json: String) { _ = db_layout_load_json(handle, json) }
  var ids: [String] { (0..<Int(db_layout_count(handle))).map { String(cString: db_layout_id_at(handle, Int32($0))) } }
  var isEmpty: Bool { db_layout_count(handle) == 0 }
  func cells(of id: String) -> [Int] {
    var out = [Int32](repeating: 0, count: Int(DB_MAX_CELLS))
    let n = db_layout_cells(handle, id, &out, Int32(out.count))
    return out.prefix(Int(n)).map(Int.init)
  }
  /// Fractions of the terminal area, top-left origin; nil = not placed
  func rect(of id: String) -> CGRect? {
    var x = 0.0, y = 0.0, w = 0.0, h = 0.0
    return db_layout_rect(handle, id, &x, &y, &w, &h) != 0 ? CGRect(x: x, y: y, width: w, height: h) : nil
  }
  func rect(ofCells cells: [Int]) -> CGRect? {
    var x = 0.0, y = 0.0, w = 0.0, h = 0.0; let c = cells.map(Int32.init)
    return db_layout_rect_of_cells(handle, c, Int32(c.count), &x, &y, &w, &h) != 0 ? CGRect(x: x, y: y, width: w, height: h) : nil
  }
  /// F3 / R1 / R3 / R6. Returns the ids this evicted (they drop out of the layout and keep running).
  @discardableResult func place(_ id: String, cells: [Int]) -> [String] {
    let c = cells.map(Int32.init)
    guard db_layout_place(handle, id, c, Int32(c.count)) != 0 else { return [] }
    return (0..<Int(db_layout_evicted_count(handle))).map { String(cString: db_layout_evicted_at(handle, Int32($0))) }
  }
  func unplace(_ id: String) { db_layout_unplace(handle, id) }
  func clear() { db_layout_clear(handle) }
  func prune(live: [String]) {
    var cs: [UnsafePointer<CChar>?] = live.map { UnsafePointer(strdup($0)) }
    db_layout_prune(handle, &cs, Int32(cs.count))
    for p in cs { free(UnsafeMutablePointer(mutating: p)) }
  }
  /// What a drop at (px, py) — fractions of the area — takes (R4 / R15 / R16), then where a placed tile lands
  func dropTarget(px: Double, py: Double, moving id: String) -> [Int] {
    var out = [Int32](repeating: 0, count: Int(DB_MAX_CELLS))
    let n = db_layout_drop_target(handle, px, py, &out, Int32(out.count))
    let target = Array(out.prefix(Int(n)))
    var res = [Int32](repeating: 0, count: Int(DB_MAX_CELLS))
    let m = db_layout_resolve_drop(handle, id, target, Int32(target.count), &res, Int32(res.count))
    return res.prefix(Int(m)).map(Int.init)
  }
}
