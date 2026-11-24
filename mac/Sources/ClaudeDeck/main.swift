// mac/Sources/ClaudeDeck/main.swift — the entry point: an NSApplication with AppDelegate. No storyboard, no nib:
// the menu bar and every window are built in code (AppDelegate.menu, DeckWindowController).
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain). Windows counterpart: wWinMain in windows/src/app.cpp.
// JS mirrored: main.js's top level (app.whenReady).
import AppKit

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)      // a Dock icon and a menu bar even when run as a bare binary (`swift run`)
app.run()
