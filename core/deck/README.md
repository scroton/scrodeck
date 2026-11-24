# `core/deck/` — the deck itself, as pure C++

What `main.js` + `lib/*.js` are to the Electron app, for the native shells: one implementation the Windows shell
runs today and the Swift shell links tomorrow. **No OS header, no thread, no sleep, no socket, no file call** —
everything the deck needs from a machine comes through the small interfaces in `seams.hpp`, and the shell *pumps*
it. `windows/HOST-NOTES.md` has every call's status, every difference from `main.js`, and the list of what still
stands between this and switching Electron off.

## Shape

```
              frames in  handle(client, frame)            frames out  sink(client, frame) -> went out?
 shell  ───►  bytes in   onPtyData / onPtyExit(token)  ◄───  Deck  ───►  PtyHost.spawn / Pty.write / resize / kill
              time       tick()  every nextDueMs()                       Files, Clock, ProcessList, Random, Env,
                                                                         Notifier, Http + Credentials, Lifecycle
 sockets ──►  Server.onAccept / onBytes / onClosed  ──► Deck.addRemote / handle / removeClient
              Server.tick()                         ◄── PhoneHost: status(), sync(), extras()
```

| File | What | Ported from | Proof |
|---|---|---|---|
| `seams.hpp` | the interfaces a shell implements | `lib/platform.js`'s OS half, node-pty, `fs`, `http` | — |
| `deck.hpp`, `deck.cpp`, `deck_more.cpp`, `deck_impl.hpp` | **the deck**: tiles, processes, the call table, frame dispatch, clients and windows, meta per client, the driver rules, layout / one-window rules, persistence, hibernation + memory guard, special prompts' wiring, triggers, restart when quiet, Elsewhere / Past / adopt, account moves, the shell-host scan, a tile following its session after /clear or /resume (`followTiles`) | `main.js`, `lib/phone.js` (the client records), `lib/hibernate.js`, `lib/quietrestart.js`, `lib/triggers.js`, `lib/shellhost.js`, `lib/tilefollow.js` | scenarios on fake seams |
| `rules.{hpp,cpp}` | spawn environment, `newTileSpec`, **`claudeLaunch`**, alias-file accounts, `DEFAULT_SETTINGS`, `applySettings` | `main.js`, `lib/accounts.js` | fixtures (core/test/fixtures/session) + windows/host-test |
| `statefile.{hpp,cpp}` | atomic writes, backup history, unreadable-file recovery | `lib/statefile.js` | host-test + scenarios |
| `remote.{hpp,cpp}` | what a remote is sent: `clipMessage`, `remoteMeta`, `deskMeta` (this tree's, and with a `DeskRule` the review branch's: `inWindow` null, the claim's mirror), the dedupe key | `lib/phone.js` | fixtures |
| `held.{hpp,cpp}` | special prompts: the list, its file, the delivery | `lib/queue.js` | fixtures (scripted runs of the real class) |
| `startup.{hpp,cpp}` | Claude Code's startup dialogs off the pty stream; the trust prompt's reader | `lib/startupprompts.js`, `main.js` `trustWatch` | fixtures (the recorded dialogs in `test/fixtures/startup-*.raw`) |
| `usage.{hpp,cpp}` | the usage poller: cadence, cache, 429 back-off | `lib/usage.js` | fixtures (the real class on a fake `fetch` and fake credentials) |
| `remoterule.{hpp,cpp}` | where a remote desk stops (the remote-desk rule, 2026-11-18): one home per session, `inWindow`, the leaving mark and hibernation's on-screen set are the deck's OWN windows'; a sleeping session is woken when it arrives on a remote's screen. **On by default**; `DeckOptions::remoteDeskIsAWindow` restores `main`'s rule | `lib/remoterule.js` (the review branch's; `gen-remoterule.js <path>`) | fixtures (its unit cases + a sweep) + scenarios (`remoterule_test.cpp`) |
| `driverrule.{hpp,cpp}` | who drives a session's size, as a claim by one kind of screen (`auto \| office \| phone \| remote`), who mirrors, whose typing is locked (R10), the button. **A proposal: OFF** unless `DeckOptions::driverClaim` | `lib/driverrule.js` (the review branch's; `gen-driverrule.js <path>`) | fixtures (the branch's 171 checks one for one, a sweep, `lib/phone.js deskMeta`) + scenarios under every driver value, claim on and off |
| `version.hpp` | the native deck's version, in one place; a resource script can include it | — | host-test (the builder) |
| `presence.{hpp,cpp}` | the deck's contract with the sessions it hosts: `deck-state.json` (`where`, `away`, `remote`, `phone`, `deliver`), `CLAUDE_DECK_TILE` / `CLAUDE_DECK_STATE` | `lib/presence.js` (the review branch's; `gen-presence.js <path>`) | fixtures (every step's file text, byte for byte) |
| `asks.{hpp,cpp}` | ask pages: the feed over the ask-page skill's state folder, drafts, the log, Done's files, `deck.json` | `lib/asks.js` | fixtures (`gen-asks.js`: scripted runs of the real class) |
| `viewer.{hpp,cpp}` | the page viewer: which .html a path names, which folders may be served, a session's pages | `lib/viewer.js` (+ `lib/phone.js` VIEW_*) | fixtures (`gen-viewer.js`) + the real server's answers (`gen-routes2.js`) |
| `deskopen.{hpp,cpp}` | desks the on-call opens: the request, the plan, the placement, the inbox | `lib/deskopen.js` | fixtures (`gen-deskopen.js`) |
| `cloud.{hpp,cpp}` | cloud sessions: ids, the created-session reader, the status poller | `lib/cloud.js` | fixtures (`gen-cloud.js`: the real poller on a fake fetch) |
| `deck_parity.cpp` | those four on `Deck::Impl`: `doneAsk`, the ask toast, `viewHtml` / `sessionHtml`, `openDeskRequest`, cloud tiles, the exit watchdog's core half, a clicked toast | `main.js` | scenarios on fake seams (`deck_parity_test.cpp`) |
| `transcript_page.{hpp,cpp}` | the phone's text view: a transcript as pages of conversation | `lib/transcript-page.js` | fixtures |
| `platform.{hpp,cpp}` | which Claude binary / shell / bash / userswitch / alias file; window keys; key help | `lib/platform.js` (the decisions) | fixtures + fakes |
| `hash.{hpp,cpp}` | SHA-1, SHA-256, HMAC-SHA-256, base64 | Node `crypto` | fixtures |
| `http.{hpp,cpp}` | request reader (keep-alive, chunked), URLs as `new URL()` reads them, cookies, responses | Node `http` as `lib/phone.js` uses it | unit + routes fixture |
| `ws.{hpp,cpp}` | RFC 6455: handshake, frame decoder (everything `ws`'s Receiver refuses), encoder | the `ws` package | fixtures (frames written by `ws`'s own Sender) |
| `server.{hpp,cpp}` | **the server**: token + cookie, routes and their headers, upgrade, heartbeat (any message counts) and the desk slot's `heardAt` / `ping` / `drop`, the 4 MB skip rule, bind choice, `phoneStatus` (+ `assets`, `assetRoot`), `syncPhone`, the Serve link | `lib/phone.js`, `main.js` `syncPhone` / `phoneStatus` | every request replayed against what the REAL `lib/phone.js` answered; the branch's heartbeat cases on the fake listener |
| `util.{hpp,cpp}` | paths, ISO time, "is this a JSON state" | — | — |
| `options.{hpp,cpp}` | **how a shell's wishes become the core's options, once for every shell**: `buildDeckOptions(HostOptions, Env, Files)` → `DeckOptions` + `ServerOptions` (every option the core grows is set HERE), the native deck's own state folder per platform, and the SHIPPING DEFAULTS of the three switches that must never be on by accident (Claude sessions, the ask feed, the exit watchdog) as pure functions of { is it the app, its arguments, its environment } | `windows/host/localdeck.cpp buildDeckOptions` + the Mac bridge's `buildOptions` (the same function twice) | `core/test/deck` `options`, both path modes; windows/host-test `builder:` |
| `hosting.{hpp,cpp}` | **the deck as a shell holds it, once for every shell**: the `Inbox` other threads post to (the one thread-safe thing; the shell gives it a WAKE callback), the pump, the server beside the deck, the shell's windows as clients with their own sinks, exact routing, deferred or inline delivery. No thread, no OS call | `windows/src/deckhost.hpp` + `windows/host/localdeck.cpp` (`Shared`, the drain) + the Mac draft's `DeckBridgeHost.hpp` / `DBDeck` | `core/test/deck` `hosting`, both path modes; everything that runs the Windows exe |

## The rules of the port

1. The JavaScript is the specification. Where a pure function exists there, a fixture is generated from it
   (`core/test/deck/gen-fixtures.js`); where `main.js` holds the logic inline, the pattern is lifted out of its
   source text, and the script throws if it is no longer found.
2. **Multi-client, as the Electron main process is.** A client is a window of this deck (`addWindow`) or a remote
   (`addRemote`: a phone until it calls `deskInit`, a desk after — one desk at a time). Each has its own attached
   set, shown set, layout, mirror view and rid space.
3. **The sink says whether a frame went out.** `false` is the transport's "busy" (a socket over 4 MB): the frame
   is skipped, and a skipped meta is sent again at the next tick.
4. Nothing starts Claude unless `DeckOptions::allowClaude` says so, nothing edits the alias file unless
   `allowAccountEdit` says so, usage is polled only when a shell hands in both `Http` and `Credentials`, cloud
   sessions are read only with `cloudPolling` and made only with `allowCloudCreate`, and the ask feed touches the
   ask-page skill's folder only when a shell names it (`askStateDir`).
5. **A remote desk is not one of the deck's own windows** (the remote-desk rule, on by default), and **the driver
   claim is a proposal** (off by default: the deck speaks this tree's `auto | phone | desk`). Both are options of
   `DeckOptions`, each path is tested, and `windows/HOST-NOTES.md` "The remote desk and the office" has the long
   form — including the one line that flips the claim's default the day the owner accepts it.

## Hosting: windows, saved places and routing (`hosting.hpp`)

A shell makes ONE `core::Host` (its seams, an `Inbox`, and a `DeckBuild` from `buildDeckOptions`), calls `start()`,
and from then on: its threads push to the `Inbox` (pty bytes and exits, socket events, finished HTTP requests);
the inbox's wake gets the shell back to its own thread (`PostMessage`, `DispatchQueue.main.async`), which calls
`pump()`, and again after `nextDueMs()`. Each window of the shell is `addWindow(bounds, sink)` /
`openWindow(tile, bounds, sink)` / `adoptWindow(client, sink)` (a client the core registered through
`Lifecycle::openWindow`), its frames go in through `handle(window, frame)`, and `removeWindow` when it closes.

**Which saved entry and index a window takes** (R7; the Mac draft's two questions, answered by `Deck::addWindow`
and checked in `hosting_test.cpp`):

- **The n-th window made takes the n-th saved entry**, whether it is made before `start()` (it takes its place when
  `start()` reads the state) or after it. So the Mac's order — window 1, `start()`, then one window per further
  saved entry — gives the same windows as `app.cpp`'s (count the entries, make them all, then `start()`). A window
  beyond the saved ones gets the bounds the shell gave, an empty layout and the next index. Indexes count up from 0
  and are never reused.
- **The quit keeps every window** (`main.js` `closed`, 11bf1ab; the native deck 2026-11-24): a window closing while
  others stay open drops out of the file but is remembered; the LAST one closing (`removeClient`), or the deck ending
  with windows still open (`shutdown`: the Windows shell's last window ends the deck), writes back every window open
  then AND every one closed in the 2 minutes before, each with its layout and bounds, in window order; a tile placed
  in more than one stays in the window that closed last. A window closed earlier than that is dropped, as before. A
  restart has saved them all already and writes nothing here. The shared layout file follows (`saveState`).
- **Every window closed, then a new one** (the Mac: Cmd+W on the last window, the deck lives on, the Dock icon —
  `main.js` `activate`): the new window takes the file's FIRST entry as it is now (after the rule above: window 0 of
  those kept) with a NEW index (`main.js createWindow` without `opts.index`: `nextWindowIndex++`). A second window
  after that is a plain new one (the saved entries were taken at boot). On Windows the last window closing quits the
  deck (`window-all-closed`), so this path is the Mac's.

**Routing is exact, by client id** (the race the Mac draft found): a frame the core raises goes to the window whose
id it names; the window BEING ADDED is matched by the id `Deck::nextClientId()` said it would get, not by "whatever
id is unknown right now"; anything else goes to the server (a phone, a desk) or nowhere. Before this, both shells
sent every unknown id to the window being added — and `openWindow` publishes a meta to the remote clients from
inside the call (`takeLayoutMap` → `pushMeta`), so a phone's meta went to the new window and the phone missed it.
The test stages exactly that (it fails with the old rule). `core::Server` adopts a socket's client the same way:
only the id `nextClientId()` named while its `addRemote` runs.

**Delivery.** By default a window's frames are queued while a `Host` call runs and delivered when the outermost one
returns, so a sink may call straight back into the host (the Mac's sinks call into the window) and is never
re-entered. A shell whose sinks only post (Windows: `PostMessage` with a heap string) asks for `deliverInline`.
`enter(fn)` gives a direct `deck()` call the same promise. After `shutdown()` nothing reaches a deferred sink.

**Paths.** The `Host` sets the separator `join()` writes from `Env::os()` (`\` on win32, `/` elsewhere) before it
makes the deck; a shell does not call `setPathSep` itself.

## Tests

```
node core/test/deck/gen-fixtures.js            # after lib/phone.js, lib/queue.js, lib/startupprompts.js, lib/usage.js,
                                               # lib/transcript-page.js, lib/triggers.js, lib/platform.js or main.js changed
node core/test/deck/gen-fixtures.js --routes   # + the HTTP routes: runs the real lib/phone.js on 127.0.0.1:8812 (from PowerShell)
node core/test/deck/gen-presence.js <path to lib/presence.js>     # the review branch's today, main's after the merge
node core/test/deck/gen-remoterule.js <path to lib/remoterule.js> # the same: the remote-desk rule
node core/test/deck/gen-driverrule.js <path to lib/driverrule.js> # the same: the claim (+ lib/phone.js deskMeta beside it)
node core/test/deck/gen-asks.js | gen-viewer.js | gen-deskopen.js | gen-cloud.js   # one per module of the 2026-11-19 build
node core/test/deck/gen-routes2.js             # /view + /ask off the real server on 127.0.0.1:8813 (from PowerShell)
powershell -File core/test/deck/build.ps1      # -> 27906 passed, 0 failed; then the POSIX twin (--posix): 6122 passed
                                               #    [remote|wire|startup|held|usage|rules|presence|transcript|asks|viewer|deskopen|cloud|deck|server|parity|remoterule|driverrule|ruling|options|hosting]
```

`core/test/deck/fakes.hpp` holds the doubles: a clock that moves only when told, files in memory, processes that are
a record of what was asked of them, a listener that is two strings per socket. No test in this folder opens a
socket, starts a process or reads a real config dir.

**Two path modes.** `fakes.hpp` has a POSIX twin (`--posix`, which `build.ps1` runs after the default run): `/`
paths, `os()` = `darwin`, a login `$SHELL` — the deck as the Mac configures it. A test writes each path once,
Windows-shaped, through `P()`; in POSIX mode the fake files do not read `\` as a separator, so a path the core
builds with a hard-coded backslash misses its file and a check fails. The deck scenarios, hosting, options, the
remote / driver rules under the deck and the server run in both modes; the fixture replays (and `parity`, which
replays the JavaScript's answers recorded with this box's paths) in the win32 mode only. What the POSIX run found
on its first day: the alias file's account folders were normalised with Windows rules on every platform
(`rules.cpp parseAccounts`, `deck.cpp toHomeRelative` / the account editor's `path.isAbsolute`), and the deck's
own fallback key help and font stack were the Windows ones whatever `Env::os()` said.

## For the Swift shell

Implement `Clock`, `Files`, `PtyHost` (`forkpty`), `ProcessList`, `Random`, `Env` (`os()` = `"darwin"`), `Listener`
(Network.framework or BSD sockets), `Net`, and — when the owner allows them — `Credentials` (the Keychain read
`lib/platform.js` documents) and `Http`. Then hold the deck the way the Windows shell does: `buildDeckOptions`
(`options.hpp`, with `shippingDefaults(isApp, args, env)` where "is the app" is the bundle's own test) and one
`core::Host` (`hosting.hpp`, above) whose inbox wakes the main queue. `core/deck/platform.hpp` already answers the
macOS questions (`claudeExe`, `shellCommand`, the Cmd-key map, the font stack); `core/input/keys.hpp` is the key
table, `core/vt` `Terminal::selectionText` the copy.

`ProcessList::commit` (the memory guard; the Mac draft's open question): `main.js commitPressure` on anything but
Windows feeds `lib/hibernate.js` `{ used: os.totalmem() - os.freemem(), limit: os.totalmem() }`. On macOS Node's
`os.freemem()` is libuv's `host_statistics(HOST_VM_INFO)` `free_count` × the page size (free pages only: inactive
and speculative pages are not counted) and `os.totalmem()` is `sysctl hw.memsize` — so `commit(used, limit)` should
return exactly those two numbers (`used = memsize - free_count * pagesize`, `limit = memsize`) and `true`.
