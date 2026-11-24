'use strict';
// windows/web-test/fake-deck.js — a deck for the ask pane's test: the REAL lib/phone.js PhoneServer (its /ask/
// route, its token-or-cookie rule, its sandbox CSP) with fake deps standing in for main.js, as test/desk/desk-test.js
// does. It serves the two sample ask pages in fixtures/ (made by the ask-page skill's own ask_page.py build, into a
// temp folder), plays a meta with those asks, keeps drafts / sent / the log the way lib/asks.js does, and records
// every call and every /ask/ request so the test exe can assert on them.
//   node fake-deck.js <port file> <calls file> <fixtures dir> [port] [cloud]
// `cloud` (windows/test/cloud.ps1): the deck also has cloud tiles (two of one account, one of another, one still
// being made), a 2 x 2 grid, and a second server on port + 1 that STANDS IN for claude.ai: plain local pages, so a
// test never loads the real site (windows/src/cloud.hpp maps https://claude.ai onto it for a test run only).
// Loopback only, a made-up token, nothing outside the files named on the command line is written.
// It is also a deck the APP itself can open as a desk (windows/test/asks.ps1: ClaudeDeckNative.exe --window-shot):
// deskInit is answered by the real PhoneServer from `desk.open` below, the one tile has a terminal that prints a
// line a second (so a picture shows the pane repainting), and every frame the app sends is in the calls file.
const fs = require('fs');
const path = require('path');
const REPO = path.resolve(__dirname, '..', '..');
const { PhoneServer } = require(path.join(REPO, 'lib', 'phone.js'));

const [portFile, callsFile, fixtures, portArg, mode] = process.argv.slice(2);
// `cloud4` (windows/test/webmem.ps1): cloud, with a FOURTH tile that has its session (c5), so a 2 x 2 grid can hold four pages
const CLOUD4 = mode === 'cloud4';
const CLOUD = mode === 'cloud' || CLOUD4;
// `legacy` (windows/test/asks.ps1 -Mode legacy): a third session whose page was made before the skill learned the host
// bridge (a plain page: it never says "ready"), and the first session's page REPLACES an older one (2 answers came over)
const LEGACY = mode === 'legacy';
if (!portFile || !callsFile || !fixtures) { console.error('usage: node fake-deck.js <port file> <calls file> <fixtures dir> [port]'); process.exit(2); }
const PORT = +(portArg || 8795), TOKEN = 'faketoken00000001';
const ID_RE = /^ask_[A-Za-z0-9-]+_[0-9a-f]{8}_r\d+_\d{4}-\d{4}_[0-9a-f]{4}$/;

const P1 = 'ask_native-test_11111111_r1_1118-1935_c8f2', S1 = '11111111-2222-4333-8444-555555555555';   // runs outside the deck
const P2 = 'ask_bug-queue_aaaaaaaa_r1_1118-1935_c233', S2 = 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee';     // a tile of this deck
const created = new Date(Date.now() - 12 * 60000).toISOString();
const P3 = 'ask_old-skill_cccccccc_r1_1118-1936_d00d', S3 = 'cccccccc-2222-4333-8444-555555555555';
const rounds = {
  [P1]: { sid: S1, name: 'native-test', account: 'claude', round: 1, total: 5 },
  [P2]: { sid: S2, name: 'bug-queue', account: 'vv', round: 1, total: 2 },
  ...(LEGACY ? { [P3]: { sid: S3, name: 'old-skill', account: 'claude', round: 1, total: 1 } } : {}),
};
const drafts = {}, sent = {}; let log = [];
const record = { calls: [], http: [], sockets: 0, cloudHttp: [] };
// replaced by rename; a reader that has the old one open without sharing makes the rename fail: tried again shortly
let saveAgain = null;
const save = () => {
  try { fs.writeFileSync(callsFile + '.tmp', JSON.stringify(record)); fs.renameSync(callsFile + '.tmp', callsFile); }
  catch (e) { clearTimeout(saveAgain); saveAgain = setTimeout(save, 40); }
};

