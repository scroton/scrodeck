'use strict';
// windows/host-test/serve-app.js — a Node phone against ClaudeDeckNative.exe --local --serve <port> (step 5 of
// serve.ps1): the app's own in-process deck, with its window, serving at the same time. Loopback only.
const path = require('path');
const WebSocket = require(path.join(__dirname, '..', '..', 'node_modules', 'ws'));
const port = Number(process.env.DECK_PORT), token = process.env.DECK_TOKEN, work = process.env.DECK_WORK;
if (!(port >= 8811 && port <= 8819) || !token) { console.error('DECK_PORT (8811-8819) and DECK_TOKEN are required'); process.exit(2); }
let failed = 0;
const check = (name, ok, d) => { if (!ok) failed++; console.log((ok ? 'PASS  ' : 'FAIL  ') + name + (!ok && d ? '  -- ' + String(d).slice(0, 300) : '')); };
const ws = new WebSocket(`ws://127.0.0.1:${port}/?t=${token}`);
const frames = []; let data = '';
const until = (pred, ms = 10000) => new Promise(res => { const at = Date.now(); const look = () => { const f = frames.find(pred); if (f) return res(f); if (Date.now() - at > ms) return res(null); setTimeout(look, 40); }; look(); });
ws.on('message', raw => { const m = JSON.parse(raw); frames.push(m); if (m.t === 'data') data += m.d; });
ws.on('error', e => { console.log('FAIL  app --serve: the socket: ' + e.message); process.exit(1); });
ws.on('open', async () => {
  const init = await until(m => m.t === 'init');
  check('app --serve: the app\'s own deck answers a phone with the hello', !!init && init.platform.os === 'win32');
  ws.send(JSON.stringify({ t: 'call', rid: 1, fn: 'newTile', args: [{ kind: 'shell', cwd: work }] }));
  const made = await until(m => m.t === 'ok' && m.rid === 1);
  check('app --serve: a phone makes a shell tile in it', !!made && typeof made.result === 'string');
  const id = made && made.result;
  ws.send(JSON.stringify({ t: 'call', rid: 2, fn: 'renameTile', args: [id, 'made by a phone'] }));
  ws.send(JSON.stringify({ t: 'attach', id }));
  ws.send(JSON.stringify({ t: 'input', id, d: 'echo app-serve-ok\r' }));
  const at = Date.now(); while (Date.now() - at < 12000 && (data.match(/app-serve-ok/g) || []).length < 2) await new Promise(r => setTimeout(r, 50));
  check('app --serve: typing from the phone runs in the app\'s shell tile', (data.match(/app-serve-ok/g) || []).length >= 2, data.slice(-200));
  ws.close();
  setTimeout(() => process.exit(failed ? 1 : 0), 200);
});
