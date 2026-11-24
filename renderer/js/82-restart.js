'use strict';
/* global deck, state, activate */
// Restart when quiet (2026-09-29). The restart button in the tool row arms it (and cancels it); the strip
// above the row says what it is waiting on, or — when the deck's own main-process files changed on disk —
// offers the restart that picks them up. main.js decides WHEN (lib/quietrestart.js); this file only shows the
// state it sends in `meta.restart` and passes the clicks back. The palette carries the same three commands.
//
// meta.restart: { armed, why ('manual' | 'update'), quietSince, settleLeftMs, blockers: [{ id, title, reason }],
//                 shells, update: { files, since } | null,
//                 code: { launched, disk, origin, behind, clean, branch, changed, lines, hold, canUpdate,
//                         pull: { busy, ok, text } | null } | null,       the folder against origin/main (fix 9)
//                 uiHeld }                                                a renderer change waits for the restart
// "Update" (code.canUpdate: clean, on main, behind, no package.json change) pulls origin/main in main.js and arms
// the restart; never automatic. Older main processes send no `code`, so the button never shows against them.
const restartBtn = $('#restart-btn');
const restartStrip = $('#restart-strip');
const rsTitle = restartStrip.querySelector('.rs-title');
const rsList = restartStrip.querySelector('.rs-list');
const rsNote = restartStrip.querySelector('.rs-note');
const rsCode = restartStrip.querySelector('.rs-code');
const rsBtn = act => restartStrip.querySelector(`[data-act="${act}"]`);
let restartInfo = null;
let nowConfirmUntil = 0;     // "Now" past a blocker takes a second click within a few seconds
let listSig = '', codeSig = '';
// a restart picks something up: the main-process files changed, or the folder's HEAD moved since launch
const restartWanted = r => !!(r && (r.update || (r.code && r.code.changed)));
// The renderer reloads itself when its files change (watchRendererFiles), so this file can arrive under a main
// process that predates it. preload.js reloads WITH the view, so the deck.restart* functions exist even then —
// only their IPC handlers are missing (2026-09-29: the live deck showed a button that did nothing). The one
// honest sign is the main process itself sending `meta.restart`; until it does there is nothing to show.
let restartSupported = false;
restartBtn.hidden = true;

function restartArm(why) { return deck.restartWhenQuiet(true, why || 'manual'); }
function restartCancel() { nowConfirmUntil = 0; return deck.restartWhenQuiet(false); }

function paintRestart(r) {
  restartSupported = !!(r && typeof r === 'object');
  restartBtn.hidden = !restartSupported;
  restartInfo = r || null;
  const armed = !!(r && r.armed);
  const offer = !armed && !!(r && (r.update || r.code));
  restartBtn.classList.toggle('armed', armed);
  restartBtn.classList.toggle('offer', offer);
  restartBtn.title = armed ? 'Restart armed: it goes when every session is quiet. Click to cancel.'
    : offer ? (restartWanted(r) ? 'The deck\'s code changed on disk. Click to restart when every session is quiet.' : 'The deck is behind origin/main (see the strip).')
    : 'Restart the deck when every session is quiet';
  renderRestartStrip();
}

