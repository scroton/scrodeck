'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- the "new" tile at the bottom
function renderNewTile() {
  const sig = JSON.stringify(state.accounts.map(a => a.id + a.label));
  if (sig === acctSig) return;
  acctSig = sig;
  newAccts.innerHTML = '';
  for (const a of state.accounts) {
    const b = document.createElement('button');
    b.textContent = a.label; b.title = `New Claude session in ${a.email || a.id}`;
    b.addEventListener('click', async ev => { ev.stopPropagation(); const id = await deck.newTile({ kind: 'claude', account: a.id }); createTerm(id); placeNew(id); });
    // right-click: the same new session with Claude in Chrome on (`--chrome`; see browserMenu in 30-tiles.js),
    // or in a deck window of its own, or a cloud session of this account (48-cloud.js: the task first, then a
    // tile that is its claude.ai page)
    b.addEventListener('contextmenu', ev => {
      ev.preventDefault(); ev.stopPropagation();
      const items = [];
      if (state.browserOption) items.push({ label: `New ${a.label} session with the browser`, run: async () => { const id = await deck.newTile({ kind: 'claude', account: a.id, browser: true }); createTerm(id); placeNew(id); } });
      items.push({ label: `New ${a.label} session in a new window`, run: () => newSessionInNewWindow(a) });
      if (state.cloudOption) items.push({ label: `New ${a.label} cloud session…`, run: () => openCloudDialog(a) });
      if (items.length) showCtxMenu(ev, items);
    });
    newAccts.appendChild(b);
  }
}
// 2026-10-21: a new session straight into a new deck window, filling it. window:open (main.js) is the
// drag-out path: it sizes the window at 80% of this one and takes x/y as the grab point, 40/20 inside its
// top-left corner, so this puts the window 10% in from this one's corner, on the same screen. Where no window
// can open (the browser desk answers null) the session is placed here instead.
async function newSessionInNewWindow(a) {
  const id = await deck.newTile({ kind: 'claude', account: a.id });
  createTerm(id);
  const opened = await openInNewWindow(id, screenX + outerWidth * 0.1 + 40, screenY + outerHeight * 0.1 + 20);
  if (opened == null) placeNew(id);
}
newTile.addEventListener('click', async ev => {
  if (ev.target.closest('button')) return;
  const id = await deck.newTile({ kind: 'shell' }); createTerm(id); placeNew(id);
});
