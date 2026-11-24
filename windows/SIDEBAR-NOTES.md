# Native sidebar (`windows/src/sidebar.cpp`) — notes

`deck::ui::Sidebar` draws the Electron deck's sidebar from the same `meta` JSON (`main.js collectMeta`) and
answers hit tests. It is a port of what `renderer/index.html`, `renderer/style.css` and `renderer/js/30-tiles.js`,
`35-groups.js`, `40-new-tile.js`, `46-search.js`, `49-asks.js` (the asks TILE only), `80-sidebar.js`,
`82-restart.js` put on screen. Every number in the code is a number from `style.css`; each block names the rule
or the JS function it ports.

## Build, run, compare

```
powershell -NoProfile -ExecutionPolicy Bypass -File windows\test\build-sidebar.ps1            # build + render every fixture
powershell -NoProfile -ExecutionPolicy Bypass -File windows\test\build-sidebar.ps1 -Truth     # … and the Electron renderer's own picture
powershell -NoProfile -ExecutionPolicy Bypass -File windows\test\build-sidebar.ps1 -NoBuild -Only mixed*
```

- Output: `windows/test/out/<name>.png` (the folder ignores itself). `--dump` shots also write `<name>.png.txt`:
  content/view height, the order, each tile's span, and a hit-test sweep down three columns with the tooltips.
- `sidebar_shot.exe <meta.json> <out.png> [width] [height] [scroll] [flags]`; the flags are listed at the top of
  `windows/test/sidebar_shot.cpp` (`--sel= --hover= --part= --grouped --collapse= --nobars --nomsgs --search=
  --searchopen --closed --window= --scale= --skew= --dump --bench=`). `-mwindows`, off-screen only.
- `-Truth` is the ground truth, by static means only: it copies `renderer/index.html`, loads the REAL
  `renderer/style.css` and the real `00-state, 10-format, 30-tiles, 35-groups, 40-new-tile, 46-search, 49-asks,
  70-meta, 80-sidebar, 82-restart` scripts over a stub for `deck` / the terminals / the layout, feeds them the
  fixture (`applyMeta`), and screenshots with headless Edge (hidden, scratch profile under `%TEMP%`, animations
  off). No Electron, no deck. It writes `truth-<name>.png` and `cmp-<name>.png` (native | truth | difference ×3)
  and prints how many pixels differ strongly. Last run, 1x: 0 to 25 px on a 340×1180 shot for most views
  (empty: 0, rail: 0, grouped: 3, layout: 9, colour mode: 25, right-hand: 24); what is left is glyph
  rasterisation (ClearType fringes, the synthesised italic of "New session", 8 px button labels).

Fixtures (`windows/test/fixtures/`): `meta-mixed.json` (9 tiles, 3 accounts, every status, pinned, groups, a
CJK + emoji title and preview, ultracode, context, a Fable-scoped bucket, one account `expired`, held prompts,
two asks + the answered log, the restart strip armed with blockers); `meta-color.json` (effort as colour,
countdown labels, an update offer, the phone drives); `meta-right.json` (sidebar on the right, week label only,
a settling restart); `meta-layout.json` (with `meta.layout`, see below); `meta-empty.json`; `meta-sixty.json`
(60 tiles). `meta-mixed.json` and `meta-empty.json` are hand-written; the other four were derived from
`meta-mixed.json` by a throwaway script (same shape, different settings / tile counts).

## What the comparison taught (all of it is in the code)

