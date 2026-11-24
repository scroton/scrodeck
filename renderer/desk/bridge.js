'use strict';
// The browser desk's bridge: window.deck, over the phone server's websocket (build-order step 5,
// docs/NATIVE-PORT.md F6(a)). The desktop renderer (renderer/js/**) runs here AS IS and expects the
// object preload.js gives it in Electron: `invoke`-style calls that answer a promise, `send`-style calls
// that fire and forget, and `on*` subscriptions for what the main process pushes. Every one of those is
// here, over the message shapes lib/phone.js speaks (core/PROTOCOL.md lists them): a call is
// { t:'call', rid, fn, args } answered by ok/err with the same rid; a pushed event arrives as
// { t:'event', name, payload } and is dispatched to the on* subscribers by name — the same channel names
// preload.js listens on, so the renderer cannot tell the difference.
//
// Served ahead of the page's own scripts by lib/phone.js (deskPage). Runs in a browser, not Electron:
// the few things that were the shell's — fullscreen, the clipboard, window keys — map to the browser's
// own, and the two that have no browser equivalent (the DevTools drag rescue, a new deck window) are
// no-ops that say so.
(() => {
  const listeners = new Map();          // channel -> [callbacks]
  const on = channel => cb => { const l = listeners.get(channel) || []; l.push(cb); listeners.set(channel, l); };
  const emit = (channel, payload) => { for (const cb of listeners.get(channel) || []) { try { cb(payload); } catch (e) { console.error(`deck ${channel}:`, e); } } };

  // ---------------------------------------------------------------- the socket
  let ws = null, opened = false, everOpened = false, backoff = 500;
  const queue = [];                     // messages sent before the socket is open
  const pending = new Map();            // rid -> { resolve, reject }
  let rid = 0;
  let platform = {};                    // from deck.init: os, isMac, keyHelp — the DECK's platform, as the renderer keys on
  function sendRaw(msg) {
    if (ws && ws.readyState === 1) { try { ws.send(JSON.stringify(msg)); } catch { /* closing */ } }
    else queue.push(msg);
  }
  function call(fn, ...args) {
    return new Promise((resolve, reject) => {
      const id = ++rid;
      pending.set(id, { resolve, reject });
      sendRaw({ t: 'call', rid: id, fn, args });
      setTimeout(() => { if (pending.delete(id)) reject(new Error(`${fn}: timed out`)); }, 120000);
    });
  }
  const fire = (fn, ...args) => sendRaw({ t: 'call', rid: 0, fn, args });

  // R8 (2026-09-05): a remote client sends what the user actually typed and nothing else. xterm
  // synthesises two kinds of input on its own — mouse reports when the application asked for tracking
  // (a wheel notch over Claude Code's TUI becomes ESC[<64;col;rowM, the exact shape of the 2026-09-05
  // leak) and focus reports (ESC[I / ESC[O) — and neither is a keystroke, so neither leaves this page.
  // Terminal replies to the session's own queries (cursor position, device attributes) are not
  // synthesised by a heuristic and stay.
  //
  // NARROWED FOR THE WHEEL (2026-11-18, from a remote desk: "I can't scroll up"). As ruled, R8 cost
  // every wheel notch over a Claude Code pane: it runs full-screen with mouse tracking on, so the wheel is
  // the only way to scroll it, and the filter dropped the report. This is the fallback docs/NATIVE-PORT.md
  // names under F6 ("allow synthesized sequences only when the stream's own mouseOn / sgrMouse say the
  // session is asking for them"), for the WHEEL ONLY: one SGR wheel report (isWheelReport) goes out when
  // the deck's latest meta for that tile says mouseOn and sgrMouse — the deck's own reading of the session's
  // stream, reset on every spawn and false while it sleeps, so a report can never land at a prompt that did
  // not ask for one (the 2026-09-05 leak). Everything else is still dropped: motion, drags, the middle and
  // right buttons, X10-encoded reports, focus reports, and a wheel the session did not ask for. (A left CLICK
  // was let out under the same gate on 2026-11-19: clickAsked below.)
  //
  // SYNTHESISED stays the list of ALL synthesised shapes (the wheel included): core/test/proto reads this
  // line out of this file. The wheel's way out is the separate, named test below.
  const SYNTHESISED = /^(?:\x1b\[<\d+;\d+;\d+[Mm]|\x1b\[M[\s\S]{3}|\x1b\[[IO])+$/;
  // exactly ONE SGR wheel report and nothing else: ESC[<B;col;rowM with B = 64 (up) or 65 (down), plus
  // xterm's modifier bits on the button (4 shift, 8 meta, 16 ctrl: 68/69, 72/73, 80/81 …). Never the
  // release form (a final 'm'), never two in one string.
  const SGR_WHEEL = /^\x1b\[<(\d+);\d+;\d+M$/;
  function isWheelReport(d) { const m = SGR_WHEEL.exec(d); if (!m) return false; const base = Number(m[1]) & ~28; return base === 64 || base === 65; }
  const tileFlags = new Map();          // id -> { mouseOn, sgrMouse } from the latest meta (noteMeta)
  function noteMeta(meta) {
    if (!meta || !Array.isArray(meta.tiles)) return;
    tileFlags.clear();
    for (const t of meta.tiles) if (t && t.id) tileFlags.set(t.id, { mouseOn: t.mouseOn === true, sgrMouse: t.sgrMouse === true });
  }
  // the session is asking for the wheel, in the encoding this report is in
  function wheelAsked(id, d) { const f = tileFlags.get(id); return !!(f && f.mouseOn && f.sgrMouse) && isWheelReport(d); }
  // NARROWED FOR A CLICK (2026-11-19, from the remote: "clicks in the remote don't work, like to tell it how
  // well claude is doing, or the yes/no questions"): exactly ONE SGR LEFT-button press (M) or release (m), modifier
  // bits only, under the wheel's gate. 20-terminals.js deskClickThrough makes them, for a click and never a drag;
  // xterm itself still makes no click report on a desk (deskSelectByDrag).
  const SGR_CLICK = /^\x1b\[<(\d+);([1-9]\d*);([1-9]\d*)[Mm]$/;
  function isClickReport(d) { const m = SGR_CLICK.exec(d); return !!m && (Number(m[1]) & ~28) === 0; }
  function clickAsked(id, d) { const f = tileFlags.get(id); return !!(f && f.mouseOn && f.sgrMouse) && isClickReport(d); }
  const input = (id, d) => {
    if (typeof d === 'string' && SYNTHESISED.test(d)) {
      if (wheelAsked(id, d)) { sendRaw({ t: 'input', id, d, kind: 'wheel' }); return; }
      if (clickAsked(id, d)) { sendRaw({ t: 'input', id, d, kind: 'click' }); return; }
      return;
    }
    sendRaw({ t: 'input', id, d, kind: 'key' });
  };

  // ---------------------------------------------------------------- attach: only what is on screen
  // The renderer attaches a terminal for EVERY tile at boot (createTerm) and would have the socket carry
  // every session's output to panes nobody can see. The desk attaches to the panes it shows (a 2×2 grid
  // shows up to four) and detaches the rest: setShown is the renderer's own answer to "what is on
  // screen", so it is the switch. A pane coming back on screen has a terminal holding whatever it showed
  // last, so it is reset here first (the renderer's own tile:reset handler) and the server's replay on
  // attach fills it — the same path a redraw takes.
  const wanted = new Map();             // id -> attach opts the renderer asked with
  const shown = new Set();              // ids the renderer says are on screen
  const attached = new Set();           // ids the socket is carrying
  const everAttached = new Set();
  function attachNow(id, opts) {
    if (attached.has(id) && !(opts && opts.sinceResize)) return;
    if (everAttached.has(id) && !attached.has(id)) emit('tile:reset', { id });
    attached.add(id); everAttached.add(id);
    sendRaw({ t: 'attach', id, since: !!(opts && opts.sinceResize) });
  }
  function detachNow(id) { if (!attached.has(id)) return; attached.delete(id); sendRaw({ t: 'detach', id }); }
  function syncAttached() {
    for (const id of shown) if (wanted.has(id) && !attached.has(id)) attachNow(id, null);
    for (const id of [...attached]) if (!shown.has(id)) detachNow(id);
  }
  function sendShown() { sendRaw({ t: 'shown', ids: [...shown], hidden: !!document.hidden }); }
  // a hidden tab shows nothing: the desk lets go of what it drives (the driver rule) and hibernation
  // no longer counts its panes as on screen; visible again, it says what it shows
  document.addEventListener('visibilitychange', () => sendShown());

  // ---------------------------------------------------------------- the overlay: away, or refused
  let note = null;
  function say(text, detail) {
    if (!note) {
      note = document.createElement('div');
      note.id = 'desk-note';
      note.style.cssText = 'position:fixed;left:50%;top:14px;transform:translateX(-50%);z-index:100000;background:#1a1a1a;color:#ddd;border:1px solid #444;border-radius:8px;padding:10px 16px;font:14px system-ui,sans-serif;max-width:80vw;box-shadow:0 4px 18px rgba(0,0,0,.6)';
      document.documentElement.appendChild(note);
    }
    note.textContent = text;
    if (detail) { const d = document.createElement('div'); d.style.cssText = 'color:#999;font-size:12px;margin-top:4px'; d.textContent = detail; note.appendChild(d); }
    note.hidden = !text;
  }

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${location.host}/desk/ws`);
    ws.onopen = () => {
      // A reconnect is a fresh window as far as the deck is concerned (the entry went with the old
      // socket), and the renderer only boots once: reload so it boots against the new registration
      // with its layout restored, exactly as a deck restart or a view reload does.
      if (everOpened) { location.reload(); return; }
      opened = everOpened = true; backoff = 500;
      say('');
      while (queue.length) sendRaw(queue.shift());
    };
    ws.onclose = () => {
      if (opened) say('The deck is away — reconnecting…', 'The page reloads by itself when it is back.');
      opened = false;
      setTimeout(connect, backoff); backoff = Math.min(8000, backoff * 1.7);
    };
    ws.onerror = () => { try { ws.close(); } catch { /* already going */ } };
    ws.onmessage = ev => {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      switch (m.t) {
        case 'ok': case 'err': {
          const p = pending.get(m.rid); if (!p) return;
          pending.delete(m.rid);
          if (m.t === 'ok') p.resolve(m.result); else p.reject(new Error(m.error));
          return;
        }
        case 'data': emit('tile:data', { id: m.id, data: m.d, replay: !!m.replay }); return;   // replay: an attach's (20-terminals.js deskAnswers)
        case 'meta': noteMeta(m.meta); emit('meta', m.meta); return;
        case 'exit': emit('tile:exit', { id: m.id, exitCode: m.exitCode }); return;
        case 'reset': emit('tile:reset', { id: m.id }); return;
        case 'event': emit(m.name, m.payload); return;
        case 'init': return;            // the phone-shaped hello; the desk's answer is deskInit's
        default: return;
      }
    };
  }

  // deskInit registers this page as a window. Refused while another desk is connected (two remote desks
  // at once is unsupported): retried for a few seconds first, because a reload's old socket may still be
  // closing, then said out loud — the renderer never boots, which is the point.
  // An optional name for this desk's machine, shown on the other screens' driver button ("the remote desk
  // (NAME)"). A browser cannot know its machine's name, so it is only what the link says: /desk/?name=laptop.
  // The native exe sends its computer name the same way.
  let deskName = '';
  try { deskName = (new URLSearchParams(location.search).get('name') || '').trim().slice(0, 40); } catch { /* no name */ }
  async function init() {
    for (let i = 0; ; i++) {
      try {
        // kind: which remote desk this is, for the state the deck's sessions read (lib/presence.js); the native exe says 'native'
        const r = await call('deskInit', deskName ? { name: deskName, kind: 'browser' } : { kind: 'browser' });
        platform = r.platform || {};
        noteMeta(r.meta);
        return r;
      } catch (e) {
        if (!/one desk at a time/.test(String(e.message)) || i >= 6) {
          say('This deck already has a browser desk open.', 'Close the other tab, then reload this one. ' + (e.message || ''));
          throw e;
        }
        await new Promise(r => setTimeout(r, 1200));
      }
    }
  }

  // ---------------------------------------------------------------- window keys
  // In Electron the main process takes these before the page sees them (before-input-event → platform
  // .keyAction) and tells the window what to do. In a browser nobody does, so this is that handler: the
  // same chords, keyed on the DECK's platform like everything else the renderer does, dispatched to the
  // same channels. Registered first, in the capture phase, and stopped there, so the page's other
  // listeners never see a key the desktop never saw either. Ctrl/Cmd+Shift+R is left to the browser
  // (it is "reload the view" on the desktop too).
  function keyAction(ev) {
    const k = String(ev.key || '').toLowerCase();
    if (platform.isMac) {
      if (ev.metaKey && ev.shiftKey && !ev.altKey && k === 'p') return 'palette';
      if (ev.metaKey && !ev.shiftKey && !ev.altKey && !ev.ctrlKey && k === 'b') return 'sidebar';
      if (ev.metaKey && ev.ctrlKey && k === 'f') return 'fullscreen';
      if (ev.metaKey && ev.shiftKey && (k === '[' || k === '{')) return 'prev';
      if (ev.metaKey && ev.shiftKey && (k === ']' || k === '}')) return 'next';
      if (ev.metaKey && !ev.shiftKey && !ev.altKey && !ev.ctrlKey && k === ',') return 'settings';
      if (ev.metaKey && !ev.shiftKey && !ev.altKey && !ev.ctrlKey && k === 'r') return 'redraw';
      if (ev.metaKey && ev.shiftKey && !ev.altKey && !ev.ctrlKey && k === 'enter') return 'zoom';
      if (ev.metaKey && ev.shiftKey && !ev.altKey && !ev.ctrlKey && k === 'a') return 'asks';
      return null;
    }
    if (ev.key === 'F5') return 'redraw';
    if (ev.key === 'F6') return 'zoom';
    if (ev.key === 'F7' && !ev.ctrlKey && !ev.altKey && !ev.metaKey) return 'asks';
    if (ev.key === 'F9' || (ev.ctrlKey && !ev.shiftKey && !ev.altKey && ev.code === 'Backquote')) return 'sidebar';
    if (ev.key === 'F10' && !ev.shiftKey) return 'fullscreen';   // Shift+F10: the tile menu (35-groups.js)
    if (ev.key === 'F11') return 'prev';
    if (ev.key === 'F12') return 'next';
    if (ev.ctrlKey && ev.shiftKey && !ev.altKey && k === 'p') return 'palette';
    return null;
  }
  function toggleFullscreen() {
    try {
      if (document.fullscreenElement) return document.exitFullscreen();
      return document.documentElement.requestFullscreen();
    } catch { return Promise.resolve(); }
  }
  window.addEventListener('keydown', ev => {
    const action = keyAction(ev);
    if (!action) return;
    ev.preventDefault(); ev.stopImmediatePropagation();
    if (action === 'fullscreen') toggleFullscreen();
    else if (action === 'redraw') emit('view:redraw');
    else if (action === 'zoom') emit('view:zoom');
    else if (action === 'asks') emit('view:asks');
    else if (action === 'prev') emit('tiles:step', -1);
    else if (action === 'next') emit('tiles:step', 1);
    else if (action === 'palette') emit('palette:open');
    else if (action === 'sidebar') emit('sidebar:toggle');
    else if (action === 'settings') emit('settings:toggle');
  }, true);

  // ---------------------------------------------------------------- the clipboard
  // navigator.clipboard needs a secure context (the HTTPS front door, or localhost); over plain http the
  // read answers nothing and the write falls back to the old textarea trick, which still works on a key.
  // Whether the browser has ALREADY granted this page clipboard-read: the one case in which a read cannot be
  // refused and asks nothing. The renderer's right-click paste keys on it (20-terminals.js); where it is not
  // granted (or the browser has no such permission to ask about: Firefox, plain http) the browser's own menu
  // and its own paste event do the pasting instead, which need no permission.
  let clipboardReadable = false;
  try {
    if (window.isSecureContext && navigator.permissions && navigator.clipboard && navigator.clipboard.readText) {
      navigator.permissions.query({ name: 'clipboard-read' }).then(st => {
        const set = () => { clipboardReadable = st.state === 'granted'; };
        set(); st.onchange = set;
      }).catch(() => { /* not a permission this browser knows: stays false */ });
    }
  } catch { /* stays false */ }
  async function readClipboard() {
    try { return navigator.clipboard && navigator.clipboard.readText ? await navigator.clipboard.readText() : ''; } catch { return ''; }
  }
  async function writeClipboard(text) {
    const t = String(text ?? '');
    try { if (navigator.clipboard && navigator.clipboard.writeText) { await navigator.clipboard.writeText(t); return; } } catch { /* fall through */ }
    try {
      const ta = document.createElement('textarea'); ta.value = t; ta.style.cssText = 'position:fixed;left:-9999px;top:0';
      document.body.appendChild(ta); ta.select(); document.execCommand('copy'); ta.remove();
    } catch { /* nothing more to try */ }
  }

  // ---------------------------------------------------------------- window.deck — every function in preload.js
  window.deck = {
    init: () => init(),
    newTile: (spec) => call('newTile', spec),
    closeTile: (id) => call('closeTile', id),
    moveTile: (id, account, opts) => call('moveTile', id, account, opts),
    restartTile: (id) => call('restartTile', id),
    bringHere: (id) => call('bringHere', id),
    sleepTile: (id) => call('sleepTile', id),
    wakeTile: (id) => call('wakeTile', id),
    setShown: (ids) => { shown.clear(); for (const id of (Array.isArray(ids) ? ids : [])) if (id) shown.add(id); sendShown(); syncAttached(); },
    saveLayout: (layout) => sendRaw({ t: 'layout', layout: layout || {} }),
    // A new deck window is a BrowserWindow on the deck's box; a desk cannot open one there, and a second
    // browser desk is unsupported (two remote desks at once, agreed 2026-09-03). A drag released past the
    // page's edge therefore does nothing here. No-op, answering null as "no window opened".
    openWindow: () => Promise.resolve(null),
    // the stranded-drag rescue drives Electron's debugger from the main process; there is no debugger to
    // drive from a browser, and its drags are not the ones that strand. No-op.
    dragRescue: () => Promise.resolve(false),
    previewLeaving: (id, on) => sendRaw({ t: 'leaving', id, on: !!on }),
    onLeaving: on('layout:leaving'),
    onLayoutEvict: on('layout:evict'),
    onGridChanged: on('grid:changed'),
    refreshTile: (id, opts) => call('refreshTile', id, opts),
    pauseTile: (id, opts) => call('pauseTile', id, opts),
    resumeTile: (id) => call('resumeTile', id),
    pauseAll: (opts) => call('pauseAll', opts),
    resumeAll: () => call('resumeAll'),
    makeAuditor: (id, string) => call('makeAuditor', id, string),
    unmakeAuditor: (id) => call('unmakeAuditor', id),
    setAuditorChild: (id, parent) => call('setAuditorChild', id, parent),
    unchildTile: (id) => call('unchildTile', id),
    auditorSend: (itemId) => call('auditorSend', itemId),
    auditorDrop: (itemId) => call('auditorDrop', itemId),
    renameTile: (id, title) => call('renameTile', id, title),
    pin: (id, pinned) => call('pin', id, pinned),
    reorder: (ids) => call('reorder', ids),
    setGroup: (id, group) => call('setGroup', id, group),
    setGroups: (names) => call('setGroups', names),
    onActivate: on('tiles:activate'),
    onScreenProbe: on('screen:probe'),
    screenProbeReply: (r) => fire('screenProbeReply', r),
    refreshUsage: () => call('refreshUsage'),
    restartWhenQuiet: (on, why) => call('restartWhenQuiet', on, why),
    restartNow: () => call('restartNow'),
    updateLater: () => call('updateLater'),
    updateCode: () => call('updateCode'),
    getSettings: () => call('getSettings'),
    saveSettings: (s) => call('saveSettings', s),
    setDriver: (mode) => call('setDriver', mode),
    holdAdd: (id, item) => call('holdAdd', id, item),
    holdRemove: (qid) => call('holdRemove', qid),
    triggersList: () => call('triggersList'),
    triggerDeclare: (name) => call('triggerDeclare', name),
    triggerSet: (name, value) => call('triggerSet', name, value),
    triggerRemove: (name) => call('triggerRemove', name),
    saveAccounts: (rows) => call('saveAccounts', rows),
    toggleFullscreen: () => Promise.resolve(toggleFullscreen()),   // the browser's fullscreen, not the window's
    externalSessions: () => call('externalSessions'),
    pastSessions: () => call('pastSessions'),
    onPalette: on('palette:open'),
    adoptSession: (s) => call('adoptSession', s),
    moveAll: (from, to, continueIfUsage, onlyIds) => call('moveAll', from, to, continueIfUsage, onlyIds),
    readClipboard: () => readClipboard(),
    writeClipboard: (t) => writeClipboard(t),
    // a picture pasted here: the deck saves it and answers its path (main.js savePastedImage)
    pasteImage: (id, b64, mime) => call('pasteImage', id, b64, mime),
    attach: (id, opts) => { wanted.set(id, opts || null); if (shown.has(id)) attachNow(id, opts || null); },
    nudge: (id, cols, rows) => fire('nudge', id, cols, rows),
    input: (id, data) => input(id, data),
    userInput: (id) => fire('userInput', id),
    resize: (id, cols, rows) => sendRaw({ t: 'resize', id, cols, rows }),
    // the window title ConPTY reports is read on the deck's own desktop window from the same stream; a
    // desk has nothing to add. No-op.
    setOscTitle: () => {},
    onData: on('tile:data'),
    onExit: on('tile:exit'),
    onReset: on('tile:reset'),
    onMeta: on('meta'),
    onSidebarToggle: on('sidebar:toggle'),
    onSettingsToggle: on('settings:toggle'),
    onTileStep: on('tiles:step'),
    onRedraw: on('view:redraw'),
    onZoom: on('view:zoom'),
    // the ask pane (renderer/js/49-asks.js): its pages come from /ask/<id>.html (lib/phone.js), sandboxed
    onAsks: on('view:asks'),
    askDraftGet: (pageId) => call('askDraftGet', pageId),
    askLogState: (pageId) => call('askLogState', pageId),
    askDraft: (id, payload) => fire('askDraft', id, payload),
    askClear: (pageId) => call('askClear', pageId),
    askDone: (sessionId, pages) => call('askDone', sessionId, pages),
    askOpenLink: (url) => { try { window.open(url, '_blank', 'noopener'); } catch { /* blocked */ } return Promise.resolve(true); },
    // not in preload.js: the desk's own state, for tests and the console
    _desk: { get attached() { return [...attached]; }, get shown() { return [...shown]; }, get connected() { return opened; },
      get clipboardReadable() { return clipboardReadable; },
      clicksAsked: (id) => { const f = tileFlags.get(id); return !!(f && f.mouseOn && f.sgrMouse); } },
  };

  connect();
})();
