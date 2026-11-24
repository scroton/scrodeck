'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- settings panel
// One editable account row: keywords | config folder | label | email or "log in" | remove
function acctRow(r, aliases) {
  const row = document.createElement('div'); row.className = 'acct-row';
  const kw = document.createElement('input'); kw.type = 'text'; kw.className = 'a-kw'; kw.placeholder = 'keyword(s)'; kw.spellcheck = false;
  kw.value = (r.keywords || []).join(', ');
  const dir = document.createElement('input'); dir.type = 'text'; dir.className = 'a-dir'; dir.placeholder = state.platform.isMac ? '~/.claude-name' : '~\\.claude-name'; dir.spellcheck = false;
  dir.value = r.dir || '';
  const label = document.createElement('input'); label.type = 'text'; label.className = 'a-label'; label.maxLength = 8; label.placeholder = 'label';
  label.value = r.email ? (aliases[r.email] || '') : (aliases[r.id] || '');
  label.dataset.key = r.email || r.id || '';
  const who = document.createElement('div'); who.className = 'a-who';
  if (r.email) who.textContent = r.email;
  else if (r.id) {
    const b = document.createElement('button'); b.className = 'small-btn'; b.textContent = 'log in';
    b.title = 'Opens a Claude session under this account so you can sign in';
    b.addEventListener('click', async () => { closeSettings(); const id = await deck.newTile({ kind: 'claude', account: r.id }); createTerm(id); placeNew(id); });
    who.appendChild(b);
  } else who.textContent = 'save, then log in';
  const rm = document.createElement('button'); rm.className = 'small-btn danger'; rm.textContent = '×'; rm.title = 'Remove this account from the list (its folder is left alone)';
  rm.addEventListener('click', () => row.remove());
  row.append(kw, dir, label, who, rm);
  return row;
}

const SETTINGS_MIN_WIDTH = 600;
let widthBeforeSettings = null;
function widenForSettings() {
  if (widthBeforeSettings !== null) return;
  widthBeforeSettings = sidebar.getBoundingClientRect().width | 0;
  // the tool row keeps the width it had, so its buttons stay where they were; the sidebar's own
  // colour fills the rest of the row
  // pinned at the row's OWN width, border box, so its padding is not added on top and nothing shifts
  const tools = document.getElementById('sb-tools');
  if (tools) { const w = tools.getBoundingClientRect().width; tools.style.boxSizing = 'border-box'; tools.style.flex = 'none'; tools.style.width = w + 'px'; }
  const want = Math.max(SETTINGS_MIN_WIDTH, widthBeforeSettings, 220);
  document.documentElement.style.setProperty('--sb-width', `${Math.min(want, window.innerWidth * 0.7)}px`);
  requestAnimationFrame(fitActive);
}
function restoreAfterSettings() {
  if (widthBeforeSettings === null) return;
  document.documentElement.style.setProperty('--sb-width', `${widthBeforeSettings}px`);
  const tools = document.getElementById('sb-tools'); if (tools) { tools.style.width = ''; tools.style.boxSizing = ''; tools.style.flex = ''; }
  widthBeforeSettings = null;
  requestAnimationFrame(fitActive);
}
function closeSettings() { settingsPanel.hidden = true; restoreAfterSettings(); }

