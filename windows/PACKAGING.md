# Claude Deck Native — packaging (`windows/package/`)

So the native deck can be put on a PC and run as the deck **without a checkout**: one folder, one zip, a per-user
installer that needs no admin, and a one-time import of the Electron deck's state. Written 2026-11-19 against HEAD
`c28ae30`; everything below marked *tested* was run by `windows/package/test-package.ps1` (85 checks then; 95 with the update of a running deck added on 2026-11-19, all passing).

| File | What |
|---|---|
| `package/make-package.ps1` | builds (or takes) the exe, assembles `package/out/ClaudeDeckNative-<version>/`, zips it, prints size + sha256 |
| `package/install.ps1` | copies a package to a program folder, writes the Start-menu shortcut with the AppUserModelID, optional auto-start. Ships inside the package |
| `package/uninstall.ps1` | removes exactly what `install.ps1` recorded. Ships inside the package |
| `package/import-from-electron.ps1` | one-time copy of the Electron deck's state files into the native deck's folder. Ships inside the package |
| `package/setup.ps1` | the first run: the accounts and their labels first (when the deck has no settings), then "set up the rest with Claude Code" (below). Ships inside the package; `install.ps1 -Setup` runs it |
| `../setup/claude-setup-prompt.md` | the prompt `setup.ps1` (and the Mac's `mac/tools/open-setup-session.sh`) hands to the Claude Code session. Ships as `setup\claude-setup-prompt.md` |
| `package/ShortcutTools.ps1` | the `.lnk` writer / reader (IShellLink + IPropertyStore through a small C# `Add-Type`), shared by the three above |
| `package/test-package.ps1` | the whole thing end to end, installing only into temp folders |
| `package/out/` | git-ignored (`out/.gitignore`) |

## The short answer on relocation

**The exe is already relocatable; nothing has to change in it for the package to work.** *Tested*: the installed exe,
started from a temp folder with **no `--assets` and no `--fonts-dir`**, served every page from beside itself and
loaded the seven font families from `assets\fonts` beside itself. The two lookups that make it so:

- fonts — `app.cpp` `loadBundledFonts`: `<exe folder>\assets\fonts` (after `--fonts-dir` and `config.json` `fontsDir`;
  since `255673b` the fonts are inside the exe itself and these folders are the fallback);
- served pages — `deckhost.hpp`: the asset root is the first of the exe's own folder and the five above it that holds
  `renderer\phone\index.html`. The exe's own folder is tried first, so a package wins over anything above it.

It writes nothing beside itself (*tested*: the program folder is byte-identical after a run), so the folder can be
read-only.

## What the package contains, and why

`ClaudeDeckNative-0.1.0\` — 83 files, 9.6 MB unpacked; the zip is **4.2 MB** (2026-11-19, the working tree's build;
9.18 MB / 3.99 MB with the build this file was first written against).

| Path | Files | Bytes | Why it is there |
|---|---|---|---|
| `ClaudeDeckNative.exe` | 1 | 5,816,320 | the deck: static, GUI subsystem, no runtime |
| `assets\fonts\**` | 25 | 2,999,243 | the seven bundled console fonts, `fonts.css`, `families.json` and each family's licence. Read by the exe from their files, and served to a browser desk (`/desk/assets/fonts/…`) |
| `renderer\phone\*` | 7 | — | the phone page: `index.html`, `app.js`, `style.css`, `sw.js`, `manifest.webmanifest`, the two icons (the server serves any file below this folder, and fingerprints four of them) |
| `renderer\index.html`, `style.css`, `js\*.js` (20), `desk\bridge.js` | 23 | — | the browser desk: exactly what `core/deck/server.cpp` `deskFileAllowed` lets through. `renderer\` is 614,607 bytes in all |
| `lib\layout.js` | 1 | 13,733 | the one `lib` file the desk page loads |
| `node_modules\@xterm\xterm\lib\xterm.js`, `…\css\xterm.css`, `@xterm\addon-fit\lib\addon-fit.js` + each package's `LICENSE` | 5 | 499,660 | the vendor files, at the paths the routes read (`/vendor/*` and `/desk/node_modules/@xterm/…`). Copied out of `node_modules`; no source maps, no `.mjs` |
| `LICENSES\` | 16 | 46,464 | `THIRD-PARTY.txt` (the index), xterm.js, addon-fit, nlohmann/json, the WebView2 SDK licence + notice + version, the webview/webview loader licence, `fonts\LICENSES.md` and every family's OFL / UFL |
| `README.txt` | 1 | 3,131 | start it as the deck (`--local`), as a remote (paste the link), where state lives, how to uninstall |
| `install.ps1`, `uninstall.ps1`, `import-from-electron.ps1`, `ShortcutTools.ps1` | 4 | 24,518 | so an unzipped package can install itself |
| `setup.ps1`, `setup\claude-setup-prompt.md` | 2 | | the first run ("First run" below), added 2026-11-23 |
| `skills\deck-triggers\SKILL.md`, `scripts\deck-trigger.js`, `scripts\README.md` | 3 | | a companion the setup session copies into each account's `skills\` (no tests), added 2026-11-23 |

**Not in it**, on purpose (*tested*): `main.js`, `preload.js`, `package.json`, any other `lib` file, source maps,
tests, docs, `assets\icon.*` (the icon is in the exe). `make-package.ps1` also fails when `renderer\index.html` names a
script or stylesheet that did not get packaged, so a new renderer file cannot be forgotten silently — and `js\*.js`
is taken by pattern, so a new one comes along by itself.

`-IncludeUserswitch` adds `bin\userswitch` (the account-move script; `core/deck/platform.cpp` looks in
`~\.local\bin` first, then `<asset root>\bin`). Off by default: the owner's box has it in `~\.local\bin`, and it
needs Git Bash either way. A PC that should move sessions between accounts and has no `~\.local\bin\userswitch`
needs the switch.

Two honest gaps in `LICENSES\`:
- the exe is linked statically against the WinLibs MinGW-w64 runtime, libstdc++, libgcc and winpthreads. Their licence
  texts are **not in the tree**, so the package names them in `THIRD-PARTY.txt` and points at the compiler's own copy
  instead of shipping them. If the zip is ever given to someone outside, copy those texts in (WinLibs:
  `mingw64\share\licenses`).
- the repository has no licence file of its own for the deck, so the package carries none.

The zip is not byte-reproducible (the generated README and index get a new time stamp each run), so the sha256
printed is of *that* zip.

## Make

```
powershell -NoProfile -ExecutionPolicy Bypass -File windows/package/make-package.ps1 [-Exe <path>] [-Version 0.1.0]
```

- Without `-Exe` it runs `windows/build.ps1` first and takes `windows\build\ClaudeDeckNative.exe`. With `-Exe`
  nothing is compiled. **That no-`-Exe` path was not exercised tonight** (other workers had the tree's build folder);
  the tests passed a private build of HEAD (`git archive HEAD … | tar -x` into a scratch folder, `build.ps1` there —
  it built clean, 4,980,224 bytes, the same size as the tree's own exe).
- The version is the exe's version resource (`FileVersion`: `0.1.0` today, from `windows/res/app.rc`) or `-Version`.
- `Compress-Archive` makes the zip; the script then counts the files inside it and rewrites it with
  `System.IO.Compression` if any is missing (font files are named like `FiraCode[wght].ttf`, and `Compress-Archive`
  has mishandled brackets before). Tonight it kept all 83.

## Install / uninstall

```
# from an unzipped package
powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-AutoStart] [-Mode local|ask]
powershell -NoProfile -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\Programs\ClaudeDeckNative\uninstall.ps1"
```

Per user, no admin, **no registry**. `install.ps1`:

1. copies the package to `-Destination` (default `%LOCALAPPDATA%\Programs\ClaudeDeckNative`). It refuses a
   non-empty folder it did not make (no `install-manifest.json`) unless `-Force`. Run from a newer package it
   updates in place and removes files the older version had and the newer one does not — also while the deck is
   RUNNING from there: the running exe is moved aside (`ClaudeDeckNative.exe.old`), the new one copied in, and the
   running deck offers the restart ("Requests", 5).
2. writes `Claude Deck Native.lnk` in `-StartMenuDir` (default: the user's Start menu `Programs`) pointing **at the
   exe**, with `System.AppUserModel.ID = demo.claude-deck-native`. Both strings are `notify.hpp`'s `kAppId` /
   `kShortcutName` (the test reads them out of the header and compares), which is what makes Windows show the
   deck's toasts. `-Mode local` (default) gives the shortcut `--local`: it is the deck of this PC, no question.
   `-Mode ask` gives it no argument: the exe asks once (this PC, or paste a link). `-NoShortcut` skips it.
3. `-AutoStart`: the same shortcut in `-StartupDir` (default: the user's Startup folder).
4. writes `install-manifest.json` (files, shortcuts) in the destination.
5. `-Setup`: runs `setup.ps1` from the destination in the same window ("First run" below), handing it every argument
   `install.ps1` does not know (`-StateDir`, `-NoStart`, ...). Without it, the last line printed says how to run it.

`uninstall.ps1` reads that manifest and removes the listed shortcuts (only one that still points at this install's
exe — a shortcut the user re-pointed stays), the listed files, the folders left empty, the manifest. A file the
installer did not put there stays, and the folder with it (*tested*). **It never removes state**:
`%APPDATA%\claude-deck-native` stays, and the script says so. Nothing of the Electron deck is touched by either.

One thing to know: on a real run the exe itself makes sure `Claude Deck Native.lnk` exists in the *real* Start menu
(`notify.hpp` `ensureShortcut`). When the installer's shortcut is already there and points at this exe, the exe
keeps its arguments (`--local`) and only re-stamps the id. So an unzip-and-run without `install.ps1` also ends up
with a Start-menu entry — one that `uninstall.ps1` knows nothing about (README.txt says to delete it by hand).

## First run: `setup.ps1` - the accounts first, then the rest with Claude Code (v1, 2026-11-23)

```
powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Setup        # install, then this
powershell -NoProfile -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\Programs\ClaudeDeckNative\setup.ps1" [-Report]
```

Per user and **no admin**; it writes nothing outside the deck's state folder (`-StateDir`, default
`%APPDATA%\claude-deck-native\local`). In order:

0. **an earlier run's report**: `<state>\setup\setup-report.md` (the Claude session writes it): its `STATUS` line and
   its "For you to do" list are printed. `-Report` prints the whole of it.
1. **The critical settings - only when the state folder has no `settings.json`**: the accounts and their short
   labels (vv / ms / ts). Found ones are offered first - another deck's `settings.json` `aliases` (`-OtherStateDirs`,
   default the Electron deck's `%APPDATA%\claude-deck`) and `~\.claude-code-account-aliases` with each folder's
   signed-in e-mail (`.claude.json`, read only) - with "import" as the third answer (stop, and bring all of the
   Electron deck's state over with `import-from-electron.ps1` first). Otherwise, or as well, it asks: login e-mail,
   label, config folder (default `~\.claude-<label>`), keyword (default `claude<label>`). Refused: a label that is not
   1-8 letters or digits, a keyword outside `[A-Za-z0-9_]` or plain `claude`, `~\.claude` itself as a folder (the deck
   drops it once named accounts exist), duplicates. Written as `settings.json`
   `{ "aliases": { "<e-mail>": "<label>", "<keyword>": "<label>" } }`: the deck finds a label by the signed-in e-mail
   (`refreshAccounts`), before the sign-in by the account's keyword, and merges the file over its defaults. Not
   written while that deck runs. It does NOT write the alias file, make the folders or sign in: that is the session's
   step 2 (the native deck's own Accounts panel cannot write the alias file yet: `allowAccountEdit` is off).
2. **Claude Code** missing: Anthropic's per-user installer (`irm https://claude.ai/install.ps1 | iex`, no admin) is
   offered and run in the same window.
3. **Tailscale** missing: the ONE step that may need admin. It says why first (its installer is machine-wide), then on
   a yes runs `winget install --id Tailscale.Tailscale -e --silent ...` alone, through `Start-Process -Verb RunAs
   -WindowStyle Hidden`: one UAC prompt, no console. No winget, declined, or failed: the session walks the user
   through it (the prompt says which).
4. **"Set up the rest with Claude Code"**: Claude in Chrome is asked about - it is a browser extension, so the
   installer says it cannot install it and opens its Web Store page on a yes; then the ask mode ("only when
   necessary", the default, or "as you go"). The prompt is filled in from `setup\claude-setup-prompt.md` into
   `<state>\setup\claude-setup-prompt.md` (the ASK MODE line set; the permission block dropped for "as you go"; the
   deck, the folders, the accounts and what the installer did / could not do filled in). The command line,
   `claude [--chrome] "Read the file '<that file>' and do what it says. ..."`, is shown in an editable box (with an
   "Open the prompt" button: Notepad) and started in a **normal, visible PowerShell window** in the home folder, never
   elevated, with no Claude session's markers in its environment (`CLAUDECODE*`, `CLAUDE_*`, `AI_AGENT`, as
   `core/deck` `baseEnv` drops them). `-EncodedCommand` carries it, so no quoting is lost on the way. The other
   answers: "paste" (the file's path and the line to paste into a session the user opens) or "later".
5. `<state>\setup\setup-installer.json`: what it found, chose and did.

`-NoStart` starts, opens and elevates nothing (the files are written, the plan printed); `-Defaults` answers every
question no parameter answers with its default; `-NoGui` asks for the command line in the console. The tests use
`-NoStart -Defaults` with every folder in TEMP and stand-in `claude` / `winget` files that are never run.

**What the installer cannot do**, said on screen and in the prompt: install the Claude in Chrome extension (that is the
user's click in the Web Store); sign in to any account; anything admin besides the one Tailscale step.

**The prompt** (`setup/claude-setup-prompt.md`, one file): the switch line `ASK MODE:` at the top; its first lines say
in plain words what it has Claude do. The permission block ("only when necessary") lists what the session may do
without asking - create the named account folders, write the alias file and the profile functions (backups first),
open a sign-in window per account, write the labels while the deck is closed, copy the companion files (backups
first), read-only checks, open web pages - and what it must ask first: anything admin, installing any program or
downloading anything to run, anything outward (sending, posting, signing up, pushing, a web account's settings). The
rules for both modes: delete nothing, only ADD in `~/.claude*`, never an account's `settings.json` / `.claude.json` /
`.credentials.json`, no token or password shown or written, never `tailscale funnel`. It depends on no private skill.
The report's shape is fixed (`STATUS:` line, "For you to do" list) so the installer can read it back.

The `deck-triggers` skill's script writes to the **Electron** deck's trigger inbox unless `CLAUDE_DECK_USERDATA` names
the native deck's state folder. Since 2026-11-23 the native deck sets it for every tile it starts, so only a session run
outside the deck (a plain terminal) writes to the Electron deck's inbox; the session says so and does not set the
variable without asking.

The Mac counterpart is `mac/tools/open-setup-session.sh` (unrun on a Mac; `docs/INSTALL.md` section 7).

## What auto-start does

**The Electron deck has no auto-start**: `main.js` never calls `setLoginItemSettings`, `docs/INSTALL.md` and the NSIS
installer set none up. What exists on the owner's box is a *keep-alive* (a scheduled task, box-level, not in the repo,
set up 2026-09-11) that relaunches the Electron deck after a crash while its `running.json` is there.
So `-AutoStart` is **off by default**, mirroring that.

When asked for, it is a shortcut in the Startup folder whose target is `ClaudeDeckNative.exe --local` — the GUI exe
itself, never `cmd`, PowerShell or `wscript`, so no console can flash (*tested*: the target is read back and compared
with the installed exe). A Startup-folder shortcut rather than a `Run` registry value, so that install and uninstall
stay file-only and every destination can be pointed at a temp folder in a test. At sign-in the deck restores its
tiles as it does on any start; a second launch while it runs brings the first forward (the single-instance lock).

The keep-alive is not packaged. The native deck writes the same marker (`%APPDATA%\claude-deck-native\local\
running.json`, `{ pid, at }`, removed on a clean quit), so the owner's existing task could be pointed at it — their
call, on their box; see "For the owner".

## Tests

```
powershell -NoProfile -ExecutionPolicy Bypass -File windows/package/test-package.ps1 [-Exe <path>] [-Work <temp>] [-Keep]
```

About three minutes with `-Exe`. 95 checks, all passing on 2026-11-19 with the build of the working tree (the
first 85 with the HEAD build before it; step 3c is the ten added that day). **136 on 2026-11-23** with an exe of main
(steps 2b and 2c added, the first-run setup; step 3b runs only when step 3 fails, so it does not count). The same day
step 3's font check was brought up to date: since `255673b` the exe loads the fonts from inside itself and the folder
is the fallback, so "from inside the exe" passes too (the check had failed on main since then):

1. **the package**: only the listed top-level entries; `lib\` is `layout.js` alone; `node_modules\` is five files; no
   `main.js` / `preload.js` / `package.json` / map / test / doc; licences and README present; fonts whole.
2. **the zip → install into temp folders** (a fake Programs folder, a fake Start menu, a fake Startup folder,
   `-AutoStart`): 83 files unpack (bracketed font names too); both shortcuts read back — target is the installed
   exe, arguments `--local`, the AppUserModelID is `notify.hpp`'s; installing again over it works.
3. **the installed exe, headless** (`--shot`: no window; `--local --serve 8813 --state-dir <temp> --home <temp>
   --no-claude`, no `--assets`, no `--fonts-dir`): 127.0.0.1 only; `/` with the token = the phone page and the cookie;
   `/` and `/desk/` without it = **401**; `/desk/` with its `<base>` and bridge; `app.js`, `style.css`, `sw.js`; the
   three `/vendor/` files; the desk's `style.css`, a `js` file, `bridge.js`, `lib/layout.js`, xterm under
   `/desk/node_modules/`; `fonts.css` and `JetBrainsMono[wght].ttf` (`font/ttf`); the manifest and both icons
   **without** the token; every `src` / `href` the desk page names answers 200; `/desk/main.js` and
   `/desk/install.ps1` are 404 and the phone route cannot climb out to the package's scripts; each body's length is
   the installed file's. The exe's log says `bundled fonts: 7 families from <installed>\assets\fonts`. Exit 0, port
   free, no child process, nothing written into the program folder, no shortcut made by the run.
3c. **a running deck updated in place**: the installed exe running headless, a "newer" package (the same files, the
   exe's time stamp moved) installed over it: exit 0, `ClaudeDeckNative.exe.old` there, the new exe in place and not
   locked, the deck still running, its meta offering the update, the manifest not listing the `.old`; the next
   start removes the `.old`, and the folder is the package and its manifest again.
4. **the import** on synthetic files (below), including the native deck booting on the imported state.
5. **uninstall**: fake Start menu and Startup empty; only the foreign file left in the program folder; the state
   folder untouched; a clean second cycle (`-Mode ask`, no auto-start) leaves nothing; uninstalling nothing fails.
2b. **`setup.ps1`** from the installed folder, `-NoStart -Defaults`, every folder in TEMP (a fake home with an alias
   file and a signed-in `.claude.json`, a fake other deck's `settings.json`, stand-in `claude.exe` / `winget.exe` files
   that are never run): the prompt and the skill are in the package; found accounts become exactly `{ aliases }`; the
   prompt is filled in (no placeholder left; the switch line and the permission block per ask mode; the accounts
   table; the report path); the command line for each choice (`--chrome` or not; terminal / paste / later); the
   window's encoded script, run once with a stand-in `claude` FUNCTION, passes exactly `--chrome` and the pointer and
   runs in the home folder; nothing started; the Tailscale step planned elevated and not run; a deck with settings
   left alone; a running deck's settings not written; refused labels and folders; "import"; Claude Code missing; an
   earlier report read back and `-Report`; `install.ps1 -Setup` handing its arguments on.
2c. **`mac/tools/open-setup-session.sh --dry-run` under Git Bash** (skipped without Git Bash): the same accounts,
   prompt and command checks for its plain-bash part. Its dialogs and the Terminal hand-over are NOT tested.
6. **the real PC**: every file under the real Start menu and Startup folder (names and times), the entries of
   `%LOCALAPPDATA%\Programs`, the `HKCU …\Run` values and the top level of `%APPDATA%\claude-deck-native` are listed
   before and after — unchanged; no `ClaudeDeckNative` under Programs; no test exe left running.

If a future exe stops finding its files beside itself, step 3 fails on the two `RELOCATABLE` lines and runs again
with `--assets` and `--fonts-dir` given, so the report says which half broke.

Not tested, because a test may not do it: a real install into the real Start menu, a real toast through the
installed shortcut, a real sign-in start. Those are the owner's first run.

## Switching from the Electron deck: the state

| | Electron deck | Native deck |
|---|---|---|
| folder | `%APPDATA%\claude-deck\` | `%APPDATA%\claude-deck-native\local\` (and `…\claude-deck-native\config.json` for the shell: mode, remembered link, window layouts of a remote) |
| files | `tiles.json`, `settings.json`, `triggers.json`, `held-prompts.json`, `usage-cache.json`, `ask-drafts.json`, `running.json`, `relaunch.json`, `uploads\`, `trigger-inbox\`, `desk-open\`, `tiles-backups\` | the same names, written by `core/deck` |
| accounts | `~\.claude-code-account-aliases` + each account's own config dir | **the same file and the same dirs** — nothing to migrate |

**The file shapes are the same**, because `core/deck` is a port of `main.js` and reads what it wrote: `restore()`
takes `tiles.json`'s `{ tiles: [{ id, kind, account, sessionId, cwd, pinned, group, createdAt, title, userTitle,
asleep, sleptAt, browser, desk?, cloudId? }], groups, windows: [{ bounds, layout }], deskLayout }` (and the older
single `bounds` / `layout`), `settings.json` is merged over the same defaults, and `HOST-NOTES.md` lists the
persistence shapes as ported. The one textual difference: the native deck writes its keys in alphabetical order
(HOST-NOTES difference 11). So the migration is a **copy**: `import-from-electron.ps1`.

What the import does and refuses (*tested*, on synthetic files in temp folders only — **it has never been run on a
real state folder**, and both folders are mandatory parameters with no default so it cannot be by accident):

- copies `tiles.json`, `settings.json`, `triggers.json`, `held-prompts.json`, `usage-cache.json`, `ask-drafts.json`
  (those that exist), byte for byte; writes a receipt, `imported-from-electron.json`;
- opens the source read-only and writes nothing there (the test compares names, sizes, times and hashes of the
  whole source folder before and after); refuses a destination inside the source and the reverse;
- **never** imports `relaunch.json` (it names the Electron launcher: the native deck's "restart" would start the
  Electron deck), `running.json`, `uploads\`, `trigger-inbox\`, backups — refused even when named with `-Files`;
- refuses while the source deck runs (its `running.json` names a live process) and while the destination deck runs;
- refuses when the destination already has one of the files, unless `-Force`, which first moves what is there to
  `import-backup-<time>\`;
- one file that does not parse stops the whole import before anything is written;
- `-PhoneOff` sets `phone.enabled` to `false` in the copy and changes nothing else (the text is edited in place, not
  re-serialised); `-DryRun` only says what it would do.

The test then starts the native exe headless on the imported folder (Claude off, a temp home whose alias file names
the tile's account): it lists the imported tile with its title.

Three things the copy does **not** settle — read before the real one:

1. **A tile whose account the native deck does not know is dropped at boot** (`restore()`: `!acctById(account)` →
   skipped), and the next save writes `tiles.json` without it. Seen in the test: with no alias file in the home, the
   imported tile was gone. On the owner's box both decks read the same alias file, so every account should be known —
   but if a tile is missing after the first boot, that is why; `local\tiles-backups\` and the untouched Electron
   folder both still have it.
2. **Booting on imported tiles resumes them.** As in the Electron deck, every Claude tile that was not asleep is
   started again at boot (`--resume`), when the deck may start Claude (a real run: yes). That is the point of the
   switch, and the reason both decks must be closed first: one session open in two decks is two processes on one
   transcript.
3. **The phone settings come along**: the same port (8788) and token, so the phone's link and the `tailscale serve`
   front door keep working with no change — and the two decks cannot both serve at once. `-PhoneOff` is for a trial
   with the Electron deck still up (but then mind point 2: import no tiles — `-Files settings.json`).

## For the owner: the switch, step by step

Before any of it: `windows/HOST-NOTES.md` "What still stands between the native deck and switching Electron off"
(the inventory of every main-process responsibility: done / partial / missing, and the rows marked **[OWNER]**) and
`windows/PARITY.md` — "Blockers the owner named for switching to the exe" (paste, scroll-up: pending your hand test)
and "Switching Electron off: what the shell still lacks". Nothing below makes those rows pass; it only moves the
program and the state.

1. Make the package (`make-package.ps1`), unzip it anywhere outside the repo.
2. **Try it beside the Electron deck first, importing nothing**: run `ClaudeDeckNative.exe --local` from the unzipped
   folder. It keeps its own state, its own toasts, its own Start-menu entry; it serves a phone only if you switch that on in ITS
   Settings (not on port 8788 while the Electron deck has it). Check paste, scroll-up, a real session, a toast.
3. When you want it to be the deck: **close the Electron deck** (all windows; wait until no `electron` process is
   left) and stop its keep-alive task so it does not come back mid-switch.
4. Install: `install.ps1` (add `-AutoStart` if you want it at sign-in).
5. Import, once, with both decks closed (see what it would do first with `-DryRun`):
   ```
   powershell -NoProfile -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\Programs\ClaudeDeckNative\import-from-electron.ps1" -Source "$env:APPDATA\claude-deck" -Destination "$env:APPDATA\claude-deck-native\local"
   ```
   If step 2 already made native state there it refuses; `-Force` replaces it after backing it up.
6. Start "Claude Deck Native" from the Start menu. The sessions that were awake resume; the phone link is the one it
   was. Check every tile is there (point 1 above).
7. Going back is starting the Electron deck again: its folder was only read. Do not run both on the same sessions.
8. Yours to decide afterwards: pointing the keep-alive task at the native deck's `running.json`; a
   `relaunch.json` in `%APPDATA%\claude-deck-native\` if you want restarts to go through a launcher of yours (the
   native deck restarts itself with its own arguments otherwise — no debug-port flag is involved or baked in
   anywhere here); removing the Electron deck's Start-menu entry.

## Requests to the exe's owners — answered 2026-11-19

Each was a request when this file was written; this is what the exe does now (`windows/src`, checked by
`windows/test/shell.ps1` `register` / `office` and step 3c of `test-package.ps1`).

1. **`--register` / `--unregister`: done.** `ClaudeDeckNative.exe --register [--shortcut-dir <folder>]
   [--shortcut-args "--local"]` runs `notify.hpp` `ensureShortcut` with that folder (default: the user's Start menu
   `Programs`) and exits: "created", "updated" (it pointed at this exe: its arguments are kept unless
   `--shortcut-args` names new ones) or "stamped" (it points somewhere else: only the id is written) = exit 0, anything
   else 1. `--unregister [--shortcut-dir <folder>]` removes the shortcut when it points at this exe (exit 0, also
   when none is there) and leaves one that points elsewhere (exit 2). Nothing is shown, no deck starts, no lock is
   taken, no state folder is read. With `--shortcut-dir` the outcome is also one line in `<folder>\register.log`.
   **`install.ps1` still writes its shortcuts with its own C# writer** (`ShortcutTools.ps1`): moving it over is a
   three-line change, not made here because `--register` with a folder leaves that `register.log` beside the
   shortcut, which the real Start menu should not get — drop the log (or give it a switch) first.
2. **A version in one place: done.** `core/deck/version.hpp` is included by `windows/res/app.rc` (`FILEVERSION`,
   `PRODUCTVERSION`, `FileVersion` `0.1.0`, `ProductVersion` `0.1.0-native`) and is what `app.cpp` hands the deck
   (`deck-state.json` `deck.version`). The package still takes the resource's `FileVersion`. (`res/app.manifest`'s
   `assemblyIdentity version` is still written by hand: a manifest cannot include a header.)
3. **Say so when the pages are not found: done.** `phoneStatus.assets` / `assetRoot` (the core's) are shown in
   Settings → Phone: "The pages are not beside the exe (renderer\phone\index.html): looked in <folder> …", in red
   under the state line.
4. **Do not look above a packaged exe: done.** With no `--assets` the app hands the deck ONE folder: the exe's own
   when it holds `renderer\phone\index.html`, else — only for an exe whose folder is a checkout's `windows\build` —
   that checkout's root, else the exe's own folder again (so "missing" names it). The host's five-levels-up search
   runs only when it is handed nothing, which the app never does. Tested: a `renderer\` two folders above a copied
   exe is not served.
5. **Updating a running deck: done ("rename-aside").** `install.ps1` no longer refuses while the deck runs from the
   destination: it moves the running exe to `ClaudeDeckNative.exe.old`, copies the new one in under the real name
   (a font or page file the running deck holds open is left when it is byte-identical; any other failure puts the
   old exe back and stops), and says the deck will offer the restart. The running deck's update offer
   (`Lifecycle::fingerprint`: the size and time of the file at its own path) sees the new exe within half a minute:
   "Deck update waiting: ClaudeDeckNative.exe". The exe deletes a stale `.old` at start, and again after
   `--wait-pid` (the restart's own hand-over); `uninstall.ps1` removes one that is still there. *Tested* (step 3c):
   a deck running from the temp destination, the update installed over it, `.old` there, the offer in the running
   deck's meta, the next start removing `.old`.
6. **`bin\userswitch`**: unchanged — `<asset root>\bin` is where a packaged one is looked for, and with request 4
   the asset root of an installed exe is its own folder, so `-IncludeUserswitch` lands where it is read.
