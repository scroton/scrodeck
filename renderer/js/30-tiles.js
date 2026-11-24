'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- tiles
const PIN_SVG = '<svg viewBox="0 0 16 16"><path d="M9.5 1.5 14.5 6.5 12 7l-2.2 2.2.7 4.3L9 15l-3-3-4 4-1-1 4-4-3-3 1.5-1.5 4.3.7L10 5z"/></svg>';
const renameFns = new Map();   // tileId -> opens that tile's rename box (built in ensureTileEl)
function startRename(id) { const f = renameFns.get(id); if (f) f(); }

function ensureTileEl(id) {
  let el = tileEls.get(id);
  if (el) return el;
  el = document.createElement('div');
  el.className = 'tile'; el.dataset.id = id;
  el.innerHTML = `
    <span class="t-pin-zone"></span><span class="t-pin" title="Pin">${PIN_SVG}</span>
    <div class="t-top"><span class="t-abbr" hidden></span><span class="t-cloud" hidden title="cloud session: runs on claude.ai, not on this machine">${CLOUD_SVG}</span><div class="t-title"></div><span class="t-ask" hidden></span><span class="t-effort" hidden title="effort level of the last answer"></span><span class="t-quad" hidden title="where this session sits in the layout"></span><div class="t-age"></div></div>
    <div class="t-sub" hidden><span class="t-acct-name"></span><span class="t-cloud t-cloud-sub" hidden title="cloud session: runs on claude.ai, not on this machine">${CLOUD_SVG}</span><span class="t-status"></span></div>
    <div class="t-bars">${BARS_HTML}
    </div>
    <div class="t-msg" hidden></div>
    <div class="t-held" hidden></div>
    <div class="t-actions" hidden></div>`;
  // Click vs double-click (2026-09-10; replaces the tap/hold split of 2026-09-06). A single click
  // SELECTS the tile and never places it — with a layout in force an unplaced tile is selected in the
  // sidebar and no pane changes. A DOUBLE-click places it: it takes the pane (or the empty slot) that was
  // selected before the first click (R12 adoption, 25-layout.js layoutAdopt). Drag, the digit chord and
  // F11/F12 place as before. The first click of a double-click has already made this tile the selection,
  // so the pane to take is remembered from before it.
  let beforeClick = null;
  el.addEventListener('click', ev => {
    if (ev.target.closest('button, .t-pin')) return;
    if (state.active !== id) beforeClick = state.active;
    activate(id, { adopt: false });
  });
  el.addEventListener('dblclick', ev => {
    if (ev.target.closest('button, .t-pin')) return;
    if (isPlaced(id) || !beforeClick || beforeClick === id) return;
    if (zoomBlocks()) return;              // placing waits for the grid to be back (25-layout.js zoom)
    state.active = beforeClick;            // activate() adopts against the previous selection
    activate(id);
    beforeClick = null;
  });
  el.querySelector('.t-pin').addEventListener('click', ev => {
    ev.stopPropagation();
    const t = state.tiles.find(x => x.id === id);
    if (t) deck.pin(id, !t.pinned);
  });
  // right click: rename the tile (deck-only title; Enter saves, Esc cancels, empty restores the session's own title)
  el.addEventListener('contextmenu', ev => {
    ev.preventDefault(); ev.stopPropagation();
    if (typeof openTileMenu === 'function') openTileMenu(ev, id); else startRename(id);   // menu in 35-groups.js
  });
  renameFns.set(id, () => {
    const titleEl = el.querySelector('.t-title');
    if (titleEl.querySelector('input')) return;
    const t = state.tiles.find(x => x.id === id);
    const input = document.createElement('input');
    input.className = 'rename'; input.type = 'text'; input.value = (t && t.title) || '';
    input.placeholder = 'tile title';
    titleEl.textContent = ''; titleEl.appendChild(input);
    renaming.add(id); el.classList.add('renaming');
    const finish = (save) => {
      if (!renaming.has(id)) return;
      renaming.delete(id); el.classList.remove('renaming');
      const v = input.value;
      input.remove();
      if (save) deck.renameTile(id, v);
      renderTiles();
    };
    input.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Enter') finish(true); else if (e.key === 'Escape') finish(false); });
    input.addEventListener('blur', () => finish(true));
    input.addEventListener('click', e => e.stopPropagation());
    input.addEventListener('mousedown', e => e.stopPropagation());
    input.focus(); input.select();
  });
  // drag & drop. Every tile can START a drag (F4: any tile can be dropped onto the terminal area,
  // 25-layout.js); what a drop MEANS is decided by where it lands. Dropped on another tile it is the
  // sidebar reorder, which is pinned-tiles-only on both ends — so the sidebar handlers check the source
  // (dragSrcId, since dataTransfer is unreadable during dragover) as well as the target.
  el.addEventListener('dragstart', ev => {
    ev.dataTransfer.setData('text/plain', id); ev.dataTransfer.effectAllowed = 'move';
    dragSrcId = id;
    el.classList.add('dragging');
  });
  el.addEventListener('dragend', ev => {
    el.classList.remove('dragging'); clearDropMarks(); hidePreview();
    const src = dragSrcId; dragSrcId = null;
    // F5: let go outside this window (nothing took the drop, pointer beyond the viewport) -> a new deck
    // window opens there with this tile filling it. Inside the window, a missed drop is just a missed drop.
    const outside = ev.clientX < 0 || ev.clientY < 0 || ev.clientX > window.innerWidth || ev.clientY > window.innerHeight;
    if (dragRescued) { dragRescued = false; return; }   // a cancelled stranded drag ends nowhere (25-layout.js)
    if (src && outside && ev.dataTransfer && ev.dataTransfer.dropEffect === 'none') openInNewWindow(src, ev.screenX, ev.screenY);
  });
  el.addEventListener('dragover', ev => {
    const t = state.tiles.find(x => x.id === id), s = state.tiles.find(x => x.id === dragSrcId);
    if (!t || !t.pinned || !s || !s.pinned) return;
    ev.preventDefault(); ev.dataTransfer.dropEffect = 'move';
    const r = el.getBoundingClientRect(); const before = ev.clientY < r.top + r.height / 2;
    clearDropMarks(); el.classList.add(before ? 'drop-before' : 'drop-after');
  });
  el.addEventListener('dragleave', () => el.classList.remove('drop-before', 'drop-after'));
  el.addEventListener('drop', ev => {
    ev.preventDefault();
    const src = ev.dataTransfer.getData('text/plain');
    const target = state.tiles.find(x => x.id === id), source = state.tiles.find(x => x.id === src);
    if (!src || src === id || !target || !target.pinned || !source || !source.pinned) { clearDropMarks(); return; }
    const before = el.classList.contains('drop-before');
    clearDropMarks();
    const ids = state.tiles.filter(x => x.pinned).map(x => x.id).filter(x => x !== src);
    const at = ids.indexOf(id);
    ids.splice(before ? at : at + 1, 0, src);
    deck.reorder(ids);
  });
  tilesEl.appendChild(el);
  tileEls.set(id, el);
  return el;
}
function clearDropMarks() { for (const el of tileEls.values()) el.classList.remove('drop-before', 'drop-after'); }

