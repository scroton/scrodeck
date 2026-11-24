'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- move every session (button left of the gear)
const moveMenu = $('#moveall-menu');
function openMoveMenu() {
  const counts = {}; for (const t of state.tiles) if (t.kind === 'claude') counts[t.account] = (counts[t.account] || 0) + 1;
  const from = $('#move-from'), to = $('#move-to'); from.innerHTML = ''; to.innerHTML = '';
  for (const a of state.accounts) {
    const o1 = document.createElement('option'); o1.value = a.id; o1.textContent = `${a.label} (${counts[a.id] || 0})`; from.appendChild(o1);
    const o2 = document.createElement('option'); o2.value = a.id; o2.textContent = a.label; to.appendChild(o2);
  }
  // default: the account with the most open tiles → the first other account
  const busiest = Object.entries(counts).sort((x, y) => y[1] - x[1])[0];
  if (busiest) from.value = busiest[0];
  const other = state.accounts.find(a => a.id !== from.value); if (other) to.value = other.id;
  $('#move-continue').checked = localStorage.getItem('deck.movecontinue') === '1';   // ships off: it types into sessions
  $('#move-advanced').checked = localStorage.getItem('deck.moveadvanced') === '1';
  $('#move-all-confirm').textContent = '';
  refreshMoveBars();
  renderMovePick();
  syncMoveMode();
  moveMenu.hidden = false;
  requestAnimationFrame(fitMenuTitle);
}
// advanced: list the source account's sessions with checkboxes (title, recent message under it, ticked by default)
function renderMovePick() {
  const box = $('#move-pick'), on = $('#move-advanced').checked, from = $('#move-from').value;
  box.hidden = !on;
  if (!on) return;
  const keep = new Map([...box.querySelectorAll('input[type=checkbox]')].map(i => [i.dataset.id, i.checked]));
  box.innerHTML = '';
  const list = state.tiles.filter(t => t.kind === 'claude' && t.account === from);
  if (!list.length) { const d = document.createElement('div'); d.className = 'hint'; d.textContent = 'No sessions in this account.'; box.appendChild(d); return; }
  for (const t of list) {
    const row = document.createElement('label'); row.className = 'pick-row';
    const cb = document.createElement('input'); cb.type = 'checkbox'; cb.dataset.id = t.id; cb.checked = keep.has(t.id) ? keep.get(t.id) : true;
    row.classList.toggle('sel', cb.checked);
    cb.addEventListener('change', () => row.classList.toggle('sel', cb.checked));
    const body = document.createElement('div'); body.className = 'pk-body';
    const title = document.createElement('div'); title.className = 'pk-title'; title.textContent = t.title || 'New session';
    body.appendChild(title);
    if (t.lastText) { const m = document.createElement('div'); m.className = 'pk-msg'; m.textContent = t.lastText.replace(/\r/g, '').split('\n').map(l => l.trimEnd()).filter(l => l.trim()).join('\n'); body.appendChild(m); }
    if (t.lastText && /spend limit|usage limit|rate limit|limit resets|\/usage-credits|hit your/i.test(t.lastText)) { const u = document.createElement('div'); u.className = 'pk-usage'; u.textContent = 'usage notice'; body.appendChild(u); }
    row.append(cb, body); box.appendChild(row);
  }
}
$('#move-advanced').addEventListener('change', ev => { localStorage.setItem('deck.moveadvanced', ev.target.checked ? '1' : '0'); renderMovePick(); syncMoveMode(); });
// "all" / "these" buttons drive the hidden advanced checkbox
function syncMoveMode() { const these = $('#move-advanced').checked; $('#move-mode-all').classList.toggle('on', !these); $('#move-mode-these').classList.toggle('on', these); }
$('#move-mode-all').addEventListener('click', ev => { ev.stopPropagation(); const a = $('#move-advanced'); a.checked = false; a.dispatchEvent(new Event('change')); });
$('#move-mode-these').addEventListener('click', ev => { ev.stopPropagation(); const a = $('#move-advanced'); a.checked = true; a.dispatchEvent(new Event('change')); });
function fitMenuTitle() {
  const el = $('#moveall-menu .menu-title'); let size = 12; el.style.fontSize = `${size}px`;
  let guard = 0; while (el.scrollWidth > el.clientWidth && size > 8 && guard++ < 20) { size -= 0.5; el.style.fontSize = `${size}px`; }
}
$('#move-from').addEventListener('change', renderMovePick);
function pickedIds() { return $('#move-advanced').checked ? [...document.querySelectorAll('#move-pick input[type=checkbox]:checked')].map(i => i.dataset.id) : null; }
function refreshMoveBars() {
  for (const [sel, box] of [['#move-from', '#move-from-bars'], ['#move-to', '#move-to-bars']]) {
    const b = $(box); const id = $(sel).value;
    if (!b.firstElementChild) b.innerHTML = BARS_HTML;
    const hasFable = !!(state.usage[id] && (state.usage[id].scoped || []).some(x => /fable/i.test(x.model)));
    applyBars(b, id, { fable: hasFable });
  }
}
$('#move-from').addEventListener('change', refreshMoveBars);
$('#move-to').addEventListener('change', refreshMoveBars);
$('#moveall-btn').addEventListener('click', ev => { ev.stopPropagation(); if (moveMenu.hidden) openMoveMenu(); else moveMenu.hidden = true; });
document.addEventListener('click', ev => { if (!ev.target.closest('#moveall-menu, #moveall-btn')) moveMenu.hidden = true; });
$('#move-continue').addEventListener('change', ev => localStorage.setItem('deck.movecontinue', ev.target.checked ? '1' : '0'));
$('#move-all-btn').addEventListener('click', async ev => {
  ev.stopPropagation();
  const from = $('#move-from').value, to = $('#move-to').value, box = $('#move-all-confirm');
  const cont = $('#move-continue').checked;
  const only = pickedIds();                       // null = every session of the account
  const n = only ? only.length : state.tiles.filter(t => t.kind === 'claude' && t.account === from).length;
  if (from === to) { box.textContent = 'Pick two different accounts.'; return; }
  if (!n) { box.textContent = only ? 'No sessions ticked.' : `No ${acctLabel(from)} sessions are open here.`; return; }
  box.innerHTML = '';
  const q = document.createElement('span'); q.textContent = `Move ${n} ${acctLabel(from)} session${n === 1 ? '' : 's'} to ${acctLabel(to)}? Busy ones lose the turn in progress. `;
  const yes = document.createElement('button'); yes.textContent = 'Move them'; yes.className = 'danger';
  const no = document.createElement('button'); no.textContent = 'Cancel';
  yes.addEventListener('click', async ev2 => {
    ev2.stopPropagation(); box.textContent = 'Moving…';
    try {
      const r = await deck.moveAll(from, to, cont, only);
      box.textContent = `Moved ${r.moved}.` + (cont && r.usageSessions ? ` "continue" queued for ${r.usageSessions}.` : '') + (r.errors.length ? ` Failed: ${r.errors.join('; ')}` : '');
    } catch (e) { box.textContent = `Failed: ${e.message || e}`; }
  });
  no.addEventListener('click', ev2 => { ev2.stopPropagation(); box.textContent = ''; });
  box.append(q, yes, no);
});

