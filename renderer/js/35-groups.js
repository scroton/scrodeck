'use strict';
/* global deck */
// Groups: single-line divider bars in the tile list. "pinned" comes first (open by default), then each
// named group in order, then "ungrouped". Several groups can be open at once; collapse state is
// remembered per viewer. Grouped view is a toggle (bottom-row button, left click); right click on that
// button creates a group. Tiles join a group from their right-click menu.
const groupEls = new Map();      // divider name -> element
let groupedView = localStorage.getItem('deck.grouped') === '1';
let collapsedGroups = new Set();
try { collapsedGroups = new Set(JSON.parse(localStorage.getItem('deck.groups.collapsed') || '[]')); } catch { /* fresh */ }
function saveCollapsed() { localStorage.setItem('deck.groups.collapsed', JSON.stringify([...collapsedGroups])); }

function dividerEl(name, count) {
  let el = groupEls.get(name);
  if (!el) {
    el = document.createElement('div'); el.className = 'group-bar'; el.dataset.group = name;
    el.innerHTML = '<span class="g-chev"></span><span class="g-name"></span><span class="g-count"></span>';
    el.addEventListener('click', ev => {
      ev.stopPropagation();
      if (collapsedGroups.has(name)) collapsedGroups.delete(name); else collapsedGroups.add(name);
      saveCollapsed(); layoutGroups();
    });
    el.addEventListener('contextmenu', ev => { ev.preventDefault(); ev.stopPropagation(); if (name !== 'pinned' && name !== 'ungrouped') openGroupMenu(ev, name); });
    groupEls.set(name, el);
  }
  el.querySelector('.g-name').textContent = name;
  el.querySelector('.g-count').textContent = String(count);
  el.classList.toggle('collapsed', collapsedGroups.has(name));
  return el;
}

// Called at the end of renderTiles. In grouped view, reorders the tile elements under divider bars and
// hides the tiles of collapsed groups (on top of the search filter).
function layoutGroups() {
  const btn = document.getElementById('groups-btn'); if (btn) btn.classList.toggle('on', groupedView);
  if (!groupedView) { for (const el of groupEls.values()) el.remove(); for (const t of state.tiles) { const el = tileEls.get(t.id); if (el) el.classList.remove('in-collapsed'); } return; }
  const pinned = state.tiles.filter(t => t.pinned);
  const byGroup = new Map((state.groups || []).map(g => [g, []]));
  const ungrouped = [];
  for (const t of state.tiles) {
    if (t.pinned) continue;
    if (t.group && byGroup.has(t.group)) byGroup.get(t.group).push(t); else ungrouped.push(t);
  }
  const sections = [['pinned', pinned], ...[...byGroup.entries()], ['ungrouped', ungrouped]];
  const order = [];
  for (const [name, list] of sections) {
    if (!list.length && name !== 'ungrouped' && name !== 'pinned') { const stale = groupEls.get(name); if (stale) stale.remove(); continue; }
    if (!list.length) { const stale = groupEls.get(name); if (stale) stale.remove(); continue; }
    order.push(dividerEl(name, list.length));
    const hide = collapsedGroups.has(name);
    for (const t of list) { const el = tileEls.get(t.id); if (!el) continue; el.classList.toggle('in-collapsed', hide); order.push(el); }
  }
  for (const [name, el] of groupEls) if (!sections.some(([n, l]) => n === name && l.length)) el.remove();
  order.forEach((el, i) => { if (tilesEl.children[i] !== el) tilesEl.insertBefore(el, tilesEl.children[i] || null); });
}