function renderTiles() {
  const seen = new Set();
  for (const t of state.tiles) { seen.add(t.id); updateTile(t, ensureTileEl(t.id)); }
  for (const [id, el] of tileEls) if (!seen.has(id)) { el.remove(); tileEls.delete(id); actionSig.delete(id); delete state.confirm[id]; }
  // keep DOM order equal to state order
  let i = 0;
  for (const t of state.tiles) { const el = tileEls.get(t.id); if (tilesEl.children[i] !== el) tilesEl.insertBefore(el, tilesEl.children[i] || null); i++; }
  if (typeof applySearchFilter === 'function') applySearchFilter();   // search pane (46-search.js) hides non-matching tiles
  if (typeof layoutGroups === 'function') layoutGroups();             // grouped view (35-groups.js) adds divider bars
  paintPaneNames();                                                   // every pane wears its tile's name (20-terminals.js)
  if (typeof paintCloudPanes === 'function') paintCloudPanes();      // a cloud tile's pane is its claude.ai page (48-cloud.js)
  paintWindowTitle();                                                 // one session filling the window names it (25-layout.js)
  if (typeof paintAsks === 'function') paintAsks();                  // the asks tile and the ask pane (49-asks.js)
}

function setText(el, text) { if (el.textContent !== text) el.textContent = text; }
// The name a tile goes by everywhere it is written: the sidebar line, the pane label (20-terminals.js).
function tileTitle(t) { return t.title || (t.kind === 'shell' ? shellName() : t.kind === 'cloud' ? 'New cloud session' : 'New session'); }
// Tiles that belong to an account: a Claude session on this machine, or one in the cloud (48-cloud.js).
const hasAcct = t => !!t && (t.kind === 'claude' || t.kind === 'cloud');

// Fill a bar block (.t-bars with .s-*, .w-*, .f-* rows) for an account. showFable: also the
// Fable-scoped weekly window when the account reports one.
// ---- colour ramps from Settings: hex → hex along the shorter hue arc, by usage percent
function hexToHsl(hex) {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || '');
  if (!m) return [0, 0, 50];
  const r = parseInt(m[1], 16) / 255, g = parseInt(m[2], 16) / 255, b = parseInt(m[3], 16) / 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2;
  if (max === min) return [0, 0, l * 100];
  const d = max - min, s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
  let h = max === r ? (g - b) / d + (g < b ? 6 : 0) : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h * 60, s * 100, l * 100];
}
function rampAt(from, to, t) {
  const a = hexToHsl(from), b = hexToHsl(to);
  let dh = b[0] - a[0]; if (dh > 180) dh -= 360; if (dh < -180) dh += 360;
  const h = (a[0] + dh * t + 360) % 360, s = a[1] + (b[1] - a[1]) * t, l = a[2] + (b[2] - a[2]) * t;
  return `hsl(${h.toFixed(0)} ${s.toFixed(0)}% ${l.toFixed(0)}%)`;
}
function rampColor(bar, used) {
  const t = Math.max(0, Math.min(100, used)) / 100;
  return `linear-gradient(90deg, ${rampAt(bar.from, bar.to, Math.max(0, t - 0.12))}, ${rampAt(bar.from, bar.to, t)})`;
}
const DEFAULT_BARS = {
  session: { show: true, name: 'Session (5 h)', from: '#22c55e', to: '#ef4444' },
  week:    { show: true, name: 'Week', from: '#22c55e', to: '#ef4444' },
  fable:   { show: true, name: 'Fable (if applicable)', from: '#22d3ee', to: '#d946ef' },
  context: { show: true, name: 'Context', from: '#fbbf24', to: '#f97316' },
};
const barCfg = k => (state.settings.bars && state.settings.bars[k]) || DEFAULT_BARS[k];

