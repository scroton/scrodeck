# Claude Deck — native core (`core/`)

This directory is the start of the native port's **shared C++ core** (docs/NATIVE-PORT.md §2, §4, R11).
It is being written *before* the R11 restructure, so it lives alongside the Electron tree as new files and
moves nothing — when build-order step 6 does the `git mv` into `electron/ core/ windows/ mac/`, this
folder is already in place.

**Why this exists at all:** the Electron app keeps its rules in pure `lib/*` modules with unit tests
(`layout`, `hibernate`, `triggers`, `queue`). Those modules are the specification. The core's job is to
hold the *same* rules in C++ so the C++ Windows shell and the Swift Mac shell share one implementation of
placement, hibernation policy, triggers and the special-prompt schedule — rather than each rebuilding them
(the "recurring tax", §7). The rules are frozen and test-covered, so the port is mechanical and verifiable.

## What is here now

| Path | What |
|---|---|
| `model/*.hpp` + `model/*.cpp` | the four model modules (layout, hibernate, prompts, triggers), **implemented and tested** |
| `vt/` | the terminal engine: VT parser + screen, scrollback, reflow, damage — a port of xterm.js 6's `src/common`, **tested cell by cell against headless xterm**; the text a selection copies (`Terminal::selectionText`, xterm's `SelectionService` rules, checked range by range against xterm.js) and a selection that follows its text when a full-screen app redraws (`vt/selection.hpp`) — what both shells copy |
| `proto/frames.hpp` | the core⇄shell frames of `PROTOCOL.md` as one codec (header-only) |
| `input/keys.hpp` | THE key table of every shell (header-only): a platform-neutral `Key` + modifiers → the bytes xterm.js sends, the paste encoding, and which presses are the window's (`windowKeyOf` / `webKeyOf`, the Windows function keys or the Mac's Cmd map). A shell maps its own key codes to `Key` (`windows/src/keys.hpp`: `VK_*`; the Mac: `DB_KEY_*`, the same numbers) |
| `input/mouse.hpp` | a mouse event → the report xterm.js sends (`CoreMouseService`, header-only; LOCAL mode only — R8) |
| `deck/` | the deck itself (multi-client, the phone / desk server, every rule of `main.js` and `lib/`), pure over the seams of `deck/seams.hpp`; `deck/options.*` (one options builder + the shipping defaults) and `deck/hosting.*` (the inbox, the pump, windows as clients) are how EVERY shell holds one — see `deck/README.md` |
| `third_party/nlohmann/` | JSON for Modern C++ 3.11.3 (MIT) |
| `third_party/miniz-tinfl/` | miniz's `tinfl.c` v1.11 (public domain, the Unlicense; unmodified): the inflater of the exe's own WebSocket client (`windows/src/wsraw.hpp`, permessage-deflate) |
| `test/run.cpp` | the model suite — the compiled proof the port matches the JS |
| `test/vt/` | the engine's suite: 184 byte streams (hand-written, real ConPTY captures, a synthesized Claude Code session, seeded fuzz) replayed whole, byte by byte and in random chunks; `oracle/` regenerates the expected grids from `@xterm/headless` 6.0.0 |
| `test/proto/` | the codec's suite; its R8 vectors are generated from `renderer/desk/bridge.js` |
| `test/input/` | the key table (the Windows shell's unit checks, the Mac draft's bridge rows, and every key × modifier set × platform against the port of `lib/platform.js keyAction`) and the mouse encoder (762 vectors from xterm.js's own service; `-Regen`) |
| `test/build.ps1`, `CMakeLists.txt` | reproducible builds |
| `test/gen-fixtures.js` + `test/fixtures/*.json` | language-neutral acceptance vectors generated from the JS |
| `PROTOCOL.md` | the core⇄shell message protocol (§4): async, serialisable, request/event |
| `PORTING.md` | the module-by-module port map + status |

## Build & test

A WinLibs **MinGW-w64 g++ 16.1** is installed on the build box (via winget:
`BrechtSanders.WinLibs.POSIX.UCRT`). Everything passes:

```
powershell -File core/test/build.ps1          # the model            -> all passed
powershell -File core/test/vt/build.ps1       # the terminal engine  -> 184/184 fixtures x 3 chunkings (-Bench, -Stress, -Regen, -Only <name>)
powershell -File core/test/proto/build.ps1    # the frame codec      -> 60/60
powershell -File core/test/input/build.ps1    # keys + mouse         -> keys 1221, mouse 762 (-Only keys|mouse, -Regen)
# or, with CMake (all three, as ctest tests):
cmake -S core -B core/build -G Ninja && cmake --build core/build && ctest --test-dir core/build
```

`core/test/run.cpp` encodes the same input→output cases as the JS unit tests (and the JSON fixtures),
calls the C++ functions, and asserts. The fixtures in `test/fixtures/` remain the language-neutral oracle
for a future Swift port and for cross-checking; the C++ runner is the compiled proof that the port matches.

The terminal engine's oracle is xterm.js itself, with the options the deck passes (scrollback 10000,
`windowsPty` conpty build 26200 — which turns reflow ON for the normal buffer). Its known, deliberate
differences from xterm.js (all of them xterm bugs that garble content) are listed at the bottom of
`vt/terminal.cpp`. Emoji are one cell wide, as in the deck: xterm 6 ships Unicode V6 widths and the deck
loads no unicode11 addon.

## The porting rule

1. The JS module in `lib/` is the source of truth. Change behaviour there (with its unit test), never
   here first.
2. Regenerate fixtures: `node core/test/gen-fixtures.js`.
3. Port / update the C++ to make the fixtures pass. The C++ must not "improve" on the JS — parity is the
   whole point (a ruling written against the app must hold identically in the shells).

## Boundaries (what the core is NOT)

The core is pure model + policy + the terminal's screen state. It does **not** own: the pty, the filesystem,
the window system, the network, or the render loop. Those belong to the shells (Windows/Swift) and reach the core only as
messages (`PROTOCOL.md`). Keeping the core free of I/O is what lets it be unit-tested off a fixture file
with no deck around it — the same property `lib/*` has today.
