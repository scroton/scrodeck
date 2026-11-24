'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- UI settings → CSS / body classes
let uiSig = '';
let lastMirrorSig = '';
let mirroredIds = new Set();      // the sessions this screen was last told to mirror (another screen drives their size)
function applyUiSettings(s) {
  if (s.asks) state.settings.asks = s.asks;          // Settings -> Asks (49-asks.js: the tile, the marks, Done)
  const sig = JSON.stringify([s.effortStyle, s.sidebarSide, s.pinSide, s.bars, s.barLabels, s.barLabelMode, s.msgLines, s.animations, s.driver, s.oneWindow, s.stepEscape]);
  if (sig === uiSig) return;
  uiSig = sig;
  state.settings.oneWindow = s.oneWindow !== false;
  state.settings.stepEscape = s.stepEscape !== false;
  document.body.classList.toggle('anim-on', s.animations !== false);   // gates the rainbow drift and the ultracode pulse
  state.settings.effortStyle = s.effortStyle || 'tag';
  state.settings.bars = s.bars || DEFAULT_BARS;
  state.settings.barLabels = s.barLabels || { time: true, day: true };
  state.settings.barLabelMode = s.barLabelMode || 'reset';
  state.settings.msgLines = s.msgLines || { normal: 3, wide: 10 };
  document.documentElement.style.setProperty('--msg-lines', String(state.settings.msgLines.normal));
  document.documentElement.style.setProperty('--msg-lines-wide', String(state.settings.msgLines.wide));
  document.body.classList.toggle('sb-right', s.sidebarSide === 'right');
  document.body.classList.toggle('pin-centre', s.pinSide === 'centre');
  state.settings.driver = driverMode(s.driver);       // 80-sidebar.js
  paintDrive();
  document.body.classList.toggle('effort-tag', state.settings.effortStyle === 'tag');
  document.body.classList.toggle('effort-color', state.settings.effortStyle === 'color');
  if (!moveMenu.hidden) refreshMoveBars();
}
document.body.classList.add('effort-tag');

// ---------------------------------------------------------------- meta stream
deck.onMeta(meta => applyMeta(meta));
deck.onActivate(id => { if (state.tiles.some(t => t.id === id)) activate(id); });   // a clicked notification toast
// main asks what the trust prompt looks like on a tile's SCREEN right now (see probeTrust in main.js)
deck.onScreenProbe(({ id, req }) => {
  const r = terms.get(id);
  if (!r) { deck.screenProbeReply({ req, id, unknown: true }); return; }
  const b = r.term.buffer.active;
  let prompt = false, onYes = false, onNo = false;
  for (let i = 0; i < b.length; i++) {
    const l = b.getLine(i); if (!l) continue;
    const s = l.translateToString(true);
    if (!s) continue;
    if (/Quick\s*safety\s*check|Yes,\s*I\s*trust\s*this\s*folder|Enter to confirm/.test(s)) prompt = true;
    if (/[❯>]\s*Yes,\s*I\s*trust/.test(s)) onYes = true;
    if (/[❯>]\s*No,\s*exit/.test(s)) onNo = true;
  }
  deck.screenProbeReply({ req, id, prompt, onYes, onNo });
});

