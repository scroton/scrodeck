'use strict';
/* global deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- cloud sessions (2026-10-12)
// A cloud tile holds a Claude Code session that runs in Anthropic's cloud, not on this machine. Its pane is
// the session's own claude.ai page in a <webview> laid over the tile's terminal (which keeps every code path
// that expects one; it shows the `claude --cloud` run that made the session, then sits under the page).
// Each account's panes share one login partition, `persist:claude-<account>`: sign in once per account, in
// the first of its panes. main.js guards what a pane may load (guardWebview) and where it may go (paneUrl).
// A browser desk has no <webview>, so there the pane is a card with a link to the page.
// Right-click an account's button under "New" for "New <acct> cloud session…": the task is its first prompt.
const CLOUD_SVG = '<svg class="cloud-ico" viewBox="0 0 16 16" aria-hidden="true"><path d="M4.4 13.2h7.6a3.2 3.2 0 0 0 .55-6.35A4.4 4.4 0 0 0 4.06 6.3 3.45 3.45 0 0 0 4.4 13.2z"/></svg>';
const canWebview = /\bElectron\//.test(navigator.userAgent);
const CLOUD_IDLE_DROP_MS = 15 * 60_000;    // a page off screen this long is dropped (a webview is a process); rebuilt when shown
const cloudPanes = new Map();               // tileId -> { el, cloudId, account, shownAt, hint }

const cloudTileOf = id => { const t = state.tiles.find(x => x.id === id); return t && t.kind === 'cloud' ? t : null; };
const cloudSuffix = id => String(id || '').replace(/^(session_|cse_)/, '');
function cloudAcctEmail(accountId) { const a = acct(accountId); return (a && a.email) || accountId || ''; }

// ---- the panes: built when a cloud tile with a session first shows, kept while it is used
function paintCloudPanes() {
  const now = Date.now();
  for (const [id, r] of terms) {
    const t = cloudTileOf(id);
    const want = t && t.cloudId ? t : null;
    r.wrap.classList.toggle('cloud', !!want);
    let have = cloudPanes.get(id);
    if (have && (!want || have.cloudId !== want.cloudId || have.account !== want.account)) { dropCloudPane(id); have = null; }
    if (!want) continue;
    const vis = isVisible(id);
    if (have) {
      if (vis) have.shownAt = now;
      else if (now - have.shownAt > CLOUD_IDLE_DROP_MS) dropCloudPane(id);
      continue;
    }
    if (vis) cloudPanes.set(id, buildCloudPane(id, want, r.wrap));
  }
  for (const id of [...cloudPanes.keys()]) if (!terms.has(id)) dropCloudPane(id);
}
function dropCloudPane(id) {
  const p = cloudPanes.get(id); if (!p) return;
  p.el.remove(); if (p.hint) p.hint.remove();
  cloudPanes.delete(id);
}
function buildCloudPane(id, t, wrap) {
  if (!canWebview) {
    // the browser desk: no <webview> in a browser, and claude.ai will not be framed — a link to it instead
    const card = document.createElement('div'); card.className = 'cloud-pane cloud-card';
    const head = document.createElement('div'); head.className = 'cc-head'; head.innerHTML = CLOUD_SVG;
    head.appendChild(document.createTextNode(' Cloud session'));
    const what = document.createElement('div'); what.className = 'cc-text';
    what.textContent = 'It runs on claude.ai, not on the deck\'s machine. Open it there to read it and answer it.';
    const a = document.createElement('a'); a.className = 'cc-link'; a.href = t.cloudUrl; a.target = '_blank'; a.rel = 'noopener'; a.textContent = 'Open on claude.ai';
    card.append(head, what, a);
    wrap.appendChild(card);
    return { el: card, cloudId: t.cloudId, account: t.account, shownAt: Date.now(), hint: null };
  }
  const wv = document.createElement('webview');
  wv.className = 'cloud-pane';
  wv.setAttribute('partition', `persist:claude-${t.account}`);
  wv.setAttribute('allowpopups', '');      // sign-in popups; main.js lets only sign-in pages open as one
  wv.setAttribute('src', t.cloudUrl);
  // a click in the page is a click in the pane: it selects the tile, as a click in a terminal does
  wv.addEventListener('focus', () => { if (state.active !== id) activate(id); });
  // the first visit in an account's partition lands on claude.ai's sign-in page: say which login it wants
  const hint = document.createElement('div'); hint.className = 'cloud-hint'; hint.hidden = true;
  const onNav = ev => {
    const url = String(ev.url || '');
    const signIn = /^https:\/\/claude\.ai\/(login|signup|magic-link)/.test(url) || !/^https:\/\/([a-z0-9-]+\.)*claude\.ai\//.test(url);
    hint.hidden = !signIn;
    if (signIn) {
      const email = cloudAcctEmail(t.account), label = acctLabel(t.account);
      hint.textContent = `Sign in as ${email}${label && label !== email ? ` (${label})` : ''}. Once is enough: every cloud pane of this account shares the login.`;
    }
  };
  wv.addEventListener('did-navigate', onNav);
  wv.addEventListener('did-navigate-in-page', ev => { if (ev.isMainFrame) onNav(ev); });
  wrap.append(wv, hint);
  return { el: wv, cloudId: t.cloudId, account: t.account, shownAt: Date.now(), hint };
}
// The pane's page has the focus instead of the terminal under it (activate in 20-terminals.js asks).
function focusCloud(id) { const p = cloudPanes.get(id); if (!p || !canWebview) return false; try { p.el.focus(); } catch { /* not attached yet */ } return true; }
// F5, a tap on the pane's name, or the tile's `reload`: the page again — back to the session if it wandered.
function cloudRedraw(id) {
  const p = cloudPanes.get(id), t = cloudTileOf(id);
  if (!p || !t || !canWebview) return false;
  try {
    const at = p.el.getURL ? p.el.getURL() : '';
    if (at && at.includes(cloudSuffix(t.cloudId))) p.el.reload(); else p.el.loadURL(t.cloudUrl);
  } catch { /* still attaching */ }
  const r = terms.get(id); if (r) { r.wrap.classList.add('redrawn'); setTimeout(() => r.wrap.classList.remove('redrawn'), 600); }
  return true;
}
// The page in the default browser (the desk has no shell: a plain new tab there).
function openCloudOutside(t) {
  if (!t || !t.cloudUrl) return;
  if (canWebview && typeof deck.openExternal === 'function') deck.openExternal(t.cloudUrl).catch(e => reportToActive(e));
  else window.open(t.cloudUrl, '_blank', 'noopener');
}

