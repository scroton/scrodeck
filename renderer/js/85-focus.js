'use strict';
/* global deck */
// The deck has a two-state focus model: the keyboard belongs to the deck's CHROME (sidebar, buttons,
// nothing in particular) until Enter or Space hands it to the shown terminal, and from then on every key
// is the terminal's until focus leaves it again. Everything keyed on that split lives here, and every
// key that depends on it asks the same gate:
//
//   Enter / Space   chrome -> put the cursor in the shown terminal
//   Up / Down       chrome, sidebar open, a tile selected -> previous / next tile        (F7)
//   1 … 9           the same gate -> the cell chord (25-layout.js)                       (F2, R2)
//   Q W E R / A S D F / Z X C V   the same gate, grids over nine cells only -> the same chord
//   Alt+digit       no gate: the same chord from anywhere, the terminal included (digits only)
//
// The gate is the whole feature. In the terminal the arrows are Claude Code's history navigation and the
// digits are text; a gate that leaks lets the deck eat keys the user meant for the session. Anything that
// wants a chrome-only key reuses `chromeHasKeyboard` / `tileKeysLive` rather than writing a second guard.

// A panel that owns the keyboard is open: settings, the move menu, elsewhere/past, search, the palette.
function panelOpen() {
  const open = id => { const el = document.getElementById(id); return el && !el.hidden; };
  return open('settings') || open('moveall-menu') || open('pull-menu') || open('search-pane') || open('palette');
}
// Does the deck's chrome hold the keyboard for this event? False when focus is in a control (a rename
// box, the hold form, a button), inside a terminal, or a panel is open.
function chromeHasKeyboard(target) {
  const t = target;
  const tag = ((t && t.tagName) || '').toLowerCase();
  if (['input', 'textarea', 'select', 'button', 'a', 'label'].includes(tag) || (t && t.isContentEditable)) return false;
  if (t && t.closest && t.closest('.xterm')) return false;
  if (panelOpen()) return false;
  return true;
}
// R2's gate, shared by F7's arrows and F2's digits: chrome has the keyboard AND the sidebar is open AND
// a tile is selected. With the sidebar collapsed there is nothing to step through or place.
function tileKeysLive(target) {
  return chromeHasKeyboard(target) && !sidebar.hidden && !!state.active;
}

window.addEventListener('keydown', ev => {
  // With Alt the chord works from ANYWHERE, the terminal included (2026-09-08: selecting a tile puts
  // the cursor in its terminal, so bare digits there are text for Claude Code, never a placement). By
  // physical key: with Alt down a Mac reports the Option glyph, not the digit. Before the modifier bail-out.
  if (ev.altKey && !ev.ctrlKey && !ev.metaKey && !ev.shiftKey && /^Digit[1-9]$/.test(ev.code)) {
    if (ev.repeat) return;
    ev.preventDefault();
    chordDown(ev.code, Number(ev.code.slice(5)));
    return;
  }
  if (ev.ctrlKey || ev.altKey || ev.metaKey) return;
  if (ev.key === 'Enter' || ev.key === ' ') {
    if (!chromeHasKeyboard(ev.target)) return;
    const r = state.active && terms.get(state.active);
    if (!r) return;
    ev.preventDefault();
    r.term.focus();
    return;
  }
  // F7: Up / Down step tiles in sidebar order, wrapping — the same step F11 / F12 make from any focus.
  // preventDefault, or the arrow also scrolls the sidebar under the tile that activate() just scrolled
  // into view.
  if (ev.key === 'ArrowUp' || ev.key === 'ArrowDown') {
    if (ev.shiftKey || !tileKeysLive(ev.target)) return;
    ev.preventDefault();
    stepTile(ev.key === 'ArrowUp' ? -1 : 1);
    return;
  }
  // F2 / R2: bare digits place the selected tile — held together they make a half or the whole window.
  // Gated per key on the way down; the release side is in 25-layout.js. Auto-repeat is not a new press.
  if (/^[1-9]$/.test(ev.key)) {
    if (ev.repeat || ev.shiftKey || !tileKeysLive(ev.target)) return;
    ev.preventDefault();
    chordDown(`Digit${ev.key}`, Number(ev.key));
    return;
  }
  // On a grid of more than nine cells the letter block under the digits joins the chord, by physical key
  // (25-layout.js letterCell). Same gate as the digits; a letter outside the grid is left alone.
  const lc = letterCell(ev.code);
  if (lc) {
    if (ev.repeat || ev.shiftKey || !tileKeysLive(ev.target)) return;
    ev.preventDefault();
    chordDown(ev.code, lc);
  }
}, true);
