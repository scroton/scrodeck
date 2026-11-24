'use strict';
/* global deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- the ask pane (2026-11-10)
// Claude sessions ask the user questions on ask pages (the ask-page skill: one self-contained page per round).
// main.js reads every session's pages from the skill's state folder (lib/asks.js) and sends them as meta.asks
// (waiting) and meta.askLog (the last 15 answered); this file draws them:
//   - the ASKS TILE, at the top of the sidebar: every session's waiting asks, one row each, sessions outside the
//     deck included, and a "recently answered" row. A click opens the pane with every session's asks at once,
//     scrolled to the row clicked (11-10: "as if it was clicked and hit f6"); F6 or F7 closes it;
//   - the PANE over the terminal area ("it should pop out like f6 does now on windows, never take the tile"):
//     one section per session, its header sticky until the next session's header reaches it, the session's
//     pages framed under it, then the recently answered ones. F7 (Cmd+Shift+A) or a session tile's
//     right-click shows just that session's: its waiting ask, or, until it asks again, its last answers;
//   - MARKS on a session's tile and pane name: questions waiting (hollow), answers in progress (filled), sent
//     and not yet read (a tick).
// Settings -> Asks: showTile, showMarks, closeOnDone (the rest are the sessions', written to deck.json by main).
// The pages are the skill's own, framed with ?embed=deck — one UI, no copy of its cards here — and talk to this
// file over postMessage (the skill's templates/page.html, "the host bridge"). Drafts live in main, so every deck
// window and the phone show the same answers in progress. Done collects each page's answers; main saves each page's
// answers file next to the page and types ONE line naming it into the session as the user's next message (lib/asks.js,
// main.js doneAsk). Sent and answered pages show read-only.
state.asks = [];                 // meta.asks: [{ sessionId, name, account, rounds: [...], total, drafted, sent, latest }]
state.askLog = [];               // meta.askLog: [{ id, sessionId, name, account, title, round, at, how, lines, url }], newest first
const askFrames = new Map();     // page id -> a waiting page's frame { id, sid, kind: 'ask', frame, wrap, note, ready, legacy, prog, waiters }
const logFrames = new Map();     // page id -> an answered page's read-only frame { ..., kind: 'log' }
const askSections = new Map();   // session id -> { el, head, body }
let askPaneMode = null;          // null = closed, 'all', a session id, or 'log:<page id>' (one answered page)
let asksFocused = false;         // the asks tile is the selection: F6 answers
let asksPick = null;             // the row picked in the asks tile (a session id, or RECENT), for F6 to scroll to
const RECENT = '__recent';
const recentOpen = new Set();    // answered pages expanded in the recent list
const askMovedSeen = new Set();  // '<new page><old page>' already flashed: "a newer page replaced …, your answers came over"
let askReq = 0;
const askKh = () => (state.platform && state.platform.keyHelp) || {};
const ASK_DEFAULTS = { showTile: true, showMarks: true, closeOnDone: true };
const askCfg = () => Object.assign({}, ASK_DEFAULTS, (state.settings && state.settings.asks) || {});

const askSession = sid => state.asks.find(s => s.sessionId === sid) || null;
const askTileOf = sid => state.tiles.find(t => t.kind === 'claude' && t.sessionId === sid) || null;
const askOpenCount = s => s.rounds.reduce((n, r) => n + Math.max(0, r.total - r.answered), 0);
const askFrameProg = id => { const f = askFrames.get(id); return f && f.prog ? f.prog : null; };
const askDrafted = s => s.rounds.reduce((n, r) => n + (r.sent ? 0 : Math.max(r.drafted || 0, (askFrameProg(r.id) || {}).drafted || 0)), 0);
const askUnsent = s => s.rounds.some(r => !r.sent);
const askLastLog = sid => state.askLog.find(e => e.sessionId === sid) || null;
function askName(s) { const t = askTileOf(s.sessionId); return t ? tileTitle(t) : (s.title || s.name); }
function askAbbr(s) { const t = askTileOf(s.sessionId); const a = t ? acct(t.account) : null; return a && a.abbr ? a.abbr : ''; }
const askTime = ms => { const d = new Date(ms); return isNaN(d) ? '' : d.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) + (new Date().toDateString() === d.toDateString() ? '' : ' ' + d.toLocaleDateString([], { month: 'short', day: 'numeric' })); };

// ---- the mark, on a session's tile (30-tiles.js) and its pane name (20-terminals.js)
function askMarkInfo(sessionId) {
  const s = sessionId ? askSession(sessionId) : null;
  if (!s) return null;
  const open = askOpenCount(s), drafted = askDrafted(s);
  const kind = !askUnsent(s) ? 'sent' : drafted ? 'drafted' : 'waiting';
  const key = askKh().asks || 'F7';
  const title = kind === 'sent' ? 'answers sent; the session has not read them yet'
    : kind === 'drafted' ? `${drafted} of ${open} answered on the page, not sent yet (${key})`
      : `${open} question${open === 1 ? '' : 's'} waiting on an ask page (${key})`;
  return { kind, text: kind === 'sent' ? '✓' : String(open), title };
}
function paintAskMark(el, sessionId) {
  if (!el) return '';
  const m = askMarkInfo(sessionId);
  el.hidden = !m;
  if (!m) return '';
  el.classList.toggle('drafted', m.kind === 'drafted');
  el.classList.toggle('sent', m.kind === 'sent');
  if (el.textContent !== m.text) el.textContent = m.text;
  if (el.title !== m.title) el.title = m.title;
  return `${m.kind}${m.text}`;
}

// ---- the asks tile
const asksTile = document.createElement('div');
asksTile.id = 'asks-tile'; asksTile.className = 'tile asks-tile';
asksTile.innerHTML = '<div class="t-top"><span class="at-glyph">?</span><div class="t-title">Asks</div><span class="at-count"></span></div><div class="at-rows"></div>';
sidebar.insertBefore(asksTile, tilesEl);
let asksTileSig = '';
function paintAsksTile() {
  const cfg = askCfg();
  asksTile.hidden = cfg.showTile === false;
  document.body.classList.toggle('no-ask-marks', cfg.showMarks === false);
  if (asksTile.hidden && asksFocused) asksBlur();
  const rows = asksTile.querySelector('.at-rows');
  const n = state.asks.length;
  const sig = JSON.stringify([asksFocused, asksPick, state.asks.map(s => [s.sessionId, askName(s), askAbbr(s), askOpenCount(s), askDrafted(s), askUnsent(s), s.latest, !!s.ended]),
    state.askLog.length, state.askLog[0] && state.askLog[0].at]) + Math.floor(state.now / 60000);
  asksTile.classList.toggle('active', asksFocused);
  asksTile.classList.toggle('none', !n);
  // 11-10: the yellow the deck uses when a Claude asks something, pulsing while a question waits on them
  asksTile.classList.toggle('asking', state.asks.some(askUnsent));
  if (sig === asksTileSig) return;
  asksTileSig = sig;
  asksTile.querySelector('.at-count').textContent = n ? `${n} waiting` : 'nothing waiting';
  asksTile.title = `Questions from Claude sessions on ask pages. Click to answer them (a row: that session first); ${askKh().zoom || 'F6'} or ${askKh().asks || 'F7'} closes. ${askKh().asks || 'F7'} on a session shows just its own.`;
  rows.textContent = '';
  for (const s of state.asks) {
    const row = document.createElement('div'); row.className = 'at-row' + (asksFocused && asksPick === s.sessionId ? ' picked' : ''); row.dataset.sid = s.sessionId;
    const mark = document.createElement('span'); mark.className = 't-ask'; paintAskMark(mark, s.sessionId);
    const ab = document.createElement('span'); ab.className = 'at-abbr'; ab.textContent = askAbbr(s);
    const name = document.createElement('span'); name.className = 'at-name'; name.textContent = askName(s);
    const where = document.createElement('span'); where.className = 'at-where'; where.textContent = s.ended ? 'session ended' : askTileOf(s.sessionId) ? '' : 'outside the deck';
    row.classList.toggle('ended', !!s.ended);
    const age = document.createElement('span'); age.className = 'at-age'; age.textContent = s.latest ? fmtAge(state.now - Date.parse(s.latest)) : '';
    row.append(mark, ab, name, where, age);
    row.title = `${s.name} · ${s.account}` + (s.ended ? ' · its session has ended: Done saves the answers file, read when the session is resumed or another takes the page over'
      : askTileOf(s.sessionId) ? '' : ' · runs outside the deck: Done saves its answers file');
    rows.appendChild(row);
  }
  if (state.askLog.length) {
    const row = document.createElement('div'); row.className = 'at-row at-recent' + (asksFocused && asksPick === RECENT ? ' picked' : ''); row.dataset.sid = RECENT;
    const l = state.askLog[0];
    row.append(Object.assign(document.createElement('span'), { className: 'at-name', textContent: `Recently answered (${state.askLog.length})` }),
      Object.assign(document.createElement('span'), { className: 'at-age', textContent: fmtAge(state.now - l.at) }));
    row.title = 'The last pages you answered, every session, read-only';
    rows.appendChild(row);
  }
}
function asksSelect(sid) {
  asksFocused = true; asksPick = sid || null;
  document.body.classList.add('asks-focus');
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
  paintAsksTile();
}
// any session selected (a click, F11 / F12, the palette): the asks tile is no longer the selection
function asksBlur() { if (!asksFocused) return; asksFocused = false; document.body.classList.remove('asks-focus'); paintAsksTile(); }
// a click is the tile selected and F6 pressed: every session's asks, open at once (a row: scrolled to it). The
// same click on the tile while that view is open closes it, as F6 does; a row click just scrolls to the row.
asksTile.addEventListener('click', ev => {
  ev.stopPropagation();
  const row = ev.target.closest('.at-row');
  if (!row && askPaneMode === 'all' && askOpen()) { closeAskPane(); return; }
  openAskPane('all', row ? row.dataset.sid : null);
});

// ---- the pane
const askPane = document.createElement('div');
askPane.id = 'ask-pane'; askPane.hidden = true; askPane.tabIndex = -1;
askPane.innerHTML = '<div class="ap-bar"><span class="ap-title"></span><span class="ap-hint"></span><button type="button" class="ap-close" title="Close">×</button></div>'
  + '<div class="ap-scroll"><div class="ap-list"></div><div class="ap-empty" hidden></div></div>';
mainEl.appendChild(askPane);
const askList = askPane.querySelector('.ap-list'), askScroll = askPane.querySelector('.ap-scroll');
askPane.querySelector('.ap-close').addEventListener('click', () => closeAskPane());
askPane.addEventListener('keydown', ev => { if (ev.key === 'Escape') { ev.preventDefault(); closeAskPane(); } });
// the recently answered pages: a section of its own, after every session's (CSS order)
const recentSec = document.createElement('section'); recentSec.className = 'ap-sess ap-recent'; recentSec.style.order = '100000';
recentSec.innerHTML = '<div class="ap-head ap-rhead"><div class="ap-who"><span class="ap-name">Recently answered</span><span class="ap-meta"></span></div></div><div class="ap-body ap-lrows"></div>';
askList.appendChild(recentSec);

const askOpen = () => !!askPaneMode && !askPane.hidden;
const logModeId = () => (askPaneMode && askPaneMode.startsWith('log:') ? askPaneMode.slice(4) : null);
function openAskPane(mode, scrollTo) {
  if (!mode) return;
  if (mode !== 'all' && !mode.startsWith('log:') && !askSession(mode)) { paneNote('No questions from this session.'); return; }
  askPaneMode = mode;
  askPane.hidden = false;
  mainEl.classList.add('asking');
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();   // never typing into a hidden terminal
  if (mode.startsWith('log:')) recentOpen.add(mode.slice(4));
  paintAskPane();
  // every frame gets the latest drafts (another window or the phone may have moved them on)
  for (const fr of askFrames.values()) if (fr.ready && askVisible(fr.sid)) askLoad(fr);
  requestAnimationFrame(() => {
    const target = scrollTo === RECENT ? recentSec : scrollTo && askSections.get(scrollTo) ? askSections.get(scrollTo).el : null;
    askScroll.scrollTop = target && mode === 'all' ? target.offsetTop : 0;
    askPane.focus({ preventScroll: true });
  });
}
function closeAskPane(opts = {}) {
  if (!askPaneMode) return;
  const wasLog = logModeId();
  askPaneMode = null;
  askPane.hidden = true;
  mainEl.classList.remove('asking');
  if (wasLog) recentOpen.delete(wasLog);
  for (const sid of askSections.keys()) askConfirmOff(sid);
  if (opts.toTile && terms.has(opts.toTile)) activate(opts.toTile, { adopt: false });
  else { const r = state.active && terms.get(state.active); if (r && !asksFocused) r.term.focus(); }
}
const askVisible = sid => askPaneMode === 'all' || askPaneMode === sid;
// a keystroke must never go to a terminal hidden behind the pane: a terminal that takes the focus gives it back
document.addEventListener('focusin', ev => {
  if (askOpen() && ev.target && ev.target.closest && ev.target.closest('.term') && !ev.target.closest('#ask-pane')) { ev.target.blur(); askPane.focus({ preventScroll: true }); }
});

function askSrc(id, url) {
  const q = 'embed=deck&theme=dark';
  if (canWebview && url) return `${url}?${q}`;                              // Electron: the page file itself
  return `/ask/${encodeURIComponent(id)}.html?${q}`;                          // the browser desk: lib/phone.js serves it
}
function askSection(s) {
  let sec = askSections.get(s.sessionId);
  if (sec) return sec;
  const el = document.createElement('section'); el.className = 'ap-sess'; el.dataset.sid = s.sessionId;
  const head = document.createElement('div'); head.className = 'ap-head';
  head.innerHTML = '<div class="ap-who"><span class="ap-mark t-ask"></span><span class="ap-abbr"></span><span class="ap-name"></span><span class="ap-meta"></span></div>'
    + '<div class="ap-row"><button type="button" class="ap-done primary">Done</button><button type="button" class="ap-copy">Copy all</button>'
    + '<button type="button" class="ap-term"></button><span class="ap-count"></span><span class="ap-flash" aria-live="polite"></span>'
    + '<button type="button" class="ap-discard">Discard answers…</button></div>'
    + '<div class="ap-confirm" hidden><span class="ap-confirm-text"></span><button type="button" class="ap-keep primary">Keep them</button><button type="button" class="ap-discard-yes danger">Discard</button></div>';
  const body = document.createElement('div'); body.className = 'ap-body';
  el.append(head, body);
  const sid = s.sessionId;
  head.querySelector('.ap-done').addEventListener('click', () => askDone(sid));
  head.querySelector('.ap-copy').addEventListener('click', () => askCopy(sid));
  head.querySelector('.ap-term').addEventListener('click', () => askToTerminal(sid));
  head.querySelector('.ap-discard').addEventListener('click', () => askConfirmOn(sid));
  head.querySelector('.ap-keep').addEventListener('click', () => askConfirmOff(sid));
  head.querySelector('.ap-discard-yes').addEventListener('click', ev => { if (ev.currentTarget.disabled) return; askDiscard(sid); });
  askList.appendChild(el);
  sec = { el, head, body };
  askSections.set(sid, sec);
  return sec;
}
// one framed page: a waiting one (kind 'ask') or an answered one, read-only (kind 'log')
function makeFrame(map, container, kind, id, sid, url, label) {
  let fr = map.get(id);
  if (fr) return fr;
  const wrap = document.createElement('div'); wrap.className = 'ap-round'; wrap.dataset.id = id;
  const note = document.createElement('div'); note.className = 'ap-rnote'; note.hidden = true;
  const frame = document.createElement('iframe');
  frame.className = 'ap-frame'; frame.title = label;
  frame.setAttribute('allow', 'clipboard-read; clipboard-write');
  frame.src = askSrc(id, url);
  wrap.append(note, frame);
  container.appendChild(wrap);
  fr = { id, sid, kind, frame, wrap, note, ready: false, legacy: false, prog: null, waiters: new Map() };
  map.set(id, fr);
  // A page made before the skill learned the host bridge never says "ready". A waiting one shows its own bar,
  // and its own Copy all and Done work as in a browser (main saves its Done file where the skill looks); an
  // answered one cannot be shown read-only, so its answer lines show instead.
  frame.addEventListener('load', () => setTimeout(() => {
    if (fr.ready || map.get(id) !== fr) return;
    fr.legacy = true;
    if (kind === 'log') { frame.remove(); paintAskPane(); return; }
    frame.style.height = '75vh'; paintAskPane();
  }, 1500));
  return fr;
}
function dropFrame(map, id) { const fr = map.get(id); if (!fr) return; for (const w of fr.waiters.values()) w(null); fr.wrap.remove(); map.delete(id); }

function paintAskPane() {
  const bar = askPane.querySelector('.ap-title'), hint = askPane.querySelector('.ap-hint');
  const live = new Set(state.asks.map(s => s.sessionId));
  // a session whose pages all went (it read its answers): its section goes. Showing only it, the pane shows its
  // answers read-only instead (F7 until it asks again), or closes when they are not in the log.
  for (const [sid, sec] of askSections) if (!live.has(sid)) {
    for (const [id, fr] of askFrames) if (fr.sid === sid) dropFrame(askFrames, id);
    sec.el.remove(); askSections.delete(sid);
  }
  if (askPaneMode && askPaneMode !== 'all' && !logModeId() && !live.has(askPaneMode)) {
    const gone = askPaneMode, last = askLastLog(gone);
    // its page was pushed out by another session's newer one: follow it there (their answers came over with it)
    const heir = state.asks.find(x => x.rounds.some(r => (r.replaces || []).some(p => p.sessionId === gone)));
    if (heir) {
      const p = heir.rounds.flatMap(r => r.replaces || []).filter(q => q.sessionId === gone);
      const n = p.reduce((k, q) => k + (q.moved || 0), 0);
      askPaneMode = heir.sessionId;
      paneNote(`That page was replaced by ${askName(heir)}'s newer one${n ? `; your ${n} answer${n === 1 ? '' : 's'} came over` : ''}.`, 6000);
    } else if (last) askPaneMode = `log:${last.id}`;
    else { closeAskPane(); const t = askTileOf(gone); paneNote(`${t ? tileTitle(t) : 'The session'} has read its answers.`); return; }
  }
  if (!askOpen()) return;
  const lid = logModeId();
  state.asks.forEach((s, i) => {
    const sec = askSection(s);
    sec.el.style.order = String(i);               // reorder by CSS, never by moving the node: a moved iframe reloads
    const vis = askVisible(s.sessionId);
    sec.el.hidden = !vis;
    const want = new Set(s.rounds.map(r => r.id));
    for (const [id, fr] of askFrames) if (fr.sid === s.sessionId && !want.has(id)) dropFrame(askFrames, id);
    if (vis) s.rounds.forEach((r, k) => {
      const fr = makeFrame(askFrames, sec.body, 'ask', r.id, s.sessionId, r.url, `${s.name} round ${r.round}`);
      fr.wrap.style.order = String(k);
      if (r.sent && fr.ready && !fr.locked) { fr.locked = true; askPost(fr, { askpage: 'lock' }); askEnsureLocked(fr); }
      const several = s.rounds.length > 1, picker = r.source === 'picker';
      const parts = [];
      if (fr.legacy) parts.push('Made by an older ask-page skill, so Done above cannot read it: use the page\'s own Copy all answers (then paste in the terminal) or its Done (saves the answers file the session watches for).');
      if (several) parts.push(`Round ${r.round}` + (r.title ? ` · ${r.title}` : ''));
      for (const p of r.replaces || []) {
        const whose = p.sessionId && p.sessionId !== s.sessionId ? `${p.name || 'another session'}'s round ${p.round == null ? '?' : p.round}` : `round ${p.round == null ? '?' : p.round}`;
        parts.push(`Replaces ${whose}` + (p.moved ? `: your ${p.moved} answer${p.moved === 1 ? '' : 's'} there came over` : ''));
        if (p.moved && !askMovedSeen.has(`${r.id}<${p.id}`)) { askMovedSeen.add(`${r.id}<${p.id}`); askFlash(s.sessionId, `A newer page replaced ${whose}; your ${p.moved} answer${p.moved === 1 ? '' : 's'} came over.`, 8000); }
      }
      if (s.ended && k === 0) parts.push('Its session has ended: Done still saves the answers file, but nothing reads it until the session is resumed or another session takes this page over');
      if (r.sent) parts.push(`Sent ${askTime(r.sent.at)}${r.sent.how === 'file' ? ' as its answers file' : ''}: read-only, waiting for the session to read it`);
      else if (picker) parts.push('From Claude Code\'s picker: it is also open in the session\'s terminal, and Done presses Esc there first');
      else if (r.background) parts.push('Also asked in the session\'s terminal: answer either place');
      const txt = parts.join(' · ');
      fr.note.hidden = !txt; if (fr.note.textContent !== txt) fr.note.textContent = txt;
    });
    paintAskHead(s.sessionId);
  });
  paintRecent(lid);
  const empty = askPane.querySelector('.ap-empty');
  empty.hidden = !!state.asks.length || !!lid || state.askLog.length > 0;
  empty.textContent = 'No questions waiting. Sessions that ask on a page show up here and in the asks tile.';
  if (askPaneMode === 'all') bar.textContent = state.asks.length ? `Asks · ${state.asks.length} session${state.asks.length === 1 ? '' : 's'} waiting` : 'Asks · nothing waiting';
  else if (lid) { const e = state.askLog.find(x => x.id === lid); bar.textContent = e ? `${askLogName(e)} · answered ${askTime(e.at)} · read-only` : 'Answered'; }
  else { const s = askSession(askPaneMode); bar.textContent = s ? `${askName(s)} asks` : 'Asks'; }
  const key = askPaneMode === 'all' ? `${askKh().zoom || 'F6'} or ${askKh().asks || 'F7'}` : (askKh().asks || 'F7');
  hint.textContent = lid ? `${key} closes · it shows here until the session asks again` : `${key} closes · Done sends that session its answers`;
}
function askLogName(e) { const t = askTileOf(e.sessionId); return t ? tileTitle(t) : (e.title || e.name); }
// the recently answered list: every one in the every-session view, one in the log view; a row opens read-only
function paintRecent(lid) {
  const show = askPaneMode === 'all' ? state.askLog.length > 0 : !!lid;
  recentSec.hidden = !show;
  const head = recentSec.querySelector('.ap-rhead');
  head.hidden = !!lid;                                   // the log view: the bar says what it is
  recentSec.querySelector('.ap-meta').textContent = `the last ${state.askLog.length} you answered, every session · read-only`;
  const list = lid ? state.askLog.filter(e => e.id === lid) : (show ? state.askLog : []);
  const rowsEl = recentSec.querySelector('.ap-lrows');
  const want = new Set(list.map(e => e.id));
  for (const el of [...rowsEl.children]) if (!want.has(el.dataset.id)) { dropFrame(logFrames, el.dataset.id); el.remove(); }
  list.forEach((e, i) => {
    let row = rowsEl.querySelector(`.ap-lrow[data-id="${CSS.escape(e.id)}"]`);
    if (!row) {
      row = document.createElement('div'); row.className = 'ap-lrow'; row.dataset.id = e.id;
      row.innerHTML = '<div class="ap-lhead"><span class="ap-lchev"></span><span class="ap-abbr"></span><span class="ap-name"></span><span class="ap-meta"></span></div><div class="ap-lbody"></div>';
      row.querySelector('.ap-lhead').addEventListener('click', () => { if (logModeId()) return; if (recentOpen.has(e.id)) recentOpen.delete(e.id); else recentOpen.add(e.id); paintAskPane(); });
      rowsEl.appendChild(row);
    }
    row.style.order = String(i);
    const t = askTileOf(e.sessionId), a = t ? acct(t.account) : null;
    row.querySelector('.ap-abbr').textContent = a && a.abbr ? a.abbr : '';
    row.querySelector('.ap-name').textContent = askLogName(e);
    const rb = e.replacedBy;
    row.querySelector('.ap-meta').textContent = [e.name, `round ${e.round}`, `${e.lines.length} answer${e.lines.length === 1 ? '' : 's'}`, `${e.how} ${askTime(e.at)}`,
      rb && `replaced by ${rb.sessionId && rb.sessionId !== e.sessionId ? `${rb.name || 'another session'}'s ` : ''}round ${rb.round == null ? '?' : rb.round}`].filter(Boolean).join(' · ');
    const open = !!lid || recentOpen.has(e.id);
    row.classList.toggle('open', open);
    const body = row.querySelector('.ap-lbody');
    body.hidden = !open;
    if (!open) { dropFrame(logFrames, e.id); return; }
    const fr = e.url || !canWebview ? makeFrame(logFrames, body, 'log', e.id, e.sessionId, e.url, `${e.name} round ${e.round}, answered`) : null;
    // no page any more, or one from before the bridge: the answer lines, as they were sent
    let pre = body.querySelector('.ap-lines');
    if (!fr || fr.legacy) {
      if (!pre) { pre = document.createElement('pre'); pre.className = 'ap-lines'; body.appendChild(pre); }
      pre.textContent = e.lines.join('\n') || '(no answers)';
    } else if (pre) pre.remove();
  });
}
function paintAskHead(sid) {
  const s = askSession(sid), sec = askSections.get(sid);
  if (!s || !sec) return;
  const h = sec.head, t = askTileOf(sid);
  paintAskMark(h.querySelector('.ap-mark'), sid);
  h.querySelector('.ap-abbr').textContent = askAbbr(s);
  h.querySelector('.ap-name').textContent = askName(s);
  const last = s.rounds[s.rounds.length - 1];
  const asked = last && last.created ? askTime(last.created) : '';
  h.querySelector('.ap-meta').textContent = [s.name, s.account, s.rounds.length > 1 ? `${s.rounds.length} pages` : `round ${last ? last.round : '?'}`, asked && `asked ${asked}`, s.ended ? 'session ended' : !t && 'outside the deck'].filter(Boolean).join(' · ');
  let total = 0, answered = 0, later = 0;
  for (const r of s.rounds) { const p = askFrameProg(r.id); total += r.total; answered += p ? p.answered : r.answered; later += p ? p.later : r.later; }
  const unsent = askUnsent(s);
  h.querySelector('.ap-count').textContent = `${answered} of ${total} answered` + (later ? `, ${later} later` : '') + (unsent ? '' : ' · sent');
  const done = h.querySelector('.ap-done');
  done.disabled = !unsent;
  done.textContent = unsent ? 'Done' : 'Sent';
  done.title = !unsent ? 'Sent; it shows read-only until the session reads it' : s.ended ? 'Its session has ended: save the answers file next to the page; it is read when the session is resumed' : t ? 'Save the answers file next to the page and tell the session where it is, in one line (after its turn, if it is busy)' : 'Save the answers file next to the page; the session picks it up';
  const term = h.querySelector('.ap-term');
  term.textContent = t ? 'Answer in the terminal' : 'Answer in its terminal';
  term.title = t ? `Close this and go to the session (${askKh().asks || 'F7'} brings it back)` : 'It runs outside the deck: close this and answer it there';
  h.querySelector('.ap-discard').disabled = !askDrafted(s);
}
function askFlash(sid, text, ms = 5000) {
  const sec = askSections.get(sid); if (!sec) return;
  const f = sec.head.querySelector('.ap-flash'); f.textContent = text;
  clearTimeout(f._t); f._t = setTimeout(() => { f.textContent = ''; }, ms);
}

// ---- the bridge to the framed pages (the skill's page.html, "the host bridge")
const askPost = (fr, m) => { try { fr.frame.contentWindow.postMessage(Object.assign({ id: fr.id }, m), '*'); } catch { /* reloading */ } };
async function askLoad(fr) {
  if (fr.kind === 'log') {
    let st = null; try { st = await deck.askLogState(fr.id); } catch { st = null; }
    askPost(fr, { askpage: 'load', state: st || {}, readonly: true });
    askEnsureLocked(fr);
    return;
  }
  const r = (askSession(fr.sid) || { rounds: [] }).rounds.find(x => x.id === fr.id);
  let st = null; try { st = await deck.askDraftGet(fr.id); } catch { st = null; }
  askPost(fr, { askpage: 'load', state: st || {}, readonly: !!(r && r.sent) });   // main's copy is the one every view shares
  if (r && r.sent) { fr.locked = true; askEnsureLocked(fr); }
}
// A page built before the skill's read-only lock still takes clicks: one that does not say it is locked gets a
// cover over its frame, so what was sent stays as it was sent
function askEnsureLocked(fr) { askGet(fr).then(m => { if (!m || m.locked !== true) fr.wrap.classList.add('blocked'); }); }
function askGet(fr, ms = 3000) {
  return new Promise(res => {
    const req = ++askReq;
    const timer = setTimeout(() => { fr.waiters.delete(req); res(null); }, ms);
    fr.waiters.set(req, m => { clearTimeout(timer); res(m); });
    askPost(fr, { askpage: 'get', req });
  });
}
window.addEventListener('message', ev => {
  const m = ev.data;
  if (!m || typeof m !== 'object' || typeof m.askpage !== 'string') return;
  let fr = null;
  for (const map of [askFrames, logFrames]) for (const x of map.values()) if (x.frame.contentWindow === ev.source) { fr = x; break; }
  if (!fr || m.id !== fr.id) return;
  const prog = () => ({ drafted: +m.drafted || 0, answered: +m.answered || 0, later: +m.later || 0, total: +m.total || 0 });
  if (m.askpage === 'ready') { fr.ready = true; if (fr.kind === 'ask') fr.prog = prog(); if (m.h) fr.frame.style.height = `${m.h}px`; askLoad(fr); }
  else if (m.askpage === 'change') {
    if (fr.kind !== 'ask' || fr.locked) return;
    fr.prog = prog();
    deck.askDraft(fr.id, { state: m.state || {}, drafted: fr.prog.drafted, answered: fr.prog.answered, later: fr.prog.later, total: fr.prog.total });
    paintAskHead(fr.sid); asksTileSig = ''; paintAsksTile(); renderTiles();
  } else if (m.askpage === 'height') { if (m.h > 0) fr.frame.style.height = `${Math.ceil(m.h)}px`; }
  else if (m.askpage === 'answers') { const w = fr.waiters.get(m.req); if (w) { fr.waiters.delete(m.req); w(m); } }
});

