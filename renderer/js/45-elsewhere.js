'use strict';
/* global deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- "elsewhere": adopt a session running in another window
const pullBtn = document.getElementById('pull-btn'), pullMenu = document.getElementById('pull-menu');

// the menu may grow up to the top of the sidebar: give it that room before showing it
function sizePullMenu() { const top = document.getElementById('sb-bottom').getBoundingClientRect().top; pullMenu.style.maxHeight = `${Math.max(120, top - 8)}px`; }

// Confirm in place: the clicked row stays put as the title, the question and buttons sit under it, and a
// single 1px white bound frames both. Cancel puts the plain row back.
function confirmRow(row, question, yesLabel, onYes) {
  const wrap = document.createElement('div'); wrap.className = 'pull-confirm-wrap';
  const title = row.cloneNode(true); title.classList.add('as-title');
  const c = document.createElement('div'); c.className = 'pull-confirm';
  const q = document.createElement('div'); q.textContent = question;
  const br = document.createElement('div'); br.className = 'btn-row';
  const yes = document.createElement('button'); yes.className = 'danger'; yes.textContent = yesLabel;
  const no = document.createElement('button'); no.textContent = 'Cancel';
  yes.addEventListener('click', ev => { ev.stopPropagation(); onYes(); });
  no.addEventListener('click', ev => { ev.stopPropagation(); wrap.replaceWith(row); });
  wrap.addEventListener('click', ev => ev.stopPropagation());
  br.append(yes, no); c.append(q, br); wrap.append(title, c);
  row.replaceWith(wrap);
}
function reportToActive(e) { const r = terms.get(state.active); if (r) r.term.write(`\r\n\x1b[31m[claude-deck] ${e && e.message || e}\x1b[0m\r\n`); }

async function openPullMenu() {
  sizePullMenu();
  const list = await deck.externalSessions();
  pullMenu.innerHTML = '';
  if (!list.length) { const d = document.createElement('div'); d.className = 'pull-empty'; d.textContent = 'No Claude sessions running outside this window.'; pullMenu.appendChild(d); }
  for (const s of list) {
    const row = document.createElement('div'); row.className = 'pull-row';
    const abbr = document.createElement('span'); abbr.className = 'pr-abbr' + (s.status === 'busy' ? ' busy' : ''); abbr.textContent = s.label;
    if (s.cloud) { const c = document.createElement('span'); c.className = 'cloud-mark'; c.title = 'cloud session'; c.innerHTML = CLOUD_SVG; abbr.appendChild(c); }
    const title = document.createElement('span'); title.className = 'pr-title'; title.textContent = s.title || s.name || s.sessionId.slice(0, 8);
    const st = document.createElement('span'); st.className = 'pr-status'; st.textContent = s.status === 'busy' ? 'busy' : (s.status === 'waiting' ? 'waiting' : fmtAge(Date.now() - (s.statusUpdatedAt || Date.now())));
    row.append(abbr, title, st);
    row.addEventListener('click', ev => {
      ev.stopPropagation();
      // a cloud session is only opened here (its page in a tile); nothing is closed, nothing is lost
      const question = s.cloud ? 'Open this cloud session here? It keeps running in the cloud.'
        : (s.status === 'busy' ? 'It is busy: the turn in progress will be lost. ' : '') + 'Close the copy in the other window and open it here?';
      confirmRow(row, question, s.cloud ? 'Open it here' : 'Bring it here', async () => {
        pullMenu.hidden = true;
        try { const id = await deck.adoptSession(s); createTerm(id); placeNew(id); } catch (e) { reportToActive(e); }
      });
    });
    pullMenu.appendChild(row);
  }
  pullMenu.hidden = false;
}
pullBtn.addEventListener('click', ev => { ev.stopPropagation(); if (pullMenu.hidden || document.getElementById('past-rows')) openPullMenu(); else pullMenu.hidden = true; });
document.addEventListener('click', ev => { if (!ev.target.closest('#pull-menu, #pull-btn, #past-btn')) pullMenu.hidden = true; });
// dim the button when nothing is out there (checked every 10 s)
async function refreshPullBadge() { try { const n = (await deck.externalSessions()).length; pullBtn.classList.toggle('none', n === 0); pullBtn.textContent = n ? `Elsewhere [${n}]` : 'Elsewhere'; } catch { /* ignore */ } }
refreshPullBadge(); setInterval(refreshPullBadge, 10000);
// the past badge reads the disk (every transcript of every account), so it refreshes on a slower clock
async function refreshPastBadge() { try { const btn = document.getElementById('past-btn'); if (!btn) return; const n = (await deck.pastSessions()).length; btn.classList.toggle('none', n === 0); btn.textContent = n ? `Past [${n}]` : 'Past'; } catch { /* ignore */ } }
refreshPastBadge(); setInterval(refreshPastBadge, 60000);
