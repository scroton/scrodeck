'use strict';
// windows/host-test/serve-client.js — a phone, a desk and a few ill-behaved clients, played by Node against the
// native deck's server (run by windows/host-test/serve.ps1, which starts deck-serve-host.exe on 127.0.0.1).
// Loopback only; the port and the token come from the environment (DECK_PORT, DECK_TOKEN), never a command line.
// Prints PASS / FAIL lines; exit code 1 on any failure.
const http = require('http');
const net = require('net');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const ROOT = path.join(__dirname, '..', '..');
const WebSocket = require(path.join(ROOT, 'node_modules', 'ws'));

const port = Number(process.env.DECK_PORT), token = process.env.DECK_TOKEN, work = process.env.DECK_WORK;
if (!(port >= 8811 && port <= 8819) || !token || !work) { console.error('DECK_PORT (8811-8819), DECK_TOKEN and DECK_WORK are required'); process.exit(2); }
const base = `http://127.0.0.1:${port}`;
let passed = 0, failed = 0;
function check(name, ok, detail) { if (ok) { passed++; console.log('PASS  ' + name); } else { failed++; console.log('FAIL  ' + name + (detail !== undefined ? '  -- ' + String(detail).slice(0, 400) : '')); } }
const sleep = ms => new Promise(r => setTimeout(r, ms));
const strip = s => s.replace(/\x1b\][^\x07]*\x07/g, '').replace(/\x1b\[[0-9;?<>=]*[ -\/]*[@-~]/g, '');

function get(target, headers = {}, method = 'GET', body = null) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, method, path: target, headers, agent: false }, res => {
      const chunks = []; res.on('data', c => chunks.push(c)); res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }));
    });
    req.on('error', reject);
    if (body) req.write(body);
    req.end();
  });
}
// a client of the deck: frames in a list, calls by rid
function client(target, headers = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}${target}`, { headers });
    const c = { ws, frames: [], data: {}, bytes: 0, rid: 0, waits: [], closed: false, status: null };
    ws.on('unexpected-response', (_q, res) => { c.status = res.statusCode; res.destroy(); resolve(c); });
    ws.on('open', () => resolve(c));
    ws.on('error', e => { if (!c.status && ws.readyState !== 1) reject(e); });
    ws.on('close', () => { c.closed = true; });
    ws.on('message', raw => {
      const m = JSON.parse(raw);
      c.frames.push(m);
      if (m.t === 'data') { c.data[m.id] = (c.data[m.id] || '') + m.d; c.bytes += m.d.length; }
      c.waits = c.waits.filter(w => !w(m));
    });
    c.send = m => ws.send(JSON.stringify(m));
    c.until = (pred, ms = 15000) => new Promise(res => {
      for (const f of c.frames) if (pred(f)) return res(f);
      const t = setTimeout(() => { c.waits = c.waits.filter(x => x !== w); res(null); }, ms);
      const w = m => { if (!pred(m)) return false; clearTimeout(t); res(m); return true; };
      c.waits.push(w);
    });
    c.call = async (fn, ...args) => { const rid = ++c.rid; c.send({ t: 'call', rid, fn, args }); return c.until(m => (m.t === 'ok' || m.t === 'err') && m.rid === rid); };
    c.sees = (id, text, ms = 20000) => new Promise(res => { const at = Date.now(); const look = () => { if (strip(c.data[id] || '').includes(text)) return res(true); if (Date.now() - at > ms) return res(false); setTimeout(look, 50); }; look(); });
    c.lastMeta = () => { for (let i = c.frames.length - 1; i >= 0; i--) if (c.frames[i].t === 'meta' || c.frames[i].t === 'init') return c.frames[i].meta; return null; };
  });
}

// one masked text frame, as a client must send it (RFC 6455)
function maskedText(text) {
  const p = Buffer.from(text), key = crypto.randomBytes(4);
  const head = p.length < 126 ? Buffer.from([0x81, 0x80 | p.length]) : Buffer.from([0x81, 0x80 | 126, p.length >> 8, p.length & 0xff]);
  const body = Buffer.alloc(p.length); for (let i = 0; i < p.length; i++) body[i] = p[i] ^ key[i & 3];
  return Buffer.concat([head, key, body]);
}
// a socket that shakes hands and then never answers a ping: what a phone out of range looks like — or, with
// `target` '/desk/ws' and `say` a deskInit, a remote desk whose connection died behind a proxy. `out.texts` are
// the text frames it was sent; `out.close()` ends it early.
function silentSocket(target = '/', say = null, giveUpMs = 70000) {
  const s = net.connect(port, '127.0.0.1');
  const out = { pings: 0, closedAfterMs: null, upgraded: false, texts: [], close: () => s.destroy() };
  out.done = new Promise(resolve => {
    const at = Date.now(); let buf = Buffer.alloc(0), head = true;
    s.on('connect', () => s.write(`GET ${target}?t=${token} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
    s.on('data', d => {
      buf = Buffer.concat([buf, d]);
      if (head) { const i = buf.indexOf('\r\n\r\n'); if (i < 0) return; out.upgraded = buf.slice(0, 12).toString() === 'HTTP/1.1 101'; buf = buf.slice(i + 4); head = false; if (out.upgraded && say) s.write(maskedText(JSON.stringify(say))); }
      for (;;) {                                   // walk the server's (unmasked) frames, counting pings
        if (buf.length < 2) return;
        let len = buf[1] & 0x7f, off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (buf.length < off + len) return;
        if ((buf[0] & 0x0f) === 9) out.pings++;
        if ((buf[0] & 0x0f) === 1) { try { out.texts.push(JSON.parse(buf.slice(off, off + len).toString())); } catch { /* not ours to judge */ } }
        buf = buf.slice(off + len);
      }
    });
    const done = () => { if (out.closedAfterMs === null) { out.closedAfterMs = Date.now() - at; resolve(out); } };
    s.on('close', done); s.on('error', done);
    setTimeout(() => { s.destroy(); done(); }, giveUpMs);
  });
  return out;
}

