// mac/Sources/ClaudeDeck/AskHostPage.swift — the ask pane's HOST DOCUMENT: the page the pane's WKWebView loads
// (deckask://pane/host, served from memory by AskSchemeHandler in AskPane.swift). It is renderer/js/49-asks.js's
// pane, PORTED AS JAVASCRIPT, function for function and with the same names (openAskPane, paintAskPane, askSection,
// makeFrame, paintRecent, paintAskHead, askLoad, askGet, askCollect, askDone, askCopy, askDiscard …), on the same
// elements and css rules as renderer/style.css (#ask-pane …), so the layout and the behaviour are the Electron
// pane's by construction. Each ask page is the skill's own, in a SANDBOXED <iframe> (opaque origin), and talks to
// this document over the skill's host bridge (postMessage: ready, change, height, answers / load, get, clear, lock).
//
// HOW THIS DIFFERS FROM THE WINDOWS PANE (windows/src/asks.hpp): there the host document decides nothing and C++
// computes every text; here the document IS the pane's logic, because the logic already exists in JavaScript
// (49-asks.js) and a Swift copy of it would be a third copy to keep. What Swift keeps is what is needed while no
// web view exists (the asks tile, the marks, F7 / Cmd+Shift+A, the tile menu, a toast: AskModel in AskPane.swift),
// the web view's life, the deck calls, the clipboard, links and downloads.
//
// THE WIRE (both directions JSON objects):
//   Swift -> here   window.deckIn(m): {op:'state', ...the deck's asks state}, {op:'open', mode, scrollTo},
//                   {op:'close'}, {op:'reply', rid, ok, value | error}, {op:'focus'}
//   here -> Swift   window.webkit.messageHandlers.deck.postMessage(m): {ev:'ready'}, {ev:'call', rid, fn, args},
//                   {ev:'fire', fn, args}, {ev:'closed', toTile}, {ev:'mode', mode}, {ev:'note', text, ms},
//                   {ev:'clipboard', text}
//   Swift takes a message only from the MAIN frame at deckask://pane (AskPane.userContentController): a framed page
//   is sandboxed and can reach Swift only through this document's relay, which checks it (the message's source is a
//   frame made here, it names that frame's page, its origin is opaque).
//
// Tested on Windows in headless Edge against a fake page that speaks the skill's bridge: mac/Tests/askhost-test.
// The Swift around it is UNBUILT here (mac/README.md); the CI builds it (.github/workflows/mac.yml).
import Foundation