// Fill a bar block for an account. opts: { fable: show the Fable-scoped window, context: percent for the context bar }
function applyBars(bars, accountId, opts = {}) {
  const u = state.usage[accountId];
  const ok = !!(u && u.session && u.week);
  bars.classList.toggle('unavailable', !ok);
  const labels = state.settings.barLabels || { time: true, day: true };
  const countdown = state.settings.barLabelMode === 'countdown';
  // countdown mode: hours until the session reset; days until the week reset, hours under 24 h; one line
  // when both windows reset at the same moment (everything resets together)
  const sameReset = countdown && u && u.session && u.week && u.session.resetsAt && u.week.resetsAt
    && Math.abs(new Date(u.session.resetsAt) - new Date(u.week.resetsAt)) < 60_000;
  bars.querySelector('.s-lbl').hidden = !labels.time;
  bars.querySelector('.w-lbl').hidden = !labels.day || sameReset;
  bars.querySelector('.bars-lbls').hidden = !labels.time && !labels.day;
  const fmtSession = countdown ? fmtUntilHours : fmtResetTime;
  const fmtWeek = countdown ? fmtUntilDays : fmtResetDay;
  const setBar = (key, lbl, fill, seg, fmt, avail = ok) => {
    const cfg = barCfg(key), row = bars.querySelector(fill).parentElement, l = lbl ? bars.querySelector(lbl) : null;
    row.hidden = !cfg.show || (key === 'fable' && !seg) || (key === 'context' && seg == null);
    const f = bars.querySelector(fill);
    if (avail && seg && seg.used != null) {
      const used = Math.max(0, Math.min(100, seg.used));
      if (l) setText(l, fmt(seg.resetsAt));
      f.style.width = `${used}%`;
      f.style.background = rampColor(cfg, used);
      row.title = `${cfg.name}: ${Math.round(used)}%`;
    } else { if (l) setText(l, '—'); f.style.width = '0%'; row.title = cfg.name; }
  };
  setBar('session', '.s-lbl', '.s-fill', u && u.session, fmtSession);
  setBar('week', '.w-lbl', '.w-fill', u && u.week, fmtWeek);
  const fable = opts.fable && u && Array.isArray(u.scoped) ? u.scoped.find(x => /fable/i.test(x.model)) : null;
  setBar('fable', null, '.f-fill', fable, null);
  setBar('context', null, '.c-fill', opts.context == null ? null : { used: opts.context }, null, true);
  const note = bars.querySelector('.t-bars-note');
  if (note) {
    let noteText = '';
    if (u && u.state === 'expired') noteText = 'usage unavailable: login expired, a running session in this account refreshes it';
    else if (u && u.state === 'no-credentials') noteText = 'usage unavailable: not logged in';
    else if (u && u.state === 'error' && !ok) noteText = `usage unavailable: ${u.error || 'error'}`;
    else if (u && u.state === 'rate-limited' && !ok) noteText = 'usage: rate limited, retrying later';
    note.hidden = !noteText; setText(note, noteText);
  }
}
const BARS_HTML = `
      <div class="bars-lbls"><span class="bar-lbl s-lbl">—</span><span class="bar-lbl w-lbl">—</span></div>
      <div class="bars-rows">
        <div class="bar s-bar" title=""><div class="fill s-fill"></div></div>
        <div class="bar w-bar" title=""><div class="fill w-fill"></div></div>
        <div class="bar f-bar f-row" hidden title="Fable weekly usage"><div class="fill f-fill"></div></div>
        <div class="bar c-bar" hidden title="Context"><div class="fill c-fill"></div></div>
      </div>
      <div class="t-bars-note" hidden></div>`;

