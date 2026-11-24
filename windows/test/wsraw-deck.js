'use strict';
// windows/test/wsraw-deck.js — the decks the exe's own WebSocket client (windows/src/wsraw.hpp) is tested against
// (windows/test/wsraw.ps1). Loopback only, a made-up token, nothing written but the result file named below.
//   node wsraw-deck.js <mode> <port> <result file> [cert dir]
// deck       the REAL lib/phone.js PhoneServer (its permessage-deflate: threshold 1 KB, serverMaxWindowBits 13) over a
//            fake main: deskInit answers, `attach big` replays ~200 KB of terminal-like output. Per socket the result
//            says what was agreed (extensions), the bytes the server wrote and read on the TCP socket, the payload
//            bytes it sent (before compression), and how the close went (close frames sent / received, the code).
// nodeflate  the same with the server's permessage-deflate turned off (the extension refused).
// breakraw   the same, but a socket that agreed permessage-deflate (the exe's own client; WinHTTP never asks) gets
//            a compressed frame that does not inflate: the client must give up its own path (auto: WinHTTP).
// frames     a hand-made server (no ws library on the wire): a script of frames the client must echo - the 7/16/64-
//            bit lengths at their edges, a fragmented message with a ping inside it, a frame written a byte at a
//            time, compressed messages (one frame, fragmented, the window carried across messages, a final block, a
//            fresh stream after it), a 125-byte ping, then the server's close. Every client frame is checked to be
//            masked and FIN.
// tls        `frames` over TLS (key.pem / cert.pem in the cert dir: a throwaway self-signed certificate).
const fs = require('fs'), path = require('path'), crypto = require('crypto'), zlib = require('zlib');
const REPO = path.resolve(__dirname, '..', '..');
const [mode, portArg, resultFile, certDir] = process.argv.slice(2);
const MODES = ['deck', 'nodeflate', 'breakraw', 'frames', 'tls'];
if (!MODES.includes(mode) || !portArg || !resultFile) { console.error('usage: node wsraw-deck.js <' + MODES.join('|') + '> <port> <result file> [cert dir]'); process.exit(2); }
const PORT = +portArg, TOKEN = 'wsrawtoken0000001';
const result = { mode, port: PORT, sockets: [], steps: [], errors: [] };
const save = () => { try { fs.writeFileSync(resultFile + '.tmp', JSON.stringify(result, null, 1)); fs.renameSync(resultFile + '.tmp', resultFile); } catch (e) { setTimeout(save, 40); } };

// ~200 KB of what a terminal shows: colours, a progress line, paths, numbers - the same bytes every run
function bigReplay() {
  let seed = 12345; const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const words = ['compile', 'src', 'lib', 'renderer', 'deck', 'session', 'tile', 'layout', 'phone', 'native', 'test', 'build', 'done', 'warning', 'ok'];
  let s = '';
  for (let i = 0; s.length < 200000; i++) {
    const w = () => words[(rnd() * words.length) | 0];
    if (i % 7 === 0) s += `\x1b[2K\r\x1b[36m[${String(i).padStart(4)}/2000]\x1b[0m ${'#'.repeat((rnd() * 40) | 0)}\r\n`;
    else s += `\x1b[32m✔\x1b[0m ${w()}/${w()}_${(rnd() * 1e6) | 0}.ts  \x1b[2m${(rnd() * 900 + 10).toFixed(1)} ms\x1b[0m  ${crypto.createHash('md5').update(String(i)).digest('hex').slice(0, (rnd() * 16) | 0)}\r\n`;
  }
  return s;
}

