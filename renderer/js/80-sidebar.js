'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- sidebar collapse + resize
function setCollapsed(v) {
  sidebar.hidden = v; rail.hidden = !v;
  if (!v) requestAnimationFrame(() => measureSidebarMin(state.accounts));   // back from the rail: the accounts may have changed meanwhile
  localStorage.setItem(lsKey('deck.collapsed'), v ? '1' : '0');   // per window (R13): an auxiliary remembers its own
  requestAnimationFrame(fitActive);
}
$('#collapse').addEventListener('click', () => setCollapsed(true));
rail.addEventListener('click', () => setCollapsed(false));   // the whole rail, not just the arrow
if (localStorage.getItem('deck.collapsed') === '1') setCollapsed(true);
deck.onSidebarToggle(() => setCollapsed(!sidebar.hidden));   // Ctrl+` / F9 from the main process
// Previous / next tile in sidebar order, wrapping. Two ways in: F11 / F12 (Cmd+Shift+[ ] on a Mac) come
// from the main process and work whatever has focus; Up / Down (85-focus.js) are renderer-only and live
// only while the deck's chrome has the keyboard, because in the terminal the arrows are Claude Code's
// history navigation.
function stepTile(delta) {
  if (zoomBlocks()) return;                // the walk places: not while zoomed (25-layout.js)
  const ids = state.tiles.map(t => t.id); if (!ids.length) return;
  const i = ids.indexOf(state.active);
  const next = ids[((i < 0 ? 0 : i) + delta + ids.length) % ids.length];
  stepPreviewBegin();                      // a walk: shown, not committed (25-layout.js)
  // The walk's HOME is the pane (or empty slot) that was selected when it began: a session that is not
  // on screen previews there, whatever was stepped past on the way. One already on screen is simply
  // selected where it is. activate() adopts against `prev`, so set prev to the home before each step.
  state.active = stepSnap.active;
  activate(next);
  stepPreviewMark();
}
deck.onTileStep(stepTile);

// The narrowest the sidebar may be (2026-11-23: "min width to display the buttons"; a fixed 220 until then): the
// widest tile strip at its natural size - the sleep icon, one "↪ <label>" per other account, refresh, the close icon,
// as renderActions builds them, at 11px with nothing shrunk - plus the tile's padding and border and the list's
// padding; never under SB_FLOOR (the title line). Measured from real buttons in a hidden tile, again whenever the
// accounts or their labels change (applyMeta) and when the sidebar comes back from the rail. css #sidebar's
// min-width reads it as --sb-min. windows/src/sidebar.cpp Sidebar::minWidth is the same rule for the exe.
const SB_FLOOR = 160;
let sbMin = SB_FLOOR, sbMinSig = null;
function measureSidebarMin(accounts) {
  const accts = Array.isArray(accounts) ? accounts.filter(a => a && a.id) : [];
  const sig = JSON.stringify(accts.map(a => [a.id, a.label]));
  if (sig === sbMinSig || sidebar.hidden) return sbMin;   // collapsed: nothing lays out; measured on the way back
  const list = $('#tiles'), probe = document.createElement('div'), box = document.createElement('div');
  probe.className = 'tile'; probe.style.cssText = 'position:absolute;left:0;top:0;width:max-content;visibility:hidden;pointer-events:none';
  box.className = 't-actions'; probe.appendChild(box);
  for (const own of accts.length ? accts : [{}]) {   // the strip of a tile on each account: every OTHER account's move
    const row = document.createElement('div'); row.className = 'btn-row';
    const add = (label, icon) => { const b = document.createElement('button'); if (icon) { b.className = 'icon'; b.innerHTML = TILE_ICONS[icon]; } else b.textContent = label; row.appendChild(b); };
    add('', 'sleep'); for (const a of accts) if (a.id !== own.id) add(`↪ ${a.label}`); add('refresh'); add('', 'close');
    box.appendChild(row);
  }
  sidebar.appendChild(probe);
  const cs = getComputedStyle(list), w = probe.getBoundingClientRect().width;
  probe.remove();
  if (!(w > 0)) return sbMin;   // not laid out (yet): asked again at the next meta
  sbMinSig = sig;
  sbMin = Math.max(SB_FLOOR, Math.ceil(w + parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight)));
  document.documentElement.style.setProperty('--sb-min', `${sbMin}px`);
  return sbMin;
}

// a web font that arrives later changes the words' widths
if (document.fonts && document.fonts.ready) document.fonts.ready.then(() => { sbMinSig = null; measureSidebarMin(state.accounts); });

