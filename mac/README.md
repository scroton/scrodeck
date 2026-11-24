# Claude Deck — the macOS shell (`mac/`)

The Swift shell of docs/NATIVE-PORT.md §2: **AppKit + Metal + CoreText on the SAME C++ core the Windows shell
runs** (`core/`: the deck itself, the terminal engine, the frame codec, the layout rules).

## State — read this first

**This tree was written on a Windows box with no Swift toolchain, no macOS SDK and no Mac. It is a first draft.**

| Part | Language | State |
|---|---|---|
| `Sources/DeckCoreBridge` — a C ABI over the core | C++17 | **built and tested here** with g++ against the real core: `Tests/build.ps1` → **125 passed, 0 failed**; on Apple clang in the mac CI: 125 / 0 (run 45233494050) |
| `Sources/DeckMacSeams/KeychainNames.hpp` — which Keychain items hold an account's sign-in | C++17 | **tested here** against vectors written by the real `lib/platform.js` (`Tests/gen-keychain-names.js`) |
| `Sources/DeckMacSeams/MacSeams.hpp` — the seam classes' declarations | C++17 | **checked here**: all 8 satisfy `core/deck/seams.hpp` (a `-fsyntax-only` probe in `Tests/build.ps1`) |
| `Sources/DeckMacSeams/*.cpp`, `MacHttp.mm` — the macOS seams' bodies | C++ / Objective-C++ | **UNBUILT, UNTESTED** (forkpty, libproc, BSD sockets, URLSession do not exist here) |
| `Sources/ClaudeDeck/*.swift` — the app | Swift | **COMPILES** on GitHub's macOS runners (`.github/workflows/mac.yml`, by hand; last: 45233494050, the ask pane included, no warnings); **never run** |
| `Sources/ClaudeDeck/AskHostPage.swift` — the ask pane's host document (JavaScript) | JS in a Swift string | **tested here** in headless Edge against fake ask pages: `Tests/askhost-test/run.ps1` → **58 passed, 0 failed** (Blink, not WebKit) |
| `Package.swift`, the module maps, `tools/make-app.sh`, `Tests/seams_smoke.cpp` | | **UNBUILT / UNRUN** |

Every line whose API shape was written from memory AND that the author knew to be unsure of is marked `// VERIFY:`
with what to check (`grep -rn VERIFY mac/Sources mac/Package.swift`: 23 lines, 21 distinct checks). An unmarked
line is not a promise: the first build WILL have errors beyond the marks. The design goal was to keep them shallow
- wrong labels, optionality and `override`s, not a wrong architecture. `PARITY.md` tracks the shell against the same rows as
`windows/PARITY.md`; nothing there is claimed "done".

## Architecture

```
 Swift            ClaudeDeck            AppDelegate ── DeckHost (local) ─┐        DeckWindowController (one per window)
 (AppKit, Metal)                         menu, Dock, quit, toasts        │          ├ SidebarView          AppKit views
                                                                         │          ├ TerminalAreaView     keys, IME, mouse
                                                                         │          │   └ TerminalRenderer Metal: atlas, quads
                                                                         │          └ DeckConnection ── local │ remote (wss)
 ───────────────────────────────────────────────────────────────────────┼──────────────────────┼─────────────────────────
 C ABI            DeckBridge.h          db_deck_*  db_conn_*  db_term_*  db_layout_*  db_key_*  db_mouse_*
 (C++17 inside)   DeckCoreBridge        DBDeck: core::Deck + core::Server, the pump, a frame queue per window
                  DeckMacSeams.h        dms_host_create → DBHost: every OS seam + the inbox
 ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 C++17, shared    core/deck  core/vt  core/proto  core/model  core/session        (Sources/DeckCore/core → ../../../core)
 with Windows     windows/src/mouse.hpp (pure; one file, by symlink)
```

