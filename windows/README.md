# Claude Deck — native Windows shell (`windows/`)

The C++ Windows shell of the native port (docs/NATIVE-PORT.md §2, R11): Win32 + Direct2D + DirectWrite over the
shared C++ core in `core/`. The Electron app is the specification; this recreates it, to replace it ("Running it as the deck" below).

It runs in two modes, one exe, the same window and the same frames (`core/PROTOCOL.md`):

- **remote mode**: run on another Windows desktop, it drives a Claude Deck running elsewhere — the socket and
  the messages the browser desk uses (`lib/phone.js`);
- **local mode** (`--local`, or "Use this PC (local preview)" on the first run): it is a deck of its own, in
  this process (`core/deck` over `host/`, `HOST-NOTES.md`): terminal tiles through ConPTY, and Claude sessions when
  the deck's `allowClaude` is on (the core/host side's switch: on in a real run, off under `--state-dir`;
  `--allow-claude` / `--no-claude`). See "Local mode: the shell half" below.

The window title ends in `— remote` or `— local`. The mode used last is remembered (`config.json` `mode`);
`--forget` asks again, and so does the palette (Ctrl+Shift+P): **Connect to another deck…** (paste the link),
**Use this PC (local preview)**, and in local mode **Connect to the remembered deck**.

## Running it as the deck

This is the short form of using the exe INSTEAD of the Electron deck on a PC. `PACKAGING.md` has the package, the
installer and the switch step by step; `PARITY.md` and `HOST-NOTES.md` say what is still different.

**Start it:** `ClaudeDeckNative.exe --local` (the installer's Start-menu shortcut passes `--local`; with no argument
the first run asks "this PC, or another deck's link" and remembers the answer). A second launch brings the first
window forward: one deck per state folder.

**On by default in a real run** (a run on its own state folder; every one of these is OFF under `--state-dir`, which
is what every test uses):

| | |
|---|---|
| Claude sessions | account buttons, Elsewhere / Past, moves between accounts (`--no-claude` = terminal tiles only) |
| The saved tiles come back | every Claude tile that was not asleep is resumed at start, as in Electron |
| The ask feed | the ask-page skill's own folders are read (`~/.local/state/ask-pages`, the Downloads folders); see the guard below (`--no-asks` switches it off) |
| Toasts and the taskbar flash | under the deck's own identity (`demo.claude-deck-native`) and its own Start-menu shortcut, `Claude Deck Native.lnk`, which the exe makes sure exists |
| Restart when quiet, the update offer | the exe starts itself again; a replaced exe is offered as an update |
| The on-call's desk requests | read from `<state>\local\desk-open` |
| The exit watchdog | the process is gone 8 s after the last window closed, whatever hangs |

