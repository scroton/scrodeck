// mac/Sources/ClaudeDeck/DeckHost.swift — LOCAL mode's deck as the app holds it: one DBHost (the macOS seams) and
// one DBDeck (core/deck + its server) for the whole process, the pump on the main thread, and the shell's own
// seams (toasts, Dock bounce, restart, open / reveal, a second window) as C callbacks into Swift.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain).
// Windows counterpart: windows/src/deckhost.hpp (DeckHost) + the hub window's pump in windows/src/app.cpp.
// JS mirrored: main.js's process half (whenReady, the timers, window-all-closed).
//
// THREADS: everything here runs on the main thread. The ONE exception is `wake`, which the seams call from a pty
// or socket thread: it only hops to the main queue. Timers of the core fire only inside pump().
import AppKit
import DeckCoreBridge
import DeckMacSeams

struct ToastInfo { var title = "", body = "", kind = "", tileId = "", sessionId = ""; var silent = false }

protocol DeckShell: AnyObject {
  func deckToast(_ toast: ToastInfo)
  func deckFlash(windowIndex: Int)
  func deckRestart(why: String) -> Bool
  func deckOpenWindow(client: Int32, boundsJSON: String, inactive: Bool) -> Bool
}

final class DeckHost {
  struct Options {
    var stateDir = ""                 // "" = dms_default_state_dir()
    var assetRoot = ""                // the folder holding renderer/, lib/, assets/ ("" = the app bundle's Resources when it has them)
    var servePort: Int32 = 0
    var allowClaude = false           // THE SWITCH: decided by AppDelegate.shippingAllowClaude (never on in a test run)
    var persistShells = false
    var asks = false
    var usagePolling = false          // the owner's to allow: nothing in the app turns it on
    var test = false                  // a test run: loopback ports 8811-8819 only, no network looked at, no login shell
  }

  private(set) var deck: OpaquePointer?          // DBDeck*
  private var host: OpaquePointer?               // DBHost*
  private var timer: DispatchSourceTimer?
  private var wakePending = false
  weak var shell: DeckShell?
  let options: Options
  private(set) var stateDir = ""