// ---- the buttons
async function askCollect(sid) {
  const s = askSession(sid); if (!s) return null;
  const pages = [];
  let old = 0;
  for (const r of s.rounds) {
    if (r.sent) continue;                             // already sent: read-only until the session reads it
    const fr = askFrames.get(r.id);
    if (fr && fr.legacy) { old++; continue; }        // an older page: its own buttons answer it
    if (!fr || !fr.ready) { askFlash(sid, 'The page is still loading; try again in a moment.'); return null; }
    const m = await askGet(fr);
    if (!m || !m.answers) { askFlash(sid, 'The page did not answer; try again.'); return null; }
    pages.push({ id: r.id, answers: m.answers });
  }
  if (!pages.length && old) { askFlash(sid, 'This page was made by an older ask-page skill: use its own Copy all answers or Done.', 8000); return null; }
  return pages;
}
// the Copy-all text (lib/asks.js composeDone): each page's Copy-all lines, headed by its page id. Done no longer types
// it (it saves the answers file and names it in one line); it is Copy's text and Done's "anything answered?" check.
function askText(pages) {
  return pages.filter(p => (p.answers.lines || []).some(l => String(l).trim())).map(p => [p.id, ...p.answers.lines].join('\n')).join('\n');
}
async function askDone(sid) {
  const pages = await askCollect(sid); if (!pages) return;
  if (!pages.length || !askText(pages).trim()) { askFlash(sid, 'Nothing answered yet: pick an option or a reply, or type in a box.'); return; }
  const sec = askSections.get(sid), btn = sec && sec.head.querySelector('.ap-done');
  if (btn) btn.disabled = true;
  try {
    const res = await deck.askDone(sid, pages);
    for (const p of pages) { const fr = askFrames.get(p.id); if (fr) { fr.locked = true; askPost(fr, { askpage: 'lock' }); askEnsureLocked(fr); } }
    const t = askTileOf(sid), s = askSession(sid);
    let msg;
    // every Done saves the answers file next to the page; a session in this deck (res.tileId) also gets ONE line naming it
    if (!res.tileId && s && s.ended) msg = `Saved the answers file for ${s.name}. Its session has ended, so it is read when the session is resumed.`;
    else if (!res.tileId) msg = `Saved the answers file for ${s ? s.name : 'it'} (it runs outside the deck); it picks it up if it is watching.`;
    else if (res.esc) msg = 'Done: saved the answers file, pressed Esc on its picker, then told it where the file is.';
    else if (t && (t.status === 'busy' || t.status === 'waiting' || t.status === 'asleep' || t.status === 'starting')) msg = `Done: saved the answers file; ${tileTitle(t)} gets one line naming it as soon as it is back at its prompt.`;
    else msg = 'Done: saved the answers file, and one line naming it goes in as your next message.';
    // Done closes a session's ask (Settings -> Asks, on by default): F7 shows it again, read-only, until the
    // session asks again. The every-session view stays open while another session still waits.
    const others = state.asks.some(x => x.sessionId !== sid && askUnsent(x));
    const close = askCfg().closeOnDone !== false && (askPaneMode === sid || (askPaneMode === 'all' && !others));
    if (close) { closeAskPane({ toTile: t && res.tileId && askPaneMode === sid ? t.id : null }); paneNote(msg, 5000); }
    else askFlash(sid, msg, 8000);
  } catch (e) {
    askFlash(sid, String((e && e.message) || e).replace(/^Error invoking remote method '[^']+': (Error: )?/, ''), 8000);
    if (btn) btn.disabled = false;
  }
}
async function askCopy(sid) {
  const pages = await askCollect(sid); if (!pages) return;
  const text = askText(pages);
  if (!text.trim()) { askFlash(sid, 'Nothing answered yet.'); return; }
  await deck.writeClipboard(text);
  const n = text.split('\n').length;
  askFlash(sid, `Copied ${n} line${n === 1 ? '' : 's'}: paste them into the session's terminal.`);
}
function askToTerminal(sid) { const t = askTileOf(sid); closeAskPane({ toTile: t ? t.id : null }); }
function askConfirmOn(sid) {
  const sec = askSections.get(sid), s = askSession(sid); if (!sec || !s) return;
  const n = askDrafted(s); if (!n) return;
  sec.head.querySelector('.ap-row').hidden = true;
  const c = sec.head.querySelector('.ap-confirm'); c.hidden = false;
  c.querySelector('.ap-confirm-text').textContent = `Discard the ${n} answer${n === 1 ? '' : 's'} in progress for ${askName(s)}? The questions stay; you answer them in the terminal.`;
  // hard to misclick (the user): the safe choice is where Done was, and Discard only arms after a moment
  const yes = c.querySelector('.ap-discard-yes'); yes.disabled = true; setTimeout(() => { yes.disabled = false; }, 700);
  c.querySelector('.ap-keep').focus();
}
function askConfirmOff(sid) {
  const sec = askSections.get(sid); if (!sec) return;
  sec.head.querySelector('.ap-row').hidden = false; sec.head.querySelector('.ap-confirm').hidden = true;
}
async function askDiscard(sid) {
  const s = askSession(sid); if (!s) return;
  for (const r of s.rounds) { if (r.sent) continue; const fr = askFrames.get(r.id); if (fr) askPost(fr, { askpage: 'clear' }); try { await deck.askClear(r.id); } catch { /* gone */ } }
  askConfirmOff(sid);
  askToTerminal(sid);                 // the same as opening it, entering nothing, and answering in the terminal
}

// ---- keys: F6 on the asks tile answers (25-layout.js asks first), F7 / Cmd+Shift+A one session's
function asksZoom() {
  if (askPaneMode === 'all') { closeAskPane(); return true; }
  if (asksFocused) { openAskPane('all', asksPick); return true; }
  return false;
}
// a session's own: its waiting ask; once that is answered, its last answers read-only, until it asks again
function askOpenFor(t) {
  if (!t || t.kind !== 'claude') return false;
  if (askSession(t.sessionId)) { openAskPane(t.sessionId); return true; }
  const last = askLastLog(t.sessionId);
  if (last) { openAskPane(`log:${last.id}`); return true; }
  return false;
}
function asksKey() {
  if (askOpen()) { closeAskPane(); return; }
  if (asksFocused) { openAskPane('all', asksPick); return; }
  const t = state.tiles.find(x => x.id === state.active);
  if (askOpenFor(t)) return;
  const n = state.asks.length;
  paneNote(n ? `This session has no questions waiting. ${n} other session${n === 1 ? ' has' : 's have'}: the asks tile at the top of the sidebar.` : 'No questions waiting.');
}
if (typeof deck.onAsks === 'function') deck.onAsks(() => asksKey());
// a new-ask toast was clicked (main.js notifyNewAsks): that session's tile, then its questions
if (typeof deck.onAskOpen === 'function') deck.onAskOpen(m => {
  const sid = m && m.sessionId, t = m && m.tileId ? state.tiles.find(x => x.id === m.tileId) : null;
  if (t) activate(t.id, { adopt: false });
  openAskPane(sid && askSession(sid) ? sid : 'all');
});   // an older main (before its restart) has no key; the tile still works
// a session tile's right-click (35-groups.js): its questions, or its last answers
function askMenuItem(t) {
  if (!t || t.kind !== 'claude') return null;
  const key = askKh().asks || 'F7', s = askSession(t.sessionId);
  if (s) { const n = askOpenCount(s); return { label: `Answer its question${n === 1 ? '' : 's'} (${n})  ${key}`, run: () => { activate(t.id, { adopt: false }); openAskPane(t.sessionId); } }; }
  const last = askLastLog(t.sessionId);
  if (last) return { label: `Its last answers (read-only)  ${key}`, run: () => { activate(t.id, { adopt: false }); openAskPane(`log:${last.id}`); } };
  return null;
}

// called at the end of renderTiles (every meta tick)
function paintAsks() {
  paintAsksTile();
  if (askOpen() || askSections.size) paintAskPane();
}
