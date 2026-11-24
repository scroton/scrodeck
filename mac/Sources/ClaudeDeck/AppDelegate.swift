// mac/Sources/ClaudeDeck/AppDelegate.swift — the process: what mode it runs in, the menu bar, the windows it has
// open, the Mac rules for leaving (Cmd+W keeps every session alive in the Dock; Cmd+Q asks while sessions run),
// toasts and the Dock bounce, restart, the single-instance lock.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain).
// Windows counterpart: wWinMain + the hub window in windows/src/app.cpp, windows/src/notify.hpp (toasts),
// windows/src/lifecycle.hpp (restart, the instance lock), windows/host/localdeck.cpp (shippingAllowClaude).
// JS mirrored: main.js — createWindow's Mac menu, `window-all-closed` / `activate` / `before-quit` on a Mac,
// notify (dock.bounce), restartDeck, app.requestSingleInstanceLock.
//
//   ClaudeDeck                         local mode: the deck runs in this process (its own state folder)
//   ClaudeDeck --remote <deck link>    remote mode: a desk of a deck on another machine (https://host/?t=token)
//   --state-dir <dir>   a scratch deck (tests): Claude sessions and the ask feed are then OFF unless said otherwise
//   --serve <port>      serve the phone / remote desk on 127.0.0.1:<port> for this run
//   --allow-claude / --no-claude, --asks / --no-asks, --assets <dir>, --test, --wait-pid <pid>
import AppKit
import UserNotifications
import DeckMacSeams

struct LaunchOptions {
  var remoteLink: String?
  var stateDir = "", assets = ""
  var servePort: Int32 = 0
  var allowClaude: Bool?, asks: Bool?
  var test = false
  var waitPid: Int32 = 0
  init(_ args: [String]) {
    var i = 1
    func next() -> String { i += 1; return i < args.count ? args[i] : "" }
    while i < args.count {
      switch args[i] {
      case "--remote": remoteLink = next()
      case "--state-dir": stateDir = next()
      case "--assets": assets = next()
      case "--serve": servePort = Int32(next()) ?? 0
      case "--allow-claude": allowClaude = true
      case "--no-claude": allowClaude = false
      case "--asks": asks = true
      case "--no-asks": asks = false
      case "--test": test = true
      case "--wait-pid": waitPid = Int32(next()) ?? 0
      default: break
      }
      i += 1
    }
  }
  /// THE SHIPPING DEFAULT of allowClaude, as windows/host/localdeck.cpp decides it: ON only for the real app (a
  /// bundle with our identifier) on its own state folder; OFF for a scratch `--state-dir` deck, a bare `swift run`
  /// binary, and any test; the flags win, the last one said.
  var shippingAllowClaude: Bool { allowClaude ?? (Bundle.main.bundleIdentifier == AppDelegate.bundleId && stateDir.isEmpty && !test) }
  var shippingAsks: Bool { asks ?? ((Bundle.main.bundleIdentifier == AppDelegate.bundleId && stateDir.isEmpty && !test) || ProcessInfo.processInfo.environment["ASK_PAGE_STATE_DIR"] != nil) }
}

final class AppDelegate: NSObject, NSApplicationDelegate, DeckShell, DeckWindowOwner, UNUserNotificationCenterDelegate {
  static let bundleId = Edition.bundleId            // its own: never the Electron deck's id (toasts and the Dock would merge)
  let options = LaunchOptions(CommandLine.arguments)
  private(set) var localHost: DeckHost?
  private var windows: [DeckWindowController] = []
  private var restarting = false, quitConfirmed = false
  private var keyMonitor: Any?

