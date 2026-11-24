# Parity: the native Mac shell against the Electron renderer

The SAME rows as `windows/PARITY.md`, section by section (generated from it on 2026-11-19 by reading its first
column - when a row is added there, add it here), so the two native shells are tracked against one list. The
specification is the Electron deck (`renderer/**`, `lib/platform.js`); what a row means, and how the Windows shell
does it, is in `windows/PARITY.md`.

**Nothing in this file has been compiled or run.** The Mac shell was written on Windows with no Swift toolchain
(`mac/README.md`). Status:

- **not started** - no code.
- **drafted** - code exists in `mac/Sources/ClaudeDeck`, UNBUILT and UNTESTED. The note says what the draft leaves out.
- **done-untested** - the draft is believed complete against the row and lacks only a build and a look. **No row is
  here yet**: nobody has seen a frame of this shell, so nothing is claimed complete.
- **in the core** - the row needs no Swift: the shared core (`core/deck`) does it for the Mac's LOCAL deck, checked by
  the core's own suites on Windows; never built or run on a Mac.
- **n/a** - the row cannot apply to a Mac.

(Since 2026-11-19 the Swift shell COMPILES on GitHub's macOS runners, `.github/workflows/mac.yml`, by hand only; none
of it has been RUN. "Drafted" still means nobody has seen it work.)

Where a note says "(tested)", the RULE behind the row is the shared core's or the bridge's and is covered by
`mac/Tests/bridge_test.cpp` on Windows - the Swift that calls it is still unbuilt.

Rows: **108 drafted**, **102 not started**, **4 in the core**, 1 n/a, 0 done-untested (plus the Mac-only table at the end);
re-counted 2026-11-23 (rows added for what landed since 11-20: the tile menu's Prompts submenu, Pause / Resume,
auditors, the deck's own log and WebSocket compression not started; the restore refresh, a tile following its session
and `CLAUDE_DECK_USERDATA` in the core). Re-counted again 2026-11-23 (later): the ask pane on WKWebView
(AskPane.swift, feat/mac-askpane) drafted - it BUILDS on the macOS runners (CI 45233494050), its host document's
JavaScript is tested in headless Edge (mac/Tests/askhost-test), and nobody has seen it on a Mac.

## Blockers the owner named for switching to the exe (2026-11-18): paste, scroll-up — pending their hand test

| Feature | Mac | Note |
|---|---|---|
| Right-click paste over Claude Code (a session that tracks the mouse) | **drafted** |  |
| Ctrl+V, Ctrl+Shift+V, Shift+Insert | **drafted** | Cmd+V; no text on the clipboard: local sends 0x16, a remote sends nothing and says so |
| The wheel over Claude Code (scroll up) | **drafted** |  |

## renderer/js/00-state.js, 10-format.js

| Feature | Mac | Note |
|---|---|---|
| `state` (accounts, tiles, usage, settings, active, confirm, platform, windowIndex) | **drafted** | DeckModel.swift |
| Remembered per window: selected tile, sidebar width / collapsed, bars / messages off, grouped, collapsed groups | **drafted** | only the selected tile and the sidebar width (UserDefaults); the rest not started |
| Font history for Settings → Font (`deck.fonthistory`) | not started |  |
| `fmtAge`, `fmtClock`, reset / countdown labels, `fmtHeldTime` | **drafted** | fmtAge only (DeckModel.swift Format); reset / countdown labels not started |

## renderer/js/20-terminals.js

