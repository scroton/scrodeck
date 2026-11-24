# Claude Deck — native port brief

**This file is self-contained.** Hand it to a fresh Claude with no other context. It carries the
architecture decision (2026-09-05), the seven features that must be designed in from the start, the
measured numbers that frame every claim, and the seven rulings — **all settled, §8**.

(Written 2026-09-05 as the brief that started the port, before any native code existed; the build has long since
moved on, and the notes beside the code — `core/`, `windows/`, `mac/` — say where it stands. The comments in the code
still cite this file's § numbers and R-rulings. §9's repo map is the private working repo's: most files it names are
not in the public copy.)

Repo: `C:\Users\demo\Documents\GitHub\claude-deck` — branch `main`, remote: the private working repo
(private). Status: **decision and specs only. Nothing native has been built.**

---

## 1. What Claude Deck is

One window hosting every Claude Code session on the box. A collapsible sidebar lists sessions as tiles
(account abbreviation, live status, usage bars, last assistant text); the selected tile's terminal fills
the rest of the window. Each session runs in its own ConPTY inside the app. Today: **Electron + xterm.js
+ node-pty**, ~3,854 lines of renderer JS and 31 KB of CSS. `README.md` in the repo is the full
behaviour spec; `docs/USER-GUIDE.md` is the user-facing one.

A **phone view** (`lib/phone.js` + `renderer/phone/**`) already serves a mobile UI over a websocket via
Tailscale, driving the same sessions.

---

## 2. The architecture decision (2026-09-05)

Three codebases over **two logic bases**:

| Piece | Language | Stack |
|---|---|---|
| **Shared core** | **C++**, C ABI | VT parse + grid, PTY, sessions/transcript/usage, session protocol server |
| **Windows shell** | **C++** | Win32 + Direct2D + DirectWrite, atlas renderer |
| **Mac shell** | **Swift** | AppKit + Metal + CoreText; imports the C++ core directly (Swift 5.9+ C++ interop, no bridging header) |
| **Phone view** | **JavaScript**, unchanged | browser client — no other option exists |
| **Electron (this repo)** | JS | stays the reference implementation and the shipping deck until a shell reaches parity |

**Core owns:** VT parse + grid + scrollback; PTY spawn/read/write/resize (ConPTY on Windows, `forkpty`
on Mac); `<configdir>/sessions/<pid>.json` (the live-status contract); transcript record shapes; usage
polling (once per account per 3 min, disk cache, 429 back-off); the websocket protocol server.
Existing specs for each: `lib/transcript.js`, `lib/usage.js`, `lib/sessions.js`, `lib/phone.js`,
`lib/platform.js`.

**Shells own:** window, input, menus, GPU rendering, and the sidebar/tile/bar/pane UI.

### Why these languages

- **Windows — C++ or Rust was a genuine tie.** The ceiling comes from DirectWrite + D3D11 with an atlas
  renderer, not the language; `windows-rs` is Microsoft's first-party COM binding with no wrapper
  penalty. C++ won for one reason: **Windows Terminal is MIT-licensed and its AtlasEngine can be lifted
  directly**, saving weeks on the hardest part. Choosing C++ here buys ~0% performance — never justify
  it on speed.
- **Mac — Swift is a real capability gap over Rust**, not a speed one: CoreText (Apple Color Emoji, the
  system fallback chain, Nerd Font ligatures, sub-pixel positioning), `NSTextInputClient` (marked text,
  dead keys, CJK candidates), and AppKit menu/Dock/`activate` semantics are free in Swift and
  hand-written `objc2` bindings in Rust.
- **Swift never gets the hot loop.** Idiomatic Swift parses VT 2–3× slower than C++/Rust/Zig — ARC
  retain/release plus COW checks on every byte of cell storage. That is exactly why the parser lives in
  the C++ core. Ghostty is the same shape (Zig core + Swift/AppKit/Metal shell) and is the proof.

### Rejected — do not re-litigate