const savedW = parseInt(localStorage.getItem('deck.sbwidth') || '', 10);
if (savedW) document.documentElement.style.setProperty('--sb-width', `${savedW}px`);
(() => {
  const handle = $('#sb-resize'); let dragging = false;
  handle.addEventListener('mousedown', ev => { dragging = true; ev.preventDefault(); document.body.style.cursor = 'col-resize'; });
  window.addEventListener('mousemove', ev => {
    if (!dragging) return;
    // mirrored: the handle is on the sidebar's inner edge, so the width grows the other way
    const x = document.body.classList.contains('sb-right') ? window.innerWidth - ev.clientX : ev.clientX;
    const w = Math.max(sbMin, Math.min(window.innerWidth * 0.7, x));
    document.documentElement.style.setProperty('--sb-width', `${w}px`);
  });
  window.addEventListener('mouseup', () => {
    if (!dragging) return; dragging = false; document.body.style.cursor = '';
    localStorage.setItem('deck.sbwidth', String(sidebar.getBoundingClientRect().width | 0));
    fitActive();
  });
})();

// recent-message boxes on / off (remembered per viewer)
function setMsgsHidden(v) {
  document.body.classList.toggle('hide-msgs', v);
  $('#msgs-btn').classList.toggle('off', v);
  localStorage.setItem('deck.hidemsgs', v ? '1' : '0');
}
$('#msgs-btn').addEventListener('click', () => setMsgsHidden(!document.body.classList.contains('hide-msgs')));

// Who drives the size: a CLAIM by one kind of screen (lib/driverrule.js has the rule and its table; design
// 2026-11-18). settings.driver is 'office' (the deck's own windows), 'phone', 'remote' (the remote desk) or
// 'auto' (whoever is looking: a phone attached to a session, else a remote desk showing it, else the office).
// The button is the same on every screen: its ICON is who drives, its COLOUR whether that is this screen
// (green: here; grey: another screen; plain: auto). ONE click claims the size for this screen; a click
// while this screen already has it releases to auto. Settings has the same four by name.
const DRIVE_ME = (typeof deck === 'object' && deck && deck._desk) ? 'remote' : 'office';   // the kind of screen this page is
function driverMode(v) { return v === 'desk' ? 'office' : (['auto', 'office', 'phone', 'remote'].includes(v) ? v : 'auto'); }   // 'desk': the word before the claim
function driveName(kind) {
  if (kind === 'office') return DRIVE_ME === 'office' ? 'this PC (the office deck)' : 'the office deck';
  if (kind === 'phone') return 'the phone';
  const n = state.remoteDesk && state.remoteDesk.name;
  return (DRIVE_ME === 'remote' ? 'this remote desk' : 'the remote desk') + (n ? ` (${n})` : '');
}
function driveTitle(m) {
  if (m === 'auto') return `Auto: a phone attached to a session drives its size, else a remote desk showing it, else ${driveName('office')}. Click to drive from here.`;
  const what = m === 'phone' ? ' Every other screen shows its size, and typing at the office deck is off.'
    : m === 'remote' ? ' That is for the sessions it shows: there the office deck shows its size and cannot type. The office deck sizes the rest.'
    : ' The phone and a remote desk show its size.';
  return `Sizes follow: ${driveName(m)}.${what} ` + (m === DRIVE_ME ? 'Click to let whoever is looking drive (auto).' : 'Click to drive from here.');
}
function paintDrive() {
  const b = $('#drive-btn'); if (!b) return;
  const m = driverMode(state.settings.driver);
  b.dataset.driver = m;                                    // the icon: who drives
  b.classList.toggle('drive-mine', m === DRIVE_ME);        // green: this screen
  b.classList.toggle('drive-other', m !== 'auto' && m !== DRIVE_ME);
  b.classList.toggle('drive-auto', m === 'auto');
  b.title = driveTitle(m);
}
$('#drive-btn').addEventListener('click', () => {
  const m = driverMode(state.settings.driver);
  deck.setDriver(m === DRIVE_ME ? 'auto' : DRIVE_ME).then(v => { state.settings.driver = v; paintDrive(); });
});
if (localStorage.getItem('deck.hidemsgs') === '1') setMsgsHidden(true);

// usage bars on / off (remembered per viewer), same idea as the message toggle
function setBarsHidden(v) {
  document.body.classList.toggle('hide-bars', v);
  $('#bars-btn').classList.toggle('off', v);
  localStorage.setItem('deck.hidebars', v ? '1' : '0');
}
$('#bars-btn').addEventListener('click', () => setBarsHidden(!document.body.classList.contains('hide-bars')));
if (localStorage.getItem('deck.hidebars') === '1') setBarsHidden(true);

// F12 / F11 fullscreen is handled in the main process (before-input-event), wherever focus is.
