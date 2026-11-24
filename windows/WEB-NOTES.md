# The web pane and the ask pane (native Windows shell)

Written 2026-11-18 by the worker that built `windows/web/**`, `windows/web-test/**`, `windows/third_party/webview2/**`
and `windows/src/asks.hpp`. **Wired into `app.cpp` the same night: "Wired in" below says what was done and where
it differs from the plan.** "Wiring it in" is kept as the plan it was.

## Wired in (2026-11-18, later that night)

`build.ps1` compiles `web/webview2_loader.cpp` and `web/webpane.cpp` into the exe (no new link library; the exe
went from 3,272,192 to 3,628,032 bytes). In `app.cpp` (search `askPane`): the member and `initAsks()` (the Deps),
`askSync()` at the top of every frame and of every WM_SIZE (layout + cover), `askPane.draw` in `drawMainOver`,
the `askpane` hit kind (everything under the pane takes no click; the bar's x closes), `windowKey` on
`keys.hpp windowKeyOf` (F6 asks the asks first, F7 is `asksKey`), Esc in `onKeyDown`, `termKeys()` (nothing typed
reaches a terminal behind the pane), the asks tile's click, the tile menu's first entry, the palette's four
entries, `view:asks` / `view:zoom` from the deck, `setMeta` on every meta, `setLink` at start and on a deck
switch, WM_MOVE, WM_ACTIVATE / WM_SETFOCUS / WM_KILLFOCUS, and the shutdown on every way out. The sidebar draws
the tile selected, the picked row, and the selected session's tile plain meanwhile (`ui::State::asksPick`).

What differs from the plan below:
- **Keys from the page.** `asks.hpp` no longer hands every accelerator key to the app and takes its answer: it
  decides with `keys.hpp webKeyOf` (pure: App / ClosePane / Page) and returns handled for the app's keys whatever
  the app did with them. `windowKey` runs on the same table (`windowKeyOf`), so the two roads cannot disagree.
  F11 / F12 from the page commit the walk at once (no later key of the page's would reach the app to end it).
- **`AcceleratorKeyPressed` is no longer unproven.** A key POSTED to `Chrome_WidgetWin_1` (the browser's own top
  window under ours, which has this thread's focus after `MoveFocus`) raises the real event in a parked window; a
  key posted to `Chrome_RenderWidgetHostHWND` or a DevTools key event does not (what the first test tried). The
  component test now asserts it (F7), and the app's walk does F9, F6, F7 and Esc through it: Esc with the caret
  in a page's text box leaves the pane open, F6 / F7 close it and the keyboard is the app's window's again; Enter
  and Tab reach the page. STILL NOT PROVEN, and only a hand can: a key from the physical keyboard (the same event,
  but through the system's input queue), and Ctrl+Shift+P / Ctrl+` from inside a page (the handler reads the real
  modifier state with GetAsyncKeyState, which a posted key cannot fake). To try: open the asks, click into a text
  box, press F7 (closes), F6, Ctrl+Shift+P; Esc in a text box must NOT close the pane, Esc after a click on a
  header must.
- **Focus.** Opening the pane gives the page the keyboard (again when its document is ready); a press anywhere in
  the app's own window takes it back (WM_LBUTTONDOWN's `SetFocus`); closing or covering the pane gives it to the
  window, and uncovering gives it back to the page if the page had it. WM_KILLFOCUS towards a child of the window
  (the page) is not "the window lost the keyboard"; WM_ACTIVATE notes that the page had it when the window is
  left, and WM_SETFOCUS hands it back on return. The last two (Alt+Tab away and back) are written, not exercised.
- **Cover** is computed in one place (`overAskPane`): the palette, any context menu, the deck-away banner, a
  dragged tile, and a panel whose box meets the terminal area.
- **Lazy.** Nothing of WebView2 is touched until the pane opens (`ensureWeb`) or the pointer rests on the asks tile
  (`prewarm`, timer `TIMER_ASKWARM`; never in a test unless `--web-warm 1`, never without a window). (SUPERSEDED
  2026-11-19: it used to be made 1.5 s after any meta in which a session asked, which kept a browser process tree
  alive for good: "Memory" below.) `create()` costs 0 to 16 ms on the window thread (the rest is asynchronous);
  cold, the first page says "ready" 500 to 765 ms after the click; warmed, about 140 ms. Until the page is there the pane shows its bar over the dark body (and
  the "No questions waiting" line when nothing waits), as the Electron pane does before its frames load.
- **Shutdown.** `beginShutdown()` closes the web view at once, the socket's goodbye runs, then `shutdown(3000)`
  waits for the browser process (only when this window's views were the last of the process, and only for a
  process a view had until a moment ago) and removes a profile folder of its own making, if it had one: 170 to 440 ms in the runs, the goodbye unchanged
  (`close.ps1`: ok). A `closing` flag keeps queued frames, timers and paints from running while that wait pumps
  messages.
- **The scroll to a row** is taken again while the pages above it grow (see PARITY).
- **Tests.** `--test-asks` (askDone / askClear really sent, loopback decks only), `--web-warm`, `--web-runtime`,
  `--do-file`, the `ask …` steps (`script.hpp`), `windows/test/asks.ps1` + `asks-walk.txt` + `asks-e2e.txt`.
  F10 does nothing in a test run (fullscreen would put the parked window on a screen).
- Not done: `markInfo`'s in-progress count on the sidebar's marks before the deck's next meta (they follow the
  meta, a moment later).
- Done later the same night: the web view is hidden and suspended while the window is minimised
  (`AskPane::minimised`); a deck switch removes the old profile folder as soon as its browser process has gone
  (`AskPane::retire`); the discard confirm, a legacy page and a replaced page are scripted
  (`windows/test/asks-more.txt`, `asks.ps1 -Mode legacy`); the pane at 150 % (`asks.ps1 -Dpi 144`).

## Memory: when a web view exists, and one browser process tree (2026-11-19)

**Measured on the owner's home PC (11.7 GB, after a low-memory event):** the exe as a remote client held SEVEN
`msedgewebview2.exe` children, about 400 MB of working set, possibly with no ask pane and no cloud pane open. Three
causes, all found in the code and all reproduced by `windows/test/webmem.ps1` on the build before this change:

1. **The ask pane's web view was always alive.** `app.cpp` made it 1.5 s after any meta in which a session was
   asking (`TIMER_ASKWARM`, "a warm-up"). A deck that always has asks waiting therefore always had a whole browser
   process tree, pane open or not: 6 processes, 275 MB, with nothing on screen.
2. **Two browser process trees.** The ask pane ran in a WebView2 environment of its own (a private temp profile),
   the cloud panes in another (`<state>\web-cloud`): two browser processes, two GPU processes, two network and two
   storage services, two crash handlers.
3. **Nothing was ever closed.** A closed ask pane kept its web view (and the ask pages loaded in it) for good; a
   cloud pane that left the screen was dropped only after 15 minutes, and one HIDDEN behind the ask pane, a zoom
   or a minimised window never (its idle clock was wound back on every frame).

### What it does now

| | |
|---|---|
| **No web view because a session is asking** | the meta warm-up is gone. The ask pane's web view is made when the pane OPENS (F7, F6 on the asks tile, a click, the tile menu, the palette, `view:asks`, `asks:open`), or a moment ahead when the pointer RESTS on the asks tile for `ASK_WARM_HOVER_MS` (150 ms; `TIMER_ASKWARM`, called off when the pointer leaves first). Never in a test unless `--web-warm 1` |
| **The ask pane closed** | its web view lives `ASK_WEB_GRACE_MS` (5 s since 2026-11-23; it was 20 s) more, so opening it again straight away is instant, then it is closed (`AskPane::armIdle` / `dropWeb`). A re-open after that loads the pages again: the drafts come back from the deck (`askDraftGet`) and the place from the scroll target (webmem.ps1 "lowmem" checks both). A view warmed by the pointer and never opened goes the same way |
| **The ask pane open, the window minimised** | hidden and suspended at once (as before), closed after `ASK_OFFSCREEN_CLOSE_MS` (2 min); made again on restore. Nothing typed is lost: drafts are the deck's (`askDraft`), the pages load them back |
| **A cloud pane off screen** (its tile left the layout, a zoom is in front, the ask pane is over the area, the window is minimised) | hidden and `TrySuspend`ed at once (as before), CLOSED after `CLOUD_OFFSCREEN_CLOSE_MS` (2 min, was 15 and only for a tile out of the layout). Back on screen it is built again and loads the session's page afresh, as the Electron pane does on a redraw. A timer (`TIMER_WEBIDLE`) runs the check while a pane is off screen: a minimised window draws no frames |
| **A cloud pane ON screen** | never closed, however long nobody touches it: a half-typed prompt is not lost. A pane shown as a still picture under an overlay counts as on screen |
| **The last web view of the process closes** | the browser process and its whole tree (GPU, network, storage, crash handler) exit by themselves, within about 0.2 s. **Zero `msedgewebview2.exe`** |
| **One browser process tree** | the ask pane and every cloud pane of every window use ONE user-data folder, `<state>\web-cloud` (the name it had when only the cloud panes used it: renaming it would sign everybody out), each through its own environment object with the SAME options: WebView2 then runs them all in one browser process. Profiles inside it: the ask pane = the InPrivate side of the folder's default profile; cloud = a named profile per account, as before |

The graces are named constants (`asks.hpp`, `cloud.hpp`); a test shortens them with `--web-grace <ask ms>,<cloud ms>`.

**The memory target (2026-11-23).** Every web view that stays - the ask pane's "web" fallback and the cloud
panes - asks its renderer for LESS memory while it is hidden (`ICoreWebView2_19::put_MemoryUsageTargetLevel(LOW)`,
`webpane.cpp` `applyMemoryTarget`, from `show(false)` and from a view made hidden) and for the normal amount again
when it shows (`show(true)`). The runtime trims what it can (caches, decoded images) and keeps the page: its script,
its scroll, its sign-in. A runtime older than `ICoreWebView2_19` has no such thing: skipped quietly (`memoryTarget()`
says -1). `ask dump` / `cloud dump` print it; webmem.ps1's "lowmem" run checks NORMAL shown, LOW hidden, NORMAL again,
and that a cloud pane hidden and shown again within its grace is the same page, still signed in.

**The NATIVE ask pane (2026-11-23, the default: config.json `askPane` "native" | "web").** `askcards.hpp` draws the
pages' cards itself (core/deck/askcards: the page file parsed, page.html's state and logic) and stands in for the
host document and the frames, so with it the ask pane makes NO web view at all: webmem.ps1 state B (the ask pane
open) has zero `msedgewebview2.exe`. Everything about the ask pane's web view in this file is the "web" fallback's.