// ---- small context menus (tile right-click, group bar right-click)
// An item: { label, run, checked, dim } or '-'; an item with `sub` (a list of items) opens a submenu in the menu's
// place (Right, Enter or a click), and Left goes back to the menu it came from. The keyboard (2026-11-23):
// Up / Down move, Enter picks, Esc closes, Right opens a submenu, Left closes it; opts.keyboard (Shift+F10, the Menu
// key) lights the first item at once.
let ctxMenu = null, ctxLevel = null;   // ctxLevel: { items, rows, active, at, parent }
function closeCtxMenu() { if (ctxMenu) { ctxMenu.remove(); ctxMenu = null; } ctxLevel = null; }
function showCtxMenu(ev, items, opts = {}) {
  closeCtxMenu();
  const m = document.createElement('div'); m.className = 'ctx-menu';
  const level = { items, rows: [], active: -1, at: { clientX: ev.clientX, clientY: ev.clientY, screenX: ev.screenX, screenY: ev.screenY }, parent: opts.parent || null };
  items.forEach((it, i) => {
    if (it === '-') { const hr = document.createElement('div'); hr.className = 'ctx-sep'; m.appendChild(hr); return; }
    const row = document.createElement('div'); row.className = 'ctx-item' + (it.checked ? ' checked' : '') + (it.dim ? ' dim' : '') + (it.sub ? ' sub' : '');
    row.textContent = it.label;
    row.addEventListener('click', e => { e.stopPropagation(); ctxPick(i); });
    row.addEventListener('mousemove', () => ctxLight(i));
    m.appendChild(row); level.rows[i] = row;
  });
  document.body.appendChild(m); ctxMenu = m; ctxLevel = level;
  const x = Math.min(ev.clientX, window.innerWidth - m.offsetWidth - 6), y = Math.min(ev.clientY, window.innerHeight - m.offsetHeight - 6);
  m.style.left = `${x}px`; m.style.top = `${y}px`;
  if (opts.keyboard || opts.active != null) ctxLight(opts.active != null ? opts.active : ctxStep(-1, 1));
}
// the next item from i in direction d (separators skipped, wrapping); -1 when there is none
function ctxStep(i, d) {
  const L = ctxLevel; if (!L) return -1;
  const n = L.items.length;
  if (i < 0 || i >= n) i = d > 0 ? -1 : n;
  for (let k = 1; k <= n; k++) { const j = ((i + d * k) % n + n) % n; if (L.items[j] !== '-' && L.rows[j]) return j; }
  return -1;
}
function ctxLight(i) {
  const L = ctxLevel; if (!L) return;
  L.rows.forEach((r, j) => { if (r) r.classList.toggle('active', j === i); });
  L.active = i;
}
// pick item i: a submenu opens beside its row; anything else closes the menu and runs
function ctxPick(i, keyboard) {
  const L = ctxLevel; const it = L && L.items[i]; if (!it || it === '-') return;
  if (it.sub) {
    const r = L.rows[i].getBoundingClientRect();
    const sub = typeof it.sub === 'function' ? it.sub() : it.sub;
    showCtxMenu({ clientX: r.right - 4, clientY: r.top, screenX: L.at.screenX, screenY: L.at.screenY }, sub, { keyboard, parent: { items: L.items, at: L.at, active: i, parent: L.parent } });
    return;
  }
  closeCtxMenu(); it.run();
}
// back to the menu a submenu came from (Left)
function ctxBack() {
  const P = ctxLevel && ctxLevel.parent; if (!P) return false;
  showCtxMenu(P.at, P.items, { parent: P.parent, active: P.active });
  return true;
}
document.addEventListener('click', () => closeCtxMenu());
// While a menu is open the keys are the menu's - before the terminal (or anything else) sees them. A menu that is
// one text box (promptInline) keeps its own keys.
window.addEventListener('keydown', ev => {
  if (!ctxMenu || !ctxLevel || ctxMenu.querySelector('input')) return;
  if (ev.ctrlKey || ev.altKey || ev.metaKey) return;
  const L = ctxLevel;
  const take = () => { ev.preventDefault(); ev.stopPropagation(); };
  if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') { take(); ctxLight(ctxStep(L.active, ev.key === 'ArrowDown' ? 1 : -1)); return; }
  if (ev.key === 'Home' || ev.key === 'End') { take(); ctxLight(ev.key === 'Home' ? ctxStep(-1, 1) : ctxStep(-1, -1)); return; }
  if (ev.key === 'Enter' || ev.key === ' ') { take(); if (L.active >= 0) ctxPick(L.active, true); return; }
  if (ev.key === 'ArrowRight') { take(); const it = L.items[L.active]; if (it && it !== '-' && it.sub) ctxPick(L.active, true); return; }
  if (ev.key === 'ArrowLeft') { take(); ctxBack(); return; }
  if (ev.key === 'Escape') { take(); closeCtxMenu(); return; }
  if (ev.key === 'Tab') { take(); return; }
}, true);
document.addEventListener('keydown', ev => { if (ev.key === 'Escape') closeCtxMenu(); });
// The tile menu from the keyboard, as Windows does: Shift+F10 or the Menu key opens the selected tile's menu under its
// sidebar tile (or in the middle of the window when the sidebar is closed). Not from a text box outside a terminal
// (a rename box, the hold form, a panel's field: their own menus are the browser's).
let ctxByKeyAt = 0;
window.addEventListener('keydown', ev => {
  const menuKey = ev.key === 'ContextMenu' || (ev.key === 'F10' && ev.shiftKey && !ev.ctrlKey && !ev.altKey && !ev.metaKey);
  if (!menuKey) return;
  const tgt = ev.target, tag = ((tgt && tgt.tagName) || '').toLowerCase();
  const inField = (['input', 'textarea', 'select'].includes(tag) || (tgt && tgt.isContentEditable)) && !(tgt.closest && tgt.closest('.xterm'));
  if (inField || !state.active || (typeof panelOpen === 'function' && panelOpen())) return;
  ev.preventDefault(); ev.stopPropagation();
  ctxByKeyAt = Date.now();
  if (ev.repeat) return;
  openTileMenuByKey(state.active);
}, true);
// the browser's own contextmenu event that follows the key (Chromium makes one) is not a second menu
window.addEventListener('contextmenu', ev => { if (Date.now() - ctxByKeyAt < 600) { ev.preventDefault(); ev.stopPropagation(); } }, true);
function openTileMenuByKey(id) {
  const el = tileEls.get(id);
  const r = el && el.offsetParent !== null ? el.getBoundingClientRect() : null;
  const x = r ? r.left + 24 : Math.round(window.innerWidth / 2) - 100, y = r ? Math.min(r.top + 22, window.innerHeight - 40) : Math.round(window.innerHeight / 3);
  openTileMenu({ clientX: x, clientY: y, screenX: window.screenX + x, screenY: window.screenY + y }, id, { keyboard: true });
}