- **Shared C++ (nothing of it is copied):** the deck (`core/deck`: tiles, the call table, clients and windows, meta,
  the driver rules, persistence, the phone / desk server down to the websocket frames), the terminal (`core/vt`),
  the frames (`core/proto`), the placement rules (`core/model/layout`).
- **C++ glue with a C face (`DeckCoreBridge`, ~740 lines):** what `windows/host/localdeck.cpp` +
  `windows/src/deckhost.hpp` are for the Windows shell, minus every HWND: options → `core::DeckOptions`, the pump,
  windows as clients, and plain-function access to the terminal, the layout, the key table and the mouse table.
- **C++ / Objective-C++ for the OS (`DeckMacSeams`):** POSIX and libproc in `.cpp`; ONE `.mm` file (URLSession).
- **Swift (`ClaudeDeck`, ~2,750 lines in 12 files):** everything a person sees and touches.

### Why a C façade, and not the core's headers through Swift's C++ interop

NATIVE-PORT.md says the Swift shell "imports the C++ core directly (Swift 5.9+ C++ interop)". The core's public
headers are `std::function` sinks, `nlohmann::json` values, abstract classes a shell must SUBCLASS (the seams),
templates and exceptions. To this session's knowledge (Swift 5.10 / 6.0 — **check the current interop status page
before relying on any of this**): a C++ class cannot be subclassed from Swift (the seams are abstract classes a
shell must implement), `std::function` parameters are not usable from Swift, class templates import only when
instantiated and named, and a C++ exception crossing into Swift ends the process. Importing `json.hpp` (25,000
lines of templates) into every Swift file is also a compile-time cost.

So the boundary is `Sources/DeckCoreBridge/include/DeckBridge.h`: opaque handles, plain structs, UTF-8 with
lengths, C function pointers with a context pointer, every constant a `#define` of one literal. It is valid C99
(checked by `Tests/build.ps1`), so **the Swift target does not need `-cxx-interoperability-mode` at all** — and
`Package.swift` leaves it off. That is a deliberate departure from the brief's wording, not from its intent: the
core is still linked in-process and called directly; nothing is serialised that the Windows shell does not also
serialise. To experiment with direct imports later, the one line is in `Package.swift`.

What crosses the boundary as JSON text: `meta`, call arguments and results, event payloads (a few KB every 2 s).
What does NOT: pty bytes (`DB_FRAME_DATA` hands Swift the core's own buffer), terminal cells (`db_term_read_line`
fills a `DBCell` array), layout geometry, keys, mouse reports.

### One connection type, two transports

`DeckConnection` is a window's link to a deck. LOCAL: a client of the in-process `DBDeck`; frames are direct calls.
REMOTE: the same frames as websocket text to `wss://<deck>/desk/ws?t=<token>` (`URLSessionWebSocketTask`). Both
build every outgoing frame with `core/proto/frames.hpp` (through `db_conn_*`) and receive one `DBFrame` per frame,
so `DeckWindowController` has a single `if conn.isLocal` for the things R8 forbids a remote (terminal replies,
mouse and focus reports, 0x16 on an empty paste). `Tests/bridge_test.cpp` runs the two ends against each other: a
remote connection of the bridge talking to the bridge's own server over an in-memory websocket.

### Threads

**The main thread owns the deck.** `DeckHost.pump()` runs on it: `db_deck_pump` drains the inbox into the core
(`onPtyData` / `onPtyExit`, `Server::onAccept` / `onBytes` / `onClosed`, finished HTTP), runs `Deck::tick` and
`Server::tick`, and delivers each window's frames. It is called on the inbox's wake and from one
`DispatchSourceTimer` re-armed to `db_deck_next_due_ms()` (never more than 2 s) — no 50 ms polling.

Other threads **only enqueue** (`db::Inbox`, a mutex and a deque, in `DeckBridgeHost.hpp`):