**Off until somebody switches it on:** the phone / remote-desk server (Settings → Phone, or `--serve <port>` for one
run; an imported `settings.json` brings the Electron deck's choice, port and token along), usage polling, cloud
polling and making cloud sessions, the account editor writing the alias file (those four are the core/host side's
switches and the owner's word: `HOST-NOTES.md`), the driver as a claim, terminal tiles surviving a restart
(`--persist-shells`).

**The ask-feed guard: it can run beside the Electron deck.** Both decks read the same skill folder. A deck that finds
another deck's live heartbeat in the skill's `deck.json` is a **guest**: it shows the waiting pages read-only (the
asks tile says "n waiting · read-only", the pane says who serves them), writes nothing there, raises no ask toast,
and takes over only when the other deck's heartbeat has been silent for 90 s or its file is gone. So starting the
native deck while the Electron deck runs changes nothing for the sessions that are asking. (Their TILES are another
matter: one session open in two decks is two processes on one transcript — never import or adopt a session the other
deck still has open.)

**Stop it:** close its windows. Closing one of several touches no session; the last one closing ends the deck and
the processes it started (the sessions are on disk and resume at the next start). Alt+F4 does the same; the restart
button restarts when every session is quiet. After a crash `local\running.json` is left behind, as the Electron
deck's is, for a keep-alive of the owner's to see.

**Where its state lives:** `%APPDATA%\claude-deck-native\` and nowhere else — never `%APPDATA%\claude-deck\` (the
Electron deck's), and nothing beside the exe.

| Path | What |
|---|---|
| `config.json` | the shell's own: local or remote, the remembered link (token DPAPI-protected), a remote's window and layouts, what `localStorage` holds in Electron |
| `local\tiles.json`, `settings.json`, `triggers.json`, `held-prompts.json`, `usage-cache.json`, `ask-drafts.json` | the deck's state: the Electron deck's file names and shapes (a copy of its files is a migration: `PACKAGING.md`). The windows' places are in `tiles.json`, the first window's included |
| `local\running.json`, `local\deck-state.json` | `{ pid, at }` while it runs; what the hosted sessions read about the deck |
| `local\uploads\`, `trigger-inbox\`, `desk-open\`, `tiles-backups\` | as in Electron |
| `web-cloud\` | the cloud panes' sign-ins (one WebView2 profile per account) |
| `relaunch.json` | optional, the owner's: what a restart starts instead of the exe itself |

**The shortcut and updates.** `ClaudeDeckNative.exe --register [--shortcut-dir <folder>] [--shortcut-args "--local"]`
writes the Start-menu shortcut that carries the AppUserModelID and exits (no window, no deck, no state);
`--unregister [--shortcut-dir <folder>]` removes it when it points at this exe. An installed deck can be updated while
it runs: `install.ps1` moves the running exe aside (`ClaudeDeckNative.exe.old`), the running deck offers the restart,
and the leftover goes at its next start.

**Switching from the Electron deck** is `PACKAGING.md` "For the owner: the switch, step by step": make the package,
try it beside the Electron deck importing nothing, then close the Electron deck (and its keep-alive), install, import
once (`import-from-electron.ps1`: a copy, the Electron folder is only read), start. Going back is starting the
Electron deck again.

## Use it on another desktop

1. Tailscale is on, on both machines (the same tailnet the phone uses).
2. Copy `ClaudeDeckNative.exe` anywhere and run it. It is one static file: no installer, no admin, no runtime.
3. The first run asks for the deck's link: on the PC that runs Claude Deck, **Settings → Phone** shows it
   (`https://<pc>.<tailnet>.ts.net/?t=…`). Paste it; it is remembered (the token DPAPI-protected) under
   `%APPDATA%\claude-deck-native\`. `ClaudeDeckNative.exe --forget` asks again. The same box has a second
   button, **Use this PC (local preview)**: no deck anywhere, a local one (its state in
   `%APPDATA%\claude-deck-native\local\`).

One remote desk at a time: while a browser desk (`/desk/`) is open on that deck, this is refused and says so.
It registers as a window of the deck, as the browser desk does, with one difference (2026-11-18): **a
remote keeps to itself**. It never treats a session as held by another window — a click shows any session — and
its grid layout stays on this machine (`config.json` `layouts[<host:port>]`), so placing a session here takes it
from nobody: the pane it has in the deck's own window stays. Who drives a pane's size follows the deck's driver
setting, as before.

**The driver button, on two kinds of deck.** A deck from before "the driver is a claim" (`main` today) says
`auto | phone | desk`: the button cycles the three, exactly as its own window's does. A deck with the claim (branch
`native-remote-server`, not merged at the time of writing) says `auto | office | phone | remote`: the icon shows
who drives (a monitor, a phone, two screens for the remote desk, an eye for auto), one click claims the size for
THIS screen (`remote`, green), a click on the claim gives it back (`auto`), grey means another screen has it.
Claiming also says this window's pane sizes at once. The exe tells the two decks apart by the meta itself
(`office` / `remote`, a tile's `sizeBy`, or `meta.remoteDesk`), so nothing is configured. It gives its computer
name with `deskInit`; a claim deck shows it on its other screens as "the remote desk (NAME)". Closing the window says goodbye properly (the websocket close handshake), so the deck's one
desk slot is free at once.

In a terminal pane **Ctrl+C with a selection copies it** (and sends nothing); with no selection it is the
interrupt.

## Cloud sessions

A cloud tile's pane is the session's own claude.ai page (an embedded WebView2, as the ask pane is), below the pane's
name. The first pane of an account lands on claude.ai's sign-in page, with a line saying which login it wants: **sign
in once per account on this machine**; every later pane of that account is signed in, and it survives restarts. The
sign-ins live in `%APPDATA%\claude-deck-native\web-cloud\` (one WebView2 profile per account, named after the
account's email); delete that folder to sign every account out. Nothing of the deck's is in it, and the ask pane's
pages never see it. Only claude.ai, claude.com, anthropic.com and the sign-in pages a login passes through load in a
pane; any other link opens in your default browser. F5 (or the tile's `reload`) loads the page again; `browser`
opens it in the default browser. Without the WebView2 runtime a cloud pane is a card with a link instead.

Right-click an account's button under "New" (or the palette: "New cloud session: …") for the dialog that starts one:
the task is its first prompt, the folder is where `claude --cloud` runs on the deck's machine.

## Fonts

The deck's bundled console fonts (Reddit Mono, JetBrains Mono, Fira Code, Source Code Pro, IBM Plex Mono, Inconsolata,
Ubuntu Mono) are files in the repository (`assets/fonts`), and this exe is one file that carries none of them. To use
one without installing it: **copy the folder `assets\fonts` next to `ClaudeDeckNative.exe`** (so that
`<folder of the exe>\assets\fonts\jetbrainsmono\…ttf` exists), or name any folder of them in `config.json`
(`"fontsDir": "D:\\fonts"`) or with `--fonts-dir`. They are read from their files (nothing is installed). An exe run
from `windows\build` inside a checkout finds the checkout's own. Without the folder, Settings → Font still takes any
font installed in Windows, and says so when the one chosen is not there (Cascadia Mono is used until it is).
Embedding them in the exe would add about 1.9 MB; it was left out on purpose and is a small change (an RCDATA
resource per file and `IDWriteFactory5::CreateInMemoryFontFileReference` in `gfx.hpp` `loadFontFolder`).

## Build

```
powershell -NoProfile -ExecutionPolicy Bypass -File windows/build.ps1
```

Needs the WinLibs MinGW-w64 g++ (`winget install BrechtSanders.WinLibs.POSIX.UCRT`). Output:
`windows/build/ClaudeDeckNative.exe`, static, GUI subsystem — it never opens a console window.

## Layout

| Path | What |
|---|---|
| `src/app.cpp` | the window and the glue: socket, panes, layout, keys, mouse (each block names the JS it ports) |
| `src/sidebar.cpp` | the sidebar: tiles, bars, confirm rows, rename, tools — `renderer/js/30-tiles.js` and `style.css`, drawn |
| `src/overlays.hpp` | menus, the palette, Elsewhere / Past, move-all, the drop preview |
| `src/settings.hpp` | the Settings panel |
| `src/edit.hpp` | the one single-line text box every panel uses |
| `src/termview.hpp` | one terminal pane: a `core/vt` screen drawn with Direct2D |
| `src/gfx.hpp` | the drawing surface: a window, or an off-screen bitmap that saves as PNG; the **scene** (the kept picture a frame repaints only the changed parts of) and the terminal's layout cache |
| `src/ws.hpp` | WebSocket client on WinHTTP (system TLS and proxy); its header states the thread and handle rules |
| `src/deckhost.hpp` | local mode: the in-process deck as the Windows SHELL holds it — `core/deck/hosting.hpp` (the inbox, the pump, windows as clients, exact routing; shared with the Mac) over the seams of `host/`, the wake a `PostMessage` to the hub, each window's frames posted to its HWND; the notifier and the lifecycle as seams, `--serve` |
| `src/notify.hpp` | toasts (WinRT through its raw COM ABI), the taskbar flash, the AppUserModelID and the Start-menu shortcut; a recording double for tests |
| `src/lifecycle.hpp` | restart (relaunch + `--wait-pid`), the update fingerprint, the single-instance lock, the exit watchdog |
| `src/localtransport.hpp` | (the core/host side's) the ONE-window adapter over `host/localdeck`; `app.cpp` no longer includes it — `deckhost.hpp` took its place. If `LocalDeck` grows windows / a notifier / a lifecycle, `deckhost.hpp` is the file to fold into it |
| `src/keys.hpp` | `VK_*` → `core/input/keys.hpp`'s `Key` (and AltGr): the table itself — what a key sends, a paste, the window's keys — is the core's, shared with the Mac shell |
| `src/mouse.hpp` | a forwarding header: the mouse encoder is `core/input/mouse.hpp` (the old path stays for the Mac draft's symlink) |
| `src/config.hpp` | `%APPDATA%\claude-deck-native\config.json` |
| `src/script.hpp` | `--do "<steps>"`: scripted clicks and keys for the headless tests |
| `src/asks.hpp` | the ask pane (49-asks.js): its model, the host document of its web view, the bridge to the ask pages |
| `src/cloud.hpp` | the cloud panes (48-cloud.js): one web view per cloud pane on show, the profile per account, what may load, the hint, the card |
| `web/` | `WebPane`: one embedded WebView2 as a child window, no loader DLL (`WEB-NOTES.md`); `third_party/webview2/` is the SDK header |
| `web-test/` | the web pane and the ask pane as components against a fake deck (`fake-deck.js`: the real `lib/phone.js`) |
| `host/` | local mode's OS side: ConPTY and an in-process deck speaking `core/PROTOCOL.md` (`HOST-NOTES.md`) |
| `res/` | icon, manifest (per-monitor-v2 DPI), version block |
| `test/` | the scratch-deck launcher and the off-screen shot harnesses |
| `PARITY.md` | every Electron feature: done / partial / missing, and how each was checked |

What is not there yet is in `PARITY.md`. The ask pane and the cloud panes are there (embedded WebView2: `web/`,
`src/asks.hpp`, `src/cloud.hpp`, `WEB-NOTES.md`); the cloud panes have only ever been run against a local page standing
in for claude.ai (no test loads the real site), so the first real sign-in is still to be seen by a person.

## Local mode: the shell half

What `src/` does around the core deck so the exe can stand in for the Electron one on this PC (PARITY.md "Local mode"
and "Switching Electron off: what the shell still lacks" have the rows and the gaps):

- **One process, several windows.** Each top-level window is an `App` and one client of the deck; a message-only hub
  window takes what belongs to the process (the deck's wake-ups and its 50 ms timer, a clicked toast, a second launch,
  "after this message" tasks). Open one from a tile's menu or by dragging a tile out of the window; close any of them
  and its sessions run on; the last one closing ends the deck. They come back after a restart.
- **The deck is told about its windows**: on screen / minimised / focused, their place, every key-down, the version.
- **Toasts** are the process's own (`demo.claude-deck-native`, `Claude Deck Native.lnk`), never the Electron deck's.
- **Restart when quiet / now**: the exe starts itself again (`relaunch.json` in the config folder names something
  else, as it does for the Electron deck) and the new one waits for the old. **One deck per config folder**: a second
  launch brings the first forward. A rebuilt exe is offered as an update.
- **Settings → Phone** and `--serve` work against the native deck.
- Tests: `powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/shell.ps1` (163 checks; ~5 minutes; nothing
  on screen, no toast, no shortcut, loopback 8811–8819 only, a temp state folder and a temp home).

## Options

| Option | What |
|---|---|
| `--local` | local mode, no question asked |
| `--link <url>` (or `CLAUDE_DECK_LINK`) | remote mode on this deck link (remembered unless `--no-save`) |
| `--forget` | drop the remembered link and mode, ask again |
| `--state-dir <folder>` | keep `config.json` and the local deck's state (`<folder>\local\`) there instead of `%APPDATA%\claude-deck-native\`. **Every test passes a temp folder**; a headless run that would need the real folder exits 7 |
| `--persist-shells` (or `"persistShells": true` in `config.json`) | local mode saves terminal tiles and brings them back as fresh shells after a restart. Default off = what Electron does (`HOST-NOTES.md` difference 1) |
| `--serve <port>` | local mode also serves a phone / a remote desk on **127.0.0.1:`<port>`** for this run, whatever Settings → Phone says (nothing is written to the settings) |
| `--assets <folder>` | where the pages that server serves live. Default: the exe's own folder (a package ships them there); only an exe in a checkout's `windows\build` also looks in that checkout's root. Not found: Settings → Phone says so, with the folder that was tried |
| `--allow-claude` / `--no-claude` | the local deck may / may not start Claude sessions; the last of the two wins. Default: the core/host side's (`host/localdeck.hpp`) — on in a real run, off under `--state-dir` |
| `--wait-pid <pid>` | (set by a restart) wait for that process to be gone before anything else |
| `--register` / `--unregister` `[--shortcut-dir <folder>] [--shortcut-args "<args>"]` | write / remove the Start-menu shortcut with the AppUserModelID and exit: nothing is shown, no deck starts, no state is touched. Exit 0 = done (or nothing of ours was there), 1 = failed, 2 = `--unregister` left a shortcut that points at something else. With `--shortcut-dir` the outcome is also a line in `<folder>\register.log`. **A test always names a temp folder** |
| `--no-asks` / `--asks` | the ask feed off / on, wherever (read by the core/host side; default: on in a real run, off under `--state-dir` unless `ASK_PAGE_STATE_DIR` is set) |
| `--no-save` | remember nothing |
| `--fonts-dir <folder>` | where the bundled fonts' files are (see Fonts); `none` loads none |
| `--desk-name <name>` | tests: the name `deskInit` gives instead of this computer's |
| `--home <folder>` | tests: the local deck's home — accounts, the alias file, sessions and transcripts are looked for there, never in the user's own |
| tests only | `--driver-claim`: the local deck with the driver as a claim (the core's `driverClaim`, a proposal that is off by default; one initialiser in `core/deck/deck.hpp` is its real switch) |
| tests only (more steps of `--do`) | `event <name> <json>` (an `event` frame as the deck would send it), `ime comp\|commit\|end <text>`, `trans <ms>\|off\|dump` (the css transitions' clock), `stamp <text>` (what the special prompt form's time box makes of an entry), `dump ghost [name]` (the picture a tile dragged out of the window carries), `life deskwindow` (a window for a client the core registered: `Lifecycle::openWindow`) |
| tests only (more steps of `--do`) | `win <n> <step>` (the step carried out by deck window n), `toast waiting\|done\|ask [session]\|click [tile]\|flash <n>\|dump`, `life spec\|bump\|second\|lock`, `dump phone\|windows\|presence\|restart` |
| tests only (steps of `--do`, `src/script.hpp`) | `feed <bytes>` (into the pane, `\e` = ESC, `\s` = `;`), `mods shift\|ctrl\|alt\|none` (held for the mouse steps), `tri`, `mclick`, `clip <text>`, `dump clipboard\|layout`, `faketile <field> <json>`, `ws silence\|count`, `close` (the window's own WM_CLOSE). A test's log names every `input`, `report` (mouse / focus), `reply` (the terminal's own answers), `resize` and `nudge` it sent |
| tests only | `--shot <png>`, `--window-shot <png>` (the real window, parked off every screen), `--probe <png>`, `--do "<script>"` (`src/script.hpp`), `--after <ms>`, `--size WxH`, `--select`, `--place`, `--type`, `--shell <exe>` (the program a local terminal tile runs; the tests use `cmd.exe`), `--dpi <n>` (an off-screen run drawn at that scale: 144 = 150 %), `--bench <log> --fixture <meta.json> [--frames n]`, `--verify-scene`, `--no-scene`, `--cloud-base http://127.0.0.1:<port>` (the local page that stands in for claude.ai in a cloud pane: a test never loads the real one), `--dpi` with `--window-shot` (the parked window and its web views as at that scale), `--web-warm 0|1` (the ask pane may start its web view when the pointer rests on the asks tile: on in a real run), `--web-grace <ask ms>,<cloud ms>` (how long a web view nobody can see lives: 20 s and 2 min in a real run), `--web-flags "<arguments>"` (the browser's command line, to measure a flag) |

## Testing without touching the live deck

Everything visual is checked from PNGs rendered off-screen; no test shows a window.

```
powershell -File windows/test/local.ps1 -Name x -Do "zone new-tile; wait 2600; text echo hi; key enter; wait 600; dump screen"
```

is a headless LOCAL run: its own deck in a temp state folder, `cmd.exe` in a pseudoconsole, the picture in
`windows/test/out/n-x.png`, the log beside it, and at the end a check that no console child of the exe is left.
`-StateDir <folder>` twice is a restart; `-PersistShells`, `-Dpi 144`, `-WindowShot` (the real window target).
`-DefaultShell` runs the deck's real default terminal (PowerShell with the profile) instead of `cmd.exe`.
`windows/test/mouse/build.ps1` runs the mouse encoder's unit test, which is the core's now (`core/test/input`, 762 vectors from xterm.js; `-Regen` rewrites them).
`windows/test/close.ps1` (does the scratch deck see the client leave?) and `windows/test/remote-own.ps1` (a remote
keeps its layout to itself) read the scratch deck from its own side over CDP 9777 (`deskcount.js`, `cdp-eval.js`).
`windows/test/bench.ps1` writes what a frame costs (below). Every `shot` of a `local.ps1` / `shot.ps1` run is
also drawn a second time whole and compared pixel for pixel with the frame the scene produced (`--verify-scene`;
the log says `scene == whole frame` or `SCENE MISMATCH`). `windows/test/pngdiff.py` compares two folders of PNGs.

Who drives the size, on both kinds of deck: `windows/test/driver.ps1` runs the exe against `driver-deck.js` twice
— the REAL `lib/phone.js` of this checkout (old words) and of the claim checkout (new words, with its
`lib/driverrule.js`; `-ClaimRoot`, default the sibling worktree `_wt-deck-remote-rule`, read-only) over a small
fake main — and checks the clicks, looks, tooltips, the sizes sent and every frame the server received (the wheel's
`kind`, `deskInit`'s name). `dump drive` is its script step. `windows/test/driver-e2e.ps1` is the same claim once
on that branch's real scratch deck (its own launcher: CDP 9778, port 8798), reading the deck's own window while the
exe holds the claim. The button's picture: `build-sidebar.ps1 -Truth` renders `claim-*.png` beside the branch's own
css / html (`fixtures/meta-claim-*.json`, made by `fixtures/make-claim.py`), `claim-montage.py` magnifies the
button in all seven states, `claim-words.js` compares the sentences with the renderer's.

The ask pane in the app: `windows/test/asks.ps1 -Name asks-walk -Script windows/test/asks-walk.txt` runs the exe
(`--window-shot`: the real window, parked off every screen, so it has the web view) as a desk of the fake deck of
`windows/web-test`, walks the pane (the tile, a card answered with a real click in the page, Done, F7, the
read-only views, an overlay over it, a resize, closing with it open), prints what the deck received and checks
that no `msedgewebview2.exe` and no profile folder stayed behind. `-Scratch` runs it on the scratch Electron deck
instead. The `ask …` steps are in `src/script.hpp`; `--do-file <path>` reads a script from a file. In a test run
the pane's Done is refused like every other call that changes something, unless `--test-asks` is given and the
deck is on 127.0.0.1. `windows/web-test/build.ps1` is the component suite (98 checks).

The cloud panes in the app: `windows/test/cloud.ps1 -Name cloud -Script windows/test/cloud-walk.txt` runs the exe as
a desk of the fake deck in its `cloud` mode (four cloud tiles, a 2 x 2 grid) with a LOCAL page standing in for
claude.ai (the fake deck's second server): one pane, three in a grid beside a terminal, a page typed into, the links
(in-site, another site, a new window, a sign-in popup), the sign-in hint, the palette and a menu over the pages, F5 /
F6 / F9 from a page, the reload and browser buttons, a resize, the ask pane over them, minimise, the close after two minutes off screen.
`-StateDir <folder>` twice shows a sign-in surviving a restart; `-Shot` is the off-screen run (no web view: the
card); `-Dpi 144` the same at 150 %; `-Runtime <a folder that is no runtime>` a machine without WebView2. The dialog:
`cloud.ps1 -Name cdlg -Shot -Script windows/test/cloud-dialog.txt` (its Start is BLOCKED and logged), and
`cloud-dialog-truth.ps1` renders the real css + js for the same state. Animations: `cloud.ps1 -Name anim -Shot -Script
windows/test/anim.txt` (`anim <ms>` sets the clock; every shot is checked against a whole redraw). Fonts:
`local.ps1 -Extra '--fonts-dir "<repo>\assets\fonts"'` with `call saveSettings [{'fontFamily':'JetBrains Mono'}]`
and `dump fonts`. The ask pane's Discard, a legacy page, a replaced page, minimise and a deck switch:
`asks.ps1 -Mode legacy -Script windows/test/asks-more.txt`.

What the web views cost: `windows/test/webmem.ps1` walks the exe through eight states against the fake deck (asks
waiting and the pane closed; the ask pane open; closed past its grace; one cloud pane; four; the ask pane over them;
all unplaced past their grace; one built again) and prints, for each, the count and the summed working set / private
bytes of the `msedgewebview2.exe` processes that DESCEND from the exe (the exe walks its own process tree; other
programs' web views are never counted). It fails unless there is NO such process while nothing web is on screen and
ONE browser process tree while something is; then it ends a second run as a crash with the ask pane open and
searches the state folder for the deck's token and its cookie's name (none may be found; a control proves the
search sees a stored cookie). `-Flags "<browser arguments>"` measures a flag. About two minutes; the graces are
shortened with the test-only `--web-grace <ask ms>,<cloud ms>`. WEB-NOTES.md "Memory" has the numbers.

Remote mode against a scratch deck:

```
powershell -File windows/test/scratch-deck.ps1 start      # a scratch Electron deck: CDP 9777, phone 127.0.0.1:8799
$env:CLAUDE_DECK_LINK = 'http://127.0.0.1:8799/?t=nativetest00000001'
windows\build\ClaudeDeckNative.exe --no-save --shot out.png --after 3000 --size 1400x900
powershell -File windows/test/scratch-deck.ps1 stop
```

`windows/test/shot.ps1 -Name x -Do "zone tile-close sel; shot x-confirm"` drives the exe through scripted clicks and
keys (grammar at the top of `src/script.hpp`) and writes `windows/test/out/n-x.png` plus a log of every call sent. In
a headless run the calls that would change something outside the test (adopt, move, save settings, restart) are
refused and logged as `BLOCKED`. `windows/test/build-sidebar.ps1 -Truth` compares the drawn sidebar and panels with
the real css + scripts rendered in headless Edge.

`--shot` runs with no window, connects as a desk, saves one frame, exits. `--probe <png>` is the read-only look
allowed at a live deck: it never registers as a desk — it reads the hello, attaches one tile nobody is showing,
saves the frame and a listing, detaches. Never use the live deck's ports (8788, CDP 9333) in a test.
The link comes from the `CLAUDE_DECK_LINK` environment variable so a token is never on a command line.

## What a frame costs

`--bench` draws the sixty-tile fixture in the sidebar and four panes of text (2 × 2), 200 frames a case, on the
off-screen bitmap and on the real window target (parked, presenting without waiting for the refresh). 2026-11-18,
1400 × 900 at 100 %, ms per frame on the window target, drawing + present:

| Case | before | now |
|---|---|---|
| nothing changed | 13.1 – 16.2 | **0.18 – 0.22** (one blit) |
| cursor blink | 14.5 | 0.21 – 0.36 |
| one line appended to one pane | 14.5 | 0.57 – 0.76 |
| full-screen update of one pane | 13.8 | **0.55 – 0.61** |
| all four panes update fully | 13.8 | 1.4 – 1.7 |
| the sidebar: a hover moves | 14.1 | **1.41** (p95 2.3) |
| the sidebar: a scroll | 14.1 | **1.34** (p95 2.1) |
| the sidebar: a new meta (the clock moved, one tile changed) | 14.1 | **1.29** (p95 1.9) |
| animations on, nothing animating (no timer runs) | — | 0.14 – 0.26 (the idle blit) |
| an animation tick: one ultracode tile pulsing and the asks tile (2 parts) | — | 0.57 – 0.73 |
| an animation tick: pulse + max drift + attention + the restart icon (5 parts) | — | 1.1 – 1.4 |

(The three animation rows were measured later the same night with other builds running on the box: the same run gave
1.6 – 2.1 ms for the three sidebar rows above, on the build from before the animations too. A tick comes 30 times a
second, and only while something that animates is on screen.)

"before" is the frame as it was (everything laid out and drawn every time); the bench still measures it as its
last case, with everything kept thrown away each frame. The sidebar's figures are for the frame alone: a new
meta also costs its `applyMeta` (the JSON), which the bench does not time.

The sidebar was the last cost (11.5 – 14 ms whenever its inputs changed). It draws only the tiles in view, as it
did; what made it slow was asking DirectWrite and Direct2D for the same things every frame. Now every text width,
every laid-out line and every wrapped block is kept under its inputs (`sidebar.cpp` "the sidebar's text, kept":
the idea of 30-tiles.js's `sig` / `actionSig`, one level down — a tile whose inputs did not change lays nothing
out), an icon's geometry is made once, and a usage bar's gradient brush is kept per target (`gfx.hpp` `gradient`;
making those again was most of what a tile cost to draw).

How: `gfx.hpp` "the scene". The panes and the sidebar are painted into a bitmap the size of the window that
lives between frames. A frame paints into it only what changed — a pane's damaged rows (`vt::Terminal::takeDamage`)
plus the rows the cursor left and entered, a whole pane when anything else about it changed (`App::paneKey`), the
sidebar when anything it is drawn from changed (`App::sidebarSig`: the meta's generation and every field of
`ui::State`) — and then blits it and draws the overlays (menus, panels, notes, the drop preview) over it. A
terminal run's `IDWriteTextLayout` is kept by text + font, so a repaint lays nothing out again. Pane and sidebar
rectangles sit on device pixels (whole DIPs at 100 % and 200 %, so nothing moved there), which is what lets a
part be repainted alone and come out exactly as a whole frame would: `--verify-scene` checks that on every shot.
One paint per display refresh (the window target presents on the refresh; invalidations coalesce into one
`WM_PAINT`), and a flood of output cannot starve the paint (`App::paintIfStarved`: 50 ms).