function promptInline(ev, placeholder, onDone, initial) {
  closeCtxMenu();
  const m = document.createElement('div'); m.className = 'ctx-menu';
  const input = document.createElement('input'); input.type = 'text'; input.placeholder = placeholder; input.spellcheck = false; input.className = 'ctx-input';
  if (initial) input.value = initial;
  m.appendChild(input); document.body.appendChild(m); ctxMenu = m;
  m.style.left = `${Math.min(ev.clientX, window.innerWidth - 200)}px`; m.style.top = `${Math.min(ev.clientY, window.innerHeight - 40)}px`;
  input.addEventListener('click', e => e.stopPropagation());
  input.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Enter') { const v = input.value.trim(); closeCtxMenu(); if (v) onDone(v); } else if (e.key === 'Escape') closeCtxMenu(); });
  input.focus();
  if (initial) input.select();
}

function openTileMenu(ev, id, how = {}) {
  const t = state.tiles.find(x => x.id === id); if (!t) return;
  const items = [
    { label: 'Rename', run: () => startRename(id) },
    { label: t.pinned ? 'Unpin' : 'Pin', run: () => deck.pin(id, !t.pinned) },
  ];
  // the Special prompt (no longer a button on the strip, 2026-11-23)
  if (t.kind === 'claude' && !t.exited) items.push({ label: 'Special prompt…', run: () => { activate(id); state.confirm[id] = { kind: 'hold' }; renderTiles(); } });
  // Prompts: Pause / Resume / Pause until restart (30-tiles.js pauseItems), then the auditors and their children
  // (30-tiles.js auditorItems; "Set as child of…" is a submenu of its own)
  const prompts = [...pauseItems(t).map(p => ({ label: p.label, run: p.run })), ...auditorItems(t, ev).map(a => ({ label: a.label, run: a.run, sub: a.sub }))];
  if (prompts.length) items.push({ label: 'Prompts', sub: prompts });
  // its questions on an ask page, or its last answers read-only (49-asks.js): the same as F7 with it selected
  { const ask = typeof askMenuItem === 'function' ? askMenuItem(t) : null; if (ask) items.unshift(ask, '-'); }
  // F1–F3 without the keyboard or a drag. Fill works on any grid; the named halves are a 2×2 idea, so
  // they only appear on a 2×2 (on bigger grids you place with the key chord or a drag).
  const g = GRID();
  items.push('-');
  const opts = [['Fill the window', Layout.fullSet(g)]];
  if (g.rows === 2 && g.cols === 2) opts.push(['Top half', [1, 2]], ['Bottom half', [3, 4]], ['Left half', [1, 3]], ['Right half', [2, 4]]);
  for (const [label, set] of opts) {
    items.push({ label, checked: Layout.same(Layout.occupancy(state.layout, id, g) || [], set, g), run: () => placeTile(id, set) });
  }
  if (isPlaced(id)) items.push({ label: 'Remove from layout', run: () => unplaceTile(id) });
  items.push({ label: 'Open in a new window', run: () => openInNewWindow(id, ev.screenX, ev.screenY) });
  if (t.kind === 'cloud' && t.cloudUrl) items.push({ label: 'Open on claude.ai in the browser', run: () => openCloudOutside(t) });
  if (t.kind === 'claude' || t.kind === 'cloud') {
    items.push('-');
    for (const g of state.groups || []) items.push({ label: g, checked: t.group === g, run: () => deck.setGroup(id, t.group === g ? null : g) });
    if ((state.groups || []).length) items.push({ label: 'No group', checked: !t.group, dim: true, run: () => deck.setGroup(id, null) });
    items.push({ label: 'New group…', run: () => promptInline(ev, 'group name', name => deck.setGroup(id, name)) });
  }
  showCtxMenu(ev, items, { keyboard: !!how.keyboard });
}
function openGroupMenu(ev, name) {
  showCtxMenu(ev, [
    { label: `Rename "${name}"`, run: () => promptInline(ev, 'new name', n => { const names = (state.groups || []).map(g => g === name ? n : g); const moved = state.tiles.filter(t => t.group === name); deck.setGroups(names).then(() => moved.forEach(t => deck.setGroup(t.id, n))); }) },
    { label: `Remove "${name}"`, dim: true, run: () => deck.setGroups((state.groups || []).filter(g => g !== name)) },
  ]);
}