- **Blink lays out in physical pixels.** Font ascent and descent are rounded to whole physical px (`line-height:
  normal` is their sum), the half-leading above a line is floored, boxes and text baselines are snapped only when
  painted. So nothing rounds a tile's y: a 14px title is 17.5 px tall and tile tops down the list land on half
  pixels. `K` (the target's dpi / 96) is used in every rounding, so 150 % and 200 % follow Chromium too.
- `font-variant-numeric: tabular-nums` matters with Segoe UI Variable (its default figures are proportional):
  the clock, ages, bar labels, counts, the ask mark.
- `letter-spacing: 0.2px` on the effort tag and the group name.
- **Font fallback.** Chromium on this box draws kana in Yu Gothic and Han in Microsoft YaHei; DirectWrite alone
  picks Yu Gothic UI, whose kana are narrower, so titles and previews wrapped elsewhere. The sidebar builds its
  own fallback (those two mappings, then the system's). Chromium chooses the Han font by locale, so this is
  right for an en-US box and a guess elsewhere.
- svg boxes (pin, cloud, tool icons) are snapped to the pixel grid; a `.t-quad` aligns by its first cell's
  bottom edge; `#sb-tools` buttons shrink toward their icon when the sidebar is too narrow for the row.
- On a right-hand sidebar with effort as colour, the selected low / medium / xhigh tile gets its level colour on
  the outer edge and a transparent inner edge (the `body.sb-right…` rules come later with the same weight).

## Implemented

| Source | What |
|---|---|
| `style.css` `:root`, `#sidebar`, `#tiles`, `#sb-bottom` | colours, paddings, 4 px gaps, the column: asks tile, scrolled list, bottom block |
| `30-tiles.js updateTile` | abbreviation (green when busy), cloud mark, title (`tileTitle`, untitled italic, `fitTitle` 14 → 8 px then wrap), ask mark, effort tag, layout glyph, age (`fmtAge`), line 2 (account label, status words, `· on your phone`), asleep / elsewhere dimming, `desk-attn` border |
| `30-tiles.js applyBars` + `rampColor` | label column (reset time / day or countdown, `sameReset`, labels on/off), session / week / Fable / context rows with `settings.bars` show + colours, the hsl ramp exactly as the JS rounds it, unavailable (50 %) and the note line, row tooltips |
| `.t-msg` | preview: `\r` dropped, lines right-trimmed, blank lines dropped, `msgLines.normal` / `.wide` (> 400 px), ellipsis on the clamped line, emoji in colour |
| `renderHeld` | held rows: when (`fmtHeldTime`, `every Nm HH:MM–HH:MM`, `⚑ trigger`, `next turn`), text, state colour, × |
| `renderActions` (plain state) | sleep / wake / resume, `open <account>`, special, refresh, close / remove; shell and cloud variants; `fitButtons` (11 → 7.5 px) |
| effort styles | `tag`: per-level colours, max rainbow text, ultracode purple, ultracode-at-max rainbow frame. `color`: left (or right) border per level, max rainbow strip, ultracode strip, both selected variants, ultracode at max. `none`: nothing |
| pin | pinned look, hover look, corner by `sidebarSide` × `pinSide` |
| `35-groups.js layoutGroups` | pinned / each group / ungrouped, counts, collapsed chevron, hidden tiles |
| `46-search.js` | the filter (same haystack), the pane: input, `n / total`, clear |
| `49-asks.js paintAsksTile` | glyph, title, count, none / asking looks, a row per session (mark, abbr, name, where, age), "Recently answered (n)", `settings.asks.showTile` / `showMarks` |
| `40-new-tile.js` + html | dashed tile, account buttons (wrap rule), Elsewhere / Past |
| `82-restart.js` | strip: three titles, blocker rows, the note, Arm / Later / Now / Cancel, offer + settling looks; the button's armed / offer looks and tooltips |
| `#sb-tools` | all nine buttons, the clock, on / off states, drive tooltips, mirrored on the right; the collapsed rail |
| zones | `tile`, `tile-pin`, `tile-acct`, `tile-close`, `group`, `asks`, `new-tile`, `new-acct`, `elsewhere`, `past`, `tool`, `restart`, `resizer`, in z-order, each with its `title` text. `order_`, `spans_`, `contentH_`, `viewH_` filled |

## Decisions where the brief and the Electron app disagree (the app won)

- **No status dot.** The Electron tile has none: status is the green abbreviation and the words on line 2.
- **No scrollbar.** `#tiles::-webkit-scrollbar { display: none }`: nothing is drawn and no `scrollbar` zone exists.
- **No × and no account chip on a tile.** `tile-close` is the `close` / `remove` button of the SELECTED tile's
  action row; `tile-acct` is the abbreviation (or the account label on line 2), which in Electron is not a
  control (a click there selects the tile).
- Zones beyond `ui.hpp`'s list, so nothing clickable is lost: `tile-action` (arg = `sleep | wake | resume |
  special | refresh | restart | reload | browser | retry | open:<account id>`), `held-x` (id = tile, arg = held
  id), `restart` id `row` (arg = tile id), `asks` with id = session id or `__recent`, `tool` ids `expand`,
  `search-input`, `search-clear`.
- `order()` is the tiles DRAWN (search and collapsed groups leave some out). The JS steps F11 / F12 through
  every tile of `meta.tiles`, hidden or not: use `meta.tiles` for that if the walk must match.
- `State.showBars` / `showMsgs` / `grouped` / `collapsedGroups` are the per-viewer localStorage toggles.

## Added 2026-11-18: what the selected tile and the overlays show

`ui::State` now says what the Electron tile keeps in the DOM, and the painter draws it (each checked against
`-Truth`, strongly differing pixels in brackets):

| State | Drawn | Source |
|---|---|---|
| `confirm[id]` kind `move` / `refresh` / `close` (+ `browser`) | the danger button, Cancel and the question under them [1] | `30-tiles.js renderActions` |
| `confirm[id]` kind `hold` + `State::hold` | the Special prompt form: kind tabs, note, prompt box, the kind's fields, add / cancel [380 for next turn; the date and time boxes are text fields] | `renderHoldForm` (`overlays.hpp` `SidebarPainter::holdForm`) |
| `renaming` + `renameEdit` | the 28 px rename box across the title line, abbreviation and age gone [5] | `renameFns`, css `.tile.renaming` |
| `dragging`, `dropMark` / `dropBefore` | the dragged tile at 45 %, the 3 px bar above / below a pinned tile [1] | css `.tile.dragging`, `.drop-before` |
| `mouseX` / `mouseY` | the pin at 70 % anywhere in its 46×34 corner zone | css `.t-pin-zone` |
| `restartConfirming` | "Stop n and restart" in the danger look [7] | `82-restart.js nowConfirmUntil` |
| `searchEdit`, `focusEdit`, `blinkOn` | the search box as a live text field (caret, selection, focus outline) | `46-search.js` |
| `elsewhereCount`, `pastCount` | "Elsewhere [n]" / "Past [n]", 40 % when nothing is there | `45-elsewhere.js` badges |
| `toolsWidth` | the tool row pinned at its width while Settings widens the sidebar | `50-settings.js widenForSettings` |
| `hoverArg` | which button of the action row is hot | |

New zone kinds: `confirm-yes`, `confirm-no` (id = tile), `hold` (id = tile, arg = `kind:<k>` | `pick:<i>` | `add` |
`cancel`), `edit` (id = `rename` | `search` | `hold-text` | `hold-time` | `hold-every` | `hold-from` | `hold-to` |
`hold-trig`). `Sidebar::find`, `all`, `tileRect`, `bottomTop` serve the app and the scripted tests.

The overlays are drawn in this translation unit too, with the sidebar's text rules, from models in `ui.hpp`:
`drawCtxMenu` (in `sidebar.cpp`), `drawPalette`, `drawPullMenu`, `drawMoveAll` (`overlays.hpp`), `drawSettings`
(`settings.hpp`; row positions within 1 px of the truth down the whole panel). `sidebar_shot.exe` takes
`--confirm= --rename= --hold= --drag= --drop= --mouse= --restartconfirm --menu= --prompt= --settings=
--settingsscroll= --toolsw=` for them, and `build-sidebar.ps1 -Truth` has a truth case for each (an `OPTS.run`
string evaluated in the page after the fixture is applied; `fixtures/settings.json` is a `getSettings` answer).

Learned on the way:
- `.t-actions button` outweighs `.sp-kind`, and the page's `input[type=text]` / `input[type=number]` rules outweigh
  `.sp-num` / `.sp-trig`: the kind tabs are ordinary 11 px buttons (only the border turns blue), and the number and
  trigger boxes are 12.5 px on `#0f0f0f`.
- A direct child of the settings body (a column flex box that overflows) shrinks to its content: `+ add account`
  is 17 px tall, not its css 19.
- A default `<select>` and checkbox on this page (no colour scheme) are the light ones: white box, blue tick.

## Not drawn

- Animations (drawn in their resting state): ultracode pulse, max hue drift, the asks tile's and `desk-attn`'s
  amber pulse, the armed restart icon turning, bar width transitions.
- The asks tile as the selection (`asks-focus`, picked row): the ask pane needs a web view.
- `.rs-list` and `.at-rows` do not scroll (the asks rows clip at 132 px as the css does).

## Needed from `ui.hpp` / `gfx.hpp` (worked around locally)

- `State`: this window's layout, `tile id → cells`. Until then the sidebar reads an optional **`meta.layout`**
  the app may add (`{ "t1": [1, 3] }`, reading-order cells, grid from `meta.settings.grid`); without it the
  occupancy glyph is not drawn (the other-window frame is, from `inWindow`).
- `State`: `asksFocused` + `asksPick` (the pointer, the hover `arg`, `confirm`, `renaming` and the drag state are there
  now: see the 2026-11-18 section).
- `Gfx`: text by baseline; tabular figures; letter-spacing; a font-fallback hook; line-clamped wrapped text
  (`layout()` ignores `maxLines`, so `textWrapped` clips instead of ending in an ellipsis); a gradient in any
  direction and as a text brush; an opacity layer; path geometry + stroke styles (round caps, dashes). All are
  in `sidebar.cpp` (`mkLayout`, `tb`, `wrap`, `lin`, `pushOpacity`, `svgGeom`) and could move to `gfx.hpp`.
- Cost: about 12 to 15 ms per draw on the software bitmap target (`--bench=`), nearly all text layout and svg
  geometry rebuilt every frame. If the window target is not much faster, cache layouts and geometries by key.