| Feature | Mac | Note |
|---|---|---|
| Terminal: theme, font stack, line height 1.1, scrollback 10000 | **drafted** | Theme.swift, GlyphAtlas (CoreText metrics; cell = advance x floor(ceil(height) * 1.1)) |
| Fit to the pane; only a visible pane this window drives pushes its size | **drafted** | TerminalAreaView.fit, DeckWindowController.relayout |
| Mirror: another client owns the size (same cols × rows, font shrunk to fit, bottom shown) | **drafted** | the size only; the font is NOT shrunk to fit (the pane clips) |
| Keys: Shift+Enter, Ctrl+Shift+C, Ctrl+V, Ctrl+Home / End, window keys kept from the pty | **drafted** | the key table is the bridge's (tested on Windows); Cmd+C / Cmd+V through the Edit menu; Ctrl+Home / End not started |
| Right click: copy the selection, else paste unless the app tracks the mouse | **drafted** | TerminalAreaView.rightMouseDown; a remote always pastes |
| **Ctrl+C with a selection copies it** (and clears it) and sends nothing; without one it is the interrupt | not started | on a Mac Cmd+C copies; Ctrl+C is always the key |
| Selection: drag; double-click a word; triple-click a line; Shift+click extends; survives scrolling | **drafted** | drag, word, line, Shift+click; no 50 ms drag-scroll repeat |
| Scrollbar: thin, `#333`, down the pane's right edge whenever there is scrollback | not started |  |
| Wheel: the scrollback (three lines a notch); in the alternate screen, an arrow key when the app did not ask for the wheel | **drafted** | scrollback / alt-screen arrow / one report (xterm's pixel sum is the bridge's) |
| Synchronized output (DEC 2026): the pane keeps its picture while the application is mid-frame, 1 s at most | not started |  |
| Ctrl+Insert copies, Shift+Insert pastes | not started |  |
| Input methods (IME): the composition window at the cursor, committed text typed | **drafted** | NSTextInputClient: marked text drawn at the cursor, firstRect for the candidate window |
| Cursor: block / underline / bar as the application asks (DECSCUSR), outline without focus, 600 ms blink, steady when asked | **drafted** | block / underline / bar / outline in the shader; 600 ms blink |
| Pane name: abbreviation (green when busy), title, ` · zoomed`, hover, attention frame | **drafted** | PaneChromeView: abbreviation, title, ` · zoomed`, attention frame; no hover |
| Pane name: the cloud mark | not started |  |
| Pane name: the ask mark | not started |  |
| Pane name: its tooltip | not started |  |
| Pane name: click = redraw (after a beat), double-click = zoom | **drafted** | at once (no beat) |
| Redraw one pane (F5 / the name): reset, re-attach since the last resize, nudge; the name flashes green | **drafted** | reset, attach since the last resize, nudge; no name flash |
| Redraw by itself when a pane's rectangle changes (400 ms), and when a session has just come up (2.5 s after its first output, 1.5 s after it enters the alternate screen) | not started |  |
| `activate`: select, adopt, zoom follows, push the size, scroll the tile into view | **drafted** |  |
| Exit line `[process exited with code n]`, reset on respawn | **drafted** |  |
| Terminal's own replies (cursor position, device attributes) | **drafted** | local only (R8) |
| The OSC window title becomes the tile's title | **drafted** | local: `setOscTitle` |
| Mouse and focus reports | **drafted** | local only; the encoder is windows/src/mouse.hpp behind the bridge (tested) |
| Settings → Font: the bundled fonts | not started |  |
| Screen probe for the trust prompt | not started |  |

## renderer/js/25-layout.js

