# The core ⇄ shell message protocol

Build-order step 6 (docs/NATIVE-PORT.md §4): **design the core/shell boundary as an asynchronous message
protocol, not a synchronous function-call ABI.** A shell must be able to run against a core that is either
in-process (local, a direct call that skips any socket) or across a network (a remote deck on another
machine). If the boundary were synchronous calls, the remote case would be a blocking round-trip per call
and unusable. So: request/event messages, serialisable, delivered asynchronously; the local case is a
direct dispatch of the same messages.

**This is now the surface the browser desk runs on (build-order step 5, 2026-09-10).** `lib/phone.js` is
the server, `renderer/desk/bridge.js` the client that maps every function in `preload.js` onto it, and the
whole desktop renderer runs against it unchanged — so the list below is not a plan, it is what a complete
deck client needs. The phone page speaks a subset of it (one attached tile, no window messages, no events).

## Frame shape

Every message is a JSON object with a tag `t`. Requests that expect a reply carry a correlation id `rid`;
the reply echoes it. A `rid` of `0` means fire-and-forget (no reply is sent).

### Shell → core

| `t` | fields | meaning |
|---|---|---|
| `call` | `rid`, `fn`, `args[]` | invoke a core action by name (the table below); answered by `ok`/`err` unless `rid` is 0 |
| `attach` | `id`, `since?` | this client shows tile `id` now; the core answers with a `data` replay marked `replay:true`. A desk attaches several tiles (one message each); a phone's attach replaces its one. `since:true` asks for only what was written since the pty last changed size (a redraw) |
| `detach` | `id?` | stop carrying tile `id` (a desk); with no `id`, everything (a phone) |
| `input` | `id`, `d`, `kind?` | bytes the user typed into a tile. `kind:'key'` marks a real keystroke; `kind:'wheel'` marks the one synthesised thing a desk may send (R8 narrowed, 2026-11-18): exactly one SGR wheel report, and only while the deck's meta for that tile says `mouseOn` and `sgrMouse` (`frames.hpp` `wheel()`, which builds nothing else). `kind:'click'` (R8 narrowed again, 2026-11-19, the owner: "clicks in the remote don't work"): exactly one SGR LEFT-button press (`M`) or release (`m`), modifier bits only, under the same gate, sent as a pair for a click (never for a drag) (`frames.hpp` `click()`). Absent on the phone today. No server reads `kind` yet (both `lib/phone.js` on `main` and on branch `native-remote-server` pass only `id` and `d` on), so a deck from before the word takes a `wheel` frame as it takes a key. A server-side backstop for R8 would drop `kind:'synth'` |
| `resize` | `id`, `cols`, `rows` | the client's view size for that tile. Before the claim: recorded as the remote's size and applied under the driver rule. With the claim: it reaches the pty only while this KIND of client drives that tile ("Who drives the size" below), and is dropped otherwise; nothing is remembered |
| `shown` | `ids[]`, `hidden` | **window:** what this client's panes show; `hidden:true` = the tab is not visible, so it shows nothing and drives nothing. From one of the deck's OWN windows: hibernation's on-screen set. From a REMOTE DESK, under the remote-desk rule of 2026-11-18 (next section): only the driver rule's "a remote desk shows it" — not on screen for sleep, except that a session ARRIVING on its screen asleep is woken. On `main` of the Electron app today a desk's `shown` still counts as on screen |
| `layout` | `layout` | **window:** `{ tileId: [cells] }` — the client's grid layout; the core enforces one home per tile across the deck's OWN windows and persists it. A remote desk's layout is kept (`deskLayout`) and, under the remote-desk rule, evicts nothing and is never evicted (on `main` today: a desk is a window like any other for this) |
| `leaving` | `id`, `on` | **window:** a stepping walk has tile `id` shown here uncommitted; the window that holds it wears the leaving mark (`on:false` clears). Under the remote-desk rule it is relayed among the deck's own windows only: one sent by a remote desk is taken and tells nobody |

The window messages are accepted only after `call deskInit` (below) registered the socket as a window.