function asksView() {
  const out = [];
  for (const [id, r] of Object.entries(rounds)) {
    const d = drafts[id];
    out.push({ sessionId: r.sid, name: r.name, account: r.account, ended: false, latest: created, total: r.total, drafted: d ? d.drafted : 0, sent: !!sent[id],
      rounds: [{ id, round: r.round, created, title: '', source: 'skill', background: false, status: 'open', total: r.total, answered: 0, later: 0,
        drafted: d ? d.drafted : 0, sent: sent[id] || null, url: null,
        replaces: LEGACY && id === P1 ? [{ id: 'ask_native-test_11111111_r0_1118-1900_aaaa', sessionId: r.sid, name: r.name, round: 0, moved: 2 }] : [], items: [] }] });
  }
  return out;
}
const meta = () => ({
  now: Date.now(), restoring: false,
  accounts: [{ id: 'vv', label: 'vv', abbr: 'vv', email: 'vv@example.com', key: 'vv@example.com', isDefault: false }]
    .concat(CLOUD ? [{ id: 'work', label: 'Work', abbr: '', email: 'work@example.com', key: 'work@example.com', isDefault: false }] : []),
  ...(CLOUD ? { cloudOption: true } : {}),
  settings: { ...(CLOUD ? { grid: { rows: 2, cols: 2 }, cwd: 'C:\\work\\repo', animations: true } : {}), msgLines: { normal: 3, wide: 10 }, asks: { deckThreshold: null, picker: 'alongside', usePages: true, background: true, showTile: true, showMarks: true, closeOnDone: true } },
  groups: [], usage: {}, triggers: [],
  tiles: [{ id: 't1', kind: 'claude', account: 'vv', sessionId: S2, title: 'Bug queue', status: 'idle', lastAt: Date.now(), held: [], shownBy: [] }]
    .concat(CLOUD ? cloudTiles() : []),
  asks: asksView(),
  askLog: log.map(e => ({ id: e.id, sessionId: e.sessionId, name: e.name, account: e.account, title: e.title, round: e.round, at: e.at, how: e.how, lines: e.lines, total: e.total, answered: e.answered, url: null, replacedBy: null })),
});
// main.js cloudMeta: what a cloud tile's meta carries (cloudId, cloudUrl, cloudError; no transcript fields)
function cloudTiles() {
  const mk = (id, account, cloudId, title, status) => ({ id, kind: 'cloud', account, title, status, lastAt: Date.now(), held: [], shownBy: [],
    cloudId, cloudUrl: cloudId ? `https://claude.ai/code/${cloudId}` : null, cloudError: null, exited: false });
  return [
    mk('c1', 'vv', 'session_01AAAfakecloudone', 'Port the worklist page', 'busy'),
    mk('c2', 'vv', 'session_01BBBfakecloudtwo', 'Fix the label sizes', 'idle'),
    mk('c3', 'work', 'session_01CCCfakecloudthree', 'Read the CSV table', 'waiting'),
    mk('c4', 'vv', null, 'write the tests for the todo list', 'starting'),
  ].concat(CLOUD4 ? [mk('c5', 'vv', 'session_01DDDfakecloudfour', 'Count the racks', 'idle')] : []);
}
const platform = { os: 'win32', isMac: false, keyHelp: { zoom: 'F6', asks: 'F7' } };