- one thread per live pty (`MacPty.cpp`): polls the master, pushes bytes, reaps the child, pushes the exit last;
- one thread for all sockets (`MacListener.cpp`);
- URLSession's delegate queue for a usage / cloud request (`MacHttp.mm`), when the owner has allowed polling.

The inbox's wake callback is the one function called off the main thread; it does `DispatchQueue.main.async`.
Frames for a window are queued and delivered when the OUTERMOST bridge call returns, so a reply never re-enters a
window from inside its own send (the Windows shell posts them for the same reason; the test checks it).

### Chrome: AppKit, not Metal

The terminals are Metal (one `MTKView` per window, §7's rules below). The chrome — sidebar tiles, the pane name
bands and borders, menus, panels — is **AppKit views drawn from `renderer/style.css`'s numbers** (`Theme.swift`
holds every colour and size, each named for its CSS variable or rule). Reasons: AppKit gives text layout,
truncation, accessibility, text fields with IME, `NSMenu`, drag and drop and `WKWebView` hosting for free, and none
of it is the hot path; the Windows shell draws its chrome by hand because Win32 gives none of those. The cost is
that pixel parity with the Electron sidebar must be checked by eye (`PARITY.md`), where Windows has a pixel diff.

### The ask pane: the WebKit built into macOS (2026-11-23)

