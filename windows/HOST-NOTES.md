# The native deck (`core/deck/` + `windows/host/`)

What exists after the overnight builds of 2026-11-18 and 2026-11-19 and the sharing refactor of 2026-11-19 (how a
shell holds the deck is the core's too now: `core/deck/options.*`, `core/deck/hosting.*`), what does not, and how a
shell plugs it in.
The Electron app (`main.js` + `lib/*.js`) is the specification: every function names the JS it ports, and where
the two differ on purpose it is listed under "Differences from main.js" at the bottom.

**The goal this serves (2026-11-18):** complete parity, so the C++ deck can replace the Electron one
entirely — with its logic where a Swift (macOS) shell can share it. So the deck is no longer a Windows class:
it is **`core/deck/`, pure C++17, no OS header**, and `windows/host/` is only the Windows implementation of the
seams it asks for. The last section of this file is the list of what still stands between this and switching
Electron off.

## The architecture, as built

| Layer | Where | What is in it |
|---|---|---|
| **The deck** (shared) | `core/deck/` | tiles and their processes, the call table (`PHONE_ACTIONS` + the window's IPC handlers), frame dispatch per `core/PROTOCOL.md`, **N clients** (windows, phones, one remote desk) each with its own attached set / shown set / layout / mirror view / rid space, per-client meta (`remoteMeta` clipping, `deskMeta`, the dedupe + 20 s keep-alive), the driver rules (`remoteDrives`, R10's input lock, mirror, resize / nudge arbitration — and, behind an option, the driver as a claim: `driverrule.*`), layout and one-window rules (among the deck's OWN windows: `remoterule.*`, the remote-desk rule of 2026-11-18), persistence shapes (`tiles.json`, `settings.json`, `triggers.json`, `held-prompts.json`, `usage-cache.json`, `running.json`), hibernation + the memory guard, special prompts + triggers + the trigger inbox, restart when quiet + the update offer, Elsewhere / Past / adopt, account moves, the shell-host scan, Claude Code's startup dialogs and the trust prompt, the usage poller, the phone's text view; since 2026-11-19 the ask feed (`asks.*`), the page viewer (`viewer.*`), the on-call's desk requests (`deskopen.*`), cloud sessions (`cloud.*`) and their wiring (`deck_parity.cpp`) |
| **The server** (shared) | `core/deck/server.*`, `http.*`, `ws.*`, `hash.*` | HTTP request parsing, the token and cookie, every route and its headers, the websocket handshake / frames / heartbeat / close, the 4 MB skip rule, which address to bind, `phoneStatus`, `syncPhone`, the Tailscale Serve link — bytes in, bytes out |
| **The seams** | `core/deck/seams.hpp` | `Clock`, `Files`, `PtyHost` / `Pty`, `ProcessList`, `Random`, `Env`, `Notifier`, `Http`, `Credentials`, `Listener`, `Net`; `PhoneHost` and `Lifecycle` in `deck.hpp` |
| **Hosting** (shared, 2026-11-19) | `core/deck/options.*`, `core/deck/hosting.*` | how EVERY shell holds a deck: the one options builder (`buildDeckOptions`) and the shipping defaults; the `Inbox` the pty / socket threads post to (the shell gives it a wake), the pump, the shell's windows as clients with exact routing by client id. No thread, no OS call. Tested in both path modes (`core/deck/README.md` "Hosting") |
| **Windows** | `windows/host/` | `conpty.*` (a process in a pseudoconsole), `statefile.*` (Win32 files, the atomic write, `WinFiles`, `WinClock`), `winseams.*` (`WinPtyHost`, `WinProcessList`, `WinRandom`, `WinEnv`), `listener.*` (`WinListener` on Winsock, `WinNet`), `winhttp.*` (`WinHttpGet`, `WinCredentials` — off unless asked), `localdeck.*` (the seams over a `core::Host`, the wake as a condition variable or a posted message, ONE window client); the app's own `windows/src/deckhost.hpp` is the same over N windows |

`core/deck/README.md` has the file-by-file table. The core is **pumped**: `handle(client, frame)`,
`onPtyData(token, bytes)`, `onPtyExit(token, code)`, `tick()` at least every `nextDueMs()` ms; every frame out goes
through a sink that answers whether it went out (false = that client's socket is over 4 MB: skip).

### Counts (2026-11-19, the sharing refactor on `native-windows`, merged with that day's `main`)

| Suite | Command | Result |
|---|---|---|
| model | `core/test/build.ps1` | 103 / 103 |
| frame codec | `core/test/proto/build.ps1` | 60 / 60 |
| the key table and the mouse encoder | `core/test/input/build.ps1` | keys 1221 / 1221 (the Windows unit checks, the Mac bridge's rows, every key x modifiers x platform against `keyAction`), mouse 762 / 762 (xterm.js's own service) |
| session ports | `core/test/session/build.ps1` | 604 / 604 |
| **the core deck and server, on fake seams** | `core/test/deck/build.ps1` | **27906 / 27906, then 6122 / 6122 on the POSIX twin of the fakes** (darwin, `/` paths, `$SHELL -l`: the scenarios, hosting, options, the rules under the deck, the server). 54 of them are 2026-11-19's `options` and `hosting` (the shared builder and host loop, the Mac draft's window questions, the routing race). The 27852 before them: the 22309 of the third build, plus `remoterule` 4573 (`lib/remoterule.js`: its unit cases and a sweep), `driverrule` 572 (`lib/driverrule.js`: the branch's 171 checks one for one, a sweep of 312 rows, the button, 50 `lib/phone.js deskMeta` cases), `ruling` 380 (the deck on fake seams: an office of two windows + a remote desk + a phone — placements, sleep, the wheel, sizes / mirrors / R10 under every driver value with the claim on and off, and the old window rule beside the ruling) and 18 more in `server` (the heartbeat's "any message counts", the desk slot, `phoneStatus.assets`). Before that: the 1935 of before (4 "refused by name" checks went with the refusals), plus `asks` 6554, `viewer` 8222, `deskopen` 3717, `cloud` 1634 (each the REAL `lib/*.js` run on synthetic input, replayed) and `parity` 247 (those four as the deck uses them on fake seams, incl. the 45 `/view` + `/ask` requests replayed against the real `lib/phone.js`) |
| the Windows seams + the deck on real pseudoconsoles | `windows/host-test/build.ps1 -Gui` | **211 / 211** (3 more: the builder hands on the version and the two new options), as a console exe and as a GUI-subsystem exe (`-Only parity`: the 2026-11-19 ports on real files, and the exit watchdog ending a hung child) |
| **one real Claude session** (gated: the owner's word) | `windows/host-test/build.ps1 -Only claude-real` + its variable | **15 / 15** (2026-11-18 23:36, Claude Code 2.1.328) |
| the server for real (loopback) | `windows/host-test/serve.ps1` | 5 parts, 56 checks, all passed (2026-11-19 01:10, parts 4 and 5 with a PRIVATELY linked app: `-Exe`, see "The remote desk and the office") |

```
node core/test/deck/gen-fixtures.js [--routes]                                      # only after the JS it reads changed
node core/test/deck/gen-asks.js; node core/test/deck/gen-viewer.js; node core/test/deck/gen-deskopen.js; node core/test/deck/gen-cloud.js
node core/test/deck/gen-routes2.js                                                  # /view + /ask from the real server, 127.0.0.1:8813 (PowerShell)
node core/test/deck/gen-remoterule.js <lib/remoterule.js>; node core/test/deck/gen-driverrule.js <lib/driverrule.js>   # the review branch's today, main's after the merge
powershell -NoProfile -ExecutionPolicy Bypass -File core/test/deck/build.ps1         # [remote|wire|startup|held|usage|rules|presence|transcript|asks|viewer|deskopen|cloud|deck|server|parity|remoterule|driverrule|ruling]
powershell -NoProfile -ExecutionPolicy Bypass -File windows/host-test/build.ps1 -Gui # [-Only conpty|rules|seams|deck|parity]
powershell -NoProfile -ExecutionPolicy Bypass -File windows/host-test/serve.ps1      # [-Skip browser,native,app] [-Exe <path>]   ~2.5 minutes
```

The host test starts only `cmd.exe` in pseudoconsoles, in a folder of its own under `%TEMP%`, with a fake home
and alias file. It never touches `%APPDATA%\claude-deck-native`, never starts `claude.exe` (its `claudeExe` points
at a file that does not exist, and `allowClaude` is off), binds only 127.0.0.1:8819, asserts that no process it
started has a visible window, and asserts that every pid it ever saw is gone at the end.

## ConPTY: what was matched

node-pty 1.1.0 as main.js calls it (`pty.spawn(file, args, { name, cols: 120, rows: 30, cwd, env, useConpty: true })`):

- **The system ConPTY** — kernel32 `CreatePseudoConsole` / `ResizePseudoConsole` / `ClosePseudoConsole`. main.js never
  passes `useConptyDll`, so node-pty does NOT use its bundled `conpty.dll` + `OpenConsole.exe`; neither does this.
- **Flags 0** — `conptyInheritCursor` is not passed, so `PSEUDOCONSOLE_INHERIT_CURSOR` is off.
- **The child** — `CreateProcessW(nullptr, cmdline, …, bInheritHandles FALSE, EXTENDED_STARTUPINFO_PRESENT |
  CREATE_UNICODE_ENVIRONMENT, env, cwd)` with `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` and `STARTF_USESTDHANDLES` over
  three null handles. No `CREATE_NO_WINDOW` is needed or used: the child's console IS the pseudoconsole, whose
  conhost runs headless. Checked: no visible top-level window belongs to any descendant of the test process, from a
  console parent and from a GUI-subsystem parent.
- **The command line** — node-pty's `argsToCommandLine`, ported line for line.
- **"File not found: …"** — node-pty's lookup (`path_util.cc`), with its two oddities kept.
- **`TERM`** — `name: 'xterm-256color'` only sets TERM on POSIX; on Windows nothing is added. Same here.
- Anonymous pipes instead of node-pty's named pipes (nothing can tell the difference).

## Threads

One thread owns a `LocalDeck` (and with it the core deck and the server): the one that constructs it. `start()`,
`handle()`, `pump()`, `shutdown()` are called on it, and **the emit callback is only ever called on it**.

Other threads only ever append to one mutex-guarded queue (`Shared` in `localdeck.cpp`) and wake the owner
(`PostMessage(notifyHwnd, notifyMsg, 0, 0)` — at most one outstanding — and a condition variable for headless
callers, `waitForWork(ms)`):

- each running tile's two `ConPty` threads — a reader and a waiter (the exit always follows the last byte);
- the listener's one thread (`listener.cpp`): it accepts, reads and flushes; a burst from one socket is merged
  into one event, as a pty's is. `send()` writes straight to the socket from the owner's thread and queues only
  what the socket would not take — `buffered()` is that queue, which is `ws.bufferedAmount`;
- one short-lived thread per usage request (`winhttp.cpp`), when usage polling is on.

`pump()` drains the queue into the core (`onPtyData` / `onPtyExit`, `Server::onAccept` / `onBytes` / `onClosed`,
queued tasks), then `Deck::tick()` and `Server::tick()`. **Timers fire only inside `pump()`**: call it on the wake
message AND at least every `nextDueMs()` ms (a 50 ms `WM_TIMER` is enough).

## The protocol table (core/PROTOCOL.md), call by call

A **window** client is a window of this deck: its input is `tile:input`'s path, its resizes the desktop's, its meta
`collectMeta`'s own. A **remote** client goes through `PHONE_ACTIONS`: a phone until it calls `deskInit`, a desk after.

### Shell → core frames

| frame | status |
|---|---|
| `call` | the table below. A window: `rid` 0 sends no reply. A remote: every call is answered, as `lib/phone.js` does |
| `attach { id, since? }` | **done** — a window / a desk carries a set of panes, a phone one tile (a new attach replaces it); the answer is one `data { replay: true }` (mode prefix + the window; `since` only for a window / desk) |
| `detach { id? }` | **done** |
| `input { id, d, kind? }` | **done** — a window's is refused while a remote drives the tile (R10), with one `input:refused` per tile per 3 s (`reason`: `phone` under the driver setting, `phone-attached` under auto); a remote's goes in. Wakes a sleeping tile (the key is dropped). `kind` is not read, as on `main` |
| `resize { id, cols, rows }` | **done** — coalesced 150 ms; a window's ignored while a remote drives, a remote's ignored under driver `desk`; a remote's size is remembered as what the desktop mirrors |
| `shown { ids, hidden }` | **done** — windows after `deskInit`, a desk; a phone's is ignored |
| `layout { layout }` | **done** — one home per tile across every window and the desk (`layout:evict` to the loser) unless `oneWindow` is off; a desk's is saved as `deskLayout` |
| `leaving { id, on }` | **done** — relayed to every other window / desk as `layout:leaving` |

### Core → shell frames

`init` (the hello: a window when it is added; a remote on connect, with `assets`, `build`, `pageKey`), `meta`,
`data`, `exit`, `reset`, `ok`, `err`, `event` — names sent: `grid:changed`, `input:refused`, `layout:evict`,
`layout:leaving`, `layout:place` (`{ id, cells }`: the on-call's desk was placed in this window), and — when the shell
reports a clicked toast through `Deck::toastClicked` — `tiles:activate` (the tile id) or `asks:open`
(`{ sessionId, tileId }`). Never sent: `screen:probe` (main.js does not send it either).

### Calls

| call | status |
|---|---|
| `deskInit` | **done** — a window: `{ home, meta, platform, layout, windowIndex, grid }`; a remote: registers it as a desk (one at a time; its window index is the next free one at the first desk and then stays), its meta with `mirror` per pane |
| `newTile(spec)` | **shell: done.** `kind:'claude'`: done through the launch seam. `kind:'cloud'`: main.js `cloudSpec`'s checks in its words (a task, at most 24,000 characters, a folder that exists), then **refused unless `allowCloudCreate`** (`making cloud sessions is switched off in the native local deck (allowCloudCreate)`); allowed, it runs `claude --cloud "<task>"` and the tile becomes the session its output names |
| `closeTile` `restartTile` `refreshTile` `sleepTile` `wakeTile` | **done** |
| `renameTile` `pin` `reorder` `setGroup` `setGroups` | **done** |
| `userInput` `input` `resize` `nudge` | **done**, per client kind (`driver: 'desktop'` / `'phone'`) |
| `setOscTitle(id, title)` | **done** for a window (the local shell IS the window that reads the title); a remote's is `unknown action`, as it is not in `PHONE_ACTIONS` |
| `screenProbeReply` | accepted, does nothing (no probe is ever sent) |
| `getSettings` | **done** — a window and a desk: the desktop shape (`accounts`, `accountRows`, `aliasFile`, `userswitch`, `bash`, `phoneStatus`); a phone: no token, no paths |
| `saveSettings(s)` `setDriver(mode)` | **done** — all of `applySettings`; a remote never changes the phone block; a change to the phone block restarts the server (`syncPhone`) |
| `moveTile` `moveAll` | **done** through the launch seam (`userswitch --start` in Git Bash); `moveAll` types `continue` into a moved session that had stopped on a usage notice |
| `adoptSession` `externalSessions` `pastSessions` | **done**. Adopt of a local session is refused while `allowClaude` is off — before the session's process elsewhere is touched. `adoptSession({ cloudId, account })` opens a cloud session as a tile (nothing runs here, so it needs no switch); `externalSessions` lists each account's other live cloud sessions when cloud polling is on |
| `holdAdd` `holdRemove` `triggersList` `triggerDeclare` `triggerSet` `triggerRemove` | **done** |
| `restartWhenQuiet` `restartNow` `updateLater` | **done** in the core; a restart needs a `Lifecycle` from the shell, and the Windows shell gives none yet (`restartNow` answers `false`, an armed restart cancels itself when its moment comes) |
| `transcript(id, opts)` | **done** (the phone's text view) |
| `refreshUsage` | **done** when usage polling is on; else refused by name |
| `saveAccounts` | **done** in the core, **refused** unless `allowAccountEdit` (the alias file is the owner's) |
| `viewHtml` `sessionHtml` | **done** (`lib/viewer.js`): a window, a phone, a desk |
| `askDraftGet` `askLogState` `askDraft` `askClear` `askDone` | **done** (`lib/asks.js`, main.js `doneAsk`). With no ask folder (the feed off) they answer as main.js does with `asks` null: null / true / `the ask pane is not running` |
| `askOpenLink(url)` `openExternal(url)` | **done** for a window (`ask:open-link`, `open:external`; a remote's is `unknown action`, as neither is in `PHONE_ACTIONS`): the core decides — http(s) outside, a file in its own program, something that would RUN only shown in its folder; `openExternal` takes `https://claude.ai/` links only — and the shell's `Lifecycle` does it (none given: `false`) |
| `readClipboard` `writeClipboard` `toggleFullscreen` `openWindow` `dragRescue` | the shell's own; they never reach a deck (`Deck::openWindow` is the core's half of F5) |

A refusal is `{ t: 'err', rid, error: "not in the native local deck yet: <fn>" }`; a remote's call outside
`PHONE_ACTIONS` is `unknown action: <fn>`. **After the third build only two `PHONE_ACTIONS` entries can still answer
that, each behind a switch that is the owner's:** `refreshUsage` (usage polling off) and `saveAccounts`
(`allowAccountEdit` off). Everything else in the table is answered.

## The Claude seam

`core/deck/rules.hpp` `claudeLaunch(ClaudeLaunchInput) -> { file, args, env, cwd, sessionId }` is `spawnTile`'s
plain Claude branch as a pure function; the `moveFrom` branch (userswitch) is built beside it in `Deck::spawnTile`.
`--resume <id>` only when a transcript exists, else `--session-id <id>`; `--chrome` / `--no-chrome`; `--model` and the
first message last; the environment without `CLAUDE_*` / `CLAUDECODE*` / `AI_AGENT` / `NO_COLOR`, plus the three the
deck sets, `CLAUDE_CONFIG_DIR`, `CLAUDE_ONCALL_KIND`.

`DeckOptions::allowClaude` — **default false in the core** — is the one switch: off, the tile takes main.js's own
"could not start" path (`exited`, `exitCode -1`, a line in its pane saying why); on, the launch goes to `PtyHost`.
`LocalDeck` passes `LocalDeckOptions::allowClaude` through.

**The shipping default (decided 2026-11-18, after the launch passed against a real `claude.exe`): ON in a real
local deck, off everywhere else.** `LocalDeckOptions::allowClaude` defaults to `host::shippingAllowClaudeHere()`
(`localdeck.hpp` / `.cpp`; the rule itself is the pure `shippingAllowClaude(exe, args)`, tested):

| The process | `allowClaude` |
|---|---|
| `ClaudeDeckNative.exe` on its own state folder (no `--state-dir`), however local mode was chosen | **on** |
| `ClaudeDeckNative.exe --state-dir <folder>` — every test of the app (app.cpp refuses a test run without one), any throwaway deck | off |
| any other exe (`deck-host-test.exe`, its GUI twin, `deck-serve-host.exe`, a tool that links the host) | off |
| `--allow-claude` on the command line | on, wherever (a test that names it) |
| `--no-claude` on the command line | off, wherever (terminal tiles only); the last of the two switches wins |

So a shell that leaves the option alone ships with Claude sessions on, and nothing that merely constructs a
`LocalDeck` in a test can start one. The two switches are read by the host off the process's own command line
(app.cpp's parser ignores what it does not know); the clean form is two fields in app.cpp's `Options`.
`usagePolling` is still off: its real run has not been made.

What the first build listed as missing before `allowClaude` could be used is **ported now** and tested on the fake
process seam and against the recorded dialogs (`test/fixtures/startup-*.raw`):

1. the **trust prompt** answerer (`trustWatch`): nudge one column to force a full repaint, read the marker off it,
   Down while it is on "No", Enter only once it was SEEN on "Yes", twelve tries, off with `autoTrust`;
2. `lib/startupprompts.js` (`StartupWatch`): the "Settings Warning" Continue 700 ms after its first paint unless a
   person typed since the spawn, the toast naming what was skipped (once per distinct warning per run), and
   `status: 'waiting'` while a session sits on any startup dialog;
3. the `moveFrom` branch for `moveTile`.

**It has started real Claude Code sessions** (2026-11-18, three of them, on the owner's word): see "The two real
runs" for what each showed.

## The deck's contract with the sessions it hosts (deck-state.json)

Ported from the deck's REVIEW branch (`_wt-deck-remote-rule`: `lib/presence.js`, "The deck's contract with the
sessions it hosts" in its `core/PROTOCOL.md`) — an Electron deck from origin/main does not have it yet; the native
deck having it early gates nothing. `core/deck/presence.{hpp,cpp}` is the pure part, `Deck` wires it:

- **The environment of every tile's process** (shell and Claude, the userswitch move too): `CLAUDE_DECK_TILE=<tile id>`
  and `CLAUDE_DECK_STATE=<state dir>\deck-state.json`, beside `TERM_PROGRAM=claude-deck`. (Added where the deck
  builds a spawn, `Deck::spawnTile`, because `baseEnv` knows no tile; `core::tileEnv` is the pair.) Since 2026-11-23
  also `CLAUDE_DECK_USERDATA=<state dir>`, so the `deck-triggers` skill writes to this deck's own trigger inbox.
- **The state file**: the exact shape and key order, two-space JSON and a final newline, written through
  `Files::writeAtomic` at start (before any tile is spawned) and then ONLY when its text changes. `where` is the
  screen the last real input came from — a window (`office`), a remote that has called `deskInit` (`remote`), any
  other remote (`phone`); a terminal's own replies never count, one SGR mouse press or wheel notch counts from the
  office and a desk but not motion, a phone's own scrolling (`ESC[1;5F` / `ESC[1;5H`, wheel reports) does not, a
  key the driver rule refused does, the deck's own writes (a held prompt) do not. `away` after
  `settings.awayMinutes` (default 30, 0 = never, at most 1440) on the meta tick. The last input's screen carries
  over a restart. `remote` = `{ kind, name }` from the desk's `deskInit` argument (`kind` `browser` unless it says
  `native`); `phone.attached` = a remote that is not a desk and did not come in on `/desk/…`. The `deliver`
  sentences are lib/presence.js's, word for word.
- **`meta.presence`** = `{ where, since, lastInputFrom }`, always.
- **`meta.remoteDesk` = `{ name, kind }` goes out with the driver claim and not before** (`DeckOptions::driverClaim`;
  the `metaRemoteDesk` switch of the third build is folded into it): on THIS tree's protocol the key's mere
  presence is one of the three marks by which a client decides a deck has the claim (`core/PROTOCOL.md` "A new deck
  is any ONE of…", `ui.hpp` `newDriverWords`). "The remote desk and the office" below has the whole of it.

Fixtures: `node core/test/deck/gen-presence.js <path to lib/presence.js>` — the path is an argument, so it runs
against the review worktree today and against `lib/presence.js` on main after the merge (186 fixture checks; every
step's file text is compared byte for byte). One thing the shell must add for full parity: `Deck::windowKey(window)`
on any key-down in its window (main.js `windowKeys`); without it only keys that reach a tile say "office".
`DeckOptions::version` (the file's `deck.version`) is `core/deck/version.hpp`'s `kNativeDeckVersion` (`0.1.0-native`) unless the shell
names another (`buildDeckOptions`); see "One source for the version" below.

## The remote desk and the office: the review of 2026-11-18

The review of 2026-11-18 settled how a REMOTE desk (the browser desk, the native exe in remote mode) and the office
deck live together (the remote-desk rule). The Electron side is branch `native-remote-server` (worktree `_wt-deck-remote-rule`, pushed, **not merged**);
this is the native side. `core/PROTOCOL.md` "A remote desk and the deck's own windows" says what is live on which deck.

### The options, their defaults, and why

| `DeckOptions` (`core/deck/deck.hpp`) | default | what it is | why that default |
|---|---|---|---|
| `remoteDeskIsAWindow` | **false** | false = THE REMOTE-DESK RULE: one home per session, `windowHolding` / `inWindow`, the leaving mark and hibernation's `shownBy` are decided among the deck's OWN windows; a remote desk's layout evicts nothing and is never evicted, comes back whole when a desk reconnects, and a desk is told `inWindow: null`; an asleep session is woken when it ARRIVES on a remote's screen. true = what `main.js` on `main` does today (a desk is a window like any other for one-window and for sleep) | it is a decided rule, not a proposal: "'it only shows one place' should only be on the same box … the other things panes drive (going to sleep) shouldn't impact it from a remote session" / "I should be able to open and see all of them here in different panes without impacting the desk at all". The `true` path stays so the old rule's tests keep running and a deck can be made to match `main` until it is merged there |
| `driverClaim` | **false** | true = the driver as a claim (`driverrule.*`): `auto \| office \| phone \| remote` (`desk` accepted as `office`), a remote desk is its own kind of screen, `sizeBy` on every tile, the office's `mirror` = the pty's own size, `meta.remoteDesk = { name, kind }`, a desk's typing marks `driver: 'remote'`, R10 locks the office when another kind drives. false = this tree's words and rules exactly, and neither `sizeBy` nor `remoteDesk` is sent | it is a PROPOSAL, not yet accepted. **The day it is, the default flips in ONE line: `bool driverClaim = false;` in `core/deck/deck.hpp`** — `LocalDeckOptions::driverClaim` is initialised from it, `buildDeckOptions` hands it on, nothing else holds a default |
| `deskSilentMs` / `deskProbeMs` | 10000 / 3000 | the desk slot: a new `deskInit` takes over from a desk not heard from for 10 s that then fails a 3 s ping | `lib/phone.js` `DESK_SILENT_MS` / `DESK_PROBE_MS` (a test shortens them) |

`LocalDeckOptions` carries `remoteDeskIsAWindow` and `driverClaim` with the same defaults; `buildDeckOptions` is
still the one place they reach the core.

### What was ported, and how it is checked

- **`core/deck/remoterule.{hpp,cpp}`** = `lib/remoterule.js` (pure): `holder`, `evictions`, `officeShownBy`,
  `officeShows`, `shownBy`, `wakesForRemote`. Fixture `remoterule.json` from the branch's module
  (`node core/test/deck/gen-remoterule.js <path to lib/remoterule.js>`): the unit file's layout sequence, `shownBy`
  rows and `wakesForRemote` rows, plus a sweep of a small world (two office windows + a desk: 1536 evictions,
  64 holders, 2048 `shownBy`, 867 wakes). 4573 checks.
- **Wired as the branch's `main.js` is**: `windowHolding` -> `remoterule::holder` over the own windows;
  `takeLayoutMap` -> `remoterule::evictions` (a remote taker evicts nothing; a remote is never in the list);
  `shownBy` -> `remoterule::shownBy(own windows, phoneWatching)`; `entryShown` for a desk -> `wakesForRemote`
  (reason `shown on a remote`) and nothing else; `deskOpen` restores `deskLayout` whole; `leaving` among own windows
  only (a desk's is taken and tells nobody); `deskMeta(..., DeskRule)` sets `inWindow: null`. Size is untouched.
- **`core/deck/driverrule.{hpp,cpp}`** = `lib/driverrule.js` (pure). Fixture `driverrule.json`
  (`node core/test/deck/gen-driverrule.js <path to lib/driverrule.js>`; it also runs that branch's
  `lib/phone.js` `deskMeta` when the file loads). The C++ test runs `test/driverrule-unit.js`'s checks one for one
  over the generated table (its 171), then a sweep (every value incl. `desk`, junk, null x every `who` x pty or
  none: 312 rows), `mode()` words, the button, and 50 `deskMeta` cases. 572 checks.
- **Wired behind `driverClaim`**: `whoLooks` / `sizeDriver`; `resizeTile` / `nudgeTile` take the caller's kind and
  ask `acceptsSize`; `collectMeta` overrides `mirror` (`officeMirror` of the pty's size, or of the size it is about
  to be mid-debounce) and adds `sizeBy`; R10 = `inputLocked`, the refusal's reason = `refusedReason`; `deskMeta` =
  `remoteMirror`; `userInput` from a desk marks `remote`; `setDriver` / `saveSettings` take the four words and
  `desk`; a stored `desk` is `office` in memory at load. Nothing of the remote's size is remembered.
- **"Became the driver -> the client pushes its size"** is the client's (the renderer's 70-meta.js; the native
  shell's own view). The server's half is tested: when the claim moves, the new driver's `mirror` goes from set
  to null, the office is told the pty's own size, the new driver's `resize` is taken, and the office's mirror
  follows it at once (the pending size), not after the debounce.
- **Scenarios on fake seams** (`core/test/deck/remoterule_test.cpp`, section `ruling`, 380 checks): two office
  windows + a remote desk + a phone. Placements (nothing evicted either way, `inWindow`, the leaving mark, the
  desk gone and back, one-window off); sleep (a session only a desk shows sleeps; the tick does not wake it; an
  arrival is woken; another pane changing wakes nothing; a hidden tab wakes nothing and its return does; typing
  from the desk wakes; a phone still wakes; a desk reconnecting with a sleeping session in its saved layout wakes
  nothing until its first `shown`); sizes, nudges, mirrors, `sizeBy`, R10 and its reason for four tiles (the desk
  shows it / a phone has it / nobody / both) under each of the four values with the claim ON, and under each of
  the three with it OFF — with and without `remoteDeskIsAWindow`; the words in and out, both ways; the wheel.
  The third build's `clients` scenario is the OLD rule's test and now sets `remoteDeskIsAWindow = true`.

### With the claim OFF (today's default), exactly

- words: `auto | phone | desk` out, always. In: those three; `office` is taken as `desk` (a client that already
  speaks the new words meant that); `remote` is no value and changes nothing. A `settings.json` a deck WITH the
  claim wrote (`office`, `remote`) is read as `desk` / `auto` in memory (decided here: without this the deck
  would say a word its clients do not know).
- size: a remote desk still counts as a phone (it drives what it shows under `auto` and `phone`; the office is
  read-only on it; `mirror` is the size a remote last sent). That is the branch's first commit too ("SIZE IS NOT
  CHANGED"): the remote-desk rule alone moved layout and sleep, not size.
- meta: no `sizeBy`, no `remoteDesk`; a desk's typing marks `driver: 'phone'`.

### The wheel from a remote desk (the branch's narrowing of R8)

The server side has no filter — the client decides what leaves it — and none was added. `input` with
`kind:'wheel'` is handled as any `input` from that client (`kind` is not read): written to the pty; for
`deck-state.json` exactly ONE SGR press / wheel report with bit 32 clear counts as input from the office or a
desk (so the wheel is one notch: `where` = `remote`), two in one message do not, motion never does, and the same
bytes from a phone are its page scrolling by itself. It is NOT typing: `lastKeyAt`, `hadInput`, `lastInputFrom`
are untouched, so the tile stays `fresh` and `driver` stays null. An office wheel into a session it may not type
into is dropped without a notice (a terminal's own report). Both with the claim on and off (18 checks).

### Heartbeat and the desk slot

The server already pinged every 15 s and dropped after two unanswered. Aligned with the branch: **any message
counts as being there** (also one that is not JSON), `heardAt` is kept per connection, and
`PhoneHost::heardAt / ping / drop` let the deck ask. `deskInit` while another desk holds the slot: heard from within
`deskSilentMs` -> refused at once, nobody pinged; else that desk is pinged and the caller's answer WAITS — it
answers (a pong or any message) -> refused, it keeps the slot; `deskProbeMs` pass (or it vanishes) -> it is
terminated (no close handshake) and the caller is registered. An asker that leaves while it waits takes nothing.
With no `PhoneHost` that knows who was heard from (the deck alone in a scenario) a connected desk always keeps
the slot. Checks: the branch's `test/desk/heartbeat-unit.js` cases on the fake listener and clock (`server`
section), and for real on loopback in `serve-client.js` (a raw socket registers as the desk and goes deaf: the
next desk has the slot ~3 s after asking, 10.5 s later).

### What the SHELL must do to match (`windows/src/**`, `windows/web/**`: not edited from here)

1. **Nothing to turn the remote-desk rule on**: it is the core's default and `buildDeckOptions` hands it on. In the shell's
   OWN windows nothing changes except that a remote desk no longer shows in a tile's `inWindow` / `shownBy`.
2. **In REMOTE mode (the exe as a desk)**: treat `inWindow` as always null from a deck with the remote-desk rule (it is);
   never wait for `layout:evict` from the office; a click shows any session without "taking" it; ignore
   `settings.oneWindow` for its own panes (the branch's renderer does: "in a desk `oneWindow()` ignores the
   setting"); `leaving` need not be sent (harmless if it is).
3. **`deskInit({ name: <computer name>, kind: 'native' })`** — `kind` is what `deck-state.json` and
   `meta.remoteDesk.kind` say. And expect its answer up to 3 s late when it is replacing a dead desk.
4. **Answer pings** (WinHTTP does) — a remote exe that neither pongs nor talks for ~45 s is dropped, and one
   silent for 10 s loses the desk slot to the next desk that asks.
5. **The claim, when it goes on** (`driverClaim`): `ui.hpp` `newDriverWords` already recognises a deck with it
   (`sizeBy`, `remoteDesk`, `office` / `remote`). Then: the button sends `setDriver(<its kind>)` / `'auto'`; a view
   that becomes the driver sends `resize` for each tile it shows when that tile's `mirror` goes from set to null,
   and again (2.5 s apart at most) while `sizeBy` is its kind, `mirror` is null and `ptySize` is not what it
   draws; mirror at `mirror` and send no size otherwise; show `input:refused` reasons `remote` /
   `remote-showing` beside `phone` / `phone-attached`. Local mode is kind `office`, remote mode kind `remote`.
6. **The wheel**: already the shell's (`frames.hpp` `wheel()`); the native deck passes it through.
7. **`phoneStatus.assets`**: show "the pages are not beside the exe (`renderer\phone\index.html`)" in Settings →
   Phone when it is `'missing'` (`assetRoot` names the folder that was tried, `""` = none was found).
8. **The version**: see the next heading.

### One source for the version (PACKAGING.md request 2)

`core/deck/version.hpp` is the one place: `CLAUDE_DECK_NATIVE_VERSION` (`"0.1.0"`),
`CLAUDE_DECK_NATIVE_VERSION_COMMAS` (`0,1,0,0`), `CLAUDE_DECK_NATIVE_VERSION_LABEL` (`"0.1.0-native"`), and for
C++ `core::kNativeDeckNumber` / `core::kNativeDeckVersion`. `buildDeckOptions` sets `DeckOptions::version` from
it when the shell names none. **The one-line request to the shell / resource side:** in `windows/res/app.rc`
`#include "../../core/deck/version.hpp"` and write `FILEVERSION CLAUDE_DECK_NATIVE_VERSION_COMMAS`,
`PRODUCTVERSION CLAUDE_DECK_NATIVE_VERSION_COMMAS`, `VALUE "FileVersion", CLAUDE_DECK_NATIVE_VERSION`,
`VALUE "ProductVersion", CLAUDE_DECK_NATIVE_VERSION_LABEL` (the header hides its C++ behind `RC_INVOKED`), and in
`windows/src/app.cpp` drop `kDeckVersion = "0.1.0-native"` for `deck::core::kNativeDeckVersion` (or pass no
version: the builder fills it in). Today all three say 0.1.0 by hand, so nothing visible changes.

### "Say so when the pages are not found" (PACKAGING.md request 3)

`phoneStatus()` (the `getSettings` / `saveSettings` answers, `LocalDeck::serveStatus()`) now carries
**`assets: 'ok' | 'missing'`** — are `renderer\phone\index.html` and `renderer\index.html` under the asset root —
and **`assetRoot`**: the folder that was tried (`""` when the lookup found none). `error` is left as it was (it is
the bind's). Not in main.js's twelve fields; a client that does not know them ignores them.

### Built and linked privately

`windows/build.ps1` writes `windows\build\ClaudeDeckNative.exe`, which the other worker's build owns tonight, so
the app was linked from this tree into `%TEMP%\cdn-private-app\ClaudeDeckNative.exe` with a copy of that script
(objects in `%TEMP%\cdn-private-app-obj`): it compiles and links against the changed core as the tree stood at
01:00, and `serve.ps1 -Exe <that exe>` ran parts 4 and 5 with it. `windows\build\` was not touched.

## The server (`--serve`, and Settings → Phone)

The native deck is now the thing a phone or a remote desk connects to. It follows `settings.json` `phone` exactly as
the Electron deck does (`syncPhone`):

| `phone.bind` | binds | when it cannot |
|---|---|---|
| `tailscale` (the default) | this machine's address in 100.64.0.0/10 (`tailscaleAddress()`) | `no Tailscale address on this machine (is Tailscale running?)` — `phoneStatus.error`, nothing listens |
| `localhost` | 127.0.0.1, for `tailscale serve` in front; the link shown is the tailnet HTTPS front door read off `tailscale serve status --json` (run hidden, `CREATE_NO_WINDOW`, at most once a minute), or the loopback link when Serve is not set up | — |
| `all` | 0.0.0.0 — only when the setting says so | — |

The token is generated on first run, kept in `settings.json` (`phone.token`), never logged; "new token" replaces it
and restarts the server; it is compared in constant time, from `?t=` or the `deck_phone` cookie, and a query hit
sets the same cookie lib/phone.js sets. `phoneStatus()` has main.js's twelve fields, so Settings → Phone works
against the native deck — plus `assets` and `assetRoot` (above: "Say so when the pages are not found").

**`--serve <port>`** (`LocalDeckOptions::servePort`) is the test and local-client form: for THIS run the server is
on, on **127.0.0.1:`<port>`**, whatever the settings say; nothing is written to them, and Tailscale is asked nothing.

**How the bind behaviour was checked without binding a real interface:** the choice is a pure function
(`bindHost`, `tailscaleAddress`, `serveUrlFromStatus` in `core/deck/server.hpp`), tested against fixtures generated
from `lib/phone.js` itself with `os.networkInterfaces` and the `tailscale` exec replaced by fakes; and
`Server::sync` is tested with a fake `Listener` that records the host and port it was asked to bind — tailscale
with and without a Tailscale address, localhost with and without Serve, all, a taken port, a new token, off. Every
REAL bind in every test is 127.0.0.1 on 8811–8819; `serve.ps1` also asserts that 8811 listens on 127.0.0.1 and on
nothing else.

Routes (each checked against what the REAL `lib/phone.js` answered to the same request — status, content type,
cache policy, cookie, CSP, and the body byte for byte; `core/test/fixtures/deck/routes.json`):
`/` and the phone page's files, `/vendor/xterm.js` `xterm.css` `addon-fit.js`, `/desk/…` (the desktop renderer with
`<base href="/desk/renderer/">` and the bridge added as it is served, only the files the page needs),
`/manifest.webmanifest` + the two icons public, `POST /upload` (saved by the deck under `<state>\uploads`), and the
websocket upgrade on any path (401 without the token). **`/ask/<id>.html`** serves the page file of an ask some
session's ask-page state names (sandboxed, the same CSP), and **`/view/<page key>/<folder id>/…`** a file from a folder
the deck was asked to show (`lib/viewer.js`: the extension whitelist, no dotfiles, the sandbox CSP, the page-links
script added to an .html) — both checked against the real `lib/phone.js` with the real `Viewer` and `AskFeed` behind
it (`core/test/fixtures/deck/routes2.json`, 45 requests: status, six headers, the body byte for byte).

**The pages come from an asset root** — the folder holding `renderer/`, `lib/layout.js`, `assets/fonts/` and
`node_modules/@xterm/`: `--assets <folder>`, else the first of the exe's own folder and the five above it that has
`renderer\phone\index.html` (a checkout: `windows\build\ClaudeDeckNative.exe` is two below the repo root). **A
packaged exe** would ship those files beside itself — `renderer\` (index.html, style.css, js\, desk\bridge.js,
phone\), `lib\layout.js`, `assets\fonts\`, and the three xterm files under `node_modules\@xterm\…` (about 1.6 MB
without the fonts) — and find them by the same lookup; nothing is embedded in the exe tonight.

### The real tests (`windows/host-test/serve.ps1`)

1. `deck-serve-host.exe` (`serve_host.cpp`: the same `LocalDeck`, no window) on 127.0.0.1:8811, state in a temp folder;
2. `serve-client.js` — a Node phone and desk: the pages, token and cookie, 401s, the hello, a shell tile, typing,
   the phone owning the size, the desk's panes under the remote-desk rule (its layout kept, no window its holder, not "on
   screen"), the one-desk rule, **the desk slot** (a raw socket registers as the desk and goes deaf: the next desk
   is refused at first and has the slot ~3 s after asking 10.5 s later), **a slow client** (a socket that stops reading while 16 MB goes
   by: it is sent less, stays connected, still gets meta, and its next attach heals the screen), **the heartbeat**
   (a socket that never answers a ping gets two and is dropped at the third beat, ~45 s);
3. `serve-browser.js` — the REAL phone page and the REAL browser desk in a headless Chromium: both load, list the
   tile and draw its terminal, with no script error and no failed request;
4. a second `ClaudeDeckNative.exe` in REMOTE mode (`CLAUDE_DECK_LINK` in the environment, `--shot`) as a desk:
   sidebar, pane, typing;
5. `ClaudeDeckNative.exe --local --serve 8812` — the app itself serving while it has its own window.

Shots (in `windows/host-test/out/`): `serve-native-desk.png`, `serve-browser-desk.png`, `serve-phone-list.png`,
`serve-phone-tile.png`, `serve-app-local.png`.

## The two real runs (the owner's word)

Two things in this deck need the owner's own word before they touch the real machine. **The Claude run has been made**
(the owner's word, 2026-11-18: "The C++ deck may start Claude sessions to test (a few throwaway ones, one account, temp
folder, close them after)") — see "The real Claude runs" below. **The usage run has not.**

| | What it does | Its gate | Run it |
|---|---|---|---|
| **Claude, for real** — **run, passed 15 / 15** | ONE Claude session on one account, in a fresh temp folder with no `.mcp.json` above it, with the deck's state in a temp folder: starts hidden under ConPTY, the startup / trust prompt answered as main.js answers it, `starting` → `idle` with its prompt on screen, types exactly `say the single word ok` (as the deck's delivery types: paste, then Enter), sleep (the process ends), wake (`--resume`), close, and asserts no process is left and no window was ever shown. It spends that account's tokens on one line; the transcript stays in the account's config dir | `-Only claude-real` **and** `DECK_HOST_TEST_REAL_CLAUDE=<account keyword>` | `$env:DECK_HOST_TEST_REAL_CLAUDE='claudets'; $env:DECK_HOST_TEST_REAL_LOG="$PWD\windows\host-test\out\claude-real.log"; powershell -File windows/host-test/build.ps1 -Only claude-real` |
| **Usage, for real** | ONE request to Anthropic's usage endpoint for one account, with the token in that config dir's `.credentials.json` — read, never written, never printed; the cache goes to a temp folder and is checked to hold no part of the token. A 429 reads `rate-limited`: stop, do not ask again | `-Only usage-real` **and** `DECK_HOST_TEST_REAL_USAGE=<config dir>` | `$env:DECK_HOST_TEST_REAL_USAGE="$HOME\.claude-thirdacct"; powershell -File windows/host-test/build.ps1 -Only usage-real` |

Every step either run takes is covered on fakes (the launch, the dialogs, sleep / wake; the request's URL and three
headers, the cadence, the cache, 401 / 429 / an expired token). What only a real run can show: that Claude Code's
dialogs today still read as the recorded ones, and that WinHTTP reaches the endpoint over TLS.

`usagePolling` is what a shell sets after its run passes. `allowClaude` is on for a real local deck since the
Claude run passed ("The Claude seam" above has the rule).

### The real Claude runs (2026-11-18, account `claudets`, Claude Code 2.1.328)

| Run | Result | What it showed |
|---|---|---|
| 1 (23:13, the coordinating session's) | 6 passed, 4 failed | **Nothing in the deck was wrong.** The session's folder was under `%TEMP%`, which is under the home folder, which has a `.mcp.json`: Claude Code reads `.mcp.json` from every folder above the session's and put its "New MCP server found" dialog up. The tile read `waiting` (`startup: MCP server prompt`) — as it does in the Electron deck; that dialog is a person's — so `starting -> idle` failed at its two minutes; the test then typed its line anyway, **into the dialog** (the Enter chose an option: the folder got a `.claude/settings.local.json` with `disabledMcpjsonServers: ["slack"]`); no turn ran, so no transcript, so no model / last message, and the wake rightly used `--session-id` (not `--resume`), which that check counted as a failure |
| 2 (23:30) | 14 of 15 | folder under `%PUBLIC%` (no `.mcp.json` above it). Trust prompt answered in 4.4 s (Down, Enter), idle at 5.3 s, prompt on screen, the line typed as the deck's own delivery types (bracketed paste, Enter 150 ms later), busy 1.2 s after the Enter, idle 8.5 s later, transcript found (model `claude-opus-5-5`, last message `ok`, title, effort, context), sleep (three Ctrl+C, gone in 1.6 s, its record removed by Claude Code itself), wake with `--resume` (idle after 6.5 s, the conversation and the prompt on screen), close, no process left, no window at any point. The one failure was the test's own new check (it looked for the typed line on the woken screen; the owner's session-start hooks print far more than 30 rows, so it had scrolled off) |
| 3 (23:36) | **15 of 15** | the same, with that check looking for the conversation's last message instead |

Sessions made (their transcripts stay in the account's config dir, for the owner to delete):
`00000000-0000-4000-8000-000000000001` (run 1: no transcript was ever written),
`00000000-0000-4000-8000-000000000002` (`projects\C--Users-Public-deck-host-test-26820-claude-real-work\`),
`00000000-0000-4000-8000-000000000003` (`projects\C--Users-Public-deck-host-test-720-claude-real-work\`). The
account's `.claude.json` also keeps a project entry for each of the three folders (the trust answer).

What the runs taught, all of it main.js's behaviour too (pinned on fakes: `core/test/deck` "real run 1 / 2"):

- **A folder below one with a `.mcp.json` starts on a dialog.** The tile reads `waiting`; nothing of the deck's
  types into it (a special prompt is held: "the session is waiting for you"); a window's own keys go to the
  process as typed. The gated test now makes its folder under the first of `%TEMP%`, `%PUBLIC%`, `%ProgramData%`
  with no `.mcp.json` above it, and **never types unless the session is idle AND its prompt is on the parsed screen
  with no dialog over it**.
- **`idle` is not "the prompt is up".** Claude Code writes `sessions\<pid>.json` 0.5–5 s after it starts, at first
  with no `status` at all, which reads `idle` (`lib/sessions.js`: `status || 'idle'`); the prompt is drawn a
  second or more later. The deck's own delivery (`lib/queue.js`) also wants the prompt marker in the recent
  output and presses Enter again if the session is not busy 3 s on; anything else that types should look too.
- **Typing.** Plain bytes through `writeTile`; a bracketed paste (`ESC [200~ … ESC [201~`) then `\r` is taken as
  one submitted line under ConPTY. No keyboard-protocol translation is needed.
- **The record's pid is the pty's own pid** (`claude.exe` is the process the deck started, not a wrapper), and the
  match is by session id, as in main.js. Claude Code removes the record itself on a Ctrl+C exit.
- **The transcript** is found by session id under whatever folder name Claude Code gave the project
  (`C--Users-Public-…`); the deck never builds that name.
- **The trust answer leaves the pty three columns narrow** (119, 118, 117: one nudge per cycle, as main.js). The
  meta says so (`ptySize`); the view that shows the tile must size it back, as the Electron window's fit does.
- **Any input to a sleeping tile wakes it — a view's terminal replies included** (main.js `writeTile`), and a
  `wakeTile` after that answers `false`. A shell that feeds a sleeping tile's last output to its terminal should
  not send that terminal's replies back, or the tile wakes by itself.
- **A just-woken session may not exit on Ctrl+C inside the 2.5 s grace** (run 2 and 3: closed 0.5 s after the
  wake, hard-killed at the grace), as main.js; Claude Code's next start of that session then falls back to its
  classic renderer for one run.
- The owner's session-start hooks ran in each test session and printed their nags into it: the run logs
  (`windows/host-test/out/claude-real-run*.log`, git-ignored) hold that text on screen dumps.

The gated test prints, with a time stamp on each line: the launch (`file`, `args`, `cwd`, the NAMES of the
environment's keys), every byte the deck writes to the process (`LocalDeckOptions::ptyTrace`), every status change
with the session's own record, the record file names, the transcript's path and size, the tile's whole meta and
the parsed screen at each stage and on every failure. `DECK_HOST_TEST_REAL_LOG=<file>` sends all of it to a file.

## The third build (2026-11-19): asks, the viewer, the on-call's desks, cloud sessions

| Module | In the core | Checked by |
|---|---|---|
| `lib/asks.js` | `core/deck/asks.{hpp,cpp}`: `AskFeed` (the scan of the skill's state folder, rounds, liveness, pruning, drafts moved onto a replacing page, DRAFT copies, the log, `deck.json`), `composeDone` / `composePointer`, `newAskRows`, `pickerOnScreen`, `locations`, `pathToFileURL`. Wiring (`deck_parity.cpp`): `doneAsk`, `notifyNewAsks`, the 4 s scan and 30 s heartbeat, `meta.asks` / `askLog`, the five calls, `/ask/<id>.html` | 6554 checks from 243 scripted steps of the REAL class (`gen-asks.js`; `deck.json` and the .txt copies byte for byte); the deck scenarios on fake seams (`parity`); real files in the host test |
| `lib/viewer.js` | `core/deck/viewer.{hpp,cpp}`: `PATH_RE` by hand, `resolvePath`, `htmlFile`, `Viewer` (ids, the LRU of 300 folders, `fileFor`), `sessionHtml`, `VIEW_CSP` / `VIEW_TYPES` / `withPageLinks`. Wiring: `viewHtml`, `sessionHtml` (cached 30 s per transcript), `meta.settings.pages`, the `/view/…` route | 8222 checks (`gen-viewer.js`: real fs, a synthetic win32 fs, a synthetic posix fs); the route replay; the deck scenarios |
| `lib/deskopen.js` | `core/deck/deskopen.{hpp,cpp}`: `pickSpot`, `checkRequest`, `accountFor`, `openPlan`, `placedWords`, `DeskOpenInbox` (take by rename, the ack written whole, `log.jsonl`, the day's prune). Wiring: `openDeskRequest`, `placeDeskTile`, `notifyDeskOpened`, `desk` / `attention` on the tile, `layout:place` | 3717 checks (`gen-deskopen.js`: the real inbox scripted in a temp folder); the deck scenarios; a real request file taken and answered in the host test |
| `lib/cloud.js` | `core/deck/cloud.{hpp,cpp}`: ids, `parseCreated`, `view`, `CloudPoller` on the `Http` / `Credentials` seams. Wiring: cloud tiles (made, held, saved, restored), `cloudCreated`, `cloudMeta`, Elsewhere, `openCloud`, `openExternal`, the toast's cloud mark | 1634 checks (`gen-cloud.js`: the real poller on a fake `fetch` and made-up tokens); the deck scenarios on a fake process and a fake Http. **No cloud session was made and no Anthropic endpoint was called** |

### The ask feed: where it reads and writes, and the two-decks warning

The feed is ON only when a shell names the skill's state folder (`DeckOptions::askStateDir`); `LocalDeck` decides
(`shippingAsks`, tested):

| The process | The ask feed |
|---|---|
| `ClaudeDeckNative.exe` on its own state folder (no `--state-dir`) | **on**, the REAL folders, resolved as `lib/asks.js locations()` does: `ASK_PAGE_STATE_DIR` / `ASK_PAGE_DOWNLOADS`, else the skill's own `settings.json` (`~/.claude`, `.claude-thirdacct`, `.claude-demo`, `.claude-vv`: the first that parses), else `~/.local/state/ask-pages` and `D:/Downloads`, `~/Downloads` |
| `--state-dir <folder>` (every test of the app), any other exe | **off** — unless `ASK_PAGE_STATE_DIR` is set in the environment (then that folder). Stricter than Electron on purpose: a scratch Electron deck uses the real folder |
| `--no-asks` / `--asks` | off / on, wherever; the last wins |

What it touches: it READS `<state dir>/<session id>.json` and never writes one; it WRITES `<state dir>/deck.json`
(at start, on every ask-settings change, every 30 s; removed at shutdown when the pid in it is its own),
`<page folder>/<page id>.answers.json` on Done (the first Downloads folder that exists when the page's folder is
gone), `<page folder>/<page id>.drafts-<HHMMSS>.json` + `.txt` when a page with unsent entries stops waiting, and its
own `<deck state>/ask-drafts.json`.

**Two decks, one skill folder: THE GUARD (in the core, `AskFeed::writeDeckFile`; not in lib/asks.js).** The Electron
deck just writes `deck.json` every 30 s. The native deck reads the file before EVERY write of its own — the first
one at start, each 30 s beat, each ask-settings change — and when it carries **another deck's fresh heartbeat** (a
`pid` that is not this process's, an `at` less than 90 s = 3 beats old) the native deck is a **GUEST**:

| A guest | |
|---|---|
| `deck.json` | never written, never removed (at shutdown it removes the file only when the pid in it is its own) |
| the shared folders | nothing: no pruning, no draft migration, no DRAFT copies, no answers file, no log entries |
| toasts for a new ask | none (the serving deck raises them) |
| `meta.asks` / `meta.askLog` | still filled, read-only: the waiting pages show |
| `call askDone` / `askDraft` / `askClear` | `err`: `another deck is serving the ask pages` (`askDraftGet` / `askLogState` still answer) |
| **`meta.askFeed`** | `{ "state": "guest", "reason": "another deck is serving the ask pages" }` |

`meta.askFeed.state` is `"writer"` (`reason` null) when this deck serves the pages and `"off"` when it has no feed:
the field for the shell to show. The guest re-checks on each 30 s beat and **takes over only when the other
heartbeat is 90 s stale or the file is gone** (the Electron deck removes it on a clean quit: takeover at the next
beat; after a crash: 90–120 s). A native deck that IS the writer and then finds a foreign fresh write (the Electron
deck was started second) **steps back to guest at its next beat** instead of fighting — up to 30 s in which the
last writer's settings stand. So `ClaudeDeckNative.exe --local` beside the live Electron deck cannot flip the ask
threshold or the picker mode: it never writes while Electron's heartbeat is alive. The identity is the `pid` field
lib/asks.js already writes; nothing was added to the file. `--no-asks` still turns the feed off outright.
Scenarios on a fake clock (`core/test/deck/deck_parity_test.cpp` `testAskGuard`): guest at start through two
minutes of foreign beats with the file byte-identical throughout; takeover after 90 s; step-back; the file removed.

### The on-call's desks

`<deck state>\desk-open` is read every 1.5 s (polled; main.js also watches). A request is refused in the deck's own
words when `settings.deskOpen` is off, no window is open, the account or folder is unknown — and, here only, when
`allowClaude` is off (before any tile is made: the on-call then starts its desk headless, as with no deck). A new
desk takes the first free cell in reading order of the main window, then of each auxiliary window; with none free
it asks the shell for a new window (`Lifecycle::openWindow(client, bounds, inactive)`: 80 % of the main window,
offset 10 %) and, **when the shell cannot open one (today's), leaves the tile in the main window's sidebar** and
answers `window: <main>, primary: true, cells: [], newWindow: false`. A native deck and the Electron deck have
separate inbox folders (each its own state folder): the on-call writes to whichever deck's folder it is pointed at.

### Cloud sessions

Three switches, all OFF, all the owner's: `cloudPolling` (reads each account's token and asks
`https://api.anthropic.com/v1/code/sessions…`, as usage polling does — never run for real), `allowCloudCreate`
(`claude --cloud "<task>"` may be started; needs `allowClaude` too — never run for real), and the pane itself: a
cloud tile's view is its `claude.ai` page in a web view, which is the shell's (WebView2) and not built. With all
three off a cloud tile restored from a `tiles.json` or opened by `adoptSession({ cloudId })` is held, saved and
shown as a tile with `cloudId` / `cloudUrl` and status `idle`; `meta.cloudOption` is true only when the deck may
make one.

## Plugging it into `windows/src/app.cpp` — every edit made (checked against the code 2026-11-19)

The list this section used to hold (edits 1-9 for `app.cpp`, the exact edits for `deckhost.hpp`) is done. Each was
checked in the code, not taken from a report:

| Edit | What | Where it is now | Checked by |
|---|---|---|---|
| 1 | `--serve` / `--assets` as options of the app | `app.cpp` `Options` + the argument loop; `localtransport.hpp` is no longer included by the app | **shell.ps1** `phone` |
| 1b | keys in the window for `deck-state.json`; `deck.version` | `App::noteKey` → `Deck::windowKey` on any key down; `DeckHost::Extra::version` | **shell.ps1** `state` |
| 2 | toasts and the taskbar flash | `notify.hpp` (`ShellNotifier`: WinRT toasts, `FlashWindowEx`), handed to the core as its `Notifier` | **shell.ps1** `notify` (recorded; no test shows a toast) |
| 3 | on screen / focused | `App` → `Deck::windowState` from the window's own messages | **shell.ps1** `state` |
| 4 | more windows (F5) | `DeckHost::openWindow` / `removeWindow`; a tile dragged out, "Open in a new window"; in remote mode too (b443ab9) | **shell.ps1** `windows`, `realwin`; `windows/test/remote-window.txt` |
| 5 | restart when quiet | `lifecycle.hpp` `ShellLifecycle`: `restart` (relaunch, `--wait-pid`, the lock), `fingerprint` | **shell.ps1** `restart`, `update` |
| 6 | Claude on by default; the trust-prompt resize; no replies for a sleeping tile | `st.claudeOff = !lo.allowClaude`; the view's resize and reply rules | **shell.ps1** `claude`, `tiles` |
| 7 | what `localPreview` hid | gone: Elsewhere, Past, special prompts, triggers, Restart when quiet answer in local mode | **shell.ps1** `undim` |
| 8 | the 2026-11-19 options (`notifier`, `lifecycle`, `asks`, `exitWatchdog`, the window's client) | `DeckHost::Extra`, `Lifecycle::openWindow` → `DeckHost::adoptWindow`, `Deck::toastClicked` | **shell.ps1** `corewindow`, `notify`, `askfeed` |
| 9 | the ask pane's data live; `layout:place` | `asks.hpp` over the core's calls; `app.cpp` `layout:place` | **shell.ps1** `askfeed`; `windows/web-test` |
| deckhost | `DeckHost::create()` on the one options builder | done, then taken further: `DeckHost` is now a `core::Host` (`core/deck/hosting.hpp`) built from `core::buildDeckOptions` (`core/deck/options.hpp`) — the inbox, the pump and the routing are the core's, shared with the Mac shell | everything that runs the exe |

`ShellLifecycle` overrides `armExitWatchdog`, `openExternal`, `openPath`, `showInFolder` and `openWindow` as the core
declares them. A clicked toast goes through `Deck::toastClicked`, which names the window and has already sent it
`tiles:activate` or `asks:open`. An older ask page's own Done (a download) saves its answers file in LOCAL mode
(`DeckHost::askDownloads()`, 727f9b8).

## What still stands between the native deck and switching Electron off (main process)

Every main-process responsibility of `main.js` and `lib/*.js`, with its state in the native deck (the core AND the
Windows shell) on 2026-11-19. **done** = written and tested (on fakes or headless unless it says real); **partial** =
done but for something named; **missing** = not started. **[OWNER]** marks what needs the owner's word.

**Headline (re-counted 2026-11-19 against the code, after the shell's work of that day): 88 rows — 78 done,
9 partial, 1 missing — and 12 more responsibilities that simply vanish with Electron.** (Before: 84 rows counted,
59 / 21 / 4; the four lettered rows 72a and 85a–c are counted now.) Every one of the 9 partial rows waits for the
owner's word, not for code: 8, 18, 32, 42, 49, 58, 80, 84, 85b. The 1 missing row (33, the macOS Keychain read) is
the Swift shell's.

**What still stands between the native deck and switching Electron off — the honest answer, in three groups:**

**(a) Needs a human hand** (written and tested headless; only a person at the screen can see it work):
- a real toast, once: the Start-menu shortcut in the real Start menu, a waiting session toasting while the window is
  not focused, a click raising the window on the tile (rows 47, 51–54, 83; `PARITY.md` "Switching Electron off" 1);
- the paste and scroll-up blockers against a real Claude Code session in a REMOTE pane, and copy on select, the
  click-through and the selection that follows its text (`PARITY.md` "Blockers"; the owner's hand test on the home PC);
- Claude sessions watched in LOCAL mode from this shell (the account buttons, Move, adopt, special prompts, sleep /
  wake), and Elsewhere / Past "Bring it here" (it takes a live session);
- a real restart on the real folder, and a sign-in start after a real install (rows 58, 84);
- the ask feed on the real skill folder with a real session asking (row 46 is a guest beside the Electron deck);
- the tooltips and the out-of-window drag picture on a real screen.

**(b) Needs the owner's decision:**
- account moves through userswitch (row 8: it copies transcripts between their accounts) and "Move every session";
- usage polling (32: it reads each account's sign-in token), cloud polling and making cloud sessions (18, 42, 49);
- the account editor writing the alias file userswitch reads (80); keeping shell tiles across restarts
  (`persistShells`, difference 1);
- whether restarts go through a launcher of theirs (`relaunch.json`, 58) and pointing their keep-alive task at the native
  deck's `running.json` (84);
- the driver as a claim (85b: a proposal, off) and merging the review branch `native-remote-server` (the remote-desk rule is
  already the default here: 61, 62, 85a);
- merging this branch, `native-windows` (the sharing refactor), into `main`;
- code signing beyond the GPG-signed releases (an Authenticode certificate in their name: a cost);
- the switch itself (`PACKAGING.md` "For the owner": close the Electron deck, install, import once).

**(c) Real work left** (one line each, with an estimate of the work alone):
- the Swift shell: build it on a Mac and adopt the shared core (the edit list in `core/deck/README.md` "For the
  Swift shell" and the branch's report) — 1–2 days on a Mac, most of it the pty and the first build;
- the macOS Keychain read for usage polling (row 33, the Swift shell's `Credentials`) — half a day, after the owner
  says yes to usage polling;
- the pane name's ask mark number in the console font (`PARITY.md` partial; cosmetic) — 1 hour;
- the special-prompt form's resize grip on the prompt box and the calendar's time columns — 2–3 hours;
- the colour picker's eyedropper and RGB / HSL formats (Chromium's popup has them) — half a day, optional;
- the palette's look measured against the page (a shot compared with the Electron one) — 1 hour;
- a `Shell_NotifyIcon` balloon as the fallback, only if group (a) finds WinRT toasts refused — 2 hours;
- fonts embedded in the exe (3 MB, DirectWrite's in-memory loader) — half a day (assessed, not started);
- WebSocket compression for the remote (`perMessageDeflate` on the office deck, an inflate in the exe) — 1 day, and the
  server half is a change to the Electron deck's `lib/phone.js` (the owner's merge).
(The rows are numbered 1–85; there is no row 19.)

### Sessions and spawn — 16 done, 2 partial

| | Responsibility (main.js / lib) | Status |
|---|---|---|
| 1 | the spawn queue: one spawn per 400 ms, never synchronous (`queueSpawn`) | done |
| 2 | terminal tiles (`spawnTile` shell branch, `platform.shellCommand`) | done — real (ConPTY) |
| 3 | the Claude launch (`--resume` / `--session-id`, browser, model + prompt, env, `CLAUDE_ONCALL_KIND`) | done — **real** (2026-11-18: start, a turn, sleep, wake with `--resume`, close), **on** in a real local deck. `--chrome`, `--model` + first message and `CLAUDE_ONCALL_KIND` are on fakes only |
| 4 | the trust prompt answerer (`trustWatch`) | done — real (a new folder's prompt answered in 4 s, twice) |
| 5 | startup dialogs: the Settings Warning Continue, `waiting` on a dialog, its toast (`lib/startupprompts.js`) | done (fixtures from the recorded dialogs) |
| 6 | kill: Ctrl+C ×3, the 2.5 s grace, the tree kill (`killPty`, `platform.killTree`) | done — real for shells |
| 7 | close / restart / refresh (`browser` kept across starts) | done |
| 8 | account moves through userswitch (`moveTile`, `usw --start`) | partial — the command and environment are built and tested; needs `allowClaude`, Git Bash and `userswitch` on the box; never run. **[OWNER]** (it copies transcripts between their accounts) |
| 9 | move every session of an account, `continue` after a usage notice (`moveAll`, `sayContinueWhenReady`) | done (fakes) |
| 10 | a Claude session started by hand in a shell tile (`lib/shellhost.js`, `scanShellHosted`) | done — the process tree and console hosts are read natively (no hidden PowerShell); never seen a real one |
| 11 | account discovery from the alias file, emails and context windows (`lib/accounts.js`, `refreshAccounts`) | done — Windows, and darwin on the POSIX twin of the fakes (2026-11-19: that run found the account folders normalised with Windows rules on every platform, fixed in `rules.cpp`); a Mac's extra alias files (`.zshrc`, `.bashrc`) are read by `platform::aliasFiles`, the editing of them is the Swift shell's |
| 12 | the spawn environment (`baseEnv`, `envFor`) | done |
| 13 | the boot restore: staggered, guarded saves, sleeping tiles restored asleep, the 2 s grace | done |
| 14 | `tiles.json`: atomic writes, backups, unreadable-file recovery (`lib/statefile.js`) | done |
| 15 | `killAll` when the last window closes | done (`shutdown`) |
| 16 | `running.json` for the keep-alive supervisor (`markRunning`) | done — `{ pid, at }` at start, removed on every deliberate quit (`shutdown`), left behind by a crash |
| 17 | the on-call's desks as tiles: `desk` kept on the tile, `--model` + first message on one start | done — opened by the desk-open inbox (row 50); `CLAUDE_ONCALL_KIND`, the model and the message last, on that start only |
| 18 | cloud tiles (`claude --cloud`, `cloudCreated`, a tile that runs nothing here) | partial — done on the fake process seam (the run, the link read off its output, the tile becoming the session, saved, restored, retry). **[OWNER]** `allowCloudCreate` (off; never run for real) and the pane (a claude.ai web view: the shell's) |

### Hibernation and the memory guard — 5 done

| | Responsibility | Status |
|---|---|---|
| 20 | sleep / wake, `asleep` as a status, frozen screen kept, wake through the spawn queue | done |
| 21 | the idle policy every 30 s (`lib/hibernate.js`: pinned, on screen, holding a prompt, background work, never-used sessions) | done |
| 22 | the memory guard at 90 % commit, pressure mode's hysteresis | done — the commit charge is read natively (`GlobalMemoryStatusEx`) |
| 23 | waking what a window shows when it comes back on screen | done — the shell reports minimise / restore / focus (`App` → `Deck::windowState`; **shell.ps1** `state`) |
| 24 | restored-asleep tiles at boot | done |

### Special prompts and triggers — 5 done

| | Responsibility | Status |
|---|---|---|
| 25 | the four kinds, the due rule, the file (`lib/queue.js`) | done (scripted runs of the real class as fixtures) |
| 26 | delivery: paste, Enter, the busy confirmation, the notes | done |
| 27 | the trigger store and its file (`lib/triggers.js`) | done |
| 28 | the trigger inbox (the deck-triggers skill drops a file) | done — polled every 4 s; main.js also watches the folder, so a drop can land up to 4 s later here |
| 29 | recurring prompts' clock windows in local time | done |

### Usage polling — 2 done, 1 partial, 1 missing

| | Responsibility | Status |
|---|---|---|
| 30 | the poller: once per account per 3 min, sooner when the credentials change, the cache, 401, 429 + back-off (`lib/usage.js`) | done (the real class on a fake `fetch` as fixtures) |
| 31 | `refreshUsage`, `meta.usage` | done |
| 32 | reading the token and asking the endpoint on Windows (`platform.readOAuth`, `fetch`) | partial — `WinCredentials` + `WinHttpGet` are written and tested on a made-up file and this test's own loopback listener; **switched off**. **[OWNER]** `usagePolling`; never run against the real endpoint |
| 33 | the macOS Keychain read | missing (the Swift shell's `Credentials`) |

### Transcripts and live state — 5 done

| | Responsibility | Status |
|---|---|---|
| 34 | the transcript tail: title, last text, model, effort, context, ultracode, background work (`lib/transcript.js`) | done (first build) |
| 35 | Claude Code's own session records (`lib/sessions.js`) | done (first build) |
| 36 | `collectMeta`: statuses, stamps, the snapshot's order | done; per-window `shownBy` / `inWindow` and the remote's mirror added |
| 37 | the phone's text view (`lib/transcript-page.js`) | done |
| 38 | is there a transcript to resume (`isResumable`, `findTranscript`) | done |

### Elsewhere / Past / adopt — 3 done, 1 partial

| | Responsibility | Status |
|---|---|---|
| 39 | sessions running elsewhere on the box (`externalSessions`) | done (local sessions) |
| 40 | past sessions on disk (`pastSessions`, `quickTitle`, `quickCwd`) | done |
| 41 | adopt: end it there, resume it here (`adoptSession`) | done, refused while `allowClaude` is off — it ends another process. Covered by **[OWNER]** `allowClaude` |
| 42 | cloud sessions in Elsewhere, `openCloud` | partial — done (`externalSessions` rows, `adoptSession({ cloudId })`); the list itself needs cloud polling. **[OWNER]** `cloudPolling` |

### Ask pages and deck.json — 6 done

| | Responsibility | Status |
|---|---|---|
| 43 | the ask feed: the skill's state folder, rounds, liveness, pruning (`lib/asks.js` `AskFeed`) | done — the real class's scripted runs as fixtures; polled every 4 s (main.js also watches: a new page can show up to 4 s later here) |
| 44 | drafts and the log (`askDraftGet` `askDraft` `askClear` `askLogState`) | done — drafts moved onto a replacing page, DRAFT copies, the 50-page log; `ask-drafts.json` in the deck's own folder |
| 45 | Done: the answers file, the one line typed through the special-prompt queue, Esc on a picker (`doneAsk`) | done (fakes; the answers file on real files) — the line dropped again when the session reads the file first |
| 46 | `deck.json` in the skill's folder (threshold, picker mode), removed on quit | done — on in a real local deck, off in every throwaway one. A second deck never fights the first: it is a guest while another deck's heartbeat is fresh ("THE GUARD") |
| 47 | the ask toast (`notifyNewAsks`), `asks:open` | done — decided and worded in the core; shown by the shell's `Notifier` (`notify.hpp`; recorded, never shown, in every test); a click goes through `Deck::toastClicked` to `asks:open` (**shell.ps1** `askfeed`, `notify`). A real toast on a screen: group (a) |
| 48 | serving an ask page (`/ask/<id>.html`, sandboxed) | done — against the real server's answers |

### Cloud sessions — 1 partial

| | Responsibility | Status |
|---|---|---|
| 49 | `lib/cloud.js` (the poller, cloud meta), the claude.ai pane and its guards | partial — the poller and `cloudMeta` are done on a fake Http with made-up tokens; **switched off**. **[OWNER]** `cloudPolling` (it reads the accounts' tokens); never run against the real endpoint. The pane is the shell's; its guards vanish with Electron |

### The desk-open inbox (the on-call's desks) — 1 done

| | Responsibility | Status |
|---|---|---|
| 50 | `lib/deskopen.js`: the request inbox, the plan, the first free cell, a new window, the pulse, the answer file | done — all of it in the core (a real request file taken and answered in the host test); a NEW auxiliary window through the shell's `Lifecycle::openWindow` → `DeckHost::adoptWindow` (**shell.ps1** `corewindow`); starting the desk's session follows `allowClaude` (on in a real run) |

### Toasts — 4 done

| | Responsibility | Status |
|---|---|---|
| 51 | waiting / done after a long turn, held back while a deck window has the focus (`maybeNotify`) | done — decided in the core, shown by the shell's WinRT `Notifier` (recorded in tests: **shell.ps1** `notify`); never on a real screen: group (a) |
| 52 | the settings-warning and memory toasts | done — the same `Notifier` (the memory toast: **shell.ps1** `corewindow`) |
| 53 | the taskbar flash | done — `FlashWindowEx` on the window holding the tile, not while it has the focus (**shell.ps1** `notify`) |
| 54 | a clicked toast brings the tile up (`tiles:activate`) | done — `Deck::toastClicked(toast)` names the window and sends `tiles:activate` / `asks:open`; the shell raises it (**shell.ps1** `notify`) |

### Restart when quiet, the update offer, relaunch — 5 done, 1 partial

| | Responsibility | Status |
|---|---|---|
| 55 | arm / cancel, the blockers, the minute of quiet, `meta.restart` (`lib/quietrestart.js`) | done |
| 56 | the fresh look before acting, never during the restore | done |
| 57 | the update offer and "Later" | done — the shell's `Lifecycle::fingerprint` (the exe's size and time: a rebuilt exe is offered; **shell.ps1** `update`) |
| 58 | the relaunch itself (`restartDeck`, `relaunch.json`, the box launcher) | partial — written and tested end to end in a temp folder: the core saves and asks the shell; the shell starts the exe again with `--wait-pid`, the new deck waits for the old one, takes the lock and restores (**shell.ps1** `restart`); a `relaunch.json` in `%APPDATA%\claude-deck-native\` is honoured (`lifecycle.hpp`). **[OWNER]** whether restarts go through a launcher of their (none exists: the exe restarts itself with its own arguments); never run on the real folder: group (a) |
| 59 | `restartNow` / `restartWhenQuiet` from a remote | done |
| 60 | the exit watchdog (a process still there 8 s after it began to quit) | done — the core arms it at `shutdown()` and at a restart (`Lifecycle::armExitWatchdog(8000)`, once); Windows: one thread that ends what the process started and then the process (no PowerShell), **real**: a hung child process is ended at its time, exit code 0. On only when the process is `ClaudeDeckNative.exe` |

### Multi-window and the window registry — 8 done

| | Responsibility | Status |
|---|---|---|
| 61 | the registry: windows and the desk as entries, `shownBy`, R5's `inWindow` | done — under the remote-desk rule of 2026-11-18 by default (`shownBy` / `inWindow` name the deck's own windows only; a desk is sent `inWindow: null`); `remoteDeskIsAWindow` = `main`'s registry |
| 62 | one home per tile, `layout:evict`, `oneWindow` off, the leaving mark | done — among the deck's own windows (the remote-desk rule); a remote desk evicts nothing, is never evicted, and its `deskLayout` comes back whole. AHEAD of `main` until the owner merges `native-remote-server` |
| 63 | layouts and windows saved and restored (R7), an empty auxiliary window not reopened (R13) | done |
| 64 | a grid change remaps every window's layout | done |
| 65 | a tile dragged out opens a window (`window:open`) | done — `DeckHost::openWindow` makes a second window holding the tile (**shell.ps1** `windows`, `realwin`); in remote mode too since b443ab9, on the first window's desk socket |
| 66 | window bounds | done — the shell reports moves and sizes (`Deck::windowBounds`), the core saves them with the layouts (R7) |
| 67 | visible / minimised / focused | done — `Deck::windowState` from the window's own messages (**shell.ps1** `state`) |
| 68 | closing one of several windows touches no session | done — `DeckHost::removeWindow` (**shell.ps1** `windows`: the tiles keep running, the layout is dropped) |

### The phone server, the viewer, uploads — 9 done

| | Responsibility | Status |
|---|---|---|
| 69 | HTTP: the token, the cookie, the public files, the phone page (`lib/phone.js`) | done — real (loopback), and against the real phone.js's answers |
| 70 | the websocket: hello, calls, attach / data, per-client meta with dedupe and keep-alive, the 4 MB skip | done — real |
| 71 | the browser desk under `/desk/` | done — real (headless Chromium) |
| 72 | the bind choices, `syncPhone`, `phoneStatus`, the Serve link | done (fakes; loopback real); `phoneStatus` also says `assets` / `assetRoot` |
| 72a | the heartbeat and the desk slot (`lib/phone.js` on the review branch: 15 s / 2 missed, any message counts; a silent desk replaced after 10 s + a 3 s ping) | done — fakes (the branch's unit cases) and real (loopback) |
| 73 | uploads (`POST /upload`, the dated folder and its cap) | done — real |
| 74 | the build string and asset fingerprint that make an installed phone app reload | done |
| 75 | where the pages come from in a packaged build | done — beside the exe (`windows/package` packages them; **test-package.ps1**); Settings → Phone says when they are missing (`phoneStatus.assets`; **shell.ps1** `office`) |
| 76 | the page viewer (`lib/viewer.js`: `/view/…`, `viewHtml`, `sessionHtml`, Settings → pages) | done — against the real server's answers |

### Settings, accounts, the alias file — 4 done, 1 partial

| | Responsibility | Status |
|---|---|---|
| 77 | `DEFAULT_SETTINGS`, the merge over a saved file, a BOM, an unreadable file kept aside | done |
| 78 | `applySettings`: every clamp and whitelist | done (first build's fixtures) |
| 79 | `getSettings` in its two shapes; a remote never sees or changes the token | done |
| 80 | the account editor writes the alias file (`saveAccountRows`, `writeAliasFile`) | partial — done and tested on fake files, **switched off**. **[OWNER]** `allowAccountEdit` (userswitch reads that file too) |
| 81 | `settings.phone` drives the server | done |

### The single-instance lock, installers, auto-start — 2 done, 1 partial

| | Responsibility | Status |
|---|---|---|
| 82 | `requestSingleInstanceLock` / `second-instance` | done — a named mutex per state folder; a second launch on the same folder tells the first to come forward and leaves (`lifecycle.hpp`; **shell.ps1** `single`) |
| 83 | the Start Menu shortcut and AppUserModelID (`ensureShortcut`) | done — `notify.hpp`: written by a real run and by `--register` / `--unregister`, tested in temp folders only (**shell.ps1** `register`); the remote has its own id and icon (030d58b). The real Start menu: group (a) |
| 84 | installers, the keep-alive scheduled task, the launcher | partial — the installer is done (`windows/package`: per user, no admin, an optional Startup shortcut, the Electron deck's state imported once; **test-package.ps1**); `running.json` is written, so the existing supervisor contract holds. **[OWNER]** pointing their keep-alive task (box-level, not in the repo) at the native deck |

### The review of 2026-11-18 (the branch `native-remote-server`; not on main yet) — 3 done, 1 partial

| | Responsibility | Status |
|---|---|---|
| 85 | `CLAUDE_DECK_TILE` / `CLAUDE_DECK_STATE` at spawn, `deck-state.json` (`lib/presence.js`), `meta.presence`, `meta.remoteDesk` | done — the environment, the file and `meta.presence` (fixtures from the branch's module; real: a shell tile echoes its tile id and finds the file); the shell reports a key anywhere in the window (`Deck::windowKey`; **shell.ps1** `state`); `meta.remoteDesk` goes out with the driver claim (85b); the version is `core/deck/version.hpp`'s |
| 85a | the remote-desk rule: a remote desk is not one of the deck's own windows (`lib/remoterule.js`) | done — ON by default (`remoteDeskIsAWindow` false); fixtures from the branch's module + scenarios |
| 85b | the driver as a claim (`lib/driverrule.js`: `auto \| office \| phone \| remote`, `sizeBy`, `remoteDesk`, R10 by kind) | partial BY DESIGN — ported, wired and tested, **OFF** (`driverClaim`): a proposal. **[OWNER]** accept it and one initialiser flips |
| 85c | the wheel from a remote desk (R8 narrowed) | done — no server filter, as the branch; counted as one wheel notch, never as typing |

### What simply vanishes with Electron (12)

`render-process-gone` reload; the renderer file watch (`watchRendererFiles`); `rebuildWindow`; `drag:rescue` and
`lib/dragtrace.js`; the webview guards and cloud partitions (`guardWebview`, `cloudPartition`); `screen:probe`
(unused in main.js too); `did-finish-load` re-attach; DevTools; the `--user-data-dir` pin; the Mac menu / Dock /
quit confirmation (the Swift shell's own); `hookAskDownloads` (a page's own Done button downloading in a webview);
`openAskLink` / `open:external` as Electron calls (their RULES are in the core now — `askOpenLink`, `openExternal` —
and the opening is `Lifecycle`'s).

### The order this suggests (2026-11-19)

1. Group (a) above, on the office PC and the home PC: the hand tests (the remote's blockers first), one real toast,
   one real restart, one real install. None needs code unless it finds a fault.
2. Group (b): the owner's switches, one at a time, each with its own real run (usage polling first: it is read-only).
3. Group (c) as wanted; the Swift shell when there is a Mac to build it on.

## Differences from main.js

Deliberate, and each one written down in the code where it happens:

1. **`persistShells` (an option, default OFF = parity).** main.js's `saveState` writes only Claude and cloud tiles;
   with `persistShells: true` a shell tile is saved and comes back as a fresh shell under the same id. The owner's call
   whether a shell-only local deck ships with it on.
2. **`meta.cloudOption` is false unless the deck may make a cloud session** (`allowClaude` and `allowCloudCreate`;
   main.js: always true), so a view offers one exactly when the call would not be refused.
3. **A heartbeat.** `lib/phone.js` on `main` has none: a phone that goes out of range without closing stays
   "attached" — and the driver, under auto — until TCP gives up. Here every socket is pinged every 15 s and dropped
   after two unanswered (any message counts as an answer), and a silent desk loses the one desk slot to the next
   (10 s + a 3 s ping). The review branch has exactly this; it stops being a difference when the owner merges it.
4. **A window's `rid` 0 gets no reply** (PROTOCOL.md); a remote's is answered, as `lib/phone.js` does.
5. **`attach` for a window** follows the desk's path (an explicit carried set, with `detach`), not the Electron
   window's (once attached, always sent).
6. **Chunk boundaries.** A replay chunk is one read burst from the pseudoconsole (merged up to 256 KB), not one Node
   socket chunk. A mode sequence split across two chunks is missed in both, at different places.
7. **Killing.** No process is started to kill one: a snapshot, `TerminateProcess` on the child and its descendants,
   then `ClosePseudoConsole`. Exit code 1 either way. A natural exit closes the pseudoconsole at once (node-pty
   leaves it for a second of quiet).
8. **No hidden PowerShell.** The process tree (the shell-host scan), console hosts and the commit charge are read
   with direct calls where main.js starts a hidden `powershell.exe`.
9. **Adopt and the alias editor refuse earlier.** `adoptSession` refuses before it ends the session's process when
   this deck may not start Claude; `saveAccounts` is off unless the shell allows it.
10. **The trigger inbox is polled** (4 s), not watched.
11. **`tiles.json` / `settings.json` text** has its keys in alphabetical order (the JSON library's). The same goes
    for every frame on the wire.
12. **The first two seconds.** Kept from main.js: nothing is written to `tiles.json` while `restoring()` is true.
13. **JSON and dates.** A transcript line holding a lone surrogate escape is skipped (the C++ JSON library rejects
    it). `Date.parse` is ported for ISO 8601 only.
14. **UTF-8.** The deck keeps bytes, the JS UTF-16 strings; every count is taken in UTF-16 units so limits and cuts
    land where Electron's do. A cut between the halves of a surrogate pair yields U+FFFD here.
15. **`ConPty::write` blocks** the deck thread for the length of a `WriteFile`; node writes asynchronously.
16. **Session ids and the token** come from `std::random_device`, not `crypto`.
17. **Static paths.** `lib/phone.js` guards the phone page's folder with `file.startsWith(root)`, which a sibling
    folder whose name starts the same would pass; here a path is resolved segment by segment and cannot leave the
    folder at all. A segment holding `:` is refused (403). An HTTP request this server cannot parse is a 400 where
    Node's own parser would have answered; a path that will not percent-decode is a 400 where main.js throws.
18. **Responses** carry `content-length` (Node sends these chunked) and no `Date` header.
19. **Usage errors.** A body that is not JSON reads `Unexpected response (not JSON)`, not V8's wording.
20. **The ask folder and the desk inbox are polled, not watched** (4 s and 1.5 s; main.js adds `fs.watch`).
21. **Throwaway decks have no ask feed** unless `ASK_PAGE_STATE_DIR` is set; Electron's scratch decks use the real folder.
22. **A desk request is refused before a tile exists when the deck may not start Claude**, and a desk that needs a
    new window stays in the main window's sidebar when the shell cannot open one (Electron always can).
23. **Ask files.** Keys inside the answers file and the DRAFT copy's nested objects are alphabetical (the page's
    order is lost on the wire); `deck.json` and the .txt copies are byte for byte. `Date.parse` is ISO only;
    lower-casing for a viewer folder id covers ASCII, Latin-1 and Cyrillic (another script's folder gets a different
    id than Electron's — ids are per deck, nothing is shared). The modules' own lists are at the top of
    `asks.cpp`, `viewer.cpp`, `deskopen.cpp`, `cloud.cpp`.
24. **Cloud.** No request timeout in the core (the shell's `Http` must end every GET); a body that is not JSON
    reads `Unexpected response (not JSON)`; `claude --cloud` has its own switch.
25. **The exit watchdog** is a thread of the process, not a child PowerShell; it ends direct children, then the
    process, exit code 0.
26. **The remote-desk rule of 2026-11-18 is ON here and not yet on `main`.** A remote desk is not one of the deck's
    own windows (one home per session, `inWindow`, the leaving mark, sleep). `remoteDeskIsAWindow` gives `main`'s
    behaviour. With the claim off, `office` is accepted as `desk`, and a settings file holding `office` / `remote`
    is read as `desk` / `auto` (main.js on `main` would keep the unknown word and treat it as neither).
27. **The desk slot's probe is pumped.** `lib/phone.js` awaits a promise; here the `deskInit` is answered from a
    timer (every `deskProbeMs / 10`, 5–100 ms). A second `deskInit` from the same caller while it waits replaces
    the first (one answer). An asker that leaves while it waits does not cost the silent desk its slot (the
    branch drops the silent desk all the same); the heartbeat ends it in its own time.
28. **`phoneStatus` has two fields main.js does not** (`assets`, `assetRoot`).
29. **`layout:evict` ids** are in the losing window's layout order, which here is alphabetical (difference 11).

Not differences, but easy to assume: collectMeta's own stamping (lastStatus, busySince, lastStoppedAt) happens on
every meta build, including the one `deskInit` answers with — as in main.js. A session's title reaches `tiles.json`
with the next save, not when it is first read — as in main.js. An idle session's `sleepBlocker` counts seconds
(`active 12s ago`), which makes its meta "changed" on every tick for the remote dedupe — as in main.js.