// Bottom-row button: left click toggles grouped view; right click opens the groups menu — the phone's
// groups screen in the desk's own shape: the grouped toggle, each group with its count (click it for
// rename / remove), and a new group. While the menu is open, either click on the button closes it.
const groupsBtn = document.getElementById('groups-btn');
let groupsMenuOpen = false;
function openGroupsMenu(ev) {
  const items = [
    { label: groupedView ? 'List is grouped' : 'Group the list', checked: groupedView, run: () => { groupedView = !groupedView; localStorage.setItem('deck.grouped', groupedView ? '1' : '0'); layoutGroups(); } },
    '-',
  ];
  const groups = state.groups || [];
  if (!groups.length) items.push({ label: 'No groups yet', dim: true, run: () => {} });
  for (const g of groups) {
    const n = state.tiles.filter(t => t.group === g).length;
    items.push({ label: `${g}  (${n})`, run: () => openGroupMenu(ev, g) });
  }
  items.push('-');
  items.push({ label: 'New group…', run: () => promptInline(ev, 'new group name', n => deck.setGroups([...(state.groups || []), n]).then(() => { groupedView = true; localStorage.setItem('deck.grouped', '1'); layoutGroups(); })) });
  showCtxMenu(ev, items);
  groupsMenuOpen = true;
}
groupsBtn.addEventListener('click', ev => {
  ev.stopPropagation();
  if (groupsMenuOpen && ctxMenu) { closeCtxMenu(); groupsMenuOpen = false; return; }   // the button closes its own menu
  groupsMenuOpen = false;
  groupedView = !groupedView; localStorage.setItem('deck.grouped', groupedView ? '1' : '0'); layoutGroups();
});
groupsBtn.addEventListener('contextmenu', ev => {
  ev.preventDefault(); ev.stopPropagation();
  if (groupsMenuOpen && ctxMenu) { closeCtxMenu(); groupsMenuOpen = false; return; }
  openGroupsMenu(ev);
});
document.addEventListener('click', () => { groupsMenuOpen = false; });