function updateTile(t, el) {
  const isActive = t.id === state.active;
  const a = hasAcct(t) ? acct(t.account) : null;
  const busy = t.status === 'busy';
  const isCloud = t.kind === 'cloud';
  el.classList.toggle('active', isActive);
  el.classList.toggle('cloud', isCloud);
  el.classList.toggle('pinned', !!t.pinned);
  el.classList.toggle('desk-attn', !!t.attention);   // a desk the Claude on-call just opened here (main.js openDeskRequest): pulses until typed into
  el.draggable = true;
  // the little 2×2 beside the age says where a placed session sits; hidden when it is not in a layout.
  // R5: every window lists every session, so a tile placed in ANOTHER window shows the glyph dimmed with
  // that window's number — selecting it here brings it into this window (main evicts it from the other).
  const quad = el.querySelector('.t-quad'), g = GRID(), N = g.rows * g.cols;
  const occ = Layout.occupancy(state.layout, t.id, g);
  const elsewhere = !occ && t.inWindow != null && t.inWindow !== state.windowIndex;
  quad.hidden = !occ && !elsewhere;
  quad.classList.toggle('elsewhere', elsewhere);
  quad.title = elsewhere ? `in deck window ${t.inWindow + 1} — select to bring it here` : 'where this session sits in the layout';
  if (!quad.hidden) {
    // the glyph mirrors the grid shape (R×C cells) with the occupied ones lit; rebuild the cells only when
    // the count changes, but keep the column count in sync every time (a stale one survives a rebuild guard)
    if (quad.children.length !== N) quad.innerHTML = '<i></i>'.repeat(N);
    quad.style.gridTemplateColumns = `repeat(${g.cols}, 1fr)`;
    quad.classList.toggle('g4', g.rows > 3 || g.cols > 3);   // a fourth row or column: a bigger glyph (style.css)
    const cells = quad.children;
    for (let k = 1; k <= N; k++) cells[k - 1].classList.toggle('on', !!(occ && occ.includes(k)));
  }
  el.classList.toggle('placed', !!occ);
  el.classList.toggle('elsewhere', elsewhere);
  el.querySelector('.t-pin').classList.toggle('on', !!t.pinned);

  // line 1: [abbr] title age — the abbreviation only when the account has one
  const abbrEl = el.querySelector('.t-abbr');
  abbrEl.hidden = !(a && a.abbr);
  if (a && a.abbr) { setText(abbrEl, a.abbr); abbrEl.classList.toggle('busy', busy); }
  // the cloud mark sits right after the account's name: the abbreviation on line 1, or the name on line 2
  const cloudTop = el.querySelector('.t-top .t-cloud'), cloudSub = el.querySelector('.t-cloud-sub');
  cloudTop.hidden = !(isCloud && a && a.abbr);
  cloudSub.hidden = !(isCloud && !(a && a.abbr));
  cloudTop.classList.toggle('busy', busy); cloudSub.classList.toggle('busy', busy);
  const titleEl = el.querySelector('.t-title');
  const title = tileTitle(t);
  if (!renaming.has(t.id) && titleEl.textContent !== title) titleEl.textContent = title;
  titleEl.classList.toggle('untitled', !t.title && t.kind !== 'shell');
  const ageEl = el.querySelector('.t-age');
  setText(ageEl, fmtAge(state.now - (t.lastAt || state.now)));
  // The age's tooltip tells the sleep story: why an idle-looking tile is still running, or since when it
  // has been asleep. Hibernation is invisible when it works, and "why is this one still up?" is the
  // question that gets asked. Claude tiles only; a shell never sleeps and says so.
  if (t.kind === 'claude') {
    const tip = t.status === 'asleep' ? `asleep since ${t.sleptAt ? new Date(t.sleptAt).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) : '?'}`
      : t.sleepBlocker ? `awake: ${t.sleepBlocker}` : 'awake: may sleep at the next check';
    if (ageEl.title !== tip) ageEl.title = tip;
  } else if (isCloud) {
    const tip = t.cloudError ? `cloud session; its status could not be read: ${t.cloudError}` : 'cloud session: runs on claude.ai, never sleeps';
    if (ageEl.title !== tip) ageEl.title = tip;
  } else if (ageEl.title) ageEl.title = '';
  // questions on an ask page: waiting (hollow) or answers in progress (filled) — 49-asks.js
  const askSig = typeof paintAskMark === 'function' ? paintAskMark(el.querySelector('.t-ask'), t.kind === 'claude' ? t.sessionId : null) : '';
  const effortEl = el.querySelector('.t-effort');
  // Ultracode is a mode on top of an effort level (xhigh by default, max when switched on at max), so while it
  // is on the tag names the mode and the tooltip the level. Claude Code itself calls xhigh + workflows "ultracode".
  const ultra = t.kind === 'claude' && !!t.ultracode;
  const effortText = ultra ? 'ultracode' : t.effort;
  effortEl.hidden = !(t.kind === 'claude' && effortText);
  if (effortText) setText(effortEl, effortText);
  const effortTip = ultra ? `ultracode on · effort ${t.effort || '?'}` : 'effort level of the last answer';
  if (effortEl.title !== effortTip) effortEl.title = effortTip;
  // refit whenever anything sharing the line changed: sidebar width, abbreviation, effort, age text, title text
  const fitKey = `${sidebar.clientWidth}|${abbrEl.hidden ? '' : abbrEl.textContent}|${cloudTop.hidden ? '' : 'c'}|${effortEl.hidden ? '' : effortText}|${el.querySelector('.t-age').textContent}|${title}|${askSig}`;
  if (!renaming.has(t.id) && titleEl.dataset.fitted !== fitKey) fitTitle(titleEl, fitKey);

  // line 2: account label when there is no abbreviation, plus status words
  let statusText = '';
  if (t.exited && t.elsewhere) statusText = t.elsewhere === 'deck' ? 'open in the other deck' : 'open elsewhere';   // the same-session guard
  else if (t.exited) statusText = isCloud && !t.cloudId ? 'no cloud session made' : t.exitCode === 0 || t.exitCode == null ? 'exited' : `exited (${t.exitCode})`;
  else if (t.status === 'asleep') statusText = 'asleep';
  else if (t.status === 'waiting') statusText = 'waiting for you';
  else if (t.status === 'starting') statusText = isCloud ? 'making the cloud session' : 'starting';
  else if (t.status === 'archived') statusText = 'archived';
  // Pause / Resume (main.js, lib/pause.js): the deck's word goes first
  const pw = pauseWords(t);
  if (pw) statusText = statusText ? `${pw} · ${statusText}` : pw;
  const showName = hasAcct(t) && !(a && a.abbr);
  const sub = el.querySelector('.t-sub');
  sub.hidden = !(showName || statusText);          // the line only exists when it has something to say
  const nameEl = el.querySelector('.t-acct-name');
  nameEl.hidden = !showName;
  if (showName) { setText(nameEl, acctLabel(t.account)); nameEl.classList.toggle('busy', busy); }
  const st = el.querySelector('.t-status');
  // under auto, WHICH screen is sizing this session is per-session news: say it on the tile. Under a claim
  // (office / phone / remote) every session follows the same screen and the driver button says so.
  if (t.mirror && (state.settings.driver || 'auto') === 'auto') {
    const by = t.sizeBy === 'remote' ? 'on the remote desk' : t.sizeBy === 'office' ? 'sized by the office deck' : 'on your phone';
    statusText = statusText ? `${statusText} · ${by}` : by;
  }
  setText(st, statusText && showName ? `· ${statusText}` : statusText);
  st.classList.toggle('exited', !!t.exited);
  st.classList.toggle('asleep', t.status === 'asleep');
  st.classList.toggle('waiting', !t.exited && t.status === 'waiting');
  st.classList.toggle('paused', !!pw);
  const pauseTip = pw ? pauseTipOf(t) : '';
  if (st.title !== pauseTip) st.title = pauseTip;
  // The tile itself dims while the session is not running: the sidebar is read at a glance, and 'asleep'
  // as a word on one line is not enough to tell twenty-four tiles apart.
  el.classList.toggle('asleep', t.status === 'asleep');

  // usage bars (Claude tiles, here or in the cloud: the account's usage, and the session's context)
  const bars = el.querySelector('.t-bars');
  bars.hidden = !hasAcct(t);
  if (hasAcct(t)) applyBars(bars, t.account, { fable: /fable/i.test(t.model || ''), context: t.context });
  // effort as colour: a class per level (styled only when body.effort-color is set)
  for (const c of [...el.classList]) if (c.startsWith('effort-')) el.classList.remove(c);
  if (t.kind === 'claude' && t.effort) el.classList.add(`effort-${String(t.effort).toLowerCase()}`);
  el.classList.toggle('ultracode', !!t.ultracode);   // ultracode is on (at any effort): purple pulse

  // last message box
  const msg = el.querySelector('.t-msg');
  const showMsg = hasAcct(t) && !!t.lastText && !busy;
  msg.hidden = !showMsg;
  if (showMsg) setText(msg, t.lastText.replace(/\r/g, '').split('\n').map(l => l.trimEnd()).filter(l => l.trim()).join('\n'));

  // prompts the deck is holding for this session (every tile, so a glance shows what is scheduled)
  renderHeld(t, el.querySelector('.t-held'));

  // actions (selected tile only)
  const actions = el.querySelector('.t-actions');
  actions.hidden = !isActive;
  if (isActive) renderActions(t, actions);
}