// "Use ask pages" off: the three settings that shape a page do nothing, so they grey out
function paintAskDeps() {
  const on = $('#set-ask-pages').checked;
  document.querySelectorAll('#settings .ask-dep').forEach(el => { el.classList.toggle('off', !on); el.querySelectorAll('input, select').forEach(i => { i.disabled = !on; }); });
}
$('#set-ask-pages').addEventListener('change', paintAskDeps);
async function openSettings() {
  placeSettingsPanel();
  widenForSettings();
  const s = await deck.getSettings();
  const rows = $('#alias-rows'); rows.innerHTML = '';
  for (const r of s.accountRows) rows.appendChild(acctRow(r, s.aliases));
  $('#acct-add').onclick = () => rows.appendChild(acctRow({ keywords: [], dir: '', email: null, id: null }, s.aliases));
  $('#set-font').value = s.fontSize;
  $('#set-zoomfont').value = s.zoomFontSize || 0;
  setupFontControls(s.fontFamily);
  // tiles + graphs
  $('#set-effort').value = s.effortStyle || 'tag';
  $('#set-side').value = s.sidebarSide || 'left';
  $('#set-pinside').value = s.pinSide || 'edge';
  $('#set-driver').value = driverMode(s.driver);   // 80-sidebar.js: 'desk' from an older main reads as 'office'
  if ($('#set-away')) $('#set-away').value = s.awayMinutes ?? 30;   // lib/presence.js: the state file's 'away'
  $('#set-msg-normal').value = (s.msgLines || {}).normal ?? 3;
  $('#set-msg-wide').value = (s.msgLines || {}).wide ?? 10;
  const gr = s.grid || { rows: 2, cols: 2 };
  $('#set-grid-rows').value = gr.rows || 2;
  $('#set-grid-cols').value = gr.cols || 2;
  $('#set-one-window').checked = s.oneWindow !== false;
  // on a remote desk the box is the office deck's setting: it changes nothing here (25-layout.js oneWindow)
  if (typeof IS_REMOTE_DESK !== 'undefined' && IS_REMOTE_DESK) { const f = $('#set-one-window').closest('label'); if (f) f.title = "The office deck's own windows. It changes nothing on this remote desk, which shows any session whatever a window there holds."; }
  $('#set-step-esc').checked = s.stepEscape !== false;
  const ak = s.asks || {};
  $('#set-ask-pages').checked = ak.usePages !== false;
  $('#set-ask-threshold').value = Number.isInteger(ak.deckThreshold) ? ak.deckThreshold : '';
  $('#set-ask-background').checked = ak.background !== false;
  $('#set-ask-picker').value = ak.picker === 'pane' ? 'pane' : 'alongside';
  $('#set-ask-tile').checked = ak.showTile !== false;
  $('#set-ask-marks').checked = ak.showMarks !== false;
  $('#set-ask-close').checked = ak.closeOnDone !== false;
  paintAskDeps();
  const hb = s.hibernate || {};
  $('#set-hib-enabled').checked = hb.enabled !== false;
  $('#set-hib-idle').value = hb.idleMinutes ?? 30;
  $('#set-hib-pinned').checked = hb.keepPinned !== false;
  const nf = s.notify || {};
  $('#set-notify-waiting').checked = nf.waiting !== false;
  $('#set-notify-done').checked = nf.done !== false;
  $('#set-notify-asks').checked = nf.asks !== false;
  $('#set-notify-min').value = nf.minTurnSec ?? 20;
  $('#set-notify-sound').checked = nf.sound !== false;
  $('#set-lbl-mode').value = s.barLabelMode || 'reset';
  $('#set-lbl-time').checked = !(s.barLabels && s.barLabels.time === false);
  $('#set-lbl-day').checked = !(s.barLabels && s.barLabels.day === false);
  const barRows = $('#bar-rows'); barRows.innerHTML = '';
  for (const k of ['session', 'week', 'fable', 'context']) {
    const cfg = { ...DEFAULT_BARS[k], ...((s.bars || {})[k] || {}) };
    const show = document.createElement('input'); show.type = 'checkbox'; show.checked = cfg.show !== false; show.dataset.bar = k; show.className = 'b-show';
    const name = document.createElement('input'); name.type = 'text'; name.value = cfg.name; name.maxLength = 40; name.dataset.bar = k; name.className = 'b-name';
    const from = document.createElement('input'); from.type = 'color'; from.value = cfg.from; from.dataset.bar = k; from.className = 'b-from'; from.title = 'color at 0 %';
    const to = document.createElement('input'); to.type = 'color'; to.value = cfg.to; to.dataset.bar = k; to.className = 'b-to'; to.title = 'color at 100 %';
    barRows.append(show, name, from, to);
  }
  $('#set-cwd').value = s.cwd;
  $('#set-autotrust').checked = s.autoTrust !== false;
  $('#set-resume').value = s.resumeChoice || 'ask';
  $('#set-anim').checked = s.animations !== false;
  $('#set-auditor-autosend').checked = s.auditorAutoSend !== false;
  $('#set-resume-after-restart').checked = s.resumeAfterRestart !== false;   // lib/pause.js: off = only "Pause until restart" resumes by itself   // auditors (lib/auditors.js): off = Send on the child's tile
  const ph = s.phoneStatus || {};
  $('#set-phone-on').checked = !!ph.enabled;
  $('#set-phone-port').value = ph.port || 8788;
  $('#set-phone-bind').value = ph.bind || 'tailscale';
  $('#set-phone-pages').value = (s.pages || []).map(p => `${p.label} = ${p.file}`).join('\n');
  paintPhone(ph);
  $('#settings-status').textContent = '';
  settingsPanel.hidden = false;
  requestAnimationFrame(sizePhoneUrl);   // the link box measures itself only once it is visible
}

// "Pages on the phone": one `Label = path` a line (a line with no '=' is a bare path, labelled by its file name)
function pagesFromText(text) {
  return String(text || '').split(/\r?\n/).map(l => l.trim()).filter(Boolean).map(l => {
    const i = l.indexOf('=');
    return i > 0 ? { label: l.slice(0, i).trim(), file: l.slice(i + 1).trim() } : { label: '', file: l };
  });
}

