'use strict';
// windows/test/wsraw-inflate.js — the inflater's cases (windows/src/wsraw.hpp PmdInflater), made by Node's zlib the way
// a permessage-deflate server makes them (RFC 7692 7.2.1: raw DEFLATE, a sync flush per message, the 00 00 FF FF
// tail removed; one stream across messages unless no_context_takeover).
//   node wsraw-inflate.js <cases.json>        (windows/test/wsraw.ps1 runs it, then `wsraw_test inflate`)
const fs = require('fs'), zlib = require('zlib'), crypto = require('crypto');
const out = process.argv[2];
if (!out) { console.error('usage: node wsraw-inflate.js <cases.json>'); process.exit(2); }
const C = zlib.constants;
let seed = 777; const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
const term = n => { let s = ''; for (let i = 0; s.length < n; i++) s += `\x1b[3${i % 8}m${['ok', 'build', 'src/deck', 'tile', '✔ done'][(rnd() * 5) | 0]} ${(rnd() * 1e5) | 0}\x1b[0m\r\n`; return Buffer.from(s).subarray(0, n); };
const noise = n => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] = (rnd() * 256) | 0; return b; };
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const strip = b => (b.length >= 4 && b.subarray(-4).equals(Buffer.from([0, 0, 0xff, 0xff])) ? b.subarray(0, -4) : b);
const msg = (plain, z) => ({ z: z.toString('base64'), len: plain.length, sha: sha(plain) });

// one stream, a sync flush per message (deflateRawSync on a shared handle is not possible: the stream object does it)
function stream(messages, opts) {
  return new Promise(resolve => {
    const z = zlib.createDeflateRaw(opts); const res = [];
    let i = 0;
    const next = () => {
      if (i >= messages.length) { z.close(); return resolve(res); }
      const plain = messages[i++]; const chunks = []; const on = d => chunks.push(d);
      z.on('data', on); z.write(plain);
      z.flush(C.Z_SYNC_FLUSH, () => { z.off('data', on); res.push(msg(plain, strip(Buffer.concat(chunks)))); next(); });
    };
    next();
  });
}
(async () => {
  const cases = [];
  const deckish = []; for (let i = 0; i < 30; i++) deckish.push(term(200 + ((rnd() * 20000) | 0)));
  cases.push({ name: 'the deck\'s settings (windowBits 13, level 6, memLevel 7), one stream across 30 messages', messages: await stream(deckish, { windowBits: 13, level: 6, memLevel: 7 }) });
  const rep = []; const base = term(20000); for (let i = 0; i < 12; i++) rep.push(Buffer.concat([base.subarray((i * 1777) % 9000, (i * 1777) % 9000 + 9000), term(3000)]));
  cases.push({ name: 'windowBits 15, level 9: back-references up to 32 KB into earlier messages', messages: await stream(rep, { windowBits: 15, level: 9 }) });
  cases.push({ name: 'a 3 MB message (the window wraps ~100 times inside one message)', messages: await stream([term(3 << 20)], { windowBits: 15, level: 6 }) });
  cases.push({ name: 'stored blocks (level 0)', messages: await stream([term(70000), term(10), term(100000)], { windowBits: 15, level: 0 }) });
  cases.push({ name: 'fixed Huffman codes only (Z_FIXED)', messages: await stream([term(5000), term(40000), term(3)], { windowBits: 15, strategy: C.Z_FIXED }) });
  cases.push({ name: 'incompressible bytes between compressible ones', messages: await stream([term(3000), noise(50000), term(3000), noise(70000)], { windowBits: 13 }) });
  cases.push({ name: 'Huffman-only streams (Z_HUFFMAN_ONLY)', messages: await stream([term(9000), term(9000)], { strategy: C.Z_HUFFMAN_ONLY }) });
  cases.push({ name: 'RLE streams', messages: await stream([Buffer.alloc(50000, 0x20), term(800), Buffer.alloc(3000, 0x41)], { strategy: C.Z_RLE }) });
  cases.push({ name: 'empty messages between others (a flush with nothing new: the payload is empty)', messages: await stream([term(500), Buffer.alloc(0), term(500), Buffer.alloc(0)], { windowBits: 13 }) });
  // no context takeover: a fresh stream per message, so the inflater must start fresh too
  { const ms = []; for (let i = 0; i < 6; i++) { const p = term(4000 + i * 1000); ms.push(msg(p, strip(zlib.deflateRawSync(p, { windowBits: 13, finishFlush: C.Z_SYNC_FLUSH })))); }
    cases.push({ name: 'server_no_context_takeover: a fresh stream per message', noContext: true, messages: ms }); }
  // a final block (BFINAL) ends a stream; the next message is a new stream on the same window (here: none needed)
  { const a = term(6000), b = term(7000), c = term(500);
    cases.push({ name: 'a message ending in a final block, then new streams', messages: [msg(a, zlib.deflateRawSync(a)), msg(b, strip(zlib.deflateRawSync(b, { finishFlush: C.Z_SYNC_FLUSH }))), msg(c, zlib.deflateRawSync(c))] }); }
  // fuzz: 300 messages of random size and mix, random flush settings per case
  for (let k = 0; k < 4; k++) {
    const ms = []; for (let i = 0; i < 75; i++) { const n = (rnd() * (i % 10 === 0 ? 80000 : 3000)) | 0; ms.push(rnd() < 0.15 ? noise(n) : term(n)); }
    cases.push({ name: `fuzz ${k + 1}: 75 messages, random sizes, windowBits ${9 + k * 2}, level ${[1, 4, 6, 9][k]}`, messages: await stream(ms, { windowBits: 9 + k * 2, level: [1, 4, 6, 9][k], memLevel: 1 + k * 2 }) });
  }
  // what must fail
  cases.push({ name: 'a stored block whose LEN and NLEN disagree fails', messages: [{ z: Buffer.from([1, 2, 3, 4, 5]).toString('base64'), bad: true }] });
  cases.push({ name: 'a reserved block type (BTYPE 11) fails', messages: [{ z: Buffer.from([0x07, 0x00]).toString('base64'), bad: true }] });
  { const p = term(5000); const z = strip(zlib.deflateRawSync(p, { finishFlush: C.Z_SYNC_FLUSH })); const cut = Buffer.from(z); cut[(z.length / 2) | 0] ^= 0xff; cut[((z.length / 2) | 0) + 1] ^= 0x55;
    cases.push({ name: 'a corrupted stream either fails or does not come out the same', messages: [{ ...msg(p, cut), maybe: true }] }); }
  fs.writeFileSync(out, JSON.stringify({ cases }));
  console.log(`${cases.length} cases, ${cases.reduce((n, c) => n + c.messages.length, 0)} messages -> ${out}`);
})();
