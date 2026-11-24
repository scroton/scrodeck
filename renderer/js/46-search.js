'use strict';
/* global deck */
// Local search over the tiles: title, recent message, account label, effort. No request leaves the box.
// The magnifier in the bottom row opens a pane above it; typing hides tiles that do not match every word.
const searchPane = document.getElementById('search-pane');
const searchInput = document.getElementById('search-input');
state.search = '';

function tileMatches(t, words) {
  if (!words.length) return true;
  const hay = [t.title || '', t.lastText || '', acctLabel(t.account), t.effort || '', t.kind === 'shell' ? `shell terminal ${shellName()}` : 'claude'].join(' ').toLowerCase();
  return words.every(w => hay.includes(w));
}
function applySearchFilter() {
  const words = state.search.toLowerCase().split(/\s+/).filter(Boolean);
  let shown = 0;
  for (const t of state.tiles) {
    const el = tileEls.get(t.id); if (!el) continue;
    const ok = tileMatches(t, words);
    el.hidden = !ok; if (ok) shown++;
  }
  const count = document.getElementById('search-count');
  if (count) count.textContent = words.length ? `${shown} / ${state.tiles.length}` : '';
}
function openSearch() { searchPane.hidden = false; document.getElementById('search-btn').classList.add('on'); searchInput.focus(); searchInput.select(); }
function closeSearch() { searchPane.hidden = true; document.getElementById('search-btn').classList.remove('on'); state.search = ''; searchInput.value = ''; applySearchFilter(); }
document.getElementById('search-btn').addEventListener('click', () => { if (searchPane.hidden) openSearch(); else closeSearch(); });
searchInput.addEventListener('input', () => { state.search = searchInput.value; applySearchFilter(); });
searchInput.addEventListener('keydown', ev => {
  if (ev.key === 'Escape') { ev.preventDefault(); closeSearch(); const r = state.active && terms.get(state.active); if (r) r.term.focus(); }
  if (ev.key === 'Enter') {   // jump to the first match and hand focus to its terminal
    const first = state.tiles.find(t => { const el = tileEls.get(t.id); return el && !el.hidden; });
    if (first) { activate(first.id); ev.preventDefault(); }
  }
});
document.getElementById('search-clear').addEventListener('click', () => { searchInput.value = ''; state.search = ''; applySearchFilter(); searchInput.focus(); });