// The phone view's line in Settings: the link to type once on the phone, and what the server is doing.
function paintPhone(ph) {
  const url = ph.url || (ph.address ? `http://${ph.address}:${ph.port}/?t=${ph.token}` : '');
  const ta = $('#set-phone-url');
  ta.value = url; ta.hidden = !url;
  sizePhoneUrl();
  const st = $('#phone-state');
  const n = `${ph.clients} phone${ph.clients === 1 ? '' : 's'} connected`;
  if (ph.error) st.textContent = `Not serving: ${ph.error}`;
  else if (!ph.enabled) st.textContent = ph.tailscale ? `Off. This machine is ${ph.tailscale} on the tailnet.` : 'Off. Tailscale is not running, so there is no tailnet address to serve on.';
  else if (ph.running && ph.bind === 'localhost' && ph.serve) st.textContent = `Behind Tailscale Serve: the link above is the HTTPS front door (${ph.serve}); the server itself listens on ${ph.address}:${ph.port}. ${n}. Open the link on the phone once; after that the app remembers it.`;
  else if (ph.running && ph.bind === 'localhost') st.textContent = `Serving on ${ph.address}:${ph.port} · ${n}. On no network of its own, and Tailscale Serve is not forwarding to this port, so the link above works only on this PC: run \`tailscale serve --bg https / http://localhost:${ph.port}\` once, which gives the page HTTPS and lets Chrome install it as an app.`;
  else if (ph.running) st.textContent = `Serving on ${ph.address}:${ph.port} · ${ph.clients} phone${ph.clients === 1 ? '' : 's'} connected. Open the link on the phone once; after that it is installable from Chrome's menu ("Install app"), which needs HTTPS — see mobile.md.`;
  else st.textContent = 'Starting…';
}
// the whole link, wrapped to fit; the box grows to its text
function sizePhoneUrl() {
  const ta = $('#set-phone-url'); if (!ta || ta.hidden) return;
  ta.rows = 1; ta.style.height = 'auto';
  if (ta.scrollHeight) ta.style.height = ta.scrollHeight + 2 + 'px';
}
function copyPhoneUrl() {
  const ta = $('#set-phone-url'); const v = ta.value;
  if (!v) return;
  deck.writeClipboard(v);
  try { ta.focus(); ta.select(); } catch { /* not focusable right now */ }
  const st = $('#settings-status'); st.textContent = 'Link copied.'; st.title = st.textContent;
}
$('#phone-copy').addEventListener('click', copyPhoneUrl);
$('#set-phone-url').addEventListener('click', copyPhoneUrl);   // clicking the link copies it too (2026-09-09)
// A new token locks out every phone that has the old one — that is the point of it.
$('#phone-newtoken').addEventListener('click', async () => {
  if (!confirm('Issue a new token? Any phone using the old link stops working until it opens the new one.')) return;
  const saved = await deck.saveSettings({ phone: { enabled: $('#set-phone-on').checked, port: $('#set-phone-port').value, bind: $('#set-phone-bind').value, newToken: true } });
  paintPhone(saved.phoneStatus || {});
});
$('#settings-btn').addEventListener('click', () => { if (settingsPanel.hidden) openSettings(); else closeSettings(); });
deck.onSettingsToggle(() => { if (settingsPanel.hidden) openSettings(); else closeSettings(); });   // Cmd+, on a Mac (main process)
// The panel stands above the tool row, so the settings button stays in view and closes it again;
// Esc closes it too, like the menus.
function placeSettingsPanel() {
  const tools = document.getElementById('sb-tools');
  const lift = tools ? tools.offsetHeight + 8 : 0;
  settingsPanel.style.bottom = lift + 'px';   // a breath of sidebar above the buttons, as the tiles have
  // The panel's height cap must be the room ABOVE that lift, not the whole sidebar: capped at 100 % it
  // could stand 34 px taller than the space and push its head — Save and Cancel — off the top of the
  // window once the settings grew past a screen (2026-09-10: "where the fuck is the option to save").
  settingsPanel.style.maxHeight = `calc(100% - ${lift}px)`;
  settingsPanel.style.width = ''; settingsPanel.style.left = ''; settingsPanel.style.right = '';   // within the sidebar
}
placeSettingsPanel();
window.addEventListener('resize', placeSettingsPanel);
document.addEventListener('keydown', ev => {
  if (ev.key !== 'Escape') return;
  if (!settingsPanel.hidden) { ev.preventDefault(); closeSettings(); return; }
  const mv = document.getElementById('moveall-menu');
  if (mv && !mv.hidden) { ev.preventDefault(); mv.hidden = true; }
});
$('#settings-close').addEventListener('click', () => closeSettings());