// ---- the dialog: right-click an account's button under "New" -> "New <acct> cloud session…"
let cloudDlg = null;
function closeCloudDialog() { if (cloudDlg) { cloudDlg.remove(); cloudDlg = null; } }
function openCloudDialog(a) {
  closeCloudDialog();
  const back = document.createElement('div'); back.className = 'cloud-dlg-back';
  const box = document.createElement('div'); box.className = 'cloud-dlg';
  const title = document.createElement('div'); title.className = 'cd-title';
  title.innerHTML = CLOUD_SVG;
  title.appendChild(document.createTextNode(` New cloud session · ${a.label}`));
  const ta = document.createElement('textarea'); ta.className = 'cd-task'; ta.rows = 7; ta.spellcheck = true;
  ta.placeholder = 'What should it do? This is its first prompt.';
  const folderRow = document.createElement('label'); folderRow.className = 'cd-folder';
  const fl = document.createElement('span'); fl.textContent = 'Folder';
  const folder = document.createElement('input'); folder.type = 'text'; folder.spellcheck = false;
  let lastFolder = ''; try { lastFolder = localStorage.getItem('deck.cloud.cwd') || ''; } catch { /* storage off */ }
  folder.value = lastFolder || (state.settings && state.settings.cwd) || '';
  folderRow.append(fl, folder);
  const note = document.createElement('div'); note.className = 'cd-note';
  note.textContent = 'A git folder\'s repository goes to the cloud with it; any other folder starts the session empty. '
    + `It runs on claude.ai under ${a.email || a.label}, not on this machine, and keeps going when the deck closes.`;
  const row = document.createElement('div'); row.className = 'btn-row';
  const go = document.createElement('button'); go.className = 'primary'; go.textContent = 'Start';
  const cancel = document.createElement('button'); cancel.textContent = 'Cancel';
  const err = document.createElement('span'); err.className = 'cd-err';
  const hintKeys = document.createElement('span'); hintKeys.className = 'cd-keys'; hintKeys.textContent = 'Ctrl+Enter starts · Esc cancels';
  row.append(go, cancel, hintKeys);
  box.append(title, ta, folderRow, note, row, err);
  back.appendChild(box);
  document.body.appendChild(back);
  cloudDlg = back;
  let starting = false;
  const start = async () => {
    if (starting) return;
    const task = ta.value.trim();
    if (!task) { err.textContent = 'Say what it should do.'; ta.focus(); return; }
    starting = true; go.disabled = true; err.textContent = '';
    try {
      const id = await deck.newTile({ kind: 'cloud', account: a.id, task, cwd: folder.value.trim() });
      try { localStorage.setItem('deck.cloud.cwd', folder.value.trim()); } catch { /* storage off */ }
      closeCloudDialog();
      createTerm(id); placeNew(id);
    } catch (e) {
      // ipcRenderer wraps a main-process throw: "Error invoking remote method 'tile:new': Error: <ours>"
      err.textContent = String((e && e.message) || e).replace(/^Error invoking remote method '[^']+': (Error: )?/, '');
      starting = false; go.disabled = false;
    }
  };
  go.addEventListener('click', ev => { ev.stopPropagation(); start(); });
  cancel.addEventListener('click', ev => { ev.stopPropagation(); closeCloudDialog(); });
  back.addEventListener('mousedown', ev => { if (ev.target === back) closeCloudDialog(); });
  box.addEventListener('keydown', ev => {
    ev.stopPropagation();                                     // the deck's own keys (digits, F-keys) stay out of the form
    if (ev.key === 'Escape') { ev.preventDefault(); closeCloudDialog(); }
    else if (ev.key === 'Enter' && (ev.ctrlKey || ev.metaKey)) { ev.preventDefault(); start(); }
  });
  box.addEventListener('click', ev => ev.stopPropagation());
  setTimeout(() => ta.focus(), 0);   // not requestAnimationFrame: a window in the background gets no frames
}