// The held-prompt rows: time, the first line of the text, its state, and a remove cross. A row whose
// time has passed but is still waiting says why (busy, waiting for you, no prompt yet).
const heldSig = new Map();
const HHMM = m => `${String(Math.floor(((m % 1440) + 1440) % 1440 / 60)).padStart(2, '0')}:${String(((m % 60) + 60) % 60).padStart(2, '0')}`;
// how a special prompt's timing reads in the list, per kind
function heldWhen(i) {
  if (i.kind === 'next') return 'next turn';
  if (i.kind === 'recurring') return `every ${i.everyMin}m` + ((i.startMin !== i.endMin) ? ` ${HHMM(i.startMin)}–${HHMM(i.endMin)}` : '');
  if (i.kind === 'triggered') return `⚑ ${i.trigger}`;
  return fmtHeldTime(i.at);   // timed
}
function renderHeld(t, box) {
  const items = Array.isArray(t.held) ? t.held : [];
  const mail = Array.isArray(t.auditorMail) ? t.auditorMail : [];   // a child's files from its auditor (lib/auditors.js)
  const sig = JSON.stringify([items.map(i => [i.id, i.kind, i.at, i.everyMin, i.trigger, i.state, i.note, i.text.slice(0, 60)]), mail.map(m => [m.id, m.state, m.sentAt, m.firstLine])]);
  if (heldSig.get(t.id) === sig) return;
  heldSig.set(t.id, sig);
  box.innerHTML = '';
  box.hidden = !items.length && !mail.length;
  for (const m of mail) box.appendChild(auditorMailRow(m));
  for (const i of items) {
    const row = document.createElement('div'); row.className = `held-row ${i.state}`;
    const when = document.createElement('span'); when.className = 'held-when'; when.textContent = heldWhen(i);
    when.title = i.kind === 'timed' ? new Date(i.at).toLocaleString() : `${i.kind} prompt`;
    const text = document.createElement('span'); text.className = 'held-text';
    text.textContent = i.text.split('\n')[0].slice(0, 120);
    text.title = i.text;
    const st = document.createElement('span'); st.className = 'held-state';
    // recurring/triggered never end; show them as "on" when idle-waiting
    const standing = i.kind === 'recurring' || i.kind === 'triggered';
    st.textContent = i.state === 'sending' ? 'typing' : i.state === 'due' ? 'waiting'
      : i.state === 'sent' ? (i.note ? 'typed?' : 'sent') : i.state === 'failed' ? 'failed'
      : standing ? 'on' : 'held';
    if (i.note) st.title = i.note;
    const x = document.createElement('button'); x.className = 'held-x'; x.textContent = '×'; x.title = 'forget this prompt';
    x.addEventListener('click', ev => { ev.stopPropagation(); deck.holdRemove(i.id); });
    row.append(when, text, st, x);
    box.appendChild(row);
  }
}
// 06:30 today, 'tue 06:30' further out
function fmtHeldTime(ms) {
  const d = new Date(ms), now = new Date();
  const hm = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  if (d.toDateString() === now.toDateString()) return hm;
  const tomorrow = new Date(now); tomorrow.setDate(now.getDate() + 1);
  if (d.toDateString() === tomorrow.toDateString()) return `tomorrow ${hm}`;
  return `${['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'][d.getDay()]} ${hm}`;
}
// "‖ pausing" / "‖ paused" / "resuming", "until restart", "resume queued" (lib/pause.js view), or ''. The mark stays
// until the session gets another prompt (2026-11-24); the deck's PAUSE, a pasted one and a typed one alike.
const PAUSE_GLYPH = '\u2016';
function pauseWords(t) {
  const p = t && !t.exited ? t.pause : null;
  if (!p) return '';
  if (p.state === 'resuming') return 'resuming';
  return `${PAUSE_GLYPH} ${p.want ? `${p.state}, resume queued` : p.untilRestart ? `${p.state} until restart` : p.state}`;
}
function pauseTipOf(t) {
  const p = t.pause;
  if (!p || p.state === 'resuming') return 'the deck typed RESUME; plain again once that turn ends';
  return `${p.by === 'prompt' ? 'a PAUSE prompt was sent to this session' : 'paused by the deck'}; the mark goes with the next prompt it gets (RESUME or any other)`;
}
// One file from the auditor, on its child's tile: "from auditor 1, 15:02", its first line, and its state; one waiting
// for Send (settings auditorAutoSend off) has Send and a drop cross
function auditorMailRow(m) {
  const row = document.createElement('div'); row.className = `held-row ${m.state === 'sent' ? 'sent' : 'due'}`;
  const d = m.sentAt ? new Date(m.sentAt) : null;
  const when = document.createElement('span'); when.className = 'held-when';
  when.textContent = d ? `from ${m.from}, ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}` : `from ${m.from}`;
  when.title = `${m.name}${d ? `, typed ${d.toLocaleString()}` : ''}: the deck types it as your own message`;
  const text = document.createElement('span'); text.className = 'held-text'; text.textContent = m.firstLine || m.name; text.title = m.firstLine || m.name;
  const st = document.createElement('span'); st.className = 'held-state';
  st.textContent = m.state === 'sent' ? 'sent' : m.state === 'hold' ? 'waits for Send' : 'when idle';
  row.append(when, text, st);
  if (m.state === 'hold') {
    const send = document.createElement('button'); send.className = 'held-x held-send'; send.textContent = 'Send'; send.title = 'type it into this session now, or when its turn ends';
    send.addEventListener('click', ev => { ev.stopPropagation(); deck.auditorSend(m.id).catch(e => console.error(e)); });
    const x = document.createElement('button'); x.className = 'held-x'; x.textContent = '×'; x.title = 'do not send it (the file goes to the inbox\'s refused folder)';
    x.addEventListener('click', ev => { ev.stopPropagation(); deck.auditorDrop(m.id).catch(e => console.error(e)); });
    row.append(send, x);
  }
  return row;
}
// "auditor <k>", the smallest k no auditor has (lib/auditors.js nextString)
function nextAuditorString() {
  const used = new Set(state.tiles.filter(x => x.auditor).map(x => String(x.auditor.string).toLowerCase()));
  for (let k = 1; ; k++) if (!used.has(`auditor ${k}`)) return `auditor ${k}`;
}
// Auditors and their children for one tile (lib/auditors.js): the tile menu (ev: where it opened) and the palette
// (flat: one row per auditor instead of a second menu) list the same
function auditorItems(t, ev, flat) {
  const items = [];
  if (!t || t.kind !== 'claude' || t.exited) return items;
  const say = p => Promise.resolve(p).catch(e => console.error(e));
  const at = ev || { clientX: Math.round(window.innerWidth / 2) - 100, clientY: Math.round(window.innerHeight / 3) };
  const later = f => (ev ? f() : setTimeout(f, 0));   // from the palette: after the click that ran it (document click closes menus)
  if (t.auditor) {
    const n = (t.auditor.children || []).length;
    items.push({ label: 'Rename auditor…', hint: 'its children are renamed with it', run: () => later(() => promptInline(at, 'auditor name', v => say(deck.makeAuditor(t.id, v)), t.auditor.string)) });
    items.push({ label: `Un-make auditor (${n} ${n === 1 ? 'child' : 'children'})`, hint: 'its children end with it', run: () => say(deck.unmakeAuditor(t.id)) });
    return items;
  }
  const auds = state.tiles.filter(x => x.auditor && x.id !== t.id && !x.exited);
  if (!t.child) items.push({ label: 'Make auditor…', hint: 'the deck types the AUDITOR prompt into it', run: () => later(() => promptInline(at, 'auditor name', v => say(deck.makeAuditor(t.id, v)), nextAuditorString())) });
  if (auds.length && flat) {
    for (const a of auds) if (!(t.child && t.child.parent === a.id)) items.push({ label: `Set as child of ${a.auditor.string}`, hint: 'the deck tells the auditor; nothing is typed here', run: () => say(deck.setAuditorChild(t.id, a.id)) });
  } else if (auds.length) {
    const sub = auds.map(a => ({ label: a.auditor.string, checked: !!(t.child && t.child.parent === a.id), run: () => say(deck.setAuditorChild(t.id, a.id)) }));
    items.push({ label: 'Set as child of…', hint: 'the auditors', sub, run: () => showCtxMenu(at, sub) });
  }
  if (t.child) items.push({ label: `Un-child (child ${t.child.n} of ${t.child.auditorName})`, hint: 'the auditor\'s files no longer reach it', run: () => say(deck.unchildTile(t.id)) });
  for (const m of (t.auditorMail || [])) if (m.state === 'hold') items.push({ label: `Send ${m.name} from ${m.from}`, hint: m.firstLine || '', run: () => say(deck.auditorSend(m.id)) });
  return items;
}
// Pause / Resume / Pause until restart for one tile: the tile menu and the palette (88-palette.js) list the same
function pauseItems(t) {
  const items = [];
  if (!t || t.kind !== 'claude' || t.exited) return items;
  const p = t.pause;
  if (p && (p.state === 'pausing' || p.state === 'paused') && !p.want) items.push({ label: 'Resume', hint: 'when its turn ends', run: () => deck.resumeTile(t.id) });
  if (t.status !== 'asleep' && t.status !== 'off' && (!p || p.state === 'resuming')) {
    items.push({ label: 'Pause', hint: 'Esc, then the PAUSE prompt', run: () => deck.pauseTile(t.id, {}) });
    items.push({ label: 'Pause until restart', hint: 'resumed by itself at the next deck start', run: () => deck.pauseTile(t.id, { untilRestart: true }) });
  }
  return items;
}