// The pty has drifted from a pane THIS screen drives (meta says so: sizeBy is this kind of screen and there is
// no mirror), e.g. a remote desk's tab was hidden, the office resized the session meanwhile, and the tab is
// back with its panes the size they were — nothing changed here, so nothing was pushed. Put it back, at most
// once per session every 2.5 s (the phone page does the same: reassertSizeIfDrifted).
const reassertAt = new Map();
function reassertDriven() {
  if (typeof visibleIds !== 'function' || !(oneWindow() || document.hasFocus())) return;
  for (const id of visibleIds()) {
    const t = state.tiles.find(x => x.id === id), r = terms.get(id);
    if (!t || !r || t.sizeBy !== DRIVE_ME || mirrorOf(id) || !t.ptySize || zoomHidden(id)) continue;
    const { cols, rows } = r.term;
    if (!(cols > 1 && rows > 1) || (t.ptySize.cols === cols && t.ptySize.rows === rows)) continue;
    if (Date.now() - (reassertAt.get(id) || 0) < 2500) continue;
    reassertAt.set(id, Date.now());
    deck.resize(id, cols, rows);
  }
}
function applyMeta(meta) {
  // the window title (a session's name, or the count of open sessions) is painted by renderTiles below
  state.accounts = meta.accounts; state.tiles = meta.tiles; state.usage = meta.usage; state.now = meta.now;
  measureSidebarMin(state.accounts);   // the sidebar's narrowest follows the tile strip (80-sidebar.js); measured only when the accounts change
  state.asks = Array.isArray(meta.asks) ? meta.asks : [];   // sessions' ask pages waiting on you (49-asks.js)
  state.askLog = Array.isArray(meta.askLog) ? meta.askLog : [];   // the last pages you answered, newest first
  state.groups = Array.isArray(meta.groups) ? meta.groups : [];
  // the connected remote desk and the name it gave, if any: the driver button names it (80-sidebar.js)
  const rdBefore = JSON.stringify(state.remoteDesk || null);
  state.remoteDesk = meta.remoteDesk || null;
  if (JSON.stringify(state.remoteDesk) !== rdBefore) paintDrive();
  state.presence = meta.presence || null;   // where the user was last active, as the hosted sessions read it (lib/presence.js)
  if (meta.settings && (meta.settings.zoomFontSize || 0) !== (state.settings.zoomFontSize || 0)) {
    state.settings.zoomFontSize = meta.settings.zoomFontSize || 0; requestAnimationFrame(fitActive);
  }
  if (meta.settings && meta.settings.fontSize && meta.settings.fontSize !== state.settings.fontSize) {
    state.settings.fontSize = meta.settings.fontSize; applyFontSize(meta.settings.fontSize);
  }
  if (meta.settings && (meta.settings.fontFamily || '') !== (state.settings.fontFamily || '')) {
    state.settings.fontFamily = meta.settings.fontFamily || ''; applyFontFamily(state.settings.fontFamily);
  }
  if (meta.settings) applyUiSettings(meta.settings);
  if (meta.settings && meta.settings.grid) state.grid = meta.settings.grid;
  if (Array.isArray(meta.triggers)) state.triggers = meta.triggers;
  if (typeof paintRestart === 'function') paintRestart(meta.restart);   // 82-restart.js
  // a phone attaching or leaving changes who owns the active terminal's size
  const mirrorSig = JSON.stringify((meta.tiles || []).map(t => [t.id, t.mirror && t.mirror.cols, t.mirror && t.mirror.rows]));
  if (mirrorSig !== lastMirrorSig) {
    lastMirrorSig = mirrorSig;
    // A session whose mirror just went away is one THIS screen now drives (the claim came here, or the screen
    // that drove it left): after the refit, say its size out loud. The fit alone only tells the pty when the
    // terminal's size CHANGES, and it does not when the mirror was never drawn (a window that got no frame
    // while it was minimised) — the pty would then stay at the other screen's size under a pane of ours.
    const now = new Set((meta.tiles || []).filter(t => t.mirror).map(t => t.id));
    const taken = [...mirroredIds].filter(id => !now.has(id));
    mirroredIds = now;
    // On the next frame, or 250 ms on when there is none: a minimised or covered window gets no frames, and
    // the size it now owns must still reach the pty (the same reason redrawTile uses a timer).
    let done = false;
    const run = () => {
      if (done) return; done = true;
      fitActive();
      for (const id of taken) {
        const r = terms.get(id);
        if (r && isVisible(id) && !mirrorOf(id) && (oneWindow() || document.hasFocus()) && r.term.cols > 1 && r.term.rows > 1) deck.resize(id, r.term.cols, r.term.rows);
      }
    };
    requestAnimationFrame(run); setTimeout(run, 250);
  } else reassertDriven();
  state.restoring = !!meta.restoring;
  state.browserOption = !!meta.browserOption;   // the main process takes { browser } on new / refresh / move
  state.cloudOption = !!meta.cloudOption;       // the main process makes and holds cloud sessions (48-cloud.js)
  // only a PUBLIC native build sends meta.edition ({ name }: core/deck/edition.hpp); this deck and the owner's native one send none
  state.edition = meta.edition && typeof meta.edition === 'object' ? meta.edition : null;
  // ... whose { moveAll: false } hides the move-every-session button (and its palette item, 88-palette.js)
  { const off = !!(state.edition && state.edition.moveAll === false), b = document.getElementById('moveall-btn');
    if (b) b.style.display = off ? 'none' : '';
    if (off && !moveMenu.hidden) moveMenu.hidden = true; }
  if (meta.settings && meta.settings.cwd) state.settings.cwd = meta.settings.cwd;   // the cloud dialog's first folder
  const ids = new Set(meta.tiles.map(t => t.id));
  let created = false;
  for (const t of meta.tiles) if (!terms.has(t.id)) { createTerm(t.id); created = true; }
  for (const id of [...terms.keys()]) if (!ids.has(id)) disposeTerm(id);
  // a tile arriving late (the staggered restore) may already have a place in the layout: paint it.
  // Pruning waits until the restore is over, or a pane whose tile has not spawned yet would be dropped.
  if (created && !Layout.isEmpty(state.layout)) applyLayout();
  if (!state.restoring) pruneLayout();
  if (!state.active || !ids.has(state.active)) {
    // the selected tile is gone. In a layout, select a remaining pane. With no layout, only the PRIMARY
    // window falls back to the top sidebar tile (legacy full-bleed); a secondary window is layout-only, so
    // it stays empty rather than auto-placing a tile the user never dropped there (25-layout.js).
    const first = !Layout.isEmpty(state.layout) ? state.tiles.find(t => t.id === placedIds()[0])
      : ((state.windowIndex || 0) === 0 ? meta.tiles[0] : null);
    if (first) activate(first.id); else { state.active = null; applyLayout(); }
  } else {
    const r = terms.get(state.active); if (r && !r.wrap.classList.contains('active')) activate(state.active);
  }
  renderNewTile();
  renderTiles();
  if (!moveMenu.hidden) refreshMoveBars();
}