const server = new PhoneServer({
  getInit: () => ({ home: 'h', meta: meta(), platform }),
  // the tile's terminal, for a desk: a banner, then what has been printed since (a line a second, below)
  replayFor: (id) => (id === 't1' ? termText : ''),
  askPage: (id) => (rounds[id] || log.some(e => e.id === id) ? path.join(fixtures, `${id}.html`) : null),
  call: (fn, args) => {
    record.calls.push({ fn, args }); save();
    const id = String(args[0] || '');
    if (fn === 'askDraftGet') return drafts[id] ? drafts[id].state : null;
    if (fn === 'askLogState') { const e = log.find(l => l.id === id); return e ? e.state || {} : null; }
    if (fn === 'askDraft') {
      const p = args[1] || {};
      if (!ID_RE.test(id) || !rounds[id]) return false;
      if (!p.drafted) delete drafts[id]; else drafts[id] = { state: p.state || {}, drafted: p.drafted | 0, answered: p.answered | 0, later: p.later | 0, total: p.total | 0 };
      server.sendMeta(meta());
      return true;
    }
    if (fn === 'askClear') { delete drafts[id]; server.sendMeta(meta()); return true; }
    // a picture pasted on the remote (main.js savePastedImage): its path, as if it were saved
    if (fn === 'pasteImage') { const n = Buffer.from(String(args[1] || ''), 'base64').length; const ext = { 'image/png': '.png', 'image/jpeg': '.jpg', 'image/gif': '.gif', 'image/webp': '.webp' }[args[2]]; if (!n || !ext) throw new Error('not a picture this can take (png, jpeg, gif, webp)'); return 'C:\\fake\\paste\\pasted-' + n + ext; }
    if (fn === 'askDone') {
      const sid = id, pages = Array.isArray(args[1]) ? args[1] : [];
      const ours = pages.filter(p => p && ID_RE.test(String(p.id || '')) && p.answers && p.answers.id === p.id && rounds[p.id] && rounds[p.id].sid === sid);
      if (!ours.length) throw new Error('none of these pages is that session\'s');
      if (!ours.some(p => (p.answers.lines || []).some(l => String(l).trim()))) throw new Error('nothing answered yet');
      const tile = sid === S2 ? 't1' : null;
      for (const p of ours) {
        sent[p.id] = Object.assign({ at: Date.now(), how: 'file', file: `<fake>/${p.id}.answers.json` }, tile ? { tileId: tile } : {});
        const r = rounds[p.id];
        log = [{ id: p.id, sessionId: r.sid, name: r.name, account: r.account, title: null, round: r.round, at: Date.now(), how: tile ? 'saved to a file, named in one line' : 'file',
          lines: (p.answers.lines || []).map(String), state: drafts[p.id] ? drafts[p.id].state : {}, total: r.total, answered: +p.answers.answered || 0 }, ...log.filter(l => l.id !== p.id)];
      }
      server.sendMeta(meta());
      return tile ? { how: 'file', files: ours.map(p => sent[p.id].file), tileId: tile, esc: false, waiting: false } : { how: 'file', files: ours.map(p => sent[p.id].file) };
    }
    // test only: the session read its answers (ask_page.py read): the round stops waiting, the log keeps it
    if (fn === '__read') { delete rounds[id]; delete drafts[id]; delete sent[id]; server.sendMeta(meta()); return true; }
    return null;
  },
  desk: { open: () => ({ home: 'h', meta: meta(), platform, layout: {}, windowIndex: 1, grid: CLOUD ? { rows: 2, cols: 2 } : { rows: 1, cols: 1 } }), close: () => {}, shown: () => {}, layout: () => {}, leaving: () => {} },
});
// how each /ask/ request was let in, and exactly what was asked for (the test asserts no token is ever in a page URL)
const onRequest = server.onRequest.bind(server);
server.onRequest = (req, res) => {
  if (req.url.startsWith('/ask/')) { record.http.push({ url: req.url, auth: server.authed(req), cookie: /(^|;\s*)deck_phone=/.test(req.headers.cookie || '') }); save(); }
  return onRequest(req, res);
};
const onSocket = server.onSocket.bind(server);
server.onSocket = (ws) => { record.sockets++; save(); return onSocket(ws); };