The ask pane (renderer/js/49-asks.js; the Windows shell's `windows/src/asks.hpp` over WebView2) is ONE `WKWebView`
over the terminal area: light (about 40-80 MB while open, the WebContent process going with the last web view), with
no function of the Windows pane dropped. `AskPane.swift` + `AskHostPage.swift`; the rows are in `PARITY.md`
("renderer/js/48-cloud.js, 49-asks.js").

- **The host document IS 49-asks.js's pane, as JavaScript** (`AskHostPage.swift`, served from memory): openAskPane,
  paintAskPane, the sections, the pages' bridge, Done / Copy all / Discard, the log view, guest read-only - the
  same functions, elements and css rules. Each ask page is the skill's own, in a sandboxed `<iframe>`. Where the
  Windows pane computes every text in C++ and its document decides nothing, here the logic stays in the language
  it was written in; Swift keeps only what is needed while no web view exists (`AskModel`: the asks tile, the
  marks, Cmd+Shift+A, Cmd+Shift+Enter on the every-session view, the tile menu, a clicked toast).
- **Swift <-> the document**: `window.deckIn(json)` one way, ONE `WKScriptMessageHandler` ("deck") the other, taken
  only from the MAIN frame at `deckask://pane` (a framed page reaches the deck only through the document's relay,
  which checks the frame, its page id and its opaque origin). The deck calls are the window's own connection
  (`askDraftGet`, `askDraft`, `askClear`, `askDone`, `askLogState`): drafts are the deck's, shared with the Electron
  deck, the phone and every window, local or remote alike.
- **What loads** (`AskSchemeHandler`): `deckask://pane/host` and `deckask://pane/ask/<id>.html`. LOCAL: the page
  FILE the meta names (only `ask_*.html`). REMOTE: the deck's `/ask/` route fetched by URLSession with the deck's
  cookie set on that request, so the token never enters the web view (the Windows pane sets it as an HttpOnly
  cookie in an InPrivate profile). Every page under the deck's sandbox CSP, in a sandboxed frame; the data store
  non-persistent. Anything else is cancelled; a link goes out (LOCAL: the deck's `askOpenLink`; a remote: http(s)
  in the default browser).
- **An older page's own Done** (a download): `WKDownload` saves `ask_<id>.answers.json` / `.txt` in the first of
  the ask feed's Done folders that exists (`db_deck_ask_downloads_json`), LOCAL only; anything else is cancelled.
- **Its life**: made when the pane opens (or the pointer rests 150 ms on the asks tile), closed 5 s after it closes,
  2 min after in a minimised window (made again when the window comes back), made again if its WebContent process
  dies. While it is open nothing typed reaches a terminal behind it; the deck's Cmd keys still work from inside a
  page (AppDelegate's local key monitor); Esc inside a page stays the page's.
- **Not run on a Mac yet.** It compiles (the mac CI); the host document's JavaScript is run here in headless Edge
  (`Tests/askhost-test`, Blink); the Swift around it - the scheme handler, the message handler, keys, links,
  downloads, the web view's life - has never run. To try it on a Mac: `swift run ClaudeDeck --test --state-dir
  /tmp/deck-scratch` with `ASK_PAGE_STATE_DIR` naming a scratch ask-page state, then Cmd+Shift+A or the asks tile.

### The renderer (docs/NATIVE-PORT.md §7)

| Rule | Where it is kept |
|---|---|
| Render only placed terminals | `TerminalRenderer.placements` holds only what the window's layout shows; a tile nobody shows has no `TerminalPane` in that window at all (`DeckWindowController.panes`), and is not attached |
| Damage tracking | `TerminalPane.dirty` (from `vt::Terminal::takeDamage`) → `TerminalRenderer.update()` rewrites only those rows' instances; a frame with no damage is never requested (`MTKView.isPaused`, `enableSetNeedsDisplay`) |
| Persistent atlas, instanced quads, one draw call | `GlyphAtlas` (one per process, shared by every window) rasterises a cell's text once with CoreText — in `update()`, **never in `draw(in:)`**; `draw(in:)` is one `drawPrimitives(instanceCount:)` over one buffer holding every pane's cells and cursors |
| `MTLResourceStorageModeShared` | the instance buffer and the atlas texture |

Not in the draft: ligatures / shaping across cells (each cell is shaped alone, as xterm's DOM renderer does), curly
and dotted underlines (drawn straight), the scrollbar, the mirror's font shrink.

## Layout

| Path | What | Lines |
|---|---|---|
| `Package.swift` | SwiftPM: `DeckCore` (C++, the repo's `core/`), `DeckCoreBridge`, `DeckMacSeams`, `ClaudeDeck` | 68 |
| `Sources/DeckCore/core` → `../../../core` | **a symlink**: the same sources the Windows shell compiles | — |
| `Sources/DeckCore/windows/src/mouse.hpp` → `windows/src/mouse.hpp` | **a symlink** to the one pure header shared from the Windows tree | — |
| `Sources/DeckCoreBridge/include/DeckBridge.h` | the C ABI (the Swift module is this header only) | 341 |
| `Sources/DeckCoreBridge/include/DeckBridgeHost.hpp` | `DBHost` + the inbox: what a seam library fills in (C++, not in the Swift module) | 74 |
| `Sources/DeckCoreBridge/DeckBridge.cpp` | the façade | 737 |
| `Sources/DeckMacSeams/include/DeckMacSeams.h` | `dms_host_create`, the state folder, the instance lock, the exit watchdog, relaunch | 63 |
| `Sources/DeckMacSeams/MacSeams.hpp` | the seam classes | 120 |
| `Sources/DeckMacSeams/PosixSeams.cpp` | Clock, Files (atomic replace by rename, `F_FULLFSYNC`), Random, Env (the login shell's environment), ProcessList (libproc), Net (getifaddrs, `tailscale serve status`), the process helpers | 383 |
| `Sources/DeckMacSeams/MacPty.cpp` | PtyHost: `forkpty`, node-pty's termios, `TIOCSWINSZ`, SIGHUP + SIGKILL to the group, exit after the last byte | 235 |
| `Sources/DeckMacSeams/MacListener.cpp` | Listener: BSD sockets, one poll thread | 163 |
| `Sources/DeckMacSeams/MacCredentials.cpp`, `KeychainNames.hpp` | Credentials: `.credentials.json`, else the Keychain through `/usr/bin/security` exactly as `lib/platform.js` | 128 + 57 |
| `Sources/DeckMacSeams/MacHttp.mm` | Http: URLSession | 63 |
| `Sources/DeckMacSeams/MacHost.cpp` | all of the above as one `DBHost` | 51 |
| `Sources/ClaudeDeck/main.swift`, `AppDelegate.swift` | the process: modes, menu bar, windows, Cmd+W / Cmd+Q rules, toasts, Dock bounce, restart, the instance lock | 11 + 264 |
| `Sources/ClaudeDeck/DeckHost.swift` | the local deck and its pump; the shell's Notifier / Lifecycle callbacks | 161 |
| `Sources/ClaudeDeck/DeckConnection.swift` | local / remote connection; `GridLayout` | 277 |
| `Sources/ClaudeDeck/DeckModel.swift`, `Theme.swift` | the meta as values; the look as numbers | 108 + 43 |
| `Sources/ClaudeDeck/DeckWindowController.swift` | one window: selection, layout, chord, attach / shown / resize, meta, keys | 414 |
| `Sources/ClaudeDeck/TerminalPane.swift` | one terminal's view state; key code → the shared key table | 183 |
| `Sources/ClaudeDeck/TerminalRenderer.swift` | the atlas, the instances, the shader | 345 |
| `Sources/ClaudeDeck/TerminalAreaView.swift` | keys (`NSTextInputClient`), Option as Meta, mouse, wheel, copy / paste, drop target, pane chrome | 440 |
| `Sources/ClaudeDeck/SidebarView.swift` | tiles, bars, tools, rename, close confirm, drag source | 315 |
| `Sources/ClaudeDeck/Panels.swift` | the panel structure; the palette worked (its ask entries too); `WebPanel` (structure, for the cloud panes) | 207 |
| `Sources/ClaudeDeck/AskPane.swift` | the ask pane: `AskModel`, the WKWebView's life, the deck calls, links, downloads, `AskSchemeHandler` | 562 |
| `Sources/ClaudeDeck/AskHostPage.swift` | the ask pane's host document (49-asks.js's pane as JavaScript) | 575 |
| `Tests/bridge_test.cpp`, `build.ps1` | the façade on fake seams, as a Mac configures it — **runs on Windows** | 511 + 73 |
| `Tests/gen-keychain-names.js`, `keychain-names.json` | vectors from the real `lib/platform.js` | |
| `Tests/askhost-test/run.ps1`, `harness.js`, `fakepage.html` | the ask pane's host document in headless Edge against fake pages speaking the skill's host bridge — **runs on Windows** | |
| `Tests/seams_smoke.cpp` | every real seam once, then the deck on them — **for the Mac, unrun** | 133 |
| `tools/make-app.sh` | wrap the build in `Claude Deck Native.app` (unsigned) | 50 |
| `PARITY.md` | the Windows parity rows, with this shell's status | |

### How the core is pulled in — the same sources, never a copy

SwiftPM only builds files under the package root, and the package root is `mac/`. So `Sources/DeckCore/core` is a
git **symlink** (mode 120000) to `../../../core`, and `Sources/DeckCore/windows/src/mouse.hpp` one to the Windows
tree's pure mouse header. Every C++ target adds `headerSearchPath("../DeckCore")`, so `#include
"core/deck/deck.hpp"` and `#include "windows/src/mouse.hpp"` are spelled the same in a SwiftPM build and in
`Tests/build.ps1` (which passes `-I <repo root>`). On a Windows checkout with `core.symlinks=false` the two links
are small text files; nothing on Windows reads them.

**If SwiftPM will not follow the symlink** (`VERIFY` in `Package.swift`): the fallbacks, in order of preference —
(1) move `Package.swift` to the repo root with `path: "core"` / `path: "mac/Sources/…"`, nothing else changes;
(2) build the core + bridge + seams as one static library with the clang++ lines below and link it from a
`systemLibrary` / `binaryTarget`; (3) an Xcode project with the folders added by reference.

### Toolchain assumed

**Xcode 15.3 or later (Swift 5.10), macOS 13 deployment target, Apple Silicon first.** `swift-tools-version:5.9` is
the floor: mixed C++ / Objective-C++ targets with `cxxLanguageStandard: .cxx17` are settled there. Nothing needs
Swift 6 or strict concurrency (the app is main-thread code with completion handlers; no `async`). The shader is
compiled from source at launch (`makeLibrary(source:)`), so plain `swift build` works without Xcode's `.metal`
build step. `UNUserNotificationCenter` needs a bundle: `swift run` gives a deck without toasts;
`tools/make-app.sh` gives the real thing.

## First-build checklist (for the first session with a Mac)

**Headline: prove the core on clang first, then each layer upward — core tests → the bridge test → the seams
smoke test → `swift build` → a window with one shell tile → remote mode. Do not start with `swift build`.**
Each step isolates one layer, so an error names its own cause. Keep a list of what you change; fix `// VERIFY:`
marks as you meet them and delete the mark.

1. **The core alone, with Apple clang** (these mirror `core/test/*/build.ps1`; from the repo root):
   ```
   clang++ -std=c++17 -O2 core/model/layout.cpp core/model/hibernate.cpp core/model/prompts.cpp core/model/triggers.cpp core/test/run.cpp -o /tmp/deck-model && /tmp/deck-model
   clang++ -std=c++17 -O2 -Wall core/vt/terminal.cpp core/vt/buffer.cpp core/vt/parser.cpp core/vt/unicode.cpp core/test/vt/vt_test.cpp -o /tmp/deck-vt && /tmp/deck-vt core/test/fixtures/vt
   clang++ -std=c++17 -O1 -Wall core/test/proto/proto_test.cpp -o /tmp/deck-proto && /tmp/deck-proto core/test/fixtures
   clang++ -std=c++17 -O1 -Wall core/session/*.cpp core/test/session/session_test.cpp -o /tmp/deck-session && /tmp/deck-session core/test/fixtures/session
   clang++ -std=c++17 -O1 -Wall -Wextra core/deck/*.cpp core/session/*.cpp core/model/layout.cpp core/test/deck/*.cpp -o /tmp/deck-deck && /tmp/deck-deck core/test/fixtures/deck "$PWD"
   ```
   Expected on Windows today: model all passed, vt 184/184 × 3, proto 51/51, deck 27,852 passed. **Expect on a Mac:**
   missing `#include`s that libstdc++ supplied transitively and libc++ does not (`<cstring>`, `<cstdio>`,
   `<algorithm>`, `<cmath>`) — add them in the core, they are harmless on Windows; warnings libc++ / clang add
   (`-Wunused-lambda-capture`, sign compares); and, in the deck test's `server` section only, failures where
   `fake::DiskFiles` opens the repo's pages with `\`-joined paths (the test's fixtures are Windows-shaped; the core
   under test is not at fault — the bridge test below is the POSIX-path proof).
2. **The bridge, with clang** (the test that passes on Windows today, unchanged):
   ```
   clang++ -std=c++17 -O1 -Wall -I . -I mac/Sources/DeckCoreBridge/include -I mac/Sources/DeckMacSeams \
     core/deck/*.cpp core/session/*.cpp core/vt/*.cpp core/model/layout.cpp \
     mac/Sources/DeckCoreBridge/DeckBridge.cpp mac/Tests/bridge_test.cpp -o /tmp/deck-bridge && /tmp/deck-bridge mac/Tests
   ```
   → `125 passed, 0 failed`. This is the deck configured as a Mac (`darwin`, `/` paths, `$SHELL -l`) on fakes.
3. **The seams, for real** (starts `/bin/sh`, `/bin/cat`, your login shell; binds 127.0.0.1:8811–8812; writes
   under `/tmp`; never Claude, never the Keychain):
   ```
   clang++ -std=c++17 -O1 -Wall -I . -I mac/Sources/DeckCoreBridge/include -I mac/Sources/DeckMacSeams -I mac/Sources/DeckMacSeams/include \
     core/deck/*.cpp core/session/*.cpp core/vt/*.cpp core/model/layout.cpp mac/Sources/DeckCoreBridge/DeckBridge.cpp \
     mac/Sources/DeckMacSeams/PosixSeams.cpp mac/Sources/DeckMacSeams/MacPty.cpp mac/Sources/DeckMacSeams/MacListener.cpp \
     mac/Sources/DeckMacSeams/MacCredentials.cpp mac/Sources/DeckMacSeams/MacHost.cpp \
     -x objective-c++ -fobjc-arc mac/Sources/DeckMacSeams/MacHttp.mm -x c++ mac/Tests/seams_smoke.cpp \
     -framework Foundation -framework CoreFoundation -o /tmp/deck-seams && /tmp/deck-seams
   ```
   Fix one seam at a time, in the file's order: clock / random / env → files → processes → `runCapture` → the pty
   → the listener → the deck on all of them. The pty is where the time will go (see below).
4. **`cd mac && swift build`.** First the three C-family targets (`swift build --target DeckMacSeams`), then the
   app. This is where the symlink, the two `module.modulemap`s and every Swift `VERIFY` are met.
5. **A window.** `swift run ClaudeDeck --test --state-dir /tmp/deck-scratch` — a scratch local deck: no Claude, no
   ask feed, loopback only, no login-shell probe. Expect, in order: a window with an empty sidebar; "New terminal"
   (`>_`) makes a tile; its pane shows a prompt; typing works; the window resizes the pty. Check the glyphs are
   the right way up and on their baseline (`GlyphAtlas.rasterise` has the one `VERIFY` that decides it).
6. **Input.** Arrows, Ctrl+C, Option+B (ESC b), a dead key (Option+E then E), an input method (marked text at the
   cursor), Cmd+C / Cmd+V, the wheel, a selection, `vim` (alternate screen), `htop` or `less` (mouse).
7. **Layout.** Two terminals; Option+1+3 and Option+2+4 put them side by side; drag a tile onto a pane; drag one
   out of the window (a second window); Cmd+W on the last window, then the Dock icon (the sessions are still there).
8. **Remote mode** against a scratch Electron or Windows deck: `swift run ClaudeDeck --remote 'http://127.0.0.1:<port>/?t=<token>'`.
9. **The bundle:** `swift build -c release && tools/make-app.sh`, then toasts, the Dock bounce, a restart.
10. Only then, and only on the owner's word: `--allow-claude` with a real account; usage polling (the Keychain).

### What is most likely to break, in the order you will meet it

1. **libc++ vs libstdc++ in the core** (step 1): missing includes. Mechanical.
2. **`forkpty` details** (step 3): `<util.h>` vs `<pty.h>`; the child between `fork` and `exec` may only make
   async-signal-safe calls (the code builds argv / envp before the fork for that reason — keep it so); a grandchild
   holding the slave open delays EOF (the reader stops waiting two quiet polls after the child is reaped); exit
   code on death by signal (`VERIFY` against node-pty: the draft reports 0).
3. **SwiftPM and the symlinked `core`** (step 4), and `exclude:` paths under it. Fallbacks above.
4. **The hand-written module maps** — if SwiftPM insists on its own umbrella, move `DeckBridgeHost.hpp` /
   `MacSeams.hpp` out of `include/` into the target's root (they are found through the header search path anyway).
5. **C → Swift import shapes**: `const char* const*` parameters (`db_conn_shown`, `db_layout_prune`,
   `dms_relaunch` — the Swift builds `[UnsafePointer<CChar>?]` with `strdup`), optional function pointers in
   `DBShellCallbacks`, `OpaquePointer` for every handle (no type safety between a `DBDeck*` and a `DBConn*`:
   wrapping each in a Swift class, as `GridLayout` and `TerminalPane` already do, is the cure).
6. **`@convention(c)` closures**: every callback handed to C must capture nothing (they take `self` from the
   context pointer). The compiler says so when one does.
7. **AppKit overrides**: which methods are `override` (`selectAll(_:)`, the dragging-destination four) and which
   are plain `@objc` (`copy(_:)`, `paste(_:)`); `NSTextInputClient`'s exact signatures.
8. **Key routing for Cmd combinations**: the deck's Cmd keys are taken by a local event monitor
   (`AppDelegate.installKeyMonitor`) because they are deliberately not menu items; check none fires twice.
9. **Metal**: the instance struct's layout (40 bytes: `MemoryLayout<CellInstance>.stride` must equal the shader's
   packed struct — assert it), premultiplied alpha and row order out of the `CGContext`, `.shared` storage for a
   texture on an Intel Mac.