| Option | Why not |
|---|---|
| **Rust for both desktops** | Correct while Windows was Rust; once Windows became C++ its only advantage (one source, two platforms) vanished, and Swift beats it on Mac |
| **Tauri** (Rust backend + this repo's JS/CSS) | WebView2 *is* Chromium on Windows → no frame-time gain, and it inherits the ~16-WebGL-context cap. Was the cheap middle rung; dropped once native was committed |
| **Zig core** (Ghostty's) | Marginally the fastest shipping parser, but forks the core from the C++ Windows shell → back to three logic bases. Not worth the last few percent |
| **C# NativeAOT + Avalonia** | No mature terminal widget — you write the VT parser anyway. Worst of both ends |
| **Go** | Its only GUI story is a webview, i.e. Tauri with a slower backend |
| **Objective-C, or anything below Metal** | Same runtime as Swift, no gain. Metal is the lowest public GPU API on macOS — nothing exists beneath it |

---

## 3. Features to design in from the start (2026-09-05)

These are **not** retrofits. Two of them change the core/shell boundary, so they belong in the design
before any code is written.

### F1 — Quadrant layout

The terminal area (`#main`, everything right of the sidebar) is a 2×2 grid:

```
┌─────────┬─────────┐
│    1    │    2    │      1 = top-left      2 = top-right
├─────────┼─────────┤      3 = bottom-left   4 = bottom-right
│    3    │    4    │
└─────────┴─────────┘
```

A session occupies a **rectangular** set of quadrants. The legal occupancies are:

| Occupancy | Set |
|---|---|
| Single quadrant | `{1}` `{2}` `{3}` `{4}` |
| Top / bottom half | `{1,2}` `{3,4}` |
| Left / right half | `{1,3}` `{2,4}` |
| Full | `{1,2,3,4}` |

`{1,4}` and `{2,3}` are diagonals and cannot be drawn as one rectangle. **Ruling R1: a diagonal chord
is promoted to the full window** — `1`+`4` and `2`+`3` both mean all four quadrants.

**Ruling R7: the layout persists across restarts** — quadrant occupancies and the second window (F5) are
restored alongside the existing tile restore.

Today `renderer/style.css:344` is `.term { position: absolute; inset: 0; display: none }` with
`.term.active { display: block }` — exactly one terminal visible, filling `#main`. Quadrants replace
that with computed insets per placed tile. **This is the cheapest possible change point**, which is why
prototyping in Electron first is the recommended build order (§5).

### F2 — Keyboard placement (chord over the digits)

With a tile selected, the user presses and holds digit keys; **on release of the last held key**, the
accumulated set is applied to the selected tile. Hold `1`+`2`, release → the session takes the top half.
Hold `1`+`3`, release → the left half. A single tap of `1` → the top-left quadrant.

Implementation shape: accumulate the set of digits held down; fire when the held set empties.

**Grids over nine cells (2026-10-05: the ceiling is 4×4).** Digits keep naming cells 1–9 in reading
order; the letter block `Q W E R / A S D F / Z X C V` joins the chord as grid rows 2–4 by physical key
(scan codes, not characters), only when the grid has more than nine cells, never with Alt (Claude Code's
own Alt+letters). Track held keys by physical key, not by cell — two keys can name one cell.

**Ruling R2: bare digits, no modifier — but gated.** The chord is live only when **the sidebar is open
AND a tile is selected AND focus has not been handed to a terminal**. Otherwise `1` and `2` are ordinary
characters and must reach Claude Code untouched.

That gate is not new machinery. `renderer/js/85-focus.js` already encodes exactly this two-state focus
model: focus sits in the deck chrome until **Enter or Space** hands it to the shown terminal, and it
already guards on "not inside `.xterm`", "not in an input/textarea/select/button", and `panelOpen()`
(settings, move menu, elsewhere/past menu, search, palette). **Reuse that guard verbatim**, adding
"sidebar not hidden" and "a tile is selected". Once the user presses Enter or Space, focus is in the
terminal and digits type normally — the layout chord is off until focus leaves it again.

### F3 — Conflict resolution (shared by F2 and F4)

When tile **T** is assigned quadrant set **S**, every other placed tile **U** with occupancy **O(U)**
resolves as:

| Case | Result |
|---|---|
| `O(U) ⊆ S` — U only occupies places being taken | **U is evicted** (ruling R6): removed from the layout, session keeps running, tile returns to unplaced in the sidebar. It is *not* closed, and it does *not* swap into the incoming tile's old spot |
| `O(U) ∩ S ≠ ∅` and `O(U) ⊄ S` — partial overlap | **U shrinks** to `O(U) \ S` |
| `O(U) ∩ S = ∅` | untouched |

**The gap the original description doesn't cover:** `O(U) \ S` is not always a rectangle. With four quadrants
there is exactly one such case — a **full-window** tile losing **one** quadrant leaves an L-shape of
three. Example: `U = {1,2,3,4}`, `S = {1}` → remainder `{2,3,4}`, illegal. The candidates to shrink to
are the two halves that avoid the taken quadrant (here `{3,4}` bottom and `{2,4}` right), which tie at
two cells each. **Ruling R3: always prefer the horizontal half** (top/bottom) — terminals want width
more than height. So `U = {1,2,3,4}` losing `{1}` becomes `{3,4}`, the bottom half.

### F4 — Drag and drop onto a pane

Dragging a tile from the sidebar over `#main` shows a live drop preview: the region the session **will**
occupy is outlined/highlighted, and the remainder of the window is shaded so the user can see what the
session will *not* take. Dropping applies the same conflict rules as F3.

**Ruling R4: corners, edges, middle.** Pointer in a quadrant's corner area → that quadrant; near the
midpoint of an edge → the adjacent half; near the centre → the full window.

**Existing constraint:** the sidebar already uses HTML5 drag/drop for reordering, and only pinned tiles
are draggable (`renderer/js/30-tiles.js:60-92`, `el.draggable = !!t.pinned` at line 197). Pane
assignment needs **every** tile draggable, so drag-within-sidebar (reorder, pinned only) and
drag-to-main (placement, any tile) must be distinguished by drop target, not by whether the drag can
start.

### F5 — Drag a tile outside the window → a new deck window

Dropping a tile outside the current window's bounds opens a **second Claude Deck window**, with its own
independent 2×2 quadrant layout, and places the tile there.

Three things this breaks in the current app, all of which must be handled:

1. **`main.js:1118` takes `requestSingleInstanceLock()`** and `second-instance` merely focuses the
   existing window. Keep the lock — additional windows are extra windows **in the same process**, never
   a second instance. (A second process losing the lock and quitting is a known failure mode on this
   box.)
2. **`main.js:1191`: `window-all-closed` calls `killAll()` on Windows.** With multiple windows, closing
   *one* must not kill anything — only closing the *last* one retains today's behaviour.
3. **A tile lives in exactly one pane in exactly one window.** Placing it elsewhere moves it.
   **Ruling R5: every window's sidebar lists every session**, marking which window a placed tile
   currently lives in.

### F6 — Remote mode (one protocol, phone and desktop as two clients)

**Naming ruling (2026-09-05): "phone mode" becomes "remote mode."** The desktop client will do
what the phone currently does, over the same protocol, so the concept is not phone-specific. The phone
and a remote desktop are two **clients** of remote mode, not two features.

This is a cross-lane rename — `lib/phone.js`, `renderer/phone/**`, `settings.phone`, `PHONE_ACTIONS`,
`syncPhone`, the `driver` values (`'phone'` / `'desk'`), README and `mobile.md` — almost all of it in
the phone session's lane under `COORDINATION.md`. **Spec only; do not start it mid-flight.** The natural
moment is when F6(a) lands, since that work already touches every one of those names. Keep the wire
format stable across the rename so an old client is not broken by a vocabulary change.

A person on a different computer runs the same desktop UI, driving the Claude sessions that live on
this box. Two layers, and they are not the same job:

- **(a) Browser desk — already planned, ~1 day.** `mobile.md:767` ("Upcoming", agreed 2026-09-03, not
  started): serve `renderer/` plus the two xterm files from the phone server under `/desk/`, same token
  and cookie; a served bridge shim defines `window.deck` over the websocket. The phone whitelist covers
  **20 of the 38** bridge functions — missing are reorder, the screen-probe reply, save accounts, OSC
  title and fullscreen (the last two become no-ops; clipboard maps to the browser's own). Events (data,
  meta, init) exist; exit and reset need messages. Agreed rulings: **a remote desk counts as a phone for
  the driver setting** (drives the tile it is attached to under auto, lets go when hidden), and **two
  remote desks at once is unsupported**.
- **(b) Native remote mode.** The C++ or Swift shell runs with **no local core**, speaking the same
  websocket protocol to a deck on another machine. This is the version asked for.

**This is the feature with the largest architectural consequence — see §4.**

---

#### Ruling R10 — while a remote is driving a tile, the desktop is read-only on it

**2026-09-05.** Local keystrokes to a remote-driven tile are ignored. The only local input the
desktop accepts on that tile is the control that **ends remote mode / takes the driver back**.

**R10 does not fix the wheel-report leak — R8 does.** The leaked bytes came *from* the remote, so a
desktop lockout would not have stopped one of them. R10 solves a different, real problem: two drivers
fighting over one session. Keep them independent; reversing one must not reverse the other.

**Scope it to "a remote is actively driving *this* tile", not "remote mode is enabled."** The deck
already computes this — `settings.driver` (`'phone'` / `'desk'` / auto), `phone.watching(t.id)`, and
`t.lastInputFrom` / `lastInputAt`, published as `driver` / `driverAt` in the tile meta
(`main.js:680-682`); under auto the remote already "lets go when hidden" (agreed 2026-09-03). Without
that scoping you lock yourself out at your own desk whenever a phone merely happens to be connected,
which is a worse bug than the one being solved.

**The lockout must be visible.** A tile that silently swallows keystrokes reads as a broken deck. Say
on the tile that a remote is driving, and put the take-back control where the message is.

#### Ruling R8 — while remote mode is active, only real keystrokes reach the pty

**2026-09-05.** A remote client may send what the user actually typed and nothing else. **No
synthesized sequences at all** — no wheel notches, no `ctrl+End` snaps, no prefetch bursts. The way to
get them back is to **turn remote mode off from the desktop client**.

The reasoning: synthesized input is generated by heuristics that fire against an assumed session state,
and when that assumption is stale the bytes land somewhere they were never meant to go. That is exactly
what produced the leak below. Real keystrokes carry no such assumption.

**What this costs, stated plainly.** `wheelSeq` is the *only* mechanism that can scroll a Claude Code
session: it runs in the alternate screen buffer, which has no scrollback, so `term.scrollLines()` moves
nothing (`renderer/phone/app.js:624-632`). Under R8, in remote mode:

| | Effect |
|---|---|
| Plain shell tiles | **Unaffected** — normal buffer, xterm scrolls locally, no pty write involved |
| Claude Code tiles | **No live scrolling.** The stitched scrollback, the peek and the prefetch (`app.js:1145`, `1319`, `1371`) are all built on `wheelSeq` |
| Reading history | The transcript text view remains the reading path |

**The narrower variant, if R8 proves too blunt in use:** allow synthesized sequences *only* when the
stream's own `mouseOn` / `sgrMouse` say the session is asking for them — i.e. "never send input the
session has not asked for" rather than "never synthesize input". That alone closes the bug below, and it
keeps phone scrolling. R8 is the stricter choice and is what is specced; this is the fallback to reach
for rather than reinventing one.

**Enforcement point.** Client-side, by not calling `wheelSeq` while remote mode is active — the four
call sites are `app.js:1154`, `1164`, `1323`, `1372`. A server-side backstop would need the wire format
to mark synthesized input (a `kind` field on the `input` message), since a synthesized sequence and a
typed one are indistinguishable on arrival. Both halves are in the phone session's lane.

#### Known issue on the remote-input path (found 2026-09-05 — upstream, not the deck's bug)

Claude Code's TUI turns mouse tracking on (`?1000h` / `?1002h` / `?1003h` + SGR `?1006h`), so the phone
view correctly sends SGR wheel notches as ordinary input (`renderer/phone/app.js:646`, emitting
`ESC[<64;40;12M`). **Claude Code parses only the `ESC[<6` prefix and leaks the tail `4;40;12M` into its
prompt as literal text.** Verified in a session transcript: two scroll notches prepended
`4;40;12M4;40;12M` to a typed message, and while an interactive option picker was open the leaked `1`
was taken as "select option 1", auto-answering five questions in a row that the user never answered.

**It is fixable on both sides, and the deck's half is the one that matters.** The owner's observation — it
only appears after the desktop has been **idle a long time** — pointed at a stale gate, and the evidence
backs it: across all **92 transcripts** on the box there is exactly **one** distinct leaked coordinate,
`4;40;12M`, which is `wheelSeq`'s hardcoded `;40;12` (`app.js:648`). A desktop xterm would report the
real pointer cell and vary. So the bytes are the remote view's, and they got through a gate that should
have stopped them.

**The gap:** `inAltScreen()` (`app.js:653`) already trusts the main process — *"it tracks the session's
screen mode from the stream, so a session is known to be full-screen the instant it opens, before any
replay has landed."* `mouseOn()` (`app.js:639`) does **not**: it trusts only the remote view's own
`term.modes.mouseTrackingMode`, which can be stale after an idle reconnect, because the replay re-emits
the modes (`main.js:409-412`) and there is a window where xterm says tracking is on while the session is
not asking for reports.

**Fix, in two halves:**
- **Done** (`main.js`, tile meta beside `altScreen`): publish the stream's own answer as `mouseOn` and
  `sgrMouse`, from `t.modes` — reset on spawn (`main.js:296`), tracked by `MODE_RE` (line 399), never
  stale.
- **Requested** of the phone session (`COORDINATION-REQUESTS.md`): make `mouseOn()` require **both**
  the stream's answer and xterm's, the way `inAltScreen` already does.

Two things still follow:
- `app.js:635`'s comment records a *different* failure of the same shape (2026-09-03: tracking **off**
  at a trust prompt). It reads as though `mouseOn()` closed the hole; it did not.
- **For the native shells:** F6(b) means a remote input path carries mouse reports. Their VT input
  parser must consume a full SGR report or none of it — never a partial prefix. The upstream partial
  parse is reported to Anthropic (2026-09-05); the core must not repeat it. Put it in the core, once.

### F7 — Up / Down step between tiles (both platforms)

With the sidebar open, **Up** and **Down** move to the previous / next tile in sidebar order, wrapping —
the same action F11 / F12 already perform on Windows and `Cmd+Shift+[` / `Cmd+Shift+]` on Mac.

**Gate: identical to R2's.** Live only while **the sidebar is open AND focus has not been handed to a
terminal AND no panel owns the keyboard**. Arrow keys must reach Claude Code untouched whenever the
terminal has focus — they are its history navigation. Reuse the `renderer/js/85-focus.js` guard, so one
gate serves both the digit chord (F2) and arrow stepping.

**Trivial to implement:** `renderer/js/80-sidebar.js:15` already has `deck.onTileStep(delta)` doing the
wrapping step. Up = `-1`, Down = `+1`. It belongs in the **renderer**, not in `lib/platform.js`
`keyAction`, because unlike the function keys it depends on renderer state (sidebar visibility, focus
location) that the main process cannot see.

The existing keys keep working unchanged, and the difference is deliberate:

| Binding | Platform | Works while the terminal has focus |
|---|---|---|
| `F11` / `F12` | Windows | **Yes** — main-process window keys |
| `Cmd+Shift+[` / `Cmd+Shift+]` | Mac | **Yes** — main-process window keys |
| `Up` / `Down` | both | **No** — chrome focus only |

**Mac already has the F11/F12 equivalent — no new hotkey is needed.** `lib/platform.js:158-159` maps
`Cmd+Shift+[` → prev and `Cmd+Shift+]` → next (with `{` / `}` as the shifted twins), and `keyHelp` at
line 173 already advertises them. That pair is the macOS convention for previous/next tab (Safari,
Chrome, Terminal.app), and it cannot collide with Claude Code because Cmd is not Meta in the terminal —
`macOptionIsMeta: true` makes **Option** the Meta key. The requirement is already met; F7 only adds the
arrows on top.

---

## 4. What the features do to the architecture

> **Design the core/shell boundary as a message protocol, not a function-call ABI.**

F6(b) means a shell must run against a core that is either **in-process** (local) or **across a
network** (remote). If the C ABI is designed as synchronous function calls and a network shim is bolted
on later, every call becomes a blocking round-trip and the remote deck is unusable. Design it from day
one as asynchronous messages — request/event, serialisable, with the local case as a direct dispatch
that skips the socket. The existing phone websocket protocol (`lib/phone.js`) is the working prototype
of that message set; treat it as the draft wire format and close its 18 missing bridge functions as part
of defining the core's surface.

Consequences of the other features:

- **"Render only the visible terminal" survives, parameterised.** The single biggest native performance
  rule (§7) assumed 1 visible tile of 24. Quadrants make it up to 4 of 24 — still an enormous saving,
  but the rule is now "render the *placed* tiles" and the shell must track a placement set rather than
  one `state.active`.
- **PTY sizing changes owner.** Today `renderer/js/20-terminals.js:94` pushes a resize to the pty only
  for `state.active`. With up to four placed tiles, **every placed tile owns its own size**, and each
  pane's dimensions drive its pty independently. Note `resizeTile`'s `from` argument is phone-owned
  under `COORDINATION.md` — coordinate before touching it.
- **The narrow-shell rule needs amending.** The 2026-09-05 rule was "keep the desktop shells narrow;
  experiments land in the phone HTML first." F1–F5 are window management, which cannot live in the
  phone HTML. **Amended rule: content experiments land in the phone view first; window and pane
  management is shell work by definition, and is prototyped in Electron first** (§5).
- **Multi-window vs the phone driver.** The phone "drives" a tile. With several windows and up to four
  visible panes each, the rule stays: a tile has one home pane, and the phone's claim is on the tile,
  not the pane.

---

## 5. Build order

Prototype the interaction model in **Electron first**, then port it twice. Reason: F1–F4's hard part is
not rendering, it is the conflict/shrink rules and the drop zones feeling right. Getting those wrong in
C++ and Swift means fixing them twice. `.term`'s `position:absolute; inset:0` makes the quadrant grid a
CSS change, and the drag/drop plumbing already exists in the sidebar.

1. **Tile hibernation, in Electron.** Kill the pty for an idle tile, keep the scrollback, `--resume` on
   wake. At **244 MB per `claude` process** this frees multiple GB — an order of magnitude more than the
   entire rewrite — and both native shells want the feature. Build it here so the core inherits a proven
   design.
2. **F7 Up/Down stepping in Electron.** Smallest item on the list — `onTileStep` already exists, this is
   the keydown handler and the focus gate. Building it first proves the gate that F2 then reuses.
3. **F1–F4 in Electron.** Quadrants, the bare-digit chord, conflict resolution, drop preview. All
   rulings are settled (§8) — build to them, then try it by hand and adjust from there.
4. **F5 multi-window in Electron.** Forces the `window-all-closed` / single-instance-lock work early,
   where it is cheap.
5. **F6(a) browser desk** (`mobile.md:767`) — closes the 18 missing bridge functions, which *is* the
   first draft of the core's message surface.
6. **Scope the core's C ABI** as an async message protocol, against `lib/transcript.js`, `lib/usage.js`,
   `lib/sessions.js`, `lib/phone.js`, `lib/platform.js` and the now-complete bridge whitelist.
7. **C++ core + C++ Windows shell**, then **Swift Mac shell**, then **F6(b) native remote mode**.

Steps 1–5 are all in the language you already have, all ship value immediately, and each one shrinks the
native work.

### Ruling R11 — repository shape: one repo, directories on `main`

**2026-09-05.** Not a long-lived branch, and not a new project.

```
docs/        specs + rulings — the source of truth
electron/    today's app, the reference implementation
core/        C++ core, C ABI
windows/     C++ shell
mac/         Swift shell
```

**Why not a branch.** A branch that never merges is a fork with extra steps. Electron stays the shipping
deck *until a shell reaches parity* — a long coexistence, not a merge. A native branch would either
drift from `main` or bleed on constant merges of code it does not share (it shares **zero** source with
the Electron app).

**Why not a separate repo.** The recurring tax (§7) is that every ruling gets built three times.
Anything that makes "change all three in one commit" harder makes that tax worse, and separate repos
guarantee the specs drift. The rulings, `README.md`, `mobile.md` and `COORDINATION.md` are the shared
asset; the implementations are the cheap part. One repo means one history, one issue tracker, one set of
rulings, and a ruling change landing as a single commit across every implementation that must honour it.

**Do not restructure yet. The trigger is build-order step 6.** Steps 1–5 are all Electron work in the
existing tree; the move only earns its cost at the first commit that is not Electron — scoping the
core's C ABI.

**Restructure checklist**, when the time comes — it is a `git mv` that moves every path in the project,
so do it as **one commit with no branch in flight**:

- [ ] No unmerged branches (`installers` was 1 commit ahead and unpushed on 2026-09-05; it owns
      packaging paths, which is exactly what a restructure moves — land it first)
- [ ] `~/.local/bin/claude-deck` launcher (both the `.cmd` and the bash twin) and the Start Menu entry
- [ ] `electron-builder.yml` `files` / `extraResources`, and `npm run mac:app` / `dist:mac`
- [ ] `package.json` `main`, and the `node_modules` junctions used by the worktrees
- [ ] `renderer/js/NN-*.js` load order and every path in `main.js` / `preload.js`
- [ ] `COORDINATION.md`'s ownership table — every path in it changes
- [ ] `docs/*` cross-references, `README.md`, `mobile.md`
- [ ] CI path filters, so a docs change does not build three apps

**The one condition that would flip this to a separate repo:** if the native app is ever meant to ship,
be licensed, or be open-sourced separately from the Electron one. That is a distribution boundary and is
worth paying for. Nothing indicates it today.

---

## 6. Measured baseline (2026-09-05, the development box, 24 live tiles)

| Process family | Count | RSS |
|---|---|---|
| `claude` sessions | 34 | **8,260 MB** (avg 244 MB) |
| `electron` (the deck) | 7 | **381 MB** — renderer 148, GPU 134, main 39 |
| `node_modules` on disk | — | 453 MB |

**The deck is 4.4% of what it hosts.** A perfect rewrite saves ~300 MB — about 1.2 Claude sessions.
Keep this in front of anyone who proposes the rewrite as a memory fix.

### Expected gains (estimates, 24 tiles)

| | Electron (now) | Native |
|---|---|---|
| RSS | 381 MB (measured) | 70–130 MB |
| Packaged size | ~200 MB | 5–10 MB |
| Cold start | 1.5–2.5 s | 0.10–0.25 s |
| Keystroke → glyph | 8–20 ms | 3–8 ms |
| Bulk output throughput | 1× | **5–15×** |
| Terminals before renderer strain | 16 (WebGL cap) | no cap |

The only gain you would *feel*: pasting a huge build log or file dump into a tile.

---

## 7. Hard constraints no language changes

1. **ConPTY caps Windows at roughly 1–10 MB/s.** `node-pty`, `portable-pty` and a raw Win32 call all go
   through the same `conhost` pipe. The parser is **not** the Windows bottleneck.
2. **macOS has a real PTY** — tens of MB/s. The parser **is** the Mac bottleneck. Core quality matters
   more on Mac than on Windows.
3. **Scrollback is data, not runtime.** 24 terminals × 10,000 lines costs the same in any language
   (native cells ~24 B, xterm.js ~16 B — the rewrite is slightly *worse* per cell). Paging or
   compressing scrollback is the only fix, and it can be done in JS today.
4. **The 34 `claude.exe` processes are untouched** by any of this.

### Renderer rules that beat any language choice

1. **Render only the placed terminals.** With quadrants that is at most 4 of 24; the rest are pure grid
   state with no draw path. This dwarfs everything below it.
2. **Damage tracking** — repaint dirty cells, not the frame.
3. **Persistent glyph atlas + instanced quads**, one draw call per frame. Never call CoreText or
   DirectWrite *during* a frame; shape once, cache the runs.
4. **`MTLResourceStorageModeShared`** on Apple Silicon — zero-copy uploads on unified memory.

`renderer/js/20-terminals.js:84` records why this repo has no WebGL addon: Chromium allows ~16 WebGL
contexts per page and the deck runs more tiles than that. A native shell has one GPU device and one
atlas for all tiles, so the cap disappears. **Do not "restore WebGL" in the Electron app.**

### The recurring tax

The UI is the part no core shares. **3,854 lines of renderer JS and 31 KB of CSS** become two hand-built
native UIs that must stay identical to each other and to the phone view. Every ruling — bar geometry,
pin-then-last-finished ordering, effort tags, the palette, drag order, F9/F10/F11/F12, and now the
quadrant model — gets built and fixed three times, forever. That tax is larger than the rewrite itself.

---

## 8. Rulings — all settled 2026-09-05

**Nothing here is open.** Build to these; do not re-ask.

| # | Question | Ruling |
|---|---|---|
| **R1** | Diagonals `{1,4}` / `{2,3}` — are they legal? | **Promote to the full window.** A diagonal chord means all four quadrants |
| **R2** | Do the digits need a modifier? | **No modifier — bare `1`–`4`**, live only while the sidebar is open AND a tile is selected AND focus is not in a terminal. Reuse the `85-focus.js` guard |
| **R3** | A full-window tile losing one quadrant leaves an illegal L-shape. Which half does it shrink to? | **The horizontal half** (top/bottom) — terminals want width. `{1,2,3,4}` minus `{1}` → `{3,4}` |
| **R4** | Drop hit-zones inside `#main` | **Corners, edges, middle.** Corner → that quadrant; edge midpoint → that half; centre → full window |
| **R5** | Does every window's sidebar list every session, or only its own? | **Every session in every window**, marked with which window holds it |
| **R6** | An evicted tile — unplaced, or does it swap? | **Drops out of the layout and keeps running.** Sits in the sidebar. Never closed, never swapped into the incoming tile's old spot |
| **R7** | Does the layout persist across restarts? | **Yes — layout and windows.** Quadrant occupancies restore, and the second window reopens with its sessions |
| **R8** | While remote mode is active, what input may reach the pty? | **Real keystrokes only.** No synthesized sequences of any kind — wheel notches, `ctrl+End` snaps, prefetch bursts. Turn remote mode off from the desktop client to get them back. Cost and the narrower fallback are under F6 |
| **R9** | Naming | **"Phone mode" becomes "remote mode."** Phone and remote desktop are two clients of one mode |
| **R10** | Can the desktop type into a tile a remote is driving? | **No — read-only on that tile.** The only local input accepted is the control that ends remote mode. Scope it to "a remote is actively driving *this* tile", and make the lockout visible. Does **not** fix the leak; R8 does |
| **R11** | Branch, new repo, or same repo? | **Same repo, directories on `main`** (`docs/` `electron/` `core/` `windows/` `mac/`). Not a long-lived branch, not a new project. **Do not restructure until build-order step 6** — checklist under §5 |

---

## 9. Repo map and verification

| Path | What |
|---|---|
| `README.md` | full behaviour spec for the current app |
| `docs/HANDOFF.md` | session log, newest first — append, never rewrite |
| `docs/NATIVE-PORT.md` | this file |
| `docs/INSTALL.md` / `USER-GUIDE.md` / `SETUP-PROMPT.md` | user-facing |
| `COORDINATION.md` | **read before editing** — several sessions edit this working copy at once; it is the ownership partition |
| `PRIMER.md` | design decisions, written for Casey |
| `mobile.md` | phone view spec; `:767` is the browser-desk plan |
| `main.js` | Electron main: spawn/kill/trust+resume prompts/restore/meta/settings |
| `lib/` | `transcript.js` `usage.js` `sessions.js` `phone.js` `platform.js` `queue.js` `accounts.js` — the core's specs |
| `renderer/js/NN-*.js` | classic scripts, one global scope, loaded in numeric order |

**Branches:** `main` (this work), `mac` (Electron Mac parity, in worktree `_wt-deck-mac`), `installers`
(in worktree `_wt-deck-installers`). Other sessions own those — do not touch them.

**Verify the environment:**

```
cd C:\Users\demo\Documents\GitHub\claude-deck
git status
node tools/transcript-probe.js          # confirms transcript shapes after a Claude Code update
```

**Test a deck without disturbing the running one** (from PowerShell — loopback is blocked from the Bash
tool):

```
electron . --user-data-dir=<scratch> --remote-debugging-port=9777
CDP_PORT=9777 node tools/cdp.js "<js>"
```

⚠️ **Do not use port 9333.** The live deck listens on it — verified 2026-09-05, `9333 LISTENING, pid
81384 = electron`. `tools/cdp.js` defaults to 9333, so running it bare **drives the real sessions on that box**.
Give the second deck its own port and pass that port to `cdp.js`. An earlier version of this section said
9333; it was wrong.

⚠️ **`--user-data-dir` alone is not enough on this branch.** `main.js:25` pins
`app.setPath('userData', …)` unconditionally, which overrides the flag — a second deck then shares the
live `tiles.json`, loses the single-instance lock and quits. **PR #3 fixes it** (pin only when no
`--user-data-dir` was passed); until that merges, **no branch cut from `native-port` can run a second
deck** — verify against the `hibernation` branch, or merge it into a local throwaway, or apply the same
guard by hand. **The failure is silent**: the second deck shares the live userData, loses the
single-instance lock and quits, with `DevTools listening` as the last line in `err.log`. If a test deck
vanishes with no error, this is why — you are not looking at a crash.

Resuming an old session costs no tokens. Restarting the live deck force-kills sessions mid-start and
trips Claude Code's classic-renderer fallback for one launch — restarts are the desktop session's to
own, per `COORDINATION.md`.