  // ------------------------------------------------------------------ launch
  func applicationDidFinishLaunching(_ notification: Notification) {
    NSApp.mainMenu = AppDelegate.menu()
    if options.waitPid > 0 { dms_wait_for_pid_exit(options.waitPid, 15000) }      // a restart: the old deck goes first
    installKeyMonitor()
    if let text = options.remoteLink {
      guard let link = DeckLink(text) else { fail("That is not a deck link. It looks like https://<deck>/?t=<token> (Settings → Phone on the deck)."); return }
      open(DeckConnection(remote: link), bounds: nil)
      return
    }
    // LOCAL: one deck per config folder — a second launch brings the first forward (app.requestSingleInstanceLock)
    if !options.test, options.stateDir.isEmpty, dms_single_instance_lock(nil) == 0 {
      NSRunningApplication.runningApplications(withBundleIdentifier: AppDelegate.bundleId).first { $0 != NSRunningApplication.current }?.activate(options: [.activateAllWindows])
      NSApp.terminate(nil); return
    }
    var o = DeckHost.Options()
    o.stateDir = options.stateDir; o.assetRoot = options.assets; o.servePort = options.servePort
    o.allowClaude = options.shippingAllowClaude; o.asks = options.shippingAsks; o.test = options.test
    guard let host = DeckHost(options: o, shell: self) else { fail("The deck could not start (see Console for the reason)."); return }
    localHost = host
    setUpNotifications()
    // R7: the windows come back. Each window made BEFORE start() takes the n-th saved window's place and layout;
    // R13: not an empty auxiliary one — and the core only knows the saved ones after start(), so the first window
    // is made now and the others once the state is loaded.
    openLocalWindow(bounds: nil)
    host.start()
    for saved in host.savedWindows.dropFirst() where !(saved.obj("layout").isEmpty) { openLocalWindow(bounds: saved.obj("bounds")) }
    // VERIFY (with the core's author): Deck::addWindow after start() — does the 2nd window still take the 2nd saved
    // entry? windows/src/app.cpp opens every saved window before start() by reading the state file's count itself.
    NSApp.activate(ignoringOtherApps: true)
  }
  private func fail(_ text: String) {
    let a = NSAlert(); a.messageText = Edition.productName; a.informativeText = text; a.runModal()
    NSApp.terminate(nil)
  }

  // ------------------------------------------------------------------ windows
  private func open(_ conn: DeckConnection, bounds: CGRect?, inactive: Bool = false) {
    let w = DeckWindowController(conn: conn, bounds: bounds, inactive: inactive)
    w.owner = self
    windows.append(w)
  }
  @discardableResult private func openLocalWindow(bounds: JSONObject?, tileId: String? = nil, adopt: Int32 = 0, inactive: Bool = false) -> Bool {
    guard let host = localHost else { return false }
    let rect = bounds.flatMap(AppDelegate.rect(fromDeck:))
    guard let conn = DeckConnection(local: host, boundsJSON: bounds.map { JSON.text($0) }, tileId: tileId, adoptClient: adopt) else { return false }
    open(conn, bounds: rect, inactive: inactive)
    return true
  }
  /// The deck keeps bounds as Electron does: { x, y, width, height } from the TOP-left of the primary display.
  static func rect(fromDeck b: JSONObject) -> CGRect? {
    guard let w = b.double("width"), let h = b.double("height"), w > 100, h > 100, let screen = NSScreen.screens.first else { return nil }
    let x = b.double("x") ?? 120, y = b.double("y") ?? 120
    return CGRect(x: x, y: Double(screen.frame.height) - y - h, width: w, height: h)
  }
  func windowClosed(_ w: DeckWindowController) { windows.removeAll { $0 === w } }
  /// F5 / "Open in a new window": the tile leaves wherever it was and fills a new window
  func openWindow(holding tileId: String, at screenPoint: NSPoint?) {
    var b: JSONObject = ["width": 1200, "height": 800]
    if let p = screenPoint, let screen = NSScreen.screens.first { b["x"] = Int(p.x) - 200; b["y"] = Int(screen.frame.height - p.y) - 40 }
    openLocalWindow(bounds: b, tileId: tileId)
  }
  func newSession(account: String, inNewWindow: Bool) { (NSApp.keyWindow?.delegate as? DeckWindowController ?? windows.first)?.sidebarNewSession(account: account) }