function renderRestartStrip() {
  const r = restartInfo;
  const armed = !!(r && r.armed);
  const code = (r && r.code) || null;
  if (!r || (!armed && !r.update && !code)) { restartStrip.hidden = true; listSig = ''; return; }
  restartStrip.hidden = false;
  const rows = armed ? (r.blockers || []) : [];
  const files = r.update ? r.update.files.join(', ') : '';
  let title;
  if (!armed) title = r.update ? `Deck update waiting: ${files}` : code && code.behind > 0 ? 'Deck behind origin/main' : 'Deck code changed on disk';
  else if (rows.length) title = `${r.why === 'update' ? 'Update: restart' : 'Restart'} when quiet, waiting on ${rows.length}`;
  else title = `Quiet: restarting in ${Math.max(1, Math.ceil((r.settleLeftMs || 0) / 1000))} s`;
  if (rsTitle.textContent !== title) rsTitle.textContent = title;
  restartStrip.classList.toggle('offer', !armed);
  restartStrip.classList.toggle('settling', armed && !rows.length);

  // the list is rebuilt only when it changes, so a row under the pointer is not replaced mid-click
  const sig = JSON.stringify(rows.map(b => [b.id, b.title, b.reason]));
  if (sig !== listSig) {
    listSig = sig;
    rsList.innerHTML = '';
    for (const b of rows) {
      const row = document.createElement('div'); row.className = 'rs-row'; row.title = 'Show this session';
      const t = document.createElement('span'); t.className = 'rs-t'; t.textContent = b.title;
      const why = document.createElement('span'); why.className = 'rs-r'; why.textContent = b.reason;
      row.append(t, why);
      row.addEventListener('click', () => { if (state.tiles.some(x => x.id === b.id)) activate(b.id); });
      rsList.appendChild(row);
    }
  }
  rsList.hidden = !rows.length;

  // the folder against origin/main, why Update is held, the pull's result (or git's error), a held reload
  const lines = [];
  if (code) {
    for (const l of code.lines || []) lines.push({ t: l.charAt(0).toUpperCase() + l.slice(1) });
    if (code.hold) lines.push({ t: `No Update here: ${code.hold}` });
    if (code.pull && code.pull.text) lines.push({ t: code.pull.text, bad: !code.pull.busy && !code.pull.ok });
  }
  if (r.uiHeld) lines.push({ t: 'This window\'s own changes wait for the restart' });
  const cs = JSON.stringify(lines);
  if (cs !== codeSig) {
    codeSig = cs;
    rsCode.innerHTML = '';
    for (const l of lines) { const d = document.createElement('div'); d.textContent = l.t; if (l.bad) d.className = 'bad'; rsCode.appendChild(d); }
  }
  rsCode.hidden = !lines.length;

  const note = r.shells ? `${r.shells} terminal${r.shells === 1 ? '' : 's'} restart${r.shells === 1 ? 's' : ''} empty` : '';
  if (rsNote.textContent !== note) rsNote.textContent = note;
  rsBtn('update').hidden = !(code && code.canUpdate && typeof deck.updateCode === 'function');
  rsBtn('arm').hidden = armed || !restartWanted(r);
  rsBtn('later').hidden = armed;
  rsBtn('cancel').hidden = !armed;
  const now = rsBtn('now');
  now.hidden = !armed;
  const confirming = Date.now() < nowConfirmUntil && rows.length;
  now.textContent = confirming ? `Stop ${rows.length} and restart` : 'Now';
  now.classList.toggle('danger', !!confirming);
}

restartStrip.querySelector('.rs-actions').addEventListener('click', ev => {
  const b = ev.target.closest('button[data-act]'); if (!b) return;
  const act = b.dataset.act;
  if (act === 'update') { if (typeof deck.updateCode === 'function') deck.updateCode(); }
  else if (act === 'arm') restartArm(restartWanted(restartInfo) ? 'update' : 'manual');
  else if (act === 'later') deck.updateLater();
  else if (act === 'cancel') restartCancel();
  else if (act === 'now') {
    const n = restartInfo && restartInfo.armed ? (restartInfo.blockers || []).length : 0;
    if (n && Date.now() >= nowConfirmUntil) { nowConfirmUntil = Date.now() + 4000; renderRestartStrip(); setTimeout(renderRestartStrip, 4100); return; }
    nowConfirmUntil = 0;
    deck.restartNow();
  }
});

restartBtn.addEventListener('click', () => {
  if (restartInfo && restartInfo.armed) restartCancel();
  else restartArm(restartWanted(restartInfo) ? 'update' : 'manual');
});

// palette entries (88-palette.js adds these to its 'deck' group)
function restartPaletteItems() {
  if (!restartSupported) return [];
  const r = restartInfo || {};
  const n = (r.blockers || []).length;
  const items = [];
  if (r.armed) items.push({ group: 'deck', label: 'Cancel quiet restart', hint: n ? `waiting on ${n}` : 'about to go', run: () => restartCancel() });
  else items.push({ group: 'deck', label: 'Restart deck when quiet', hint: restartWanted(r) ? 'picks up the update' : '', run: () => restartArm(restartWanted(r) ? 'update' : 'manual') });
  if (r.code && r.code.canUpdate && typeof deck.updateCode === 'function') items.push({ group: 'deck', label: 'Update deck from origin/main', hint: `${r.code.behind} behind; then restart when quiet`, run: () => deck.updateCode() });
  // Now past a blocker is confirmed on the strip, never straight from a keystroke
  items.push({ group: 'deck', label: n ? 'Restart deck now…' : 'Restart deck now', hint: n ? `stops ${n}; confirm on the strip` : 'nothing running',
    run: () => { if (!n) { deck.restartNow(); return; } if (!r.armed) restartArm('manual'); nowConfirmUntil = Date.now() + 6000; renderRestartStrip(); setTimeout(renderRestartStrip, 6100); } });
  return items;
}
