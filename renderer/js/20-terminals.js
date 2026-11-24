'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- terminals
// fallback chain per platform (Cascadia on Windows, SF Mono / Menlo on a Mac); main hands it over at init
const defaultFont = () => (state.platform && state.platform.fontStack) || '"Cascadia Mono", "Cascadia Code", Consolas, monospace';
function fontStack(name) {
  const n = (name || '').trim();
  return n ? `"${n.replace(/"/g, '')}", ${defaultFont()}` : defaultFont();
}
async function applyFontFamily(name) {
  const stack = fontStack(name);
  // bundled fonts arrive as @font-face; wait for the face so xterm measures the real glyphs
  const n = (name || '').trim().replace(/"/g, '');
  if (n && document.fonts && typeof document.fonts.load === 'function') { try { await document.fonts.load(`14px "${n}"`); } catch { /* not a loadable face */ } }
  for (const r of terms.values()) { r.term.options.fontFamily = stack; }
  requestAnimationFrame(fitActive);
}

// Settings → Font: a dropdown (bundled fonts + ones used before) and a free-text box.
// Whichever was touched last is the one that counts; the other is greyed.
const BUNDLED_FONTS = ['Reddit Mono', 'JetBrains Mono', 'Fira Code', 'Source Code Pro', 'IBM Plex Mono', 'Inconsolata', 'Ubuntu Mono'];
let fontSource = 'select';
function fontHistory() { try { return JSON.parse(localStorage.getItem('deck.fonthistory') || '[]').filter(x => typeof x === 'string' && x.trim()); } catch { return []; } }
function rememberFont(name) {
  const n = (name || '').trim(); if (!n) return;
  const h = [n, ...fontHistory().filter(x => x.toLowerCase() !== n.toLowerCase())].slice(0, 12);
  localStorage.setItem('deck.fonthistory', JSON.stringify(h));
}
function setFontSource(src) {
  fontSource = src;
  $('#set-fontsel').classList.toggle('dim', src !== 'select');
  $('#set-fontfamily').classList.toggle('dim', src !== 'box');
}
function setupFontControls(current) {
  const sel = $('#set-fontsel'), box = $('#set-fontfamily');
  const names = ['Cascadia Mono (default)', ...BUNDLED_FONTS];
  for (const h of fontHistory()) if (!names.some(x => x.toLowerCase() === h.toLowerCase())) names.push(h);
  sel.innerHTML = '';
  for (const n of names) { const o = document.createElement('option'); o.value = n === 'Cascadia Mono (default)' ? '' : n; o.textContent = n; sel.appendChild(o); }
  const cur = (current || '').trim();
  const inList = !cur || names.some(x => x.toLowerCase() === cur.toLowerCase());
  sel.value = inList ? cur : ''; box.value = cur;
  setFontSource(inList ? 'select' : 'box');
  sel.onchange = () => { setFontSource('select'); box.value = sel.value; };
  sel.onfocus = () => setFontSource('select');
  box.oninput = () => setFontSource('box');
  box.onfocus = () => setFontSource('box');
}
function effectiveFont() { return fontSource === 'select' ? $('#set-fontsel').value : $('#set-fontfamily').value; }
const THEME = {
  background: '#0c0c0c', foreground: '#e6e6e6', cursor: '#e6e6e6', cursorAccent: '#0c0c0c', selectionBackground: '#3a3d41',
  black: '#0c0c0c', red: '#c50f1f', green: '#13a10e', yellow: '#c19c00', blue: '#0037da', magenta: '#881798', cyan: '#3a96dd', white: '#cccccc',
  brightBlack: '#767676', brightRed: '#e74856', brightGreen: '#16c60c', brightYellow: '#f9f1a5', brightBlue: '#3b78ff',
  brightMagenta: '#b4009e', brightCyan: '#61d6d6', brightWhite: '#f2f2f2',
};

function createTerm(id) {
  if (terms.has(id)) return terms.get(id);
  if (typeof layoutRestored !== 'undefined' && layoutRestored && !state.restoring) markStarting(id);   // a runtime-created tile is a fresh process
  const wrap = document.createElement('div');
  wrap.className = 'term'; wrap.dataset.id = id; mainEl.appendChild(wrap);
  const term = new Terminal({
    theme: THEME,
    fontFamily: fontStack(state.settings.fontFamily),
    fontSize: state.settings.fontSize || 14,
    lineHeight: 1.1,
    cursorBlink: true,
    allowProposedApi: true,
    scrollback: 10000,
    // ConPTY quirk handling is for Windows only; on a Mac the Option key must act as Meta so
    // Option+Enter, Option+arrows and friends reach Claude Code as the escape sequences it binds
    ...((state.platform && state.platform.isMac)
      ? { macOptionIsMeta: true, macOptionClickForcesSelection: true }
      : { windowsPty: { backend: 'conpty', buildNumber: 26200 } }),
  });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(wrap);
  // the pane's name, top-left, one size under the console (2026-09-08); painted by paintPaneNames.
  // Tapping it redraws the pane (2026-09-09).
  const name = document.createElement('div'); name.className = 'pane-name'; name.title = 'Click to redraw this pane · double-click to zoom it'; wrap.appendChild(name);
  // click = redraw; double-click = zoom the pane to the whole area and back (2026-09-14; 25-layout.js).
  // The redraw waits a beat for a second click, so a double-click zooms without redrawing first (the zoom's
  // own resize redraws the pane anyway).
  let nameClick = null;
  name.addEventListener('click', ev => { ev.stopPropagation(); clearTimeout(nameClick); nameClick = setTimeout(() => { nameClick = null; redrawTile(id); }, 250); });
  name.addEventListener('dblclick', ev => { ev.stopPropagation(); ev.preventDefault(); clearTimeout(nameClick); nameClick = null; zoomToggle(id); });
  // a hidden view cannot be fitted; give it the shown terminal's size so the process starts at the
  // right dimensions and switching to it later does not trigger a resize and repaint
  const shown = state.active && terms.get(state.active);
  if (shown && shown.term.cols > 1 && shown.term.rows > 1) term.resize(shown.term.cols, shown.term.rows);
  // No WebGL addon: Chromium allows ~16 WebGL contexts per page, and a deck with more tiles than that loses
  // the newest contexts (a white pane with a broken-image mark). xterm's default renderer has no such cap.

  term.onData(d => deck.input(id, d));
  term.onBinary(d => deck.input(id, d));
  let touched = false;
  term.onKey(() => { if (!touched) { touched = true; deck.userInput(id); } });   // a real key press: the tile is no longer fresh
  term.onTitleChange(t => deck.setOscTitle(id, t));
  term.buffer.onBufferChange(buf => {
    if (buf.type === 'alternate' && altPending.has(id)) { altPending.delete(id); if (typeof queuePaneRedraw === 'function') queuePaneRedraw([id], 1500); }
  });
  // Only a VISIBLE terminal drives its pty — the selected one, or every placed pane in a layout, each
  // at its own size. Hidden views are still held at the selected pane's size (fitActive, so switching to
  // one never reflows), but they must not push that size down: while a phone is attached to a tile the
  // deck is not showing, the phone owns the size. activate() takes it back.
  // With "one window per session" OFF a session may be on screen in several windows: only the focused
  // one pushes its size, so two panes fitting one pty do not fight (the others show it at that size).
  term.onResize(({ cols, rows }) => { if (isVisible(id) && !mirrorOf(id) && (oneWindow() || document.hasFocus())) deck.resize(id, cols, rows); });
  // a remote desk (the browser desk; renderer/desk/bridge.js): its clipboard is the BROWSER's, not this PC's
  const onDesk = typeof deck === 'object' && !!deck && !!deck._desk;
  term.attachCustomKeyEventHandler(ev => {
    if (ev.type !== 'keydown') return true;
    const k = ev.key.toLowerCase();
    const mac = state.platform && state.platform.isMac;
    if (!mac && (ev.key === 'F5' || ev.key === 'F9' || ev.key === 'F10' || ev.key === 'F11' || ev.key === 'F12')) return false;   // window keys: redraw, sidebar, fullscreen, prev/next tile
    if (!mac && ev.ctrlKey && !ev.shiftKey && !ev.altKey && ev.code === 'Backquote') return false;     // sidebar toggle, handled by the window
    if (ev.altKey && !ev.ctrlKey && !ev.metaKey && /^Digit[1-9]$/.test(ev.code)) return false;        // Alt+digit: the layout chord (85-focus.js), never ESC+digit to the pty
    if (mac && ev.metaKey && ((ev.shiftKey && ['p', '[', ']', '{', '}'].includes(k)) || (!ev.shiftKey && !ev.ctrlKey && (k === 'b' || k === 'r')) || (ev.ctrlKey && k === 'f'))) return false;   // Cmd window keys (Cmd+R redraws the pane)
    // Ctrl+Home / Ctrl+End: in a fullscreen app (Claude Code's TUI, alternate buffer) the app owns
    // scrolling and binds these keys itself, so let them through; in a plain shell scroll the terminal.
    // (Cmd+Up / Cmd+Down are the same gesture on a Mac, where Ctrl+Home/End do not exist on a laptop keyboard)
    const toTop = (ev.ctrlKey && ev.key === 'Home') || (mac && ev.metaKey && !ev.shiftKey && ev.key === 'ArrowUp');
    const toEnd = (ev.ctrlKey && ev.key === 'End') || (mac && ev.metaKey && !ev.shiftKey && ev.key === 'ArrowDown');
    if (toTop || toEnd) {
      const tile = state.tiles.find(t => t.id === id);
      if (term.buffer.active.type === 'alternate' || (tile && tile.kind === 'claude')) return true;
      if (toTop) term.scrollToTop(); else term.scrollToBottom();
      return false;
    }
    if (ev.key === 'Enter' && ev.shiftKey && !ev.ctrlKey && !ev.altKey) { deck.input(id, '\x1b\r'); return false; }   // newline in the prompt
    // Ctrl+C with text selected COPIES it, clears the selection and sends nothing to the session; with nothing
    // selected it is the interrupt, as ever (2026-11-18: in the browser desk "Ctrl+C risks interrupting the
    // Claude session, and Ctrl+Shift+C opens Chrome DevTools"). The same in the desktop app, as Windows Terminal
    // does it. Not on a Mac, where Cmd+C copies and Ctrl+C is documented as always the interrupt. preventDefault:
    // the browser's own copy would otherwise run after the selection is cleared and put nothing on the clipboard.
    if (!mac && ev.ctrlKey && !ev.shiftKey && !ev.altKey && !ev.metaKey && k === 'c') {
      const s = term.hasSelection() ? term.getSelection() : '';
      if (s) { ev.preventDefault(); deck.writeClipboard(s); term.clearSelection(); return false; }
    }
    if (ev.ctrlKey && ev.shiftKey && k === 'c') { const s = term.getSelection(); if (s) deck.writeClipboard(s); return false; }
    // macOS: Cmd+C copies the selection (Ctrl+C stays the interrupt), Cmd+V pastes; both bypass the
    // Edit-menu roles, which only reach ordinary text fields, not the terminal
    if (mac && ev.metaKey && !ev.shiftKey && !ev.altKey && !ev.ctrlKey && k === 'c') { const s = term.getSelection(); if (s) deck.writeClipboard(s); return false; }
    // PASTE ON A REMOTE DESK (2026-11-18: "the browser version not allowing paste"). The desktop app reads
    // the clipboard itself (below). A browser page may not: navigator.clipboard.readText() needs a secure
    // context AND the user's permission, and answers nothing when either is missing - and the Ctrl+V branch
    // below then sent the raw Ctrl+V (0x16) to the session, so Claude Code pasted the OFFICE PC's clipboard,
    // or nothing. So on a desk the key is simply left to the browser: no preventDefault, and `false` only
    // keeps xterm from sending the key. The browser's own `paste` event then reaches xterm's textarea and
    // xterm pastes its clipboardData - bracketed when the application asked - with no permission needed (a
    // paste the user made is always allowed). Ctrl+V, Ctrl+Shift+V and Cmd+V (a Mac's browser) all end here;
    // Shift+Insert never came through this handler and takes the same road. deskPaste() below is the rest.
    if (onDesk && !ev.altKey && k === 'v' && (ev.ctrlKey || ev.metaKey)) return false;
    if (mac && ev.metaKey && !ev.shiftKey && !ev.altKey && !ev.ctrlKey && k === 'v') {
      ev.preventDefault();   // returning false only keeps xterm from handling the key; the browser would still paste into its textarea, and xterm pastes that too
      deck.readClipboard().then(t => { if (t) { term.paste(t); deck.userInput(id); } });
      return false;
    }
    if (ev.ctrlKey && ev.shiftKey && k === 'i') return false;                                  // devtools, handled by the window
    if (ev.ctrlKey && !ev.altKey && k === 'v') {
      ev.preventDefault();   // returning false only keeps xterm from handling the key; the browser would still paste into its textarea, and xterm pastes that too
      deck.readClipboard().then(t => { if (t) { term.paste(t); deck.userInput(id); } else deck.input(id, '\x16'); });
      return false;
    }
    return true;
  });
  wrap.addEventListener('contextmenu', ev => {
    const s = term.getSelection();
    if (onDesk) {
      // A remote desk. A selection is copied, as in the desktop app. With none, the right click pastes when
      // the browser has ALREADY said this page may read the clipboard (deck._desk.clipboardReadable, kept
      // current by the bridge) - and in every pane: a desk sends no mouse reports (R8), so the application
      // never sees the click and never pastes by itself. Otherwise the click is left alone and the BROWSER's
      // own menu opens, with Paste in it (xterm has already put its textarea under the pointer for exactly
      // that): never a right click that silently does nothing, never a read that can only fail.
      if (s) { ev.preventDefault(); deck.writeClipboard(s); term.clearSelection(); return; }
      if (!deck._desk.clipboardReadable) return;
      ev.preventDefault();
      deck.readClipboard().then(t => { if (t) { term.paste(t); deck.userInput(id); } });
      return;
    }
    ev.preventDefault();
    if (s) { deck.writeClipboard(s); term.clearSelection(); return; }
    // An app that has mouse reporting on gets the right-click itself (xterm sends the SGR report), and
    // Claude Code answers a right-click by pasting the clipboard on its own — so the deck pasting too put
    // the text in twice (2026-09-10; verified: one bracketed paste from the deck, the marker on
    // screen twice). The deck pastes only where nobody else will: a shell, or an app that ignores the mouse.
    const appHasMouse = term.modes && term.modes.mouseTrackingMode && term.modes.mouseTrackingMode !== 'none';
    if (!appHasMouse) deck.readClipboard().then(t => { if (t) term.paste(t); });
  });
  wrap.addEventListener('mousedown', () => { if (state.active !== id) activate(id); });

  deskSelectByDrag(term);
  if (onDesk) deskPaste(id, wrap);
  if (onDesk) deskNoReplies(term, id);
  if (onDesk) deskCopyOnSelect(term, wrap);
  if (onDesk) deskTouchSelect(term, wrap);
  if (onDesk) deskClickThrough(id, term, wrap);
  const rec = { term, fit, wrap };
  terms.set(id, rec);
  deck.attach(id);
  return rec;
}

// The other half of paste on a remote desk (the key handler in createTerm leaves the key to the browser): the
// browser's own `paste` event, on its way down to xterm's textarea. Heard here first (capture phase):
//   - it is real input from this desk, as a key press is: deck.userInput(id);
//   - a paste with NO text (an image, a file, an empty clipboard) stops here. xterm would send an empty
//     bracketed paste, which Claude Code answers by reading the clipboard of the PC it runs on - the office's,
//     not this desk's - and pasting whatever picture is on it.
// The desktop app never gets here: it pastes from Electron's clipboard and prevents the browser's paste.
function deskPaste(id, wrap) {
  wrap.addEventListener('paste', ev => {
    let text = '';
    try { text = (ev.clipboardData && ev.clipboardData.getData('text/plain')) || ''; } catch { /* no data: treated as empty */ }
    if (!text) {
      ev.preventDefault(); ev.stopPropagation();
      // a PICTURE (2026-11-19, item 6): it goes to the deck, which saves it and answers its path; the
      // path is pasted, and Claude Code takes a pasted image path as the picture. A deck from before the action
      // answers "unknown action": it needs its next restart.
      let file = null;
      try { const it = [...((ev.clipboardData && ev.clipboardData.items) || [])].find(i => i.kind === 'file' && /^image\/(png|jpeg|gif|webp)$/.test(i.type)); file = it ? it.getAsFile() : null; } catch { /* none */ }
      if (file && deck.pasteImage) deskPastePicture(id, file, wrap);
      return;
    }
    deck.userInput(id);
  }, true);
}
// TERMINAL REPLIES on a remote desk (2026-11-19, item 14b: exactly one answerer per session - "never two",
// a second answer is junk in Claude's prompt, and never none, a stall). The Electron office window has a terminal
// for every session and answers every query a program asks (cursor position, device attributes, colours), so under
// it a desk answers nothing. The C++ office's windows have terminals only for what they show; that deck names the
// one answerer per tile in meta `answerer` (core/deck/remoterule answerer: its window that shows it, else the remote
// desk, else the phone), and a desk answers exactly when it is named "remote" (2026-11-23) - never for a sleeping
// tile (any input wakes it) and never what a replay asks (deck.onData: a query asked long ago, answered into whatever
// runs now), unless the session has only just started (its first output came before the pane was attached), as the
// native exe does. xterm raises its own replies as NOT user input (CoreService.triggerDataEvent(data, false)); those
// stop here unless the desk answers, and keys, paste and the mouse (user input) always go on. If a later xterm
// renames this, the desk answers as before and nothing else breaks. desk-test checks it.
function deskAnswers(id) {
  const t = state.tiles.find(x => x.id === id);
  if (!t || t.answerer !== 'remote' || t.status === 'asleep') return false;
  const r = terms.get(id);
  return !(r && r.replaying > 0);
}
function deskNoReplies(term, id) {
  try {
    const cs = term._core && term._core.coreService;
    if (!cs || typeof cs.triggerDataEvent !== 'function') return;
    const orig = cs.triggerDataEvent.bind(cs);
    cs.triggerDataEvent = (data, wasUserInput = false) => { if (wasUserInput) orig(data, true); else if (deskAnswers(id)) orig(data, false); };
  } catch { /* the desk answers as before */ }
}
function deskPastePicture(id, file, wrap) {
  if (file.size > 20 * 1024 * 1024) { deskCopiedNote(wrap, 'The picture is over 20 MB: not sent'); return; }
  deskCopiedNote(wrap, 'Sending the picture\u2026');
  const rd = new FileReader();
  rd.onload = () => {
    const b64 = String(rd.result || '').replace(/^data:[^,]*,/, '');
    Promise.resolve(deck.pasteImage(id, b64, file.type)).then(p => {
      const r = terms.get(id);
      if (r && typeof p === 'string' && p) { r.term.paste(p); deck.userInput(id); deskCopiedNote(wrap, 'Picture pasted'); }
    }).catch(e => deskCopiedNote(wrap, /unknown action/.test(String((e && e.message) || e)) ? 'This deck cannot take pictures yet: it needs its next restart' : 'The picture was not taken: ' + String((e && e.message) || e)));
  };
  rd.readAsDataURL(file);
}

// COPY ON SELECT on a remote desk (2026-11-19, the remote: "it looks like I would have to ctrl+C, it doesn't
// behave like the normal terminal"). Over a full-screen Claude Code session their own terminal lets the application
// select and copy on release; a desk sends no mouse reports (R8), so the selection is the desk's own - and it is
// copied the moment the mouse is released over it (a drag, a double or triple click, Shift+click), with a short
// "Copied" in the pane. The selection stays on screen; Ctrl+C and the right click are unchanged. Per BROWSER, on by
// default: localStorage 'deck.desk.copyOnSelect' = '0' turns it off (the palette's "Copy on select" toggles it).
// The desktop app is untouched (select, then Ctrl+C or right-click, as before). Checked by test/desk/desk-test.js.
function deskCopyOnSelectOn() { try { return localStorage.getItem('deck.desk.copyOnSelect') !== '0'; } catch { return true; } }
function setDeskCopyOnSelect(on) { try { localStorage.setItem('deck.desk.copyOnSelect', on ? '1' : '0'); } catch { /* this browser keeps nothing: on, as by default */ } }
function deskCopyOnSelect(term, wrap) {
  let down = false;
  wrap.addEventListener('mousedown', ev => { if (ev.button === 0) down = true; }, true);
  // on the window: a drag may end outside the pane
  window.addEventListener('mouseup', ev => {
    if (!down || ev.button !== 0) return;
    down = false;
    if (!deskCopyOnSelectOn()) return;
    setTimeout(() => {                       // after xterm has finished the selection (a double click makes it on this same release)
      const s = term.hasSelection() ? term.getSelection() : '';
      if (!s) return;
      // the copy may go through a hidden textarea (a page that is not https has no navigator.clipboard): the
      // keyboard comes back to the terminal after it
      Promise.resolve(deck.writeClipboard(s)).finally(() => { try { term.focus(); } catch { /* gone */ } });
      deskCopiedNote(wrap);
    }, 0);
  }, true);
}
// TOUCH SELECTION ON A REMOTE DESK (2026-11-20: the desk on a PHONE "has no touch selection" - a finger drag
// scrolls). A finger held still for TOUCH_HOLD_MS starts a selection at its cell; dragging then extends it (the page
// does not scroll while it does); lifting the finger copies it, as copy on select does for a mouse release
// (deskCopyOnSelectOn, "Copied" in the pane). A finger that moves first is a scroll, as before; the long press's
// own context menu (the right click's copy / paste) stands down. Touch only: a mouse never fires these events.
// Checked by test/desk/desk-test.js.
const TOUCH_HOLD_MS = 500, TOUCH_SLOP_PX = 10;
function deskTouchSelect(term, wrap) {
  let hold = null, from = null, startXY = null, selectingAt = 0;
  const cellOf = (x, y) => {
    const scr = wrap.querySelector('.xterm-screen'); if (!scr) return null;
    const b = scr.getBoundingClientRect(); if (!b.width || !b.height) return null;
    const col = Math.max(0, Math.min(term.cols - 1, Math.floor((x - b.left) / (b.width / term.cols))));
    const row = Math.max(0, Math.min(term.rows - 1, Math.floor((y - b.top) / (b.height / term.rows))));
    return { col, row: row + term.buffer.active.viewportY };
  };
  const selectTo = c => {
    if (!from || !c) return;
    const [a, z] = (c.row < from.row || (c.row === from.row && c.col < from.col)) ? [c, from] : [from, c];
    term.select(a.col, a.row, (z.row - a.row) * term.cols + (z.col - a.col) + 1);
  };
  const cancelHold = () => { if (hold) { clearTimeout(hold); hold = null; } };
  wrap.addEventListener('touchstart', ev => {
    cancelHold(); from = null;
    if (ev.touches.length !== 1) return;
    const t = ev.touches[0]; startXY = { x: t.clientX, y: t.clientY };
    hold = setTimeout(() => {
      hold = null;
      from = cellOf(startXY.x, startXY.y);
      if (!from) return;
      selectingAt = Date.now();
      selectTo(from);
    }, TOUCH_HOLD_MS);
  }, { capture: true, passive: true });
  wrap.addEventListener('touchmove', ev => {
    const t = ev.touches[0]; if (!t) return;
    if (from) { ev.preventDefault(); ev.stopPropagation(); selectTo(cellOf(t.clientX, t.clientY)); return; }
    if (hold && startXY && Math.hypot(t.clientX - startXY.x, t.clientY - startXY.y) > TOUCH_SLOP_PX) cancelHold();   // a scroll
  }, { capture: true, passive: false });
  const end = ev => {
    cancelHold();
    if (!from) return;
    from = null;
    if (ev.cancelable) ev.preventDefault();            // no mouse events made from this touch: they would clear it
    if (ev.type !== 'touchend' || !deskCopyOnSelectOn()) return;
    const s = term.hasSelection() ? term.getSelection() : '';
    if (!s) return;
    Promise.resolve(deck.writeClipboard(s)).catch(() => { /* the note still shows; Ctrl+C remains */ });
    deskCopiedNote(wrap);
  };
  wrap.addEventListener('touchend', end, { capture: true, passive: false });
  wrap.addEventListener('touchcancel', end, { capture: true, passive: false });
  // a long press is also a context menu (the right click: copy / paste): not while the finger selects
  wrap.addEventListener('contextmenu', ev => {
    if (from || hold || Date.now() - selectingAt < 1500) { ev.preventDefault(); ev.stopImmediatePropagation(); }
  }, true);
}
// CLICKS ON A REMOTE DESK (2026-11-19: "clicks in the remote don't work, like to tell it how well claude is
// doing, or the yes/no questions"). A desk makes no mouse report of its own (deskSelectByDrag), so Claude Code's
// clickable prompts never saw a click. Now a LEFT CLICK - the press and the release on the same cell, no selection
// made between them, no Shift - goes to the session as one SGR press and its release at the press's cell, on the
// release (only then is it known not to be a drag), while the deck's meta says the session tracks the mouse in SGR
// (bridge.js clickAsked, the wheel's gate). A drag stays the desk's selection (copied on release), the right button
// stays copy / paste, Shift+click extends; a double click's first click is a click. Checked by test/desk/desk-test.js.
function deskClickThrough(id, term, wrap) {
  let at = null;
  const cellOf = ev => {
    const scr = wrap.querySelector('.xterm-screen'); if (!scr) return null;
    const b = scr.getBoundingClientRect(); if (!b.width || !b.height) return null;
    const col = Math.floor((ev.clientX - b.left) / (b.width / term.cols)) + 1, row = Math.floor((ev.clientY - b.top) / (b.height / term.rows)) + 1;
    return { col: Math.max(1, Math.min(term.cols, col)), row: Math.max(1, Math.min(term.rows, row)) };
  };
  wrap.addEventListener('mousedown', ev => {
    at = null;
    if (ev.button !== 0 || ev.shiftKey || ev.detail > 1) return;
    const mode = term.modes && term.modes.mouseTrackingMode;
    if (!mode || mode === 'none' || mode === 'x10') return;
    if (!(deck._desk.clicksAsked && deck._desk.clicksAsked(id))) return;
    const c = cellOf(ev); if (c) at = { ...c, mods: (ev.altKey ? 8 : 0) | (ev.ctrlKey ? 16 : 0) };
  }, true);
  window.addEventListener('mouseup', ev => {
    if (!at || ev.button !== 0) return;
    const c = at; at = null;
    const up = cellOf(ev);
    setTimeout(() => {                       // after xterm has settled the selection for this release
      if (term.hasSelection() || !up || up.col !== c.col || up.row !== c.row) return;   // a drag: the desk's own selection
      deck.input(id, `\x1b[<${c.mods};${c.col};${c.row}M`);
      deck.input(id, `\x1b[<${c.mods};${c.col};${c.row}m`);
      deck.userInput(id);
    }, 0);
  }, true);
}
function deskCopiedNote(wrap, text = 'Copied') {
  let n = wrap.querySelector(':scope > .desk-copied');
  if (!n) {
    n = document.createElement('div'); n.className = 'desk-copied';
    n.style.cssText = 'position:absolute;right:10px;top:8px;z-index:5;padding:2px 8px;border-radius:4px;font:12px sans-serif;background:rgba(0,0,0,.72);color:#fff;pointer-events:none;transition:opacity .3s';
    if (getComputedStyle(wrap).position === 'static') wrap.style.position = 'relative';
    wrap.appendChild(n);
  }
  n.textContent = text; n.style.opacity = '1';
  clearTimeout(n._t); n._t = setTimeout(() => { n.style.opacity = '0'; }, 900);
}

// In a remote desk a plain drag selects text even while the application has mouse tracking on (Claude Code's
// TUI). xterm switches its own selection off while an application tracks the mouse and leaves the drag to the
// application (Shift+drag forces a selection); but a desk sends no mouse reports at all (R8, renderer/desk/
// bridge.js), so there the drag did nothing. The desktop app is untouched: its drags still go to the application.
// This reaches into xterm's internals (no public option exists; macOptionClickForcesSelection is the nearest
// and is Mac-only), in two places, both needed: the selection service must stay on, and the terminal must not
// build the mouse reports at all — xterm counts each report as user input and clears the selection on it (the
// bridge dropped them anyway). If a later xterm renames either, the desk is back to Shift+drag and nothing
// else breaks. Checked by test/desk/desk-test.js.
// The WHEEL is the exception (2026-11-18: "I can't scroll up" from a remote desk): its report is still
// built, and the bridge lets it out when the session asked for it (bridge.js, R8 narrowed for the wheel) —
// a full-screen application scrolls by nothing else. A wheel notch therefore clears a selection, as typing does.
function deskSelectByDrag(term) {
  if (!(typeof deck === 'object' && deck && deck._desk)) return;
  try {
    const core = term._core, sel = core && core._selectionService, mouse = core && core.coreMouseService;
    if (!sel || typeof sel.enable !== 'function' || typeof sel.disable !== 'function' || !mouse || typeof mouse.triggerMouseEvent !== 'function') return;
    const report = mouse.triggerMouseEvent.bind(mouse);
    const WHEEL = 4;                         // xterm's CoreMouseButton.WHEEL
    // no report is made (false = "not sent", what xterm answers for an event nobody asked for) — but the wheel's
    mouse.triggerMouseEvent = e => (e && e.button === WHEEL ? report(e) : false);
    sel.disable = () => {};                  // an application turning mouse tracking on no longer switches selection off
    sel.enable();
  } catch { /* Shift+drag still selects */ }
}

function disposeTerm(id) {
  const r = terms.get(id); if (!r) return;
  try { r.term.dispose(); } catch { /* ignore */ }
  r.wrap.remove(); terms.delete(id);
}

// While a phone is attached to a tile, the phone's size wins and this view matches it exactly:
// the same columns and rows, at whatever font size makes those rows fit the pane. Both ends then
// render the session identically and neither one reflows it under the other.
function mirrorOf(id) {
  const t = state.tiles.find(x => x.id === id);
  return (t && t.mirror && t.mirror.cols > 1 && t.mirror.rows > 1) ? t.mirror : null;
}
function applyMirror(r, m) {
  const h = r.wrap.clientHeight;
  const base = state.settings.fontSize || 14;
  // xterm's row box is fontSize * lineHeight (1.1 here); shrink only as far as needed
  const fit = h > 0 ? Math.floor(h / (m.rows * 1.1)) : base;
  const size = Math.max(6, Math.min(base, fit || base));
  if (r.term.options.fontSize !== size) r.term.options.fontSize = size;
  if (r.term.cols !== m.cols || r.term.rows !== m.rows) r.term.resize(m.cols, m.rows);
  // The phone asks for a screen several times taller than its window (up to 600 rows), and even at
  // the smallest face that is taller than this pane. Show its BOTTOM, where the input box is, so the
  // desk can still see the prompt and type into it; the rest is above, out of view.
  requestAnimationFrame(() => {
    const el = r.term.element; if (!el) return;
    const over = el.getBoundingClientRect().height - r.wrap.clientHeight;
    el.style.transform = over > 1 ? `translateY(${-Math.round(over)}px)` : '';
  });
}
// Fit every visible pane to its own box — one pane with no layout, up to four with one — then hold the
// hidden views at the SELECTED pane's size, so switching to one never resizes the pty underneath it.
function fitOne(id) {
  const r = terms.get(id); if (!r) return;
  const m = mirrorOf(id);
  if (m) { applyMirror(r, m); return; }               // the phone owns the size; do not fit over it
  if (r.term.element && r.term.element.style.transform) r.term.element.style.transform = '';   // no mirror: the whole terminal fits
  // a zoomed pane (25-layout.js) takes Settings > Zoomed pane font size when one is set (2026-09-28)
  const px = (state.zoom === id && state.settings.zoomFontSize) || state.settings.fontSize || 14;
  if (r.term.options.fontSize !== px) r.term.options.fontSize = px;
  try { r.fit.fit(); } catch { /* not laid out yet */ }
}
function fitActive() {
  // The ResizeObserver below can fire while later scripts (25-layout.js, which defines visibleIds) are
  // still being fetched — a rendering opportunity between parser-inserted scripts — and logged a
  // ReferenceError on every view load. Nothing to fit that early.
  if (typeof visibleIds !== 'function') return;
  const vis = visibleIds().filter(id => terms.has(id));
  if (!vis.length) return;
  for (const id of vis) if (!zoomHidden(id)) fitOne(id);   // a pane hidden behind a zoom keeps its size
  const r = terms.get(vis.includes(state.active) ? state.active : vis[0]);
  const { cols, rows } = r.term;
  if (cols > 1 && rows > 1) for (const [id, o] of terms) if (!vis.includes(id) && (o.term.cols !== cols || o.term.rows !== rows)) o.term.resize(cols, rows);
}

// Redraw ONE pane (2026-09-09): tap its name, or F5 / Cmd+R for the selected pane. The terminal is
// reset and re-attached, then refitted, and the pty is nudged one column narrower and back so a
// full-screen app (Claude Code's TUI) repaints its own screen rather than trusting what it thinks is
// there. The replay is only what the session wrote SINCE the pane last changed size: output from a
// different width re-wraps wrongly however often it is replayed, and the app never repaints its
// history (2026-09-09) — so that part is dropped from the view rather than shown
// garbled. Sessions untouched.
function redrawTile(id) {
  if (typeof cloudRedraw === 'function' && cloudRedraw(id)) return;   // a cloud pane: its page loads again (48-cloud.js)
  const r = terms.get(id); if (!r) return;
  r.term.reset();
  deck.attach(id, { sinceResize: true });
  r.wrap.classList.add('redrawn'); setTimeout(() => r.wrap.classList.remove('redrawn'), 600);
  // a timer, not requestAnimationFrame: a covered window gets no frames, and the nudge must still go
  setTimeout(() => {
    fitOne(id);
    const { cols, rows } = r.term;
    // main does the narrower-and-back pair itself: two deck.resize calls 80 ms apart fell inside the
    // 150 ms debounce and cancelled out, so the first redraw never resized anything
    if (isVisible(id) && !mirrorOf(id) && cols > 2 && rows > 1) deck.nudge(id, cols, rows);
  }, 50);
}
deck.onRedraw(() => { if (state.active) redrawTile(state.active); });

function applyFontSize(px) {
  for (const r of terms.values()) { if (r.term.options.fontSize !== px) r.term.options.fontSize = px; }
  paintPaneNames(px);
  requestAnimationFrame(fitActive);
}
// Every pane carries its tile's name in the top-left corner, in the console's font one size smaller.
// Cheap enough to run on every tile repaint (the 2 s tick): a text compare per terminal.
function paintPaneNames(fontPx) {
  const first = terms.values().next().value;
  const size = fontPx || (first && first.term.options.fontSize) || state.settings.fontSize || 14;
  const px = Math.max(8, size - 1) + 'px';
  if (mainEl.style.getPropertyValue('--pane-name-size') !== px) mainEl.style.setProperty('--pane-name-size', px);
  if (mainEl.style.getPropertyValue('--pane-font-size') !== size + 'px') mainEl.style.setProperty('--pane-font-size', size + 'px');   // the label's growing pulse climbs to this
  const family = fontStack(state.settings.fontFamily);
  if (mainEl.style.getPropertyValue('--pane-name-font') !== family) mainEl.style.setProperty('--pane-name-font', family);
  for (const [id, r] of terms) {
    const label = r.wrap.querySelector('.pane-name'); if (!label) continue;
    const t = state.tiles.find(x => x.id === id);
    r.wrap.classList.toggle('desk-attn', !!(t && t.attention));   // a desk the on-call just opened: the pane pulses until the user types into it
    // the sidebar line, one to one: [abbr] title — the abbreviation when the account has one, green while busy;
    // a cloud session wears the cloud mark right after it
    const a = t && (t.kind === 'claude' || t.kind === 'cloud') ? acct(t.account) : null;
    const abbr = a && a.abbr ? a.abbr : '';
    const cloudy = !!t && t.kind === 'cloud';
    const text = t ? tileTitle(t) : '';
    const busy = !!t && t.status === 'busy';
    const ask = t && t.kind === 'claude' && typeof askMarkInfo === 'function' ? askMarkInfo(t.sessionId) : null;   // 49-asks.js
    const sig = `${abbr}|${cloudy ? 1 : 0}|${busy ? 1 : 0}|${text}|${ask ? ask.kind + ask.text : ''}`;
    if (label.dataset.sig === sig) continue;
    label.dataset.sig = sig;
    label.textContent = '';
    if (abbr) { const ab = document.createElement('span'); ab.className = 'abbr' + (busy ? ' busy' : ''); ab.textContent = abbr; label.appendChild(ab); }
    if (cloudy && typeof CLOUD_SVG === 'string') { const c = document.createElement('span'); c.className = 'cloud-mark' + (busy ? ' busy' : ''); c.innerHTML = CLOUD_SVG; label.appendChild(c); }
    if (abbr || cloudy) label.appendChild(document.createTextNode(' '));
    label.appendChild(document.createTextNode(text));
    if (ask) { const m = document.createElement('span'); m.className = 't-ask pane-ask' + (ask.kind === 'drafted' ? ' drafted' : ask.kind === 'sent' ? ' sent' : ''); m.textContent = ask.text; m.title = ask.title; label.appendChild(m); }
    label.title = (abbr ? `${abbr} ` : '') + (cloudy ? '(cloud) ' : '') + text + (ask ? ` · ${ask.title}` : '');
  }
}

// opts.adopt (default true): in a layout, selecting an unplaced tile gives it the selected pane (R12) or
// the selected empty slot (25-layout.js). A single click on a sidebar tile passes adopt:false — only a
// double-click, a drag, the chord or F11/F12 place (2026-09-10); a click must never evict a pane.
function activate(id, opts = {}) {
  const prev = state.active;
  if (typeof asksBlur === 'function') asksBlur();          // a session selected: the asks tile is no longer the selection (49-asks.js)
  state.active = id;
  localStorage.setItem(lsKey('deck.active'), id || '');
  // zoomed and another placed session gets selected (a sidebar click, the palette): the zoom follows it
  if (state.zoom && id && id !== state.zoom && isPlaced(id)) state.zoom = id;
  if (id && id !== prev && opts.adopt !== false) layoutAdopt(id, prev);
  else if (id && isPlaced(id)) clearEmptySlot();     // selecting a real pane drops any empty-slot selection
  for (const [tid, r] of terms) r.wrap.classList.toggle('active', tid === id);
  // applyLayout paints the panes, hides or shows the empty notice, and tells the deck what is on screen
  // (deck.setShown — the ONLY honest answer: every tile gets a terminal and an attach at boot, so
  // `attached` says nothing; hibernation reads this and wakes a sleeping tile that lands on screen)
  applyLayout();
  // push the size on activate, even when the local view did not change size: a phone may have
  // resized this pty while the deck was showing something else
  requestAnimationFrame(() => {
    fitActive();
    const r = terms.get(id);
    if (r) {
      if (!mirrorOf(id) && r.term.cols > 1 && r.term.rows > 1) deck.resize(id, r.term.cols, r.term.rows);
      if (!(typeof focusCloud === 'function' && focusCloud(id))) r.term.focus();   // a cloud pane: its page takes the keys
    }
  });
  // keep the active tile in view when it was chosen by keyboard (F11/F12, the palette)
  const el = tileEls.get(id); if (el) el.scrollIntoView({ block: 'nearest' });
}

new ResizeObserver(() => fitActive()).observe(mainEl);

// A session that has just come up — opened, woken, refreshed, restarted, moved to another account — gets
// the F5 redraw once its new process has drawn (2026-09-10): main sends tile:reset for every respawn
// and a tile created at runtime is a fresh process too; the first output after that is the new process's,
// and 2.5 s later its first screen is up. Boot-time terminals are not marked: their first output is the
// replay, and a view reload would redraw every pane for nothing.
// Two triggers per marked tile: the first output (a shell's prompt is up within a second of it) and,
// for a full-screen app, its switch INTO the alternate screen — the moment Claude Code draws its first
// frame. An account switch (`open <acc>`) is why the second exists (2026-09-10): its first output
// is userswitch's own text, and Claude Code's screen comes several seconds later, after a redraw timed
// from the first output had already run.
const startPending = new Set(), altPending = new Set();
function markStarting(id) { startPending.add(id); altPending.add(id); }
deck.onData(({ id, data, replay }) => {
  const r = terms.get(id); if (!r) return;
  // a replay (a remote desk's attach: bridge.js passes the server's `replay`) is not answered (deskAnswers), unless
  // the session has only just started; counted until xterm has parsed it
  if (replay && !startPending.has(id)) { r.replaying = (r.replaying || 0) + 1; r.term.write(data, () => { r.replaying--; }); }
  else r.term.write(data);
  if (startPending.has(id)) { startPending.delete(id); if (typeof queuePaneRedraw === 'function') queuePaneRedraw([id], 2500); }
});
deck.onExit(({ id, exitCode }) => { const r = terms.get(id); if (r) r.term.write(`\r\n\x1b[90m[process exited with code ${exitCode}]\x1b[0m\r\n`); });
deck.onReset(({ id }) => { const r = terms.get(id); if (r) { r.term.reset(); r.term.clear(); markStarting(id); } });