  init?(options: Options, shell: DeckShell?) {
    self.options = options
    self.shell = shell
    var mo = DMSOptions()
    mo.login_environment = options.test ? 0 : 1
    mo.http_and_credentials = options.usagePolling ? 1 : 0
    mo.server = 1
    if options.test { mo.test_port_low = 8811; mo.test_port_high = 8819 }
    host = dms_host_create(&mo)

    var o = DBOptions()
    db_options_init(&o)
    let state = options.stateDir.isEmpty ? String(cString: dms_default_state_dir()) : options.stateDir
    stateDir = state
    let assets = options.assetRoot.isEmpty ? (Bundle.main.resourcePath ?? "") : options.assetRoot
    let resources = Bundle.main.resourcePath ?? ""
    o.allow_claude = options.allowClaude ? 1 : 0
    o.persist_shells = options.persistShells ? 1 : 0
    o.asks = options.asks ? 1 : 0
    o.usage_polling = options.usagePolling ? 1 : 0
    o.serve_port = options.servePort

    var cb = DBShellCallbacks()
    cb.ctx = Unmanaged.passUnretained(self).toOpaque()
    cb.toast = { ctx, t in
      guard let ctx = ctx, let t = t?.pointee else { return }
      let me = Unmanaged<DeckHost>.fromOpaque(ctx).takeUnretainedValue()
      me.shell?.deckToast(ToastInfo(title: DeckHost.s(t.title), body: DeckHost.s(t.body), kind: DeckHost.s(t.kind), tileId: DeckHost.s(t.tile_id), sessionId: DeckHost.s(t.session_id), silent: t.silent != 0))
    }
    cb.flash = { ctx, index in
      guard let ctx = ctx else { return }
      Unmanaged<DeckHost>.fromOpaque(ctx).takeUnretainedValue().shell?.deckFlash(windowIndex: Int(index))
    }
    cb.restart = { ctx, why in
      guard let ctx = ctx else { return 0 }
      return Unmanaged<DeckHost>.fromOpaque(ctx).takeUnretainedValue().shell?.deckRestart(why: DeckHost.s(why)) == true ? 1 : 0
    }
    cb.arm_exit_watchdog = { _, ms in dms_arm_exit_watchdog(ms) }
    cb.open_external = { _, url in
      guard let u = URL(string: DeckHost.s(url)) else { return 0 }
      return NSWorkspace.shared.open(u) ? 1 : 0                              // shell.openExternal
    }
    cb.open_path = { _, path in NSWorkspace.shared.open(URL(fileURLWithPath: DeckHost.s(path))) ? 1 : 0 }     // shell.openPath
    cb.show_in_folder = { _, path in
      NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: DeckHost.s(path))]); return 1      // shell.showItemInFolder: reveal in Finder
    }
    cb.open_window = { ctx, client, bounds, inactive in
      guard let ctx = ctx else { return 0 }
      return Unmanaged<DeckHost>.fromOpaque(ctx).takeUnretainedValue().shell?.deckOpenWindow(client: client, boundsJSON: DeckHost.s(bounds), inactive: inactive != 0) == true ? 1 : 0
    }
    // cb.fingerprint_json stays nil: the update offer compares main-process files, which a signed .app does not have.

    var err = [CChar](repeating: 0, count: 512)
    // The C strings must outlive the call only: withCString nests keep them alive for db_deck_create.
    let made: OpaquePointer? = state.withCString { cState in
      assets.withCString { cAssets in
        resources.withCString { cRes in
          o.state_dir = cState; o.asset_root = cAssets; o.resources_dir = cRes
          return db_deck_create(&o, host, &cb, &err, err.count)
        }
      }
    }
    guard let d = made else {
      NSLog("%@: the deck could not be made: %@", Edition.productName, String(cString: err))
      db_host_destroy(host); host = nil
      return nil
    }
    deck = d
    db_host_set_wake(host, { ctx in
      // ANY thread. One hop to the main queue per burst (the inbox already collapses a burst into one wake).
      guard let ctx = ctx else { return }
      DispatchQueue.main.async { Unmanaged<DeckHost>.fromOpaque(ctx).takeUnretainedValue().pump() }
    }, Unmanaged.passUnretained(self).toOpaque())
    // (DeckHost lives as long as the app: AppDelegate owns it and stop() runs before it is released, so the
    //  unretained pointer in `wake` cannot outlive it.)
  }

  private static func s(_ p: UnsafePointer<CChar>?) -> String { p.map { String(cString: $0) } ?? "" }

  /// Load settings + state and restore the saved tiles. Windows added BEFORE this take their saved place (R7).
  func start() {
    guard let d = deck else { return }
    db_deck_start(d)
    let t = DispatchSource.makeTimerSource(queue: .main)
    t.setEventHandler { [weak self] in self?.pump() }
    t.schedule(deadline: .now() + .milliseconds(50))
    t.resume()
    timer = t
  }

  /// Drain what the pty / socket threads queued, run the core's timers, deliver frames; then sleep until the core
  /// says something is due (never more than 2 s: the meta tick).
  func pump() {
    guard let d = deck else { return }
    db_deck_pump(d)
    let due = max(1, Int(db_deck_next_due_ms(d)))
    timer?.schedule(deadline: .now() + .milliseconds(due), leeway: .milliseconds(min(50, due / 4)))
  }

  func stop() {
    timer?.cancel(); timer = nil
    if let d = deck { db_deck_destroy(d); deck = nil }      // shutdown: every pty killed, the listener stopped
    if let h = host { db_host_destroy(h); host = nil }
  }

  var liveCount: Int { deck.map { Int(db_deck_live_count($0)) } ?? 0 }
  var phoneStatus: JSONObject { deck.flatMap { JSON.parse(String(cString: db_deck_phone_status_json($0))) as? JSONObject } ?? [:] }
  var savedWindows: [JSONObject] { deck.flatMap { JSON.parse(String(cString: db_deck_saved_windows_json($0))) as? [JSONObject] } ?? [] }
  var relaunchSpec: JSONObject? { deck.flatMap { JSON.parse(String(cString: db_deck_relaunch_spec_json($0))) as? JSONObject } }

  /// A clicked toast: the client id of the window to raise (0 = none); that window is sent `tiles:activate` / `asks:open`.
  /// The ask feed's Done folders (where an older ask page's own Done is saved: AskPane.downloadDestination)
  var askDownloads: [String] {
    guard let d = deck else { return [] }
    return (JSON.parse(String(cString: db_deck_ask_downloads_json(d))) as? [Any])?.compactMap { $0 as? String } ?? []
  }
  func toastClicked(_ t: ToastInfo) -> Int32 {
    guard let d = deck else { return 0 }
    return t.title.withCString { title in t.body.withCString { body in t.kind.withCString { kind in t.tileId.withCString { tile in t.sessionId.withCString { session in
      var c = DBToast(title: title, body: body, silent: t.silent ? 1 : 0, kind: kind, tile_id: tile, session_id: session)
      return db_deck_toast_clicked(d, &c)
    } } } } }
  }
}