10. **`UNUserNotificationCenter` without a bundle** raises: guarded by `Bundle.main.bundleIdentifier != nil`.

### Open questions for the core's author (not answerable from here)

- **Windows opened after `Deck::start()`**: does the second `addWindow` still take the second saved entry (R7)?
  `AppDelegate` opens window 1, starts, then opens one window per further saved entry; `windows/src/app.cpp` reads
  the count itself before `start()`.
- **Cmd+W on the last window, then the Dock icon**: the new window is a fresh `addWindow`. Which saved entry and
  index does it take — the closed one's, or a new one? On a Mac this is the normal way to come back.
- **`ProcessList::commit`** (the memory guard) has no macOS reading in the draft. What does `main.js` feed
  `lib/hibernate.js` on a Mac today?
- **The adopting window in the sink**: while `addWindow` runs, the bridge sends frames for a client id it does not
  know yet to the window being added (as `deckhost.hpp` does). A meta for a REMOTE client raised inside that call
  would be misrouted. Returning the id before any frame is sent (or a `Deck::nextClientId()`) would close it.

## What the Windows shell and the core could change to make sharing easier

None of these was done (this branch touches `mac/**` only); each is a move, not a rewrite.

1. **The key table → one file both shells include.** `windows/src/keys.hpp` `keyToBytes` / `pasteBytes` are pure
   but keyed by `VK_*` and include `<windows.h>`. `db_key_bytes` / `db_paste_bytes` in `DeckBridge.cpp` are the same
   table keyed by a neutral `DB_KEY_*` (the bridge test checks every row). Move it to `core/input/keys.hpp` and
   have `keys.hpp` map `VK_*` → that enum; the window-key half already lives in `core/deck/platform.cpp`.
2. **`windows/src/mouse.hpp` → `core/input/mouse.hpp`.** It is already pure and already shared (by symlink); it
   only lives in the wrong folder.
3. **`buildDeckOptions` → the core.** `windows/host/localdeck.cpp buildDeckOptions` and `DeckBridge.cpp
   buildOptions` are the same function with `wstring` vs `const char*` inputs. One `core::buildDeckOptions(plain
   struct, Env&, Files&)` would make "every option the core grows is set in ONE place" true for both shells.
4. **The inbox + pump + windows-as-clients → shared.** `db::Inbox` and `DBDeck`'s pump / per-window frame queue
   are a platform-neutral rewrite of `Shared` + `DeckHost` in `windows/src/deckhost.hpp` (PostMessage replaced by
   a wake callback). The Windows shell could sit on `DeckBridgeHost.hpp` directly and drop its own copy.
5. **Selection text → the core.** `db_term_text` (trim, join wrapped lines) is terminal logic; `vt::Terminal`
   could own it so both shells copy the same text.
6. **`core/test/deck/fakes.hpp` path shape.** `fake::norm` forces `\`; a POSIX twin (as in `bridge_test.cpp`)
   would let the deck's 27,852 checks run in `darwin` mode too, which is the configuration the Mac ships.