### Core → shell

| `t` | fields | meaning |
|---|---|---|
| `init` | `meta`, `home`, `platform`, `build`, `assets` | the hello on connect (the phone's boot; a desk ignores it and calls `deskInit`) |
| `meta` | `meta`, `build`, `assets` | the whole-deck snapshot, on the ~2 s tick — accounts, tiles, settings, groups, usage, triggers. Per client: a desk's copy has `mirror` set per pane to what THAT desk must render at (null when it drives), and — under the remote-desk rule of 2026-11-18 — `inWindow` null on every tile (a remote desk is told of no window on the deck's box holding a session; `shownBy` never names a desk) |
| `data` | `id`, `d`, `replay?` | pty bytes for a tile the client attached; `replay:true` is the scrollback window sent on attach |
| `exit` | `id`, `exitCode` | a tile's process ended |
| `reset` | `id` | clear the tile's screen (a wake, a refresh, a move) |
| `ok` | `rid`, `result` | reply to a `call` |
| `err` | `rid`, `error` | a `call` failed (`error` is the message) |
| `event` | `name`, `payload` | **window:** a channel the main process pushes to one window or all — the names are preload.js's: `layout:evict` (`[ids]` this window lost to another — under the remote-desk rule never sent to a remote desk by the deck's own windows, and a desk's `layout` evicts nothing there), `layout:leaving` (`{ id, on }`), `grid:changed` (`{ rows, cols }`; every layout was cleared), `tiles:activate` (`id`, a clicked toast), `screen:probe` (`{ id, req }`, answered by `call screenProbeReply`; defined, but main's `probeTrust` is not called today) |

**Heartbeat (2026-11-18).** The server sends a WebSocket ping to every client every 15 s and terminates one that
left two in a row unanswered. A client must answer pings with pongs (browsers and WinHTTP do it themselves) or
keep sending messages; any message counts as being there. (The native deck has had this since its first server;
the Electron deck gets it with the branch.)

Five more channels exist in preload.js and are **client-local in a browser**: `tiles:step` (±1), `view:redraw`,
`palette:open`, `sidebar:toggle`, `settings:toggle`. In Electron the main process produces them from the
window keys (`before-input-event` → `lib/platform.js keyAction`); the desk's bridge produces them from the
same chords itself. A native shell does the same: they never cross the wire.

## A remote desk and the deck's own windows: what is live where (2026-11-18)

The remote-desk rule settles how a REMOTE desk (the browser desk at `/desk/`, the native exe in remote mode) and
the office deck live together. As it was asked for: *"'it only shows one place' should only be on the same box, it shouldn't care about
others on other boxes; likewise, the other things panes drive (going to sleep) shouldn't impact it from a remote
session ... I should be able to open and see all of them here in different panes without impacting the desk at
all."* A design proposal (the driver as a claim) and a state file for the hosted sessions were built beside it.
THREE decks speak this protocol while that work is not merged, and a client must work with all of them:

| | Electron deck before 2026-11-19 (still RUNNING at the office until its next restart) | Electron deck with branch `native-remote-server` (**merged to `main` 2026-11-19**, live at the office deck's next restart) | native deck (`core/deck`) |
|---|---|---|---|
| **The remote-desk rule**: one home per session, `inWindow`, the leaving mark and hibernation's on-screen set are decided among the deck's OWN windows; a desk's layout evicts nothing and is never evicted; a desk is sent `inWindow: null`; an asleep session is woken when it ARRIVES on a remote's screen (new to its panes, or its tab back from hidden) and sleeps again by the office's rules | no: a desk is a window like any other (its layout evicts and is evicted, `shownBy` names it `win:N` + `remote`, its saved layout comes back minus what a real window holds) | yes (`lib/remoterule.js`) | **yes, by default** (`core/deck/remoterule.*`); `DeckOptions::remoteDeskIsAWindow = true` gives `main`'s behaviour |
| **Heartbeat**: ping every 15 s, dropped after two unanswered, any message counts | no heartbeat at all | yes | yes |
| **The desk slot**: a new `deskInit` replaces a desk not heard from for 10 s that then fails a 3 s ping (the answer to that `deskInit` waits for the probe: up to 3 s) | refused while the other socket is open | yes | yes (`DeckOptions::deskSilentMs` / `deskProbeMs`) |
| **The driver as a claim** (next section) — A PROPOSAL, not yet accepted | no: `auto` \| `phone` \| `desk`, a remote desk counts as a phone | yes (`lib/driverrule.js`) | built, **OFF** (`DeckOptions::driverClaim`): it speaks `main`'s words and sends neither `sizeBy` nor `remoteDesk`. The day the owner accepts it, that option's default flips (one line, `core/deck/deck.hpp`) |
| **The wheel from a desk** (`input` with `kind:'wheel'`) | the server passes any `input` on; `main`'s bridge sends no wheel | the bridge sends it (R8 narrowed) | the server passes it on (no filter: the client decides); it counts as one wheel notch for `where`, never as typing |
| **The contract with the hosted sessions** (below): two environment variables, `deck-state.json`, `meta.presence` | no | yes (`lib/presence.js`) | yes (`core/deck/presence.*`) |
| `deskInit`'s argument `{ name, kind }` | ignored | read | read |
| `phoneStatus` (in `getSettings` / `saveSettings` answers) | twelve fields | twelve fields | the twelve + `assets` (`'ok'` \| `'missing'`: are the pages under the asset root) and `assetRoot` (the folder tried, `""` = none found) |

What follows from the remote-desk rule for a client that is a REMOTE DESK: never expect `layout:evict` from the office and
never treat a tile's `inWindow` as "it lives elsewhere" (it is null); a single click shows any session, also one
placed in an office pane, without taking it from there; `oneWindow` is a rule of the deck's box, so a desk ignores
the setting for its own panes. For a client that is one of the deck's OWN windows nothing changes except that a
remote desk no longer appears in `shownBy` / `inWindow`.

**Who answers a session's terminal queries (14b, 2026-11-23).** Exactly one screen answers what a program asks the
terminal (device attributes, cursor position, status, colours): never two (junk in Claude's prompt), never none (a
stall). The C++ deck names it per tile in `meta.tiles[].answerer` (`core/deck/remoterule` `answerer`): `office` when
one of its own visible windows shows the tile, else `remote` when the remote desk's visible tab shows it, else
`phone` when a phone is attached to it, else `office`. The exe (either mode), the browser desk and the phone answer
only when named (the exe in local mode: unless `remote` or `phone` is named), never for a sleeping tile and never
what a replay asks (the exe and the desk: unless the session has only just started). `main.js` names none: the Electron window has a terminal for every session and answers all, so
there no remote answers.

## Who drives the size (2026-11-18: a claim)

`lib/driverrule.js` is the rule (pure; `test/driverrule-unit.js` is its table). Three KINDS of screen show a
session: **office** (the deck's own windows), **phone** (a client that never called `deskInit`; attached to one
tile) and **remote** (a client that called `deskInit`: the browser desk, the native exe). One kind drives a
tile's size; the others render at the pty's size and their `resize` / `nudge` are dropped.

`meta.settings.driver` is one of:

| value | who drives a tile |
|---|---|
| `office` | the office, always. **Was `desk`** until 2026-11-18 |
| `phone` | the phone, always (attached or not) |
| `remote` | the remote desk if it shows the tile (in its last `shown`, not `hidden`), else the office. **New** |
| `auto` | a phone attached to the tile, else a remote desk showing it, else the office |

- **Out:** `meta.settings.driver` carries only these four words. A client that knew `auto | phone | desk` must
  learn `office` and `remote`; `desk` is never sent any more.
- **In:** `setDriver(mode)` and `saveSettings({ driver })` take the four, and `desk` as `office` (older clients).
  Anything else changes nothing. `setDriver` answers the value now in force.
- **Until 2026-11-18 a remote desk counted as a phone** here (it drove under `phone`, and under `auto` with a
  phone on the same tile nobody outranked anybody). Now `phone` excludes it and `remote` excludes the phone.
- **Per tile in `meta`:** `sizeBy` (`office` | `phone` | `remote`: the kind driving it right now) and `mirror`.
  `mirror` is per client: `{ cols, rows }` = render this tile at that size and send no size; `null` = this
  client drives it (or does not show it). The office's copy and a phone's copy carry the office's `mirror`;
  a desk's copy carries its own. It is always the pty's own size (`ptySize`), never a size remembered from a client.
  A phone needs neither field: it mirrors (takes `ptySize`, sends no size) exactly when the driver is `office` or `remote`.
- **A client that becomes the driver must say its size** (a `resize` per tile it shows), also when its own
  pane did not change: the pty is still at the last driver's size. The desktop renderer does it when a tile's
  `mirror` goes from set to `null`, and re-sends when `sizeBy` is its own kind, `mirror` is null and `ptySize`
  is not what it draws (at most every 2.5 s per tile).
- **Typing:** the office's keystrokes to a tile another kind drives are dropped (R10). A phone's and a remote
  desk's `input` is never dropped.
- **`meta.tiles[].driver`** (who typed last) is `desktop` | `phone` | `remote` | `deck`: a remote desk's
  `userInput` now marks `remote`, not `phone`.
- **`meta.remoteDesk`**: `{ name, kind }` while a remote desk is connected (`name` null when it gave none), else `null`.
  `deskInit` takes one optional argument, `{ name, kind }`. `name` (a string; trimmed, control characters
  dropped, 40 at most): what the desk's machine is called, shown on the other screens as "the remote desk
  (NAME)". The browser desk sends the link's `?name=`; a native shell sends its computer name. `kind`
  (2026-11-18): `'browser'` or `'native'` — **a native shell sends `kind: 'native'`**; anything else, and no
  `kind` at all, is `browser`. It goes into the state the hosted sessions read (below) and comes back in
  `meta.remoteDesk.kind`.
- **`meta.presence`** (2026-11-18): `{ where, since, lastInputFrom }`, the same values as the state file
  below; `null` from a core that keeps none.
- **The button** (every screen): one click sends `setDriver(<this screen's kind>)`; a click while
  `driver` is already this screen's kind sends `setDriver('auto')`. Look: green when `driver` is this
  screen's kind, grey when it is another kind, a third look for `auto`; the icon shows who drives.

### Which deck has the claim, and telling them apart

The section above is the CLAIM as the branch built it and as the native deck implements it behind
`DeckOptions::driverClaim`. Until the owner accepts the proposal a client meets decks without it — `main`, and the
native deck in its default state — and must not send them words they do not know:

| | a deck from before the claim (`main` today) | a deck with the claim |
|---|---|---|
| `meta.settings.driver` | `auto` \| `phone` \| `desk` | `auto` \| `office` \| `phone` \| `remote` (`desk` is never sent) |
| `meta.tiles[].sizeBy` | absent | `office` \| `phone` \| `remote`, on every tile |
| `meta.remoteDesk` | absent | `{ name, kind }` while a remote desk is connected, else `null` — the key is always there |
| `meta.tiles[].driver` (who typed last) | `desktop` \| `phone` \| `deck` | also `remote` |
| `setDriver(mode)` | the three; another word changes nothing | the four, and `desk` as `office` |
| `deskInit` | takes no argument; one sent anyway is ignored (`deskOpen(ws)`) | one optional argument `{ name, kind }` |
| the button | cycles auto → phone → desk | one click = `setDriver(<this screen's kind>)`; on its own claim, `setDriver('auto')` |
| a client that becomes the driver | says nothing extra | says its size (`resize`) when a tile's `mirror` goes from set to `null`, and again (2.5 s apart at most) while `sizeBy` is its kind, `mirror` is null and `ptySize` is not what it draws |
| the native deck (`core/deck`) | `DeckOptions::driverClaim` off (the default): this column, exactly. It also takes `office` as `desk` from a client that already speaks the new words; `remote` changes nothing. A settings file a deck WITH the claim wrote (`office`, `remote`) is read as `desk` / `auto` | on: this column, exactly (`sizeBy` on every tile, `remoteDesk` always present, `mirror` = the pty's own size, `meta.tiles[].driver` = `remote` for a desk) |

**A new deck is any ONE of:** `settings.driver` is `office` or `remote`; a tile has a `sizeBy` field; meta has a
`remoteDesk` key (`windows/src/ui.hpp` `newDriverWords`). All three are on every meta a new deck sends, so nothing
is remembered between metas. A client never sends `office` or `remote` to a deck that did not show one of the marks.

Under EITHER rule the remote-desk rule of the section before holds on a deck that has it: size is not part of it
(with the claim off a remote desk still counts as a phone for the size: it drives what it shows under `auto` and
`phone`, and the office is read-only on it).

## The deck's contract with the sessions it hosts (2026-11-18)

Not a websocket message: what a core gives the PROCESSES it spawns, so a Claude session can tell that it runs
in a deck and which screen the user is on. A native core implements the same three things; the reference is
`lib/presence.js` (pure; `test/presence-unit.js` is its table) and `main.js` (`spawnTile`, `startPresence`,
`sawInput`, `presenceTick`).

**1. The environment of every tile's process** (Claude and shell alike), added to what the deck already sets
(`TERM_PROGRAM=claude-deck`, `COLORTERM=truecolor`, `CLAUDE_CODE_FORCE_SESSION_PERSISTENCE=1`, the account's
`CLAUDE_CONFIG_DIR`, the inherited `CLAUDE*` / `AI_AGENT` / `NO_COLOR` removed):

| variable | value |
|---|---|
| `CLAUDE_DECK_TILE` | the tile's id |
| `CLAUDE_DECK_STATE` | the absolute path of the state file |
| `CLAUDE_DECK_USERDATA` | the C++ deck only: its own state folder (2026-11-23), so the `deck-triggers` skill writes to this deck's trigger inbox; the Electron deck's folder is the default the skill finds without it |

A session without them is not in a deck and assumes the user is at its own machine.

**2. The state file**, `<the deck's data folder>/deck-state.json`, UTF-8, two-space JSON plus a final newline,
keys in this order:

```
{ "v": 1,
  "deck": { "pid": <int>, "startedAt": <ISO time>, "version": <string> },
  "where": "office" | "remote" | "phone" | "away",
  "since": <ISO time `where` took this value>,
  "remote": { "kind": "browser" | "native", "name": <string or null> } | null,
  "phone": { "attached": <bool> },
  "lastInputFrom": "office" | "remote" | "phone" | null,
  "deliver": <one plain-ASCII English line> }
```

- **Written atomically** (temp file, flush, rename over) and **only when the text differs from what was
  written last**. Never on a timer: a deck where nothing changes does not touch the file (a test holds its
  mtime across meta ticks). It is written once at start, before the first tile is spawned.
- **`where`** = the kind of screen the last REAL input came from; the newest input wins and the file is
  written at that moment, not at the next tick (a session reads it right after the prompt that asked).
  Real input is: `input` data from a client that is not a terminal reply (`ESC[I` / `ESC[O`, `ESC[…R|c|n`,
  an OSC answer, a mouse report) — from the office and from a remote desk ALSO one SGR mouse press or wheel
  report (`ESC[<B;c;rM` with bit 32 of B clear), never motion; a `userInput` call; at the office also any
  key-down in one of the deck's own windows, and a key the driver rule refused (R10). From a phone,
  `ESC[1;5F` / `ESC[1;5H` and wheel reports do not count (its page sends them by itself). Not input: focus,
  a client connecting, `shown` / `layout`, the deck's own writes to a pty.
- **`away`**: no input from any screen for `settings.awayMinutes` (default 30, 0 = never, at most 1440),
  checked on the meta tick. `lastInputFrom` keeps the last screen; any input ends it.
- **Before any input**: `office`, `lastInputFrom: null`, the away clock running from the start. If the
  previous run's file is readable (`v: 1`, a `lastInputFrom`), `where` and `lastInputFrom` start from its
  `lastInputFrom` — a deck restarted from a remote desk does not claim the office — and `since` carries over
  when `where` is unchanged.
- **`since`** moves only when `where` changes. **`remote`** is the desk that has called `deskInit` and is
  connected (its `kind` and `name`, above), else null. **`phone.attached`**: a client that is not a desk and
  did not come in on the desk's socket path (`/desk/ws`) is connected. Both are read on the meta tick.
- **`deliver`** is derived from the rest (`lib/presence.js deliver()`); a native core should produce the same
  sentences so a standing instruction written against one deck reads the same on the other.
- The file is left in place when the deck quits; `deck.pid` not running = stale.

**3. `kind` in `deskInit`** (above): a native shell says `'native'`.

**Where this is live:** the Electron app from `main` (the branch `native-remote-server`, merged 2026-11-19; the running
office deck has it from its next restart) and the native deck (`core/deck/presence.*`; `deck.cpp` `spawnTile` adds the two variables), which writes
the same file by the same rules; `deck.version` there is `core/deck/version.hpp`'s.

## The `call` surface (the bridge)

Every function `preload.js` gives the desktop renderer, and how it travels. **call** = `{ t:'call', rid, fn, args }`
answered by `ok`/`err`; **fire** = the same with `rid: 0`; **message** = one of the frames above; **local** =
the browser's own, no wire.

| bridge function | how | notes |
|---|---|---|
| `init()` | call `deskInit` (one optional arg `{ name, kind }`, above; a native exe sends its computer name and `kind: 'native'`; `main` ignores the argument) | registers the socket as a window; answers `{ home, meta, platform, layout, windowIndex, grid }` (what `deck:init` answers a window). Refused with "one desk at a time" while another desk is connected and answering; on the branch and the native deck a desk not heard from for 10 s that also fails a 3 s ping is replaced by the caller (2026-11-18) — the answer then takes up to 3 s. On `main`: refused while another desk's socket is open |
| `newTile(spec)` `closeTile(id)` `moveTile(id, account)` `restartTile(id)` `refreshTile(id)` `sleepTile(id)` `wakeTile(id)` | call | tiles / lifecycle |
| `renameTile(id, title)` `pin(id, pinned)` `reorder(ids)` `setGroup(id, group)` `setGroups(names)` | call | |
| `setShown(ids)` | message `shown` | plus `hidden` from the page's visibility |
| `saveLayout(layout)` | message `layout` | |
| `previewLeaving(id, on)` | message `leaving` | |
| `attach(id, opts)` | message `attach` (`since` = `opts.sinceResize`) | the desk sends it only for a shown pane; a pane coming back on screen is reset locally first |
| `input(id, data)` | message `input` (`kind:'key'`, `'wheel'` or `'click'`) | mouse reports and focus reports xterm synthesises are dropped before the wire (R8), except one SGR wheel report, or one SGR left press / release, while the tile's `mouseOn` and `sgrMouse` are both true (the `input` row above) |
| `userInput(id)` | fire `userInput` | a real key press: marks who typed last (`meta.tiles[].driver`: `phone` from a phone; from a desk `remote` with the claim, `phone` without) |
| `resize(id, cols, rows)` | message `resize` | |
| `nudge(id, cols, rows)` | fire `nudge` | a redraw's resize-and-back; honours the driver (with the claim: dropped unless this kind of client drives the tile) |
| `screenProbeReply(r)` | fire `screenProbeReply` | |
| `externalSessions()` `pastSessions()` `adoptSession(s)` `moveAll(from, to, continueIfUsage, onlyIds)` | call | sessions |
| `holdAdd(id, item)` `holdRemove(qid)` `triggersList()` `triggerDeclare(name)` `triggerSet(name, value)` `triggerRemove(name)` | call | special prompts / triggers |
| `getSettings()` `saveSettings(s)` `setDriver(mode)` `saveAccounts(rows)` `refreshUsage()` | call | a desk's `getSettings` answers the desktop shape (alias file, phone status and link); `saveSettings` from any remote never changes the phone block |
| `restartWhenQuiet(on, why)` `restartNow()` `updateLater()` | call | restart when quiet: arm (`why` `'manual'` or `'update'`) or cancel; restart at once; dismiss the update offer. The state rides in `meta.restart` (`armed`, `blockers[]`, `settleLeftMs`, `shells`, `update`). A remote can arm or trigger a restart, never name the command that relaunches (`relaunch.json`) |
| `readClipboard()` `writeClipboard(t)` | local | `navigator.clipboard` (reads need a secure context AND the user's permission), a textarea + `execCommand('copy')` fallback for writes. **Paste from a remote client (2026-11-18)**: the client pastes ITS OWN machine's clipboard as ordinary `input` - bracketed (`ESC[200~` … `ESC[201~`) when the tile's stream asked for it (`?2004h`), line ends as CR - plus a `userInput`; it never sends `0x16` for a paste and sends nothing when its clipboard holds no text (either makes Claude Code read the clipboard of the DECK's PC). The browser desk does it by leaving Ctrl+V / Ctrl+Shift+V / Shift+Insert to the browser's own `paste` event |
| `toggleFullscreen()` | local | the browser's Fullscreen API |
| `openWindow(spec)` | local no-op → `null` | a new deck window is a BrowserWindow on the deck's box; a second remote desk is unsupported |
| `dragRescue()` | local no-op → `false` | drives Electron's debugger; nothing to drive from a browser |
| `setOscTitle(id, title)` | local no-op | the deck's own window reads the title from the same stream |
| `onData` `onExit` `onReset` `onMeta` | messages `data` `exit` `reset` `meta` | |
| `onLayoutEvict` `onLeaving` `onGridChanged` `onActivate` `onScreenProbe` | `event` by name | |
| `onTileStep` `onRedraw` `onPalette` `onSidebarToggle` `onSettingsToggle` | local (the key handler) | |

Phone-only calls, also in the table main keeps (`PHONE_ACTIONS`): `transcript(id, opts)` (the text view's
reader) and `POST /upload` (a file for the session; HTTP, not the socket).

**The core's C ABI is exactly this set of message types plus this named-call table** — nothing more. A shell
links the core, hands it a send-callback, and pumps received frames in; the core answers with
`ok`/`err`/`meta`/`data`/`exit`/`reset`/`event`.

## What step 5 settled, and what stays open

**Settled by use (2026-09-10):** the frame shape and tags; `rid` correlation with `0` as fire-and-forget;
`attach` per pane with `since`; `detach` per pane; the three window messages and `deskInit`; `event` as the
one shape for pushed channels; per-client `meta` (a desk's `mirror`); `input.kind`; the full call table above.
Backpressure: the server skips a frame for a client whose socket buffer is over 4 MB (`MAX_BUFFERED`) —
a slow remote loses output rather than growing the heap, and its next attach replays the window; the desk
lives with that as the phone does.

**Still open for the native core:** whether `meta` diffs or resends whole (whole today: ~2 s, a few KB per
tile); a windowed ack for `data` instead of the drop-over-4 MB rule; the exact serialisation of `transcript`
and `pastSessions` (large payloads, phone-only today). (The R9 rename of the driver's words was overtaken on
2026-11-18 by the claim above: `phone` stayed the phone, `remote` is the remote desk, `desk` became `office`.)

## Local vs remote

The local shell (Windows/Swift on the same box as the core) gets a **direct dispatch**: the send-callback
is a function pointer, not a socket, and `attach`/`data`/`meta` never serialise. The remote shell gets the
same frames over a websocket (the phone transport). Because both go through the identical message types,
"a remote desk counts as a phone" (§3 F6 ruling) falls out for free — it is the same protocol with a
different transport. The browser desk is the proof: one client, one protocol, the whole deck.
