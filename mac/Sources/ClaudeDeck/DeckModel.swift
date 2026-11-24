// mac/Sources/ClaudeDeck/DeckModel.swift — the deck's meta as Swift values: what a window keeps of the last
// `meta` frame (core/PROTOCOL.md "Core -> shell"), read defensively (a key that is missing is a default, never a crash).
// UNBUILT, UNTESTED. Windows counterpart: `struct TileInfo` / `applyMeta` in windows/src/ui.hpp + app.cpp.
// JS mirrored: renderer/js/00-state.js (state), 70-meta.js (applyMeta), 10-format.js (fmtAge).
import Foundation

typealias JSONObject = [String: Any]

enum JSON {
  static func parse(_ text: UnsafePointer<CChar>?, length: Int) -> Any? {
    guard let text = text, length > 0 else { return nil }
    let data = Data(bytes: text, count: length)
    return try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed])
  }
  static func parse(_ text: String) -> Any? { try? JSONSerialization.jsonObject(with: Data(text.utf8), options: [.fragmentsAllowed]) }
  static func text(_ value: Any) -> String {
    guard let d = try? JSONSerialization.data(withJSONObject: value, options: [.fragmentsAllowed]) else { return "null" }
    return String(decoding: d, as: UTF8.self)
  }
}
extension Dictionary where Key == String, Value == Any {
  func str(_ k: String) -> String { self[k] as? String ?? "" }
  func bool(_ k: String) -> Bool { (self[k] as? Bool) ?? ((self[k] as? NSNumber)?.boolValue ?? false) }
  func int(_ k: String) -> Int? { (self[k] as? NSNumber)?.intValue }
  func double(_ k: String) -> Double? { (self[k] as? NSNumber)?.doubleValue }
  func obj(_ k: String) -> JSONObject { self[k] as? JSONObject ?? [:] }
  func arr(_ k: String) -> [Any] { self[k] as? [Any] ?? [] }
}

/// One tile of `meta.tiles` (core/session/meta.cpp tileMeta writes it). Only what the shell draws or decides on.
struct Tile {
  var id = "", kind = "", title = "", account = "", status = "", group = "", lastText = "", effort = "", cwd = "", model = ""
  var busy = false, waiting = false, asleep = false, pinned = false, exited = false, attention = false, ultracode = false
  var cloud = false, mouseOn = false, sgrMouse = false, altScreen = false, fresh = false
  var exitCode: Int?
  var lastAt: Double?                       // ms: the last message's time (the age)
  var context: Double?                      // 0…1 of the context window, when known
  var inWindow: Int?                        // which window of the deck holds it (R5); nil = none / a remote is told none
  var mirror: (cols: Int, rows: Int)?       // another client owns the size: render at exactly this (nil = this window drives)
  var driver = ""                           // who typed last: "" | "phone" | "desk" | "remote"
  var held: [JSONObject] = []               // special prompts waiting on it
  var raw: JSONObject = [:]

  init(_ o: JSONObject) {
    raw = o
    id = o.str("id"); kind = o.str("kind"); title = o.str("title"); account = o.str("account"); status = o.str("status")
    group = o.str("group"); lastText = o.str("lastText"); effort = o.str("effort"); cwd = o.str("cwd"); model = o.str("model")
    busy = o.bool("busy"); waiting = o.bool("waiting"); asleep = o.bool("asleep"); pinned = o.bool("pinned"); exited = o.bool("exited")
    attention = o.bool("attention"); ultracode = o.bool("ultracode"); cloud = o.bool("cloud"); mouseOn = o.bool("mouseOn")
    sgrMouse = o.bool("sgrMouse"); altScreen = o.bool("altScreen"); fresh = o.bool("fresh")
    exitCode = o.int("exitCode"); lastAt = o.double("lastAt"); context = o.double("context"); inWindow = o.int("inWindow")
    driver = o.str("driver"); held = o.arr("held").compactMap { $0 as? JSONObject }
    let m = o.obj("mirror")
    if let c = m.int("cols"), let r = m.int("rows"), c > 0, r > 0 { mirror = (c, r) }
  }
  var isShell: Bool { kind == "shell" }
  var displayTitle: String { title.isEmpty ? (isShell ? "terminal" : "untitled") : title }
}

/// One entry of `meta.accounts`. `abbr` is the account's own (30-tiles.js shows the mark only when it has one).
struct Account { var id = "", label = "", dir = "", abbr = ""; var raw: JSONObject = [:]
  init(_ o: JSONObject) { raw = o; id = o.str("id"); label = o.str("label"); dir = o.str("dir"); abbr = o.str("abbr") } }

/// The last snapshot. `apply` returns which tile ids came and went so the window can prune its layout and panes.
struct DeckMeta {
  var tiles: [Tile] = []
  var accounts: [Account] = []
  var settings: JSONObject = [:]
  var usage: JSONObject = [:]
  var groups: [String] = []
  var restart: JSONObject = [:]
  var asks: [JSONObject] = []
  var now: Double = 0                       // meta.now: the deck's clock (ages are measured against it, not this machine's)
  var raw: JSONObject = [:]

  func tile(_ id: String) -> Tile? { tiles.first { $0.id == id } }
  func account(_ id: String) -> Account? { accounts.first { $0.id == id } }

  mutating func apply(_ meta: JSONObject) -> (added: [String], removed: [String]) {
    let before = Set(tiles.map { $0.id })
    raw = meta
    tiles = meta.arr("tiles").compactMap { ($0 as? JSONObject).map(Tile.init) }
    accounts = meta.arr("accounts").compactMap { ($0 as? JSONObject).map(Account.init) }
    settings = meta.obj("settings"); usage = meta.obj("usage"); restart = meta.obj("restart")
    groups = meta.arr("groups").compactMap { $0 as? String }
    asks = meta.arr("asks").compactMap { $0 as? JSONObject }
    now = meta.double("now") ?? Date().timeIntervalSince1970 * 1000
    let after = Set(tiles.map { $0.id })
    return (tiles.map { $0.id }.filter { !before.contains($0) }, before.filter { !after.contains($0) })
  }
  var grid: (rows: Int, cols: Int) { let g = settings.obj("grid"); return (max(1, min(4, g.int("rows") ?? 2)), max(1, min(4, g.int("cols") ?? 2))) }
  var fontSize: CGFloat { CGFloat(settings.double("fontSize") ?? Double(Theme.terminalFontSize)) }
  var fontFamily: String { settings.str("fontFamily") }
  /// 30-tiles.js ordering: pinned first (their own order), then the meta's order, which the core already sorted
  /// (pin-then-last-finished is main.js's, so the shell does NOT sort again)
  var ordered: [Tile] { tiles }
}

enum Format {
  /// 10-format.js fmtAge(ms): "now", "5m", "2h", "3d" — called with `meta.now - (tile.lastAt || meta.now)`
  static func age(ms: Double) -> String {
    let s = Int(max(0, (ms / 1000).rounded(.down)))
    if s < 60 { return "now" }
    let m = s / 60; if m < 60 { return "\(m)m" }
    let h = m / 60; if h < 24 { return "\(h)h" }
    return "\(h / 24)d"
  }
}