### Before / after (`windows/test/webmem.ps1`, this box, the fake deck's `cloud4` mode)

Every number is of the `msedgewebview2.exe` processes that DESCEND from the exe (the exe walks its own process tree:
`webmem`, `app.cpp`); other programs' web views are never counted. Working set / private bytes, MB.

| State | Before: processes | Before: MB | After: processes | After: MB |
|---|---|---|---|---|
| A. idle, asks waiting, ask pane closed | 6 | 275 / 130 | **0** | **0** |
| B. ask pane open on its two pages | 7 | 366 / 207 | 7 | 363 / 206 |
| C. ask pane closed, past its grace ("+30 s") | 7 | 365 / 207 | **0** | **0** |
| D. one cloud pane | 12 (two trees) | 552 / 305 | 6 (one tree) | 315 / 158 |
| E. four cloud panes (two accounts) | 16 (two trees) | 868 / 447 | 9 (one tree) | 482 / 232 |
| F. the ask pane open over the four | 16 | 875 / 452 | 10 | 559 / 289 |
| G. cloud panes unplaced, past their grace ("+3 min") | 16 | 889 / 460 | **0** | **0** |
| H. one cloud pane again (built again, page loaded) | 16 | 730 / 460 | 6 | 286 / 141 |

What one tree is made of (state E, after): browser 139 / 57, crash handler 10 / 3, GPU 68 / 70, network service
37 / 12, storage service 20 / 8, and a renderer per pane, about 52 / 21 each. So the first web view costs about
275 MB of working set and each further cloud pane about 52. The stand-in pages are tiny: the real claude.ai is
heavier per renderer; the fixed part is the same.

