'use strict';
/* global Terminal, FitAddon */
// The phone view. One WebSocket to the deck's main process carries everything: `meta` on the same
// 2 s tick the desktop renderer gets, terminal bytes, and every action as a named call.
//
// Navigation is a stack of full screens over one bottom bar (ruling 2026-09-02): the list is home, a
// tile opens its terminal full screen, and each bottom icon opens a screen of its own. Pushing a
// screen pushes a history entry, so the phone's own back gesture is the back button.

// ---------------------------------------------------------------- socket
const BUILD = 'v151-2026-11-18';
// Kept across reloads, because the app reloads itself the moment a file changes and a zeroed counter
// reads the same as a broken one. `doc` counts every touch the page sees at all; `start`/`move` only
// those that reach the terminal's handler. doc>0 with start=0 means the events exist but never get
// to the terminal; both zero means the page is seeing no touches whatsoever.
const touchStats = (() => {
  const zero = { doc: 0, start: 0, move: 0, cancelable: 0, scrolled: 0, wheel: 0, lines: 0 };
  try { return { ...zero, ...JSON.parse(localStorage.getItem('deck.touch') || '{}') }; } catch { return { ...zero }; }
})();
function noteTouch() {
  try { localStorage.setItem('deck.touch', JSON.stringify(touchStats)); } catch { /* private mode */ }
}
document.addEventListener('touchstart', () => { touchStats.doc++; noteTouch(); }, { capture: true, passive: true });