  /// The Mac rule (main.js `window-all-closed`): closing the last window stops NOTHING. The deck stays in the Dock
  /// with every pty running; clicking the icon builds a fresh window, which re-attaches each session with its replay.
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
  func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
    if !flag {
      if localHost != nil { openLocalWindow(bounds: nil) }
      else if let text = options.remoteLink, let link = DeckLink(text) { open(DeckConnection(remote: link), bounds: nil) }
    }
    return true
  }

  // ------------------------------------------------------------------ leaving
  /// Cmd+Q sits next to Cmd+W and Cmd+A, and one slip would stop every hosted session: ask first while sessions
  /// run (main.js `before-quit` on a Mac; Terminal.app does the same). A restart that was asked for does not ask.
  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    guard let host = localHost, !restarting, !quitConfirmed else { return .terminateNow }
    let running = host.liveCount                             // (main.js counts Claude tiles only; this counts every live process: PARITY)
    if running == 0 { return .terminateNow }
    let a = NSAlert()
    a.alertStyle = .warning
    a.messageText = "Quit \(Edition.productName)?"
    a.informativeText = "\(running) session\(running == 1 ? "" : "s") will be stopped. They resume when the deck opens again."
    a.addButton(withTitle: "Cancel"); a.addButton(withTitle: "Quit")       // Cancel is the default (Return)
    if a.runModal() != .alertSecondButtonReturn { return .terminateCancel }
    quitConfirmed = true
    return .terminateNow
  }
  func applicationWillTerminate(_ notification: Notification) {
    dms_arm_exit_watchdog(8000)                              // whatever holds the exit, the process is gone in 8 s (main.js armExitWatchdog)
    for w in windows { w.conn.close() }
    localHost?.stop()                                        // every pty killed (killAll); state was saved as it changed
  }

  // ------------------------------------------------------------------ DeckShell (the core's Notifier and Lifecycle)
  private func setUpNotifications() {
    guard Bundle.main.bundleIdentifier != nil else { return }     // a bare binary has no notification centre (it would raise)
    let c = UNUserNotificationCenter.current()
    c.delegate = self
    c.requestAuthorization(options: [.alert, .sound]) { _, _ in }
  }
  func deckToast(_ t: ToastInfo) {
    guard Bundle.main.bundleIdentifier != nil else { return }
    let content = UNMutableNotificationContent()
    content.title = t.title; content.body = t.body
    if !t.silent { content.sound = .default }
    content.userInfo = ["title": t.title, "body": t.body, "kind": t.kind, "tileId": t.tileId, "sessionId": t.sessionId, "silent": t.silent]
    UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
  }
  /// The core's flash: `app.dock.bounce('informational')` on a Mac (one bounce; no badge — the Electron deck sets none)
  func deckFlash(windowIndex: Int) { if !NSApp.isActive { NSApp.requestUserAttention(.informationalRequest) } }
  /// Shown even while the deck is in front (Electron's Notification does not hide itself for the focused app)
  func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification, withCompletionHandler done: @escaping (UNNotificationPresentationOptions) -> Void) { done([.banner, .sound]) }
  /// A clicked toast: the core says which window and tells it what to show; raising it is ours
  func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse, withCompletionHandler done: @escaping () -> Void) {
    let u = response.notification.request.content.userInfo
    let t = ToastInfo(title: u["title"] as? String ?? "", body: u["body"] as? String ?? "", kind: u["kind"] as? String ?? "", tileId: u["tileId"] as? String ?? "", sessionId: u["sessionId"] as? String ?? "", silent: u["silent"] as? Bool ?? false)
    DispatchQueue.main.async {
      if self.windows.isEmpty { self.openLocalWindow(bounds: nil) }
      let client = self.localHost?.toastClicked(t) ?? 0
      let w = self.windows.first { $0.conn.client == client } ?? self.windows.first
      NSApp.activate(ignoringOtherApps: true)
      if w?.window.isMiniaturized == true { w?.window.deminiaturize(nil) }
      w?.window.makeKeyAndOrderFront(nil)
      done()
    }
  }
  /// restartDeck: start the deck again (what relaunch.json names, else this executable with its own arguments),
  /// told to wait for this process, then quit without asking.
  func deckRestart(why: String) -> Bool {
    let spec = localHost?.relaunchSpec
    let file = spec?.str("command").isEmpty == false ? spec!.str("command") : (Bundle.main.executablePath ?? CommandLine.arguments[0])
    var args = spec?.str("command").isEmpty == false ? (spec!.arr("args").compactMap { $0 as? String }) : Array(CommandLine.arguments.dropFirst())
    if let i = args.firstIndex(of: "--wait-pid") { args.removeSubrange(i...min(i + 1, args.count - 1)) }
    var cs: [UnsafePointer<CChar>?] = args.map { UnsafePointer(strdup($0)) } + [nil]
    let ok = dms_relaunch(file, &cs, 1) != 0
    for p in cs { if let p = p { free(UnsafeMutablePointer(mutating: p)) } }
    if ok { restarting = true; DispatchQueue.main.async { NSApp.terminate(nil) } }
    return ok
  }
  /// The on-call's desk when no cell is free anywhere: a window for a client the core already registered, shown
  /// WITHOUT taking the focus (keys being typed elsewhere never land in it)
  func deckOpenWindow(client: Int32, boundsJSON: String, inactive: Bool) -> Bool {
    openLocalWindow(bounds: JSON.parse(boundsJSON) as? JSONObject, adopt: client, inactive: inactive)
  }

  // ------------------------------------------------------------------ keys that never reach a view
  /// The deck's own Cmd keys (Cmd+B, Cmd+Shift+P, Cmd+Shift+[ ] …) are NOT menu items (main.js keeps them out of the
  /// menu so they never fire twice), and AppKit does not promise a keyDown to the first responder for an unclaimed
  /// Cmd combination — nor to the terminal while the sidebar or a web view has the keyboard. So they are taken here.
  private func installKeyMonitor() {
    keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown]) { event in
      guard event.modifierFlags.contains(.command), let w = event.window?.delegate as? DeckWindowController else { return event }
      return w.areaWindowKey(event) ? nil : event            // nil = consumed
    }
    // VERIFY: with this monitor in place TerminalAreaView.keyDown never sees a Cmd combination the window owns; one
    // it does not own (Cmd+C, Cmd+V, Cmd+Q, Cmd+W …) passes through to the menu bar as usual.
  }

  // ------------------------------------------------------------------ the menu bar (main.js createWindow, the Mac branch)
  /// appMenu, editMenu, View (full screen), windowMenu — the standard roles, which is what gives Cmd+Q, Cmd+H,
  /// Cmd+M and Cmd+C / V / X / A their meaning.
  static func menu() -> NSMenu {
    let main = NSMenu()
    func sub(_ title: String, _ build: (NSMenu) -> Void) -> NSMenu { let i = NSMenuItem(); main.addItem(i); let m = NSMenu(title: title); i.submenu = m; build(m); return m }
    let name = Edition.productName
    _ = sub(name) { m in
      m.addItem(withTitle: "About \(name)", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
      m.addItem(.separator())
      let services = NSMenuItem(title: "Services", action: nil, keyEquivalent: ""); let sm = NSMenu(title: "Services"); services.submenu = sm; NSApp.servicesMenu = sm; m.addItem(services)
      m.addItem(.separator())
      m.addItem(withTitle: "Hide \(name)", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
      let others = m.addItem(withTitle: "Hide Others", action: #selector(NSApplication.hideOtherApplications(_:)), keyEquivalent: "h"); others.keyEquivalentModifierMask = [.command, .option]
      m.addItem(withTitle: "Show All", action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: "")
      m.addItem(.separator())
      m.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    }
    _ = sub("Edit") { m in
      m.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
      let redo = m.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "z"); redo.keyEquivalentModifierMask = [.command, .shift]
      m.addItem(.separator())
      m.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
      m.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
      m.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
      m.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
    }
    _ = sub("View") { m in
      let fs = m.addItem(withTitle: "Toggle Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f"); fs.keyEquivalentModifierMask = [.command, .control]
      // (Electron's View menu also has zoom in / out / reset of the PAGE; here the font size is a deck setting)
    }
    let window = sub("Window") { m in
      m.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
      m.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
      m.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
      m.addItem(.separator())
      m.addItem(withTitle: "Bring All to Front", action: #selector(NSApplication.arrangeInFront(_:)), keyEquivalent: "")
    }
    NSApp.windowsMenu = window
    return main
  }
}
