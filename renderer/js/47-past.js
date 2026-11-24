'use strict';
/* global deck */
// "past": every Claude session on disk across the accounts that is not open here and not running elsewhere,
// newest first, with a filter box. Picking one confirms in place, then opens it in a new tile under its
// account. Shares the upward-opening menu container (and confirmRow) with "elsewhere".
const pastBtn = document.getElementById('past-btn');
let pastList = [];

function pastRow(s) {
  const row = document.createElement('div'); row.className = 'pull-row';
  const abbr = document.createElement('span'); abbr.className = 'pr-abbr'; abbr.textContent = s.label;
  const title = document.createElement('span'); title.className = 'pr-title'; title.textContent = s.title || s.sessionId.slice(0, 8);
  const age = document.createElement('span'); age.className = 'pr-status'; age.textContent = fmtAge(Date.now() - s.mtime);
  row.append(abbr, title, age);
  row.addEventListener('click', ev => {
    ev.stopPropagation();
    confirmRow(row, `Open this ${s.label} session here?`, 'Open here', async () => {
      pullMenu.hidden = true;
      try { const id = await deck.newTile({ kind: 'claude', account: s.account, sessionId: s.sessionId, cwd: s.cwd || undefined }); createTerm(id); placeNew(id); }
      catch (e) { reportToActive(e); }
    });
  });
  return row;
}
function renderPastList(filter) {
  const words = (filter || '').toLowerCase().split(/\s+/).filter(Boolean);
  const box = document.getElementById('past-rows'); box.innerHTML = '';
  const shown = pastList.filter(s => words.every(w => `${s.label} ${s.title || ''} ${s.account}`.toLowerCase().includes(w)));
  if (!shown.length) { const d = document.createElement('div'); d.className = 'pull-empty'; d.textContent = pastList.length ? 'No match.' : 'No past sessions found.'; box.appendChild(d); return; }
  for (const s of shown.slice(0, 60)) box.appendChild(pastRow(s));
}
async function openPastMenu() {
  sizePullMenu();
  pullMenu.innerHTML = '';
  const head = document.createElement('div'); head.className = 'past-head';
  const input = document.createElement('input'); input.type = 'text'; input.placeholder = 'filter past sessions'; input.spellcheck = false;
  const note = document.createElement('span'); note.className = 'hint'; note.textContent = 'loading…';
  head.append(input, note); pullMenu.appendChild(head);
  const rows = document.createElement('div'); rows.id = 'past-rows'; pullMenu.appendChild(rows);
  pullMenu.hidden = false;
  input.focus();
  pastList = await deck.pastSessions();
  note.textContent = `${pastList.length} on disk`;
  renderPastList('');
  input.addEventListener('input', () => renderPastList(input.value));
  input.addEventListener('keydown', ev => { if (ev.key === 'Escape') { ev.preventDefault(); pullMenu.hidden = true; } });
}
pastBtn.addEventListener('click', ev => { ev.stopPropagation(); if (pullMenu.hidden || !document.getElementById('past-rows')) openPastMenu(); else pullMenu.hidden = true; });