// ---- save
$('#settings-save').addEventListener('click', async () => {
  const status = $('#settings-status');
  // 1. accounts → alias file (keywords + folders), then re-read what main resolved
  const rowsIn = [...document.querySelectorAll('#alias-rows .acct-row')].map(row => ({
    keywords: row.querySelector('.a-kw').value, dir: row.querySelector('.a-dir').value, label: row.querySelector('.a-label').value,
    key: row.querySelector('.a-label').dataset.key,
  }));
  let saved;
  try {
    const resolved = await deck.saveAccounts(rowsIn.map(r => ({ keywords: r.keywords, dir: r.dir })));
    // 2. labels: key by email when known, else by the account's keyword
    const aliases = {};
    for (const r of rowsIn) {
      const kws = r.keywords.split(/[\s,]+/).filter(Boolean);
      const match = resolved.find(a => kws.some(k => a.keywords.includes(k)));
      const key = (match && (match.email || match.id)) || r.key || kws[0];
      if (key && r.label.trim()) aliases[key] = r.label.trim();
    }
    const bars = {};
    for (const k of ['session', 'week', 'fable', 'context']) {
      const q = cls => document.querySelector(`#bar-rows .${cls}[data-bar="${k}"]`);
      bars[k] = { show: q('b-show').checked, name: q('b-name').value, from: q('b-from').value, to: q('b-to').value };
    }
    saved = await deck.saveSettings({
      aliases, aliasesExplicit: true, fontSize: $('#set-font').value, zoomFontSize: $('#set-zoomfont').value, fontFamily: effectiveFont(), cwd: $('#set-cwd').value, autoTrust: $('#set-autotrust').checked, resumeChoice: $('#set-resume').value, animations: $('#set-anim').checked, auditorAutoSend: $('#set-auditor-autosend').checked, resumeAfterRestart: $('#set-resume-after-restart').checked,
      effortStyle: $('#set-effort').value, sidebarSide: $('#set-side').value, pinSide: $('#set-pinside').value, driver: $('#set-driver').value, bars,
      barLabels: { time: $('#set-lbl-time').checked, day: $('#set-lbl-day').checked },
      barLabelMode: $('#set-lbl-mode').value,
      grid: { rows: $('#set-grid-rows').value, cols: $('#set-grid-cols').value },
      oneWindow: $('#set-one-window').checked, stepEscape: $('#set-step-esc').checked,
      ...($('#set-away') ? { awayMinutes: $('#set-away').value } : {}),
      asks: { deckThreshold: $('#set-ask-threshold').value.trim() === '' ? null : $('#set-ask-threshold').value, picker: $('#set-ask-picker').value,
        usePages: $('#set-ask-pages').checked, background: $('#set-ask-background').checked, showTile: $('#set-ask-tile').checked,
        showMarks: $('#set-ask-marks').checked, closeOnDone: $('#set-ask-close').checked },
      hibernate: { enabled: $('#set-hib-enabled').checked, idleMinutes: $('#set-hib-idle').value, keepPinned: $('#set-hib-pinned').checked },
      notify: { waiting: $('#set-notify-waiting').checked, done: $('#set-notify-done').checked, asks: $('#set-notify-asks').checked, minTurnSec: $('#set-notify-min').value, sound: $('#set-notify-sound').checked },
      msgLines: { normal: $('#set-msg-normal').value, wide: $('#set-msg-wide').value },
      phone: { enabled: $('#set-phone-on').checked, port: $('#set-phone-port').value, bind: $('#set-phone-bind').value },
      pages: pagesFromText($('#set-phone-pages').value),
    });
    applyUiSettings(saved);
    if (saved.phoneStatus) paintPhone(saved.phoneStatus);
  } catch (e) { status.textContent = `Not saved: ${e.message || e}`; return; }
  state.settings.zoomFontSize = saved.zoomFontSize || 0;
  state.settings.fontSize = saved.fontSize; applyFontSize(saved.fontSize);
  state.settings.fontFamily = saved.fontFamily || ''; rememberFont(saved.fontFamily); applyFontFamily(state.settings.fontFamily);
  // 3. say so when the chosen font is not installed (the terminal silently falls back to Cascadia Mono)
  let note = 'Saved';
  const fam = (saved.fontFamily || '').trim();
  if (fam && document.fonts && typeof document.fonts.check === 'function' && !document.fonts.check(`12px "${fam.replace(/"/g, '')}"`)) {
    note = `Saved. "${fam}" is not installed on this machine, so Cascadia Mono is used until it is.`;
  }
  status.textContent = note; status.title = note;
  if (note === 'Saved') setTimeout(closeSettings, 400);
  else openSettings().then(() => { status.textContent = note; });
});