// ---- the stand-in for claude.ai (cloud mode): a session page, a sign-in page, a settings page. Plain http on
// loopback; a 'sign-in' is one persistent cookie, so a second run on the same profile folder finds it.
function cloudPage(req, res) {
  const u = new URL(req.url, 'http://x');
  const signed = /(^|;\s*)fake_login=1/.test(req.headers.cookie || '');
  record.cloudHttp.push({ url: req.url, signed, ua: /Edg\//.test(req.headers['user-agent'] || '') ? 'edge' : 'other' }); save();
  const page = (title, body) => { res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(`<!doctype html><meta charset="utf-8"><title>${title}</title><style>body{margin:0;padding:18px 22px;background:#1b1b19;color:#e8e6dc;font:14px/1.5 "Segoe UI",sans-serif}h1{font-size:19px;margin:0 0 6px}a,button{color:#9cc7f5;font:inherit}p{margin:6px 0}input{font:inherit;padding:3px 6px;background:#0f0f0f;color:#eee;border:1px solid #555}.who{color:#3ecf5a}.no{color:#f0b429}</style>${body}`); };
  if (u.pathname === '/login') {
    if (u.searchParams.get('do') === '1') { res.writeHead(302, { 'Set-Cookie': 'fake_login=1; Path=/; Max-Age=31536000', Location: u.searchParams.get('back') || '/' }); return res.end(); }
    return page('sign in', `<h1>stand-in for claude.ai: the sign-in page</h1><p>not signed in</p><p><a id="signin" href="/login?do=1&back=${encodeURIComponent(u.searchParams.get('back') || '/')}">Sign in</a></p>`);
  }
  if (u.pathname === '/settings') return page('settings', `<h1>stand-in for claude.ai: settings</h1><p id="where">the page wandered away from its session</p>`);
  const m = /^\/code\/([A-Za-z0-9_]+)$/.exec(u.pathname);
  if (!m) { res.writeHead(404); return res.end('nothing here'); }
  page(m[1], `<h1>stand-in for claude.ai</h1><p>session <b id="sid">${m[1]}</b>${u.search ? ' <span id="q">' + u.search.replace(/[<&]/g, '') + '</span>' : ''}</p>
<p id="login" class="${signed ? 'who' : 'no'}">${signed ? 'signed in (the cookie of this profile)' : 'not signed in'}</p>
<p>loads of this page in this profile: <b id="n"></b> &middot; device pixels per px: <b id="dpr"></b> &middot; width <b id="w"></b></p>
<p><input id="box" placeholder="type here"> <span id="keys"></span></p>
<p><a id="in" href="/code/${m[1]}?tab=2">a link inside the site</a> &middot; <a id="wander" href="/settings">settings (leaves the session)</a> &middot; <a id="tologin" href="/login?back=${encodeURIComponent(u.pathname)}">the sign-in page</a></p>
<p><a id="out" href="https://example.com/outside">a link to another site</a> &middot; <a id="blank" target="_blank" href="https://example.org/new-window">another site, new window</a> &middot; <button id="pop" onclick="window.open('https://accounts.google.com/o/oauth2/auth?client=fake')">a sign-in popup</button></p>
<script>var k='n:'+location.pathname,n=+(localStorage.getItem(k)||0)+1;localStorage.setItem(k,n);document.getElementById('n').textContent=n;
document.getElementById('dpr').textContent=devicePixelRatio;document.getElementById('w').textContent=innerWidth;
addEventListener('resize',function(){document.getElementById('w').textContent=innerWidth});
window.__k=[];addEventListener('keydown',function(e){window.__k.push(e.key);document.getElementById('keys').textContent=window.__k.join(' ')},true);</script>`);
}
if (CLOUD) require('http').createServer(cloudPage).listen(PORT + 1, '127.0.0.1');

let termText = 'fake deck: the terminal of tile t1 (Bug queue)\r\n', ticks = 0;
server.start({ port: PORT, token: TOKEN, bind: 'localhost' }).then(() => {
  setInterval(() => { const line = `tick ${++ticks}\r\n`; termText = (termText + line).slice(-4000); server.tileData('t1', line); }, 1000);
  save();
  fs.writeFileSync(portFile, String(PORT));
  console.log(`fake deck on 127.0.0.1:${PORT}`);
  setInterval(() => server.sendMeta(meta()), 2000);
}).catch(e => { console.error('fake deck:', e.message); process.exit(1); });
