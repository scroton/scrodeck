'use strict';
/* global deck */
// Command palette (Ctrl+Shift+P, handled in the main process): jump to a tile, start a session, move,
// open settings, toggles. Type to filter, arrows to move, Enter to run, Esc to close.
const palette = document.getElementById('palette');
const paletteInput = document.getElementById('palette-input');
const paletteList = document.getElementById('palette-list');
let paletteItems = [], paletteIndex = 0;

function paletteCommands() {
  const items = [];
  for (const t of state.tiles) {
    const label = t.kind === 'shell' ? shellName() : `${acctLabel(t.account)}  ${t.title || 'New session'}`;
    items.push({ group: 'sessions', label, hint: t.status === 'busy' ? 'busy' : t.status === 'waiting' ? 'waiting for you' : '', run: () => activate(t.id) });
  }
  for (const a of state.accounts) items.push({ group: 'new', label: `New Claude session: ${a.label}`, hint: a.email || a.id, run: async () => { const id = await deck.newTile({ kind: 'claude', account: a.id }); createTerm(id); placeNew(id); } });
  if (state.browserOption) for (const a of state.accounts) items.push({ group: 'new', label: `New Claude session with the browser: ${a.label}`, hint: a.email || a.id, run: async () => { const id = await deck.newTile({ kind: 'claude', account: a.id, browser: true }); createTerm(id); placeNew(id); } });
  if (state.cloudOption) for (const a of state.accounts) items.push({ group: 'new', label: `New cloud session: ${a.label}`, hint: 'runs on claude.ai', run: () => openCloudDialog(a) });
  items.push({ group: 'new', label: `New ${shellName()}`, run: async () => { const id = await deck.newTile({ kind: 'shell' }); createTerm(id); placeNew(id); } });
  const cur = state.tiles.find(t => t.id === state.active);
  if (cur && cur.kind === 'claude') {
    for (const a of state.accounts.filter(a => a.id !== cur.account)) items.push({ group: 'this session', label: `Open in ${a.label}`, hint: 'confirm on the tile', run: () => { state.confirm[cur.id] = { kind: 'move', account: a.id }; actionSig.delete(cur.id); renderTiles(); } });
  }
  if (cur && cur.kind === 'cloud') {
    if (cur.cloudId) items.push({ group: 'this session', label: 'Reload the cloud session\'s page', run: () => { cloudRedraw(cur.id); deck.refreshTile(cur.id); } });
    if (cur.cloudUrl) items.push({ group: 'this session', label: 'Open the cloud session in the browser', run: () => openCloudOutside(cur) });
    items.push({ group: 'this session', label: 'Remove the tile', hint: 'the session keeps running', run: () => deck.closeTile(cur.id) });
    items.push({ group: 'this session', label: 'Rename tile', run: () => { const el = tileEls.get(cur.id); if (el) el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true })); } });
  } else if (cur) {
    items.push({ group: 'this session', label: 'Refresh session', hint: 'confirm on the tile', run: () => { state.confirm[cur.id] = { kind: 'refresh' }; actionSig.delete(cur.id); renderTiles(); } });
    if (cur.kind === 'claude' && 'browser' in cur) items.push({ group: 'this session', label: `Refresh session ${browserPhrase(cur)}`, hint: 'confirm on the tile', run: () => { state.confirm[cur.id] = { kind: 'refresh', browser: cur.browser !== true }; actionSig.delete(cur.id); renderTiles(); } });
    items.push({ group: 'this session', label: 'Close session', hint: 'confirm on the tile', run: () => { state.confirm[cur.id] = { kind: 'close' }; actionSig.delete(cur.id); renderTiles(); } });
    for (const p of pauseItems(cur)) items.push({ group: 'this session', label: `${p.label} session`, hint: p.hint, run: p.run });
    for (const a of auditorItems(cur, null, true)) items.push({ group: 'this session', label: a.label, hint: a.hint, run: a.run });   // auditors (30-tiles.js)
    items.push({ group: 'this session', label: 'Rename tile', run: () => { const el = tileEls.get(cur.id); if (el) el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true })); } });
  }
  if (typeof askSession === 'function') {          // the ask pane (49-asks.js)
    const kh2 = (state.platform && state.platform.keyHelp) || {};
    if (state.asks.length) items.push({ group: 'asks', label: `Answer every session's questions (${state.asks.length} waiting)`, hint: 'the asks tile', run: () => openAskPane('all') });
    if (cur && cur.kind === 'claude' && askSession(cur.sessionId)) items.push({ group: 'asks', label: 'Answer this session\'s questions', hint: kh2.asks || 'F7', run: () => openAskPane(cur.sessionId) });
    for (const s of state.asks) if (!cur || s.sessionId !== cur.sessionId) items.push({ group: 'asks', label: `Answer ${askName(s)}`, hint: `${askOpenCount(s)} waiting`, run: () => openAskPane(s.sessionId) });
    if (state.askLog.length) items.push({ group: 'asks', label: 'Recently answered', hint: `the last ${state.askLog.length}, read-only`, run: () => openAskPane('all', RECENT) });
  }
  if (!(state.edition && state.edition.moveAll === false)) items.push({ group: 'deck', label: 'Move every session…', run: () => openMoveMenu() });   // (not in a public native build: 70-meta.js)
  // Pause / Resume every session (main.js pauseAll / resumeAll)
  if (state.tiles.some(t => t.kind === 'claude' && !t.exited && t.status !== 'asleep' && t.status !== 'off' && !(t.pause && t.pause.state !== 'resuming'))) {
    items.push({ group: 'deck', label: 'Pause every session', hint: 'Esc, then the PAUSE prompt, in each', run: () => deck.pauseAll({}) });
    items.push({ group: 'deck', label: 'Pause every session until restart', hint: 'each resumed by itself at the next deck start', run: () => deck.pauseAll({ untilRestart: true }) });
  }
  const paused = state.tiles.filter(t => t.pause && (t.pause.state === 'pausing' || t.pause.state === 'paused') && !t.pause.want).length;
  if (paused) items.push({ group: 'deck', label: `Resume every paused session (${paused})`, hint: 'each when its turn ends', run: () => deck.resumeAll() });
  items.push({ group: 'deck', label: 'Search sessions', hint: 'local', run: () => openSearch() });
  items.push({ group: 'deck', label: 'Settings', run: () => openSettings() });
  // a remote desk only: this browser's choice (20-terminals.js deskCopyOnSelect)
  if (typeof deck === 'object' && deck && deck._desk) items.push({ group: 'deck', label: deskCopyOnSelectOn() ? 'Copy on select: turn off' : 'Copy on select: turn on', hint: deskCopyOnSelectOn() ? 'on: a selection is copied as you make it (this browser)' : 'off: Ctrl+C or right-click copies (this browser)', run: () => setDeskCopyOnSelect(!deskCopyOnSelectOn()) });
  if (typeof restartPaletteItems === 'function') items.push(...restartPaletteItems());   // 82-restart.js
  if (!Layout.isEmpty(state.layout) && !state.zoom) items.push({ group: 'deck', label: 'Reset layout', hint: 'one pane again', run: () => clearLayout() });
  const kh = (state.platform && state.platform.keyHelp) || { sidebar: 'F9', fullscreen: 'F10' };
  items.push({ group: 'deck', label: sidebar.hidden ? 'Show sidebar' : 'Hide sidebar', hint: kh.sidebar, run: () => setCollapsed(!sidebar.hidden) });
  items.push({ group: 'deck', label: 'Toggle fullscreen', hint: kh.fullscreen, run: () => deck.toggleFullscreen() });
  if (state.zoom) items.push({ group: 'deck', label: 'Back to the grid', hint: kh.zoom || 'F6', run: () => unzoom() });
  else if (state.active && isPlaced(state.active)) items.push({ group: 'deck', label: 'Zoom the selected pane', hint: kh.zoom || 'F6', run: () => zoomTo(state.active) });
  if (state.active) items.push({ group: 'deck', label: 'Redraw the selected pane', hint: kh.redraw || 'F5', run: () => redrawTile(state.active) });
  items.push({ group: 'deck', label: document.body.classList.contains('hide-msgs') ? 'Show recent messages' : 'Hide recent messages', run: () => setMsgsHidden(!document.body.classList.contains('hide-msgs')) });
  items.push({ group: 'deck', label: document.body.classList.contains('hide-bars') ? 'Show usage bars' : 'Hide usage bars', run: () => setBarsHidden(!document.body.classList.contains('hide-bars')) });
  return items;
}
function renderPalette() {
  const words = paletteInput.value.toLowerCase().split(/\s+/).filter(Boolean);
  paletteItems = paletteCommands().filter(it => words.every(w => (it.label + ' ' + (it.hint || '') + ' ' + it.group).toLowerCase().includes(w)));
  paletteIndex = Math.min(paletteIndex, Math.max(0, paletteItems.length - 1));
  paletteList.innerHTML = '';
  let lastGroup = null;
  paletteItems.forEach((it, i) => {
    if (it.group !== lastGroup) { const g = document.createElement('div'); g.className = 'pal-group'; g.textContent = it.group; paletteList.appendChild(g); lastGroup = it.group; }
    const row = document.createElement('div'); row.className = 'pal-row' + (i === paletteIndex ? ' sel' : '');
    const l = document.createElement('span'); l.className = 'pal-label'; l.textContent = it.label;
    const h = document.createElement('span'); h.className = 'pal-hint'; h.textContent = it.hint || '';
    row.append(l, h);
    row.addEventListener('mousemove', () => { if (paletteIndex !== i) { paletteIndex = i; renderPalette(); } });
    row.addEventListener('click', () => runPalette(i));
    paletteList.appendChild(row);
  });
  const sel = paletteList.querySelector('.pal-row.sel'); if (sel) sel.scrollIntoView({ block: 'nearest' });
}
function runPalette(i) { const it = paletteItems[i]; closePalette(); if (it) it.run(); }
function openPalette() { paletteInput.value = ''; paletteIndex = 0; palette.hidden = false; renderPalette(); paletteInput.focus(); }
function closePalette() { palette.hidden = true; const r = state.active && terms.get(state.active); if (r) r.term.focus(); }
paletteInput.addEventListener('input', () => { paletteIndex = 0; renderPalette(); });
paletteInput.addEventListener('keydown', ev => {
  if (ev.key === 'Escape') { ev.preventDefault(); closePalette(); }
  else if (ev.key === 'ArrowDown') { ev.preventDefault(); paletteIndex = Math.min(paletteItems.length - 1, paletteIndex + 1); renderPalette(); }
  else if (ev.key === 'ArrowUp') { ev.preventDefault(); paletteIndex = Math.max(0, paletteIndex - 1); renderPalette(); }
  else if (ev.key === 'Enter') { ev.preventDefault(); runPalette(paletteIndex); }
});
palette.addEventListener('click', ev => { if (ev.target === palette) closePalette(); });
deck.onPalette(() => { if (palette.hidden) openPalette(); else closePalette(); });