enum AskHostPage {
  /// mac/Tests/askhost-test/run.ps1 reads the text between the `#"""` line and the `"""#` line: keep each on a line of its own.
  static let html = #"""
<!doctype html>
<html><head><meta charset="utf-8"><title>asks</title>
<style>
:root { --bg: #0c0c0c; --tile-border: #2c2c2c; --fg: #ececec; --fg-dim: #9a9a9a; --fg-faint: #6a6a6a; --green: #3ecf5a;
  --btn: #262626; --btn-border: #383838; --danger: #c94a4a; --ask: #6aa9ff; --ask-ink: #0b1220; --ask-dim: #4a6c99; }
* { box-sizing: border-box; }
[hidden] { display: none !important; }
html, body { margin: 0; height: 100%; background: var(--bg); color: var(--fg);
  font-family: -apple-system, BlinkMacSystemFont, "Helvetica Neue", system-ui, sans-serif; font-size: 13px; overflow: hidden; }
button { font: inherit; color: inherit; cursor: pointer; }
.t-ask { flex: 0 0 auto; display: inline-block; min-width: 15px; height: 13px; padding: 0 3px; border: 1px solid var(--ask);
  color: var(--ask); font-size: 10px; line-height: 11px; font-weight: 700; text-align: center; font-variant-numeric: tabular-nums; }
.t-ask::before { content: "?"; margin-right: 1px; opacity: .75; }
.t-ask.drafted { background: var(--ask); color: var(--ask-ink); }
.t-ask.sent { border-color: var(--ask-dim); color: var(--ask-dim); }
.t-ask.sent::before { content: ""; margin: 0; }
#ask-pane { position: absolute; inset: 0; display: flex; flex-direction: column; background: var(--bg); outline: none; }
#ask-pane .ap-bar { flex: 0 0 auto; display: flex; align-items: baseline; gap: 10px; padding: 4px 8px; background: #0f0f0f; border-bottom: 1px solid var(--tile-border); }
#ask-pane .ap-title { font-weight: 600; font-size: 13px; }
#ask-pane .ap-hint { flex: 1 1 auto; color: var(--fg-faint); font-size: 11.5px; }
#ask-pane .ap-close { background: transparent; border: 0; color: var(--fg-dim); font-size: 16px; line-height: 16px; padding: 0 4px; }
#ask-pane .ap-close:hover { color: var(--fg); background: #1f1f1f; }
#ask-pane .ap-scroll { flex: 1 1 auto; overflow-y: auto; overflow-x: hidden; }
#ask-pane .ap-list { display: flex; flex-direction: column; }
#ask-pane .ap-empty { padding: 16px; color: var(--fg-dim); }
#ask-pane .ap-note { position: absolute; left: 50%; bottom: 14px; transform: translateX(-50%); max-width: 80%; padding: 6px 12px;
  background: #1c1c1c; border: 1px solid #3a3a3a; color: var(--fg); font-size: 12.5px; z-index: 5; }
.ap-sess { border-bottom: 1px solid var(--tile-border); }
.ap-head { position: sticky; top: 0; z-index: 2; background: #141414; border-bottom: 1px solid var(--tile-border); border-left: 2px solid var(--ask); padding: 4px 8px 5px; }
.ap-who { display: flex; align-items: baseline; gap: 6px; min-width: 0; }
.ap-who .t-ask { align-self: center; }
.ap-abbr { color: var(--fg-dim); font-weight: 600; }
.ap-name { font-weight: 600; font-size: 13.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; }
.ap-meta { color: var(--fg-faint); font-size: 11.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; flex: 1 1 auto; }
.ap-row, .ap-confirm { display: flex; align-items: center; gap: 4px; margin-top: 4px; flex-wrap: wrap; }
.ap-head button { height: 22px; padding: 0 10px; font-size: 12px; background: var(--btn); border: 1px solid var(--btn-border); border-radius: 0; color: var(--fg); }
.ap-head button:hover:not(:disabled) { filter: brightness(1.25); }
.ap-head button:disabled { opacity: .4; cursor: default; }
.ap-head button.primary { background: var(--ask); border-color: var(--ask); color: var(--ask-ink); font-weight: 600; }
.ap-head button.danger { background: var(--danger); border-color: var(--danger); color: #fff; }
.ap-count { color: var(--fg-dim); font-size: 12px; margin-left: 6px; font-variant-numeric: tabular-nums; }
.ap-flash { color: var(--green); font-size: 12px; flex: 1 1 auto; min-width: 0; }
.ap-head button.ap-discard { margin-left: auto; background: transparent; border-color: transparent; color: var(--fg-faint); }
.ap-head button.ap-discard:hover:not(:disabled) { color: var(--danger); border-color: #3a2424; filter: none; }
.ap-confirm-text { color: var(--fg); font-size: 12px; margin-right: 8px; }
.ap-body { display: flex; flex-direction: column; padding: 0 8px 6px; }
.ap-round { display: flex; flex-direction: column; }
.ap-rnote { color: var(--fg-dim); font-size: 11.5px; padding: 6px 0 0; }
.ap-frame { display: block; width: 100%; height: 320px; border: 0; background: var(--bg); }
.ap-recent .ap-rhead { border-left-color: var(--fg-faint); }
.ap-lrows { display: flex; flex-direction: column; gap: 2px; padding-top: 4px; }
.ap-lrow { display: flex; flex-direction: column; border: 1px solid var(--tile-border); background: #121212; }
.ap-lhead { display: flex; align-items: baseline; gap: 6px; padding: 3px 6px; cursor: pointer; min-width: 0; }
.ap-lhead:hover { background: #1b1b1b; }
.ap-lchev::before { content: "\25B8"; color: var(--fg-faint); font-size: 10px; }
.ap-lrow.open .ap-lchev::before { content: "\25BE"; }
.ap-lbody { padding: 0 6px 6px; }
.ap-lines { margin: 4px 0 0; padding: 4px 6px; background: var(--bg); border: 1px solid var(--tile-border); white-space: pre-wrap; font: 12px/1.4 "SF Mono", Menlo, Monaco, monospace; color: var(--fg); }
.ap-round.blocked { position: relative; }
.ap-round.blocked::after { content: ""; position: absolute; inset: 0; cursor: not-allowed; }
</style></head>
<body><div id="ask-pane" tabindex="-1" hidden><div class="ap-bar"><span class="ap-title"></span><span class="ap-hint"></span><button type="button" class="ap-close" title="Close">&times;</button></div>
<div class="ap-scroll"><div class="ap-list"></div><div class="ap-empty" hidden></div></div><div class="ap-note" hidden></div></div>
<script>
(function () {
  'use strict';
  const H = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.deck;
  const tell = m => { try { if (H) H.postMessage(m); } catch (e) { /* closing */ } };

  // ---- the deck, as preload.js's `deck` is to 49-asks.js: calls answered by Swift (DeckConnection.call)
  let rid = 0;
  const pending = new Map();
  const call = (fn, args) => new Promise((res, rej) => { const r = ++rid; pending.set(r, { res, rej }); tell({ ev: 'call', rid: r, fn, args: args || [] }); });
  const deck = {
    askDraftGet: id => call('askDraftGet', [id]),
    askLogState: id => call('askLogState', [id]),
    askDraft: (id, payload) => tell({ ev: 'fire', fn: 'askDraft', args: [id, payload] }),
    askClear: id => call('askClear', [id]),
    askDone: (sid, pages) => call('askDone', [sid, pages]),
    writeClipboard: text => { tell({ ev: 'clipboard', text }); return Promise.resolve(); },
  };

  // ---- the deck's state (Swift sends it with every meta: AskPane.statePayload)
  const st = { asks: [], askLog: [], tiles: [], accounts: [], cfg: {}, kh: {}, guest: false, guestWhy: '', remote: false, base: 'deckask://pane' };
  const askFrames = new Map();     // page id -> a waiting page's frame { id, sid, kind: 'ask', frame, wrap, note, ready, legacy, prog, waiters }
  const logFrames = new Map();     // page id -> an answered page's read-only frame { ..., kind: 'log' }
  const askSections = new Map();   // session id -> { el, head, body }
  let askPaneMode = null;          // null = closed, 'all', a session id, or 'log:<page id>'
  const RECENT = '__recent';
  const recentOpen = new Set();
  const askMovedSeen = new Set();
  let askReq = 0;
  const askKh = () => st.kh || {};
  const ASK_DEFAULTS = { showTile: true, showMarks: true, closeOnDone: true };
  const askCfg = () => Object.assign({}, ASK_DEFAULTS, st.cfg || {});
  const roundsOf = s => (s && Array.isArray(s.rounds) ? s.rounds : []);
  const askSession = sid => st.asks.find(s => s.sessionId === sid) || null;
  const askTileOf = sid => st.tiles.find(t => t.kind === 'claude' && t.sessionId === sid) || null;
  const acct = id => st.accounts.find(a => a.id === id) || null;
  const tileTitle = t => t.title || (t.kind === 'cloud' ? 'New cloud session' : t.kind === 'shell' ? 'Shell' : 'New session');   // 30-tiles.js
  const askOpenCount = s => roundsOf(s).reduce((n, r) => n + Math.max(0, (r.total || 0) - (r.answered || 0)), 0);
  const askFrameProg = id => { const f = askFrames.get(id); return f && f.prog ? f.prog : null; };
  const askDrafted = s => roundsOf(s).reduce((n, r) => n + (r.sent ? 0 : Math.max(r.drafted || 0, (askFrameProg(r.id) || {}).drafted || 0)), 0);
  const askUnsent = s => roundsOf(s).some(r => !r.sent);
  const askLastLog = sid => st.askLog.find(e => e.sessionId === sid) || null;
  function askName(s) { const t = askTileOf(s.sessionId); return t ? tileTitle(t) : (s.title || s.name || ''); }
  function askAbbr(s) { const t = askTileOf(s.sessionId); const a = t ? acct(t.account) : null; return a && a.abbr ? a.abbr : ''; }
  const askTime = v => { if (v == null || v === '') return ''; const d = new Date(v); return isNaN(d) ? '' : d.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) + (new Date().toDateString() === d.toDateString() ? '' : ' ' + d.toLocaleDateString([], { month: 'short', day: 'numeric' })); };
  const guestLine = () => `Read-only here: ${st.guestWhy || 'another deck is serving the ask pages'}. Answer on that deck, or in the session's terminal.`;
  const plural = n => (n === 1 ? '' : 's');
  const show = v => (v == null ? '?' : String(v));

  function askMarkInfo(sessionId) {
    const s = sessionId ? askSession(sessionId) : null;
    if (!s) return null;
    const open = askOpenCount(s), drafted = askDrafted(s);
    const kind = !askUnsent(s) ? 'sent' : drafted ? 'drafted' : 'waiting';
    const key = askKh().asks || 'Cmd+Shift+A';
    const title = kind === 'sent' ? 'answers sent; the session has not read them yet'
      : kind === 'drafted' ? `${drafted} of ${open} answered on the page, not sent yet (${key})`
        : `${open} question${plural(open)} waiting on an ask page (${key})`;
    return { kind, text: kind === 'sent' ? '✓' : String(open), title };
  }
  function paintAskMark(el, sessionId) {
    if (!el) return;
    const m = askMarkInfo(sessionId);
    el.hidden = !m;
    if (!m) return;
    el.classList.toggle('drafted', m.kind === 'drafted');
    el.classList.toggle('sent', m.kind === 'sent');
    if (el.textContent !== m.text) el.textContent = m.text;
    if (el.title !== m.title) el.title = m.title;
  }

  // ---- the pane
  const askPane = document.getElementById('ask-pane');
  const askList = askPane.querySelector('.ap-list'), askScroll = askPane.querySelector('.ap-scroll'), noteEl = askPane.querySelector('.ap-note');
  askPane.querySelector('.ap-close').addEventListener('click', () => closeAskPane());
  // Esc in this document closes the pane; Esc inside a page stays the page's (a key in a frame never reaches here)
  askPane.addEventListener('keydown', ev => { if (ev.key === 'Escape') { ev.preventDefault(); closeAskPane(); } });
  const recentSec = document.createElement('section'); recentSec.className = 'ap-sess ap-recent'; recentSec.style.order = '100000'; recentSec.hidden = true;
  recentSec.innerHTML = '<div class="ap-head ap-rhead"><div class="ap-who"><span class="ap-name">Recently answered</span><span class="ap-meta"></span></div></div><div class="ap-body ap-lrows"></div>';
  askList.appendChild(recentSec);

  // paneNote (20-terminals.js): over the pane while it is open; the deck window's own note once it is closed
  let noteT = null;
  function paneNote(text, ms = 2500) {
    if (!askOpen()) { tell({ ev: 'note', text, ms }); return; }
    noteEl.textContent = text; noteEl.hidden = false;
    clearTimeout(noteT); noteT = setTimeout(() => { noteEl.hidden = true; }, ms);
  }

  const askOpen = () => !!askPaneMode;
  const logModeId = () => (askPaneMode && askPaneMode.startsWith('log:') ? askPaneMode.slice(4) : null);
  function openAskPane(mode, scrollTo) {
    if (!mode) return;
    if (mode !== 'all' && !mode.startsWith('log:') && !askSession(mode)) { askPaneMode = null; askPane.hidden = true; tell({ ev: 'closed', toTile: null }); tell({ ev: 'note', text: 'No questions from this session.', ms: 2500 }); return; }
    askPaneMode = mode;
    askPane.hidden = false;
    if (mode.startsWith('log:')) recentOpen.add(mode.slice(4));
    paintAskPane();
    // every frame gets the latest drafts (another window or the phone may have moved them on)
    for (const fr of askFrames.values()) if (fr.ready && askVisible(fr.sid)) askLoad(fr);
    requestAnimationFrame(() => {
      scrollTarget = mode === 'all' ? scrollTo || null : null; scrollUntil = Date.now() + 2500;
      rescroll();
      askPane.focus({ preventScroll: true });
    });
  }
  // `quiet`: Swift closed it (F7, a tile click …) and knows; otherwise Swift is told, with the tile to go to
  function closeAskPane(opts = {}) {
    if (!askPaneMode) return;
    const wasLog = logModeId();
    askPaneMode = null;
    askPane.hidden = true;
    noteEl.hidden = true;
    if (wasLog) recentOpen.delete(wasLog);
    for (const sid of askSections.keys()) askConfirmOff(sid);
    if (!opts.quiet) tell({ ev: 'closed', toTile: opts.toTile || null });
  }
  const askVisible = sid => askPaneMode === 'all' || askPaneMode === sid;

  const askSrc = id => `${st.base}/ask/${encodeURIComponent(id)}.html?embed=deck&theme=dark`;
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
    const guarded = f => ev => { if (ev.currentTarget.disabled) return; if (st.guest) { askFlash(sid, guestLine(), 6000); return; } f(ev); };
    head.querySelector('.ap-done').addEventListener('click', guarded(() => askDone(sid)));
    head.querySelector('.ap-copy').addEventListener('click', () => askCopy(sid));
    head.querySelector('.ap-term').addEventListener('click', () => askToTerminal(sid));
    head.querySelector('.ap-discard').addEventListener('click', guarded(() => askConfirmOn(sid)));
    head.querySelector('.ap-keep').addEventListener('click', () => askConfirmOff(sid));
    head.querySelector('.ap-discard-yes').addEventListener('click', guarded(() => askDiscard(sid)));
    askList.appendChild(el);
    sec = { el, head, body };
    askSections.set(sid, sec);
    return sec;
  }
  // the deck's own sandbox list (lib/phone.js, the /ask/ route); the page arrives under its sandbox CSP as well
  const SANDBOX = 'allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads';
  function makeFrame(map, container, kind, id, sid, label) {
    let fr = map.get(id);
    if (fr) return fr;
    const wrap = document.createElement('div'); wrap.className = 'ap-round'; wrap.dataset.id = id;
    const note = document.createElement('div'); note.className = 'ap-rnote'; note.hidden = true;
    const frame = document.createElement('iframe');
    frame.className = 'ap-frame'; frame.title = label;
    frame.setAttribute('sandbox', SANDBOX);
    frame.setAttribute('allow', 'clipboard-read; clipboard-write');
    frame.src = askSrc(id);
    wrap.append(note, frame);
    container.appendChild(wrap);
    fr = { id, sid, kind, frame, wrap, note, ready: false, legacy: false, prog: null, waiters: new Map() };
    map.set(id, fr);
    // A page made before the skill learned the host bridge never says "ready": a waiting one shows its own bar and
    // its own Copy all / Done (AskPane saves that Done's download where the skill looks); an answered one cannot be
    // shown read-only, so its answer lines show instead. (A page the deck does not have is the same: no "ready".)
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
    const live = new Set(st.asks.map(s => s.sessionId));
    // a session whose pages all went (it read its answers): its section goes. Showing only it, the pane shows its
    // answers read-only instead (until it asks again), or closes when they are not in the log.
    for (const [sid, sec] of askSections) if (!live.has(sid)) {
      for (const [id, fr] of askFrames) if (fr.sid === sid) dropFrame(askFrames, id);
      sec.el.remove(); askSections.delete(sid);
    }
    if (askPaneMode && askPaneMode !== 'all' && !logModeId() && !live.has(askPaneMode)) {
      const gone = askPaneMode, last = askLastLog(gone);
      // its page was pushed out by another session's newer one: follow it there (their answers came over with it)
      const heir = st.asks.find(x => roundsOf(x).some(r => (r.replaces || []).some(p => p.sessionId === gone)));
      if (heir) {
        const p = roundsOf(heir).flatMap(r => r.replaces || []).filter(q => q.sessionId === gone);
        const n = p.reduce((k, q) => k + (q.moved || 0), 0);
        askPaneMode = heir.sessionId; tell({ ev: 'mode', mode: askPaneMode });
        paneNote(`That page was replaced by ${askName(heir)}'s newer one${n ? `; your ${n} answer${plural(n)} came over` : ''}.`, 6000);
      } else if (last) { askPaneMode = `log:${last.id}`; recentOpen.add(last.id); tell({ ev: 'mode', mode: askPaneMode }); }
      else { const t = askTileOf(gone); closeAskPane(); paneNote(`${t ? tileTitle(t) : 'The session'} has read its answers.`); return; }
    }
    if (!askOpen()) return;
    const lid = logModeId();
    st.asks.forEach((s, i) => {
      const sec = askSection(s);
      sec.el.style.order = String(i);               // reorder by CSS, never by moving the node: a moved iframe reloads
      const vis = askVisible(s.sessionId);
      sec.el.hidden = !vis;
      const rs = roundsOf(s);
      const want = new Set(rs.map(r => r.id));
      for (const [id, fr] of askFrames) if (fr.sid === s.sessionId && !want.has(id)) dropFrame(askFrames, id);
      if (vis) rs.forEach((r, k) => {
        const fr = makeFrame(askFrames, sec.body, 'ask', r.id, s.sessionId, `${s.name} round ${show(r.round)}`);
        fr.wrap.style.order = String(k);
        // sent (or another deck serves the pages: the guest is read-only): the page is locked
        if ((r.sent || st.guest) && fr.ready && !fr.locked) { fr.locked = true; askPost(fr, { askpage: 'lock' }); askEnsureLocked(fr); }
        const several = rs.length > 1, picker = r.source === 'picker';
        const parts = [];
        if (fr.legacy) parts.push('Made by an older ask-page skill, so Done above cannot read it: use the page\'s own Copy all answers (then paste in the terminal) or its Done (saves the answers file the session watches for).');
        if (several) parts.push(`Round ${show(r.round)}` + (r.title ? ` · ${r.title}` : ''));
        for (const p of r.replaces || []) {
          const whose = p.sessionId && p.sessionId !== s.sessionId ? `${p.name || 'another session'}'s round ${show(p.round)}` : `round ${show(p.round)}`;
          parts.push(`Replaces ${whose}` + (p.moved ? `: your ${p.moved} answer${plural(p.moved)} there came over` : ''));
          if (p.moved && !askMovedSeen.has(`${r.id}<${p.id}`)) { askMovedSeen.add(`${r.id}<${p.id}`); askFlash(s.sessionId, `A newer page replaced ${whose}; your ${p.moved} answer${plural(p.moved)} came over.`, 8000); }
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
    empty.hidden = !!st.asks.length || !!lid || st.askLog.length > 0;
    empty.textContent = 'No questions waiting. Sessions that ask on a page show up here and in the asks tile.';
    if (askPaneMode === 'all') bar.textContent = st.asks.length ? `Asks · ${st.asks.length} session${plural(st.asks.length)} waiting` : 'Asks · nothing waiting';
    else if (lid) { const e = st.askLog.find(x => x.id === lid); bar.textContent = e ? `${askLogName(e)} · answered ${askTime(e.at)} · read-only` : 'Answered'; }
    else { const s = askSession(askPaneMode); bar.textContent = s ? `${askName(s)} asks` : 'Asks'; }
    const key = askPaneMode === 'all' ? `${askKh().zoom || 'Cmd+Shift+Enter'} or ${askKh().asks || 'Cmd+Shift+A'}` : (askKh().asks || 'Cmd+Shift+A');
    hint.textContent = lid ? `${key} closes · it shows here until the session asks again`
      : st.guest ? `${key} closes · read-only: ${st.guestWhy || 'another deck is serving the ask pages'}`
        : `${key} closes · Done sends that session its answers`;
  }
  function askLogName(e) { const t = askTileOf(e.sessionId); return t ? tileTitle(t) : (e.title || e.name || ''); }
  // the recently answered list: every one in the every-session view, one in the log view; a row opens read-only
  function paintRecent(lid) {
    const show_ = askPaneMode === 'all' ? st.askLog.length > 0 : !!lid;
    recentSec.hidden = !show_;
    const head = recentSec.querySelector('.ap-rhead');
    head.hidden = !!lid;                                   // the log view: the bar says what it is
    recentSec.querySelector('.ap-meta').textContent = `the last ${st.askLog.length} you answered, every session · read-only`;
    const list = lid ? st.askLog.filter(e => e.id === lid) : (show_ ? st.askLog : []);
    const rowsEl = recentSec.querySelector('.ap-lrows');
    const want = new Set(list.map(e => e.id));
    for (const el of [...rowsEl.children]) if (!want.has(el.dataset.id)) { dropFrame(logFrames, el.dataset.id); el.remove(); }
    list.forEach((e, i) => {
      let row = null;
      for (const el of rowsEl.children) if (el.dataset.id === e.id) { row = el; break; }
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
      const rb = e.replacedBy, lines = Array.isArray(e.lines) ? e.lines : [];
      row.querySelector('.ap-meta').textContent = [e.name, `round ${show(e.round)}`, `${lines.length} answer${plural(lines.length)}`, `${e.how || ''} ${askTime(e.at)}`,
        rb && `replaced by ${rb.sessionId && rb.sessionId !== e.sessionId ? `${rb.name || 'another session'}'s ` : ''}round ${show(rb.round)}`].filter(Boolean).join(' · ');
      const open = !!lid || recentOpen.has(e.id);
      row.classList.toggle('open', open);
      const body = row.querySelector('.ap-lbody');
      body.hidden = !open;
      if (!open) { dropFrame(logFrames, e.id); return; }
      // e.url || !canWebview (49-asks.js): a remote always asks the deck for the page; the in-process deck only when the file is there
      const fr = (st.remote || e.url) ? makeFrame(logFrames, body, 'log', e.id, e.sessionId, `${e.name} round ${show(e.round)}, answered`) : null;
      // no page any more, or one from before the bridge: the answer lines, as they were sent
      let pre = body.querySelector('.ap-lines');
      if (!fr || fr.legacy) {
        if (!pre) { pre = document.createElement('pre'); pre.className = 'ap-lines'; body.appendChild(pre); }
        pre.textContent = lines.join('\n') || '(no answers)';
      } else if (pre) pre.remove();
    });
  }
  function paintAskHead(sid) {
    const s = askSession(sid), sec = askSections.get(sid);
    if (!s || !sec) return;
    const h = sec.head, t = askTileOf(sid), rs = roundsOf(s);
    paintAskMark(h.querySelector('.ap-mark'), sid);
    h.querySelector('.ap-abbr').textContent = askAbbr(s);
    h.querySelector('.ap-name').textContent = askName(s);
    const last = rs[rs.length - 1];
    const asked = last && last.created ? askTime(last.created) : '';
    h.querySelector('.ap-meta').textContent = [s.name, s.account, rs.length > 1 ? `${rs.length} pages` : `round ${last ? show(last.round) : '?'}`, asked && `asked ${asked}`, s.ended ? 'session ended' : !t && 'outside the deck'].filter(Boolean).join(' · ');
    let total = 0, answered = 0, later = 0;
    for (const r of rs) { const p = askFrameProg(r.id); total += r.total || 0; answered += p ? p.answered : (r.answered || 0); later += p ? p.later : (r.later || 0); }
    const unsent = askUnsent(s);
    h.querySelector('.ap-count').textContent = `${answered} of ${total} answered` + (later ? `, ${later} later` : '') + (unsent ? '' : ' · sent');
    const done = h.querySelector('.ap-done');
    done.disabled = !unsent || !!sec.busy || st.guest;
    done.textContent = unsent ? 'Done' : 'Sent';
    done.title = st.guest ? guestLine() : !unsent ? 'Sent; it shows read-only until the session reads it' : s.ended ? 'Its session has ended: save the answers file next to the page; it is read when the session is resumed' : t ? 'Save the answers file next to the page and tell the session where it is, in one line (after its turn, if it is busy)' : 'Save the answers file next to the page; the session picks it up';
    const term = h.querySelector('.ap-term');
    term.textContent = t ? 'Answer in the terminal' : 'Answer in its terminal';
    term.title = t ? `Close this and go to the session (${askKh().asks || 'Cmd+Shift+A'} brings it back)` : 'It runs outside the deck: close this and answer it there';
    h.querySelector('.ap-discard').disabled = st.guest || !askDrafted(s);
    paintFlash(sec);
  }
  // the section's line of text: a flash while it lasts, else (a guest) why it is read-only
  function paintFlash(sec) { const f = sec.head.querySelector('.ap-flash'); const t = sec.flash || (st.guest ? guestLine() : ''); if (f.textContent !== t) f.textContent = t; }
  function askFlash(sid, text, ms = 5000) {
    const sec = askSections.get(sid); if (!sec) return;
    sec.flash = text; paintFlash(sec);
    clearTimeout(sec.flashT); sec.flashT = setTimeout(() => { sec.flash = ''; paintFlash(sec); }, ms);
  }

  // ---- the bridge to the framed pages (the skill's page.html, "the host bridge")
  const askPost = (fr, m) => { try { fr.frame.contentWindow.postMessage(Object.assign({ id: fr.id }, m), '*'); } catch (e) { /* reloading */ } };
  async function askLoad(fr) {
    if (fr.kind === 'log') {
      let s = null; try { s = await deck.askLogState(fr.id); } catch (e) { s = null; }
      askPost(fr, { askpage: 'load', state: s || {}, readonly: true });
      askEnsureLocked(fr);
      return;
    }
    const r = roundsOf(askSession(fr.sid)).find(x => x.id === fr.id);
    let s = null; try { s = await deck.askDraftGet(fr.id); } catch (e) { s = null; }
    const ro = !!(r && r.sent) || st.guest;                // a guest's pages are read-only: the deck takes no draft
    askPost(fr, { askpage: 'load', state: s || {}, readonly: ro });   // the deck's copy is the one every view shares
    if (ro) { fr.locked = true; askEnsureLocked(fr); }
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
    if (ev.origin !== 'null' && ev.origin !== location.origin) return;     // a sandboxed page's origin is opaque
    const prog = () => ({ drafted: +m.drafted || 0, answered: +m.answered || 0, later: +m.later || 0, total: +m.total || 0 });
    if (m.askpage === 'ready') { fr.ready = true; if (fr.kind === 'ask') fr.prog = prog(); if (m.h) fr.frame.style.height = `${m.h}px`; askLoad(fr); rescroll(); }
    else if (m.askpage === 'change') {
      if (fr.kind !== 'ask' || fr.locked || st.guest) return;
      fr.prog = prog();
      deck.askDraft(fr.id, { state: m.state || {}, drafted: fr.prog.drafted, answered: fr.prog.answered, later: fr.prog.later, total: fr.prog.total });
      paintAskHead(fr.sid);
    } else if (m.askpage === 'height') { if (m.h > 0) fr.frame.style.height = `${Math.ceil(m.h)}px`; rescroll(); }
    else if (m.askpage === 'answers') { const w = fr.waiters.get(m.req); if (w) { fr.waiters.delete(m.req); w(m); } }
  });

  // ---- the buttons
  async function askCollect(sid) {
    const s = askSession(sid); if (!s) return null;
    const pages = [];
    let old = 0;
    for (const r of roundsOf(s)) {
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
  // the Copy-all text (lib/asks.js composeDone): each page's Copy-all lines, headed by its page id
  function askText(pages) {
    return pages.filter(p => (p.answers.lines || []).some(l => String(l).trim())).map(p => [p.id, ...p.answers.lines].join('\n')).join('\n');
  }
  async function askDone(sid) {
    const pages = await askCollect(sid); if (!pages) return;
    if (!pages.length || !askText(pages).trim()) { askFlash(sid, 'Nothing answered yet: pick an option or a reply, or type in a box.'); return; }
    const sec = askSections.get(sid), btn = sec && sec.head.querySelector('.ap-done');
    if (sec) sec.busy = true;
    if (btn) btn.disabled = true;
    try {
      const res = (await deck.askDone(sid, pages)) || {};
      for (const p of pages) { const fr = askFrames.get(p.id); if (fr) { fr.locked = true; askPost(fr, { askpage: 'lock' }); askEnsureLocked(fr); } }
      const t = askTileOf(sid), s = askSession(sid);
      let msg;
      // every Done saves the answers file next to the page; a session in this deck (res.tileId) also gets ONE line naming it
      if (!res.tileId && s && s.ended) msg = `Saved the answers file for ${s.name}. Its session has ended, so it is read when the session is resumed.`;
      else if (!res.tileId) msg = `Saved the answers file for ${s ? s.name : 'it'} (it runs outside the deck); it picks it up if it is watching.`;
      else if (res.esc) msg = 'Done: saved the answers file, pressed Esc on its picker, then told it where the file is.';
      else if (t && (t.status === 'busy' || t.status === 'waiting' || t.status === 'asleep' || t.status === 'starting')) msg = `Done: saved the answers file; ${tileTitle(t)} gets one line naming it as soon as it is back at its prompt.`;
      else msg = 'Done: saved the answers file, and one line naming it goes in as your next message.';
      // Done closes a session's ask (Settings -> Asks, on by default); the every-session view stays open while
      // another session still waits
      const others = st.asks.some(x => x.sessionId !== sid && askUnsent(x));
      const close = askCfg().closeOnDone !== false && (askPaneMode === sid || (askPaneMode === 'all' && !others));
      if (sec) sec.busy = false;
      if (close) { closeAskPane({ toTile: t && res.tileId && askPaneMode === sid ? t.id : null }); paneNote(msg, 5000); }
      else askFlash(sid, msg, 8000);
    } catch (e) {
      if (sec) sec.busy = false;
      askFlash(sid, String((e && e.message) || e), 8000);
      if (btn) btn.disabled = false;
    }
  }
  async function askCopy(sid) {
    const pages = await askCollect(sid); if (!pages) return;
    const text = askText(pages);
    if (!text.trim()) { askFlash(sid, 'Nothing answered yet.'); return; }
    await deck.writeClipboard(text);
    const n = text.split('\n').length;
    askFlash(sid, `Copied ${n} line${plural(n)}: paste them into the session's terminal.`);
  }
  function askToTerminal(sid) { const t = askTileOf(sid); closeAskPane({ toTile: t ? t.id : null }); }
  function askConfirmOn(sid) {
    const sec = askSections.get(sid), s = askSession(sid); if (!sec || !s) return;
    const n = askDrafted(s); if (!n) return;
    sec.head.querySelector('.ap-row').hidden = true;
    const c = sec.head.querySelector('.ap-confirm'); c.hidden = false;
    c.querySelector('.ap-confirm-text').textContent = `Discard the ${n} answer${plural(n)} in progress for ${askName(s)}? The questions stay; you answer them in the terminal.`;
    // hard to misclick: the safe choice is where Done was, and Discard only arms after a moment
    const yes = c.querySelector('.ap-discard-yes'); yes.disabled = true; setTimeout(() => { yes.disabled = false; }, 700);
    c.querySelector('.ap-keep').focus();
  }
  function askConfirmOff(sid) {
    const sec = askSections.get(sid); if (!sec) return;
    sec.head.querySelector('.ap-row').hidden = false; sec.head.querySelector('.ap-confirm').hidden = true;
  }
  async function askDiscard(sid) {
    const s = askSession(sid); if (!s) return;
    for (const r of roundsOf(s)) { if (r.sent) continue; const fr = askFrames.get(r.id); if (fr) askPost(fr, { askpage: 'clear' }); try { await deck.askClear(r.id); } catch (e) { /* gone */ } }
    askConfirmOff(sid);
    askToTerminal(sid);                 // the same as opening it, entering nothing, and answering in the terminal
  }

  // The row asked for, at the top. On a first open the pages above it are still loading (a frame is 320px until its
  // page says how tall it is), so the place is taken again as each one grows, for a moment, until the person
  // scrolls, clicks or types (windows/src/asks.hpp's host document does the same).
  let scrollTarget = null, scrollUntil = 0;
  function rescroll() {
    if (Date.now() > scrollUntil) return;
    const target = scrollTarget === RECENT ? recentSec : scrollTarget && askSections.get(scrollTarget) ? askSections.get(scrollTarget).el : null;
    askScroll.scrollTop = target ? target.offsetTop : 0;
  }
  for (const ev of ['wheel', 'mousedown', 'keydown']) window.addEventListener(ev, () => { scrollUntil = 0; }, { capture: true, passive: true });

  // ---- Swift's messages
  function setState(m) {
    const wasGuest = st.guest;
    for (const k of ['asks', 'askLog', 'tiles', 'accounts']) st[k] = Array.isArray(m[k]) ? m[k] : [];
    st.cfg = m.cfg && typeof m.cfg === 'object' ? m.cfg : {};
    st.kh = m.kh && typeof m.kh === 'object' ? m.kh : {};
    st.guest = !!m.guest; st.guestWhy = String(m.guestWhy || '');
    st.remote = !!m.remote;
    if (typeof m.base === 'string' && m.base) st.base = m.base;
    // the pages already up are locked (a guest now) or loaded again unlocked (this deck serves them now): made afresh
    if (st.guest !== wasGuest) { for (const id of [...askFrames.keys()]) dropFrame(askFrames, id); for (const sid of askSections.keys()) askConfirmOff(sid); }
    if (askOpen() || askSections.size) paintAskPane();      // 49-asks.js paintAsks
  }
  window.deckIn = m => {
    if (!m || typeof m !== 'object') return;
    if (m.op === 'state') setState(m);
    else if (m.op === 'open') openAskPane(m.mode, m.scrollTo);
    else if (m.op === 'close') closeAskPane({ quiet: true });
    else if (m.op === 'focus') askPane.focus({ preventScroll: true });
    else if (m.op === 'reply') {
      const p = pending.get(m.rid); if (!p) return;
      pending.delete(m.rid);
      if (m.ok) p.res(m.value === undefined ? null : m.value); else p.rej(new Error(String(m.error || 'failed')));
    }
  };
  tell({ ev: 'ready' });
})();
</script></body></html>
"""#
}