(async () => {
  const silent = silentSocket().done;              // runs alongside everything else: it needs 45 s

  // ---- the pages, the token, the cookie
  let r = await get('/');
  check('http: no token -> 401, with the page that says where the link is', r.status === 401 && /this link needs its token/.test(r.body.toString()));
  r = await get('/?t=' + token.slice(0, -1) + 'x');
  check('http: a wrong token -> 401', r.status === 401);
  r = await get('/?t=' + token);
  const cookie = ((r.headers['set-cookie'] || [])[0] || '').split(';')[0];
  check('http: the token in the query -> the phone page, and the cookie that keeps it', r.status === 200 && /<title>/.test(r.body.toString()) && cookie === 'deck_phone=' + token && /HttpOnly/.test(r.headers['set-cookie'][0]), r.status);
  r = await get('/app.js', { cookie });
  check('http: the cookie alone opens the page\'s own files, never cached', r.status === 200 && r.headers['cache-control'] === 'no-cache' && r.body.length > 100000 && r.body.equals(fs.readFileSync(path.join(ROOT, 'renderer', 'phone', 'app.js'))));
  r = await get('/vendor/xterm.js', { cookie });
  check('http: the vendor files', r.status === 200 && r.body.equals(fs.readFileSync(path.join(ROOT, 'node_modules', '@xterm', 'xterm', 'lib', 'xterm.js'))));
  r = await get('/manifest.webmanifest');
  check('http: the manifest and icons are public', r.status === 200 && r.headers['content-type'] === 'application/manifest+json' && (await get('/icon-192.png')).status === 200);
  r = await get('/desk/', { cookie });
  check('http: /desk/ is the desktop renderer with <base> and the bridge added', r.status === 200 && r.body.toString().includes('<base href="/desk/renderer/">') && r.body.toString().includes('<script src="/desk/renderer/desk/bridge.js"></script>') && r.body.toString().includes("connect-src 'self' ws: wss:"));
  r = await get('/desk/main.js', { cookie });
  check('http: /desk/ serves only the files the page needs', r.status === 404 && (await get('/desk/renderer/desk/bridge.js', { cookie })).status === 200 && (await get('/desk/lib/layout.js', { cookie })).status === 200);
  r = await get('/upload', { cookie, 'x-file-name': encodeURIComponent('note from the phone.txt') }, 'POST', Buffer.from('uploaded bytes'));
  const up = r.status === 200 ? JSON.parse(r.body.toString()) : {};
  check('http: POST /upload saves the file under the deck\'s state folder and hands back its path', r.status === 200 && up.path && up.path.startsWith(work) && /uploads/.test(up.path) && fs.readFileSync(up.path, 'utf8') === 'uploaded bytes', r.body.toString());
  check('http: an upload without the token is refused', (await get('/upload', {}, 'POST', Buffer.from('x'))).status === 401);

  // ---- the socket: refused without the token
  let c = await client('/');
  check('socket: no token -> 401 before the handshake', c.status === 401);
  c = await client('/?t=nope');
  check('socket: a wrong token -> 401', c.status === 401);

  // ---- a phone
  const phone = await client('/?t=' + token);
  const init = await phone.until(m => m.t === 'init');
  check('phone: the hello: init with home, platform, meta, the page\'s build and fingerprint', !!init && Array.isArray(init.meta.tiles) && init.platform.os === 'win32' && typeof init.home === 'string' && /^v\d+/.test(init.build || '') && /app\.js:\d/.test(init.assets) && /^[0-9a-f]{16}$/.test(init.pageKey),
    init && JSON.stringify(Object.keys(init)));
  const made = await phone.call('newTile', { kind: 'shell', cwd: work });
  const id = made && made.result;
  check('phone: newTile({ kind: "shell" }) answers the tile\'s id', made && made.t === 'ok' && typeof id === 'string', JSON.stringify(made));
  await phone.call('renameTile', id, 'native serve one');
  phone.send({ t: 'attach', id });
  check('phone: attached, the shell\'s prompt arrives', await phone.sees(id, '>'));
  phone.send({ t: 'call', rid: 0, fn: 'userInput', args: [id] });
  phone.send({ t: 'input', id, d: 'echo serve-phone-ok\r' });
  check('phone: what it types runs, and the output comes back', await phone.sees(id, '\nserve-phone-ok') || (strip(phone.data[id] || '').match(/serve-phone-ok/g) || []).length >= 2, strip(phone.data[id] || '').slice(-200));
  phone.send({ t: 'resize', id, cols: 51, rows: 21 });
  const sized = await phone.until(m => m.t === 'meta' && m.meta.tiles.some(t => t.id === id && t.ptySize && t.ptySize.cols === 51 && t.ptySize.rows === 21), 8000);
  check('phone: attached to the tile, it owns the size (auto): the pty takes the phone\'s', !!sized);
  const t0 = phone.lastMeta().tiles.find(t => t.id === id);
  check('phone: the meta shows the tile as the deck knows it', t0 && t0.title === 'native serve one' && t0.kind === 'shell' && t0.status === 'shell' && t0.driver === 'phone' && t0.shownBy.includes('remote'), JSON.stringify(t0));
  const unknown = await phone.call('setOscTitle', id, 'x');
  check('phone: an action outside the table is refused by name', unknown && unknown.t === 'err' && unknown.error === 'unknown action: setOscTitle');

  // ---- a desk; one desk at a time
  const desk = await client('/desk/', { cookie });
  const dinit = await desk.call('deskInit', { name: 'TEST-DESK' });
  check('desk: deskInit registers it as a window of the deck', dinit && dinit.t === 'ok' && Number.isInteger(dinit.result.windowIndex) && dinit.result.grid.rows === 2 && dinit.result.meta.tiles.some(t => t.id === id && 'mirror' in t), JSON.stringify(dinit).slice(0, 300));
  desk.send({ t: 'attach', id });
  desk.send({ t: 'shown', ids: [id], hidden: false });
  desk.send({ t: 'layout', layout: { [id]: [1, 2, 3, 4] } });
  check('desk: its attach replays the screen so far', await desk.sees(id, 'serve-phone-ok'));
  // The remote-desk rule (2026-11-18): a remote desk's panes are its own. Its layout is kept (tiles.json deskLayout),
  // but no window of the deck "holds" the tile because of it, the desk is not "on screen" for sleep, and a desk is
  // told inWindow null. (Size is another matter: under auto, with no phone on the tile... the phone IS attached
  // here, so the desk mirrors the pty.)
  const tilesFile = () => { try { return JSON.parse(fs.readFileSync(path.join(work, 'local', 'tiles.json'), 'utf8')); } catch { return {}; } };
  let kept = false;
  for (let i = 0; i < 80 && !kept; i++) { kept = JSON.stringify((tilesFile().deskLayout || {})[id]) === '[1,2,3,4]'; if (!kept) await sleep(100); }
  check('desk: its layout is its own and is kept (deskLayout)', kept, JSON.stringify(tilesFile().deskLayout));
  // (the meta that follows its `shown`: it shows the tile now, so it is told what to render it at)
  const after = await desk.until(m => m.t === 'meta' && m.meta.tiles.some(t => t.id === id && t.mirror), 8000);
  const dt = after && after.meta.tiles.find(t => t.id === id);
  check('desk: placing and showing a tile makes no window of the deck its holder, and is not "on screen" (the remote-desk rule of 2026-11-18)',
    !!dt && dt.inWindow === null && !dt.shownBy.some(s => /^win:/.test(s)) && dt.shownBy.includes('remote') /* the phone, still attached */, JSON.stringify(dt && { inWindow: dt.inWindow, shownBy: dt.shownBy }));
  check('desk: the claim is a proposal, off here: this tree\'s driver words, no sizeBy, no remoteDesk', !!after && after.meta.settings.driver === 'auto' && !('sizeBy' in dt) && !('remoteDesk' in after.meta), JSON.stringify(after && after.meta.settings.driver));
  const second = await client('/desk/?t=' + token);
  const refused = await second.call('deskInit', { name: 'SECOND' });
  check('desk: a second desk is refused while the first is connected', refused && refused.t === 'err' && /^one desk at a time/.test(refused.error), JSON.stringify(refused));
  desk.ws.close();
  await sleep(600);
  const allowed = await second.call('deskInit', { name: 'SECOND' });
  check('desk: once the first has gone, the second may register (same window index)', allowed && allowed.t === 'ok' && allowed.result.windowIndex === dinit.result.windowIndex, JSON.stringify(allowed).slice(0, 200));
  second.ws.close();

  // ---- a slow client: over 4 MB waiting, the deck skips frames for it rather than queue them for ever
  const fast = await client('/?t=' + token), slow = await client('/?t=' + token);
  fast.send({ t: 'attach', id }); slow.send({ t: 'attach', id });
  await fast.sees(id, 'serve-phone-ok'); await slow.sees(id, 'serve-phone-ok');
  const big = path.join(work, 'big.txt');
  { const line = '0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz\r\n'; fs.writeFileSync(big, line.repeat(150000)); }   // ~16 MB
  slow.ws._socket.pause();                         // it stops reading: the server's writes to it back up
  const fastBefore = fast.bytes;
  phone.send({ t: 'input', id, d: `type "${big}" & echo big-is-done\r` });
  const finished = await fast.sees(id, '\nbig-is-done', 180000);
  const slowAtPause = slow.bytes;
  slow.ws._socket.resume();
  await sleep(2500);
  phone.send({ t: 'input', id, d: 'echo after-the-flood\r' });
  const alive = await slow.sees(id, 'after-the-flood', 20000);
  check('skip: the fast client saw all of a 16 MB burst', finished && fast.bytes - fastBefore > 8 * 1024 * 1024, `${fast.bytes - fastBefore} bytes`);
  check('skip: the slow one was sent less (frames were skipped, not queued), and is alive afterwards', alive && slow.bytes < fast.bytes - 2 * 1024 * 1024 && !slow.closed, `slow ${slow.bytes} (at pause ${slowAtPause}) vs fast ${fast.bytes}`);
  const metaAfter = await slow.until(m => m.t === 'meta', 25000);
  check('skip: and it still gets the meta', !!metaAfter);
  slow.send({ t: 'attach', id });
  check('skip: its next attach replays the window, so the screen heals', await slow.sees(id, 'after-the-flood'));
  fast.ws.close(); slow.ws.close();
  try { fs.unlinkSync(big); } catch { /* the folder goes anyway */ }

  // ---- left for the browser and the native desk (serve.ps1 runs them next): the tile stays, the phone goes
  fs.writeFileSync(path.join(work, 'tile.json'), JSON.stringify({ id, cookie: 'kept in the environment, not here' }));
  phone.ws.close();

  // ---- the desk slot: a desk whose connection died must not hold it (2026-11-18). A raw socket registers as the
  // desk and then answers nothing; a second desk is refused while it was just heard from, and takes its place
  // once it has been silent for 10 s and lets a 3 s ping go unanswered.
  { const dead = silentSocket('/desk/ws', { t: 'call', rid: 1, fn: 'deskInit', args: [{ name: 'DEAD-DESK', kind: 'native' }] }, 40000);
    for (let i = 0; i < 50 && !dead.texts.some(m => m.rid === 1); i++) await sleep(100);
    const got = dead.texts.find(m => m.rid === 1);
    const next = await client('/desk/ws', { cookie });
    const early = await next.call('deskInit', { name: 'NEXT-DESK', kind: 'native' });
    check('desk slot: a desk heard from a moment ago holds the slot', !!got && got.t === 'ok' && early && early.t === 'err' && /^one desk at a time/.test(early.error), JSON.stringify(early));
    await sleep(10500);
    const t0 = Date.now();
    const taken = await next.call('deskInit', { name: 'NEXT-DESK', kind: 'native' });
    const waited = Date.now() - t0;
    const deadEnd = await Promise.race([dead.done, sleep(2000).then(() => null)]);
    check('desk slot: silent for 10 s and deaf to a 3 s ping, it loses the slot to the new desk', taken && taken.t === 'ok' && taken.result.windowIndex === got.result.windowIndex && waited >= 2800 && waited < 6000, `${waited} ms ${JSON.stringify(taken).slice(0, 120)}`);
    check('desk slot: ...and its socket was ended by the deck', !!deadEnd && deadEnd.pings >= 1, JSON.stringify(deadEnd && { pings: deadEnd.pings, closedAfterMs: deadEnd.closedAfterMs }));
    dead.close(); next.ws.close(); }

  // ---- the heartbeat
  const hb = await silent;
  check('heartbeat: a socket that never answers a ping gets two, and is dropped at the third beat', hb.upgraded && hb.pings === 2 && hb.closedAfterMs > 40000 && hb.closedAfterMs < 60000, JSON.stringify(hb));

  console.log(`\n${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