### What a cold open costs now

The first open after the browser is gone (the common case now), three runs of five each, on this box:
`create()` 0 to 16 ms on the window thread; the web view ready 250 to 313 ms after the press; the host document
at 406 to 500 ms; **the first ask page ready 500 to 765 ms after the press**. Until then the pane shows its bar over
the dark body, as the Electron pane does before its frames load. Opened after the pointer rested on the tile: the
first page ready **141 ms** after the click. Reopened within the grace (5 s since 2026-11-23, 20 s before): nothing loads, it is a `show()`.

**A trap found on the way, and why the ask pane is on the default profile.** The first web view EVER made in a NEW
named profile takes about five seconds to come up (5297 to 5359 ms in four runs of five; a constant, so a timeout
inside the runtime), once per profile per folder; later cold starts in that profile take 0.3 s. The ask pane was
first given a named InPrivate profile (`deck-asks`) and showed exactly that stall; on the InPrivate side of the
default profile it never does. The cloud panes had this before this change and still have it: the first pane of
an account, the first time ever, is blank for about five seconds (the css background shows). Not fixed; nothing in
the WebView2 API was found that avoids it. It is a known runtime fault: MicrosoftEdge/WebView2Feedback issue 5557
("a non-default profile name makes CreateCoreWebView2ControllerWithOptions block about 5 seconds on a cold start;
the default profile does not"), open at the time of writing. Seen ONCE on the default profile too, in about thirty
cold opens measured (the second process on a folder a moment after the first had left it): the pane then shows its
bar over the dark body for those five seconds. `webmem.ps1` waits 7.5 s where a stall would spoil a measurement.

### The deck's token: what replaces "the temp profile is deleted on exit"

Before: a private temp folder per process, InPrivate, removed when the browser process had gone. Now the ask pane
is in the shared, PERSISTENT folder, so the guarantee has to hold without deleting anything:

- The view is created with `IsInPrivateModeEnabled` and nothing else of the profile (`ICoreWebView2ControllerOptions`);
  a runtime too old for that (before 101) is refused, as before. The pane's dump says what the RUNTIME says
  (`ICoreWebView2Profile::get_IsInPrivateModeEnabled`): `InPrivate (the runtime's word) 1`.
- The cookie is set through that InPrivate profile's cookie manager only, the token is in no URL (unchanged).
- When the pane's view closes, `WebPane::clearCookies()` (`DeleteAllCookies` on the InPrivate profile) runs first,
  with the last ask view of the process: the cookie does not outlive the pane even while the browser process
  lives on for a cloud pane. (The InPrivate profile dies with its last view anyway; this does not rely on it.)
- **Verified on disk, three ways** (`webmem.ps1`, every run):
  1. after a normal run: every file under the state folder is searched for the token and for the cookie's NAME
     `deck_phone`, as bytes and as UTF-16: **0 files** (457 searched, none unreadable);
  2. **after a crash with the pane open** (the `crash` step: `TerminateProcess` with the pane up, its pages loaded
     with the cookie, an answer picked; nothing closed, nothing cleared): the same search, **0 files**;
  3. the control: the walk signs in to the stand-in page in a cloud profile, and the same search FINDS that
     cookie's name (`fake_login`) in `EBWebView\WV2Profile_claude-vv@example.com\Network\Cookies`. So the search
     does see a stored cookie when there is one. (The name is searched because Chromium encrypts cookie VALUES on
     disk; a stored token would not show as its text.)
  The default profile's own cookie store (`EBWebView\Default\Network\Cookies`, 20480 bytes: an empty table)
  exists and holds neither.
- `webscan` (a test step) does the same search while the browser runs; three or four files are then locked by the
  browser (the cookie stores among them), which is why the proof is taken after the run and after the crash.
- With no state folder to share (a test without `--state-dir`, the component test) the pane falls back to the old
  private temp folder, removed as before; `WebPane` marks and removes only a folder it invented itself, never one it
  was given.

### Browser arguments (`AdditionalBrowserArguments`): each one measured

One command line for every view (`webpane.cpp kDefaultArgs`): environments on one folder must have the same
options or the second fails. `webmem.ps1 -Flags "<arguments>"` replaces it for a run.

| Argument | Measured (same walk, one run each) | Kept |
|---|---|---|
| `--no-first-run --no-default-browser-check` | nothing may put a window up; no memory effect | yes (as before) |
| `--disable-features=msEdgeTranslate` | no translate bar; no memory effect | yes (as before) |
| `--disable-features=msSmartScreenProtection` | 9 processes / 484 MB against 8 to 9 / 431 to 491 without: no difference beyond run-to-run noise | yes, **and it now covers the cloud panes too** (they had SmartScreen on): one command line is the price of one process tree. A pane's top document can only be at claude.ai, Anthropic or a sign-in page (`cloud.hpp paneUrl` refuses the rest before it loads); the alternative, SmartScreen on for the ask pane, would send the deck's own addresses to a reputation service |
| `--disable-background-networking --disable-component-update --disable-sync` | 9 / 497 MB (E), 7 / 356 (B): no fewer processes, the network service 34 / 10 against 38 / 12 | no: nothing measurable |
| `--disable-features=SpareRendererForSitePerProcess,msWebOOUI,msPdfOOUI` + `--renderer-process-limit=2` | 9 / 485 MB (E): identical. WebView2 keeps no spare renderer here, and the limit does not merge the renderers of separate web views | no |
| `--disable-gpu` | the GPU process 44 to 59 / 14 to 16 instead of 68 to 70 / 70 to 98 (private bytes of the tree 176 against 235 at E), each renderer about 11 MB more working set (ask pane), the total working set about the same | **no**: it trades private bytes for software rasterising of every page, and the real page (claude.ai, streaming text) was not measured, only the stand-in |

No argument removed a process: the tree is browser + crash handler + GPU + network + storage + a renderer per
view, and only CLOSING views shrinks it. That is where the saving is.

### Not verified
- The real claude.ai in a pane closed and rebuilt (its page loads afresh: what a half-typed prompt in a pane that
  was OFF screen for two minutes does is the site's own draft handling; an on-screen pane is never closed).
- The pointer-rest warm-up with a real mouse (the test moves the pointer with the window's own handler; a step
  cannot leave the tile within 150 ms, so "called off when the pointer leaves first" is by reading).
- Two deck windows with ask panes at once (the cookie is cleared with the LAST ask view: by reading).
- An OLD and a NEW exe running at once on the same state folder: their browser arguments differ for the cloud
  panes, so the second one's web view would fail to start until the first exits.

## What is there

| File | What |
|---|---|
| `windows/third_party/webview2/WebView2.h` | the SDK header, Microsoft.Web.WebView2 **1.0.4314.35**, unmodified; licences beside it (`VERSION.txt` says which is which). No binary |
| `windows/web/webview2_loader.hpp/.cpp` | what `WebView2Loader.dll` does, built in: finds the Evergreen runtime (registry `EdgeUpdate\ClientState\{F3017226-…}` `EBWebView`, then `Clients\{…}` `location` + `pv`; HKLM then HKCU, the 32-bit view; or a fixed-version folder), loads `EBWebView\x64\EmbeddedBrowserWebView.dll`, calls `CreateWebViewEnvironmentWithOptionsInternal`. Ported from webview/webview (MIT) |
| `windows/web/webpane.hpp/.cpp` | `deck::web::WebPane`: one WebView2 as a child of a window. The header names no WebView2 type |
| `windows/src/asks.hpp` | `deck::asks::AskPane`: 49-asks.js's model and view logic, the host document, the page bridge |
| `windows/web-test/` | `build.ps1`, `smoke.cpp`, `asks_test.cpp`, `fake-deck.js`, `fixtures/` (two pages made by the ask-page skill's own `ask_page.py build`) |

Runtime found on this box: **157.0.4341.60** (`C:\Program Files (x86)\Microsoft\EdgeWebView\Application\157.0.4341.60`).
Builds with MinGW-w64 g++ 16.1 (`-std=c++17 -Wall`, no warnings), static. `WebView2.h` needed nothing done to it: its
IIDs are `__declspec(selectany)` definitions, MinGW's `EventToken.h` exists, and the handler interfaces are
implemented by one small template (`Cb<I, Args…>` in `webpane.cpp`), no WRL.

### WebPane, in short
- `create(parent, rect in DIPs, scale, options)` is asynchronous (`onReady`); what is asked before it is ready (a
  cookie, a served document, a navigation, bounds, show) is queued in order. `false` + `state() == NoRuntime` when
  there is no runtime (`web::runtimeDownloadName()` is the name to show; `AskPane::draw` writes the note itself).
- Always off: dev tools, context menus, status bar, autofill, password saving, the browser's accelerator keys,
  zoom, pinch, swipe navigation, script dialogs, host objects, downloads (cancelled), permission prompts (denied,
  except clipboard-read, which the Electron frame also allows). `DefaultBackgroundColor` 0x0c0c0c: no white flash.
- A new window is never opened: `onNewWindow(url)`. A navigation of the top document or of a frame is cancelled
  unless it is what the host itself asked for, or `onNavigate` says yes. `WebPane::openExternal` opens http(s)
  only, refuses anything with a space, a quote or a control character.
- Profile: **InPrivate** (cookies and storage in memory only; a runtime too old for it, before 101, is refused
  rather than used with a profile on disk). In the app: inside the deck's one web folder ("Memory" above). With
  no folder given (`userDataDir` empty: the component test): `%TEMP%\ClaudeDeckNative-web\<pid>` with a marker file;
  `destroy(waitMs)` waits for the browser process and removes that folder (only a folder with the marker is ever
  removed), `WebPane::sweepStale()` removes the folders of dead processes (call it once at start: a crash leaves one).
- `serve(url, document)` answers one URL from memory; `addResourceFilter` + `onResource` answer a pattern.
  `setCookie` makes the deck's cookie as `lib/phone.js` does (HttpOnly, SameSite=Lax, Path=/).
- `capture` / `capturePng` (CapturePreview), `exec`, `execInFrame` (by the frame's `name`), `devtoolsCall`: tests.

### Why the list is in the web view (the one place this departs from the brief)
The brief said: the list around the page drawn with Gfx, the WebPane holding only the sandboxed frame. That works
for one page and fails for the pane as 49-asks.js lays it out: the pane is a SCROLLER of sections (a sticky header
each, then that session's pages at their full content height, then "recently answered"). With one child window per
page under Gfx-drawn headers, (a) a wheel turn over a page never reaches the app (the frame is another origin in
another process; nothing bubbles out), so the list could not be scrolled with the mouse over the very thing that
fills it, and (b) a child window cannot be clipped under a header drawn by Direct2D, so the header could not stick.
So: **the WebPane is the pane's `.ap-scroll`**. Its host document holds the same elements with the same css rules,
copied from `renderer/style.css` and 49-asks.js (`askSection`, `makeFrame`, `paintRecent`), so the layout is the
Electron pane's by construction. The host document decides nothing: `asks.hpp` computes every text, every disabled
state and which frames exist (`AskPane::render`, one JSON message), the document reports clicks and relays the
pages' postMessage protocol. Gfx draws the bar (`.ap-bar`: 26 px, title / hint / × on one baseline at 18 px,
measured in the scratch Electron deck and matched) and the no-runtime note.
The sandbox is kept twice: each page is in an `<iframe sandbox="allow-scripts allow-popups
allow-popups-to-escape-sandbox allow-downloads">` (the deck's own list) AND arrives under the deck's sandbox CSP;
the relay passes a message on only if it comes from a frame it made, names that frame's page id, and has an opaque
(or the deck's) origin. The host document's own CSP: `default-src 'none'; script-src 'unsafe-inline'; style-src
'unsafe-inline'; frame-src <deck>/ask/`.

### The token
The host document is served from memory **at the deck's origin** (`<deck>/__claude-deck-native/asks`, intercepted,
never sent), so the frames' requests to `/ask/<id>.html?embed=deck&theme=dark` are same-site and carry the cookie
`deck_phone=<token>`, set through the cookie manager of the InPrivate profile before the navigation. The token is in
no URL the web view loads, is never logged, and never reaches disk (InPrivate; searched for on disk after every
`webmem.ps1` run and after a crash with the pane open: "The deck's token" above).
The test asserts it: every `/ask/` request reached the real `lib/phone.js` with `auth: cookie`, no `t=` in any URL,
and `document.cookie` reads empty in the host document (HttpOnly).

## What the test proves (`windows/web-test/build.ps1`, exit 0 = all passed; last run: 92 checks, 0 failed)
`fake-deck.js` is the REAL `lib/phone.js` PhoneServer with fake deps (as `test/desk/desk-test.js` does), on
127.0.0.1:8795 with a made-up token; the exe connects with `ws.hpp` as the app does. Nothing shows on screen (parked
window, GUI exes, hidden node). Pictures in `-Out` (default `%TEMP%\deck-web-test-out`):

| Picture | What |
|---|---|
| `smoke.png` | step 1: a WebView2 from a MinGW build, no loader DLL |
| `asks-0-window-before.png` | the base line: this window's Direct2D frame alone |
| `asks-1-all.png` | every session's asks: two sections, the pages at their own height |
| `asks-2-answered.png` | a card clicked, another typed into: `askDraft` reached the deck as `[page id, { state, drafted: 2, answered: 2, later: 0, total: 5 }]`, the header reads "2 of 5 answered", the mark is filled |
| `asks-2b-covered.png` | `cover(true)`: the page as a still picture with an app overlay drawn over it |
| `asks-2c-window.png` | **the window itself** (PrintWindow): the Direct2D bar and the web view child together, eight frames presented after the page came up and none over it |
| `asks-3-done.png` | Done (a real mouse click through DevTools input): `askDone(sessionId, [{ id, answers }])` with the page's own answers JSON; Done reads Sent, the page is locked |
| `asks-4-session.png` | F7 on a session: only its section; Done with nothing answered flashes so |
| `asks-5-log.png` | after the session read its answers: the read-only "answered" view (`askLogState`) |
| `asks-6-recent.png` | the asks tile's "Recently answered" row: the view scrolled to the list |
| `asks-7-local.png` | no deck link: the page FILE served from memory at a made-up origin (the in-process deck's case) |

Also asserted: Copy all's text; closeOnDone closing onto the tile with the note; the tile toggling the
every-session view; F6 / F7 / Esc logic; the tile-menu labels; a `target=_blank` link arriving as an address and
opening nothing; `openExternal`'s refusals; no `msedgewebview2.exe` and no profile folder left behind.

**Not covered by the test:**
- **`AcceleratorKeyPressed` from the physical keyboard** (now raised by a posted key and asserted: "Wired in"
  above says what that does and does not prove).
- A scale other than 100 % ON A REAL MONITOR (this box's parked window is 96 dpi; `--dpi` forces the scale for the
  app's frame and, through `RasterizationScale`, for the page: PARITY "The web views at 125 / 150 / 200 %").
- The scrollbar is the engine's own; the Electron pane's is too, but Edge and Electron may draw it differently.

## Wiring it in (edits for `app.cpp` and `build.ps1`; nothing else changes)

**`windows/build.ps1`**: add `'windows/web/webview2_loader.cpp', 'windows/web/webpane.cpp'` to `$sources`, and
`windows\web`, `windows\third_party\webview2` to the `$headers` folders. **No new link library** (the test links
with exactly the app's list; `advapi32` is MinGW's default). `WebView2.h` is included by `webpane.cpp` only.

**`app.cpp`**:
1. `#include "asks.hpp"`; a member `asks::AskPane askPane;` (beware: a few functions have a local named `asks`;
   inside those write `deck::asks::`).
2. The window: add `WS_CLIPCHILDREN` to the `CreateWindowExW` style. Once at start `web::WebPane::sweepStale()`.
3. After the window exists (not in `--shot` / `--probe` runs: pass `nullptr` as the parent there and the pane
   draws its bar and no page):
   ```cpp
   asks::Deps d;
   d.call = [this](const std::string& fn, const json& a, auto done) { call(fn, a, [done](bool ok, const json& r) { done(ok, ok ? r : json(), ok ? "" : errText(r)); }); };
   d.fire = [this](const std::string& fn, const json& a) { fire(fn, a); };
   d.activate = [this](const std::string& id) { activate(id, false); SetFocus(hwnd); };
   d.focusTerminal = [this] { SetFocus(hwnd); st.focusInTerminal = true; invalidate(); };
   d.note = [this](const std::string& t, int ms) { note(t, ms); };
   d.writeClipboard = [this](const std::string& t) { setClipboard(widen(t)); };
   d.changed = [this] { st.asksFocus = askPane.asksFocused(); invalidate(); };   // plus whatever makes the sidebar repaint (its scene key)
   d.accelerator = [this](unsigned vk, bool c, bool s, bool a) { Mods m; m.ctrl = c; m.shift = s; m.alt = a; return windowKey(vk, m); };
   askPane.init(headless() ? nullptr : hwnd, d);
   askPane.setLink(localMode ? DeckLink() : link);      // and again in switchDeck / askDeckLink
   ```
4. Every meta (`applyMeta`): `askPane.setMeta(meta);`.
5. Layout and paint: at the top of `drawFrame`, `askPane.layout(mainRect(), gfx.scale);` (it does nothing when
   nothing moved, so every frame is fine: resize, the sidebar's width, collapse, fullscreen and a DPI change are
   all covered) and `askPane.cover(palette.open || ctx.open || <any panel or drag picture over mainRect()>);`.
   In `drawMainOver`, AFTER `drawPaneNote(mr)` and inside the clip: `askPane.draw(gfx);` (css: the pane is z-index
   20, the note 6, so the pane hides the note in Electron too). Where the target is replaced or lost
   (`targetWindow`, `dropCaches`): `askPane.dropDeviceResources();`.
   The scene cache needs nothing: the pane is drawn in the overlay pass, like the notes; the panes under it keep
   their own cache. (Optional: skip repainting panes while `askPane.isOpen()`; they are fully hidden.)
6. Z-order: the web view is a child window, so it is over everything Direct2D draws inside its rectangle. That
   is what `cover(true)` is for: the page is photographed, the child hidden, and `draw()` paints the photograph,
   so a menu, the palette or a panel shows over the page (a still picture, about a tenth of a second late).
   The native tooltip window is its own top-level window and needs nothing.
7. Mouse: first thing in `onMouseDown`: `if (askPane.click(x, y)) { invalidate(); return; }` (true for the whole
   terminal area while the pane is open: clicks there are the pane's); in the move handler
   `if (askPane.mouseMove(x, y)) invalidate();`; `tooltipAt` for the ×. Wheel and clicks over the page never reach
   the app (they are the web view's).
8. Keys. In `windowKey`: `if (vk == VK_F6 && askPane.asksZoom()) return true;` BEFORE the zoom toggle
   (25-layout.js asks the asks first), and replace the F7 note with `askPane.asksKey(st.selected); return true;`.
   In `onKeyDown` after `windowKey`: `if (askPane.key(vk)) return true;` (Esc while the app has the keys).
   While `askPane.isOpen()`, typed characters must not reach a terminal (49-asks.js blurs any terminal that takes
   the focus): return early from the WM_CHAR / terminal-input path.
   While the PAGE has the keyboard, the web view hands the app every key a page does not type with
   (`AcceleratorKeyPressed` -> `Deps::accelerator` -> `windowKey`), with the real modifier state; Esc closes the
   pane unless a page's own box has the focus. The pane gives the keyboard back itself (`SetFocus(parent)`) when
   it closes or is covered.
   `WM_KILLFOCUS` now also happens when the page takes the keyboard: use `askPane.webFocused()` (or
   `WM_ACTIVATE`) before drawing the window as unfocused.
9. The asks tile (`k == "asks"`): `askPane.asksSelect(h.id); askPane.tileClick(h.id);` (`h.id` is '' for the
   tile, a session id, or `__recent`). `activate()` calls `askPane.asksBlur()` where it clears `st.asksFocus`.
   The sidebar's `.at-row.picked` wants `askPane.asksPick()`.
10. The tile menu: `auto mi = askPane.menuItem(tile); if (mi.has) items.push_back(mi(widen(mi.label), mi.run));`.
    The palette's asks entries (88-palette.js, four kinds): every session's -> `askPane.openPane("all")`; this
    session's (when `askPane.hasAsk(sid)`) -> `openPane(sid)`; "Answer <name>" for each other session
    (`sessionName(sid)`, hint `<openCount(sid)> waiting`) -> `openPane(sid)`; "Recently answered" ->
    `openPane("all", asks::AskPane::RECENT)`.
    A clicked toast is `asks:open` in preload.js only (the Electron window); bridge.js has no such event, so a
    remote gets none. Local mode, if it ever raises one: `askPane.onAskOpen(sessionId, tileId)`.
11. `WM_MOVE` / `WM_MOVING`: `askPane.parentMoved();`. Before `DestroyWindow` in `WM_CLOSE`:
    `askPane.shutdown(3000);` (waits for the browser process, removes the profile folder).
12. `blockedInTests` already refuses `askDone` and `askClear` in test runs; `askDraft` is a `fire`. Keep it so.
13. `--shot` runs have no window: the pane draws its bar over an empty body. A scripted `--window-shot` run sees
    the real pane (`captureWindow`'s PrintWindow includes the web view: `asks-2c-window.png` is made that way).

The marks on tiles and pane names stay the sidebar's (from `meta`); `askPane.markInfo(sessionId)` also counts
what the open page has in progress before the deck's next meta says so, if the sidebar wants it.

## PARITY.md, the 49-asks.js rows as they read once wired

| Feature | Electron source | Native | Note |
|---|---|---|---|
| Tile menu: "Answer its questions (n)" / "Its last answers (read-only)" | `askMenuItem` (49-asks.js) | done | `AskPane::menuItem` |
| Palette: the asks entries | 88-palette.js | done | open the pane (`openPane("all")`, the recent row scrolled to) |
| Asks tile: glyph, count, rows, recently answered | `paintAsksTile` | done | drawn; a click opens every session's asks at the row clicked, the same click closes it |
| Ask pane: the bar (title, hint, ×) | `paintAskPane`, css `.ap-bar` | done | drawn with Gfx at the Electron pane's measured numbers (26 px, one baseline at 18) |
| Ask pane: a section per session (sticky header, mark, name, meta, Done / Copy all / Answer in the terminal / count / flash / Discard with its confirm), the pages, the round notes | `askSection`, `paintAskHead`, `makeFrame` | done | the same elements and css rules in an embedded web view (WebView2); every text is decided in `asks.hpp` |
| Ask pane: the pages' bridge (ready, change, height, answers; load, get, clear, lock), drafts shared through the deck | the `message` listener, `askLoad`, `askGet` | done | `web-test`: `askDraft` and `askDone` payloads asserted against the real `lib/phone.js` |
| Ask pane: Done, Copy all, Discard, closeOnDone, the notes each one leaves | `askDone`, `askCopy`, `askDiscard` | done | |
| Ask pane: one session's view (F7, the tile menu), its last answers read-only until it asks again, a page replaced by another session's | `askOpenFor`, `paintAskPane` | done | |
| Ask pane: recently answered, a row opening read-only, the answer lines when the page is gone or old | `paintRecent` | done | |
| Ask pane: a link on a page | `askOpenLink` | partial | http(s) opens in the default browser; a `file:` link is not opened (on a remote it names a file on the deck's PC) |
| Ask pane: an older page's own Done (a download) | `hookAskDownloads` | missing | downloads are cancelled: on a remote the file would land on the wrong machine. The page's own Copy all works |
| F7 asks; F6 on the asks tile; Esc | `keyAction`, `asksKey`, `asksZoom` | done | also while the page has the keyboard (`AcceleratorKeyPressed`): check by hand, the headless test cannot |
| No WebView2 runtime | — | **added** | the pane opens with a note naming "Microsoft Edge WebView2 Runtime (Evergreen)"; Windows 11 has it |
| Local mode: ask pages | (the Electron `<webview>` on the page file) | done (2026-11-19) | `AskPane` serves the page file itself when there is no deck link (`asks-7-local.png`); the core's feed now fills `meta.asks` in local mode (`deckhost.hpp` builds the deck through `host::buildDeckOptions`): **shell.ps1** `askfeed`. `meta.askFeed.state` = `guest` (another deck serves the pages) shows them read-only, in plain words |

## The cloud panes (48-cloud.js): built 2026-11-18, late (`src/cloud.hpp`, `app.cpp` search `cloudPanes`)

What was built, where it differs from the plan under the next heading, and what was decided:

- **One WebPane per cloud pane on show**, on the css rectangle of `.term .cloud-pane` (from `--pane-name-size * 1.2 +
  6px` under the pane's top to its bottom), inset by the pane's border (1px in a grid, 2px under the attention
  frame): Electron draws those lines OVER the page's edge (`::after` z 3, `::before` z 5), a child window cannot be
  drawn over. The pane's own paint puts the css background (#262624) under the page and skips the terminal
  (`.term.cloud .xterm { visibility: hidden }`).
- **The profile**: ONE persistent user-data folder, `<state>\web-cloud\`, and a NAMED PROFILE per account inside it
  (`WebPaneOptions::profileName`, `ICoreWebView2ControllerOptions::put_ProfileName`): `claude-<email, else id>`. One
  folder = one browser process for every cloud pane AND the ask pane (2026-11-19), a renderer each. By email because a remote may be a desk of
  several decks. `inPrivate = false`, `removeUserDataOnClose = false`. New `WebPaneOptions::site = true`: the page's
  context menu, password saving and autofill, downloads and the browser's own error page stay on (the plan's list);
  dev tools, browser accelerator keys, zoom, the status bar, host objects stay off; permissions as before (reading
  the clipboard only: WebView2 raises none of the other kinds main.js allows).
- **What may load**: `cloud.hpp` `pageUrlOk` / `paneUrl` / `signInUrl` are main.js's `guardWebview` / `paneUrl` /
  `signInUrl` (unit-tested in `web-test`). `onNavigate` is asked for the TOP document only and not for a server
  redirect (`WebPane::navRedirected`), as `will-navigate` is; a refused address goes to the default browser. New
  `WebPane::onPopup`: a new window for a sign-in host is let through (the runtime's own popup, same profile: it must
  talk to its opener); any other goes to `onNewWindow` and the default browser.
- **The hint** (`.cloud-hint`, z 4 over the page): a small child window of its own above the page's, painted with
  Direct2D on a DC render target by the same function that draws it into the frame while the page is a still.
  WS_DISABLED + HTTRANSPARENT: a click on it goes to the app's window, not the page under it (css: pointer-events
  none, where it would go to the page).
- **Cover**: per pane (`setCovered`), with the ask pane's mechanism: capture, hide, draw the picture, overlay on top.
  A pane is covered while the palette, a context menu, the cloud dialog, the deck-away banner or a drag is up, or a
  panel / the pane note / the leaving mark meets its rectangle. **Hidden** (not covered) while the ask pane is open,
  a zoom is in front, the pane left the layout, or the window is minimised: `put_IsVisible(false)` + `TrySuspend`,
  and after two minutes off screen the pane's web view is closed ("Memory" above).
- **Focus**: `activate()` hands the keyboard to a cloud tile's page once its web view is Ready (`cloudWantFocus`:
  never queued on a view still starting, or a later selection would be stolen back when it arrives); `GotFocus` not
  preceded by our own `focus()` within 0.8 s is a click in the page: it selects the tile and commits a walk.
  `termKeys()` is false for a cloud tile: nothing typed reaches the terminal under the page.
- **Keys from a page**: `onAccelerator` -> `keys.hpp windowKeyOf`; a window key is the app's, everything else the
  page's. F11 / F12 do NOT end the walk (the next step comes the same way); Esc undoes a walk in progress.
- **Reload** as `cloudRedraw`: reload when the address still holds the session's id, else navigate to it. As in
  Electron, `queuePaneRedraw` reloads a pane 400 ms after its rectangle changed.
- **Shutdown**: every view `destroy(0)`, then ONE wait for the shared browser process
  (`WebPane::waitProcessGone`); the folder stays.
- **Tests**: `--cloud-base` maps `https://claude.ai` onto a loopback stand-in (`real()` / `canon()`): the rules run on
  the address as it would have been, the web view only ever loads the stand-in, a popup is never opened, `OPEN <url>`
  is logged instead of a browser. No stand-in = no web view in a test (the card, with "claude.ai is never loaded").
- **Not seen**: the real claude.ai in a pane; a real sign-in (Google's popup included: WebView2 opens it as its own
  top-level window); a download; the page's context menu; a real mouse click (focus was moved with `MoveFocus`).

Also new in `WebPane` for both panes: `setRasterScale` (a forced scale for a parked window; a real window follows its
monitor), `suspend` / `resume`, `reload`, `url`, `onUrl` (SourceChanged), `removeProfileFolder`, `userDataDir`.

## The same component for the cloud panes (48-cloud.js): what it would take (the plan it was built from)
A cloud tile's pane is the session's claude.ai page in an Electron `<webview>` with a persistent, per-account
partition. With `WebPane`:
- One WebPane per shown cloud pane, placed on the pane's rectangle (`setBounds` from the layout, `show(false)` when
  the pane leaves the screen, `cover` while an overlay is over it, exactly as above). A pane's name bar stays
  Gfx's; the page starts under it.
- **Not InPrivate**: the sign-in must last. `WebPaneOptions{ inPrivate = false, removeUserDataOnClose = false,
  userDataDir = <the app's own data folder>\web\<account id> }` — one profile folder per account, as Electron's
  partitions are (`persist:claude-<account>`, 48-cloud.js). (Or one folder and `ICoreWebView2ControllerOptions::put_ProfileName`
  per account; WebPane would need that option added: three lines in `makeController`.)
- `onNavigate`: allow what main.js allows a cloud pane (claude.ai and Anthropic, plus the sign-in pages a login
  passes through: `signInUrl`); anything else -> the default browser, as `onNewWindow` does now. The permission
  handler here denies everything but clipboard-read: port main.js's `ALLOWED` set. main.js also presents the
  partition as plain Chrome (Google's sign-in refuses Electron); WebView2 says Edge, which those pages accept.
- What the ask pane turns off must come back on for a real site: password saving and autofill (the user signs
  in), downloads (decide), the context menu (copy / paste by mouse). These are constants in `Impl::setup` today;
  they would become `WebPaneOptions` fields.
- Keys: the same `onAccelerator` route (F6, F11 / F12 stepping, Alt+digit chords — note Alt+digit arrives there
  too, as a system key).
- Each profile is its own browser process tree (~100 MB and up); panes of one account share one. The remote case
  needs nothing from the deck: the page is claude.ai's.
- The exe stays one file: no new dependency.