if (mode === 'deck' || mode === 'nodeflate' || mode === 'breakraw') {
  const { PhoneServer } = require(path.join(REPO, 'lib', 'phone.js'));
  const replay = bigReplay();
  result.replay = { chars: replay.length, bytes: Buffer.byteLength(replay), sha256: crypto.createHash('sha256').update(replay, 'utf8').digest('hex') };
  const tiles = [{ id: 'big', kind: 'shell', title: 'big replay', status: 'shell', ptySize: { cols: 120, rows: 40 }, mouseOn: false, sgrMouse: false, driver: null }];
  const meta = () => ({ now: Date.now(), restoring: false, accounts: [], settings: { driver: 'auto', grid: { rows: 1, cols: 1 } }, groups: [], usage: {}, triggers: [], asks: [], askLog: [],
    tiles: tiles.map(t => ({ ...t, lastAt: Date.now(), held: [], shownBy: [], mirror: null })) });
  const platform = { os: 'win32', isMac: false, shellName: 'PowerShell', keyHelp: { zoom: 'F6', asks: 'F7' } };
  const server = new PhoneServer({
    getInit: () => ({ home: 'h', meta: meta(), platform }),
    replayFor: id => (id === 'big' ? replay : `tile ${id}\r\n`),
    call: () => null,
    desk: { open: () => ({ home: 'h', meta: meta(), platform, layout: {}, windowIndex: 1, grid: { rows: 1, cols: 1 } }), close: () => {}, shown: () => {}, layout: () => {}, leaving: () => {} },
  });
  // what the server sent, before compression, per socket
  const sendTo = server.sendTo.bind(server);
  server.sendTo = (ws, msg) => { if (ws.rec) { ws.rec.payloadBytes += Buffer.byteLength(JSON.stringify(msg)); ws.rec.messages++; } return sendTo(ws, msg); };
  const onSocket = server.onSocket.bind(server);
  server.onSocket = ws => {
    const sock = ws._socket;
    const rec = { extensions: ws.extensions || '', via: ws.extensions ? 'raw' : 'winhttp', messages: 0, payloadBytes: 0, frames: [], at: Date.now() };
    ws.rec = rec; result.sockets.push(rec); save();
    const w0 = sock.bytesWritten, r0 = sock.bytesRead;   // the upgrade's answer is already written: counted apart
    rec.upgradeBytes = w0;
    ws.on('message', raw => { rec.frames.push(String(raw).slice(0, 80)); save(); });
    ws.on('close', (code) => {
      rec.closeCode = code; rec.closeFrameReceived = ws._closeFrameReceived; rec.closeFrameSent = ws._closeFrameSent;
      rec.wireBytes = sock.bytesWritten - w0; rec.readBytes = sock.bytesRead - r0; rec.ms = Date.now() - rec.at; save();
    });
    if (mode === 'breakraw' && ws.extensions) {
      // RSV1 + FIN text, 5 bytes that are no deflate stream (a stored block whose LEN and NLEN disagree)
      sock.write(Buffer.from([0xc1, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05]));
      rec.broken = true; save();
      return;
    }
    onSocket(ws);
  };
  server.start({ port: PORT, token: TOKEN, bind: 'localhost' }).then(() => {
    if (mode === 'nodeflate') server.wss.options.perMessageDeflate = false;
    save(); console.log(`wsraw deck (${mode}) on 127.0.0.1:${PORT}`);
    setInterval(() => server.sendMeta(meta()), 1000);
  }).catch(e => { console.error('wsraw deck:', e.message); process.exit(1); });
} else {
  // ------------------------------------------------------------ frames / tls: the wire by hand
  const http = require('http'), https = require('https');
  const srv = mode === 'tls'
    ? https.createServer({ key: fs.readFileSync(path.join(certDir, 'key.pem')), cert: fs.readFileSync(path.join(certDir, 'cert.pem')) })
    : http.createServer();
  srv.on('request', (_q, res) => { res.writeHead(404); res.end(); });
  const frame = (op, payload, { fin = true, rsv1 = false } = {}) => {
    const n = payload.length; let h;
    if (n < 126) h = Buffer.from([(fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | op, n]);
    else if (n < 65536) { h = Buffer.alloc(4); h[0] = (fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | op; h[1] = 126; h.writeUInt16BE(n, 2); }
    else { h = Buffer.alloc(10); h[0] = (fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | op; h[1] = 127; h.writeBigUInt64BE(BigInt(n), 2); }
    return Buffer.concat([h, payload]);
  };
  const text = (n, salt) => { let s = ''; for (let i = 0; s.length < n; i++) s += `${salt}:${i} the quick brown fox ${i * 7919 % 1000}; `; return Buffer.from(s.slice(0, n)); };
  srv.on('upgrade', (req, sock) => {
    const rec = { url: req.url, offered: req.headers['sec-websocket-extensions'] || '', masked: true, fin: true, echoes: 0, at: Date.now() };
    result.sockets.push(rec); save();
    if (!/[?&]t=wsrawtoken0000001(&|$)/.test(req.url)) { sock.end('HTTP/1.1 401 Unauthorized\r\n\r\n'); rec.refused = 401; save(); return; }
    const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    const deflate = /permessage-deflate/.test(rec.offered);
    sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n' +
      (deflate ? 'Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=13\r\n' : '') + '\r\n');
    sock.setNoDelay(true);
    // the client's frames, unmasked
    let buf = Buffer.alloc(0); const inbox = []; let wake = null;
    sock.on('data', d => {
      buf = Buffer.concat([buf, d]);
      for (;;) {
        if (buf.length < 2) break;
        const b0 = buf[0], b1 = buf[1]; let len = b1 & 0x7f, h = 2;
        if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); h = 4; } else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); h = 10; }
        const masked = !!(b1 & 0x80); if (!masked) rec.masked = false;
        if (!(b0 & 0x80)) rec.fin = false;
        if (buf.length < h + (masked ? 4 : 0) + len) break;
        const key = masked ? buf.subarray(h, h + 4) : null; const body = Buffer.from(buf.subarray(h + (masked ? 4 : 0), h + (masked ? 4 : 0) + len));
        if (key) for (let i = 0; i < body.length; i++) body[i] ^= key[i & 3];
        inbox.push({ op: b0 & 0x0f, rsv1: !!(b0 & 0x40), body }); buf = buf.subarray(h + (masked ? 4 : 0) + len);
      }
      if (wake && inbox.length) { const w = wake; wake = null; w(); }
    });
    const next = (ms = 5000) => new Promise((resolve) => {
      const take = () => { const f = inbox.shift(); resolve(f || null); };
      if (inbox.length) return take();
      const t = setTimeout(() => { wake = null; resolve(null); }, ms);
      wake = () => { clearTimeout(t); take(); };
    });
    const sleep = ms => new Promise(r => setTimeout(r, ms));
    const step = (name, ok, detail) => { result.steps.push({ name, ok: !!ok, detail: detail || '' }); save(); };
    const expectEcho = async (name, want) => {
      const f = await next();
      const ok = f && f.op === 1 && !f.rsv1 && f.body.equals(want);
      if (ok) rec.echoes++;
      step(name, ok, f ? `op ${f.op} rsv1 ${f.rsv1} ${f.body.length} bytes (want ${want.length})` : 'no echo');
    };
    (async () => {
      await sleep(150);
      for (const n of [0, 1, 125, 126, 127, 65535, 65536, 200000]) { const t = text(n, 'len' + n); sock.write(frame(1, t)); await expectEcho(`a ${n}-byte message comes back intact`, t); }
      // fragmented, a ping between the first and second fragment: the pong comes first, then the whole message
      { const a = text(3000, 'fragA'), b = text(10, 'fragB'), c = text(70000, 'fragC');
        sock.write(frame(1, a, { fin: false })); sock.write(frame(9, Buffer.from('mid-message ping'))); sock.write(frame(0, b, { fin: false })); sock.write(frame(0, c));
        const p = await next(); step('a ping inside a fragmented message is answered (pong, same bytes)', p && p.op === 10 && p.body.toString() === 'mid-message ping', p ? `op ${p.op} ${p.body}` : 'nothing');
        await expectEcho('the fragmented message (3 fragments) comes back whole', Buffer.concat([a, b, c])); }
      // a frame a byte at a time (TCP segments of one byte: the header split everywhere)
      { const t = text(300, 'bytewise'); const f = frame(1, t);
        for (let i = 0; i < f.length; i++) { sock.write(f.subarray(i, i + 1)); if (i < 12 || i % 50 === 0) await sleep(3); }
        await expectEcho('a frame written a byte at a time comes back intact', t); }
      if (deflate) {
        // one deflate stream across messages (context takeover), sync-flushed, the 00 00 FF FF tail removed (RFC 7692 7.2.1)
        const z = zlib.createDeflateRaw({ windowBits: 13, level: 6, memLevel: 7 });
        const squeeze = buf => new Promise(resolve => { const out = []; const on = d => out.push(d); z.on('data', on); z.write(buf); z.flush(zlib.constants.Z_SYNC_FLUSH, () => { z.off('data', on); let o = Buffer.concat(out); if (o.subarray(-4).equals(Buffer.from([0, 0, 0xff, 0xff]))) o = o.subarray(0, -4); resolve(o); }); });
        const m1 = text(50000, 'deflate-one'); const c1 = await squeeze(m1);
        sock.write(frame(1, c1, { rsv1: true }));
        await expectEcho(`a compressed message (one frame, ${m1.length} -> ${c1.length} bytes) inflates`, m1);
        const m2 = text(40000, 'deflate-two'); const c2 = await squeeze(m2);
        const k1 = Math.floor(c2.length / 3), k2 = Math.floor(2 * c2.length / 3);
        sock.write(frame(1, c2.subarray(0, k1), { fin: false, rsv1: true })); sock.write(frame(9, Buffer.from('ping in a compressed message'))); sock.write(frame(0, c2.subarray(k1, k2), { fin: false })); sock.write(frame(0, c2.subarray(k2)));
        const p = await next(); step('a ping inside a compressed fragmented message is answered', p && p.op === 10, p ? `op ${p.op}` : 'nothing');
        await expectEcho('a compressed message in 3 fragments (RSV1 on the first only) inflates', m2);
        const m3 = Buffer.from(m2.subarray(m2.length - 6000)); const c3 = await squeeze(m3);   // the window is 8 KB (server_max_window_bits=13): the end of the message before
        sock.write(frame(1, c3, { rsv1: true }));
        await expectEcho(`a message made of earlier ones inflates from the carried window (${m3.length} -> ${c3.length} bytes)`, m3);
        step('the carried window is used (the repeat squeezes below 3 %; alone it would not)', c3.length * 100 < m3.length * 3 && zlib.deflateRawSync(m3, { windowBits: 13 }).length * 100 > m3.length * 3, `${c3.length} of ${m3.length}, alone ${zlib.deflateRawSync(m3, { windowBits: 13 }).length}`);
        // a final block (BFINAL; allowed), then a message on a fresh stream
        const m4 = text(9000, 'final-block'); const c4 = zlib.deflateRawSync(m4, { windowBits: 13 });
        sock.write(frame(1, c4, { rsv1: true }));
        await expectEcho('a compressed message that ends in a final block inflates', m4);
        const m5 = text(5000, 'after-final'); const c5 = zlib.deflateRawSync(m5, { windowBits: 13, finishFlush: zlib.constants.Z_SYNC_FLUSH }).subarray(0, -4);
        sock.write(frame(1, c5, { rsv1: true }));
        await expectEcho('the message after a final block (a new stream) inflates', m5);
        const m6 = Buffer.from('small, sent uncompressed although the extension is on'); sock.write(frame(1, m6));
        await expectEcho('an uncompressed message on a compressing connection', m6);
      } else step('no permessage-deflate offered by the client', false, rec.offered);
      { const big = Buffer.alloc(125, 0x41); sock.write(frame(9, big)); const p = await next(); step('a 125-byte ping (the control maximum) is answered with the same 125 bytes', p && p.op === 10 && p.body.equals(big), p ? `op ${p.op} ${p.body.length}` : 'nothing'); }
      // the server's goodbye: the client answers with its own close frame
      sock.write(frame(8, Buffer.from([0x03, 0xe8])));
      const cl = await next(); step('the server\'s close is answered with a close frame', cl && cl.op === 8, cl ? `op ${cl.op} code ${cl.body.length >= 2 ? cl.body.readUInt16BE(0) : '-'}` : 'nothing');
      rec.done = true; save();
      sock.end();
    })().catch(e => { result.errors.push(String(e && e.stack || e)); save(); sock.destroy(); });
  });
  srv.listen(PORT, '127.0.0.1', () => { save(); console.log(`wsraw ${mode} server on 127.0.0.1:${PORT}`); });
}
process.on('SIGTERM', () => { save(); process.exit(0); });