| Feature | Mac | Note |
|---|---|---|
| Grid of panes, percent rectangles, cell lines, selected pane's border | **drafted** | frames from core/model/layout; borders in PaneChromeView |
| No layout: the selected tile fills the area; held in another window: the empty notice | **drafted** |  |
| Window title: the one session that fills it, else `Claude Deck [n]` | **drafted** |  |
| Zoom (F6 / double-click the name), the zoom follows the selection, grid changes refused with a note | **drafted** | Cmd+Shift+Enter / double-click the name; the zoom font size; no refusal note on a grid change |
| "The phone is driving" note | not started |  |
| F11 / F12 (and Up / Down) walk as a preview; a press, another key or losing focus commits; Esc undoes (`stepEscape`) | **drafted** | a plain step (Cmd+Shift+[ / ]); no preview / commit / Esc |
| Leaving mark: dashed amber frame, stripes, "moving away" tag; sent to the other window that holds the session | **drafted** | the dashed frame on receive; this window never SENDS `leaving` yet |
| Place, unplace, clear, prune | **drafted** | GridLayout (the core's rules, tested through the bridge) |
| Selected empty slot (dashed), filled by a double-click | not started |  |
| A new session takes the first empty region (never the selected pane) | not started |  |
| Selecting an unplaced tile takes the selected pane | **drafted** |  |
| Digit chord (1–9), Alt+digit from anywhere, letter block on grids over nine cells | **drafted** | Option+digit by physical key; no bare digits from the sidebar (R2), no letter block |
| Drag a tile onto the area: preview frame, shaded rest, zone label, lines hidden under a multi-cell preview, growing label, leaving mark, first drop splits the full-screen session | **drafted** | preview frame + shaded rest; no zone label |
| The dragged tile's picture under the pointer | **drafted** |  |
| Drag out of the window → a new deck window | **drafted** | local only |
| Stranded-drag rescue, drag trace | not started |  |
| Another window took a tile / the grid changed / main placed a tile | **drafted** | `layout:evict`, `grid:changed` |
| Restore the saved layout at start | **drafted** | from deskInit's `layout` |

## renderer/js/30-tiles.js

| Feature | Mac | Note |
|---|---|---|
| Tile: every part of `updateTile` (abbr, cloud, title fit, ask mark, effort, quad, age, line 2, bars, preview, held rows) | **drafted** | abbr, title, effort, placed mark, age, line 2, last message, session + week bars, the ask mark (AsksTileView.drawMark); NOT: cloud, quad cells, Fable / context bars, bar labels |
| Click selects (never places); double-click places in the pane selected before | **drafted** |  |
| Pin button, shown near the corner (70 %), full over it | **drafted** | a dot when pinned; pin / unpin from the menu |
| Action row: sleep (icon) / wake / resume, `↪ <account>`, refresh, close / remove (icon); shell and cloud variants; `fitButtons` (2026-11-23: special moved to the tile menu; the move's arrow the curved U+21AA since 12:15) | not started | the sleep / close shapes are SVG in the renderer and drawn by the exe (windows/src/sidebar.cpp): a Swift strip would draw the same |
| Tile menu: Prompts submenu, submenus (Right / Left), Shift+F10 / the Menu key, the arrows (2026-11-23) | not started | the tile menu (DeckWindowController.sidebarMenu, an NSMenu: Rename, Pin, placements, Sleep / Wake, Refresh, Close) has no Prompts submenu and no Special prompt…; no key opens it. The arrows and submenus inside an NSMenu are AppKit's own. Not checked on a Mac |
| Pause / Resume / Pause until restart (2026-11-20; Resume names the file Pause named, kept across a /clear, 2026-11-21) | not started | the Swift part: no Pause / Resume items in the tile menu or the palette (Panels.swift), no pausing / paused / resuming words on the tile. The deck side is the shared core's (`core/deck/pause.hpp`, deck_more.cpp `D::pauseTile` …), so the Mac's LOCAL deck has it with the core, and a remote's office deck does it; reached only by a call nothing in the Swift shell makes. Checked on Windows only: the core deck scenarios "pause" and "pause + clear" (HANDOFF 2026-11-20 23:10, 2026-11-21 01:55) |
| Auditor sessions and their children; URGENT delivery (`to-<n>.now.md` typed at once, Esc mid-turn) (2026-11-23) | not started | the Swift part: no auditor items (Make auditor…, Set as child of, Un-child, Rename / Un-make auditor, Send), no auditor-mail rows on a child tile, no auditorAutoSend checkbox. The deck side (the inbox, typing, refused/, log.md, CHILD lines) is the shared core's (`core/deck/auditors.hpp`), so the Mac's LOCAL deck has it with the core. Checked on Windows only: the core deck scenarios "auditors (pure)", "auditors", "auditor leaves", "auditor urgent" (HANDOFF 2026-11-23 10:31, 11:45) |
| Confirm in the tile: move / refresh / close — the danger button, Cancel, the question | **drafted** | close only |
| Right-click `open …` / `refresh`: the same with the browser on or off | not started |  |
| A failed move / sleep is written into the pane in red | not started |  |
| Rename: opened from the tile's menu; Enter saves, Esc cancels, leaving the box saves | **drafted** |  |
| Special prompt form: next turn / at a time / recurring / triggered, picks, add / cancel, errors | not started |  |
| Held rows' × | not started |  |
| Drag to reorder pinned tiles: the bar above / below | **drafted** | no insertion bar |
| Animations: ultracode pulse (strip opacity, selected border, tag text), max hue drift (tag, strip, selected border), the attention pulse with its ring | not started |  |
| Transitions: the pin's 120 ms fade, the usage bar's 400 ms width ease, the group chevron's 120 ms turn; the panels' 160 ms rise (`slide-up`) | not started |  |

## renderer/js/35-groups.js

| Feature | Mac | Note |
|---|---|---|
| Grouped view, dividers, counts, collapse | not started |  |
| Context menu (css `.ctx-menu`): rows, ticks, dim rows, separators, closes on any click or Esc | **drafted** | an NSMenu (the system's look, not the css one) |
| Inline prompt in the menu (new group, rename) | not started |  |
| Tile menu: Rename, Pin / Unpin, Special prompt…, Fill / halves (ticked), Remove from layout, groups, No group, New group… | **drafted** | Rename, Pin, Fill, Remove from layout, Sleep / Wake, Refresh, Close; no halves, no groups, no Special prompt |
| Tile menu: "Answer its questions (n)" / "Its last answers (read-only)" | not started |  |
| Tile menu: Open in a new window | **drafted** |  |
| Tile menu: Open on claude.ai in the browser (cloud tiles) | not started |  |
| Group bar menu: Rename, Remove | not started |  |
| Groups button: left click toggles, right click its menu; the button closes its own menu | not started |  |

## renderer/js/40-new-tile.js, 45-elsewhere.js, 47-past.js

| Feature | Mac | Note |
|---|---|---|
| New terminal tile, account buttons | **drafted** |  |
| Right-click an account: New … session with the browser / in a new window / New … cloud session… | not started |  |
| Elsewhere: the list, account / title / status, the count on the button (10 s) | not started |  |
| Past: filter box, "n on disk", 60 rows, the count on the button (60 s) | not started |  |
| Confirm in place (white bound, the row as the title), Bring it here / Open here / Open it here | not started |  |

## renderer/js/46-search.js, 88-palette.js

| Feature | Mac | Note |
|---|---|---|
| Search pane: filter as you type, `n / total`, clear, Enter jumps to the first match, Esc closes | not started |  |
| Palette (Ctrl+Shift+P): sessions, new, this session, asks, deck; filter, arrows, Enter, Esc, hover, click, click outside | **drafted** | THE WORKED PANEL (Panels.swift PalettePanel): sessions, new, this session, asks (88-palette.js's four), deck; filter, arrows, Enter, Esc |
| Palette: **Connect to another deck…**, **Use this PC (local preview)**, (local) **Connect to the remembered deck** | not started |  |

## renderer/js/48-cloud.js, 49-asks.js

| Feature | Mac | Note |
|---|---|---|
| Cloud pane: the session's claude.ai page over the terminal, below the pane's name | not started |  |
| Cloud pane: one login per account, kept | not started |  |
| Cloud pane: what may load | not started |  |
| Cloud pane: the sign-in hint | not started |  |
| Cloud pane: focus | not started |  |
| Cloud pane: the window's keys while the page has the keyboard | not started |  |
| Cloud pane: reload (F5, the pane's name, the tile's `reload`, the palette) | not started |  |
| Cloud pane: `browser`, "Open on claude.ai in the browser", `retry` | not started |  |
| Cloud pane: under the palette, a menu, a panel, the drop preview, a note, the dialog, the deck-away banner | not started |  |
| Cloud pane: several at once | not started |  |
| Cloud pane: closed after 2 minutes off screen, built again (page loaded again) when shown; never while on screen | not started |  |
| Cloud pane: no web view (no WebView2 runtime; a view with no `<webview>`) | not started |  |
| New cloud session dialog | not started |  |
| Asks tile: glyph, count, rows, recently answered | **drafted** | AsksTileView (SidebarView.swift): the glyph, the count ("n waiting · read-only" for a guest), a row per session (mark, abbr, name, "outside the deck" / "session ended", age), "Recently answered (n)"; a click as 49-asks.js (AskPane.tileClick); the pointer resting 150 ms prewarms the web view. NOT: the pulse (the border is yellow, still), hover looks, a scroll past 7 rows, a hover title per row |
| Ask pane: the bar (title, hint, ×) | **drafted** | in the WKWebView's host document (AskHostPage.swift: 49-asks.js's markup and style.css's rules); tested in Edge (mac/Tests/askhost-test) |
| Ask pane: a section per session (sticky header, mark, name, meta, Done / Copy all / Answer in the terminal / count / flash / Discard with its confirm), the pages, the round notes | **drafted** | the same elements and css rules, 49-asks.js's code ported as JavaScript in the host document, each page a sandboxed iframe; tested in Edge (mac/Tests/askhost-test) |
| Ask pane: the pages' bridge (ready, change, height, answers; load, get, clear, lock), drafts shared through the deck | **drafted** | the host document relays it (only a frame it made, naming its own page, from an opaque origin); askDraftGet / askDraft / askLogState go through the window's connection (AskPane.deckCall), so the drafts are the deck's and round-trip with the Electron deck and the phone; tested in Edge (mac/Tests/askhost-test) against fake pages speaking the skill's bridge |
| Ask pane: Done, Copy all, closeOnDone, the notes each one leaves | **drafted** | askDone's payload, closeOnDone, the notes; Copy all to NSPasteboard; tested in Edge (mac/Tests/askhost-test) |
| Ask pane: Discard (the 700 ms arm), a legacy page (no bridge), a page replaced by a newer one | **drafted** | tested in Edge (mac/Tests/askhost-test) |
| Ask pane: one session's view (F7, the tile menu), its last answers read-only until it asks again | **drafted** | Cmd+Shift+A (AskPane.asksKey / openFor) and the tile menu's first entry (AskPane.menuItem); the log view tested in Edge (mac/Tests/askhost-test) |
| Ask pane: recently answered, a row opening read-only | **drafted** | a row opens its page read-only (askLogState); a page the deck no longer has: the answer lines; tested in Edge (mac/Tests/askhost-test) |
| Ask pane: a row of the asks tile scrolls its session to the top | **drafted** | taken again while the pages above it grow (as the Windows host document does) |
| Ask pane: over it, the palette, a menu, a panel, the deck-away banner, a dragged tile | **drafted** | AppKit views over a WKWebView: the palette, panels, menus and the banner are above it in the view tree (no still picture needed, unlike the Windows child window); a dragged tile drops on the terminal area under it |
| Ask pane: a link on a page | **drafted** | never inside the pane: target=_blank / window.open (WKUIDelegate) and a frame's own navigation (WKNavigationDelegate) are cancelled and the link goes out - LOCAL: the deck's `askOpenLink` (main.js openAskLink's rule); a remote: http(s) in this Mac's default browser, nothing else |
| Ask pane: an older page's own Done (a download) | **drafted** | WKDownload: `ask_<id>.answers.json` / `.txt` saved, no dialog, in the first of the ask feed's Done folders that exists (`db_deck_ask_downloads_json`, tested), LOCAL only; anything else a page downloads is cancelled (as the Windows pane) |
| Ask pane: a new-ask toast clicked | **drafted** | `asks:open` -> AskPane.onAskOpen: its tile, then its questions |
| No WebView2 runtime | n/a | WebKit is part of macOS: always there |
| The ask pane's web view exists only while it is of use | **drafted** | made when the pane opens (or the pointer rests 150 ms on the asks tile), closed 5 s after it closes (2026-11-23, as the Windows pane), 2 min after in a minimised window and made again when the window comes back, made again if its WebContent process dies; the state is sent to it only when it changes |
| Another deck is serving the ask pages (`meta.askFeed.state` = `guest`) | **drafted** | read-only: Done and Discard off, the bar and each section say why in plain words, the pages loaded read-only, no draft sent (tested in Edge (mac/Tests/askhost-test)); the asks tile reads "n waiting · read-only" |
| Local mode: ask pages | **drafted** | AskSchemeHandler serves the page FILE the meta names (only `ask_*.html`), under the deck's sandbox CSP; a remote's pages come from its /ask/ route, fetched by URLSession with the deck's cookie on the request (the token never enters the web view) |

## renderer/js/50-settings.js

| Feature | Mac | Note |
|---|---|---|
| Panel: place above the tool row, the sidebar widens to 600 px, the tool row keeps its width, Esc / Cancel / the gear close | not started |  |
| Every section rendered from `getSettings`: Accounts, Tiles, Layout, Asks, Sleep, Notifications, Graphs, Terminal, Phone | not started |  |
| Controls: ticks, selects (their list opens as a menu), number and text boxes, the bar name boxes | not started |  |
| Controls: colour boxes | not started |  |
| Controls: hover titles (the Asks rows, the recipe hint, Zoomed pane font size, the font list) | not started |  |
| Accounts: edit rows, add, remove, log in | not started |  |
| Save: accounts → aliases → everything else; "Saved", then the panel closes; the not-installed-font note | not started |  |
| Phone block: on / port / bind | not started |  |
| Phone link: copy (button or a click on the link) | not started |  |
| Phone: new token | not started |  |

## renderer/js/60-move.js

| Feature | Mac | Note |
|---|---|---|
| Menu: title fit, all / these, from / to (most tiles → first other), usage bars under each, the continue option, Go | not started |  |
| These: one row per session, title + two lines of its last message, usage notice, tick by click | not started |  |
| Go: the checks, the question, Move them / Cancel, Moving…, the result line | not started |  |

## renderer/js/70-meta.js, 80-sidebar.js, 82-restart.js, 85-focus.js, 90-boot.js

| Feature | Mac | Note |
|---|---|---|
| Apply a meta: tiles come and go, layout pruned, selection falls back, settings applied | **drafted** |  |
| Sidebar width: 220 px at least, 70 % of the window at most | **drafted** | NSSplitView limits |
| Collapse / rail, Ctrl+` / F9 | **drafted** | hidden / shown (Cmd+B); no rail |
| Sidebar resize by its edge | **drafted** | NSSplitView |
| Messages / bars toggles | not started |  |
| Driver button: three states, colours, tooltips, cycles auto → phone → desk | not started |  |
| Driver button as a CLAIM: icon = who drives (monitor / phone / two screens / eye), green = this screen (`remote`), grey = another (`office`, `phone`), the auto look; one click `setDriver('remote')`, on its own claim `setDriver('auto')`; the tooltips, with "this remote desk (NAME)" | not started |  |
| Per-tile words under auto: "on your phone" / "on the remote desk" / "sized by the office deck"; none under a claim | not started |  |
| Becoming the driver: a shown tile whose `mirror` went from set to null gets a `resize` although the pane did not change; a pty that drifted under `sizeBy: remote` is re-sent, at most every 2.5 s per tile | not started |  |
| `deskInit({ name, kind: 'native' })`: this computer's name (GetComputerNameW; `--desk-name` in tests), and that it is the native exe (the branch's lib/presence.js `deskKind`, added there 2026-11-18 late: without it the deck calls this desk a browser) | **drafted** | Host.current().localizedName |
| Settings > "Size is driven by": the four names (auto / the office deck / the phone / the remote desk) | not started |  |
| Restart button: armed / offer looks and tooltips; arms, cancels | not started |  |
| Restart strip: titles, blocker rows (click shows the session), note, Arm / Later / Now / Cancel | not started |  |
| "Now" past a blocker: second click within four seconds, "Stop n and restart" in the danger look | not started |  |
| Focus model: chrome vs terminal, Enter / Space, Up / Down, digits, panels take the keys | not started |  |
| Clock, ages ticking | not started |  |
| Boot: remembered selection, layout, grid | **drafted** |  |

## renderer/desk/bridge.js, lib/platform.js keyAction

| Feature | Mac | Note |
|---|---|---|
| Socket, calls with ok / err, reconnect as a fresh window, the away / refused banners | **drafted** | URLSessionWebSocketTask; reconnect after 2 s as a fresh window; the away / refused banner |
| WebSocket compression (permessage-deflate): the deck's server offers it (lib/phone.js, 2026-11-19), a browser desk negotiates it by itself; the Windows exe's own client since 2026-11-23 | not started | DeckConnection.swift's URLSessionWebSocketTask: nothing in mac/Sources asks for or handles permessage-deflate; whether URLSessionWebSocketTask offers it by itself is not checked (never built). The LOCAL deck's own server (the shared core's server.cpp) offers none either. Not checked |
| Leaving the deck (window close, sign-out, another deck) | not started |  |
| A call the deck refuses (`err`) | **drafted** | a note over the area |
| Attach only what is on screen; `shown` with `hidden` | **drafted** |  |
| Window keys: F5 redraw, F6 zoom, F9 / Ctrl+` sidebar, F10 fullscreen, F11 / F12 step, Ctrl+Shift+P palette | **drafted** | the Mac map (lib/platform.js keyAction through the core): Cmd+R, Cmd+Shift+Enter, Cmd+B, Cmd+Ctrl+F, Cmd+Shift+[ ], Cmd+Shift+P, Cmd+, |
| F7 asks; F6 on the asks tile and on the every-session pane; Esc on the pane | **drafted** | the Mac's keys: Cmd+Shift+A, and Cmd+Shift+Enter asks the asks first (it closes the every-session view); Esc in the host document, or with the window's keyboard; while a page has the keyboard the deck's Cmd keys still work (AppDelegate's local key monitor), Esc inside a page stays the page's. NOT: the asks tile as the selection (no walk reaches it in this shell) |
| While the ask pane is open nothing typed reaches a terminal | **drafted** | the terminal area's keys are swallowed while the pane is open (Cmd keys go on to the menu bar) and areaSend refuses a real key or a paste |
| Clipboard | **drafted** | NSPasteboard |
| Tooltips (`title="…"`) | not started |  |
| The window's place, size, maximised | **drafted** | bounds reported top-left as Electron keeps them; maximised not |

## A remote keeps to itself: deliberate differences from the browser desk (decided 2026-11-18)

| Feature | Mac | Note |
|---|---|---|
| A session another window of the deck holds | not started |  |
| The wheel over a session that tracks the mouse (Claude Code: alternate screen, no scrollback) | **drafted** |  |
| The grid layout | **drafted** |  |
| `layout:evict` from the deck | not started |  |
| `grid:changed` | not started |  |
| The walk's leaving mark in the other window (`leaving`) | not started |  |
| `shown` | **drafted** |  |
| Who drives a pane's size | not started |  |
| The wheel report's frame | **drafted** | `db_conn_wheel` (the codec refuses any other shape: tested) |

## Local mode (`--local`)

| Feature | Mac | Note |
|---|---|---|
| New terminal tile, typing, output | **drafted** | the login shell (`$SHELL -l`), forkpty |
| The pty follows the pane (150 ms debounce in the deck) | **drafted** |  |
| Two tiles side by side with the chord (Alt+1+3, Alt+2+4); the letter block on a 4×4 grid | **drafted** | Option+1+3 |
| Rename, pin, close with the confirm row | **drafted** |  |
| `[process exited with code n]` on `exit` | **drafted** |  |
| Restart: terminal tiles do not come back (Electron); with `--persist-shells` they do, as fresh shells with their title and pin | not started |  |
| Settings saved (grid, sidebar side) | not started |  |
| First run: "Use this PC (local preview)" in the link box; the mode is remembered | not started |  |
| Claude sessions | **drafted** | follows `allow_claude` (LaunchOptions.shippingAllowClaude: on only for the real bundle on its own state folder) |
| A Claude tile's pty after the trust prompt | not started |  |
| A sleeping tile is not woken by its own terminal | not started |  |
| Elsewhere, Past | not started |  |
| Restart when quiet, Restart now, the update offer, "Later" | **drafted** | the relaunch itself (deckRestart); no restart strip |
| Move every session | not started |  |
| Special prompts, triggers, sleep / wake | not started |  |
| Refresh every restored session once the restore is done (2026-11-20; `refreshAfterRestore: false` turns it off) | in the core | `core/deck/restorerefresh.hpp`, `D::startRestoreRefresh` / `restoreRefreshTick`, run by the core's own timers (DeckHost.swift's pump); nothing Swift to add. Checked on Windows only: the core deck scenario "restore refresh" (HANDOFF 2026-11-20 22:20). Never built or run on a Mac |
| A tile follows its session after /clear or /resume (2026-11-20) | in the core | `D::followTiles` (core/deck/deck.cpp); a direct spawn's rule (the pty's pid is claude's) should hold with MacPty's forkpty + execvp, but was checked for ConPTY only; a session inside a shell tile rides on the shell-host scan, not started here. Checked on Windows only: the core deck scenario `follow` (HANDOFF 2026-11-20 23:55) |
| Accounts editing, usage, cloud sessions, the page viewer, ask pages | not started | | ask pages: drafted (AskPane.swift, the page files); the rest not started |
| The ask feed | in the core | the core's feed (core/deck/asks.cpp) on `DBOptions.asks` (LaunchOptions.shippingAsks: the real bundle on its own state folder, `--asks`, or ASK_PAGE_STATE_DIR); never run on a Mac |
| Mouse and focus reports to the application | **drafted** | local only; the encoder is windows/src/mouse.hpp behind the bridge (tested) |
| The default shell (PowerShell 5.1 with the profile and PSReadLine) | **drafted** | `$SHELL -l` as lib/platform.js shellCommand |
| An untitled terminal tile's name | not started |  |
| Being a deck for a phone / a remote desk: `--serve <port>`, Settings → Phone | **drafted** | `--serve <port>`; Settings -> Phone has no panel yet |
| Toasts, the taskbar flash, a clicked toast | **drafted** | UNUserNotificationCenter, a clicked toast through the core, the Dock bounce |
| Window state to the deck: on screen / minimised / focused, a key anywhere in the window, the version | **drafted** |  |
| The single-instance lock | **drafted** | flock on <config>/instance.lock |
| The exit watchdog | **drafted** | dms_arm_exit_watchdog |
| Hibernation, the shell-host scan, the memory guard | not started |  |
| The deck's own log (Electron: `console.log` to the main process's output; the Windows exe: `<state dir>\deck.log`, 2026-11-21) | not started | the core hands its lines (`[follow]`, `[pause]`, `[restore-refresh]`, `[auditor]`, `[spawn]`, `[cloud]`) to the optional `Log` seam (core/deck/seams.hpp); MacHost.cpp passes none, so the Mac's local deck logs nothing. Not checked |
| `CLAUDE_DECK_USERDATA` in every tile's environment (2026-11-23; the Electron deck sets nothing) | in the core | core/deck/deck.cpp sets it to the deck's state folder for every shell, Claude, move and cloud tile, so the Mac's local deck has it with the core; the deck-triggers skill reads it. Checked on Windows only: the core deck suite's "presence" check (HANDOFF 2026-11-23 11:45 M1) |

## The renderer and the window messages

| Feature | Mac | Note |
|---|---|---|
| Repaint only what changed | **drafted** | damage rows -> instance slices; one draw call |
| The sidebar's own repaint | not started |  |
| Fractional scales (125 %, 150 %, 175 %) | **drafted** | the backing scale factor; the atlas is rebuilt on a display change |
| The web views at 125 / 150 / 200 % | **drafted** | the ask pane: WKWebView follows the window's backing scale by itself (the cloud panes: not started) |
| Minimised | not started |  |
| A deck switch and the ask pane's profile | **drafted** | nothing to remove: the pane's data store is non-persistent (memory only) and the deck's token is never in it; a window's deck is fixed for its life |
| Animations: one timer | not started |  |
| Keys | not started |  |
| Minimise | not started |  |
| WM_SIZE storms | not started |  |

## A second window ("Open in a new window", "New … session in a new window", a tile dragged out)

| Feature | Mac | Note |
|---|---|---|
| The tile menu's "Open in a new window" | **drafted** |  |
| A tile dragged out of the window | **drafted** |  |
| "New … session in a new window" (an account button's menu) | not started |  |
| Every window's sidebar lists every session and marks who holds it (R5) | **drafted** | "in window n" on line 2 |
| One home per tile, `layout:evict`, the leaving mark | **drafted** | the core's; `layout:evict` handled |
| Each window is its own terminal | **drafted** | panes are per window; the atlas is shared |
| Closing one of several windows touches no session | **drafted** |  |
| The windows and their layouts come back (R7; R13: not an empty auxiliary one) | **drafted** | VERIFY with the core: windows opened after start() |
| Window bounds | **drafted** |  |
| REAL windows | not started |  |

## Mac only (no row in `windows/PARITY.md`)

| Feature | Mac | Note |
|---|---|---|
| The menu bar: app, Edit, View, Window with the standard roles (main.js createWindow, the Mac branch) | **drafted** | AppDelegate.menu; the deck's own Cmd keys stay OUT of the menu, as in main.js |
| Cmd+W leaves the deck in the Dock with every pty alive; the Dock icon builds a fresh window | **drafted** | applicationShouldTerminateAfterLastWindowClosed = false, applicationShouldHandleReopen |
| Cmd+Q asks while sessions run ("Quit Claude Deck?" / "n sessions will be stopped...") | **drafted** | counts every live process, not Claude tiles only |
| Option as Meta; Option+click forces a selection (xterm `macOptionIsMeta`, `macOptionClickForcesSelection`) | **drafted** | TerminalAreaView.keyDown step 5; no setting to turn it off |
| Dock bounce on a toast (`app.dock.bounce('informational')`) | **drafted** | no badge: the Electron deck sets none |
| The children's environment is the login shell's (tools/mac-make-app.sh starts Electron through `$SHELL -lc`) | **drafted** | MacEnv: `$SHELL -l -c "env -0"`, once |
| Credentials from the Keychain (lib/platform.js readOAuth) | **drafted** | the item names are tested against lib/platform.js; the read itself is unbuilt, and OFF unless the owner allows usage polling |
| `claude` found in ~/.local/bin, /opt/homebrew/bin, /usr/local/bin, ~/.npm-global/bin | in the core (tested) | core/deck/platform.cpp claudeExe; the bridge test runs the deck as `darwin` |
| A signed, notarised .app; auto-start at login | not started | mac/tools/make-app.sh makes an unsigned bundle for the machine that built it |
| The deck link kept in the Keychain (Windows keeps it DPAPI-protected) | not started | remote mode takes `--remote <link>` on the command line only |
| The ask pane's pages through its own scheme (`deckask://pane`); a remote's fetched by URLSession with the deck's cookie on the request | **drafted** | AskSchemeHandler (AskPane.swift): the deck's token never enters the web view (the Windows pane sets it as an HttpOnly cookie in an InPrivate profile). VERIFY on a Mac: WebKit applies a scheme handler's CSP header; a remote deck on plain http needs App Transport Security's leave (as the remote socket does) |

## What a test must never do

As `windows/PARITY.md`: never the live deck's port or state folder, never a real Claude session, never the real
clipboard, never a toast on the owner's screen. `ClaudeDeck --test --state-dir <temp>` is the scratch deck:
loopback ports 8811-8819 only, no network looked at, no login shell run, Claude and the ask feed off.
