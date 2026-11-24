'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- clock + boot
function tickClock() { clockEl.textContent = fmtClock(new Date()); }
tickClock(); setInterval(tickClock, 1000);
setInterval(() => { state.now = Date.now(); for (const t of state.tiles) { const el = tileEls.get(t.id); if (el) setText(el.querySelector('.t-age'), fmtAge(state.now - (t.lastAt || state.now))); } }, 15000);

deck.init().then(({ meta, platform, layout, windowIndex, grid }) => {
  state.platform = platform || {};
  state.windowIndex = windowIndex || 0;
  if (state.windowIndex) {         // an auxiliary window (R13) remembers its own selection and sidebar state
    state.active = localStorage.getItem(lsKey('deck.active')) || null;
    setCollapsed(localStorage.getItem(lsKey('deck.collapsed')) === '1');
  }
  if (grid) state.grid = grid;     // the layout grid shape (R16) before anything is painted
  newTile.title = `New ${shellName()} terminal`;   // "New PowerShell terminal" / "New zsh terminal"
  applyMeta(meta);                 // creates the terminals the layout refers to
  restoreLayout(layout || {});     // R7: layout occupancies come back with the tiles
});