// The Special prompt form: one message plus a KIND — next turn, at a time, recurring, or triggered
// (R17). The deck holds it and types it into this session when the condition is met, the way a person
// would, and only when the session is idle at its prompt.
function renderHoldForm(t, box, rerender) {
  const local = (ms) => { const d = new Date(ms); d.setMinutes(d.getMinutes() - d.getTimezoneOffset()); return d.toISOString().slice(0, 16); };
  const hhmm = (ms) => { const d = new Date(ms); return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`; };
  const minutesOf = (v) => { const m = /^(\d{1,2}):(\d{2})$/.exec(v || ''); return m ? (+m[1] % 24) * 60 + (+m[2] % 60) : 0; };

  const form = document.createElement('div'); form.className = 'hold-form';

  // kind picker
  const kinds = [['next', 'Next turn'], ['timed', 'At a time'], ['recurring', 'Recurring'], ['triggered', 'Triggered']];
  const tabs = document.createElement('div'); tabs.className = 'sp-kinds';
  let kind = 'next';
  const panes = {};
  const note = document.createElement('div'); note.className = 'confirm-text';
  const NOTES = {
    next: 'Sent as soon as this session finishes its current turn and is idle at its prompt.',
    timed: 'Held on the desktop and typed in at the time you set; if the session is busy then, as soon as it is idle afterward.',
    recurring: 'While the deck is running and the time of day is in the window, sent every N minutes.',
    triggered: 'Sent when the named trigger goes true — a Claude session can raise it with the deck-triggers skill.',
  };
  function selectKind(k) {
    kind = k;
    for (const b of tabs.children) b.classList.toggle('on', b.dataset.k === k);
    for (const key of Object.keys(panes)) panes[key].hidden = key !== k;
    note.textContent = NOTES[k];
  }
  for (const [k, label] of kinds) {
    const b = document.createElement('button'); b.className = 'sp-kind'; b.dataset.k = k; b.textContent = label;
    b.addEventListener('click', ev => { ev.stopPropagation(); selectKind(k); });
    tabs.appendChild(b);
  }

  const ta = document.createElement('textarea'); ta.className = 'hold-text'; ta.placeholder = 'the prompt'; ta.rows = 3; ta.spellcheck = false;

  // timed pane: a datetime plus the account's reset times as picks
  const timedPane = document.createElement('div'); timedPane.className = 'sp-pane'; panes.timed = timedPane;
  const u = (state.usage || {})[t.account] || {};
  const AFTER = 5 * 60 * 1000, picks = [];
  const addPick = (label, iso) => { const ms = Date.parse(iso || ''); if (ms > Date.now()) picks.push({ label, ms: ms + AFTER }); };
  addPick('session reset +5m', u.session && u.session.resetsAt);
  addPick('week reset +5m', u.week && u.week.resetsAt);
  for (const sc of (u.scoped || [])) addPick(`${sc.model || 'model'} reset +5m`, sc.resetsAt);
  picks.push({ label: 'in 1 h', ms: Date.now() + 3600 * 1000 });
  const time = document.createElement('input'); time.type = 'datetime-local'; time.className = 'hold-time'; time.value = local(picks[0].ms);
  const whenRow = document.createElement('div'); whenRow.className = 'hold-when'; whenRow.appendChild(time);
  for (const pk of picks) { const b = document.createElement('button'); b.className = 'hold-pick'; b.textContent = `${pk.label} ${fmtHeldTime(pk.ms)}`; b.addEventListener('click', ev => { ev.stopPropagation(); time.value = local(pk.ms); }); whenRow.appendChild(b); }
  timedPane.appendChild(whenRow);

  // recurring pane: interval + a from/to time-of-day window
  const recPane = document.createElement('div'); recPane.className = 'sp-pane'; panes.recurring = recPane;
  const every = document.createElement('input'); every.type = 'number'; every.min = '1'; every.max = '1440'; every.value = '30'; every.className = 'sp-num';
  const from = document.createElement('input'); from.type = 'time'; from.className = 'sp-time'; from.value = '09:00';
  const to = document.createElement('input'); to.type = 'time'; to.className = 'sp-time'; to.value = '17:00';
  const rf = document.createElement('div'); rf.className = 'sp-fields';
  rf.append(mkLabel('every', every), mkLabel('min, between', from), mkLabel('and', to));
  recPane.appendChild(rf);

  // triggered pane: a trigger name (datalist of known triggers)
  const trigPane = document.createElement('div'); trigPane.className = 'sp-pane'; panes.triggered = trigPane;
  const trig = document.createElement('input'); trig.type = 'text'; trig.className = 'sp-trig'; trig.placeholder = 'trigger name'; trig.setAttribute('list', 'sp-trig-list');
  const dl = document.createElement('datalist'); dl.id = 'sp-trig-list';
  for (const tr of (state.triggers || [])) { const o = document.createElement('option'); o.value = tr.name; dl.appendChild(o); }
  const trigHint = document.createElement('div'); trigHint.className = 'confirm-text sp-hint';
  trigHint.textContent = 'Sent when this trigger turns on. Adding the prompt does not turn it on: a Claude session does, with the deck-triggers skill. Pick one the deck knows, or type a new name (the deck creates it, off).';
  trigPane.append(mkLabel('trigger', trig), dl, trigHint);

  const row = document.createElement('div'); row.className = 'btn-row';
  const add = document.createElement('button'); add.textContent = 'add'; add.className = 'primary';
  const cancel = document.createElement('button'); cancel.textContent = 'cancel';
  const err = document.createElement('span'); err.className = 'confirm-text hold-err';
  add.addEventListener('click', ev => {
    ev.stopPropagation();
    if (!ta.value.trim()) { err.textContent = 'nothing to send'; return; }
    let spec = { kind, text: ta.value };
    if (kind === 'timed') { const at = Date.parse(time.value); if (!(at > Date.now())) { err.textContent = 'that time has passed'; return; } spec.at = at; }
    else if (kind === 'recurring') { spec.everyMin = +every.value; spec.startMin = minutesOf(from.value); spec.endMin = minutesOf(to.value); }
    else if (kind === 'triggered') { if (!trig.value.trim()) { err.textContent = 'name a trigger'; return; } spec.trigger = trig.value.trim(); }
    deck.holdAdd(t.id, spec).then(() => { delete state.confirm[t.id]; rerender(); }).catch(e => { err.textContent = String(e && e.message || e); });
  });
  cancel.addEventListener('click', ev => { ev.stopPropagation(); delete state.confirm[t.id]; rerender(); });
  for (const n of [ta, time, every, from, to, trig]) { n.addEventListener('click', ev => ev.stopPropagation()); n.addEventListener('keydown', ev => ev.stopPropagation()); }
  row.append(add, cancel, err);
  form.append(tabs, note, ta, timedPane, recPane, trigPane, row);
  box.appendChild(form);
  selectKind('next');
  requestAnimationFrame(() => ta.focus());
}
function mkLabel(text, input) { const l = document.createElement('label'); l.className = 'sp-lab'; const s = document.createElement('span'); s.textContent = text; l.append(s, input); return l; }

function renderActions(t, box) {
  const c = state.confirm[t.id];
  const others = state.accounts.filter(a => a.id !== t.account);
  const sig = JSON.stringify([t.kind, t.account, t.exited, t.status === 'asleep', c, others.map(a => a.id + a.label), 'browser' in t, t.browser, t.cloudId || null]);
  if (actionSig.get(t.id) === sig) return;
  actionSig.set(t.id, sig);
  box.innerHTML = '';
  const row = document.createElement('div'); row.className = 'btn-row';
  const btn = (label, cls, fn) => { const b = document.createElement('button'); b.textContent = label; if (cls) b.className = cls; b.addEventListener('click', ev => { ev.stopPropagation(); fn(); }); row.appendChild(b); return b; };
  // an icon in place of the word (2026-11-23: sleep, close): its tooltip says the word
  const iconBtn = (name, tip, fn) => { const b = btn('', 'icon', fn); b.innerHTML = TILE_ICONS[name]; b.dataset.icon = name; b.title = tip; b.setAttribute('aria-label', tip); return b; };
  const text = (s) => { const span = document.createElement('span'); span.className = 'confirm-text'; span.textContent = s; box.appendChild(span); };
  const rerender = () => { actionSig.delete(t.id); renderTiles(); };
  const report = (e) => { const r = terms.get(t.id); if (r) r.term.write(`\r\n\x1b[31m[claude-deck] ${e && e.message || e}\x1b[0m\r\n`); };
  box.appendChild(row);
  // c.browser (set by the right-click on `open …` / `refresh`): turn Claude in Chrome on or off as it restarts
  const browserWord = c && c.browser === true ? ' with the browser on' : c && c.browser === false ? ' with the browser off' : '';
  const browserOpts = c && typeof c.browser === 'boolean' ? { browser: c.browser } : undefined;

  // A cloud tile: nothing runs here, so nothing here can be lost — no confirmations. Its page reloads, opens in
  // the default browser, or the tile goes (the session keeps running in the cloud and Elsewhere lists it).
  if (t.kind === 'cloud') {
    if (t.cloudId) {
      btn('reload', '', () => { cloudRedraw(t.id); deck.refreshTile(t.id); }).title = 'Load the session\'s page again';
      btn('browser', '', () => openCloudOutside(t)).title = 'Open the session on claude.ai in your browser';
    } else if (t.exited) btn('retry', '', () => deck.restartTile(t.id)).title = 'Run claude --cloud with the same task again';
    iconBtn('close', t.cloudId ? 'Remove the tile. The session keeps running in the cloud; Elsewhere lists it.' : t.exited ? 'Remove' : 'Stop and remove the tile', () => deck.closeTile(t.id));
    requestAnimationFrame(() => fitButtons(row));
    return;
  }

  if (c && c.kind === 'move') {
    text(`Move this session to ${acctLabel(c.account)}${browserWord}? This window closes and reopens there.`);
    btn(`Move to ${acctLabel(c.account)}`, 'danger', () => { delete state.confirm[t.id]; deck.moveTile(t.id, c.account, browserOpts).catch(report); rerender(); });
    btn('Cancel', '', () => { delete state.confirm[t.id]; rerender(); });
    return;
  }
  if (c && c.kind === 'refresh') {
    text(t.status === 'busy' ? `Refresh this session${browserWord}? It is busy, so the turn in progress is lost.` : `Refresh this session${browserWord}? The process restarts and resumes where it was.`);
    btn('Refresh', 'danger', () => { delete state.confirm[t.id]; deck.refreshTile(t.id, browserOpts); rerender(); });
    btn('Cancel', '', () => { delete state.confirm[t.id]; rerender(); });
    return;
  }
  if (c && c.kind === 'hold') { row.remove(); renderHoldForm(t, box, rerender); return; }
  if (c && c.kind === 'close') {
    text(t.exited ? 'Remove this tile?' : 'Close this session? The process is killed.');
    btn(t.exited ? 'Remove' : 'Close', 'danger', () => { delete state.confirm[t.id]; deck.closeTile(t.id); rerender(); });
    btn('Cancel', '', () => { delete state.confirm[t.id]; rerender(); });
    return;
  }
  if (t.kind === 'claude') {
    // open in a terminal: "Bring it here" ends that process and resumes it here (Elsewhere's flow); resume looks again
    if (t.exited && t.elsewhere === 'terminal' && deck.bringHere) btn('bring it here', '', () => deck.bringHere(t.id)).title = 'This session runs in another window: end it there and resume it here';
    if (t.exited) btn('resume', '', () => deck.restartTile(t.id)).title = t.elsewhere ? 'Look again: resumed here only if nothing else has it open' : '';
    // Sleeping is not closing and not exiting: the session is intact on disk and one click brings the
    // process back. Say 'wake' / 'sleep' rather than start/stop so it never reads as losing anything.
    else if (t.status === 'asleep') btn('wake', '', () => deck.wakeTile(t.id));
    else if (t.sessionId) iconBtn('sleep', 'Sleep', () => deck.sleepTile(t.id).catch(report));
    // "↪ vv": move the session to that account (U+21AA, the curved arrow - 2026-11-23); the Special prompt is in the tile menu now
    for (const a of others) {
      const b = btn(`↪ ${a.label}`, 'move', () => { state.confirm[t.id] = { kind: 'move', account: a.id }; rerender(); });
      browserMenu(t, b, `Move to ${a.label} ${browserPhrase(t)}`, browser => { state.confirm[t.id] = { kind: 'move', account: a.id, browser }; rerender(); });
      b.title = `Move to ${a.label}` + (b.title ? `\n${b.title}` : '');
    }
  } else if (t.exited) {
    btn('restart', '', () => deck.restartTile(t.id));
  }
  if (!t.exited && t.status !== 'asleep') {
    const b = btn('refresh', '', () => { if (t.fresh) { deck.refreshTile(t.id); return; } state.confirm[t.id] = { kind: 'refresh' }; rerender(); });   // same rule as close
    browserMenu(t, b, `Refresh ${browserPhrase(t)}`, browser => { if (t.fresh) { deck.refreshTile(t.id, { browser }); return; } state.confirm[t.id] = { kind: 'refresh', browser }; rerender(); });
  }
  iconBtn('close', t.exited ? 'Remove' : 'Close', () => { if (t.fresh || t.exited) { deck.closeTile(t.id); return; } state.confirm[t.id] = { kind: 'close' }; rerender(); });
  requestAnimationFrame(() => fitButtons(row));
}
// the strip's icons (inline SVG, currentColor; 1em square so fitButtons() shrinks them with the words): a crescent
// moon with a "z" in its hollow for sleep, an X for close
const TILE_ICONS = {
  sleep: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z" fill="currentColor"/><path d="M14.5 2h7l-7 7h7" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></svg>',
  close: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5.5 5.5l13 13M18.5 5.5l-13 13" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round"/></svg>',
};

// Claude in Chrome from the tile buttons: right-click `open …` or `refresh` for the same action with the
// browser switched on, or off when this session already runs with it. t.browser is true / false / null
// (null = the account's own default); a main process that predates the option sends no key, and then
// nothing is offered rather than a menu item that would quietly do a plain refresh.
function browserPhrase(t) { return t.browser === true ? 'without the browser' : 'with the browser'; }
function browserMenu(t, b, label, go) {
  if (t.kind !== 'claude' || !('browser' in t)) return;
  const next = t.browser !== true;
  b.title = `right-click: ${label.charAt(0).toLowerCase()}${label.slice(1)}`;
  b.addEventListener('contextmenu', ev => { ev.preventDefault(); ev.stopPropagation(); showCtxMenu(ev, [{ label, run: () => go(next) }]); });
}

// Shrink a button row's font until every button fits on the single line (down to 7.5px).
function fitButtons(row) {
  if (!row || !row.isConnected) return;
  const buttons = [...row.querySelectorAll('button')];
  if (!buttons.length) return;
  let size = 11;
  // an icon button keeps a little more padding than the words (2026-11-23)
  const apply = () => { for (const b of buttons) { const icon = b.classList.contains('icon'); b.style.fontSize = `${size}px`; b.style.paddingLeft = b.style.paddingRight = icon ? (size < 9.5 ? '4px' : '6px') : size < 9.5 ? '2px' : '3px'; } };
  apply();
  let guard = 0;
  while (row.scrollWidth > row.clientWidth + 0.5 && size > 7.5 && guard++ < 20) { size -= 0.5; apply(); }
}
// Title: 14px when it fits one line, otherwise the largest size down to 9px that does; at 9px it wraps.
function fitTitle(titleEl, key) {
  if (!titleEl.isConnected || titleEl.offsetParent === null) return;   // not laid out yet: retried on the next tick
  let size = 14;
  const oneLine = () => titleEl.getBoundingClientRect().height <= size * 1.25 + 1.5;
  titleEl.style.fontSize = `${size}px`;
  let guard = 0;
  while (!oneLine() && size > 8 && guard++ < 20) { size -= 0.5; titleEl.style.fontSize = `${size}px`; }   // floor 8px, then it wraps
  titleEl.dataset.fitted = key || '';
}
// sidebar resized: forget every fit key so the next render refits each title
function fitAllTitles() { for (const el of tileEls.values()) el.querySelector('.t-title').dataset.fitted = ''; renderTiles(); }
new ResizeObserver(() => { fitAllTitles(); document.body.classList.toggle('sb-wide', sidebar.clientWidth > 400); }).observe(sidebar);
document.body.classList.toggle('sb-wide', sidebar.clientWidth > 400);

function fitActiveButtons() { const el = state.active && tileEls.get(state.active); if (el) fitButtons(el.querySelector('.btn-row')); }
new ResizeObserver(() => fitActiveButtons()).observe(sidebar);