const state = {
  accounts: [], tiles: [], usage: {}, groups: [], settings: {}, now: Date.now(), home: '',
  connected: false,
};
let ws = null, backoff = 500, callId = 0;
// The fingerprint the page booted with. When the server reports a different one, this copy of the
// app is stale — reload. Installed to the home screen there is no address bar to do it by hand.
let bootAssets = null;
let serverBuild = null;
let pageKey = '';                      // the key in a viewer path (the page viewer; lib/phone.js pageKey)
const pending = new Map();

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/ws`);
  ws.onopen = () => {
    backoff = 500; state.connected = true; $('#offline').hidden = true;
    setTimeout(pumpOutbox, 2500);                                       // anything sent while the link was down
    // attaching replays the tile's buffer, so wipe the screen first or the log appears twice
    if (termId) { if (term) term.reset(); sendAttach(termId); }
  };
  ws.onclose = () => {
    state.connected = false; $('#offline').hidden = false;
    setTimeout(connect, backoff); backoff = Math.min(8000, backoff * 1.7);
  };
  ws.onerror = () => { try { ws.close(); } catch { /* already going */ } };
  ws.onmessage = ev => {
    let m; try { m = JSON.parse(ev.data); } catch { return; }
    if (m.t === 'init' || m.t === 'meta') {
      // Two independent staleness checks. The fingerprint catches files changing while we watch;
      // the build string catches having booted old, which the fingerprint never can.
      if (m.build) serverBuild = m.build;
      if (m.pageKey) pageKey = m.pageKey;
      if (m.build && m.build !== BUILD) { if (refreshForNewBuild(m.build)) return; }
      if (m.assets) {
        if (bootAssets === null) bootAssets = m.assets;
        else if (m.assets !== bootAssets) { bootAssets = m.assets; location.reload(); return; }
      }
      if (m.build === BUILD) { try { sessionStorage.removeItem('deck.refresh'); } catch { /* ignore */ } }
      const meta = m.meta;
      paintHeld(!!(meta && meta.restart && meta.restart.uiHeld));
      if (m.home) state.home = m.home;
      const driverBefore = driverMode();
      Object.assign(state, meta);
      applyMeta();
      paintKeyRow();
      paintDrive();                                    // the word, the colour and the remote desk's name, from every meta
      // the driver changed hands; or this phone is a viewer and the screen that drives has resized the session
      if (current() === 's-term' && (driverMode() !== driverBefore || viewerDrifted())) requestAnimationFrame(fitTerm);
    } else if (m.t === 'data') {
      if (m.id === termId && term) {
        // the replay is marked by the server; the first unmarked data after our size went out is drawn at our size
        if (!stitchArmed && stitchSizeSentAt && !m.replay) armWhenSettled();
        if (m.replay) phoneReplaying++;                // what a replay asks is not answered (phoneAnswers)
        const wasReplay = !!m.replay;
        term.write(scrubHints(m.d), () => { if (wasReplay) phoneReplaying--; settlePrefetch(); scheduleChoices(); releaseRedraw(); scheduleLive(); scheduleSticky(); scheduleStitch(); });
      }
    } else if (m.t === 'reset') {
      if (m.id === termId && term) term.reset();
    } else if (m.t === 'exit') {
      if (m.id === termId && term) term.write(`\r\n\x1b[90m[exited${m.exitCode ? ` (${m.exitCode})` : ''}]\x1b[0m\r\n`);
    } else if (m.t === 'ok' || m.t === 'err') {
      const p = pending.get(m.rid);
      if (p) { pending.delete(m.rid); m.t === 'ok' ? p.resolve(m.result) : p.reject(new Error(m.error)); }
    }
  };
}
function sendRaw(msg) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(msg)); }
function call(fn, ...args) {
  return new Promise((resolve, reject) => {
    if (!ws || ws.readyState !== 1) return reject(new Error('not connected'));
    const rid = ++callId;
    pending.set(rid, { resolve, reject });
    sendRaw({ t: 'call', rid, fn, args });
    setTimeout(() => { if (pending.delete(rid)) reject(new Error('timed out')); }, 120000);
  });
}
// Reload onto a newer build, but never more than twice per session: if a reload does not actually
// change what we are running (a wedged cache), say so instead of spinning.
// The deck keeps this app at the code it was handed while its own main process is older than the files on disk
// (lib/phone.js phoneTextBuf): no reload onto new code against the old deck; the restart brings it, and this line
// says so, as a held deck window's strip does. A tap hides it for this page's life.
let heldHidden = false;
function paintHeld(on) { const el = $('#held'); if (el) el.hidden = !on || heldHidden; }
document.addEventListener('click', ev => { if (ev.target && ev.target.id === 'held') { heldHidden = true; paintHeld(false); } });
function refreshForNewBuild(serverBuild) {
  let tries = 0;
  try { tries = +(sessionStorage.getItem('deck.refresh') || 0); } catch { /* private mode */ }
  if (tries < 3) {
    try { sessionStorage.setItem('deck.refresh', String(tries + 1)); } catch { /* ignore */ }
    // a fresh query defeats any cache between here and the PC
    location.replace(location.pathname + '?r=' + Date.now());
    return true;
  }
  const bar = $('#stale');
  bar.hidden = false;
  // a second visit to the banner means the tap did not do it: say what does (clearing the site's data)
  let taps = 0; try { taps = +(sessionStorage.getItem('deck.banner') || 0); } catch { /* ignore */ }
  const hint = taps ? ' If tapping does not do it: long-press the app icon → App info → Storage → Clear cache, then open it again.' : '';
  bar.textContent = `This app is ${BUILD}; the PC has ${serverBuild}. Tap to force a fresh copy.` + hint;
  // what the PC actually serves right now, past every cache — if it is this build, the meta was stale, not the app
  fetch('/app.js?probe=' + Date.now(), { cache: 'no-store', credentials: 'same-origin' }).then(r => r.text()).then(src => {
    const m = /const BUILD = '([^']+)'/.exec(src.slice(0, 4000));
    if (m && m[1] === BUILD) { bar.hidden = true; try { sessionStorage.removeItem('deck.refresh'); sessionStorage.removeItem('deck.banner'); } catch { /* ignore */ } }
    else if (m) bar.textContent = `This app is ${BUILD}; the PC serves ${m[1]}. Tap to force a fresh copy.`;
  }).catch(() => {});
  bar.onclick = () => {
    try { sessionStorage.removeItem('deck.refresh'); sessionStorage.setItem('deck.banner', String(taps + 1)); } catch { /* ignore */ }
    if ('serviceWorker' in navigator) {
      navigator.serviceWorker.getRegistrations()
        .then(rs => Promise.all(rs.map(r => r.unregister())))
        .then(() => caches.keys())
        .then(ks => Promise.all(ks.map(k => caches.delete(k))))
        .catch(() => {})
        .finally(() => location.replace(location.pathname + '?r=' + Date.now()));
    } else location.replace(location.pathname + '?r=' + Date.now());
  };
  return false;
}

const $ = s => document.querySelector(s);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

// ---------------------------------------------------------------- formatting (ported from renderer/js/10-format.js)
function fmtAge(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  if (s < 60) return 'now';
  const m = Math.floor(s / 60); if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60); if (h < 24) return `${h}h`;
  return `${Math.floor(h / 24)}d`;
}
function fmtResetTime(iso) {
  if (!iso) return '—';
  const d = new Date(iso); if (isNaN(d)) return '—';
  const h = d.getHours();
  return `${h % 12 || 12}:${String(d.getMinutes()).padStart(2, '0')}${h >= 12 ? 'p' : 'a'}`;
}
function fmtResetDay(iso) { const d = new Date(iso || ''); return isNaN(d) ? '—' : `${d.getMonth() + 1}.${d.getDate()}`; }
function fmtUntilHours(iso) {
  const ms = new Date(iso || '') - Date.now(); if (isNaN(ms)) return '—';
  if (ms <= 0) return 'now';
  const m = Math.ceil(ms / 60000), h = Math.floor(m / 60);
  return h >= 1 ? `${h}h ${String(m % 60).padStart(2, '0')}m` : `${m}m`;
}
function fmtUntilDays(iso) {
  const ms = new Date(iso || '') - Date.now(); if (isNaN(ms)) return '—';
  return ms < 86400000 ? fmtUntilHours(iso) : `${Math.ceil(ms / 86400000)}d`;
}
function hexToHsl(hex) {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || '');
  if (!m) return [0, 0, 50];
  const r = parseInt(m[1], 16) / 255, g = parseInt(m[2], 16) / 255, b = parseInt(m[3], 16) / 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2;
  if (max === min) return [0, 0, l * 100];
  const d = max - min, s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
  const h = max === r ? (g - b) / d + (g < b ? 6 : 0) : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h * 60, s * 100, l * 100];
}
function rampAt(from, to, t) {
  const a = hexToHsl(from), b = hexToHsl(to);
  let dh = b[0] - a[0]; if (dh > 180) dh -= 360; if (dh < -180) dh += 360;
  return `hsl(${((a[0] + dh * t + 360) % 360).toFixed(0)} ${(a[1] + (b[1] - a[1]) * t).toFixed(0)}% ${(a[2] + (b[2] - a[2]) * t).toFixed(0)}%)`;
}
function rampColor(bar, used) {
  const t = Math.max(0, Math.min(100, used)) / 100;
  return `linear-gradient(90deg, ${rampAt(bar.from, bar.to, Math.max(0, t - 0.12))}, ${rampAt(bar.from, bar.to, t)})`;
}
const DEFAULT_BARS = {
  session: { show: true, name: 'Session (5 h)', from: '#22c55e', to: '#ef4444' },
  week: { show: true, name: 'Week', from: '#22c55e', to: '#ef4444' },
  fable: { show: true, name: 'Fable (if applicable)', from: '#22d3ee', to: '#d946ef' },
  context: { show: true, name: 'Context', from: '#fbbf24', to: '#f97316' },
};
const barCfg = k => (state.settings.bars && state.settings.bars[k]) || DEFAULT_BARS[k];
const acct = id => state.accounts.find(a => a.id === id) || null;
const acctLabel = id => { const a = acct(id); return a ? a.label : (id || ''); };
const tileById = id => state.tiles.find(t => t.id === id) || null;
// TERMINAL REPLIES (2026-11-19, item 14b: exactly one answerer per session - never two, a second answer is junk
// in Claude's prompt; never none, a stall). xterm answers a program's queries by itself (device attributes, cursor
// position, status, colours) and raises them as NOT user input (CoreService.triggerDataEvent(data, false)). The phone
// passes them on only when the deck names it the answerer (meta `answerer` === 'phone': the C++ deck, when neither
// its window nor a remote desk shows the session; core/deck/remoterule answerer), never for a sleeping tile (any
// input wakes it) and never what a replay asks (a query asked long ago, answered into whatever runs now). The
// Electron deck names none - its window has a terminal for every session and answers them all - so under it the
// phone answers nothing (until 2026-11-23 it answered too, in keys mode: two answers). Keys and paste (user input)
// always go on.
// If a later xterm renames this, the phone answers as before and nothing else breaks.
let phoneReplaying = 0;
function phoneAnswers() {
  const t = termId ? tileById(termId) : null;
  return !!t && t.answerer === 'phone' && t.status !== 'asleep' && phoneReplaying === 0;
}
function phoneReplies(t) {
  phoneReplaying = 0;                                  // a new terminal: an old one's pending writes never finish
  try {
    const cs = t._core && t._core.coreService;
    if (!cs || typeof cs.triggerDataEvent !== 'function') return;
    const orig = cs.triggerDataEvent.bind(cs);
    // an answer goes straight out: not through term.onData, which would also mark a user's input (userInput), and
    // xterm drops everything while the line mode has its stdin off (disableStdin), so in that mode the phone never
    // answered at all before
    cs.triggerDataEvent = (data, wasUserInput = false) => { if (wasUserInput) orig(data, true); else if (phoneAnswers() && termId) sendRaw({ t: 'input', id: termId, d: data }); };
  } catch { /* the phone answers as before */ }
}

// ---------------------------------------------------------------- bars
const BARS_HTML = `
  <div class="bars-lbls"><span class="bar-lbl s-lbl">—</span><span class="bar-lbl w-lbl">—</span></div>
  <div class="bars-rows">
    <div class="bar s-bar"><div class="fill s-fill"></div></div>
    <div class="bar w-bar"><div class="fill w-fill"></div></div>
    <div class="bar f-bar" hidden><div class="fill f-fill"></div></div>
    <div class="bar c-bar" hidden><div class="fill c-fill"></div></div>
  </div>`;
function applyBars(bars, accountId, opts = {}) {
  const u = state.usage[accountId];
  const ok = !!(u && u.session && u.week);
  bars.classList.toggle('unavailable', !ok);
  const labels = state.settings.barLabels || { time: true, day: true };
  const countdown = state.settings.barLabelMode === 'countdown';
  const sameReset = countdown && ok && u.session.resetsAt && u.week.resetsAt
    && Math.abs(new Date(u.session.resetsAt) - new Date(u.week.resetsAt)) < 60000;
  bars.querySelector('.s-lbl').hidden = !labels.time;
  bars.querySelector('.w-lbl').hidden = !labels.day || sameReset;
  bars.querySelector('.bars-lbls').hidden = !labels.time && !labels.day;
  const setBar = (key, lblSel, fillSel, seg, fmt, avail = ok) => {
    const cfg = barCfg(key), fill = bars.querySelector(fillSel), row = fill.parentElement;
    row.hidden = !cfg.show || (key === 'fable' && !seg) || (key === 'context' && seg == null);
    const l = lblSel ? bars.querySelector(lblSel) : null;
    if (avail && seg && seg.used != null) {
      const used = Math.max(0, Math.min(100, seg.used));
      if (l) l.textContent = fmt(seg.resetsAt);
      fill.style.width = `${used}%`;
      fill.style.background = rampColor(cfg, used);
    } else { if (l) l.textContent = (avail && seg && seg.resetsAt) ? fmt(seg.resetsAt) : '—'; fill.style.width = '0%'; }   // no usage yet: the reset time still shows when the window has one
  };
  setBar('session', '.s-lbl', '.s-fill', u && u.session, countdown ? fmtUntilHours : fmtResetTime);
  setBar('week', '.w-lbl', '.w-fill', u && u.week, countdown ? fmtUntilDays : fmtResetDay);
  const fable = opts.fable && u && Array.isArray(u.scoped) ? u.scoped.find(x => /fable/i.test(x.model)) : null;
  setBar('fable', null, '.f-fill', fable, null);
  setBar('context', null, '.c-fill', opts.context == null ? null : { used: opts.context }, null, true);
}

// ---------------------------------------------------------------- the list
function tileEl(t) {
  const e = el('div', 'tile');
  e.dataset.id = t.id;
  const a = t.kind === 'claude' ? acct(t.account) : null;
  const busy = t.status === 'busy';
  if (t.pinned) e.classList.add('pinned');
  if (t.kind === 'claude' && t.effort) e.classList.add('effort-' + String(t.effort).toLowerCase());
  if (t.ultracode) e.classList.add('ultracode');

  const top = el('div', 't-top');
  if (a && a.abbr) { const s = el('span', `t-abbr${busy ? ' busy' : ''}`, a.abbr); top.appendChild(s); }
  const title = el('div', 't-title', t.title || (t.kind === 'shell' ? 'PowerShell' : 'New session'));
  if (!t.title && t.kind !== 'shell') title.classList.add('untitled');
  top.appendChild(title);
  if (t.kind === 'claude' && t.effort) top.appendChild(el('span', 't-effort', t.effort));
  { const n = (t.held || []).filter(h => h.state === 'held' || h.state === 'due' || h.state === 'sending').length; if (n) { const c = el('span', 't-held', `\u23f0 ${n}`); c.title = 'prompts held for this session'; top.appendChild(c); } }
  top.appendChild(el('span', 't-age', fmtAge(state.now - (t.lastAt || state.now))));
  e.appendChild(top);

  let statusText = '';
  if (t.exited) statusText = t.exitCode === 0 || t.exitCode == null ? 'exited' : `exited (${t.exitCode})`;
  else if (t.status === 'waiting') statusText = 'waiting for you';
  else if (t.status === 'starting') statusText = 'starting';
  const showName = t.kind === 'claude' && !(a && a.abbr);
  if (showName || statusText) {
    const sub = el('div', 't-sub');
    if (showName) sub.appendChild(el('span', busy ? 'busy' : '', acctLabel(t.account)));
    if (statusText) {
      const st = el('span', 't-status', statusText);
      if (t.exited) st.classList.add('exited');
      if (!t.exited && t.status === 'waiting') st.classList.add('waiting');
      sub.appendChild(st);
    }
    e.appendChild(sub);
  }

  if (t.kind === 'claude') {
    const bars = el('div', 't-bars');
    bars.innerHTML = BARS_HTML;
    e.appendChild(bars);
    applyBars(bars, t.account, { fable: /fable/i.test(screenModelOf[t.id] || t.model || ''), context: t.context });   // the screen's word first: a /model just given
  }
  if (t.kind === 'claude' && t.lastText && !busy) {
    { const m = el('div', 't-msg'); m.appendChild(el('div', 'in', t.lastText.replace(/\r/g, '').split('\n').map(l => l.trimEnd()).filter(l => l.trim()).join('\n'))); e.appendChild(m); }
  }

  // Tap opens the terminal, press-and-hold opens the tile's actions. Click is what actually opens
  // the session — touch devices synthesise one after touchend — so the tile still works if touch
  // events never arrive. The hold only sets a flag that swallows the click that follows it.
  let timer = null, held = false;
  const cancel = () => { clearTimeout(timer); timer = null; };
  e.addEventListener('touchstart', () => {
    held = false;
    timer = setTimeout(() => { timer = null; held = true; navigator.vibrate && navigator.vibrate(12); openSheet(t.id); }, 480);
  }, { passive: true });
  e.addEventListener('touchmove', cancel, { passive: true });
  e.addEventListener('touchend', cancel);
  e.addEventListener('touchcancel', () => { cancel(); held = false; });
  e.addEventListener('contextmenu', ev => { ev.preventDefault(); held = true; openSheet(t.id); });
  e.addEventListener('click', () => { if (held) { held = false; return; } openTerm(t.id); });
  return e;
}

function renderList() {
  const box = $('#list');
  const grouped = localStorage.getItem('deck.grouped') === '1' && state.groups.length;
  box.textContent = '';
  if (askCfgP().showTile !== false) box.appendChild(asksTileEl());     // the asks tile tops the list (2026-11-10)
  if (!state.tiles.length) { box.appendChild(el('div', 'empty', 'No sessions open on the PC.')); return; }
  if (!grouped) {
    for (const t of state.tiles) box.appendChild(tileEl(t));
    return;
  }
  const seen = new Set();
  for (const g of state.groups) {
    const inGroup = state.tiles.filter(t => t.group === g);
    if (!inGroup.length) continue;
    box.appendChild(el('div', 'group-head', g));
    for (const t of inGroup) { seen.add(t.id); box.appendChild(tileEl(t)); }
  }
  const rest = state.tiles.filter(t => !seen.has(t.id));
  if (rest.length) {
    if (seen.size) box.appendChild(el('div', 'group-head', 'ungrouped'));
    for (const t of rest) box.appendChild(tileEl(t));
  }
}

// ---------------------------------------------------------------- asks (2026-11-10)
// Sessions' questions on ask pages (the ask-page skill; main.js lib/asks.js), as the desk shows them: the asks
// tile tops the list (Settings -> Asks on the desk can hide it), tapping it opens the ask screen with every
// session's pages under sticky headers and the recently answered ones; a session's own screen has a "?" button
// for just its asks (or, until it asks again, its last answers, read-only). The pages are the skill's own,
// served sandboxed by lib/phone.js (/ask/<id>.html) and framed here; they talk to this screen by postMessage,
// the same bridge as the desk's ask pane, and the drafts are the deck's, shared with the desk. Done saves the
// answers file next to the page and names it to the session in one line (only the file when it runs outside the deck).
let askMode = 'all';                     // 'all', a session id, or 'log:<page id>'
const askFr = new Map();                 // page id -> { id, sid, kind, frame, wrap, note, ready, legacy, locked, prog, waiters }
const askSecs = new Map();               // session id -> { el, head, body }
const askOpenLog = new Set();
let askReqN = 0;
const asksOf = () => (Array.isArray(state.asks) ? state.asks : []);
const askLogOf = () => (Array.isArray(state.askLog) ? state.askLog : []);
const askCfgP = () => Object.assign({ showTile: true }, (state.settings && state.settings.asks) || {});
const askOf = sid => asksOf().find(s => s.sessionId === sid) || null;
const tileOfSession = sid => state.tiles.find(t => t.kind === 'claude' && t.sessionId === sid) || null;
const askOpenN = s => s.rounds.reduce((n, r) => n + Math.max(0, r.total - r.answered), 0);
const askProgP = id => { const f = askFr.get(id); return f && f.kind === 'ask' && f.prog ? f.prog : null; };
const askDraftN = s => s.rounds.reduce((n, r) => n + (r.sent ? 0 : Math.max(r.drafted || 0, (askProgP(r.id) || {}).drafted || 0)), 0);
const askUnsentP = s => s.rounds.some(r => !r.sent);
const askNameOf = s => { const t = tileOfSession(s.sessionId); return t ? (t.title || 'session') : (s.title || s.name); };
const askWhen = ms => { const d = new Date(ms); return isNaN(d) ? '' : d.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }); };
function askMarkEl(s) {
  const m = el('span', 't-ask');
  if (!askUnsentP(s)) { m.classList.add('sent'); m.textContent = '✓'; }
  else { if (askDraftN(s)) m.classList.add('drafted'); m.textContent = String(askOpenN(s)); }
  return m;
}
function asksTileEl() {
  const list = asksOf(), log = askLogOf();
  const e = el('div', 'asks-tile' + (list.length ? '' : ' none') + (list.some(askUnsentP) ? ' asking' : ''));   // not a .tile: that class is a session's (tests, effort marks)
  const top = el('div', 't-top');
  top.append(el('span', 'at-glyph', '?'), el('div', 't-title', 'Asks'), el('span', 'at-count', list.length ? `${list.length} waiting` : 'nothing waiting'));
  e.appendChild(top);
  for (const s of list) {
    const row = el('div', 'at-row');
    row.append(askMarkEl(s), el('span', 'at-name', askNameOf(s)), el('span', 'at-where', s.ended ? 'session ended' : tileOfSession(s.sessionId) ? '' : 'outside the deck'),
      el('span', 'at-age', s.latest ? fmtAge(state.now - Date.parse(s.latest)) : ''));
    row.addEventListener('click', ev => { ev.stopPropagation(); openAsks(s.sessionId); });
    e.appendChild(row);
  }
  if (log.length) {
    const row = el('div', 'at-row at-recent');
    row.append(el('span', 'at-name', `Recently answered (${log.length})`), el('span', 'at-age', fmtAge(state.now - log[0].at)));
    row.addEventListener('click', ev => { ev.stopPropagation(); openAsks('all', true); });
    e.appendChild(row);
  }
  e.addEventListener('click', () => openAsks('all'));
  return e;
}
function openAsks(mode, toRecent) {
  askMode = mode;
  if (mode.startsWith('log:')) askOpenLog.add(mode.slice(4));
  if (current() === 's-asks') renderAsks(); else show('s-asks');
  if (toRecent) setTimeout(() => { const r = $('#asks-body .ap-recent'); if (r) $('#asks-body').scrollTop = r.offsetTop - 4; }, 60);
}
// a session's own (the "?" on its screen): its waiting ask, else its last answers until it asks again
function openAsksFor(sid) {
  if (askOf(sid)) return openAsks(sid);
  const last = askLogOf().find(e => e.sessionId === sid);
  if (last) return openAsks(`log:${last.id}`);
  toast('no questions from this session');
}
const askSrcP = id => `/ask/${encodeURIComponent(id)}.html?embed=phone&theme=dark`;
function askFrameP(container, kind, id, sid) {
  let f = askFr.get(id);
  if (f && f.kind === kind) return f;
  const wrap = el('div', 'ap-round'), note = el('div', 'ap-rnote'); note.hidden = true;
  const frame = document.createElement('iframe'); frame.className = 'ap-frame'; frame.src = askSrcP(id);
  frame.setAttribute('allow', 'clipboard-write');
  wrap.append(note, frame); container.appendChild(wrap);
  f = { id, sid, kind, frame, wrap, note, ready: false, legacy: false, locked: false, prog: null, waiters: new Map() };
  askFr.set(id, f);
  frame.addEventListener('load', () => setTimeout(() => { if (!f.ready && askFr.get(id) === f) { f.legacy = true; if (kind === 'log') frame.remove(); else frame.style.height = '70vh'; renderAsks(); } }, 1500));
  return f;
}
function dropAskFr(id) { const f = askFr.get(id); if (!f) return; for (const w of f.waiters.values()) w(null); f.wrap.remove(); askFr.delete(id); }
function askSecP(s) {
  let sec = askSecs.get(s.sessionId);
  if (sec) return sec;
  const sid = s.sessionId;
  const sEl = el('section', 'ap-sess'), head = el('div', 'ap-head'), body = el('div', 'ap-body');
  head.innerHTML = '<div class="ap-who"><span class="ap-mark"></span><span class="ap-name"></span><span class="ap-count"></span></div>'
    + '<div class="ap-row"><button class="tbtn primary ap-done">Done</button><button class="tbtn ap-copy">Copy all</button><button class="tbtn ap-term">Terminal</button><button class="tbtn ap-discard">Discard…</button></div>'
    + '<div class="ap-confirm" hidden><span class="ap-confirm-text"></span><button class="tbtn primary ap-keep">Keep them</button><button class="tbtn danger ap-discard-yes">Discard</button></div>'
    + '<div class="ap-flash"></div>';
  head.querySelector('.ap-done').addEventListener('click', () => askDoneP(sid));
  head.querySelector('.ap-copy').addEventListener('click', () => askCopyP(sid));
  head.querySelector('.ap-term').addEventListener('click', () => { const t = tileOfSession(sid); if (t) openTerm(t.id); else back(); });
  head.querySelector('.ap-discard').addEventListener('click', () => {
    const n = askDraftN(askOf(sid) || { rounds: [] }); if (!n) return;
    head.querySelector('.ap-row').hidden = true; const c = head.querySelector('.ap-confirm'); c.hidden = false;
    c.querySelector('.ap-confirm-text').textContent = `Discard the ${n} answer${n === 1 ? '' : 's'} in progress? The questions stay.`;
    const yes = c.querySelector('.ap-discard-yes'); yes.disabled = true; setTimeout(() => { yes.disabled = false; }, 700);
  });
  head.querySelector('.ap-keep').addEventListener('click', () => { head.querySelector('.ap-row').hidden = false; head.querySelector('.ap-confirm').hidden = true; });
  head.querySelector('.ap-discard-yes').addEventListener('click', async ev => {
    if (ev.currentTarget.disabled) return;
    const s2 = askOf(sid); if (!s2) return;
    for (const r of s2.rounds) { if (r.sent) continue; const f = askFr.get(r.id); if (f) askPostP(f, { askpage: 'clear' }); try { await call('askClear', r.id); } catch { /* gone */ } }
    head.querySelector('.ap-row').hidden = false; head.querySelector('.ap-confirm').hidden = true;
    const t = tileOfSession(sid); if (t) openTerm(t.id); else back();
  });
  sEl.append(head, body);
  $('#asks-list').appendChild(sEl);
  sec = { el: sEl, head, body };
  askSecs.set(sid, sec);
  return sec;
}
function askFlashP(sid, text) { const sec = askSecs.get(sid); if (!sec) return; const f = sec.head.querySelector('.ap-flash'); f.textContent = text; clearTimeout(f._t); f._t = setTimeout(() => { f.textContent = ''; }, 7000); }
function renderAsks() {
  if (current() !== 's-asks') return;
  const list = asksOf(), log = askLogOf();
  const lid = askMode.startsWith('log:') ? askMode.slice(4) : null;
  if (askMode !== 'all' && !lid && !askOf(askMode)) {
    const last = log.find(e => e.sessionId === askMode);
    if (last) { askMode = `log:${last.id}`; askOpenLog.add(last.id); return renderAsks(); }
    askMode = 'all';
  }
  const live = new Set(list.map(s => s.sessionId));
  for (const [sid, sec] of askSecs) if (!live.has(sid)) { for (const [id, f] of askFr) if (f.sid === sid && f.kind === 'ask') dropAskFr(id); sec.el.remove(); askSecs.delete(sid); }
  list.forEach((s, i) => {
    const sec = askSecP(s);
    sec.el.style.order = String(i);
    const vis = askMode === 'all' || askMode === s.sessionId;
    sec.el.hidden = !vis;
    const want = new Set(s.rounds.map(r => r.id));
    for (const [id, f] of askFr) if (f.sid === s.sessionId && f.kind === 'ask' && !want.has(id)) dropAskFr(id);
    if (vis) for (const r of s.rounds) {
      const f = askFrameP(sec.body, 'ask', r.id, s.sessionId);
      if (r.sent && f.ready && !f.locked) { f.locked = true; askPostP(f, { askpage: 'lock' }); }
      const parts = [];
      if (f.legacy) parts.push('Made by an older ask-page skill: use the page\'s own Copy all answers, then paste in the terminal.');
      if (r.sent) parts.push(`sent ${askWhen(r.sent.at)}: read-only until the session reads it`);
      else if (r.source === 'picker') parts.push('also open in the session\'s picker; Done presses Esc there first');
      for (const p of r.replaces || []) parts.push(`replaces ${p.sessionId && p.sessionId !== s.sessionId ? `${p.name || 'another session'}'s ` : ''}round ${p.round == null ? '?' : p.round}`
        + (p.moved ? `: your ${p.moved} answer${p.moved === 1 ? '' : 's'} came over` : ''));
      if (s.ended && r === s.rounds[0]) parts.push('its session has ended: Done saves the file, read when the session is resumed');
      const txt = parts.join(' · '); f.note.hidden = !txt; f.note.textContent = txt;
    }
    const h = sec.head;
    h.querySelector('.ap-mark').replaceChildren(askMarkEl(s));
    h.querySelector('.ap-name').textContent = askNameOf(s) + (s.ended ? ' (session ended)' : tileOfSession(s.sessionId) ? '' : ' (outside the deck)');
    let total = 0, answered = 0; for (const r of s.rounds) { const p = askProgP(r.id); total += r.total; answered += p ? p.answered : r.answered; }
    h.querySelector('.ap-count').textContent = `${answered}/${total}`;
    const done = h.querySelector('.ap-done'); done.disabled = !askUnsentP(s); done.textContent = askUnsentP(s) ? 'Done' : 'Sent';
    h.querySelector('.ap-discard').disabled = !askDraftN(s);
  });
  // the recently answered, read-only (every one in the every-session view; one in the log view)
  const rec = $('#asks-list .ap-recent');
  const showRec = askMode === 'all' ? log.length > 0 : !!lid;
  rec.hidden = !showRec;
  rec.querySelector('.ap-head').hidden = !!lid;
  const rows = rec.querySelector('.ap-lrows');
  const items = lid ? log.filter(e => e.id === lid) : (showRec ? log : []);
  const want = new Set(items.map(e => e.id));
  for (const r of [...rows.children]) if (!want.has(r.dataset.id)) { const f = askFr.get(r.dataset.id); if (f && f.kind === 'log') dropAskFr(r.dataset.id); r.remove(); }
  items.forEach((e, i) => {
    let row = [...rows.children].find(x => x.dataset.id === e.id);
    if (!row) {
      row = el('div', 'ap-lrow'); row.dataset.id = e.id;
      const hd = el('div', 'ap-lhead'); hd.append(el('span', 'ap-lchev'), el('span', 'ap-name'), el('span', 'ap-meta'));
      hd.addEventListener('click', () => { if (askMode.startsWith('log:')) return; if (askOpenLog.has(e.id)) askOpenLog.delete(e.id); else askOpenLog.add(e.id); renderAsks(); });
      row.append(hd, el('div', 'ap-lbody'));
      rows.appendChild(row);
    }
    row.style.order = String(i);
    const t = tileOfSession(e.sessionId);
    row.querySelector('.ap-name').textContent = t ? (t.title || 'session') : (e.title || e.name);
    row.querySelector('.ap-meta').textContent = `${e.how} ${askWhen(e.at)}`;
    const open = !!lid || askOpenLog.has(e.id);
    row.classList.toggle('open', open);
    const body = row.querySelector('.ap-lbody'); body.hidden = !open;
    if (!open) { const f = askFr.get(e.id); if (f && f.kind === 'log') dropAskFr(e.id); return; }
    const f = askFrameP(body, 'log', e.id, e.sessionId);
    let pre = body.querySelector('.ap-lines');
    if (f.legacy) { if (!pre) { pre = el('pre', 'ap-lines'); body.appendChild(pre); } pre.textContent = e.lines.join('\n'); } else if (pre) pre.remove();
  });
  $('#asks-title').textContent = lid ? 'Answered (read-only)' : askMode === 'all' ? (list.length ? `Asks · ${list.length} waiting` : 'Asks') : `${askNameOf(askOf(askMode))} asks`;
  $('#asks-empty').hidden = !!list.length || showRec;
}
const askPostP = (f, m) => { try { f.frame.contentWindow.postMessage(Object.assign({ id: f.id }, m), '*'); } catch { /* reloading */ } };
async function askLoadP(f) {
  if (f.kind === 'log') { let st = null; try { st = await call('askLogState', f.id); } catch { st = null; } askPostP(f, { askpage: 'load', state: st || {}, readonly: true }); f.locked = true; return; }
  const r = (askOf(f.sid) || { rounds: [] }).rounds.find(x => x.id === f.id);
  let st = null; try { st = await call('askDraftGet', f.id); } catch { st = null; }
  askPostP(f, { askpage: 'load', state: st || {}, readonly: !!(r && r.sent) });
  if (r && r.sent) f.locked = true;
}
function askGetP(f) {
  return new Promise(res => { const req = ++askReqN; const timer = setTimeout(() => { f.waiters.delete(req); res(null); }, 3000);
    f.waiters.set(req, m => { clearTimeout(timer); res(m); }); askPostP(f, { askpage: 'get', req }); });
}
window.addEventListener('message', ev => {
  const m = ev.data;
  if (!m || typeof m !== 'object' || typeof m.askpage !== 'string') return;
  let f = null; for (const x of askFr.values()) if (x.frame.contentWindow === ev.source) { f = x; break; }
  if (!f || m.id !== f.id) return;
  const prog = () => ({ drafted: +m.drafted || 0, answered: +m.answered || 0, later: +m.later || 0, total: +m.total || 0 });
  if (m.askpage === 'ready') { f.ready = true; if (f.kind === 'ask') f.prog = prog(); if (m.h) f.frame.style.height = `${m.h}px`; askLoadP(f); }
  else if (m.askpage === 'change') { if (f.kind !== 'ask' || f.locked) return; f.prog = prog(); sendRaw({ t: 'call', rid: 0, fn: 'askDraft', args: [f.id, { state: m.state || {}, ...f.prog }] }); renderAsks(); }
  else if (m.askpage === 'height') { if (m.h > 0) f.frame.style.height = `${Math.ceil(m.h)}px`; }
  else if (m.askpage === 'answers') { const w = f.waiters.get(m.req); if (w) { f.waiters.delete(m.req); w(m); } }
});
async function askCollectP(sid) {
  const s = askOf(sid); if (!s) return null;
  const pages = [];
  for (const r of s.rounds) {
    if (r.sent) continue;
    const f = askFr.get(r.id);
    if (f && f.legacy) continue;
    if (!f || !f.ready) { askFlashP(sid, 'still loading - try again'); return null; }
    const m = await askGetP(f); if (!m || !m.answers) { askFlashP(sid, 'the page did not answer - try again'); return null; }
    pages.push({ id: r.id, answers: m.answers });
  }
  return pages;
}
const askTextP = pages => pages.filter(p => (p.answers.lines || []).length).map(p => [p.id, ...p.answers.lines].join('\n')).join('\n');   // as lib/asks.js composeDone
async function askDoneP(sid) {
  const pages = await askCollectP(sid); if (!pages) return;
  if (!askTextP(pages).trim()) { askFlashP(sid, 'nothing answered yet'); return; }
  try {
    const res = await call('askDone', sid, pages);
    for (const p of pages) { const f = askFr.get(p.id); if (f) { f.locked = true; askPostP(f, { askpage: 'lock' }); } }
    // every Done saves the answers file next to the page; a session in the deck (res.tileId) also gets one line naming it
    const ended = (asksOf().find(x => x.sessionId === sid) || {}).ended;
    const msg = !res.tileId ? (ended ? 'saved its answers file (its session has ended: read when it is resumed)' : 'saved its answers file (it runs outside the deck)') : res.esc ? 'sent: Esc on its picker, then one line naming the answers file' : 'sent: answers file saved, one line naming it goes in as your next message';
    const others = asksOf().some(x => x.sessionId !== sid && askUnsentP(x));
    if (askCfgP().closeOnDone !== false && (askMode === sid || !others)) { toast(msg); back(); } else askFlashP(sid, msg);
  } catch (e) { askFlashP(sid, String((e && e.message) || e)); }
}
async function askCopyP(sid) {
  const pages = await askCollectP(sid); if (!pages) return;
  const text = askTextP(pages); if (!text.trim()) { askFlashP(sid, 'nothing answered yet'); return; }
  toast(await copyText(text) ? `copied ${text.split('\n').length} lines - paste them in the session` : 'could not reach the clipboard');
}

function applyMeta() {
  // the home title carries the count of open sessions
  // (a PUBLIC native build names itself in meta.edition.name, core/deck/edition.hpp; every other deck sends none)
  const name = (state.edition && state.edition.name) || 'Claude Deck';
  { const n = state.tiles.filter(t => !t.exited).length; const bt = document.querySelector('#s-list .bar-title'); if (bt) bt.textContent = name + ' [' + n + ']'; }
  if (state.edition && state.edition.name && document.title !== name) document.title = name;
  // ... and with { moveAll: false } the move-every-session tab is not shown
  { const mv = document.querySelector('[data-go="s-move"]'); if (mv) mv.style.display = state.edition && state.edition.moveAll === false ? 'none' : ''; }
  applyFonts();
  reassertSizeIfDrifted();
  const effortStyle = state.settings.effortStyle || 'tag';
  document.body.classList.toggle('effort-tag', effortStyle === 'tag');
  document.body.classList.toggle('effort-color', effortStyle === 'color');
  document.body.classList.toggle('hide-msgs', localStorage.getItem('deck.msgs') === '0');
  document.body.classList.toggle('hide-bars', localStorage.getItem('deck.bars') === '0');
  const lines = (state.settings.msgLines && state.settings.msgLines.normal) || 3;
  document.documentElement.style.setProperty('--msg-lines', lines);
  renderPageBtns();
  if (!restored && state.tiles.length) restoreWhere();
  if (current() === 's-list') renderList();
  if (current() === 's-search') renderSearch();
  if (current() === 's-asks') renderAsks();
  if (current() === 's-term') {
    const t = tileById(termId);
    // its questions on an ask page: the "?" button, with the count (or a tick once sent; its last answers after)
    { const b = $('#term-ask'), s = t && t.kind === 'claude' ? askOf(t.sessionId) : null, last = t && askLogOf().find(e => e.sessionId === t.sessionId);
      if (b) { b.hidden = !s && !last; b.textContent = s ? (askUnsentP(s) ? `? ${askOpenN(s)}` : '? \u2713') : '?'; b.classList.toggle('on', !!s && askUnsentP(s)); } }
    $('#term-title').textContent = t ? (t.title || 'session') : 'session';
    if (viewMode === 'text' && t) {
      const box = $('#compose-input');
      const want = t.status === 'busy' ? 'this session is working — your message will queue'
        : t.status === 'waiting' ? 'this session is waiting on you' : 'message this session';
      if (box.placeholder !== want) box.placeholder = want;
    }
    // a refresh held back for a selection: take it as soon as the selection is gone
    if (viewMode === 'text' && tvDeferred && !hasSelection()) { tvDeferred = false; loadTranscript(true); }
    if (viewMode === 'text') scheduleLive();
    if (viewMode === 'text' && t && t.lastAt !== tvLastAt) {
      const box = $('#textview');
      const atEnd = box.scrollHeight - box.scrollTop - box.clientHeight < 80;
      tvLastAt = t.lastAt;
      if (atEnd) loadTranscript(true);     // following the conversation as it happens
    }
  }
}

// ---------------------------------------------------------------- screens
// The list is the root; the bottom bar's screens are its siblings, and a terminal or a sub-list is
// pushed on top. Only popstate pops the stack — back() just asks the browser to go back, so the
// phone's own back gesture and the on-screen ‹ take exactly the same path.
const stack = ['s-list'];
const TABS = new Set(['s-new', 's-search', 's-groups', 's-move', 's-settings']);
const current = () => stack[stack.length - 1];
// The app replaces itself whenever the PC's copy changes, which used to mean landing back on the
// list mid-read. Remember the screen (and which session) and come back to it.
function rememberWhere(id) {
  try { sessionStorage.setItem('deck.where', JSON.stringify({ screen: id, term: termId, page: pageAt ? { n: pageAt.n, path: pageAt.path } : null })); } catch { /* private mode */ }
}
let restored = false;
function restoreWhere() {
  if (restored) return;
  restored = true;
  let w = null;
  try { w = JSON.parse(sessionStorage.getItem('deck.where') || 'null'); } catch { /* ignore */ }
  if (!w || !w.screen || w.screen === 's-list') return;
  if (w.screen === 's-term') { if (w.term && tileById(w.term)) openTerm(w.term); return; }
  if (w.screen === 's-page') { const pg = w.page; if (pg && pg.path) openHtmlPath(pg.path); else if (pg && pg.n != null) openPage(pg.n); else if (typeof pg === 'number') openPage(pg); return; }
  if ($('#' + w.screen)) show(w.screen);
}

const OVER_SESSION = new Set(['s-page', 's-asks']);   // screens that keep the session attached under them (paint)
function paint(id, animate) {
  document.querySelectorAll('.screen').forEach(s => s.classList.toggle('on', s.id === id));
  if (animate) { const now = $('#' + id); now.classList.add('pushed'); setTimeout(() => now.classList.remove('pushed'), 200); }
  document.body.classList.toggle('termview', id === 's-term');
  document.querySelectorAll('#tabbar button').forEach(b => b.classList.toggle('on', b.dataset.go === id));
  // A screen opened over a session (the page viewer, the ask screen from its "?") sits over it: the session stays
  // attached, so back (or the ask screen's Done) finds it as it was, the text view where you were reading, instead
  // of a session screen with no session behind it, whose menu and "?" did nothing. Its size is not touched meanwhile
  // (that is all guarded by current() === 's-term'). Any other screen detaches it as before.
  if (id !== 's-term' && !(OVER_SESSION.has(id) && termId)) detachTerm();
  rememberWhere(id);
  onEnter(id);
}
function show(id) {
  if (current() === id) return;
  // A session opened FROM a bottom-bar screen (new, search, past, elsewhere…) takes that screen's place
  // in the stack rather than stacking on it: back from the session lands on the list, not on the
  // screen you came through (ruled 2026-09-03).
  if (id === 's-term' && TABS.has(current()) && current() !== 's-list') {
    stack[stack.length - 1] = id;
    history.replaceState({ screen: id }, '');
    paint(id, true);
    return;
  }
  stack.push(id);
  history.pushState({ screen: id }, '');
  paint(id, true);
}
// A bottom-bar tab is a sibling of the list, not a child of whatever is showing: switching tabs
// replaces the top of the stack, so one back always lands on the list rather than walking every
// tab you happened to visit.
function goTab(id) {
  // the icon of the screen you are on closes it: back to the list
  if (current() === id) { if (id !== 's-list') back(); return; }
  if (!TABS.has(id) || current() === 's-list') return show(id);
  stack[stack.length - 1] = id;
  history.replaceState({ screen: id }, '');
  paint(id, true);
}
function back() { if (stack.length > 1) history.back(); }
// The sheet and the system back: opening a sheet pushes a history entry, so the phone's back
// closes the sheet rather than leaving the screen; closing it any other way consumes that entry.
let sheetPushed = false, sheetClosing = false, sheetOpenedAt = 0;
function showSheet() {
  const sheet = $('#sheet');
  if (sheet.hidden) { try { history.pushState({ sheet: true }, ''); sheetPushed = true; } catch { sheetPushed = false; } }
  sheet.hidden = false;
  sheetOpenedAt = Date.now();
}
function hideSheet() {
  const sheet = $('#sheet');
  if (sheet.hidden) return;
  sheet.hidden = true;
  if (sheetPushed) { sheetPushed = false; sheetClosing = true; try { history.back(); } catch { sheetClosing = false; } }
}
window.addEventListener('popstate', () => {
  if (sheetClosing) { sheetClosing = false; return; }               // our own back, after a tap closed the sheet
  if (sheetPushed) { sheetPushed = false; $('#sheet').hidden = true; return; }   // the system back closes the sheet
  if (stack.length > 1) stack.pop();
  paint(current(), false);
});
function onEnter(id) {
  if (id === 's-list') renderList();
  else if (id === 's-search') { renderSearch(); setTimeout(() => $('#search-input').focus(), 60); }
  else if (id === 's-new') renderNew();
  else if (id === 's-groups') renderGroups();
  else if (id === 's-move') renderMove();
  else if (id === 's-elsewhere') renderElsewhere();
  else if (id === 's-past') renderPast();
  else if (id === 's-settings') renderSettings();
  else if (id === 's-asks') renderAsks();
}
document.querySelectorAll('[data-back]').forEach(b => b.addEventListener('click', back));
{ const b = document.getElementById('term-ask'); if (b) b.addEventListener('click', () => { const t = tileById(termId); if (t && t.sessionId) openAsksFor(t.sessionId); }); }
document.querySelectorAll('#tabbar button').forEach(b => b.addEventListener('click', () => goTab(b.dataset.go)));

// ---------------------------------------------------------------- terminal
let term = null, fit = null, termId = null, ctrlArmed = false;
// line mode is the default: it is the one where the phone keyboard cannot corrupt the session
let inputMode = (() => { try { return localStorage.getItem('deck.inputmode') || 'line'; } catch { return 'line'; } })();
// One font for everything that shows session text. The family follows the deck's own setting (the
// PC's terminal font, if it is one the phone has), the size is the phone's to choose, and the
// leading is fixed so a row in the terminal and a line in the text view are the same height.
const MONO_LH = 1.3;
function monoFamily() {
  const f = (state.settings.fontFamily || '').trim().replace(/"/g, '');
  return (f ? '"' + f + '", ' : '') + 'ui-monospace, "Cascadia Mono", Menlo, monospace';
}
// Width as a number of columns. Claude Code draws its diffs and boxes a dozen columns narrower
// than the terminal, which is nothing at 200 columns and a fifth of the screen at 56; the only
// way to give it more is a smaller face, so this setting chooses the face for a wanted width.
// 60 columns is the ruled default (2026-09-03): a phone that has not chosen since gets it once, and the face
// follows from the width alone — the keyboard, the key rows and the box change the height, never the size
try { if (!localStorage.getItem('deck.cols60')) { localStorage.setItem('deck.cols', '60'); localStorage.setItem('deck.cols60', '1'); } } catch { /* private mode */ }
function colsSetting() { try { return Math.max(0, Math.min(200, +(localStorage.getItem('deck.cols') || 60))); } catch { return 60; } }
// the typing block (the prompt row and footer under the terminal, and the footer under the box) keeps its
// own width, 60 columns by default, so the terminal's own width setting or a pinch never changes its size
function typeColsSetting() { try { return Math.max(40, Math.min(200, +(localStorage.getItem('deck.typecols') || 60))); } catch { return 60; } }
function typeSize() { if (!charRatio) measureCharRatio(); const wrap = $('.term-wrap'); const w = (wrap && wrap.clientWidth) || window.innerWidth; return Math.max(6, Math.min(20, Math.floor(w / (typeColsSetting() * charRatio)))); }
let charRatio = 0;      // one glyph's width as a fraction of the font size, for the face in use
function measureCharRatio() {
  try {
    const c = document.createElement('canvas').getContext('2d');
    c.font = '100px ' + monoFamily();
    charRatio = c.measureText('MMMMMMMMMM').width / 1000 || 0.6;
  } catch { charRatio = 0.6; }
}
function monoSize() {
  const cols = colsSetting();
  if (cols) {
    if (!charRatio) measureCharRatio();
    const wrap = $('.term-wrap');
    const w = (wrap && wrap.clientWidth) || window.innerWidth;
    return Math.max(6, Math.min(20, Math.floor(w / (cols * charRatio))));
  }
  try { return Math.max(9, Math.min(20, +(localStorage.getItem('deck.fontpx') || 12))); } catch { return 12; }
}
function applyFonts() {
  const root = document.documentElement.style;
  root.setProperty('--mono', monoFamily());
  root.setProperty('--mono-size', monoSize() + 'px');
  root.setProperty('--type-size', typeSize() + 'px');
  root.setProperty('--mono-lh', String(MONO_LH));
  if (term) {
    let changed = false;
    if (term.options.fontFamily !== monoFamily()) { term.options.fontFamily = monoFamily(); changed = true; }
    if (term.options.fontSize !== monoSize()) { term.options.fontSize = monoSize(); changed = true; }
    if (changed) requestAnimationFrame(fitTerm);
  }
}

// Who owns the size of a session: a CLAIM by one kind of screen (lib/driverrule.js has the rule; design
// 2026-11-18). 'phone': this phone, attached or not. 'office': the deck's own windows (the word was 'desk').
// 'remote': the remote desk, for the sessions it shows. 'auto': whoever is looking — this phone while it is
// attached to a session, else a remote desk showing it, else the office deck. The setting lives in the deck,
// so every screen agrees; any of them can change it.
function driverMode() {
  const v = state.settings && state.settings.driver;
  return v === 'desk' ? 'office' : (['auto', 'office', 'phone', 'remote'].includes(v) ? v : 'auto');
}
// This phone is a viewer of the session's size, and sends none of its own, when another screen has claimed it.
function phoneMirrors() { const m = driverMode(); return m === 'office' || m === 'remote'; }
// a viewer draws the session at the pty's size: true when the pty is no longer the size this terminal shows
function viewerDrifted() {
  if (!phoneMirrors() || !term || !termId) return false;
  const t = tileById(termId), ps = t && t.ptySize;
  return !!(ps && ps.cols > 1 && ps.rows > 1 && (term.cols !== ps.cols || term.rows !== ps.rows));
}
function driverName(kind) {
  if (kind === 'phone') return 'this phone';
  if (kind === 'office') return 'the office deck';
  const n = state.remoteDesk && state.remoteDesk.name;
  return 'the remote desk' + (n ? ` (${n})` : '');
}
// the same sentences as the desk's button (renderer/js/80-sidebar.js), said from the phone
function driverTitle(m) {
  if (m === 'auto') return 'Auto: this phone drives the size of the session it has open, else a remote desk showing it, else the office deck. Tap to drive from this phone.';
  return `Sizes follow: ${driverName(m)}. ` + (m === 'phone' ? 'Tap to let whoever is looking drive (auto).' : 'Tap to drive from this phone.');
}
// A shell tile (PowerShell here, the login shell on a Mac) is a plain terminal: no transcript on disk,
// no Claude Code input box, no alternate screen. Everything that reads or drives Claude Code's screen —
// the text view, the outbox, the ctrl+End "back to the bottom" chord, bracketed paste — is for a
// Claude tile only; a shell opened in the text view showed 'no such session' and swallowed every
// command it ran (2026-09-10, "I can see them but not text into them").
function isShellTile(id = termId) { const t = id ? tileById(id) : null; return !!(t && t.kind === 'shell'); }
function paintDrive() {
  const b = $('#t-drive'); if (!b) return;
  const m = driverMode();
  // the WORD is who drives; green (.on) = this phone, dimmed (.other) = another screen, plain = auto
  if (b.textContent !== m) b.textContent = m;
  b.classList.toggle('on', m === 'phone');
  b.classList.toggle('other', m === 'office' || m === 'remote');
  const title = driverTitle(m); if (b.title !== title) b.title = title;
  const note = $('#desknote-text'), text = deskNoteText(); if (note && note.textContent !== text) note.textContent = text;
}
function deskNoteText() {
  return `${driverMode() === 'remote' ? 'The remote desk' : 'The office deck'} owns the size of this session, so the terminal here is a viewer at its width. The text view and the message box work as usual.`;
}
function setDriverMode(mode) {
  return call('setDriver', mode).then(v => {
    state.settings = { ...(state.settings || {}), driver: v };
    paintDrive();
    // the switch's own label says it
    if (current() === 's-term') requestAnimationFrame(fitTerm);
    return v;
  }).catch(e => { toast(String(e.message || e)); });
}
// ONE tap: this phone drives. A tap while it already does: release to auto. A phone has no tooltip, so the
// sentence is said in a toast.
$('#t-drive').addEventListener('click', () => {
  setDriverMode(driverMode() === 'phone' ? 'auto' : 'phone').then(v => v && toast(driverTitle(driverMode()).replace(/ Tap to .*$/, '')));
});

function ensureTerm() {
  if (term) return term;
  applyFonts();
  term = new Terminal({
    fontSize: monoSize(),
    fontFamily: monoFamily(),
    lineHeight: MONO_LH,
    theme: { background: '#000000', foreground: '#e6e6e6' },
    cursorBlink: true,
    scrollback: 4000,
    convertEol: false,
    // the phone keyboard runs autocorrect and swipe-typing through xterm's hidden textarea;
    // both turn terminal input into nonsense
    screenReaderMode: false,
  });
  fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open($('#term'));
  const ta = $('#term').querySelector('textarea');
  if (ta) {
    ta.setAttribute('autocorrect', 'off'); ta.setAttribute('autocapitalize', 'none');
    ta.setAttribute('autocomplete', 'off'); ta.setAttribute('spellcheck', 'false');
    // Gboard ignores autocorrect=off on a plain text field and keeps swipe-typing and predictions,
    // which arrive as whole replaced words and wreck a TUI. inputmode=url drops the prediction strip.
    ta.setAttribute('inputmode', 'url');
  }
  attachTouchScroll();
  // The deep buffer only applies once the session is known to be a full-screen app, and that is
  // only known after the replay has put xterm into the alternate buffer — which can be AFTER the
  // first fit. Re-size on the switch, or the session stays at the window's height for good.
  term.buffer.onBufferChange(() => requestAnimationFrame(sizeTerminal));
  phoneReplies(term);
  window.__deckTerm = term;
  const note = el('div', 'desknote'); note.id = 'desknote'; note.hidden = true;
  const noteText = el('div', '', deskNoteText()); noteText.id = 'desknote-text';
  note.appendChild(noteText);
  const take = el('button', '', 'Drive from this phone instead');
  take.addEventListener('click', () => setDriverMode('phone'));
  note.appendChild(take);
  $('.term-wrap').appendChild(note);
  const badge = el('div', 'colsbadge'); badge.id = 'colsbadge'; badge.hidden = true;
  $('.stage').appendChild(badge);
  attachPinch($('#textview'));                          // the text view zooms the same way, the same setting
  const sticky = el('pre', 'sticky'); sticky.id = 'sticky'; sticky.hidden = true;
  $('.term-wrap').appendChild(sticky);
  const st = el('div', 'stitch'); st.id = 'stitch';
  $('.term-wrap').insertBefore(st, sticky);
  // the terminal under the stitched view is taller than the wrap; when its focused textarea (at the
  // cursor, far down) is scrolled into view by the keyboard opening, the wrap must not scroll with it
  $('.term-wrap').addEventListener('scroll', () => { const w = $('.term-wrap'); if (w.scrollTop || w.scrollLeft) { w.scrollTop = 0; w.scrollLeft = 0; } }, { passive: true });
  st.addEventListener('scroll', onStitchScroll, { passive: true });
  // the box growing, the keyboard, a mode switch: whatever changes the height, a following view keeps its end
  if (window.ResizeObserver) new ResizeObserver(() => { if (stitchFollow && stitchOn()) st.scrollTop = st.scrollHeight; }).observe(st);
  // a tap gives the terminal the keyboard, as on the terminal itself. Both events: on a scrollable,
  // selectable surface Android does not always turn a tap into a click.
  let stTapY = 0, stTapAt = 0;
  st.addEventListener('touchstart', e => { if (e.touches.length === 1) { stTapY = e.touches[0].clientY; stTapAt = Date.now(); } }, { passive: true });
  const focusTerm = how => {
    term.focus();
    touchStats.focusTaps = (touchStats.focusTaps || 0) + 1; noteTouch();
    // (the diagnosis of 2026-09-03 lived here: Android would not open a keyboard for a focus inside an
    // opacity-0 ancestor; the terminal is hidden by stacking order now, and a tap just works)
    if (document.activeElement !== term.textarea) toast('tap did not focus the terminal (' + how + ')');
  };
  // a tap (short, still) on the stitched view: a collapsed tool line expands; otherwise, in keys mode, the
  // terminal takes the keyboard — off the touch itself, since a click does not always follow here
  let stTapDone = 0;
  const expandAt = () => false;   // (a tap on a row does nothing special: tool output is toggled from the ••• menu)
  st.addEventListener('touchend', e => {
    if (e.changedTouches.length !== 1) return;
    const t = e.changedTouches[0];
    if (Date.now() - stTapAt < 350 && Math.abs(t.clientY - stTapY) < 8) {
      stTapDone = Date.now();
      if (expandAt(t.clientX, t.clientY)) return;
      if (inputMode === 'direct' && term) focusTerm('touch');
    }
  }, { passive: true });
  st.addEventListener('click', ev => {
    // A collapsed tool line ('… ctrl+o to expand'): a tap on it sends the session its own ctrl+o, which
    // toggles the expanded output — for every tool line at once, as Claude Code does it. The row is
    // marked as a control when it is stitched (see .expandable).
    if (Date.now() - stTapDone < 500) return;              // the touch already did it
    if (expandAt(ev.clientX, ev.clientY)) return;
    if (inputMode === 'direct' && term && document.activeElement !== term.textarea) focusTerm('click');
  });
  attachPinch(st);
  paintStitched();                                             // also for poking at from devtools, as tools/cdp.js does on the desktop
  $('#term').addEventListener('click', () => { if (inputMode === 'direct') term.focus(); });
  term.onData(d => {
    if (ctrlArmed) {
      // sticky ctrl: the soft keyboard gives no usable key events, so fold the modifier into the
      // character xterm hands us
      ctrlArmed = false; $('#k-ctrl').classList.remove('on');
      const c = d.charCodeAt(0);
      if (d.length === 1 && c >= 63 && c <= 127) d = String.fromCharCode(c === 63 ? 127 : c & 0x1f);
    }
    if (termId) { sendRaw({ t: 'input', id: termId, d }); sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [termId] }); }
  });
  // xterm's own resize event is not a source of truth any more: sizeTerminal is, and it sends.
  return term;
}
// Scrolling. xterm 6 has no DOM scroll area to drag — the viewport's scrollHeight equals its
// clientHeight and the buffer is scrolled through the API — so a touch drag has to be turned into
// scrollLines() by hand. Without this the terminal cannot be scrolled on a phone at all: there is no
// wheel event, and nothing in the DOM to fling.
// How to scroll depends entirely on what the session is doing with the terminal.
//
// Claude Code runs in the ALTERNATE screen buffer with mouse tracking on (verified: it emits
// ?1049h, ?1000h/?1002h/?1003h and SGR ?1006h). The alternate buffer HAS no scrollback — there is
// nothing for scrollLines() to move, which is why every attempt to scroll xterm's buffer did
// nothing at all. The application owns its own view and scrolls it when it receives wheel events,
// so that is what we send: SGR-encoded wheel notches, straight down the same pipe as a keystroke.
//
// A plain shell tile is in the normal buffer and does have scrollback, so there we scroll xterm.
const ESC = String.fromCharCode(27);
// A wheel event is only a wheel event to an application that asked for mouse reports. Sent while
// mouse tracking is off — a session still on its trust or resume prompt after a restart — the bytes
// are typed into its input box as text ('4;40;12M', seen 2026-09-03 15:51). So no reports unless the
// terminal underneath says the mode is on.
// R8 (2026-09-05): while remote mode is active a remote client sends only what the user actually
// typed — no synthesized sequences at all. This is that ruling, and it is also the only lever we have on
// a live bug: a well-formed SGR wheel report is parsed by Claude Code only as far as `ESC[<6`, and the
// tail `4;40;12M` lands in its prompt as text. Its embedded `1` is then read by an option picker as
// "select option 1" — it silently answered five of 2026-09-05, and three more at
// ~19:58 EDT the same evening in another session. Reported upstream; nothing here can parse it for them.
//
// Note the gate below was probably never wrong: Claude Code's TUI genuinely has mouse tracking on, so
// `mouseTrackingMode` reads true honestly and `t.mouseOn` (the stream's own answer, added to the tile
// meta the same day) would agree. That is exactly why a stricter gate is not the fix and not sending is.
//
// COST, accepted when R8 was decided: no live scrolling of a Claude Code tile from a remote client.
// The alternate screen has no scrollback, so `wheelSeq` was the only thing that could move it. Plain
// shell tiles are unaffected (normal buffer, xterm scrolls locally) and the transcript text view is
// still the reading path. To get scrolling back, turn remote mode off from the desktop client.
//
// TO REVERT: delete the one line marked R8. Everything below it is untouched.
// TO DO BETTER: scroll with real key sequences instead (Claude Code already answers `ctrl+End`, which
// this file sends today) — that keeps the feature and still honours R8, since a keystroke a session does
// not bind is a no-op rather than garbage in the prompt. Needs testing against its actual bindings.
const REMOTE_KEYSTROKES_ONLY = true;
function mouseOn() {
  try {
    if (REMOTE_KEYSTROKES_ONLY) return false;            // R8 — delete this line to restore wheel reports
    const t = termId ? tileById(termId) : null;
    if (t && t.status === 'starting') return false;      // still on its trust or resume prompt: no mouse yet
    if (t && t.mouseOn === false) return false;          // the STREAM says no reports; xterm's mode may be stale
    return !!term && term.modes.mouseTrackingMode !== 'none';
  } catch { return false; }
}
const wheelSeq = (back, notches) => {
  const button = back ? 64 : 65;                       // SGR: 64 = wheel up, 65 = wheel down
  let out = '';
  for (let i = 0; i < notches; i++) out += ESC + '[<' + button + ';40;12M';
  return out;
};
// Trust the main process as well as xterm: it tracks the session's screen mode from the stream,
// so a session is known to be full-screen the instant it opens, before any replay has landed.
const inAltScreen = () => {
  if (term && term.buffer.active.type === 'alternate') return true;
  const t = termId ? tileById(termId) : null;
  return !!(t && t.altScreen);
};
const LINES_PER_NOTCH = 3;
// A wheel notch moves the application about three lines, and there is no finer unit available — so
// smoothness is a question of TIMING, not size. Emitting notches the moment a drag accumulates them
// delivers them in clumps (four at once, then nothing), which reads as chop. Queue them and release
// one per animation frame: the same distance, spread evenly, and the queue is capped so the view
// never keeps travelling after the finger has stopped.
// ---------------------------------------------------------------- the deep buffer
// An alt-screen application has no scrollback: to see earlier content the application must redraw,
// and that redraw has to cross the network. That is a hard floor on how smooth scrolling can be —
// unless the phone already HOLDS more than it shows.
//
// So tell the session the terminal is several screens tall. It draws that much, all of it arrives
// here, and this shows a window onto it: the terminal is translated under a clipping box, so moving
// the window is a CSS transform and nothing else. Dragging inside the buffer is pixel-smooth and
// costs no network at all. Only when the window reaches the edge of what the session has drawn do we
// ask it to scroll, which is the round trip we used to pay for every single step.
const BUFFER_SCREENS = 8;                 // how much taller than the window
const MAX_BUFFER_ROWS = 600;              // a sane ceiling whatever the font size
let deepBuffer = (() => { try { return localStorage.getItem('deck.deep') !== '0'; } catch { return true; } })();
let viewRows = 0, bufRows = 0, viewOffset = 0;   // offset measured in rows from the top of the buffer

function rowPx() {
  const rows = $('#term').querySelector('.xterm-rows');
  if (!rows || !rows.children.length) return 0;
  return rows.getBoundingClientRect().height / rows.children.length;
}
function maxOffset() { return Math.max(0, bufRows - viewRows); }
// Claude Code writes 'Jump to bottom (ctrl+End)' over the last row whenever it is scrolled up.
// There is no ctrl+End on a phone (the drag is the way back), so the words are blanked before
// xterm sees them — same length, so nothing else on the row moves.
// The hint is drawn on a grey background, so the styling around it goes as well as the letters
// (blanking only the letters left a grey box of spaces). And it is drawn OVER the last content row
// of the window, whose characters under it are gone: that row is never stitched from a repaint
// that carried the hint — the row is already in the history from a repaint where it was clean.
let hintScrubbedAt = 0;
function scrubHints(d) {
  if (d.indexOf('Jump to bottom') < 0) return d;
  hintScrubbedAt = Date.now();
  return d.replace(/(?:\x1b\[[0-9;]*m)*Jump to bottom \(ctrl\+End\)( \u2193)?(?:\x1b\[[0-9;]*m)*/g, (m, arrow) => ' '.repeat(27 + (arrow ? 2 : 0)));
}
// The bottom of the session's screen — its input box and the footer under it — stays put while
// you read above it, the way a chat's box does. It is the text of those rows, pinned over the
// terminal in the same face; shown only when the window is above the bottom, so at rest the
// terminal's own rows are what you see.
let stickyText = '', stickyTimer = null;
function stickyOn() { try { return localStorage.getItem('deck.sticky') !== '0'; } catch { return true; } }
function bottomStripText() {
  if (!term) return '';
  const buf = term.buffer.active, rows = [];
  for (let i = 0; i < term.rows; i++) { const l = buf.getLine(buf.baseY + i); rows.push(l ? l.translateToString(true).replace(/\s+$/, '') : ''); }
  const seps = [];
  rows.forEach((r, i) => {
    if (!/^\s*\u2500{8,}\s*$/.test(r)) return;
    if (seps.length && seps[seps.length - 1].end === i - 1) seps[seps.length - 1].end = i; else seps.push({ start: i, end: i });
  });
  if (seps.length < 2) return '';
  const start = seps[seps.length - 2].start;
  let end = rows.length; while (end > start && !rows[end - 1]) end--;
  if (end - start > 12) return '';                    // a dialog, not the box: leave it be
  return rows.slice(start, end).join(String.fromCharCode(10));
}
function paintSticky() {
  const el_ = $('#sticky'); if (!el_) return;
  stickyText = (stickyOn() && deepBuffer && !stitchOn() && inAltScreen()) ? bottomStripText() : '';   // the stitched view keeps its bottom block fixed; the pinned copy is for the terminal itself
  if (el_.textContent !== stickyText) el_.textContent = stickyText;
  showSticky();
}
function showSticky() {
  const el_ = $('#sticky'); if (!el_) return;
  el_.hidden = !(stickyText && !selecting && !atEnd());
}
function scheduleSticky() { clearTimeout(stickyTimer); stickyTimer = setTimeout(paintSticky, 120); }

// ---------------------------------------------------------------- the stitched scrollback
// The session's screen is a window on its own history, and every wheel event moves that window and
// repaints all of it. Moving the reader's window over a buffer of it (the deep buffer) is smooth
// until the buffer runs out; then a repaint lands with everything shifted, and however well the
// shift is measured there is a seam. This removes the seam by never showing the buffer at all:
// every repaint is STITCHED into one local history, row by row, found by a fingerprint of three
// distinctive rows, and that history is what is on screen — plain rows in the terminal's face and
// colours, scrolled natively. Older rows are prepended when the session scrolls back for us; the
// scroll position is corrected by exactly the inserted height in the same frame, so nothing moves
// under the finger. Rows that changed in place (a spinner, a diff being drawn) are replaced in
// place. The xterm underneath still parses the stream — the choice buttons, the live strip and the
// pinned bottom read it — it just is not what you look at.
// A tool's result line — the elbow (⎿ 'Read 40 lines') — and, where the width allows it, Claude Code's own
// hint ('… ctrl+o to expand'); at 60 columns the hint is not drawn, the elbow always is
const toolOutputOpen = {};   // session id -> whether its tool output was expanded from here (ctrl+o is a toggle)
const transcriptOpen = {};
const toolOutputByAll = {};  // session id -> the tool output was turned on by 'expand all', to be turned off with it   // session id -> whether its full transcript view was opened from here (ctrl+r; Esc leaves)
function stitchOn() { try { return localStorage.getItem('deck.stitch') !== '0'; } catch { return true; } }
// The stitched view is a black layer painted over the terminal, filled from the session's alternate-screen
// repaints. A shell never enters the alternate screen, so nothing is ever stitched for it and the layer
// stayed an empty black sheet over a perfectly good terminal (2026-09-10: a new PowerShell from the phone
// was a black screen). For a shell the terminal itself is what you look at.
function paintStitched() { document.body.classList.toggle('stitched', stitchOn() && !isShellTile()); }
let sText = [], sHtml = [], sEl = [];       // the history: text for matching, html for showing
let stitchAnchor = 0;                       // where the pty's row 0 sits in the history
let stitchTimer = null, stitchFollow = true, stitchFetching = false, stitchFetchTimer = null, stitchQuietUntil = 0;
let sessionBack = 0;                        // net wheel-ups sent: how far back the session's own window is
let stitchExpectTop = false;
const PALETTE = ['#000000', '#cd3131', '#0dbc79', '#e5e510', '#2472c8', '#bc3fbc', '#11a8cd', '#e5e5e5',
  '#666666', '#f14c4c', '#23d18b', '#f5f543', '#3b8eea', '#d670d6', '#29b8db', '#e5e5e5'];
function paletteColor(i) {
  if (i < 16) return PALETTE[i];
  if (i < 232) { const n = i - 16, r = Math.floor(n / 36), g = Math.floor(n / 6) % 6, b = n % 6; const v = x => x ? 55 + x * 40 : 0; return `rgb(${v(r)},${v(g)},${v(b)})`; }
  const g = 8 + (i - 232) * 10; return `rgb(${g},${g},${g})`;
}
function cellColor(cell, fg) {
  if (fg ? cell.isFgDefault() : cell.isBgDefault()) return '';
  const v = fg ? cell.getFgColor() : cell.getBgColor();
  if (fg ? cell.isFgRGB() : cell.isBgRGB()) return `rgb(${(v >> 16) & 255},${(v >> 8) & 255},${v & 255})`;
  return paletteColor(v);
}
const ESC_HTML = { '&': '&amp;', '<': '&lt;', '>': '&gt;' };
function rowHtml(line, cursorX = -1) {
  // runs of one style become one span; the row's trailing blanks are dropped
  let out = '', run = '', style = '', cls = '';
  const flush = () => { if (!run) return; out += (style || cls) ? `<span${cls ? ' class="' + cls + '"' : ''}${style ? ' style="' + style + '"' : ''}>${run}</span>` : run; run = ''; };
  const n = line.length;
  let last = n; while (last > 0) { const c = line.getCell(last - 1); if (c && c.getChars().trim()) break; last--; }
  const cell = {};
  for (let x = 0; x < last; x++) {
    const c = line.getCell(x, cell.c); if (!c) continue; cell.c = c;
    if (c.getWidth() === 0) continue;                    // the second half of a wide glyph
    let fg = cellColor(c, true), bg = cellColor(c, false);
    if (c.isInverse()) { const t = fg || '#e6e6e6'; fg = bg || '#000000'; bg = t; }
    const st = (fg ? 'color:' + fg + ';' : '') + (bg ? 'background:' + bg + ';' : '');
    const cl = (c.isBold() ? 'b ' : '') + (c.isItalic() ? 'i ' : '') + (c.isUnderline() ? 'u ' : '') + (c.isDim() ? 'd ' : '') + (c.isStrikethrough() ? 's ' : '');
    if (st !== style || cl.trim() !== cls) { flush(); style = st; cls = cl.trim(); }
    const ch = c.getChars() || ' ';
    let esc = ch.length === 1 && ESC_HTML[ch] ? ESC_HTML[ch] : ch;
    // a glyph outside ASCII comes from whatever font has it, at whatever width: pin it to its cell
    if (ch.charCodeAt(0) > 0x7e) esc = '<span class="g' + (c.getWidth() === 2 ? ' w' : '') + '">' + esc + '</span>';
    if (x === cursorX) { flush(); out += '<span class="cur">' + esc + '</span>'; continue; }   // the cursor cell
    run += esc;
  }
  flush();
  if (cursorX >= last) out += ' '.repeat(cursorX - last) + '<span class="cur"> </span>';   // the cursor past the text
  return out;
}
function resetStitch() {
  sText = []; sHtml = []; sEl = []; stitchAnchor = 0; sessionBack = 0; stitchFollow = true;
  stitchFetching = false; stitchExpectTop = false; stitchQuietUntil = 0; stitchDir = null; stitchPeek = false; stitchEmpty = 0; stitchTopReached = false;
  stitchArmed = false; stitchSizeSentAt = 0; stitchTurbo = false; stitchTopHitAt = 0; stitchLastUp = null; stitchLpnMeasured = false; stitchLpn = LINES_PER_NOTCH;
  clearTimeout(stitchFetchTimer);
  if (termId && stitchOn() && !isShellTile()) sendRaw({ t: 'input', id: termId, d: ESC + '[1;5F' });   // whatever was in flight, the session's window belongs at its bottom now (a shell reads ctrl+End as delete-to-end-of-line: never sent one)
  const st = $('#stitch'); if (st) st.textContent = '';   // the tail is recreated on the next repaint
}
// 80 ms of quiet guards against stitching a half-drawn repaint; while a pull is in flight the answer is a
// whole repaint, so 50 is enough
// After a size went out, the first repaint at our size may come in pieces (a busy session): arm only
// once the data has been quiet for a moment, so a half-drawn screen is never the first thing stitched.
let armTimer = null, armFirstAt = 0;
let armNeedsRedraw = false;   // set by a width change: the first output at the new width may be garbled
function armWhenSettled() {
  if (stitchArmed) return;
  if (!armFirstAt) armFirstAt = Date.now();
  clearTimeout(armTimer);
  const wait = Date.now() - armFirstAt > 2500 ? 0 : 220;   // but not forever: a session that streams settles never
  armTimer = setTimeout(() => {
    if (stitchArmed) return;
    // After a width change the session may still have been writing at the OLD width when the pty
    // changed under it, and its repaint rewrites only the rows it thinks changed — the garbled ones
    // stay. So once that first output settles: clear the screen here and ask for a full repaint (the
    // nudge), and stitch only from what that brings.
    if (armNeedsRedraw && term) {
      armNeedsRedraw = false;
      if (inAltScreen()) term.write(String.fromCharCode(27) + '[2J' + String.fromCharCode(27) + '[H');
      armFirstAt = 0;
      forceRedraw();
      return;                                             // the nudge's output arms us, through here again
    }
    stitchArmed = true; armFirstAt = 0; scheduleStitch();
  }, wait);
}
function scheduleStitch() { if (!stitchOn()) return; clearTimeout(stitchTimer); stitchTimer = setTimeout(stitchFromTerm, stitchFetching ? 50 : 80); }
// find the pty screen inside the history: a triple of distinctive rows, looked for near where the
// screen was last time first, then anywhere
function findAnchor(text) {
  const n = text.length;
  // a row worth matching on: long, and not a rule of one repeated character (those repeat on every
  // screen — the input box's separators — and would align every screen by its bottom)
  const good = r => r.length > 12 && !/^(.){7,}$/.test(r);
  const triples = [];
  // sampled across the whole screen: the overlap with what we have may be at its top (scrolled back)
  // or at its bottom (the screen grew taller, or new output arrived)
  for (const f of [0, 0.15, 0.3, 0.45, 0.6, 0.75, 0.9]) {
    for (let i = Math.floor(f * n); i < n - 2; i++) {
      if (good(text[i]) && good(text[i + 1]) && good(text[i + 2])) { if (!triples.includes(i)) triples.push(i); break; }
    }
  }
  // and the last rows, closely: a small overlap at the bottom is the common case when the screen grew
  for (let i = n - 3; i >= Math.max(0, n - 40); i -= 3) {
    if (good(text[i]) && good(text[i + 1]) && good(text[i + 2]) && !triples.includes(i)) triples.push(i);
  }
  if (!triples.length) return null;
  const at = (i, from, to) => {
    for (let k = Math.max(0, from); k < Math.min(sText.length - 2, to); k++) {
      if (sText[k] === text[i] && sText[k + 1] === text[i + 1] && sText[k + 2] === text[i + 2]) return k;
    }
    return -1;
  };
  // every triple votes for an alignment; the majority wins, a lone vote still counts
  const votes = new Map();
  for (const i of triples) {
    const guess = stitchAnchor + i;
    let k = at(i, guess - 2 * n, guess + 2 * n);
    if (k < 0) k = at(i, 0, sText.length);
    if (k >= 0) votes.set(k - i, (votes.get(k - i) || 0) + 1);
  }
  if (!votes.size) return null;
  let best = null, bestN = 0;
  for (const [a, c] of votes) if (c > bestN) { best = a; bestN = c; }
  return best;
}
// The screen has two parts: the content, which is a window on the session's history, and the input
// box with the footer under it, which sits at the bottom of the screen whatever the content shows.
// Only the content is stitched; the box is a fixed tail after the history, replaced in place.
function splitScreen(text) {
  const isRule = r => /^\s*\u2500{8,}\s*$/.test(r);
  const seps = [];
  text.forEach((r, i) => {
    if (!isRule(r)) return;
    if (seps.length && seps[seps.length - 1].end === i - 1) seps[seps.length - 1].end = i; else seps.push({ start: i, end: i });
  });
  let end = text.length; while (end > 0 && !text[end - 1]) end--;
  // what a box is made of: rules, the prompt, the footer, blanks. The box starts at the earliest
  // rule after which there is nothing else — the normal box, an older drawing of it left above by a
  // repaint caught half-way, or a box only half drawn (its first rule so far). A dialog drawn where
  // the box was is taller than twelve rows and is content.
  const boxish = r => !r || isRule(r) || /^[\u276f>](\s|$)/.test(r) || /% context|shift\+tab|mode on|\/rc$/.test(r);
  // whatever follows the last rule is the footer, whatever it says (the context line, the mode line,
  // a worktree name, more to come), as long as it is short; a footer line need not be recognised
  // ...and the same for the few rows after ANY rule, so an older drawing of the box (with the same
  // unknown footer line) left above the new one is box too, not content
  const afterRule = i => seps.some(g => i > g.end && i - g.end <= 6);
  const boxishAt = (r, i) => boxish(r) || afterRule(i);
  let boxStart = text.length;
  for (let g = 0; g < seps.length; g++) {
    let ok = true;
    for (let i = seps[g].start; i < end; i++) if (!boxishAt(text[i], i)) { ok = false; break; }
    if (ok) { boxStart = seps[g].start; break; }
  }
  if (boxStart < text.length && end - boxStart > 12) boxStart = text.length;
  let contentEnd = boxStart < text.length ? boxStart : end;
  while (contentEnd > 0 && !text[contentEnd - 1]) contentEnd--;
  // the footer: whatever follows the box's last rule (the context line, the mode line)
  let footerStart = end;
  for (let i = end - 1; i >= boxStart; i--) if (isRule(text[i])) { footerStart = i + 1; break; }
  return { contentEnd, boxStart, end, footerStart };
}
function stitchFromTerm() {
  if (!term || !stitchOn() || !inAltScreen() || !stitchArmed) return;
  const st = $('#stitch'); if (!st) return;
  const buf = term.buffer.active, n = term.rows;
  const all = new Array(n), lines = new Array(n);
  for (let i = 0; i < n; i++) { const l = buf.getLine(buf.baseY + i); lines[i] = l; all[i] = l ? l.translateToString(true).replace(/\s+$/, '') : ''; }
  const { contentEnd, boxStart, end, footerStart } = splitScreen(all);
  // the hint is only ever drawn while the session is scrolled back; a repaint at its bottom that
  // follows one within the window must keep its last row (it is the newest row, not a hole)
  const hinted = Date.now() - hintScrubbedAt < 600 && sessionBack > 0;
  // Leading blank rows of a window are never information (whatever is above is in the history
  // already, or there is nothing): a screen redrawn with its content low — the survey, a dialog —
  // must not stitch a black area in. The content rows start at the first non-blank one.
  let lead = 0; while (lead < contentEnd && !all[lead]) lead++;
  const text = all.slice(lead, hinted ? Math.max(0, contentEnd - 1) : contentEnd);   // the hinted row is a hole: leave it out
  const clines = lines.slice(lead);
  stitchContentRows = boxStart;                         // rows of content the window can show
  // the scroll event is asynchronous: if the reader has moved since the last stitch, read the intent
  // from where they are NOW, or a repaint landing mid-drag would snap them back to the end
  if (Math.abs(st.scrollTop - stitchLastTop) > 1) stitchFollow = atEnd();
  const wasAtEnd = atEnd();
  const before = st.scrollHeight, beforeTop = st.scrollTop, lenBefore = sText.length;
  let tail = $('#stitch-tail');
  if (!tail) { tail = el('div', 'tail'); tail.id = 'stitch-tail'; st.appendChild(tail); }
  let anchor = sText.length ? findAnchor(text) : 0;
  let prepended = 0;
  if (anchor === null) {
    // nowhere in what we have: a jump (ctrl+Home / ctrl+End), or the session cleared. Keep the
    // history, mark the gap, and put the screen above or below it as the jump implies.
    touchStats.stitchMiss = (touchStats.stitchMiss || 0) + 1; noteTouch();
    const gapEl = el('div', 'r gap', '\u22ef');
    if (stitchExpectTop || stitchDir === 'up' || sessionBack > 0) {
      sText.unshift('\u22ef'); sHtml.unshift(null); sEl.unshift(gapEl); st.insertBefore(gapEl, st.firstChild);
      anchor = -text.length;
    } else {
      sText.push('\u22ef'); sHtml.push(null); sEl.push(gapEl); st.insertBefore(gapEl, tail);
      anchor = sText.length;
    }
  }
  stitchExpectTop = false;
  if (anchor < 0) {
    prepended = -anchor;
    const frag = document.createDocumentFragment(), nt = [], nh = [], ne = [];
    for (let i = 0; i < prepended; i++) {
      const html = clines[i] ? rowHtml(clines[i]) : '';
      const d = el('div', 'r'); d.innerHTML = html;
      nt.push(text[i]); nh.push(html); ne.push(d); frag.appendChild(d);
    }
    sText = nt.concat(sText); sHtml = nh.concat(sHtml); sEl = ne.concat(sEl);
    st.insertBefore(frag, st.firstChild);
    anchor = 0;
  }
  for (let i = prepended; i < text.length; i++) {
    const idx = anchor + i;
    const html = clines[i] ? rowHtml(clines[i]) : '';
    if (idx >= sText.length) {
      const d = el('div', 'r'); d.innerHTML = html;
      sText.push(text[i]); sHtml.push(html); sEl.push(d); st.insertBefore(d, tail);
    } else if (sHtml[idx] !== html) {
      sText[idx] = text[i]; sHtml[idx] = html; sEl[idx].innerHTML = html; sEl[idx].className = 'r';
    }
  }
  stitchAnchor = anchor;
  if (sessionBack === 0 && stitchDir !== 'up' && !prepended && anchor >= 0 && sText.length > anchor + text.length && sText[sText.length - 1] !== '\u22ef') {
    while (sText.length > anchor + text.length) { const d = sEl.pop(); sText.pop(); sHtml.pop(); if (d) d.remove(); }
  }
  // The measure of a pull: how far the window moved for the notches sent. A burst brings several
  // repaints, each further than the last, so the furthest within a couple of seconds is the answer;
  // it is committed when the next pull goes out.
  if (stitchLastUp && prepended > 0 && Date.now() - stitchLastUp.at < 2500) {
    stitchLastUp.prepended += prepended;
    const moved = (stitchLastUp.anchorBefore + stitchLastUp.prepended) - anchor;
    if (moved >= stitchLastUp.n * 2) stitchLastUp.best = Math.max(stitchLastUp.best, moved / stitchLastUp.n);   // a partial answer is no measure
  }
  // the box and footer, as they are now
  const boxHtml = [];   // (the /rc hint is scrubbed from the footer rows below: remote control is a given here)
  // the cursor, where xterm says it is — normally on the prompt row of the box
  let curY = buf.cursorY, curX = buf.cursorX;
  if (curY < boxStart || curY >= end) {
    // the session parks its cursor elsewhere at times (mid-render, or after the footer): the caret
    // belongs on the prompt row, after whatever is typed there
    for (let i = boxStart; i < end; i++) if (/^[\u276f>]/.test(all[i])) { curY = i; curX = Math.max(2, all[i].length); break; }
  }
  // The block is drawn at its own width, not the terminal's: a rule spans the block whatever number of
  // dashes the session drew (many at a wide terminal, few at a narrow one), and the prompt row wraps
  // within the block rather than at the terminal's columns.
  const isRuleRow = r => /^\s*─{8,}\s*$/.test(r);
  for (let i = boxStart; i < end; i++) {
    if (isRuleRow(all[i])) { boxHtml.push('<div class="r rule"></div>'); continue; }
    const isPrompt = /^[❯>](\s|$)/.test(all[i]);
    boxHtml.push('<div class="r' + (isPrompt ? ' prompt' : '') + '">' + (lines[i] ? rowHtml(lines[i], i === curY ? curX : -1).replace(/(\s|&nbsp;)*\/rc(<\/span>)?\s*$/, '$2') : (i === curY ? ' '.repeat(curX) + '<span class="cur"> </span>' : '')) + '</div>');
  }
  const th = boxHtml.join('');
  // In line mode the message box IS the input, standing where the session's prompt row was: the
  // tail shows nothing and the footer moves to its own strip under the box. In keys mode the
  // session's prompt row is the input and the tail shows it, footer and all.
  const lineMode = inputMode === 'line';
  const fs = $('#footer-strip');
  // the mode, as the footer states it ('auto mode on', 'plan mode on'); nothing said means normal
  const footerText = all.slice(footerStart, end).join(' ');
  const mm = /(\w[\w-]*)\s+mode\s+on/i.exec(footerText);
  const mode = mm ? mm[1].toLowerCase() : (footerText.trim() ? 'normal' : '');
  if (mode !== sessionMode) { sessionMode = mode; paintKeyRow(); }
  const mdl = /\[([^\]]+)\]\s*\[/.exec(footerText);           // '[Opus 5] [▓░░] 23% context'
  sessionModel = mdl ? mdl[1].trim() : sessionModel;
  if (termId && sessionModel) screenModelOf[termId] = sessionModel;
  // the screen says the effort as well — the banner ('Fable 5.1 with xhigh effort') and the reply to
  // /effort ('Set effort level to xhigh') — and the model's reply ('Set model to Opus 5'); the latest wins
  for (let i = all.length - 1; i >= 0; i--) {
    const r = all[i];
    let m = /Set effort level to (\w+)/.exec(r) || /with (\w+) effort/.exec(r);
    if (m) { screenEffort = m[1].toLowerCase(); break; }
  }
  for (let i = all.length - 1; i >= 0; i--) {
    const m = /Set model to ([A-Za-z]+ [\d.]+|[A-Za-z]+)/.exec(all[i]);
    if (m) { sessionModel = m[1].trim(); break; }
    if (/^\s*\[[^\]]+\]\s*\[/.test(all[i])) break;             // the footer's own word stands once found
  }
  // either way the history ends at the content: the bottom block is fixed under the terminal
  if (tail.innerHTML !== '') tail.innerHTML = '';
  const tt = $('#term-tail');
  // Claude Code's transcript view draws no prompt and takes no input until Esc: the block offers the
  // way out where the prompt row would be, and the menu's state follows the screen
  const inTranscript = /Showing detailed transcript/i.test(footerText);
  if (termId) transcriptOpen[termId] = inTranscript;
  const leave = inTranscript ? '<div class="r leave-row"><button class="leave">leave the transcript view</button></div>' : '';
  if (fs) fs.hidden = true;                             // the plain strip is retired: the footer is drawn in colour in every mode
  const footerOnly = boxHtml.slice(Math.max(0, footerStart - boxStart)).join('');
  if (tt) {
    const want = leave + (lineMode ? footerOnly : th);
    if (tt.innerHTML !== want) tt.innerHTML = want;
    tt.hidden = !want || viewMode !== 'terminal';
  }
  const tf = $('#tv-footer');
  if (tf) { if (tf.innerHTML !== footerOnly) tf.innerHTML = footerOnly; tf.hidden = !footerOnly || viewMode !== 'text'; }
  // A run of blank rows in the history is shown as at most two: the session pads the screen — an update
  // notice drawn near the bottom with nothing above it — and the padding is not content. The rows stay
  // (the stitching maps window rows to history rows one for one); they are only hidden past the second.
  { let run = 0; for (let i = 0; i < sText.length; i++) { const blank = !sText[i]; run = blank ? run + 1 : 0; const hide = run > 2; if (sEl[i] && sEl[i].hidden !== hide) sEl[i].hidden = hide; } }
  // a ceiling on the history, trimmed on whichever side the reader is not
  if (sText.length > STITCH_CAP) {
    const drop = sText.length - STITCH_CAP;
    if (st.scrollTop + st.clientHeight / 2 < st.scrollHeight / 2) {
      for (let k = 0; k < drop; k++) { const d = sEl.pop(); sText.pop(); sHtml.pop(); if (d) d.remove(); }
    } else {
      const h0 = st.scrollHeight;
      for (let k = 0; k < drop; k++) { const d = sEl.shift(); sText.shift(); sHtml.shift(); if (d) d.remove(); }
      stitchAnchor -= drop; st.scrollTop -= (h0 - st.scrollHeight);
    }
  }
  if (prepended) {
    st.scrollTop = beforeTop + (st.scrollHeight - before);   // the same rows stay under the finger
    touchStats.stitchUp = (touchStats.stitchUp || 0) + prepended; noteTouch();
  }
  // an upward fetch is answered by a repaint that prepends; any other by the next repaint at all
  // an upward fetch whose window moved but only over rows already held (after a snap to the bottom, the
  // way back up through known history) is progress too: done now, not after the timeout, and not 'empty'
  if (stitchFetching) { if (stitchDir !== 'up') fetchDone(0); else if (prepended || sText.length > lenBefore) fetchDone(Math.max(prepended, sText.length - lenBefore)); else if (stitchAnchor !== stitchAnchorBefore) fetchDone(0, true); }
  // following the end is an intent, set by where the reader last scrolled to, not a measurement:
  // the container can shrink under a pinned view (the key row appearing) and leave it a few rows short
  if (stitchFollow || wasAtEnd) { stitchFollow = true; st.scrollTop = st.scrollHeight; requestAnimationFrame(() => { if (stitchFollow) { st.scrollTop = st.scrollHeight; stitchLastTop = st.scrollTop; } }); }
  stitchLastTop = st.scrollTop;
  showSticky();
}
// Keep STITCH_MARGIN screens of history beyond the reader in whichever direction they move, pulling
// from the moment they leave the centre rather than when they hit an edge. Going up while the reader
// is at the live end is a PEEK: a burst of wheel-ups deep enough to grow the history, and ctrl+End
// the moment its repaint has landed, so the session's own window is back at its bottom and new
// output keeps arriving. Once the reader is up and the session's window is behind them, pulls are
// incremental from where it is. Coming back down, the window is walked down ahead of the reader
// and snapped to the end with ctrl+End for the last screen. A pump runs this, not the scroll event,
// so it keeps going while the finger is still.
const STITCH_MARGIN = 4;            // screens of history to keep beyond the reader, each way
const STITCH_PEEK_MAX = 2000;       // rows: how deep a peek from the live end will go
const STITCH_CAP = 10000;           // rows of history kept
let stitchDir = null, stitchPeek = false, stitchEmpty = 0, stitchTopReached = false, stitchContentRows = 0;
let stitchArmed = false, stitchSizeSentAt = 0, stitchTopAt = 0, stitchTurbo = false, stitchTopHitAt = 0, stitchLastTop = -1;
let stitchLpn = LINES_PER_NOTCH, stitchSentNotches = 0, stitchAnchorBefore = 0;   // measured lines per notch
let stitchLastUp = null;   // { n, anchorBefore, prepended, at, best }: the last pull up, measured over every repaint it brings
let stitchLpnMeasured = false;
// The session's window is sessionBack notches above its bottom. The next window up must still
// overlap the top of the history (or it cannot be stitched) and should grow it by six tenths of a
// window. Its top wants to sit at L + G rows above the bottom, where L is the history's length.
function pullNotches() {
  const win = stitchContentRows || Math.max(1, term.rows - 6);
  // the session answers a burst in ~30 ms: pull big, keep ~50 rows of overlap — once its lines per notch
  // have been measured; until then a smaller pull, so a session that scrolls faster than assumed still overlaps
  const grow = Math.round(win * (stitchLpnMeasured ? 0.85 : 0.6));
  const wantTop = sText.length + grow;
  const haveTop = sessionBack * stitchLpn + win;
  const cap = Math.ceil(2 * win / stitchLpn);            // never more than two windows of lines in one pull
  return Math.max(3, Math.min(cap, Math.ceil((wantTop - haveTop) / stitchLpn)));
}
function stitchNotches() { const win = stitchContentRows || Math.max(1, term.rows - 6); return Math.max(3, Math.round(win * 0.85 / LINES_PER_NOTCH)); }
// a hit on the top only counts when there is something to scroll: an empty or short history is always 'at the top'
function hitTop(st) { return st.scrollTop < st.clientHeight && st.scrollHeight > st.clientHeight * 2; }
function onStitchScroll() { stitchFollow = atEnd(); showSticky(); const st = $('#stitch'); if (st && hitTop(st)) stitchTopHitAt = Date.now(); }
// After a resize the session's window could be anywhere — a repaint at a new size tends to start
// from its bottom — so whatever pull was in flight is forgotten and the window is sent to its
// bottom on purpose; the next pull is computed from there.
function stitchSizeChanged() {
  if (!stitchOn() || !termId || isShellTile()) return;   // a shell: the keyboard opening would otherwise send ctrl+End into the line being typed
  stitchFetching = false; stitchDir = null; stitchPeek = false; clearTimeout(stitchFetchTimer); stitchLastUp = null;
  sessionBack = 0;
  if (!$('#choicerow').hidden) return;                  // not into a menu
  sendRaw({ t: 'input', id: termId, d: ESC + '[1;5F' });
}
function commitLpn() {
  if (stitchLastUp && stitchLastUp.best > 0) { stitchLpn = Math.max(2.5, Math.min(4.5, (stitchLpn + stitchLastUp.best) / 2)); stitchLpnMeasured = true; }
  stitchLastUp = null;
}
function startFetch(dir, notches = 0) {
  stitchFetching = true; stitchDir = dir; stitchSentNotches = notches; stitchAnchorBefore = stitchAnchor;
  clearTimeout(stitchFetchTimer);
  stitchFetchTimer = setTimeout(() => fetchDone(0), 2000);   // no answer: do not stay stuck waiting
}
function fetchDone(prepended, moved = false) {
  const dir = stitchDir; stitchFetching = false; stitchDir = null; clearTimeout(stitchFetchTimer);
  if (dir === 'up') {
    if (prepended > 0 || moved) stitchEmpty = 0;
    else if (++stitchEmpty >= 3) { stitchTopReached = true; stitchTopAt = Date.now(); }   // three times nothing earlier: the start of its history
    else stitchQuietUntil = Date.now() + 1500;
    if (stitchPeek) { stitchPeek = false; sessionBack = 0; if (termId) sendRaw({ t: 'input', id: termId, d: ESC + '[1;5F' }); }
  }
}
function pumpStitch() {
  const st = $('#stitch');
  if (!st || !term || !termId || !stitchOn() || current() !== 's-term' || viewMode !== 'terminal' || selecting || !inAltScreen()) return;
  const above = st.scrollTop, below = st.scrollHeight - st.scrollTop - st.clientHeight, screen = st.clientHeight;
  // Turbo: the reader keeps hitting the top. Every prepend moves them back down (the same rows
  // stay under the finger), so 'at the top' is a recent event, not a position: within the last
  // three seconds. The one lever that is safe is rows per answer, so while it lasts the session
  // is asked for the tallest window we allow, and given back its usual height afterwards. Each
  // pull still overlaps the last: nothing about matching changes.
  if (hitTop(st)) stitchTopHitAt = Date.now();
  const wantTurbo = Date.now() - stitchTopHitAt < 3000 && !stitchTopReached && !phoneMirrors();
  if (wantTurbo !== stitchTurbo) { stitchTurbo = wantTurbo; sizeTerminal(); return; }
  // the reader turned round: a pull upward still waiting for its answer must not hold up the way down
  if (stitchFetching && stitchDir === 'up' && sessionBack > 0 && below < screen * STITCH_MARGIN) { stitchFetching = false; stitchDir = null; stitchPeek = false; clearTimeout(stitchFetchTimer); }
  if (stitchFetching || Date.now() < stitchQuietUntil || phoneMirrors() || !mouseOn()) return;
  if (!$('#choicerow').hidden) return;                   // a menu is up: its keys must not be interleaved with ours
  if (stitchTopReached && Date.now() - stitchTopAt > 8000) { stitchTopReached = false; stitchEmpty = 0; }   // ask again later; a slow repaint is not the top
  if (sessionBack > 0 && below < screen * STITCH_MARGIN) {
    if (below < screen) { sessionBack = 0; sendRaw({ t: 'input', id: termId, d: ESC + '[1;5F' }); startFetch('end'); return; }
    const n = Math.min(sessionBack, stitchNotches());
    sessionBack -= n;
    // the last step down is snapped with ctrl+End as well: notches are counted, not measured, and the
    // session's window must be exactly at its bottom for new output to show
    sendRaw({ t: 'input', id: termId, d: wheelSeq(false, n) + (sessionBack === 0 ? ESC + '[1;5F' : '') }); startFetch(sessionBack === 0 ? 'end' : 'down'); return;
  }
  if (above < screen * STITCH_MARGIN && !stitchTopReached) {
    const following = below < screen;
    if (following && sText.length > STITCH_PEEK_MAX) return;    // deep enough for anyone reading at the end
    const n = pullNotches();
    stitchPeek = following;                                      // following: snap the session back after
    commitLpn();
    stitchLastUp = { n, anchorBefore: stitchAnchor, prepended: 0, at: Date.now(), best: 0 };
    sessionBack += n;
    sendRaw({ t: 'input', id: termId, d: wheelSeq(true, n) }); startFetch('up', n);
  }
}
setInterval(pumpStitch, 60);

let pinchScale = 1;      // a preview while two fingers are down; 1 whenever they are not
function applyOffset() {
  const px = Math.round(viewOffset * rowPx());
  const tr = [];
  if (px > 0) tr.push(`translateY(${-px}px)`);
  if (pinchScale !== 1) tr.push(`scale(${pinchScale})`);
  $('#term').style.transform = tr.join(' ');
  showSticky();
}
// Set the width in columns from anywhere: the pinch, the menu's -/+, the setting.
function setCols(n, say = true) {
  n = Math.max(40, Math.min(160, Math.round(n)));
  try { localStorage.setItem('deck.cols', String(n)); } catch { /* private mode */ }
  charRatio = 0;
  applyFonts();
  if (say) { showBadge(n + ' columns'); hideBadge(); }   // the badge, where the width is, not a note over the keys
}
let badgeTimer = null;
function showBadge(text) { const b = $('#colsbadge'); if (!b) return; clearTimeout(badgeTimer); b.textContent = text; b.hidden = false; }
function hideBadge() { clearTimeout(badgeTimer); badgeTimer = setTimeout(() => { const b = $('#colsbadge'); if (b) b.hidden = true; }, 900); }
function atBottom() { return viewOffset >= maxOffset() - 0.5; }
function atEnd() {
  if (stitchOn()) { const st = $('#stitch'); return !st || st.scrollHeight - st.scrollTop - st.clientHeight < rowPx(); }
  return atBottom();
}
// ---------------------------------------------------------------- the one size
// Every resize the session ever receives from this phone goes through here, and it always carries
// the size this phone INTENDS: the fitted columns and the deep-buffer rows. Three separate paths
// used to send whatever they happened to read at the moment — a nudge could read the rows before
// the buffer had grown and send 38, then the sizer sent 135, then the drift check argued with both.
// Ink repaints on every SIGWINCH, and repaints through a storm of them land on top of each other:
// that is the interleaved-character screen. One intention, one sender, no argument.
let intended = { cols: 0, rows: 0 };
let lastSent = { cols: 0, rows: 0, at: 0 };
let nudgeUntil = 0;
function sendSize(cols, rows) {
  if (!termId || !(cols > 1 && rows > 1)) return;
  if (cols === lastSent.cols && rows === lastSent.rows && Date.now() - lastSent.at < 400) return;
  lastSent = { cols, rows, at: Date.now() };
  if (!stitchArmed) {
    stitchSizeSentAt = Date.now();      // the next repaint is at our size: stitch from there
    // ...unless the pty already has this size (a refresh of the page): then nothing will repaint, and
    // the replay we were handed IS at our size
    const t = tileById(termId);
    if (t && t.ptySize && t.ptySize.cols === cols && t.ptySize.rows === rows) { stitchArmed = true; scheduleStitch(); }
    else setTimeout(() => { if (!stitchArmed && stitchSizeSentAt) { stitchArmed = true; scheduleStitch(); forceRedraw(); } }, 900);   // no repaint came: show what we have, ask for a fresh one
  }
  sendRaw({ t: 'resize', id: termId, cols, rows });
  stitchSizeChanged();
}
// Give the session a taller screen than the window, and show the last screenful of it.
function sizeTerminal() {
  if (!term || !fit) return;
  let d = null;
  try { d = fit.proposeDimensions(); } catch { /* not laid out */ }
  if (!d || !(d.cols > 1) || !(d.rows > 1)) return;
  // another screen drives (the office deck, or the remote desk): show the session at the pty's size, and
  // never tell it a size of ours
  const desk = phoneMirrors();
  // The note sends the reader to the text view, which a shell does not have — so for a shell the
  // terminal stays uncovered and simply shows the desk's width (the driver is still the list bar's).
  const dn = $('#desknote'); if (dn) dn.hidden = !desk || isShellTile();
  if (desk) {
    const t = termId ? tileById(termId) : null, ps = t && t.ptySize;
    if (ps && ps.cols > 1 && ps.rows > 1 && (term.cols !== ps.cols || term.rows !== ps.rows)) term.resize(ps.cols, ps.rows);
    viewRows = d.rows; bufRows = term.rows; intended = { cols: 0, rows: 0 };
    viewOffset = maxOffset(); applyOffset();
    return;
  }
  const stick = bufRows === 0 || atBottom();
  viewRows = d.rows;
  bufRows = (deepBuffer && inAltScreen()) ? Math.min(MAX_BUFFER_ROWS, stitchTurbo && stitchOn() ? MAX_BUFFER_ROWS : d.rows * BUFFER_SCREENS) : d.rows;
  intended = { cols: d.cols, rows: bufRows };
  if (term.cols !== d.cols) { resetStitch(); armFirstAt = 0; clearTimeout(armTimer); armNeedsRedraw = true; }   // rows re-wrap at a new width: the history is void; a full repaint is asked for before stitching
  if (stitchOn() && stitchFollow) requestAnimationFrame(() => { const st = $('#stitch'); if (st && stitchFollow) st.scrollTop = st.scrollHeight; });
  if (term.cols !== d.cols) { term.resize(d.cols, bufRows); if (inAltScreen()) term.write('[2J[H'); }   // a new width: nothing of the old layout stays on screen
  else if (term.rows !== bufRows) term.resize(d.cols, bufRows);
  sendSize(intended.cols, intended.rows);            // the session is told the intention, not a reading
  if (stick) viewOffset = maxOffset();
  viewOffset = Math.min(maxOffset(), Math.max(0, viewOffset));
  applyOffset();
}

// Ask the session for more BEFORE the buffer runs out. Waiting until the window hits the edge
// gives smooth local motion right up to a wall and then a crawl at the speed of the network.
// Instead, once the window comes within a screen of the top, fetch about half a screen more in
// the background.
//
// The session redraws its whole screen with earlier content, so the text under the window shifts
// down by however much it scrolled; the window moves with it and the reader feels nothing. That
// compensation only happens if the screen actually CHANGED — at the start of a session there is
// nothing earlier to fetch, and moving the window then would be a jump out of nowhere.
let prefetching = false;
// When a top-up brings nothing back, the session has nothing earlier to give — the start of its
// own screen history. Stop knocking for a few seconds instead of asking on every drag.
let prefetchQuietUntil = 0;
function topRowText() {
  if (!term) return '';
  const line = term.buffer.active.getLine(term.buffer.active.baseY);
  return line ? line.translateToString(true) : '';
}
// The seam. When the session scrolls back for us, its whole screen is redrawn with earlier content
// and everything under the window moves down. The window must move with it by EXACTLY the same
// amount, at the SAME instant, or the reader sees a jump. Two things used to make that a stutter:
// the shift was assumed (notches x 3 lines) rather than measured, and it was applied on a timer
// rather than when the redraw actually landed. Now a fingerprint of a few rows is taken before
// asking, and when the next redraw arrives those rows are found again in the new buffer: the
// distance they moved is the shift, applied in the same frame the redraw is painted.
let prefetchMark = null;          // { rows: [text...], at: index } taken before asking
function rowText(i) {
  const buf = term.buffer.active;
  const line = buf.getLine(buf.baseY + i);
  return line ? line.translateToString(true).replace(/\s+$/, '') : '';
}
function takeMark() {
  // a few consecutive non-empty rows near the top of the window, distinctive enough to find again
  const from = Math.max(0, Math.round(viewOffset));
  for (let i = from; i < from + Math.max(1, viewRows - 6); i++) {
    const rows = [rowText(i), rowText(i + 1), rowText(i + 2)];
    if (rows.every(r => r.length > 12)) return { rows, at: i };
  }
  return null;
}
function findMark(mark) {
  if (!mark) return -1;
  const n = term.rows;
  for (let i = 0; i < n - 2; i++) {
    if (rowText(i) === mark.rows[0] && rowText(i + 1) === mark.rows[1] && rowText(i + 2) === mark.rows[2]) return i;
  }
  return -1;
}
// called from the data path once the redraw that answers a prefetch has been painted
function settlePrefetch() {
  if (!prefetching || !prefetchMark) return;
  const now = findMark(prefetchMark);
  if (now < 0) return;                               // not this redraw; keep waiting (the timer backstops)
  const moved = now - prefetchMark.at;
  clearTimeout(prefetchTimer);
  if (moved > 0) {
    viewOffset = Math.min(maxOffset(), viewOffset + moved);
    applyOffset();
    prefetchQuietUntil = 0;
  } else {
    prefetchQuietUntil = Date.now() + 4000;            // it did not move: nothing earlier to give
  }
  prefetching = false; prefetchMark = null;
}
let prefetchTimer = null;
function maybePrefetch(goingBack) {
  if (!goingBack || prefetching || !termId || !deepBuffer || !inAltScreen()) return;
  if (Date.now() < prefetchQuietUntil) return;
  if (viewOffset > viewRows * 3) return;               // top up once within three screens of the top
  const mark = takeMark();
  if (!mark || !mouseOn()) return;
  prefetching = true;
  prefetchMark = mark;
  const notches = Math.max(3, Math.round(viewRows * 2 / LINES_PER_NOTCH));   // two screens at a time
  sendRaw({ t: 'input', id: termId, d: wheelSeq(true, notches) });
  // backstop: if no redraw ever comes back, do not stay stuck waiting for one
  clearTimeout(prefetchTimer);
  prefetchTimer = setTimeout(() => {
    if (!prefetching) return;
    prefetchQuietUntil = Date.now() + 4000;
    prefetching = false; prefetchMark = null;
  }, 900);
}

// Rate control. Two failed shapes taught the constraint: one notch per animation frame floods the
// link (sixty full repaints a second) and judders; waiting for each redraw before sending the next
// is too conservative — if the session is slow to answer, or answers nothing because it is already
// at the top, the whole drag runs at a few steps a second, which is jumpy AND slow at once.
//
// So: send on a short timer, and let an arriving redraw release the next batch EARLY. The timer sets
// the floor, the link sets the ceiling. Batches stay small so each step is a few lines rather than a
// lurch, and the queue holds a long drag's full distance instead of throwing it away.
const STEP_MS = 35;              // ~28 steps a second even if nothing comes back
const MAX_BATCH = 6;             // about eighteen lines at the top speed
let notchQueue = 0;              // signed, still to send
let stepTimer = null;
function queueNotches(n) {
  if (!n) return;
  notchQueue = Math.max(-20, Math.min(20, notchQueue + n));
  pumpNotches();
}
function pumpNotches() {
  if (stepTimer || !notchQueue) return;
  const batch = Math.max(-MAX_BATCH, Math.min(MAX_BATCH, notchQueue));
  notchQueue -= batch;
  scrollNotches(batch);
  stepTimer = setTimeout(() => { stepTimer = null; pumpNotches(); }, STEP_MS);
}
// the session answered: we know the link is keeping up, so go again without waiting out the timer
function releaseRedraw() {
  if (!stepTimer || !notchQueue) return;
  clearTimeout(stepTimer);
  stepTimer = null;
  pumpNotches();
}
// notches < 0 goes back through the log
function scrollNotches(notches) {
  if (!term || !notches) return;
  if (!inAltScreen()) { touchStats.lines += Math.abs(notches); noteTouch(); term.scrollLines(notches * LINES_PER_NOTCH); return; }
  if (!termId) return;
  touchStats.wheel += Math.abs(notches);               // what actually leaves the phone
  noteTouch();
  if (!mouseOn()) return;
  sendRaw({ t: 'input', id: termId, d: wheelSeq(notches < 0, Math.abs(notches)) });
}
// lines < 0 goes back through the log, as scrollLines does
function scrollTerminal(lines) {
  if (!lines) return;
  const notches = lines < 0 ? -Math.max(1, Math.round(-lines / LINES_PER_NOTCH))
    : Math.max(1, Math.round(lines / LINES_PER_NOTCH));
  scrollNotches(notches);
}
function scrollTerminalToEnd() {
  if (!term) return;
  if (stitchOn()) { sessionBack = 0; stitchFollow = true; stitchPeek = false; stitchFetching = false; }
  if (!inAltScreen()) { term.scrollToBottom(); return; }
  // the app's own shortcut, which it advertises in its scroll hint
  if (termId) sendRaw({ t: 'input', id: termId, d: ESC + '[1;5F' });
}

// Selecting text and dragging to scroll cannot share one surface: whichever owns the gesture, the
// other cannot happen. So selecting is a mode you turn on, like line/keys — while it is on the
// browser gets the touches back (touch-action: auto) and drag-scrolling stands down.
let selecting = false;
// xterm paints its rows as a grid of absolutely placed spans, and Android's long press will not
// take hold of them however permissive the CSS is. So selection mode swaps the terminal for the
// same characters in an ordinary <pre>: identical text, ordinary selection, ordinary Copy.
function bufferText() {
  if (!term) return '';
  const buf = term.buffer.active;
  const out = [];
  const first = Math.max(0, buf.baseY);
  for (let i = 0; i < term.rows; i++) {
    const line = buf.getLine(first + i);
    out.push(line ? line.translateToString(true).replace(/\s+$/, '') : '');
  }
  while (out.length && !out[out.length - 1]) out.pop();
  while (out.length && !out[0]) out.shift();
  return out.join(String.fromCharCode(10));
}
function setSelecting(on) {
  selecting = !!on;
  document.body.classList.toggle('selecting', selecting);
  $('#selbar').hidden = !selecting;
  $('#selview').hidden = !selecting;
  $('#stage').style.visibility = selecting ? 'hidden' : '';
  document.querySelector('.term-wrap').style.visibility = selecting ? 'hidden' : '';
  if (selecting) {
    // the whole deep buffer, and the view opened at the rows you were reading, not at the end
    const buf = term.buffer.active;
    let lead = 0;
    while (lead < term.rows) { const l = buf.getLine(buf.baseY + lead); if (l && l.translateToString(true).trim()) break; lead++; }
    $('#sel-text').textContent = bufferText();
    const sv = $('#selview');
    // the stitched view and the selection view share row heights, so the same scroll is the same rows
    sv.scrollTop = stitchOn() ? Math.max(0, $('#stitch').scrollTop - (stitchAnchor + lead) * monoSize() * MONO_LH + 0) : (deepBuffer && inAltScreen()) ? Math.max(0, (viewOffset - lead) * monoSize() * MONO_LH) : sv.scrollHeight;
  }
  showSticky();
  setTimeout(fitTerm, 80);
}
$('#sel-done').addEventListener('click', () => setSelecting(false));

function attachPinch(host) { attachTouchScroll(host, true); }
function attachTouchScroll(hostEl, pinchOnly = false) {
  const host = hostEl || $('#term');
  let lastY = 0, startY = 0, acc = 0, vel = 0, lastT = 0, dragging = false, glide = null;
  const rowHeight = () => {
    const rows = host.querySelector('.xterm-rows');
    if (rows && rows.children.length) return rows.getBoundingClientRect().height / rows.children.length;
    return 16;
  };
  // Pixels to wheel notches, compressively. Each frame's movement is mapped through log2, so a slow
  // deliberate drag is close to linear and precise, while a fast flick — big per-frame deltas — is
  // damped hard instead of launching the view. PX_PER_NOTCH sets the overall pace; the fractional
  // remainder is carried so slow drags still move.
  const PX_PER_NOTCH = 90;
  let pending = 0;
  const scrollBy = px => {
    if (!term || !px) return;
    // Inside the buffer this is pure local motion: move the window, no network, no rounding to
    // notches. Only what does not fit inside it is handed to the session as wheel events.
    if (deepBuffer && inAltScreen() && maxOffset() > 0) {
      const rp = rowPx();
      if (rp > 0) {
        const want = px / rp;
        const before = viewOffset;
        viewOffset = Math.min(maxOffset(), Math.max(0, viewOffset + want));
        applyOffset();
        px = (want - (viewOffset - before)) * rp;      // the part that ran off an edge
        touchStats.local = (touchStats.local || 0) + 1;
        noteTouch();
        maybePrefetch(want < 0);                       // top up while moving, not at the wall
        if (Math.abs(px) < rp / 2) return;
      }
    }
    const sign = px < 0 ? -1 : 1;
    // log2 alone compresses the top end too hard; the small linear term gives fast movement its
    // speed back without touching the precision of a slow drag.
    const a = Math.abs(px) / PX_PER_NOTCH;
    pending += sign * (Math.log2(1 + a) + a * 0.18);
    const whole = Math.trunc(pending);
    if (!whole) return;
    pending -= whole;
    queueNotches(whole);                               // paced out, one per frame
  };
  const CAP = { capture: true, passive: false };
  // Two fingers set the width. Apart: bigger text, fewer columns. Together: more columns. The
  // terminal is only SCALED while the fingers are down — the badge shows the count — and the real
  // resize happens once, on release: a resize per move would be a SIGWINCH storm, and a storm of
  // repaints is the interleaved-character screen.
  let pinch = null;
  const dist = ts => Math.hypot(ts[0].clientX - ts[1].clientX, ts[0].clientY - ts[1].clientY);
  host.addEventListener('touchstart', e => {
    touchStats.start++; noteTouch();
    if (!selecting && e.touches.length === 2 && term && !phoneMirrors()) {
      if (!$('#colsbadge')) return;
      if (glide) { cancelAnimationFrame(glide); glide = null; }
      dragging = false;
      pinch = { d0: dist(e.touches), cols0: term.cols, cols: term.cols };
      pinchScale = 1;
      showBadge(term.cols + ' columns');
      return;
    }
    if (pinchOnly || selecting || e.touches.length !== 1) return;
    if (glide) { cancelAnimationFrame(glide); glide = null; }
    notchQueue = 0; clearTimeout(stepTimer); stepTimer = null; prefetching = false; prefetchMark = null;   // start clean
    dragging = true; vel = 0; acc = 0;
    startY = lastY = e.touches[0].clientY;
    lastT = performance.now();
  }, { capture: true, passive: true });
  host.addEventListener('touchmove', e => {
    touchStats.move++;
    if (e.cancelable) touchStats.cancelable++;
    if (pinch) {
      if (e.touches.length === 2) {
        const s = dist(e.touches) / pinch.d0;
        // no more columns than the smallest face (6 px) can fit: the badge must promise only what can be delivered
        if (!charRatio) measureCharRatio();
        const maxCols = Math.max(40, Math.floor(host.clientWidth / (6 * charRatio)));
        pinch.cols = Math.max(40, Math.min(160, maxCols, Math.round(pinch.cols0 / s)));
        pinchScale = pinch.cols0 / pinch.cols;
        if (host.id === 'textview') host.style.transform = pinchScale === 1 ? '' : 'scale(' + pinchScale + ')'; else applyOffset();
        showBadge(pinch.cols + ' columns');
      }
      if (e.cancelable) e.preventDefault();
      return;
    }
    if (pinchOnly || selecting || !dragging || e.touches.length !== 1) return;
    const y = e.touches[0].clientY;
    if (Math.abs(y - startY) < 6) return;              // still a tap: leave it to xterm
    // a selection is up: the drag belongs to Android's selection handles, not to scrolling
    if (hasSelection()) return;
    const now = performance.now(), dt = Math.max(1, now - lastT);
    const dy = lastY - y;                              // finger up = forward through the log
    // clamp: a fast flick should coast a few screens, not fly to the far end of the scrollback
    vel = Math.max(-3, Math.min(3, dy / dt));
    lastY = y; lastT = now;
    scrollBy(dy);
    touchStats.scrolled++; noteTouch();
    // Only claim the gesture once we are actually scrolling. A stationary long press never reaches
    // here, so Android is free to start a selection with it.
    if (e.cancelable) e.preventDefault();
  }, CAP);
  const release = () => {
    if (pinch) {
      const p = pinch; pinch = null;
      pinchScale = 1; if (host.id === 'textview') host.style.transform = ''; else applyOffset();
      hideBadge();
      if (p.cols !== p.cols0) setCols(p.cols);
      return;
    }
    if (!dragging) return;
    dragging = false;
    if (Math.abs(vel) < 0.08) return;
    let speed = vel * 11;                              // px per frame, decaying
    const step = () => {
      speed *= 0.90;                                   // a flick coasts a little, not for pages
      scrollBy(speed);
      glide = Math.abs(speed) > 1.5 ? requestAnimationFrame(step) : null;
    };
    glide = requestAnimationFrame(step);
  };
  host.addEventListener('touchend', release, { capture: true });
  host.addEventListener('touchcancel', release, { capture: true });
}

let choiceTimer = null;
// the buffer is only settled once xterm has finished writing, so coalesce the repaints
function scheduleChoices() {
  clearTimeout(choiceTimer);
  choiceTimer = setTimeout(() => { renderChoices(); if (atBottom()) applyOffset(); }, 120);
}

function sendAttach(id) { sendRaw({ t: 'attach', id }); }

// The torn, mixed-width screens come from one thing: the pty being some size other than the one
// this view is drawing at. The desktop's mirroring is meant to prevent that, but a renderer reload,
// a startup nudge or a detach-and-reattach can still leave the pty elsewhere. So every tick, if the
// session's real size (from node-pty itself) is not ours while we are the one looking, put it back.
let lastReassertAt = 0;
function reassertSizeIfDrifted() {
  if (!term || !termId || current() !== 's-term') return;
  if (Date.now() < nudgeUntil) return;                 // a nudge is deliberately mid-flight
  if (phoneMirrors()) return;                          // not ours to assert
  const t = tileById(termId);
  if (!t || !t.ptySize || !(intended.cols > 1 && intended.rows > 1)) return;
  if (t.ptySize.cols === intended.cols && t.ptySize.rows === intended.rows) return;
  if (Date.now() - lastReassertAt < 2500) return;
  lastReassertAt = Date.now();
  touchStats.reasserts = (touchStats.reasserts || 0) + 1; noteTouch();
  lastSent = { cols: 0, rows: 0, at: 0 };
  sendSize(intended.cols, intended.rows);
}

// Make the SESSION repaint, not just xterm. The replay we attach to is whatever the application last
// emitted — at whatever width it was emitted for — so a stale or half-drawn screen stays stale no
// matter how often xterm redraws its own buffer. A resize makes a full-screen application redraw
// everything, so nudge the pty a column and put it back. The two steps are spaced past main's 150 ms
// resize coalescing, or they would merge into a single no-op at the size it already has.
function forceRedraw() {
  if (!term || !termId || phoneMirrors()) return;
  try { term.refresh(0, term.rows - 1); } catch { /* disposed */ }
  if (!(intended.cols > 2 && intended.rows > 1)) sizeTerminal();
  const { cols, rows } = intended;
  if (!(cols > 2 && rows > 1)) return;
  const id = termId;
  nudgeUntil = Date.now() + 1200;                      // the drift check must not join in mid-nudge
  lastSent = { cols: 0, rows: 0, at: 0 };
  sendSize(cols - 1, rows);
  setTimeout(() => { if (termId === id) { lastSent = { cols: 0, rows: 0, at: 0 }; sendSize(cols, rows); } }, 350);
}
function openTerm(id) {
  termId = id;
  const t = tileById(id);
  $('#term-title').textContent = t ? (t.title || 'session') : 'session';
  show('s-term');
  ensureTerm();
  term.reset();
  resetStitch();
  screenEffort = ''; sessionModel = ''; sessionMode = '';
  sendAttach(id);
  // Do not focus anything: on a phone, focus means the keyboard swallows half the screen before
  // you have even read the session. Tap the terminal (or the keyboard key) when you want to type.
  const draft = loadDraft(id);
  $('#compose-input').value = draft;
  setInputMode(inputMode, !!draft);        // the keyboard opens only when a draft is waiting
  tvLastAt = null;
  // A shell has nothing for the text view to show, so it always opens in the terminal and the
  // text/terminal switch is put away; the reader's own choice (text unless they last chose the
  // terminal) is kept for the next Claude session rather than overwritten by the shell's.
  const shell = isShellTile(id);
  $('#viewseg').hidden = shell;
  paintStitched();                         // the stitched layer is for a Claude session's screen; a shell shows its terminal
  if (shell) setViewMode('terminal', false);
  else setViewMode(savedViewMode());
  if (viewMode === 'terminal') requestAnimationFrame(() => { fitTerm(); setTimeout(forceRedraw, 250); });
}
function detachTerm() {
  if (termId) { saveDraft(); sendRaw({ t: 'detach' }); termId = null; }
  intended = { cols: 0, rows: 0 }; lastSent = { cols: 0, rows: 0, at: 0 };
  if (selecting) setSelecting(false);
  const bar = $('#compose-bar');
  if (bar && !bar.hidden) { bar.hidden = true; $('#k-kbd').classList.remove('on'); }
  choiceSig = ''; $('#choicerow').hidden = true; $('#choicerow').textContent = '';
}
function fitTerm() { sizeTerminal(); }
window.addEventListener('resize', () => { if (colsSetting()) applyFonts(); if (current() === 's-term') setTimeout(fitTerm, 60); });
if (window.visualViewport) window.visualViewport.addEventListener('resize', () => { if (current() === 's-term') setTimeout(fitTerm, 60); });

// 'clear' empties the session's input line with a burst of backspaces: the one way to do it that
// cannot exit the session by accident (^C on an empty line is the first half of 'press again to
// exit'). Three hundred covers anything typed, wrapped lines included; extra ones do nothing.
const KEYS = { esc: '\x1b', tab: '\t', stab: '\x1b[Z', up: '\x1b[A', down: '\x1b[B', left: '\x1b[D', right: '\x1b[C', cc: '\x03', clear: '\x7f'.repeat(300) };
// The keys act on pointerdown, and the default is prevented there: a button that took the focus
// would take it from the terminal's textarea, and the phone's keyboard goes with the focus. So
// no button here ever has it, and the keyboard stays up through ctrl, arrows and the rest.
$('#keyrow').addEventListener('pointerdown', ev => {
  const b = ev.target.closest('button'); if (!b) return;
  ev.preventDefault();
  pressKey(b);
});
$('#keyrow').addEventListener('click', ev => {
  const b = ev.target.closest('button'); if (!b) return;
  ev.preventDefault();
  if (b.dataset.pressed && Date.now() - +b.dataset.pressed < 700) return;   // the pointerdown already did it
  pressKey(b);
});
function pressKey(b) {
  b.dataset.pressed = String(Date.now());
  const k = b.dataset.key;
  if (k === 'esc') { pressEsc(b); return; }
  if (k === 'file') { keyFelt(b); openFileSheet(); return; }   // the keyboard state is left as it is
  if (k === 'end') { keyFelt(b); if (viewMode === 'text') loadTranscript(true); else scrollTerminalToEnd(); return; }   // back to the newest
  if (k === 'clear' && (inputMode === 'line' || viewMode === 'text')) {   // the box is the input: clear the box
    const box = $('#compose-input'); box.value = ''; growCompose(); saveDraft(); keyFelt(b); return;   // the keyboard stays as it was
  }
  if (k === 'ctrl') { ctrlArmed = !ctrlArmed; b.classList.toggle('on', ctrlArmed); if (inputMode === 'direct' && term) term.focus(); return; }
  // the mode key is back-tab with a name, and a guard against a double tap skipping a mode
  if (k === 'stab') {
    if (Date.now() - lastModeAt < 500) { keyFelt(b); return; }
    lastModeAt = Date.now();
  }
  if (k === 'kbd') { toggleCompose(); return; }
  if (KEYS[k] && termId) sendRaw({ t: 'input', id: termId, d: KEYS[k] });
  keyFelt(b);
  // clr and mode leave the keyboard exactly as it was: open stays open, closed stays closed
  if (inputMode === 'direct' && term && k !== 'clear' && k !== 'stab') term.focus();
}
// What kind of thing: the camera, the photo gallery, or any file — the picker Android opens depends
// on what the input asks for, so the input is set up for the pick, then clicked.
function openFileSheet() {
  const sheet = $('#sheet'), box = $('#sheet-actions');
  $('#sheet-title').textContent = 'send to this session';
  box.textContent = '';
  showSheet();
  const opt = (label, accept, capture) => {
    const b = el('button', '', label);
    b.addEventListener('click', () => {
      // the picker must open inside the tap itself (a browser opens a file chooser only on a user
      // gesture), so the input is clicked first and the sheet closed after
      const input = $('#file-pick');
      if (accept) input.setAttribute('accept', accept); else input.removeAttribute('accept');
      if (capture) input.setAttribute('capture', capture); else input.removeAttribute('capture');
      input.click();
      hideSheet();
    });
    box.appendChild(b);
  };
  opt('Any file', null, null);
  opt('Photos and videos', 'image/*,video/*', null);
  opt('Take a photo', 'image/*', 'environment');
}
// A file for the session, the way a path dropped on the desk's terminal works: the phone uploads it
// over the same tailnet link, the deck saves it under its data folder, and the path goes into the
// input — the box in line mode, typed into the session in keys mode. Claude Code reads a path in a
// message, images included. Several files: several paths, space-separated, quoted when needed.
$('#file-pick').addEventListener('change', async () => {
  const input = $('#file-pick');
  const files = [...(input.files || [])];
  input.value = '';
  if (!files.length || !termId) return;
  const paths = [];
  for (const f of files) {
    if (f.size > 2 * 1024 * 1024) toast('sending ' + f.name + '\u2026');   // only a big one is worth a word
    try {
      const r = await fetch('/upload', { method: 'POST', credentials: 'same-origin', headers: { 'x-file-name': encodeURIComponent(f.name), 'content-type': 'application/octet-stream' }, body: f });
      if (!r.ok) throw new Error('upload failed (' + r.status + ')');
      const j = await r.json();
      paths.push(/\s/.test(j.path) ? '"' + j.path + '"' : j.path);
    } catch (e) { toast(String(e.message || e)); return; }
  }
  const text = paths.join(' ');
  if (inputMode === 'line') {
    const box = $('#compose-input');
    box.value = (box.value ? box.value.replace(/\s*$/, ' ') : '') + text + ' ';
    growCompose(); saveDraft();
  } else {
    sendRaw({ t: 'input', id: termId, d: text + ' ' });
  }
  // the path in the input is the confirmation
});

// ---------------------------------------------------------------- the footer's sheet
// A long-press on the session's footer bar (any view, any mode) opens model, effort and mode.
// Model and effort are slash commands typed into the session's own line — after that line is
// emptied, so nothing half-typed gets glued on. A mode has no command: it is the Shift+Tab cycle
// (normal → auto-accept → plan → normal), so the pick sends as many steps as the cycle needs from
// the mode the footer shows, reads the footer again, and steps once more if it is not there yet.
const MODELS = [['fable', 'Fable 5.1'], ['opus', 'Opus 5'], ['sonnet', 'Sonnet 5'], ['haiku', 'Haiku 4.5']];
const EFFORTS = ['low', 'medium', 'high', 'xhigh', 'max'];
const MODE_CYCLE = ['normal', 'auto', 'plan'];
function sessionCommand(cmd) {
  if (!termId) return;
  const id = termId;
  sendRaw({ t: 'input', id, d: KEYS.clear + '\x1b[200~' + cmd + '\x1b[201~' });   // bracketed: no command list pops up mid-word
  setTimeout(() => { if (termId === id) sendRaw({ t: 'input', id, d: CR }); }, 150);
  sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [id] });
}
let modeJob = null;
const effortChosen = new Map();   // termId -> { level, seen }: a level chosen here, until the tile reports a new one
function setSessionMode(target) {
  if (!termId || !MODE_CYCLE.includes(target)) return;
  if (modeJob) clearTimeout(modeJob);
  let rounds = 0;
  const step = () => {
    const cur = MODE_CYCLE.indexOf(sessionMode);
    if (sessionMode === target) return;                 // the footer shows it
    if (++rounds > 4) { toast('could not reach ' + target + ' mode (the footer says ' + (sessionMode || '?') + ')'); return; }
    const n = cur < 0 ? 1 : (MODE_CYCLE.indexOf(target) - cur + MODE_CYCLE.length) % MODE_CYCLE.length || MODE_CYCLE.length;
    for (let i = 0; i < n; i++) setTimeout(() => { if (termId) sendRaw({ t: 'input', id: termId, d: KEYS.stab }); }, i * 160);
    modeJob = setTimeout(step, n * 160 + 700);      // the footer has repainted by then
  };
  step();
}
function openFooterSheet() {
  if (!termId) return;
  const t = tileById(termId); if (!t || t.kind !== 'claude') return;
  const sheet = $('#sheet'), box = $('#sheet-actions');
  $('#sheet-title').textContent = 'this session';
  box.textContent = '';
  showSheet();
  const close = () => hideSheet();
  const pick = (label, items, isNow, run) => {
    box.appendChild(el('div', 'lbl', label));
    const row = el('div', 'pick');
    for (const [v, name] of items) { const b = el('button', isNow(v, name) ? 'now' : '', name); b.addEventListener('click', () => { close(); run(v); }); row.appendChild(b); }
    box.appendChild(row);
  };
  pick('model' + (sessionModel ? ' \u00b7 ' + sessionModel : ''), MODELS, (v, name) => sessionModel && name.toLowerCase().startsWith(sessionModel.toLowerCase().split(' ')[0]), v => { sessionCommand('/model ' + v); });
  // The effort in use comes with the tile (read off the transcript: the level the LAST answer ran at),
  // the footer does not say it. So a level just chosen here is remembered until the next answer
  // reports one, and a new session that has not answered yet says so.
  const known = screenEffort || String(t.effort || '').toLowerCase();   // the screen first (it is current), the transcript's last answer otherwise
  const chosen = effortChosen.get(termId);
  if (chosen && chosen.seen !== known) { effortChosen.delete(termId); }
  const eff = (effortChosen.get(termId) || {}).level || known;
  const effNote = eff ? ' · ' + eff + (effortChosen.has(termId) ? ' (set)' : '') : ' · not known until the first answer';
  // ultracode is not a level but a switch (/ultracode toggles it); it sits in the same row, marked when on
  pick('effort' + effNote + (t.ultracode ? ' · ultracode' : ''), EFFORTS.map(e => [e, e]).concat([['ultracode', 'ultracode']]), v => v === eff || (v === 'ultracode' && !!t.ultracode), v => { if (v === 'ultracode') sessionCommand('/ultracode'); else { sessionCommand('/effort ' + v); effortChosen.set(termId, { level: v, seen: known }); } });
  pick('mode' + (sessionMode ? ' \u00b7 ' + sessionMode : ''), [['normal', 'normal'], ['auto', 'auto-accept'], ['plan', 'plan']], v => v === sessionMode, v => setSessionMode(v));
  if (sessionMode && !MODE_CYCLE.includes(sessionMode)) { const b = el('button', '', 'step the mode (now: ' + sessionMode + ')'); b.addEventListener('click', () => { close(); sendRaw({ t: 'input', id: termId, d: KEYS.stab }); }); box.appendChild(b); }
}
// the long-press itself, on each footer bar
function attachFooterPress(el_) {
  if (!el_) return;
  let timer = null, y0 = 0;
  el_.addEventListener('touchstart', e => { if (e.touches.length !== 1) return; y0 = e.touches[0].clientY; clearTimeout(timer); timer = setTimeout(() => { timer = null; openFooterSheet(); }, 480); }, { passive: true });
  el_.addEventListener('touchmove', e => { if (timer && Math.abs(e.touches[0].clientY - y0) > 8) { clearTimeout(timer); timer = null; } }, { passive: true });
  el_.addEventListener('touchend', () => { clearTimeout(timer); timer = null; }, { passive: true });
  el_.addEventListener('contextmenu', e => { e.preventDefault(); openFooterSheet(); });   // a mouse, or a phone that turns the press into one
}
attachFooterPress($('#term-tail'));
$('#term-tail').addEventListener('click', ev => { if (ev.target.closest('.leave') && termId) { transcriptOpen[termId] = false; sendRaw({ t: 'input', id: termId, d: KEYS.esc }); sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [termId] }); } });
attachFooterPress($('#tv-footer'));

// One key, by context. In a Claude session it is Esc under every name: 'cancel' while a menu or
// dialog is up, 'stop claude' while a turn runs, 'esc' otherwise. ^C is not offered for Claude —
// exiting is the menu's job — only for a shell, where it is the interrupt. Stopping a turn is
// guarded against a double tap: a second press within a moment and a half is ignored.
let lastStopAt = 0, lastModeAt = 0, sessionMode = '', sessionModel = '', screenEffort = '';
const screenModelOf = {};   // session id -> the model its footer or /model reply last said
function escContext() {
  const t = termId ? tileById(termId) : null;
  if (!$('#choicerow').hidden) return 'cancel';
  if (t && t.kind === 'claude' && t.status === 'busy') return 'stop';
  return 'esc';
}
function pressEsc(b) {
  const ctx = escContext();
  if (ctx === 'stop') {
    if (Date.now() - lastStopAt < 1500) { keyFelt(b); return; }
    lastStopAt = Date.now();
  }
  if (termId) sendRaw({ t: 'input', id: termId, d: KEYS.esc });
  keyFelt(b);
  if (inputMode === 'direct' && term) term.focus();
}
function paintKeyRow() {
  const t = termId ? tileById(termId) : null;
  const cc = $('#k-cc'); if (cc) cc.hidden = !(t && t.kind === 'shell');
  const md = $('#k-mode'); if (md) md.hidden = true;   // (the mode is set from the footer's sheet now)
  const esc = $('#k-esc');
  if (esc) { const ctx = escContext(); esc.textContent = ctx === 'cancel' ? 'cancel' : ctx === 'stop' ? 'stop claude' : 'esc'; }
}
// a key that was pressed says so: a short buzz where the phone allows one, and a flash
function keyFelt(b) {
  try { if (navigator.vibrate) navigator.vibrate(12); } catch { /* not allowed */ }
  b.classList.add('hit');
  setTimeout(() => b.classList.remove('hit'), 140);
}
// Line mode. Typing straight into a terminal on a phone means every autocorrect and swipe guess
// lands in the session as it happens; here the line is yours until you send it.
// The two input modes are exclusive, which is the point of them. In LINE mode the session accepts
// nothing the soft keyboard produces — xterm's stdin is off and its textarea never takes focus — so
// autocorrect and swipe-typing cannot reach it; everything goes through the box, the key row and the
// choice row. In DIRECT mode you type straight into the session, keystroke by keystroke, and the IME
// is yours to fight. Nothing is lost in line mode: esc/tab/ctrl/arrows are the key row, and a select
// prompt is the choice row.
let toggling = false;
function setInputMode(mode, focusBox) {
  inputMode = mode;
  try { localStorage.setItem('deck.inputmode', mode); } catch { /* private mode */ }
  const line = mode === 'line';
  $('#compose-bar').hidden = !line;
  const k = $('#k-kbd');
  k.classList.toggle('on', line);
  k.textContent = line ? 'line' : 'keys';
  if (term) {
    term.options.disableStdin = line;                 // the session stops listening to the keyboard
    const ta = $('#term').querySelector('textarea');
    if (ta) ta.readOnly = line;                       // and Android has nothing to autocorrect into
    if (line && document.activeElement === ta) ta.blur();
  }
  // The keyboard follows the focus: hand it over, never drop it. Line: the box takes it. Keys: the
  // terminal takes it if the box had it (a switch pressed from the box keeps typing going).
  if (stitchOn()) scheduleStitch();                    // the tail and the footer strip follow the mode
  paintSticky();
  const hadBox = document.activeElement === $('#compose-input');
  const hadTerm = !!term && document.activeElement === term.textarea;
  // hand the keyboard over only if one side had it (or a draft asks for it); never open one that was closed
  if (line && (hadTerm || (focusBox && !toggling))) $('#compose-input').focus();
  if (!line && hadBox) { $('#compose-input').blur(); if (term) term.focus(); }
  setTimeout(fitTerm, 80);
}
function toggleCompose(on) { toggling = true; try { setInputMode(on === undefined ? (inputMode === 'line' ? 'direct' : 'line') : (on ? 'line' : 'direct'), on === undefined); } finally { toggling = false; } }
const CR = String.fromCharCode(13);
// What actually goes down the wire. Bracketed paste keeps a multi-line message from being submitted
// line by line — the session takes the whole thing as one message, the way a paste behaves.
function sendToSession(text) {
  if (!termId) return;
  const id = termId;
  // A shell takes the line as typed: no bracketed paste (Windows PowerShell's line editor does not
  // switch it on, so the marks would arrive as text) and no Enter retry (that looks for Claude
  // Code's prompt marker, which a shell never draws).
  if (isShellTile(id)) { sendRaw({ t: 'input', id, d: text + CR }); sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [id] }); return; }
  const body = (text.includes(String.fromCharCode(10)) || text.startsWith('/'))
    ? ESC + '[200~' + text + ESC + '[201~'                 // bracketed: one message, and no command list for a slash
    : text;
  sendRaw({ t: 'input', id, d: body + CR });
  sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [id] });
  // The Enter can be swallowed — the session still finishing an answer, a prompt just closing — and the
  // text then sits in its input line, where the next send is glued onto it. So look at the prompt row a
  // moment later; if the message is still there, press Enter again (twice at most).
  const head = text.trim().slice(0, 24);
  const stillTyped = () => {
    if (!term || termId !== id || head.length < 3) return false;
    const buf = term.buffer.active, n = term.rows;
    for (let i = 0; i < n; i++) {
      const l = buf.getLine(buf.baseY + i); if (!l) continue;
      const r = l.translateToString(true);
      if (/^[❯>]\s/.test(r) && r.includes(head)) return true;
    }
    return false;
  };
  setTimeout(() => { if (stillTyped()) { sendRaw({ t: 'input', id, d: CR }); setTimeout(() => { if (stillTyped()) sendRaw({ t: 'input', id, d: CR }); }, 2500); } }, 1500);
}
function sendComposed() {
  const box = $('#compose-input');
  const text = box.value;
  if (!termId || (!text && viewMode === 'text')) return;
  if (viewMode === 'text') showSentOptimistically(text);
  sendToSession(text);
  if (!isShellTile()) outboxAdd(termId, text);    // the outbox confirms against a transcript; a shell has none
  box.value = '';
  growCompose();
  saveDraft();
  box.focus();
}

// ---------------------------------------------------------------- the outbox
// What the desk's terminal gives you for free: a message you send is not lost. Here a send is
// only bytes into a pty over a link that can drop, so every send is KEPT until the session's
// transcript shows it arrived. A checker looks every few seconds and after a reconnect: still
// sitting in the prompt row — press Enter again; gone without a trace and the session idle —
// send it again (three times at most); the session busy — wait, it will take it when free.
let outbox = (() => { try { return JSON.parse(localStorage.getItem('deck.outbox') || '[]'); } catch { return []; } })();
function outboxSave() { try { localStorage.setItem('deck.outbox', JSON.stringify(outbox.slice(-40))); } catch { /* private mode */ } }
function outboxAdd(id, text) {
  if (!text.trim()) return;
  outbox.push({ id, text, sentAt: Date.now(), tries: 1, checkedAt: 0 });
  outboxSave();
}
let outboxBusy = false;
async function pumpOutbox() {
  if (outboxBusy || !state.connected || !outbox.length) return;
  outboxBusy = true;
  try {
    const now = Date.now();
    for (const item of outbox.slice()) {
      if (now - item.sentAt < 8000 || now - item.checkedAt < 5000) continue;   // give a fresh send its own moment
      item.checkedAt = now;
      const t = tileById(item.id);
      if (!t || t.exited || t.kind === 'shell') { outbox = outbox.filter(x => x !== item); continue; }   // the session is gone (or is a shell, which has no transcript to confirm against): nothing to deliver to
      if (now - item.sentAt > 6 * 3600 * 1000) { outbox = outbox.filter(x => x !== item); continue; }   // stale beyond use
      let seen = false;
      try {
        const p = await call('transcript', item.id, { limit: 12 });
        const head = item.text.trim().slice(0, 40);
        seen = p.entries.some(e => e.role === 'user' && ((e.at && Date.parse(e.at) >= item.sentAt - 3000) || (head && e.text.trim().startsWith(head))));
      } catch { continue; }                                         // no reading: try later
      if (seen) { outbox = outbox.filter(x => x !== item); continue; }
      if (t.status === 'busy') continue;                              // it will take the queued line when free
      if (item.tries >= 4) continue;                                  // said enough: the mark stays 'not seen'
      const head = item.text.trim().slice(0, 24);
      const onPrompt = item.id === termId && term && (() => {
        const buf = term.buffer.active, n = term.rows;
        for (let i = 0; i < n; i++) { const l = buf.getLine(buf.baseY + i); if (l && /^[\u276f>]\s/.test(l.translateToString(true)) && l.translateToString(true).includes(head)) return true; }
        return false;
      })();
      item.tries++;
      if (onPrompt) sendRaw({ t: 'input', id: item.id, d: CR });     // typed but not sent: Enter
      else sendRaw({ t: 'input', id: item.id, d: ESC + '[200~' + item.text + ESC + '[201~' + CR });   // gone: again, whole
      sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [item.id] });
    }
    outboxSave();
  } finally { outboxBusy = false; }
}
setInterval(pumpOutbox, 5000);

// Put the message on screen immediately, then wait for the session's own transcript to carry it. The
// terminal is where it really lands, and the transcript is the only honest confirmation that it did —
// so it stays marked "sending" until it appears there, and says so if it never does.
// Claude Code does not store what you sent verbatim — a long or pasted message becomes a
// placeholder in its transcript — so matching the text exactly left the mark stuck on 'sending'.
// Confirm by TIME instead: this phone is the one typing, so any user entry the transcript records
// after the send is this message. A prefix match is accepted too, for the ordinary case.
function showSentOptimistically(text) {
  const list = $('#tv-list'), box = $('#textview');
  const sentAt = Date.now();
  const node = tvEntry({ role: 'user', at: new Date(sentAt).toISOString(), text });
  node.classList.add('pending');
  list.appendChild(node);
  box.scrollTop = box.scrollHeight;
  const head = text.trim().slice(0, 40);
  let tries = 0;
  const look = () => {
    if (!termId || !node.isConnected) return;
    if (++tries > 45) {
      node.classList.remove('pending');
      node.classList.add('unconfirmed');
      return;
    }
    call('transcript', termId, { limit: 8 })
      .then(p => {
        const seen = p.entries.some(e => e.role === 'user' && (
          (e.at && Date.parse(e.at) >= sentAt - 3000) ||
          (head && e.text.trim().startsWith(head))
        ));
        if (seen) { if (viewMode === 'text') loadTranscript(true); return; }
        setTimeout(look, 1000);
      })
      .catch(() => setTimeout(look, 1500));
  };
  setTimeout(look, 800);
}

// An unsent line is kept per session, so leaving a terminal and coming back does not lose it —
// and a waiting draft is the only reason the keyboard opens by itself.
const draftKey = id => 'deck.draft.' + id;
function loadDraft(id) { try { return localStorage.getItem(draftKey(id)) || ''; } catch { return ''; } }
function saveDraft() {
  if (!termId) return;
  const v = $('#compose-input').value;
  try { v ? localStorage.setItem(draftKey(termId), v) : localStorage.removeItem(draftKey(termId)); } catch { /* private mode */ }
}
$('#compose-send').addEventListener('click', sendComposed);
$('#compose-keys').addEventListener('pointerdown', ev => { if (ev.target.closest('button')) ev.preventDefault(); });   // the box keeps the keyboard
$('#compose-keys').addEventListener('click', ev => {
  const b = ev.target.closest('button'); if (!b || !termId) return;
  sendRaw({ t: 'input', id: termId, d: b.dataset.send === 'esc' ? ESC : String.fromCharCode(3) });
});
$('#compose-input').addEventListener('input', saveDraft);
// Enter puts a new line in the box; only the button sends. (Ctrl+Enter sends too, for a keyboard.)
$('#compose-input').addEventListener('keydown', e => { if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); sendComposed(); } });
function growCompose() {
  const box = $('#compose-input');
  box.style.height = 'auto';
  box.style.height = Math.min(160, Math.max(40, box.scrollHeight)) + 'px';
}
$('#compose-input').addEventListener('input', growCompose);


$('#term-menu').addEventListener('click', () => { if (termId) openSheet(termId); });

// ---------------------------------------------------------------- the session's own options
// Claude Code draws a select list as a marked line plus siblings indented to the same column:
//
//      No, exit
//    > Yes, I trust this folder          (> is U+276F)
//
// Twelve pixels tall and a thumb's width apart, which is nearly untappable. Read the list out of the
// live buffer and mirror it as real buttons. Selection is sent as arrows plus Enter rather than a
// digit: it works for numbered and unnumbered lists alike, and it is what the list itself expects.
// the marker Claude Code draws before its prompt and its menu's chosen line: \u276f in some builds,
// a plain > in others (this one, 2026-09-03). A character class, used inside the regexes below.
const MARKER = '[\u276f>]';
function readChoices() {
  if (!term) return null;
  const buf = term.buffer.active;
  const lines = [];
  const first = Math.max(0, buf.length - term.rows);
  for (let i = first; i < buf.length; i++) {
    const l = buf.getLine(i);
    lines.push(l ? l.translateToString(true) : '');
  }
  let at = -1, indent = 0;
  for (let i = lines.length - 1; i >= 0; i--) {
    const m = new RegExp('^(\\s*)' + MARKER + '\\s+(\\S.*)$').exec(lines[i]);
    if (m) { at = i; indent = m[1].length; break; }
  }
  if (at < 0) return null;
  // siblings sit two columns further in, where the marker would be
  const sibling = new RegExp('^\\s{' + (indent + 2) + '}(\\S.*)$');
  const marked = new RegExp('^\\s{' + indent + '}' + MARKER + '\\s+(\\S.*)$');
  const take = (i) => {
    const mk = marked.exec(lines[i]);
    if (mk) return { text: mk[1], sel: true };
    const sb = sibling.exec(lines[i]);
    return sb ? { text: sb[1], sel: false } : null;
  };
  const out = [];
  for (let i = at; i >= 0; i--) { const v = take(i); if (!v) break; out.unshift(v); }
  for (let i = at + 1; i < lines.length; i++) { const v = take(i); if (!v) break; out.push(v); }
  if (out.length < 2 || out.length > 9) return null;
  // The shape alone is not proof: Claude Code draws YOUR OWN messages with the same marker, and a
  // wrapped one continues on a line indented like a sibling option — a perfect false menu. A real
  // prompt gives itself away one of two ways: the options are numbered, or a hint follows within
  // a few lines ('Enter to confirm', 'Esc to cancel', 'to select'). Demand one of them.
  const numbered = out.filter(o => /^\d{1,2}[.)]\s+/.test(o.text)).length >= Math.min(2, out.length);
  const lastIdx = at + out.filter((_, k) => k > out.findIndex(o => o.sel)).length;
  const trailer = lines.slice(lastIdx + 1, lastIdx + 5).join(' ');
  const hinted = /enter to (confirm|select|continue|submit)|esc to (cancel|go back|dismiss)|to select|to confirm|to choose/i.test(trailer);
  if (!numbered && !hinted) return null;
  const current = out.findIndex(o => o.sel);
  return {
    current,
    options: out.map(o => {
      const num = /^(\d{1,2})[.)]\s+(.*)$/.exec(o.text);
      return { label: (num ? num[2] : o.text).trim().slice(0, 80) };
    }),
  };
}

let choiceSig = '';
// 'How is Claude doing this session?' is Claude Code's occasional survey: one line of numbered
// answers (1: Bad … 4: Great, 0: Dismiss) rather than a marker menu, answered by pressing the
// number. It gets the same buttons; and a setting can answer it with Dismiss every time.
function feedbackSetting() { try { return localStorage.getItem('deck.feedback') || 'ask'; } catch { return 'ask'; } }
let feedbackSeen = '';
function readFeedback() {
  if (!term) return null;
  const buf = term.buffer.active, n = term.rows, rows = [];
  for (let i = 0; i < n; i++) { const l = buf.getLine(buf.baseY + i); rows.push(l ? l.translateToString(true) : ''); }
  let at = -1;
  for (let i = rows.length - 1; i >= 0; i--) if (/How is Claude doing/i.test(rows[i])) { at = i; break; }
  if (at < 0) return null;
  const options = [];
  for (let i = at; i < Math.min(rows.length, at + 4); i++) {
    const re = /(\d)\s*[:.)]\s*([A-Za-z][A-Za-z !'-]*?)(?=\s{2,}|\s*$|\s+\d\s*[:.)])/g;
    let m; while ((m = re.exec(rows[i]))) options.push({ label: m[2].trim(), key: m[1] });
    if (options.length) break;
  }
  if (options.length < 2) return null;
  return { kind: 'feedback', current: -1, options };
}
function renderChoices() {
  const row = $('#choicerow');
  const c = current() === 's-term' && term ? (readChoices() || readFeedback()) : null;
  const sig = c ? JSON.stringify(c) : '';
  if (sig === choiceSig) return;
  // the survey, answered for you when the setting says so — once per sighting
  if (c && c.kind === 'feedback' && feedbackSetting() === 'dismiss' && termId && feedbackSeen !== sig) {
    feedbackSeen = sig;
    const d = c.options.find(o => /dismiss/i.test(o.label));
    sendRaw({ t: 'input', id: termId, d: d ? d.key : KEYS.esc });
    choiceSig = sig; row.textContent = ''; row.hidden = true; return;
  }
  choiceSig = sig;
  row.textContent = '';
  row.hidden = !c;
  paintKeyRow();
  if (!c) return;
  c.options.forEach((o, i) => {
    const b = el('button', i === c.current ? 'sel' : '');
    b.appendChild(el('span', 'n', o.key != null ? String(o.key) : String(i + 1)));
    b.appendChild(el('span', 'l', o.label));
    b.addEventListener('click', () => pickChoice(i));
    row.appendChild(b);
  });
  setTimeout(fitTerm, 60);
}
// Walk the marker to the wanted line, then confirm. Each keystroke is spaced so a repaint cannot
// swallow it — the same lesson the trust prompt taught.
function pickChoice(index) {
  const c = readChoices() || readFeedback();
  if (!c || !termId) return;
  if (c.kind === 'feedback') {                         // the survey: the number is the answer
    const o = c.options[index]; if (!o) return;
    sendRaw({ t: 'input', id: termId, d: o.key });
    sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [termId] });
    return;
  }
  const steps = index - c.current;
  const key = steps > 0 ? '\x1b[B' : '\x1b[A';
  let n = Math.abs(steps), delay = 0;
  for (let i = 0; i < n; i++) {
    setTimeout(() => { if (termId) sendRaw({ t: 'input', id: termId, d: key }); }, delay);
    delay += 60;
  }
  setTimeout(() => {
    if (!termId) return;
    sendRaw({ t: 'input', id: termId, d: CR });
    sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [termId] });
  }, delay + 120);
}

// ---------------------------------------------------------------- the text view
// The session's own conversation, read off disk and paged. Scrolling it is native and instant, and
// it goes back to the first message rather than to the top of whatever the application last drew.
function savedViewMode() { try { return localStorage.getItem('deck.view') === 'terminal' ? 'terminal' : 'text'; } catch { return 'text'; } }
let viewMode = savedViewMode();
let tvFrom = null, tvTotal = 0, tvLoading = false, tvLastAt = null;
let tvDeferred = false;      // a refresh we held back because text was selected

function setViewMode(mode, remember = true) {
  // Measure where the reader is BEFORE anything moves: once viewMode has flipped and the text view
  // is hidden it has no height to measure, and the answer is always zero.
  const leavingText = viewMode === 'text' && mode === 'terminal';
  const back = leavingText ? rowsBehindInText() : 0;
  viewMode = mode === 'terminal' ? 'terminal' : 'text';
  if (remember) { try { localStorage.setItem('deck.view', viewMode); } catch { /* private mode */ } }   // not for a shell's forced terminal
  const term_ = viewMode === 'terminal';
  $('#seg-text').classList.toggle('on', !term_);
  $('#seg-term').classList.toggle('on', term_);
  document.body.classList.toggle('view-text', !term_);
  document.body.classList.toggle('view-terminal', term_);
  $('#keyrow').hidden = false;                        // both views: the text view hides the terminal-only keys by CSS
  paintKeyRow();
  { const fs = $('#footer-strip'); if (fs) fs.hidden = true; const tt = $('#term-tail'); if (tt && !term_) tt.hidden = true; const tf = $('#tv-footer'); if (tf && term_) tf.hidden = true; }
  if (term_ && stitchOn()) scheduleStitch();
  // In the text view the box is the whole point, so it is always there. In the terminal it is
  // line mode's box, and direct typing replaces it.
  $('#compose-bar').hidden = term_ ? inputMode !== 'line' : false;
  $('#compose-keys').hidden = true;                    // the key row carries esc now, in both views
  $('#choicerow').hidden = !$('#choicerow').children.length;   // answerable from either view
  requestAnimationFrame(fitTerm);
  if (term_) {
    // Re-read the screen on the way in: if the session is sitting on a question, its options must
    // appear straight away rather than waiting for the next thing it happens to print.
    choiceSig = '';
    requestAnimationFrame(() => {
      fitTerm();
      forceRedraw();
      scheduleChoices();
      if (back > 2) setTimeout(() => followBack(back), 500);
    });
  }
  else if (termId && !isShellTile()) loadTranscript(true);
}
$('#seg-text').addEventListener('click', () => setViewMode('text'));
$('#seg-term').addEventListener('click', () => setViewMode('terminal'));

// The terminal shows Claude's markdown with weight and colour; plain monospace loses half of what
// the eye uses to navigate. This is a deliberately small renderer — bold, inline code, fenced blocks,
// headings and bullets — escaped first, so a message can never inject markup.
// An absolute .html path in a message (C:\a\b.html, C:/a/b.html, /c/a/b.html, ~/a/b.html; spaces allowed, as in a path
// in backticks) becomes tappable: it opens in the page viewer. The same pattern as lib/viewer.js PATH_RE (the two must
// agree; test/viewer-unit.js checks), run on the escaped text, so a path never holds a quote or an angle bracket.
const TV_PATH_RE = /(^|[\s(\[`'"=;:,>])((?:[A-Za-z]:[\\/]|~[\\/]|\/(?!\/))[^\r\n`'"<>|*?]{0,300}?\.html?)(?=$|[\s)\]`'",;:!?&<]|\.(?:\s|$))/gm;
const tvPaths = t => t.replace(TV_PATH_RE, (m, pre, p) => `${pre}<span class="hpath" data-path="${p}">${p}</span>`);
function tvMarkup(text) {
  const esc = String(text)
    .split('&').join('&amp;')
    .split('<').join('&lt;')
    .split('>').join('&gt;');
  const NL = String.fromCharCode(10);
  // A fence is a line that is ``` (with an optional language word), nothing else: three backticks
  // inside a sentence ('I wrapped the table in a ``` fence') are just characters. Split on those
  // lines alone, so the parts alternate prose / fenced / prose as before.
  const parts = [];
  { let cur = []; for (const line of esc.split(NL)) { if (/^\s*```[a-zA-Z0-9_-]*\s*$/.test(line)) { parts.push(cur.join(NL)); cur = []; } else cur.push(line); } parts.push(cur.join(NL)); }
  let out = '';
  parts.forEach((part, i) => {
    if (i % 2 === 1) {                                    // inside a fence
      out += '<span class="block">' + tvPaths(part.replace(/^\n/, '').replace(/\n$/, '')) + '</span>';
      return;
    }
    let t = tvPaths(part);
    t = t.replace(/`([^`\n]+)`/g, '<span class="code">$1</span>');
    t = t.replace(/\*\*([^*\n]+)\*\*/g, '<b>$1</b>');
    t = t.split(NL).map(line => {
      const h = /^(#{1,6})\s+(.*)$/.exec(line);
      if (h) return '<span class="h">' + h[2] + '</span>';
      const b = /^(\s*)[-*]\s+(.*)$/.exec(line);
      if (b) return b[1] + '<span class="li"><span class="bullet">-</span> ' + b[2] + '</span>';
      const n = /^(\s*)(\d{1,2}[.)])\s+(.*)$/.exec(line);
      if (n) return n[1] + '<span class="li num"><span class="bullet">' + n[2] + '</span> ' + n[3] + '</span>';
      return line;
    }).join(NL);
    out += t;
  });
  return out;
}

// How far back the reader is, in terminal rows. The text view knows how many pixels of conversation
// sit below the fold; at its own line height that is a number of lines, and the terminal's lines
// carry the same words — near enough to land in the same part of the session rather than at the end.
function rowsBehindInText() {
  const box = $('#textview');
  if (!box || box.hidden || !box.clientHeight) return 0;
  const below = box.scrollHeight - box.scrollTop - box.clientHeight;
  if (below < 40) return 0;                                // reading the newest: stay at the end
  const lineH = parseFloat(getComputedStyle(box).fontSize) * 1.45 || 18;
  return Math.round(below / lineH);
}
// Walk the terminal back that far: the local buffer first (free), then ask the session for the rest.
function followBack(rows) {
  if (!term) return;
  let left = rows;
  if (deepBuffer && inAltScreen() && maxOffset() > 0) {
    const take = Math.min(left, viewOffset);
    viewOffset -= take;
    left -= take;
    applyOffset();
  }
  if (left > 2) scrollTerminal(-left);
}

// No long-press handler here on purpose. Android's own long press already selects text and offers
// Copy / Share / Select all — attaching a second meaning to the same gesture fired both at once,
// with the selection handles fighting a sheet on top of them. Selection is the platform's; the bulk
// copies live in the ••• menu, where they do not collide with anything.
function tvEntry(e) {
  const d = el('div', 'tv-msg ' + e.role);
  // no speaker line: the shapes say who speaks (a dim block with > for you, a bullet for the reply,
  // an elbow for tool activity); the time, and the effort on a reply, sit small at the right
  // no notes (not a messenger): the shapes say who speaks; the state of a message you sent shows
  // on the message itself (pending / unconfirmed / working)
  const body = el('div', 'tv-body');
  // the machinery's postings (a system reminder, a slash command's echo, an interruption note): a
  // dim folded line naming the kind, opening to the text — never shown as you
  if (e.role === 'meta') {
    d.classList.add('meta');
    body.textContent = e.label || 'context';
    d.appendChild(body);
    const full = el('div', 'tv-meta-body'); full.innerHTML = tvMarkup(e.text || ''); full.hidden = true;
    d.appendChild(full);
    body.addEventListener('click', () => { full.hidden = !full.hidden; d.classList.toggle('open', !full.hidden); });
    return d;
  }
  body.innerHTML = tvMarkup(e.text);
  d.appendChild(body);
  if (e.role === 'tool' || (e.role === 'assistant' && /^\[[A-Za-z]+\]$/.test(e.text.trim()))) d.classList.add('tool');
  // the 'Called …' line opens to show what each call was about, when the transcript page says
  if (e.role === 'tool' && Array.isArray(e.calls) && e.calls.length) {
    d.classList.add('has-calls');
    const list = el('div', 'tv-calls'); list.hidden = true;
    for (const c of e.calls) {
      const row = el('div', 'tv-call'); row.appendChild(el('span', 'n', c.name)); row.appendChild(el('span', 'b', c.brief || ''));
      list.appendChild(row);
      // the terminal shows what a call wrote or ran, expanded; here it opens on a tap of the call
      if (c.body || c.result) {
        row.classList.add('has-body');
        const det = el('div', 'tv-call-body'); det.hidden = true;
        if (c.body) { const pre = el('pre', 'tv-pre', c.body); det.appendChild(pre); }
        if (c.result) { det.appendChild(el('div', 'tv-res-lbl', 'result')); det.appendChild(el('pre', 'tv-pre res', c.result)); }
        list.appendChild(det);
        row.addEventListener('click', ev => { ev.stopPropagation(); det.hidden = !det.hidden; row.classList.toggle('open', !det.hidden); });
      }
    }
    d.appendChild(list);
    body.addEventListener('click', () => { list.hidden = !list.hidden; d.classList.toggle('open', !list.hidden); });
  }
  return d;
}
// fresh = start again at the newest; otherwise fetch the page before what we already have
// ---------------------------------------------------------------- copying from the live terminal
// 2026-09-09: selecting in the terminal view and copying "copied the whole text in the session".
// The stylesheet hands selection to Android by forcing user-select:text onto xterm's rows, so the user's
// selection is a DOM Range over rows xterm repaints constantly (the cursor row on every blink, the rows
// the session's spinner touches). When a node at either end of a live Range is replaced, the browser
// moves that end up to the row element, so String(selection) grows to whole rows — while the handles
// still look where the user left them — and xterm's own copy handler steps aside (its selection model is
// empty: touch sends it no mouse events). The ROW elements persist, though. So map the Range's ends to
// (row, column) and read the text from xterm's BUFFER, which is right whatever the DOM did in between,
// and can never reach outside the rows the user chose. Used by the copy event and by the menu's
// "Copy the selection". Null when the selection is not in the live terminal (text view, select mode).
// PHONE_ROWS names the rows container; the desktop test deck substitutes its own.
const PHONE_ROWS = '.term-wrap .xterm-rows';
function termSelectionText() {
  if (!term || selecting) return null;
  const s = window.getSelection && window.getSelection();
  if (!s || s.isCollapsed || !s.rangeCount) return null;
  const rowsEl = document.querySelector(PHONE_ROWS);
  if (!rowsEl) return null;
  const range = s.getRangeAt(0);
  if (!rowsEl.contains(range.startContainer) && !rowsEl.contains(range.endContainer)) return null;
  const rows = [...rowsEl.children];
  if (!rows.length) return null;
  const point = (node, offset, isEnd) => {
    if (!rowsEl.contains(node)) return isEnd ? { row: rows.length - 1, col: Infinity } : { row: 0, col: 0 };
    if (node === rowsEl) {                          // an end that fell up to the container: a row boundary
      const r = Math.min(rows.length - 1, Math.max(0, isEnd ? offset - 1 : offset));
      return { row: r, col: isEnd ? Infinity : 0 };
    }
    let rowEl = node; while (rowEl && rowEl.parentNode !== rowsEl) rowEl = rowEl.parentNode;
    const row = Math.max(0, rows.indexOf(rowEl));
    // the column is the text between the row's start and the endpoint — a Range measures that for any node type
    const r2 = document.createRange(); r2.selectNodeContents(rowEl);
    try { r2.setEnd(node, offset); } catch { return { row, col: isEnd ? Infinity : 0 }; }
    return { row, col: r2.toString().length };
  };
  const a = point(range.startContainer, range.startOffset, false);
  const b = point(range.endContainer, range.endOffset, true);
  if (b.row < a.row || (b.row === a.row && b.col <= a.col)) return '';
  const buf = term.buffer.active;
  const out = [];
  for (let r = a.row; r <= b.row; r++) {
    const line = buf.getLine(buf.viewportY + r);
    const text = line ? line.translateToString(true) : '';
    const from = r === a.row ? a.col : 0, to = r === b.row ? b.col : Infinity;
    out.push(text.slice(from, to === Infinity ? undefined : to));
  }
  return out.join(String.fromCharCode(10));
}
// Capture phase on the document, ahead of xterm's own listener: the clipboard gets the buffer text.
document.addEventListener('copy', ev => {
  const text = termSelectionText();
  if (text == null || !ev.clipboardData) return;
  ev.clipboardData.setData('text/plain', text);
  ev.preventDefault(); ev.stopImmediatePropagation();
}, true);

// "The phone app's Copy takes the whole text" (2026-11-20). Android drops a selection the moment the ••• button
// is tapped, before its menu is built: the menu saw no selection, offered no "Copy the selection", and the copies it
// did offer take the whole screen or the whole scrollback. So the last selection's text is kept as it changes, and
// the menu uses it when the selection went away just before the menu opened (SEL_GRACE_MS).
function currentSelText() {
  try {
    const fromTerm = termSelectionText();          // the live terminal: read the buffer, not the DOM (see termSelectionText)
    if (fromTerm != null) return fromTerm.trim() ? fromTerm : '';
    const s = window.getSelection && window.getSelection(); const t = (s && !s.isCollapsed) ? String(s) : ''; return t.trim() ? t : '';
  } catch { return ''; }
}
const SEL_GRACE_MS = 1500;
let lastSel = { text: '', goneAt: 0 };
document.addEventListener('selectionchange', () => {
  const t = currentSelText();
  if (t) lastSel = { text: t, goneAt: 0 };
  else if (lastSel.text && !lastSel.goneAt) lastSel.goneAt = Date.now();
});
// the selection the menu means: the one on screen, or the one the tap that opened the menu just dropped
function menuSelText() {
  const t = currentSelText();
  if (t) return t;
  return (lastSel.text && lastSel.goneAt && Date.now() - lastSel.goneAt < SEL_GRACE_MS) ? lastSel.text : '';
}

// A selection is destroyed the moment its DOM is replaced, and this list refreshes every time the
// session says anything — which is why copying kept failing. Never rebuild under a live selection.
function hasSelection() {
  const s = window.getSelection && window.getSelection();
  return !!(s && !s.isCollapsed && String(s).trim());
}
function loadTranscript(fresh) {
  if (!termId || tvLoading) return;
  if (isShellTile()) { $('#tv-top').textContent = 'a shell has no transcript — this is the terminal'; return; }   // never asked for; a guard
  if (fresh && hasSelection()) { tvDeferred = true; return; }
  const list = $('#tv-list'), box = $('#textview');
  if (fresh) { tvFrom = null; tvTotal = 0; list.textContent = ''; liveNode = null; liveLast = ''; $('#tv-top').textContent = 'loading…'; }
  if (!fresh && tvFrom === 0) return;
  tvLoading = true;
  call('transcript', termId, { before: fresh ? null : tvFrom, limit: 30 })
    .then(p => {
      tvLoading = false;
      tvTotal = p.total;
      if (!p.entries.length && fresh) { $('#tv-top').textContent = 'nothing in this transcript yet'; return; }
      const keepH = box.scrollHeight, keepTop = box.scrollTop;
      const frag = document.createDocumentFragment();
      for (const e of p.entries) frag.appendChild(tvEntry(e));
      if (fresh) list.appendChild(frag); else list.insertBefore(frag, list.firstChild);
      tvFrom = p.from;
      $('#tv-top').textContent = tvFrom > 0 ? `${tvFrom} earlier message${tvFrom === 1 ? '' : 's'} — scroll up for more` : 'the start of this session';
      if (fresh) box.scrollTop = box.scrollHeight;
      else box.scrollTop = keepTop + (box.scrollHeight - keepH);      // keep the reader's place
    })
    .catch(e => { tvLoading = false; $('#tv-top').textContent = String(e.message || e); });
}
document.addEventListener('selectionchange', () => {
  if (tvDeferred && !hasSelection() && viewMode === 'text') { tvDeferred = false; loadTranscript(true); }
});
$('#textview').addEventListener('scroll', () => {
  const box = $('#textview');
  if (box.scrollTop < 200) loadTranscript(false);
}, { passive: true });

// ---------------------------------------------------------------- the turn in progress
// The transcript only ever holds FINISHED messages, so while the session is working the text view
// would go quiet for the whole turn. But the terminal is attached the entire time and draws the
// answer as it streams — so read the current turn's output straight off the screen and show it as
// a provisional message, redrawn as it grows, then swap it for the real one when the transcript
// carries it. The same optimistic-then-confirm shape as sending, pointed the other way.
//
// Where the current turn is on the screen: Claude Code draws a separator (a run of box-drawing
// dashes) above and below its input box at the bottom. The conversation ends above the upper one.
// Within it, the last line beginning with the prompt marker and some text is the user's latest
// message; everything after that line is the turn's output so far.
function liveTurnText() {
  if (!term) return '';
  const buf = term.buffer.active;
  const rows = [];
  for (let i = 0; i < term.rows; i++) {
    const line = buf.getLine(buf.baseY + i);
    rows.push(line ? line.translateToString(true).replace(/\s+$/, '') : '');
  }
  // a separator drawn wider than the terminal wraps into several dash-only rows; one boundary
  const seps = [];
  rows.forEach((r, i) => {
    if (!/^\s*\u2500{8,}\s*$/.test(r)) return;
    if (seps.length && seps[seps.length - 1].end === i - 1) seps[seps.length - 1].end = i;
    else seps.push({ start: i, end: i });
  });
  const end = seps.length >= 2 ? seps[seps.length - 2].start : (seps.length === 1 ? seps[0].start : rows.length);
  let start = -1;
  for (let i = end - 1; i >= 0; i--) {
    if (new RegExp('^\\s*' + MARKER + '\\s+\\S').test(rows[i])) { start = i; break; }
  }
  if (start < 0) return '';
  // skip the user's own message (its wrapped lines are indented to its text column)
  let i = start + 1;
  while (i < end && /^\s{2,}\S/.test(rows[i]) && !/^\s*[\u25cf\u2022*-]\s/.test(rows[i])) i++;
  // a prompt (permission, a question, a menu) is drawn where the input box was: while the
  // session waits, show everything down to the last row so the prompt is in the strip
  const t = termId ? tileById(termId) : null;
  const out = rows.slice(i, (t && t.status === 'waiting') ? rows.length : end);
  while (out.length && !out[0].trim()) out.shift();
  while (out.length && !out[out.length - 1].trim()) out.pop();
  return out.join(String.fromCharCode(10));
}

let liveTimer = null, liveNode = null, liveLast = '', liveIdleSince = 0;
function scheduleLive() { clearTimeout(liveTimer); liveTimer = setTimeout(renderLive, 150); }
function renderLive() {
  if (viewMode !== 'text' || !termId) return;
  const t = tileById(termId);
  const busy = !!t && (t.status === 'busy' || t.status === 'waiting');
  const text = busy ? liveTurnText() : '';
  const list = $('#tv-list'), box = $('#textview');
  if (!text) {
    // idle: give the transcript a moment to land the finished message, then let go of the draft
    if (liveNode) {
      if (!liveIdleSince) liveIdleSince = Date.now();
      if (Date.now() - liveIdleSince > 6000) { liveNode.remove(); liveNode = null; liveLast = ''; }
    }
    return;
  }
  liveIdleSince = 0;
  if (text === liveLast) return;
  liveLast = text;
  const atEnd = box.scrollHeight - box.scrollTop - box.clientHeight < 80;
  if (!liveNode) {
    // verbatim, not markdown: this is the terminal's own drawing of the turn — its spinner, its
    // tool lines, its prompt — in the same face, so the two views agree while it runs
    liveNode = tvEntry({ role: 'assistant', at: new Date().toISOString(), text: '' });
    liveNode.classList.add('streaming', 'live');
    liveNode.querySelector('.tv-body').textContent = text;
    list.appendChild(liveNode);
  } else {
    liveNode.querySelector('.tv-body').textContent = text;
  }
  if (atEnd) box.scrollTop = box.scrollHeight;
}

// ---------------------------------------------------------------- the tile sheet
// Prompt later: the deck on the desktop holds a message for this session and types it in at a time.
// Written here, kept there — the phone may be asleep when the time comes. The time picks are the
// account's own reset times, since that is what this is for.
function openHoldSheet(id) {
  const t = tileById(id);
  if (!t) return;
  const sheet = $('#sheet'), box = $('#sheet-actions');
  $('#sheet-title').textContent = `Prompt later · ${t.title || 'session'}`;
  box.textContent = '';
  if (sheet.hidden) showSheet();
  const note = (s, cls) => box.appendChild(el('div', 'note' + (cls ? ' ' + cls : ''), s));
  note('Claude Deck on the desktop holds this and types it into this session at the time you pick. If the session is busy then, it goes in as soon as it is idle at its prompt. The desktop deck must be running.');
  const ta = el('textarea', 'hold-text'); ta.rows = 4; ta.placeholder = 'the prompt'; ta.spellcheck = false;
  ta.value = loadDraft(id);
  box.appendChild(ta);
  const u = (state.usage || {})[t.account] || {};
  const picks = [];
  // five minutes after the reset, not on it: the window is open for certain by then
  const AFTER = 5 * 60 * 1000;
  const addPick = (label, iso) => { const ms = Date.parse(iso || ''); if (ms > Date.now()) picks.push({ label, ms: ms + AFTER }); };
  addPick('session reset +5m', u.session && u.session.resetsAt);
  addPick('week reset +5m', u.week && u.week.resetsAt);
  for (const sc of (u.scoped || [])) addPick(`${sc.model || 'model'} reset +5m`, sc.resetsAt);
  picks.push({ label: 'in 1 h', ms: Date.now() + 3600 * 1000 });
  const local = (ms) => { const d = new Date(ms); d.setMinutes(d.getMinutes() - d.getTimezoneOffset()); return d.toISOString().slice(0, 16); };
  const time = el('input', 'hold-time'); time.type = 'datetime-local'; time.value = local(picks[0].ms);
  box.appendChild(time);
  const pk = el('div', 'pick');
  for (const p of picks) { const b = el('button', '', `${p.label} ${fmtHeldTime(p.ms)}`); b.addEventListener('click', () => { time.value = local(p.ms); for (const o of pk.children) o.classList.toggle('now', o === b); }); pk.appendChild(b); }
  if (pk.children.length) pk.children[0].classList.add('now');
  box.appendChild(pk);
  const err = note('', 'err');
  const hold = el('button', 'g2', 'Hold it');
  hold.addEventListener('click', () => {
    const at = Date.parse(time.value);
    if (!ta.value.trim()) { err.textContent = 'nothing to hold'; return; }
    if (!(at > Date.now())) { err.textContent = 'that time has passed'; return; }
    hold.disabled = true;
    call('holdAdd', id, { text: ta.value, at })
      .then(() => { if (loadDraft(id) === ta.value) { try { localStorage.removeItem(draftKey(id)); } catch { /* ignore */ } if (termId === id) { $('#compose-input').value = ''; growCompose(); } } toast(`held for ${fmtHeldTime(at)}`); openHoldSheet(id); })
      .catch(e => { hold.disabled = false; err.textContent = String(e.message || e); });
  });
  box.appendChild(hold);
  // what is already held for this session
  const items = t.held || [];
  if (items.length) {
    note('Held for this session', 'g3');
    for (const i of items) {
      const row = el('div', 'sheet-row held ' + i.state);
      const lbl = el('span', 'held-lbl', `${fmtHeldTime(i.at)} · ${i.text.split(String.fromCharCode(10))[0].slice(0, 60)}`);
      const st = el('span', 'side', i.state === 'held' ? 'held' : i.state === 'due' ? 'waiting' : i.state === 'sending' ? 'typing' : i.state === 'sent' ? (i.note ? 'typed?' : 'sent') : 'failed');
      const x = el('button', 'held-x', '×');
      x.addEventListener('click', () => call('holdRemove', i.id).then(() => openHoldSheet(id)).catch(e => toast(String(e.message || e))));
      row.append(lbl, st, x);
      box.appendChild(row);
      if (i.note) note(i.note, 'held-note');
    }
  }
  const back = el('button', '', 'Back');
  back.addEventListener('click', () => openSheet(id));
  box.appendChild(back);
}
function fmtHeldTime(ms) {
  const d = new Date(ms), now = new Date();
  const hm = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  if (d.toDateString() === now.toDateString()) return hm;
  const tomorrow = new Date(now); tomorrow.setDate(now.getDate() + 1);
  if (d.toDateString() === tomorrow.toDateString()) return `tomorrow ${hm}`;
  return `${['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'][d.getDay()]} ${hm}`;
}
function openSheet(id, mode) {
  const t = tileById(id);
  if (!t) return;
  const sheet = $('#sheet'), box = $('#sheet-actions');
  $('#sheet-title').textContent = `${t.kind === 'claude' ? acctLabel(t.account) + ' · ' : ''}${t.title || 'session'}`;
  box.textContent = '';
  showSheet();
  const add = (label, cls, fn) => { const b = el('button', cls, label); b.addEventListener('click', fn); box.appendChild(b); return b; };
  const note = (s, cls) => box.appendChild(el('div', 'note' + (cls ? ' ' + cls : ''), s));
  const close = () => hideSheet();
  const run = (p, msg) => { close(); Promise.resolve(p).then(() => msg && toast(msg)).catch(e => toast(String(e.message || e))); };

  if (mode && mode.confirm) {
    note(mode.text);
    add(mode.label, 'danger', () => run(mode.run(), mode.done));
    add('Cancel', '', () => openSheet(id));
    return;
  }
  const inTerm = termId === id && term;
  if (!inTerm) add('Open terminal', '', () => { close(); openTerm(id); });
  if (!inTerm && t.kind === 'claude' && t.sessionId) add('Pages this session made', '', () => sessionPagesSheet(id));
  if (inTerm) {
    // The session's own menu, in the order ruled 2026-09-03: reading first, then the terminal's
    // tools, then the width, then the deep buffer. From the text view, a tool that only means
    // something in the terminal is shown greyed with the word 'terminal' beside it.
    const inText = viewMode === 'text';
    const only = (label, fn, cls = 'g3') => {
      if (!inText) return add(label, cls, fn);
      const b = el('button', 'dim ' + cls, label); b.disabled = true;
      b.appendChild(el('span', 'side', 'terminal'));
      box.appendChild(b); return b;
    };
    add('Scroll to the top', 'g1', () => {
      close();
      if (inText) {
        tvLoading = false;
        call('transcript', termId, { before: 30, limit: 30 })
          .then(p => {
            const list = $('#tv-list');
            list.textContent = '';
            for (const e of p.entries) list.appendChild(tvEntry(e));
            tvFrom = p.from; tvTotal = p.total;
            $('#tv-top').textContent = 'the start of this session';
            $('#textview').scrollTop = 0;
          })
          .catch(e => toast(String(e.message || e)));
      } else if (termId) { if (stitchOn()) { stitchExpectTop = true; sessionBack = 1e9; } sendRaw({ t: 'input', id: termId, d: ESC + '[1;5H' }); }   // the app's ctrl+Home
    });
    add('Scroll to the bottom', 'g1', () => {
      close();
      if (inText) loadTranscript(true);
      else scrollTerminalToEnd();
    });
    // reading too: the .html files this session made, for the page viewer (2026-11-17)
    if (t.kind === 'claude' && t.sessionId) add('Pages this session made', 'g1', () => sessionPagesSheet(id));
    { const n = (t.held || []).filter(h => h.state !== 'sent' && h.state !== 'failed').length; add(n ? `Prompt later… (${n} held)` : 'Prompt later…', 'g2', () => openHoldSheet(id)); }
    const selectItem = () => add(selecting ? 'Stop selecting' : 'Select text', 'g2', () => { close(); setSelecting(!selecting); });
    if (!inText && !stitchOn()) selectItem();          // the stitched view selects natively; the item is for the terminal itself
    // When the reader has actually selected something — selection mode, the stitched view, the text
    // view — that selection is what Copy should mean. Both items below copy in bulk, so without this
    // one the menu answers "copy this" with the whole screen or the whole scrollback. Capture the text
    // HERE, at build time: closing a sheet replaces DOM, and a selection dies the moment its DOM does.
    {
      const sel = menuSelText();                       // (one the tap on ••• just dropped counts: menuSelText)
      if (sel) add('Copy the selection', 'g2', async () => {
        close(); lastSel = { text: '', goneAt: 0 };
        const n = sel.split(String.fromCharCode(10)).length;
        toast(await copyText(sel) ? `copied ${n} line${n === 1 ? '' : 's'}` : 'could not reach the clipboard');
      });
    }
    add('Copy what is on screen', 'g2', async () => {
      close();
      if (inText) {
        const text = [...document.querySelectorAll('#tv-list .tv-msg')]
          .map(n => (n.classList.contains('user') ? '> ' : '') + n.querySelector('.tv-body').textContent)
          .join(String.fromCharCode(10) + String.fromCharCode(10));
        const n = document.querySelectorAll('#tv-list .tv-msg').length;
        toast(await copyText(text) ? `copied ${n} messages` : 'could not reach the clipboard');
        return;
      }
      const buf = term.buffer.active;
      const out = [];
      for (let i = 0; i < term.rows; i++) {
        const line = buf.getLine(buf.viewportY + i);
        out.push(line ? line.translateToString(true) : '');
      }
      const text = out.join(String.fromCharCode(10)).replace(/\s+$/, '');
      toast(await copyText(text) ? `copied ${term.rows} lines` : 'could not reach the clipboard');
    });
    add('Copy the scrollback', 'g2', async () => {
      close();
      if (inText) {
        // the whole conversation off disk, not just what is loaded
        try {
          const p = await call('transcript', termId, { limit: 100000 });
          const text = p.entries.map(e => (e.role === 'user' ? '> ' : '') + e.text).join(String.fromCharCode(10) + String.fromCharCode(10));
          toast(await copyText(text) ? `copied ${p.entries.length} messages` : 'could not reach the clipboard');
        } catch (e) { toast(String(e.message || e)); }
        return;
      }
      term.selectAll();
      const text = term.getSelection();
      term.clearSelection();
      const lines = text.split(String.fromCharCode(10)).length;
      toast(await copyText(text) ? `copied ${lines} lines` : 'could not reach the clipboard');
    });
    if (inText) {
      add('Open terminal', 'g3', () => { close(); setViewMode('terminal'); });
      if (!stitchOn()) only('Select text', null, 'g23');
    }
    // ctrl+o is Claude Code's global toggle for tool output: one item here rather than a tap per line
    // 'last' is ctrl+o, Claude Code's toggle for the tool output of what is shown in full; 'all' is
    // ctrl+r, its full-transcript view (every past turn's folded 'Ran 6 shell commands' opened), which
    // Esc leaves — so the item reads 'Back from all' while it is open
    // both are the terminal's: offered there only (not greyed in the text view); 'all' reads its state off
    // the screen, so it says 'Collapse' when the transcript view is open however it was opened
    if (!inText) add(toolOutputOpen[id] ? 'Collapse last tool output' : 'Expand last tool output', 'g1', () => { close(); toolOutputOpen[id] = !toolOutputOpen[id]; sendRaw({ t: 'input', id, d: String.fromCharCode(15) }); sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [id] }); });
    // 'all' opens the full-transcript view (ctrl+r) AND turns the tool output on (ctrl+o) if it is not,
    // since the transcript view still folds each result until ctrl+o is on
    if (!inText) add(transcriptOpen[id] ? 'Collapse all tool output' : 'Expand all tool output', 'g1', () => {
      close();
      const open = !transcriptOpen[id]; transcriptOpen[id] = open;
      sendRaw({ t: 'input', id, d: open ? String.fromCharCode(18) : KEYS.esc });
      // 'all' turned the tool output on for itself: 'collapse all' turns it off again, so nothing stays
      // expanded that you did not ask for with 'expand last'
      if (open && !toolOutputOpen[id]) { toolOutputOpen[id] = true; toolOutputByAll[id] = true; setTimeout(() => { if (termId === id) sendRaw({ t: 'input', id, d: String.fromCharCode(15) }); }, 350); }
      if (!open && toolOutputByAll[id]) { toolOutputByAll[id] = false; toolOutputOpen[id] = false; setTimeout(() => { if (termId === id) sendRaw({ t: 'input', id, d: String.fromCharCode(15) }); }, 350); }
      sendRaw({ t: 'call', rid: 0, fn: 'userInput', args: [id] });
    });
    only('Redraw the screen', () => { close(); forceRedraw(); });
    only('Clear the view', () => { close(); term.clear(); });
    {
      const row = el('div', 'sheet-row g3' + (inText ? ' dim' : ''));
      const lab = el('span', '', '');
      const paintRow = () => { lab.textContent = 'Width: ' + (term ? term.cols : '?') + ' columns' + (colsSetting() ? '' : ' (by text size)'); };
      const minus = el('button', '', '\u2212'), plus = el('button', '', '+');
      minus.addEventListener('click', () => { setCols((term ? term.cols : 56) - 4, false); setTimeout(paintRow, 300); });
      plus.addEventListener('click', () => { setCols((term ? term.cols : 56) + 4, false); setTimeout(paintRow, 300); });
      if (inText) { minus.disabled = true; plus.disabled = true; }
      paintRow();
      row.appendChild(minus); row.appendChild(lab);
      if (inText) row.appendChild(el('span', 'side', 'terminal'));
      row.appendChild(plus);
      box.appendChild(row);
      note('Or pinch the terminal: fingers apart for bigger text, together for more columns.', 'g3');
    }
    only(deepBuffer ? 'Deep buffer: on' : 'Deep buffer: off', () => {
      close();
      deepBuffer = !deepBuffer;
      try { localStorage.setItem('deck.deep', deepBuffer ? '1' : '0'); } catch { /* private mode */ }
      viewOffset = 0; bufRows = 0;
      $('#term').style.transform = '';
      sizeTerminal();
      forceRedraw();
    });
  }
  const claudeApp = () => { if (t.bridge) add('Open in the Claude app', '', () => { close(); window.open(`https://claude.ai/code/${t.bridge}`, '_blank'); }); };
  if (!inTerm) claudeApp();
  add(t.pinned ? 'Unpin' : 'Pin', '', () => run(call('pin', id, !t.pinned)));
  add('Rename', '', () => {
    close();
    const v = prompt('Tile title (blank restores the session\'s own)', t.title || '');
    if (v !== null) call('renameTile', id, v).catch(e => toast(String(e.message || e)));
  });
  if (state.groups.length && !inTerm) {
    add('Group…', '', () => {
      box.textContent = '';
      note('Put this session in a group.');
      for (const g of state.groups) add(t.group === g ? `✓ ${g}` : g, '', () => run(call('setGroup', id, t.group === g ? null : g)));
      add('Cancel', '', () => openSheet(id));
    });
  }
  if (t.kind === 'claude' && !t.exited) {
    for (const a of state.accounts.filter(a => a.id !== t.account)) {
      add(`open ${a.label}`, '', () => openSheet(id, {
        confirm: true, label: `Move to ${a.label}`, done: `moving to ${a.label}`,
        text: `Move this session to ${a.label}? The process is killed here and the same session resumes there.`,
        run: () => call('moveTile', id, a.id),
      }));
    }
  }
  if (inTerm) claudeApp();
  if (t.exited) add('resume', '', () => run(call('restartTile', id)));
  if (!t.exited) {
    add(inTerm ? 'refresh session' : 'refresh', '', () => {
      if (t.fresh) return run(call('refreshTile', id));
      openSheet(id, { confirm: true, label: 'Refresh', text: t.status === 'busy' ? 'Refresh this session? It is busy, so the turn in progress is lost.' : 'Refresh this session? The process restarts and resumes where it was.', run: () => call('refreshTile', id) });
    });
  }
  // Claude in Chrome: the same refresh with the browser switched on, or off when the session already has
  // it. t.browser is true / false / null (null = the account's default); a PC too old to take the choice
  // sends no key, and then the item is left out rather than doing a plain refresh.
  if (!t.exited && t.kind === 'claude' && 'browser' in t) {
    const on = t.browser === true, what = on ? 'without browser' : 'with browser';
    const go = () => call('refreshTile', id, { browser: !on });
    add(inTerm ? `refresh session ${what}` : `refresh ${what}`, '', () => {
      if (t.fresh) return run(go());
      openSheet(id, { confirm: true, label: `Refresh ${what}`, text: t.status === 'busy' ? `Refresh this session ${what}? It is busy, so the turn in progress is lost.` : `Refresh this session ${what}? The process restarts and resumes where it was.`, run: go });
    });
  }
  add(t.exited ? 'remove' : 'close', 'danger', () => {
    if (t.fresh || t.exited) return run(call('closeTile', id).then(() => { if (termId === id) back(); }));
    openSheet(id, { confirm: true, label: 'Close', text: 'Close this session? The process is killed.', run: () => call('closeTile', id).then(() => { if (termId === id) back(); }) });
  });
}
// a tap on the backdrop closes it — on the touch itself as well as the click, since the click after a
// long-press does not always come
// ...but not the release of the long-press that opened it: that click lands wherever the finger was
const backdropTap = () => { if (Date.now() - sheetOpenedAt > 700) hideSheet(); };
// ...nor may it pick an item: a key that opens a sheet acts on the touch, and the release of that same
// touch lands on whatever the sheet put under the finger
$('#sheet-actions').addEventListener('click', ev => { if (Date.now() - sheetOpenedAt < 400) { ev.stopPropagation(); ev.preventDefault(); } }, true);
$('#sheet-actions').addEventListener('touchend', ev => { if (Date.now() - sheetOpenedAt < 400) { ev.stopPropagation(); ev.preventDefault(); } }, true);
$('#sheet').addEventListener('click', ev => { if (ev.target.id === 'sheet') backdropTap(); });
$('#sheet').addEventListener('touchend', ev => { if (ev.target.id === 'sheet') { ev.preventDefault(); backdropTap(); } });

// navigator.clipboard exists only in a secure context, which plain http:// over the tailnet is not,
// so fall back to the old selection-and-copy trick.
async function copyText(text) {
  try {
    if (navigator.clipboard && window.isSecureContext) { await navigator.clipboard.writeText(text); return true; }
  } catch { /* fall through */ }
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.top = '-1000px';
  document.body.appendChild(ta);
  ta.select();
  ta.setSelectionRange(0, text.length);
  let ok = false;
  try { ok = document.execCommand('copy'); } catch { /* blocked */ }
  ta.remove();
  return ok;
}

let toastTimer = null;
function toast(msg) {
  const t = $('#toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3200);
}

// ---------------------------------------------------------------- list toggles
function toggleBtn(sel, key, cls) {
  const b = $(sel);
  const read = () => localStorage.getItem(key) !== '0';
  const paint = () => { b.classList.toggle('on', read()); document.body.classList.toggle(cls, !read()); };
  b.addEventListener('click', () => { localStorage.setItem(key, read() ? '0' : '1'); paint(); });
  paint();
}
toggleBtn('#t-msgs', 'deck.msgs', 'hide-msgs');
toggleBtn('#t-bars', 'deck.bars', 'hide-bars');

// ---------------------------------------------------------------- the page viewer (2026-11-16 / 11-17)
// Any .html file on the PC, on its own screen: the pages in Settings -> Phone -> "Pages on the phone" on the desk (a
// button each in the list's top bar: the contributor report), an .html path in a session's message (tap it), and a
// session's "Pages this session made" (its menu). lib/phone.js serves it at /view/<page key>/<folder id>/<file>,
// sandboxed; links to pages beside it open in the frame, replacing it, and links anywhere else open in the browser.
// pageKey (from init/meta) is the key it wants in the path: the framed page is an opaque origin, so what it asks for
// carries no cookie, and a relative URL keeps the path. The phone never sends a configured page's path.
let pageAt = null;                                     // { dir, name, label, n?, path? } while the viewer shows one
const pagesOf = () => (state.settings && Array.isArray(state.settings.pages) ? state.settings.pages : []);
const viewSrc = v => `/view/${pageKey}/${v.dir}/${encodeURIComponent(v.name)}`;   // no cache-buster: the server says no-cache and the worker fetches first, so one cached copy per page (offline)
const oldPc = () => { toast('The PC runs an older Deck: restart it there to open pages here'); };
function renderPageBtns() {
  const box = $('#page-btns'); if (!box) return;
  const list = pagesOf(), key = JSON.stringify(list);
  if (box.dataset.key === key) return;
  box.dataset.key = key;
  box.replaceChildren(...list.map((p, n) => { const b = el('button', 'tbtn page-btn', p.label); b.dataset.page = String(n); b.addEventListener('click', () => openPage(n)); return b; }));
}
// A fresh frame each time: loading one adds no history entry, where navigating the old one would (the served page's
// own links replace it for the same reason), so one back always leaves the viewer.
function pageFrame(src) {
  const old = $('#page-frame'); if (old) old.remove();
  if (!src) return;
  const f = document.createElement('iframe'); f.id = 'page-frame'; f.className = 'page-frame'; f.title = 'page'; f.src = src;
  $('#page-body').appendChild(f);
}
function openView(v) {
  if (!pageKey || !v || !v.dir) return oldPc();
  pageAt = v;
  $('#page-title').textContent = v.label || v.name;
  pageFrame(viewSrc(v));
  if (current() === 's-page') rememberWhere('s-page'); else show('s-page');
}
function openPage(n) { const p = pagesOf()[n]; if (p) openView({ ...p, n }); }
// a path from a message or a session's list: the PC checks it and lets the viewer serve its folder, or says why not
function openHtmlPath(p) {
  if (!pageKey) return oldPc();
  call('viewHtml', p).then(v => openView(v)).catch(e => toast(String(e.message || e)));
}
// reload asks the PC again (after a Deck restart the folder has to be shown again)
$('#page-reload').addEventListener('click', () => {
  if (!pageAt) return;
  if (pageAt.path) openHtmlPath(pageAt.path); else if (pageAt.n != null) openPage(pageAt.n); else pageFrame(viewSrc(pageAt));
});
$('#page-back').addEventListener('click', () => { pageFrame(null); setTimeout(back, 0); });
// an .html path in the text view (tvMarkup marks it .hpath)
$('#tv-list').addEventListener('click', ev => {
  const h = ev.target && ev.target.closest ? ev.target.closest('.hpath') : null;
  if (!h) return;
  ev.stopPropagation();
  if (window.getSelection && String(window.getSelection())) return;   // selecting text, not opening it
  openHtmlPath(h.dataset.path);
});
// a session's own pages: what it wrote or named that is on the PC now, newest first
function sessionPagesSheet(id) {
  const box = $('#sheet-actions');
  const say = text => { box.textContent = ''; box.appendChild(el('div', 'note', text)); };
  if (!pageKey) return say('The PC runs an older Deck: restart it there to open pages here.');
  say('Looking through the session…');
  call('sessionHtml', id).then(list => {
    if (!list || !list.length) return say('No .html files from this session on the PC.');
    say(`${list.length} page${list.length === 1 ? '' : 's'} this session wrote or named, newest first`);
    for (const f of list) {
      const b = el('button', 'page-row');
      b.append(el('span', 'pr-name', f.name), el('span', 'pr-where', `${f.folder} · ${fmtAge(Date.now() - f.mtime)}`));
      b.addEventListener('click', () => { hideSheet(); openHtmlPath(f.path); });
      box.appendChild(b);
    }
  }).catch(e => say(String(e.message || e)));
}

// ---------------------------------------------------------------- new session
// One session per tap, one at a time. A tap sends at once, so on a stalled link (the socket still open,
// nothing coming back) every further tap queued another newTile and they all landed together when the
// link recovered: 15 empty sessions on 2026-10-08. While one is starting, every new / resume button is
// ignored and the tapped one says so; it lets go when the PC answers, refuses, or the call times out.
let newTileBusy = false;
function newTileOnce(btn, args) {
  if (newTileBusy) return;
  newTileBusy = true;
  document.body.classList.add('new-starting');
  const label = btn && btn.querySelector('span');
  const was = label ? label.textContent : null;
  if (label) label.textContent = 'starting…';
  call('newTile', args)
    .then(id => { if (id) openTerm(id); })
    .catch(e => toast(String(e.message || e)))
    .finally(() => {
      newTileBusy = false;
      document.body.classList.remove('new-starting');
      if (label) label.textContent = was;
    });
}
function renderNew() {
  $('#new-cwd').textContent = state.settings.cwd || state.home || '';
  const box = $('#new-accts');
  box.textContent = '';
  for (const a of state.accounts) {
    const b = el('button', 'row');
    b.appendChild(el('span', '', `New ${a.label} session`));
    b.appendChild(el('span', 'chev', '›'));
    b.addEventListener('click', () => newTileOnce(b, { kind: 'claude', account: a.id }));
    box.appendChild(b);
  }
}
$('#new-shell').addEventListener('click', e => newTileOnce(e.currentTarget, { kind: 'shell' }));
$('#go-elsewhere').addEventListener('click', () => show('s-elsewhere'));
$('#go-past').addEventListener('click', () => show('s-past'));

// ---------------------------------------------------------------- search
function renderSearch() {
  const q = $('#search-input').value.trim().toLowerCase();
  const box = $('#search-list');
  box.textContent = '';
  const hits = !q ? state.tiles : state.tiles.filter(t =>
    (t.title || '').toLowerCase().includes(q)
    || (t.lastText || '').toLowerCase().includes(q)
    || acctLabel(t.account).toLowerCase().includes(q));
  if (!hits.length) { box.appendChild(el('div', 'empty', 'nothing matches')); return; }
  for (const t of hits) box.appendChild(tileEl(t));
}
$('#search-input').addEventListener('input', renderSearch);

// ---------------------------------------------------------------- groups
function renderGroups() {
  const box = $('#groups-body');
  box.textContent = '';
  const on = localStorage.getItem('deck.grouped') === '1';
  const toggle = el('button', 'row');
  toggle.appendChild(el('span', '', on ? '✓ List is grouped' : 'Group the list'));
  toggle.addEventListener('click', () => { localStorage.setItem('deck.grouped', on ? '0' : '1'); renderGroups(); renderList(); });
  const rows = el('div', 'rows'); rows.appendChild(toggle); box.appendChild(rows);

  box.appendChild(el('h3', '', 'Groups'));
  const list = el('div', 'rows');
  if (!state.groups.length) list.appendChild(el('div', 'hint', 'No groups yet. A session joins one from its own menu (press and hold a tile).'));
  for (const g of state.groups) {
    const r = el('button', 'row');
    const n = state.tiles.filter(t => t.group === g).length;
    const label = el('span', ''); label.appendChild(document.createTextNode(g));
    label.appendChild(el('span', 'sub', `${n} session${n === 1 ? '' : 's'}`));
    r.appendChild(label); r.appendChild(el('span', 'chev', '✕'));
    r.addEventListener('click', () => {
      if (!confirm(`Remove the group "${g}"? Its sessions stay open, just ungrouped.`)) return;
      call('setGroups', state.groups.filter(x => x !== g)).then(renderGroups).catch(e => toast(String(e.message || e)));
    });
    list.appendChild(r);
  }
  box.appendChild(list);
  const add = el('button', 'wide', '+ new group');
  add.addEventListener('click', () => {
    const name = prompt('Group name');
    if (!name || !name.trim()) return;
    call('setGroups', [...state.groups, name.trim()]).then(renderGroups).catch(e => toast(String(e.message || e)));
  });
  box.appendChild(add);
}

// ---------------------------------------------------------------- move all
function acctOptions(sel, skip) {
  const keep = sel.value;
  sel.textContent = '';
  for (const a of state.accounts) {
    if (skip && a.id === skip) continue;
    const o = document.createElement('option');
    o.value = a.id; o.textContent = a.label;
    sel.appendChild(o);
  }
  if (keep && [...sel.options].some(o => o.value === keep)) sel.value = keep;
}
function moveBars(boxSel, accountId) {
  const box = $(boxSel);
  box.textContent = '';
  const bars = el('div', 't-bars');
  bars.innerHTML = BARS_HTML;
  box.appendChild(bars);
  applyBars(bars, accountId, { fable: true });   // the fable bar too, when the account has one
}
// 'all' moves every open session of the source account; 'these' moves only the ticked ones, which
// is what `moveAll`'s onlyIds argument has always been for.
let moveMode = 'all';
const movePicked = new Set();
function renderMovePick() {
  const box = $('#move-pick');
  box.hidden = moveMode !== 'these';
  box.textContent = '';
  if (moveMode !== 'these') return;
  const from = $('#move-from').value;
  const mine = state.tiles.filter(t => t.kind === 'claude' && t.account === from);
  if (!mine.length) { box.appendChild(el('div', 'empty', 'nothing open on that account')); return; }
  for (const t of mine) {
    const b = el('button', 'pick' + (movePicked.has(t.id) ? ' on' : ''));
    b.appendChild(el('span', 'box', movePicked.has(t.id) ? '\u2713' : ''));
    const body = el('span', '');
    body.appendChild(document.createTextNode(t.title || 'untitled'));
    const mdl = screenModelOf[t.id] || t.model || '';
    body.appendChild(el('span', 'who', [t.status, mdl, t.effort].filter(Boolean).join(' · ')));
    b.appendChild(body);
    b.addEventListener('click', () => {
      movePicked.has(t.id) ? movePicked.delete(t.id) : movePicked.add(t.id);
      renderMovePick();
      updateMoveButton();
    });
    box.appendChild(b);
  }
}
function updateMoveButton() {
  const from = $('#move-from').value, to = $('#move-to').value;
  const all = state.tiles.filter(t => t.kind === 'claude' && t.account === from);
  const n = moveMode === 'all' ? all.length : all.filter(t => movePicked.has(t.id)).length;
  $('#move-status').textContent = moveMode === 'all'
    ? `${all.length} open session${all.length === 1 ? '' : 's'} on ${acctLabel(from)}`
    : `${n} of ${all.length} picked`;
  $('#move-go').disabled = !n || from === to;
  $('#move-confirm').hidden = true;
}
function renderMove() {
  acctOptions($('#move-from'));
  acctOptions($('#move-to'));
  const from = $('#move-from').value;
  if ($('#move-to').value === from) {
    const other = state.accounts.find(a => a.id !== from);
    if (other) $('#move-to').value = other.id;
  }
  moveBars('#move-from-bars', $('#move-from').value);
  moveBars('#move-to-bars', $('#move-to').value);
  $('#move-mode-all').classList.toggle('on', moveMode === 'all');
  $('#move-mode-these').classList.toggle('on', moveMode === 'these');
  renderMovePick();
  updateMoveButton();
}
$('#move-mode-all').addEventListener('click', () => { moveMode = 'all'; renderMove(); });
$('#move-mode-these').addEventListener('click', () => { moveMode = 'these'; renderMove(); });
$('#move-from').addEventListener('change', renderMove);
$('#move-to').addEventListener('change', renderMove);
$('#move-go').addEventListener('click', () => {
  const from = $('#move-from').value, to = $('#move-to').value;
  const c = $('#move-confirm');
  const all = state.tiles.filter(t => t.kind === 'claude' && t.account === from);
  const onlyIds = moveMode === 'these' ? all.filter(t => movePicked.has(t.id)).map(t => t.id) : null;
  const n = onlyIds ? onlyIds.length : all.length;
  if (c.hidden) {
    c.textContent = `Move ${n} session${n === 1 ? '' : 's'} from ${acctLabel(from)} to ${acctLabel(to)}? Each one is killed here and resumed there. Press Move again to go.`;
    c.hidden = false;
    return;
  }
  c.hidden = true;
  $('#move-go').disabled = true;
  $('#move-status').textContent = 'moving…';
  call('moveAll', from, to, $('#move-continue').checked, onlyIds)
    .then(r => { $('#move-status').textContent = `moved ${r.moved}${r.errors && r.errors.length ? `, ${r.errors.length} failed` : ''}`; renderMove(); })
    .catch(e => { $('#move-status').textContent = String(e.message || e); $('#move-go').disabled = false; });
});

// ---------------------------------------------------------------- elsewhere / past
function renderElsewhere() {
  const box = $('#elsewhere-body');
  box.textContent = '';
  box.appendChild(el('div', 'hint', 'Claude sessions running in other windows on the PC. Bringing one in kills that process and resumes the same session here.'));
  call('externalSessions').then(list => {
    box.querySelectorAll('.rows').forEach(n => n.remove());
    const rows = el('div', 'rows');
    if (!list.length) rows.appendChild(el('div', 'empty', 'nothing running elsewhere'));
    for (const s of list) {
      const r = el('button', 'row');
      const label = el('span', '');
      label.appendChild(document.createTextNode(s.title || s.name || s.sessionId.slice(0, 8)));
      label.appendChild(el('span', 'sub', `${s.label} · pid ${s.pid} · ${s.status}`));
      r.appendChild(label); r.appendChild(el('span', 'chev', '›'));
      r.addEventListener('click', () => {
        if (!confirm(`Bring this session in? The copy running elsewhere (pid ${s.pid}) is killed first.`)) return;
        call('adoptSession', s).then(id => { if (id) openTerm(id); }).catch(e => toast(String(e.message || e)));
      });
      rows.appendChild(r);
    }
    box.appendChild(rows);
  }).catch(e => box.appendChild(el('div', 'empty', String(e.message || e))));
}
function renderPast() {
  const box = $('#past-body');
  box.textContent = '';
  box.appendChild(el('div', 'hint', 'Sessions on disk that are not open anywhere. Opening one resumes it — reading the transcript costs nothing.'));
  call('pastSessions').then(list => {
    const rows = el('div', 'rows');
    if (!list.length) rows.appendChild(el('div', 'empty', 'nothing on disk'));
    for (const s of list) {
      const r = el('button', 'row');
      const label = el('span', '');
      label.appendChild(document.createTextNode(s.title || s.sessionId.slice(0, 8)));
      label.appendChild(el('span', 'sub', `${s.label} · ${fmtAge(Date.now() - s.mtime)} ago`));
      r.appendChild(label); r.appendChild(el('span', 'chev', '›'));
      r.addEventListener('click', () => newTileOnce(r, { kind: 'claude', account: s.account, sessionId: s.sessionId, cwd: s.cwd }));
      rows.appendChild(r);
    }
    box.appendChild(rows);
  }).catch(e => box.appendChild(el('div', 'empty', String(e.message || e))));
}

// ---------------------------------------------------------------- settings
let settingsDraft = null;
function renderSettings() {
  const box = $('#settings-body');
  box.textContent = '';
  box.appendChild(el('div', 'hint', 'These are the deck\'s own settings on the PC — changing them here changes them there too. Accounts, fonts and colours stay on the desktop.'));
  call('getSettings').then(s => {
    settingsDraft = JSON.parse(JSON.stringify(s));
    settingsDraft.bars = settingsDraft.bars || {};      // an older settings file may predate a bar
    box.textContent = '';
    box.appendChild(el('div', 'hint', 'The deck\'s settings on the PC. Saving changes them for the desktop window too.'));

    const field = (label, node) => { const f = el('label', 'field'); f.appendChild(el('span', '', label)); f.appendChild(node); box.appendChild(f); return node; };
    const select = (value, opts, fn) => {
      const s2 = document.createElement('select');
      for (const [v, t] of opts) { const o = document.createElement('option'); o.value = v; o.textContent = t; s2.appendChild(o); }
      s2.value = value; s2.addEventListener('change', () => fn(s2.value));
      return s2;
    };
    const check = (label, value, fn) => {
      const l = el('label', 'chk');
      const i = document.createElement('input'); i.type = 'checkbox'; i.checked = !!value;
      i.addEventListener('change', () => fn(i.checked));
      l.appendChild(i); l.appendChild(el('span', '', label));
      box.appendChild(l);
      return i;
    };
    const number = (label, value, min, max, fn) => {
      const i = document.createElement('input'); i.type = 'number'; i.min = min; i.max = max; i.value = value;
      i.addEventListener('change', () => fn(+i.value));
      return field(label, i);
    };

    box.appendChild(el('h3', '', 'Tiles'));
    field('Effort shown as', select(settingsDraft.effortStyle || 'tag', [['tag', 'tag'], ['color', 'color'], ['none', 'nothing']], v => settingsDraft.effortStyle = v));
    number('Preview lines', (settingsDraft.msgLines || {}).normal || 3, 1, 40, v => settingsDraft.msgLines = { ...settingsDraft.msgLines, normal: v });

    box.appendChild(el('h3', '', 'Bars'));
    field('Labels show', select(settingsDraft.barLabelMode || 'reset', [['reset', 'reset time and day'], ['countdown', 'time until reset']], v => settingsDraft.barLabelMode = v));
    check('session reset label', (settingsDraft.barLabels || {}).time !== false, v => settingsDraft.barLabels = { ...settingsDraft.barLabels, time: v });
    check('week reset label', (settingsDraft.barLabels || {}).day !== false, v => settingsDraft.barLabels = { ...settingsDraft.barLabels, day: v });
    for (const k of ['session', 'week', 'fable', 'context']) {
      check(`show the ${k} bar`, (settingsDraft.bars[k] || {}).show !== false, v => settingsDraft.bars[k] = { ...settingsDraft.bars[k], show: v });
    }

    box.appendChild(el('h3', '', 'Notifications on the PC'));
    check('toast when a session waits', (settingsDraft.notify || {}).waiting !== false, v => settingsDraft.notify = { ...settingsDraft.notify, waiting: v });
    check('toast when a turn finishes', (settingsDraft.notify || {}).done !== false, v => settingsDraft.notify = { ...settingsDraft.notify, done: v });

    box.appendChild(el('h3', '', 'Terminal'));
    number('Font size (desktop)', settingsDraft.fontSize || 14, 9, 28, v => settingsDraft.fontSize = v);

    box.appendChild(el('h3', '', 'This phone'));
    const fs = el('label', 'field');
    fs.appendChild(el('span', '', 'Session text size'));
    const sel = document.createElement('select');
    for (const n of [10, 11, 12, 13, 14, 15, 16]) { const o = document.createElement('option'); o.value = n; o.textContent = n + ' px'; sel.appendChild(o); }
    sel.value = String(monoSize());
    sel.addEventListener('change', () => { try { localStorage.setItem('deck.fontpx', sel.value); } catch { /* ignore */ } applyFonts(); });
    fs.appendChild(sel);
    box.appendChild(fs);
    const cw = el('label', 'field');
    cw.appendChild(el('span', '', 'Terminal width'));
    const csel = document.createElement('select');
    for (const [v, l] of [[0, 'by text size'], [56, '56 columns'], [60, '60 columns'], [64, '64 columns'], [72, '72 columns'], [80, '80 columns'], [96, '96 columns'], [120, '120 columns']]) {
      const o = document.createElement('option'); o.value = v; o.textContent = l; csel.appendChild(o);
    }
    csel.value = String(colsSetting());
    csel.addEventListener('change', () => { try { localStorage.setItem('deck.cols', csel.value); } catch { /* ignore */ } charRatio = 0; applyFonts(); });
    cw.appendChild(csel);
    box.appendChild(cw);
    box.appendChild(el('div', 'hint', 'A width in columns picks the text size for you. Claude Code draws diffs a dozen columns narrower than the terminal, so 64 or 72 gives them room; the text view follows the same size.'));
    const tw = el('label', 'field');
    tw.appendChild(el('span', '', 'Typing block width'));
    const tsel = document.createElement('select');
    for (const n of [48, 52, 56, 60, 64, 72, 80]) { const o = document.createElement('option'); o.value = n; o.textContent = n + ' columns'; tsel.appendChild(o); }
    tsel.value = String(typeColsSetting());
    tsel.addEventListener('change', () => { try { localStorage.setItem('deck.typecols', tsel.value); } catch { /* ignore */ } applyFonts(); });
    tw.appendChild(tsel);
    box.appendChild(tw);
    box.appendChild(el('div', 'hint', 'The prompt row and footer under the terminal keep this size whatever the terminal is set to.'));
    box.appendChild(el('div', 'hint', 'Applies to the terminal, the text view and selection alike, so a line sits the same in all three. The face follows the desktop\'s font setting when the phone has it.'));
    const local = (label, key, hint, onChange) => {
      const lab = el('label', 'chk');
      const cb = document.createElement('input'); cb.type = 'checkbox';
      try { cb.checked = localStorage.getItem(key) !== '0'; } catch { cb.checked = true; }
      cb.addEventListener('change', () => { try { localStorage.setItem(key, cb.checked ? '1' : '0'); } catch { /* ignore */ } onChange && onChange(cb.checked); });
      lab.appendChild(cb); lab.appendChild(el('span', '', label)); box.appendChild(lab);
      if (hint) box.appendChild(el('div', 'hint', hint));
    };
    local('Stitched scrollback', 'deck.stitch', 'Every repaint the session sends is stitched into one local history, so scrolling back never shows a seam. Off: the terminal itself, with the deep buffer.', v => {
      paintStitched(); resetStitch(); if (term) { sizeTerminal(); forceRedraw(); }
    });
    local('Pin the input box and footer while reading above', 'deck.sticky', null, () => { stickyText = ''; scheduleSticky(); });
    const fb = el('label', 'field');
    fb.appendChild(el('span', '', '\u201cHow is Claude doing this session?\u201d'));
    const fsel = document.createElement('select');
    for (const [v, l] of [['ask', 'show it, with buttons'], ['dismiss', 'always dismiss it']]) { const o = document.createElement('option'); o.value = v; o.textContent = l; fsel.appendChild(o); }
    fsel.value = feedbackSetting();
    fsel.addEventListener('change', () => { try { localStorage.setItem('deck.feedback', fsel.value); } catch { /* ignore */ } });
    fb.appendChild(fsel);
    box.appendChild(fb);
    const clear = el('button', 'wide', 'Reset the touch counters');
    clear.addEventListener('click', () => {
      for (const k of Object.keys(touchStats)) touchStats[k] = 0;
      noteTouch();
      toast('counters cleared — open a session and drag, then come back');
    });
    box.appendChild(clear);
    const reload = el('button', 'wide', 'Reload the app');
    reload.addEventListener('click', () => location.reload());
    box.appendChild(reload);
    const info = el('div', 'hint');
    const mode = term ? (inAltScreen() ? 'alt-screen (wheel)' : 'normal (buffer)') : 'no terminal open';
    const deep = term ? ` · buffer ${bufRows} rows showing ${viewRows}` : '';
    info.textContent = `Build ${BUILD}${serverBuild && serverBuild !== BUILD ? ' (PC has ' + serverBuild + ')' : ' — up to date'} · touch: page ${touchStats.doc}, terminal ${touchStats.start} starts / ${touchStats.move} moves / ${touchStats.scrolled} handled · SENT: ${touchStats.wheel} wheel notches, ${touchStats.lines} buffer scrolls · last terminal was ${mode}${deep} · local drags ${touchStats.local || 0} · size reasserts ${touchStats.reasserts || 0} · connected to ${location.host}. The list and the terminals come from the PC; nothing on this phone talks to Anthropic.`;
    box.appendChild(info);
  }).catch(e => box.appendChild(el('div', 'empty', String(e.message || e))));
}
$('#set-save').addEventListener('click', () => {
  if (!settingsDraft) return;
  call('saveSettings', settingsDraft).then(() => back()).catch(e => toast(String(e.message || e)));
});

// A backgrounded page gets no animation frames, so xterm can come back to the foreground showing a
// stale screen. Force a repaint and re-fit when the phone is unlocked or the app is switched back to.
// And a hidden page lets go of its tile: while the phone is attached the desk mirrors that tile at
// the phone's size (auto), so a phone put in a pocket mid-scroll would leave the desk showing a
// 60-column window — 600 rows tall if the turbo was on. On hiding: the turbo off, the usual size
// sent, then detach; on showing: attach again (the server replays the screen) and re-fit.
document.addEventListener('visibilitychange', () => {
  if (!term || current() !== 's-term' || !termId) return;
  if (document.hidden) {
    if (stitchTurbo) { stitchTurbo = false; stitchTopHitAt = 0; sizeTerminal(); }
    sendRaw({ t: 'detach' });
    return;
  }
  sendAttach(termId);
  lastSent = { cols: 0, rows: 0, at: 0 };                  // the size goes again: the desk may have changed it meanwhile
  requestAnimationFrame(() => { fitTerm(); forceRedraw(); });
});
window.addEventListener('pageshow', () => {
  if (current() === 's-term' && term) requestAnimationFrame(() => { fitTerm(); try { term.refresh(0, term.rows - 1); } catch { /* disposed */ } });
});

// ---------------------------------------------------------------- go
history.replaceState({ screen: 's-list' }, '');
connect();
setInterval(() => { state.now = Date.now(); if (current() === 's-list') renderList(); }, 10000);
if ('serviceWorker' in navigator) navigator.serviceWorker.register('/sw.js').catch(() => { /* http:// or blocked; the app still runs */ });
