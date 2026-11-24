'use strict';
/* global Layout, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- quadrant layout (F1–F4)
// The terminal area is a 2×2 grid and a placed tile occupies a rectangle of it: one quadrant, a half,
// or the whole thing. The rules — which sets are legal, who gets evicted or shrunk when a tile lands,
// what a drop position means — are in lib/layout.js (window.Layout) and are NOT re-decided here. This
// file is the view: insets on `.term` panes, the drop preview, the digit chord, persistence.
//
// Two modes, and the difference is deliberate:
//   layout EMPTY   — today's deck: the selected tile fills the area (`.term.active`), nothing else shows.
//                    Every deck starts here and a deck that never places anything never leaves it.
//   layout NON-EMPTY — the placed tiles show at their rectangles (`.term.placed`); everything else is in
//                    the sidebar only. Selecting an unplaced tile does not break the layout: it takes
//                    over the pane of the tile that was selected (see layoutAdopt), the way a tab would.
//
// R7: the layout persists in tiles.json (`layout:set` → main) and is restored at boot, pruned of tiles
// that no longer exist.

state.layout = {};                   // tileId -> [cells]; the model's plain shape (reading-order cells)
state.grid = { rows: 2, cols: 2 };   // the layout grid shape (R16); filled from deck.init / grid:changed
const GRID = () => state.grid || { rows: 2, cols: 2 };
let layoutRestored = false;          // the boot restore has run; saves before that would clobber it

const isPlaced = id => !!(id && state.layout[id]);
const placedIds = () => Layout.placedIds(state.layout);
// What is on screen: the placed tiles, or — with no layout — the selected one. Except (F5) when the
// selected tile lives in ANOTHER window's layout: a tile shows in one pane of one window, so this window
// shows the empty notice instead of a second copy. Selecting it again here brings it over (layoutAdopt).
function heldElsewhere(id) { const t = id && state.tiles.find(x => x.id === id); return !!(t && t.inWindow != null && t.inWindow !== state.windowIndex); }
// Settings → Layout (2026-09-08). The setting is the DECK'S OWN windows' rule (2026-11-18:
// "the setting should get interpreted per box"): in a remote desk (renderer/desk/bridge.js) it has no say in
// what the desk shows. There nothing is ever held elsewhere (main sends a desk no inWindow), so the rule has
// nothing to act on, and the one other thing the setting switches (only the focused window pushes a pane's
// size when it is off, 20-terminals.js) stays as with it on: the desk is one window.
const IS_REMOTE_DESK = typeof deck === 'object' && !!deck && !!deck._desk;
const oneWindow = () => IS_REMOTE_DESK || state.settings.oneWindow !== false;
// R13 as ruled (2026-09-08): an AUXILIARY window behaves exactly like the main one — with no layout
// it shows the selected tile full-bleed. "One window per session" still holds: a tile held in another
// window's layout shows here only once it is selected here, which moves it (layoutAdopt places it full
// and main evicts it there); with that setting off it simply shows in both.
function isPrimaryWindow() { return (state.windowIndex || 0) === 0; }
function visibleIds() {
  if (!Layout.isEmpty(state.layout)) return placedIds();
  if (!state.active) return [];
  if (heldElsewhere(state.active) && oneWindow()) return [];
  return [state.active];
}
const isVisible = id => visibleIds().includes(id);
// The window's name (Electron follows document.title). A window that ONE session fills — full-bleed with no
// layout, or the only pane of a layout and covering the whole grid — is named after that session, so the
// taskbar and Alt+Tab say which one it is (2026-10-21). A zoom (F6 / double-click) does not count: the
// grid is still there behind it. Anything else wears the deck's name and the count of open sessions.
// Run from renderTiles, which every meta tick and every layout paint ends with.
function paintWindowTitle() {
  const vis = visibleIds().filter(id => terms.has(id));
  let fills = vis.length === 1;
  if (fills && !Layout.isEmpty(state.layout)) { const r = Layout.rect(Layout.occupancy(state.layout, vis[0], GRID()), GRID()); fills = !!r && r.w === 1 && r.h === 1; }
  const t = fills && state.tiles.find(x => x.id === vis[0]);
  const n = state.tiles.filter(x => !x.exited).length;
  const name = (state.edition && state.edition.name) || 'Claude Deck';   // a public native build names itself in meta.edition (70-meta.js)
  const want = t ? tileTitle(t) : (state.windowIndex ? name + ' · auxiliary [' : name + ' [') + n + ']';
  if (document.title !== want) document.title = want;
}

// ---------------------------------------------------------------- zoom (2026-09-14)
// Double-click a pane's name (or F6 / Cmd+Shift+Enter) and that session fills the terminal area; the same
// again brings the grid back. A view-only toggle: nothing is written to the layout, the other panes keep
// their cells AND their sizes (hidden, not resized, so the grid returns without a resize storm), and a
// restart or a view reload comes back in the grid. While zoomed the grid cannot be changed BY HAND — a
// drop, the digit chord, the F11/F12 walk, a sidebar double-click, the tile menu's placements, reset —
// because you cannot see what the change would do; each attempt shows a note instead. Placements the
// deck makes on its own still land underneath: a new session takes the first empty region, a closed
// session leaves its cell, a grid-size change remaps. Selecting another placed session (a sidebar click,
// the palette) zooms that one; if the zoomed session leaves the layout the grid comes back by itself.
// Hibernation's "on screen" set is the grid's membership, unchanged by a zoom (or every zoom would put
// the other panes to sleep and every unzoom wake them all).
state.zoom = null;                   // the zoomed tile id, or null; per window, never persisted
const zoomHidden = id => !!(state.zoom && id !== state.zoom && isPlaced(id));
function zoomTo(id) {
  if (!id || !terms.has(id) || !isPlaced(id) || Layout.isEmpty(state.layout)) return;
  state.zoom = id;
  activate(id, { adopt: false });    // paints via applyLayout; the pane's new rectangle queues its redraw
}
function unzoom() {
  if (!state.zoom) return;
  const owed = [...redrawOwed]; redrawOwed.clear();
  state.zoom = null;
  applyLayout();
  if (owed.length) queuePaneRedraw(owed);   // redraws that fell due while those panes were hidden
}
function zoomToggle(id) { if (state.zoom) unzoom(); else zoomTo(id); }
// One short note at the top of the terminal area, for "you did something and the deck did not do it":
// a grid change while zoomed, typing while the phone drives. A newer note replaces the one showing.
const zoomNoteEl = document.createElement('div');
zoomNoteEl.id = 'zoom-note'; zoomNoteEl.hidden = true; mainEl.appendChild(zoomNoteEl);
let zoomNoteTimer = null;
function paneNote(text, ms = 2500) {
  zoomNoteEl.textContent = text;
  zoomNoteEl.hidden = false;
  clearTimeout(zoomNoteTimer); zoomNoteTimer = setTimeout(() => { zoomNoteEl.hidden = true; }, ms);
  return true;
}
function zoomNote() {
  const kh = (state.platform && state.platform.keyHelp) || {};
  return paneNote(`Zoomed — double-click the pane name or press ${kh.zoom || 'F6'} to return to the grid before changing it.`);
}
// Typing refused because another screen is driving (main.js noteInputRefused; 2026-09-16). The driver
// button at the bottom of the sidebar (80-sidebar.js) takes it back in one click. Reasons: 'phone' / 'remote'
// = the setting says that screen; 'phone-attached' / 'remote-showing' = auto, and it is looking at this session.
if (typeof deck.onInputRefused === 'function') deck.onInputRefused(({ reason }) => {
  const back = ' Click the driver button at the bottom of the sidebar to drive from this PC.';
  paneNote((reason === 'phone' ? 'The phone is driving, so typing here is ignored.'
    : reason === 'remote' ? 'The remote desk is driving this session, so typing here is ignored.'
    : reason === 'remote-showing' ? 'The remote desk is driving this session (it is on screen there), so typing here is ignored. Hide it there, or:'
    : 'The phone is driving this session (it is open on the phone), so typing here is ignored. Close it on the phone, or:') + back, 6000);
});
// true (and the note shown) when a hand change to the grid must wait for the grid to be back
const zoomBlocks = () => state.zoom ? zoomNote() : false;
// F6 answers the asks tile when it is the selection, and closes the every-session ask pane (49-asks.js)
if (typeof deck.onZoom === 'function') deck.onZoom(() => { if (typeof asksZoom === 'function' && asksZoom()) return; zoomToggle(state.active); });   // an older main (before its restart) has no bridge for the key; the name still works

// Paint the model: one `.term` per tile, placed ones at their rectangle. Percent insets so the grid
// follows the window without another resize pass; the ResizeObserver on #main refits the terminals.
// A pane whose rectangle CHANGED gets the F5 redraw on its own (2026-09-10: "I find I'm having to do
// that manually a bunch"). The rectangle is remembered per tile as a key; a change after the first paint
// queues a redraw 400 ms out — past main's 150 ms resize debounce, so the pane's real resize (and ConPTY's
// re-emission at the new width) has landed before the reset + replay + nudge that F5 does. Coalesced per
// tile, so a walk or a drag that changes a pane several times redraws it once. Never during the boot
// restore (every pane would "change" from nothing), and only for panes still on screen when it fires.
const paneRectKey = new Map();
// Also used for a session that has just come up (70-meta.js): opened, woken, refreshed, moved to another
// account — its first screen is drawn into a pane that was showing the old process's replay, so it gets
// the same redraw, with a longer delay so the first frame has landed. Any on-screen pane qualifies there
// (the legacy full-bleed view too), so the caller passes the delay and the visibility test is isVisible.
const redrawOwed = new Set();        // redraws that fell due for panes hidden behind a zoom; run when the grid returns
let paneRedrawTimer = null; const paneRedrawQueue = new Set(); let paneRedrawDelay = 0;
function queuePaneRedraw(ids, delay = 400) {
  for (const id of ids) paneRedrawQueue.add(id);
  paneRedrawDelay = Math.max(paneRedrawDelay, delay);
  clearTimeout(paneRedrawTimer);
  paneRedrawTimer = setTimeout(() => {
    const due = [...paneRedrawQueue]; paneRedrawQueue.clear(); paneRedrawDelay = 0;
    for (const id of due) { if (!terms.has(id) || !isVisible(id)) continue; if (zoomHidden(id)) redrawOwed.add(id); else redrawTile(id); }
  }, paneRedrawDelay);
}
function applyLayout() {
  const empty = Layout.isEmpty(state.layout);
  // a zoom outlives its session's place in the layout by nothing: evicted, closed, unplaced -> the grid is back
  if (state.zoom && (empty || !terms.has(state.zoom) || !isPlaced(state.zoom))) state.zoom = null;
  mainEl.classList.toggle('zoomed', !!state.zoom);
  const changed = [];
  for (const [id, r] of terms) {
    const set = empty ? null : Layout.occupancy(state.layout, id, GRID());
    let rect = set ? Layout.rect(set, GRID()) : null;
    // zoomed: that pane takes the whole area; the other placed panes keep their rectangle (their key does
    // not change, so no redraw) and are hidden by class
    const zoomed = !!rect && state.zoom === id, behindZoom = !!rect && !!state.zoom && !zoomed;
    if (zoomed) rect = { x: 0, y: 0, w: 1, h: 1 };
    r.wrap.classList.toggle('zoomed', zoomed);
    r.wrap.classList.toggle('zoom-hidden', behindZoom);
    r.wrap.classList.toggle('placed', !!rect);
    r.wrap.classList.toggle('grid', !empty);
    if (rect) {
      r.wrap.style.top = `${rect.y * 100}%`; r.wrap.style.left = `${rect.x * 100}%`;
      r.wrap.style.width = `${rect.w * 100}%`; r.wrap.style.height = `${rect.h * 100}%`;
    } else { r.wrap.style.top = r.wrap.style.left = r.wrap.style.width = r.wrap.style.height = ''; }
    const key = rect ? `${rect.x},${rect.y},${rect.w},${rect.h}` : '';
    if (key && paneRectKey.has(id) && paneRectKey.get(id) !== key && layoutRestored && !state.restoring) changed.push(id);
    paneRectKey.set(id, key);
  }
  if (changed.length) queuePaneRedraw(changed);
  // the grid's own cell lines on the area (style.css #main.has-grid): without them an empty cell is just
  // black and a 3×3 with four sessions looks unbounded (2026-09-10); panes cover the lines under them
  const { R, C } = Layout.dims(GRID());
  mainEl.classList.toggle('has-grid', !empty);
  if (mainEl.style.getPropertyValue('--grid-rows') !== String(R)) mainEl.style.setProperty('--grid-rows', String(R));
  if (mainEl.style.getPropertyValue('--grid-cols') !== String(C)) mainEl.style.setProperty('--grid-cols', String(C));
  paintEmptySlot();
  const vis = visibleIds();
  // applyLayout is authoritative over what shows: in the empty layout the `.active` class is what makes a
  // terminal visible (`.term.active`), so sync it here rather than leaving it to activate() — an eviction
  // clears state.active without an activate() call, and a stale `.active` would keep the old tile on screen.
  for (const [id, r] of terms) r.wrap.classList.toggle('active', id === state.active);
  emptyEl.hidden = vis.some(id => terms.has(id));
  deck.setShown(vis.filter(id => terms.has(id)));   // hibernation's "on screen" set (main.js shownBy)
  requestAnimationFrame(fitActive);
  renderTiles();
}
// No saves before the boot restore has run, and none while main is still spawning restored tiles: either
// would write a layout missing the panes whose tiles have not arrived yet.
function persistLayout() { if (layoutRestored && !state.restoring && !stepSnap) deck.saveLayout(state.layout); }

// ---------------------------------------------------------------- stepping is a PREVIEW (2026-09-08)
// F11 / F12 (and Up / Down) walk the sessions; the selected pane shows each one as you pass, but nothing
// is committed — no save, no removal from where the session was — until an action that draws focus
// elsewhere: a click anywhere, a key that is not a step, the window losing focus. Stepping again first
// restores the layout the walk started from, so the preview is always "the starting layout plus this one
// step". The pane the stepped session would leave wears the leaving mark until the walk commits.
let stepSnap = null;   // { layout, active, emptySlot } from before the first step of the walk
function stepPreviewBegin() {
  if (!stepSnap) { stepSnap = { layout: state.layout, active: state.active, emptySlot: state.emptySlot }; return; }
  state.layout = stepSnap.layout; state.emptySlot = stepSnap.emptySlot;
}
let stepLeavingElsewhere = null;   // a tile another window holds, shown here by the walk: that window marks its pane
function stepPreviewMark() {
  if (stepLeavingElsewhere && stepLeavingElsewhere !== state.active) { deck.previewLeaving(stepLeavingElsewhere, false); stepLeavingElsewhere = null; }
  if (!stepSnap || !state.active) return hideLeaving();
  const was = Layout.occupancy(stepSnap.layout, state.active, GRID()), now = Layout.occupancy(state.layout, state.active, GRID());
  // it left a pane of this window for another one: mark the pane it is leaving
  if (was && was.length && now && now.length && !Layout.same(was, now, GRID())) showLeaving(was); else hideLeaving();
  // it is held in another window and one window per session: that window's pane is the one it would leave
  if (heldElsewhere(state.active) && oneWindow() && stepLeavingElsewhere !== state.active) { stepLeavingElsewhere = state.active; deck.previewLeaving(state.active, true); }
}
function stepPreviewEnd() {
  stepSnap = null; hideLeaving();
  if (stepLeavingElsewhere) { deck.previewLeaving(stepLeavingElsewhere, false); stepLeavingElsewhere = null; }
}
function stepPreviewCommit() {
  if (!stepSnap) return;
  stepPreviewEnd();
  persistLayout();
}
deck.onLeaving(({ id, on }) => { if (on && isPlaced(id)) showLeaving(Layout.occupancy(state.layout, id, GRID())); else hideLeaving(); });
// Esc during a walk (Settings, on by default): back to the session and layout the walk started from,
// and the Esc goes to no session — the first Esc only; the walk is over after it.
function stepPreviewCancel() {
  if (!stepSnap) return false;
  const s = stepSnap; stepPreviewEnd();
  state.layout = s.layout; state.emptySlot = s.emptySlot;
  activate(s.active, { adopt: false });
  return true;
}
const isStepKey = ev => ev.key === 'F11' || ev.key === 'F12' || ((ev.key === 'ArrowUp' || ev.key === 'ArrowDown') && typeof tileKeysLive === 'function' && tileKeysLive(ev.target));
window.addEventListener('mousedown', () => stepPreviewCommit(), true);
window.addEventListener('keydown', ev => {
  if (isStepKey(ev) || ['Shift', 'Control', 'Alt', 'Meta'].includes(ev.key)) return;
  if (ev.key === 'Escape' && stepSnap && state.settings.stepEscape !== false) { ev.preventDefault(); ev.stopImmediatePropagation(); stepPreviewCancel(); return; }
  stepPreviewCommit();
}, true);
window.addEventListener('blur', () => stepPreviewCommit());

// ---------------------------------------------------------------- the leaving mark (2026-09-08)
// "Lightweight but unambiguous" over the pane a session is about to be removed from: a dashed amber
// frame, faint diagonal stripes, a corner tag. Shown while a drag would MOVE a placed session (the pane
// it is leaving) and while a stepping walk has a session away from its pane, uncommitted.
const leavingEl = document.createElement('div');
leavingEl.id = 'leaving'; leavingEl.hidden = true; mainEl.appendChild(leavingEl);
function showLeaving(set) {
  const rect = Layout.rect(set, GRID()); if (!rect) return hideLeaving();
  leavingEl.style.top = `${rect.y * 100}%`; leavingEl.style.left = `${rect.x * 100}%`;
  leavingEl.style.width = `${rect.w * 100}%`; leavingEl.style.height = `${rect.h * 100}%`;
  leavingEl.hidden = false;
}
function hideLeaving() { leavingEl.hidden = true; }

// Tile `id` takes `set`. Everyone else follows F3 (lib/layout.js place): evicted back to the sidebar
// (R6, still running) or shrunk (R3). The placed tile becomes the selected one.
function placeTile(id, set) {
  if (!id || !terms.has(id)) return null;
  if (zoomBlocks()) return null;       // no hand changes to the grid while zoomed
  const r = Layout.place(state.layout, id, set, GRID());
  state.layout = r.layout;
  state.emptySlot = null;
  activate(id);                        // paints via applyLayout
  persistLayout();
  return r;
}
function unplaceTile(id) {
  if (!isPlaced(id)) return;
  if (zoomBlocks()) return;
  state.layout = Layout.unplace(state.layout, id);
  // the selected tile must stay visible: if it was the one removed, select a placed one (or, with the
  // layout now empty, it simply fills the area again)
  if (state.active === id && !Layout.isEmpty(state.layout)) activate(placedIds()[0]); else applyLayout();
  persistLayout();
}
function clearLayout() { if (zoomBlocks()) return; state.layout = {}; applyLayout(); persistLayout(); }

// ---------------------------------------------------------------- the selected EMPTY slot
// A click on an empty part of the grid selects that slot (bordered like a pane). Double-clicking an
// unplaced sidebar tile then fills THAT slot rather than adopting a pane — so an evicted region can be
// refilled without a drag and without displacing anyone. Cleared by any placement or by selecting a real pane.
state.emptySlot = null;      // cells, or null
const slotEl = document.createElement('div');
slotEl.id = 'slot-sel'; slotEl.hidden = true; mainEl.appendChild(slotEl);
function paintEmptySlot() {
  const set = state.emptySlot;
  const rect = set && Layout.rect(set, GRID());
  if (!rect || Layout.isEmpty(state.layout)) { slotEl.hidden = true; return; }
  slotEl.style.top = `${rect.y * 100}%`; slotEl.style.left = `${rect.x * 100}%`;
  slotEl.style.width = `${rect.w * 100}%`; slotEl.style.height = `${rect.h * 100}%`;
  slotEl.hidden = false;
}
function clearEmptySlot() { if (state.emptySlot) { state.emptySlot = null; paintEmptySlot(); } }
// the empty region to select for a point: every cell not held by any pane; if those form a rectangle
// that is the slot (a 2×2 with its bottom half free -> the whole half), else just the cell under the point
function emptySlotAt(px, py) {
  const g = GRID();
  const taken = new Set(); for (const set of Object.values(state.layout)) for (const k of set) taken.add(k);
  const free = Layout.fullSet(g).filter(k => !taken.has(k));
  const cell = Layout.cellAt(px, py, g)[0];
  if (!free.includes(cell)) return null;
  return Layout.isLegal(free, g) ? free : [cell];
}
// The first EMPTY region of the grid, in reading order: the largest rectangle of free cells that starts
// at the first free cell (all the free cells when they form one; otherwise that cell). Null when full.
function firstEmptyRegion() {
  const g = GRID();
  const taken = new Set(); for (const set of Object.values(state.layout)) for (const k of set) taken.add(k);
  const free = Layout.fullSet(g).filter(k => !taken.has(k));
  if (!free.length) return null;
  if (Layout.isLegal(free, g)) return free;
  // grow a rectangle from the first free cell: widen along the row, then deepen while every cell is free
  const { R, C } = Layout.dims(g);
  const r0 = Math.floor((free[0] - 1) / C), c0 = (free[0] - 1) % C;
  const isFree = (r, c) => r < R && c < C && !taken.has(r * C + c + 1);
  let w = 1; while (isFree(r0, c0 + w)) w++;
  let h = 1; while ([...Array(w).keys()].every(i => isFree(r0 + h, c0 + i))) h++;
  const out = []; for (let r = r0; r < r0 + h; r++) for (let c = c0; c < c0 + w; c++) out.push(r * C + c + 1);
  return out;
}
// A NEW session (new terminal, new Claude session, one adopted from past / elsewhere) never takes the
// selected pane (2026-09-10): with a layout in force it goes to the first empty region of the grid;
// with the grid full it is selected in the sidebar and placed by you (double-click, drag, chord); with
// no layout it shows full-bleed as always.
function placeNew(id) {
  if (!terms.has(id)) return;
  if (Layout.isEmpty(state.layout)) { activate(id); return; }
  const region = firstEmptyRegion();
  // zoomed: the new session takes its cell underneath, and the zoom stays on what you were looking at
  if (region && state.zoom) { state.layout = Layout.place(state.layout, id, region, GRID()).layout; applyLayout(); persistLayout(); return; }
  if (region) placeTile(id, region); else activate(id, { adopt: false });
}
mainEl.addEventListener('mousedown', ev => {
  if (Layout.isEmpty(state.layout)) return;
  if (ev.target.closest('.term')) return;             // a pane: activate() handles it
  const b = mainEl.getBoundingClientRect();
  const slot = emptySlotAt((ev.clientX - b.left) / b.width, (ev.clientY - b.top) / b.height);
  if (!slot) return;
  state.emptySlot = slot;
  paintEmptySlot();
});

// Called by activate() before it paints. In layout mode an unplaced tile that gets selected takes the
// selected EMPTY slot if there is one, else the pane of the tile that WAS selected (R12) — so clicking
// through the sidebar always shows what you clicked, and the layout keeps its shape.
function layoutAdopt(id, prev) {
  if (isPlaced(id) || !terms.has(id)) return;
  if (zoomBlocks()) return;            // adopting evicts a pane: not while zoomed
  if (state.emptySlot && !Layout.isEmpty(state.layout)) {
    const slot = state.emptySlot; state.emptySlot = null;
    state.layout = Layout.place(state.layout, id, slot, GRID()).layout;
    persistLayout();
    return;
  }
  // no layout here: the primary window shows the tile legacy-style (no placement); a secondary window is
  // layout-only, so selecting a tile there places it as this window's full pane. Either way, a tile that
  // lives in another window (heldElsewhere) is taken as a full pane, which evicts it there (main enforces
  // one home per tile on layout:set).
  if (Layout.isEmpty(state.layout)) {
    // held in another window and one window per session: selecting it here MOVES it here (placed full;
    // main evicts it there on save). Otherwise the legacy full-bleed view needs no placement.
    if (heldElsewhere(id) && oneWindow()) { state.layout = Layout.place({}, id, Layout.fullSet(GRID()), GRID()).layout; persistLayout(); }
    return;
  }
  const slot = Layout.occupancy(state.layout, prev, GRID()) || Layout.occupancy(state.layout, placedIds()[0], GRID());
  if (!slot) return;
  state.layout = Layout.place(Layout.unplace(state.layout, prev), id, slot, GRID()).layout;
  persistLayout();
}
// The meta tick removes closed tiles' terminals; their entries go too.
function pruneLayout() {
  if (state.restoring) return;
  const next = Layout.prune(state.layout, [...terms.keys()], GRID());
  if (Object.keys(next).sort().join() === Object.keys(state.layout).sort().join()) return;
  state.layout = next; applyLayout(); persistLayout();
}

// ---------------------------------------------------------------- F2: the digit chord
// Hold digits, release the last one, and the set you held is applied to the selected tile — `1`+`2` the
// top half, `1`+`3` the left half, a tap of `1` the top-left quadrant, `1`+`4` the whole window (R1).
// The gate (85-focus.js tileKeysLive) is checked on the way DOWN, per key; a digit that arrives with the
// gate closed is text for the terminal and is never accumulated. Auto-repeat is ignored. Losing window
// focus mid-chord abandons it: keyups the window never sees would otherwise leave keys "held" forever.
//
// A grid of more than nine cells (3×4, 4×3, 4×4 — the user raised the ceiling to 4×4 on 2026-10-05) has cells
// the digits cannot name, so there the LETTER BLOCK under the digits joins the chord, laid out like the
// grid: the keyboard rows 1 2 3 4 / Q W E R / A S D F / Z X C V are the grid's rows, by physical key. The
// digits keep naming cells 1–9 in reading order, and where the two overlap they agree (on a 4×4, Q is
// cell 5 and so is 5; V is cell 16). Bare letters only, behind the same gate: with Alt the letters are
// Claude Code's (Alt+V pastes an image on Windows; Alt+W, Alt+P, Alt+O, Alt+T are its toggles), so the
// Alt chord stays on the digits.
const CHORD_LETTERS = [null, ['KeyQ', 'KeyW', 'KeyE', 'KeyR'], ['KeyA', 'KeyS', 'KeyD', 'KeyF'], ['KeyZ', 'KeyX', 'KeyC', 'KeyV']];
// the cell a physical letter key names on the current grid; 0 when it is not a block letter, falls
// outside the grid, or the grid has nine cells or fewer (the digits already reach every one)
function letterCell(code) {
  const { R, C, N } = Layout.dims(GRID());
  if (N <= 9) return 0;
  for (let r = 1; r < CHORD_LETTERS.length; r++) {
    const c = CHORD_LETTERS[r].indexOf(code);
    if (c >= 0) return (r < R && c < C) ? Layout.cell(r, c, C) : 0;
  }
  return 0;
}
// `key` is the physical key ('Digit5', 'KeyQ'), so two keys naming one cell are still two held keys
const chord = { pressed: new Set(), held: new Set() };
function chordDown(key, cell) { chord.pressed.add(cell); chord.held.add(key); }
function chordUp(key) {
  if (!chord.held.has(key)) return;
  chord.held.delete(key);
  if (chord.held.size) return;
  const set = [...chord.pressed]; chord.pressed.clear();
  if (state.active) placeTile(state.active, set);
}
function chordAbandon() { chord.pressed.clear(); chord.held.clear(); }
// keyup by key, or by physical key for the Alt chord (with Alt down a Mac reports the Option glyph, not the
// digit); letters always by physical key. A key that was never held is ignored by chordUp.
window.addEventListener('keyup', ev => {
  const d = /^[1-9]$/.test(ev.key) ? ev.key : (/^Digit[1-9]$/.test(ev.code) ? ev.code.slice(5) : '');
  if (d) chordUp(`Digit${d}`); else if (/^Key[A-Z]$/.test(ev.code)) chordUp(ev.code);
}, true);
window.addEventListener('blur', chordAbandon);

// ---------------------------------------------------------------- F4: drag a tile onto the area
// The sidebar's own drag (reorder, pinned tiles only) and this one start the same way; they are told
// apart by where the drop lands, not by whether the drag may start (30-tiles.js). While a tile is over
// the area the preview shows the rectangle it WILL take and shades what it will not (R4 zones).
let dragSrcId = null;                // set by 30-tiles.js at dragstart, cleared at dragend
// A drag the browser accepted but never ran (Windows, 2026-09-08): dragstart, then not one drag event
// more — and the page is deaf to the mouse until the browser ends the drag it never started. A real
// drag delivers dragenter to the tile under the cursor at once, so "dragstart and nothing for a second"
// is that shape and nothing else. Ask main to cancel it (drag:rescue); dragend then arrives as usual,
// and main rebuilds this window — its drags would strand from now on (2026-10-20, main.js rebuildWindow).
// `drag` counts as an answer too: it fires on the source only while the OS drag loop is really running.
let dragWatchdog = null, dragRescued = false;
// The drag trace (lib/dragtrace.js): every drag reports where it started, so the next window whose drags
// die says why. What Chromium checks is the LAST pointer position this page saw before the drag began —
// not the press, and not dragstart's own coordinates, which repeat the press — so that is what is kept,
// with how long before dragstart it came and the gap before it (a busy page delivers moves late and far
// apart, and a late move can already sit outside the window). `deck.dragTrace` is missing in a window
// whose preload predates it (until the deck restarts); then nothing is traced.
const dragPtr = { down: null, move: null, gap: 0 };
document.addEventListener('pointerdown', ev => { dragPtr.down = dragPtr.move = [ev.screenX, ev.screenY, ev.clientX, ev.clientY, ev.timeStamp]; dragPtr.gap = 0; }, { capture: true, passive: true });
document.addEventListener('pointermove', ev => {
  if (!ev.buttons) return;
  if (dragPtr.move) dragPtr.gap = ev.timeStamp - dragPtr.move[4];
  dragPtr.move = [ev.screenX, ev.screenY, ev.clientX, ev.clientY, ev.timeStamp];
}, { capture: true, passive: true });
const traceDrag = rec => { if (deck.dragTrace) deck.dragTrace(rec); };
function dragContext(ev) {
  const now = performance.now(), m = dragPtr.move, d = dragPtr.down;
  const tileEl = ev.target && ev.target.closest ? ev.target.closest('.tile') : null;
  return {
    ptr: m ? [m[0], m[1]] : [ev.screenX, ev.screenY], ptrClient: m ? [m[2], m[3]] : [ev.clientX, ev.clientY],
    sinceMoveMs: m ? now - m[4] : null, moveGapMs: dragPtr.gap, sinceDownMs: d ? now - d[4] : null,
    view: [screenX, screenY, innerWidth, innerHeight], vis: document.visibilityState, hasFocus: document.hasFocus(),
    tile: tileEl ? tileEl.dataset.id : (ev.target && ev.target.nodeName) || null,
  };
}
let dragCtx = null;
document.addEventListener('dragstart', ev => {
  clearTimeout(dragWatchdog); dragRescued = false;
  dragCtx = dragContext(ev);
  traceDrag({ kind: 'start', ...dragCtx });
  dragWatchdog = setTimeout(() => {
    dragWatchdog = null; dragRescued = true;
    console.warn('drag: nothing followed dragstart — asking main to cancel the stranded drag');
    traceDrag({ kind: 'stranded', ...dragCtx });
    deck.dragRescue();
  }, 1200);
}, true);
for (const t of ['drag', 'dragenter', 'dragover', 'drop', 'dragend']) document.addEventListener(t, () => { if (dragWatchdog) { clearTimeout(dragWatchdog); dragWatchdog = null; } }, true);
document.addEventListener('dragend', ev => {
  if (!dragCtx) return;   // a drag from outside this page has no start here
  traceDrag({ kind: 'end', tile: dragCtx.tile, dropEffect: ev.dataTransfer ? ev.dataTransfer.dropEffect : null, endAt: [ev.screenX, ev.screenY], rescued: dragRescued });
  dragCtx = null;
}, true);
const dropPreview = document.createElement('div');
dropPreview.id = 'drop-preview'; dropPreview.hidden = true; mainEl.appendChild(dropPreview);
// The drop-model layout to reckon against (R15). With no layout yet, the deck is showing the selected
// session full-bleed, so treat it as an implicit full-window pane: dropping onto it splits it, the incoming
// tile taking the half you aimed at and the sitting one keeping the rest. `dropBase()` is that layout;
// `zoneAt` asks lib/layout.js what a point over it takes.
function dropBase() {
  if (!Layout.isEmpty(state.layout)) return state.layout;
  return (state.active && state.active !== dragSrcId && terms.has(state.active)) ? { [state.active]: Layout.fullSet(GRID()) } : {};
}
function zoneAt(ev) {
  const b = mainEl.getBoundingClientRect();
  const raw = Layout.dropTarget(dropBase(), (ev.clientX - b.left) / b.width, (ev.clientY - b.top) / b.height, GRID());
  // a tile already placed here shrinks / grows / moves (lib/layout.js resolveDrop); the preview shows which
  return (dragSrcId && isPlaced(dragSrcId)) ? Layout.resolveDrop(state.layout, dragSrcId, raw, GRID()) : raw;
}
// dragover fires many times a second; restyle only when the zone actually changes, or the preview
// re-lays out on every event and the drag feels laggy (it did — 2026-09-08).
let previewZone = null;
function showPreview(set) {
  const zone = Layout.key(set, GRID());
  if (zone === previewZone && !dropPreview.hidden) return;
  const rect = Layout.rect(set, GRID()); if (!rect) { hidePreview(); return; }
  dropPreview.style.top = `${rect.y * 100}%`; dropPreview.style.left = `${rect.x * 100}%`;
  dropPreview.style.width = `${rect.w * 100}%`; dropPreview.style.height = `${rect.h * 100}%`;
  dropPreview.dataset.zone = zone;
  dropPreview.hidden = false;
  previewZone = zone;
  // 2026-09-08: a preview spanning several cells hides the lines between the panes it covers
  // (a pane whose cells all lie inside the target); a placed source about to GROW gets its label pulsing
  // (.growing); a placed source about to MOVE wears the leaving mark on the pane it leaves.
  const target = Layout.norm(set, GRID());
  const inside = cells => cells.every(k => target.includes(k));
  for (const [id, r] of terms) {
    const occ = Layout.occupancy(state.layout, id, GRID());
    r.wrap.classList.toggle('under-preview', !!occ && occ.length > 0 && target.length > 1 && inside(occ));
  }
  const own = dragSrcId ? Layout.occupancy(state.layout, dragSrcId, GRID()) : null;
  const src = dragSrcId && terms.get(dragSrcId);
  const grows = !!(own && own.length && inside(own) && target.length > own.length);
  const moves = !!(own && own.length && !inside(own) && !own.every(k => target.includes(k)) && !target.every(k => own.includes(k)));
  if (src) src.wrap.classList.toggle('growing', grows);
  if (moves) showLeaving(own); else hideLeaving();
}
function hidePreview() {
  dropPreview.hidden = true; previewZone = null; hideLeaving();
  for (const r of terms.values()) { r.wrap.classList.remove('under-preview'); r.wrap.classList.remove('growing'); }
}
mainEl.addEventListener('dragover', ev => {
  if (!dragSrcId) return;
  if (state.zoom) { zoomNote(); return; }   // no preventDefault: the drop is refused, and the note says why
  ev.preventDefault(); ev.dataTransfer.dropEffect = 'move';
  showPreview(zoneAt(ev));
});
mainEl.addEventListener('dragleave', ev => { if (!mainEl.contains(ev.relatedTarget)) hidePreview(); });
mainEl.addEventListener('drop', ev => {
  if (!dragSrcId || state.zoom) return;
  ev.preventDefault();
  const id = dragSrcId, set = zoneAt(ev);
  // first drop onto a full-screen session: seed it as a full pane so the conflict rules split it with the
  // incoming tile rather than dropping it off-screen (dropBase treated it as full for the preview too)
  if (Layout.isEmpty(state.layout) && state.active && state.active !== id && terms.has(state.active)) {
    state.layout = { [state.active]: Layout.fullSet(GRID()) };
  }
  hidePreview(); dragSrcId = null;
  placeTile(id, set);
});

// ---------------------------------------------------------------- other windows (F5)
// A tile lives in one pane of one window. When another window takes a tile this one holds, main says so
// and it leaves this layout — no save back (main already knows), just the repaint.
deck.onLayoutEvict(ids => {
  const gone = (ids || []).filter(id => state.layout[id]);
  hideLeaving();                                   // whatever was marked as leaving has now left
  if (!gone.length) return;
  for (const id of gone) state.layout = Layout.unplace(state.layout, id);
  // A tile taken by another window is no longer THIS window's, so it must not stay selected here — the
  // meta that says so (inWindow) may not have arrived yet, and legacy mode would keep showing it. Move
  // to a pane that remains, or (nothing placed) clear the selection so the empty notice shows.
  if (gone.includes(state.active)) { const keep = placedIds()[0] || null; state.active = keep; if (keep) localStorage.setItem('deck.active', keep); }
  if (state.active && !Layout.isEmpty(state.layout) && !isPlaced(state.active)) activate(placedIds()[0]); else applyLayout();
});
// Main placed a tile in this window (2026-11-16: a desk the Claude on-call opened takes the first free cell in reading order;
// main.js openDeskRequest chose the cell and already holds it). The view places it WITHOUT selecting it: the pane the user is typing
// in keeps the keys. Under a zoom it lands underneath, as a new session does. An older main never sends it.
if (typeof deck.onLayoutPlace === 'function') deck.onLayoutPlace(({ id, cells } = {}) => {
  if (!id || !Array.isArray(cells) || !cells.length) return;
  if (!terms.has(id)) createTerm(id);
  state.layout = Layout.place(state.layout, id, cells, GRID()).layout;
  applyLayout();
  persistLayout();
});
// Drag a tile out of the window: a new deck window opens where it was dropped, with the tile filling it.
// Called from the tile's dragend (30-tiles.js) when the pointer let go outside this window's bounds.
function openInNewWindow(id, screenX, screenY) {
  return deck.openWindow({ tileId: id, x: screenX, y: screenY });
}

// The global grid shape changed (Settings -> Layout). The cell numbers mean something different now, so
// main has already cleared every window's layout; drop ours and repaint. The selected tile falls back to
// legacy full-bleed (primary window) or the empty notice (secondary).
deck.onGridChanged(grid => {
  const g = grid || GRID();
  state.grid = { rows: g.rows, cols: g.cols };
  // main sends this window's layout remapped onto the new shape (Layout.regrid): the placed sessions stay
  // where they were, the new cells are empty. A payload without one (an older main) means "cleared".
  state.layout = g.layout ? Layout.prune(g.layout, [...terms.keys()], state.grid) : {};
  state.emptySlot = null;
  applyLayout();
});

// ---------------------------------------------------------------- restore (R7)
function restoreLayout(saved) {
  // validate the shape only — tiles still arriving from the staggered restore are not "gone"; the first
  // meta tick after the restore ends prunes whatever really is
  state.layout = Layout.prune(saved, Object.keys(saved || {}), GRID());
  layoutRestored = true;
  // the remembered selection may not be in the restored layout (a tile closed since): select a placed
  // one rather than adopting it into someone's pane
  if (!Layout.isEmpty(state.layout) && !isPlaced(state.active)) activate(placedIds()[0]); else applyLayout();
}
