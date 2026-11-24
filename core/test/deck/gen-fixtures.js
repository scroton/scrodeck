'use strict';
// Generate the acceptance fixtures for core/deck from the CANONICAL JavaScript: lib/phone.js, lib/queue.js,
// lib/startupprompts.js, lib/usage.js, lib/platform.js and lib/triggers.js are required as they are; the pieces
// of main.js that are not a module (TERMINAL_REPLY, USAGE_NOTICE, the trust prompt's patterns) are lifted out of
// its source text — so the expected values are what the Electron deck's own code produces. If main.js is
// refactored so a piece is no longer found, this script throws rather than guess.
//
//   node core/test/deck/gen-fixtures.js                 # everything but the HTTP routes
//   node core/test/deck/gen-fixtures.js --routes        # ALSO the routes: runs the real lib/phone.js PhoneServer on
//                                                       # 127.0.0.1:8812 (from PowerShell: loopback is blocked in the
//                                                       # Bash tool) with fake deps, and records what it answers
//
// Everything fed in is SYNTHETIC. No real config dir, credential or transcript is read; the usage poller runs
// against a fake fetch and fake credentials. Output: core/test/fixtures/deck/*.json.
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');

const ROOT = path.join(__dirname, '..', '..', '..');
const FX = path.join(__dirname, '..', 'fixtures', 'deck');
fs.mkdirSync(FX, { recursive: true });
const MAIN = fs.readFileSync(path.join(ROOT, 'main.js'), 'utf8').replace(/\r\n/g, '\n');
const hex = s => Buffer.from(s, 'utf8').toString('hex');

function write(name, data) {
  const f = path.join(FX, name + '.json');
  fs.writeFileSync(f, JSON.stringify(data, null, 1) + '\n');
  console.log(`wrote ${name} -> ${path.relative(process.cwd(), f)} (${fs.statSync(f).size} bytes)`);
}
function lift(re, what) { const m = re.exec(MAIN); if (!m) throw new Error(`main.js: ${what} not found`); return m[1]; }

const phoneLib = require(path.join(ROOT, 'lib', 'phone.js'));
const { PhoneServer, clipMessage, remoteMeta } = phoneLib;

// ---------------------------------------------------------------- remote.json: what a remote client is sent
{
  const texts = [
    '', 'one line', 'one\ntwo\nthree\nfour\nfive', '\n\n  \nfirst\n\n\nsecond\n   \nthird\nfourth', 'x'.repeat(900), 'a\n' + 'y'.repeat(700) + '\nb\nc',
    'caf\u00e9 \u2014 na\u00efve\n\ud83d\ude00 emoji line\nthird \u00a0\n fourth', '\ud83d\ude00'.repeat(450), 'only\n', '\n', 'tab\there\n\u00a0\u2003\nreal\nlast',
    'l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11\nl12',
  ];
  const clip = [];
  for (const text of texts) for (const lines of [1, 2, 3, 10]) clip.push({ text, lines, out: clipMessage(text, lines) });
  const tile = (id, more) => ({ id, kind: 'claude', lastText: null, ptySize: { cols: 120, rows: 30 }, mirror: null, status: 'idle', ...more });
  const metas = [
    { now: 1, settings: { msgLines: { normal: 3, wide: 10 }, driver: 'auto' }, tiles: [tile('a', { lastText: texts[2] }), tile('b', { lastText: texts[4] }), tile('c', { ptySize: null }), tile('d', { lastText: texts[11], mirror: { cols: 40, rows: 12 } })] },
    { now: 2, settings: { msgLines: { normal: 1, wide: 2 }, driver: 'desk' }, tiles: [tile('a', { lastText: texts[3] }), tile('b', { lastText: texts[6] })] },
    { now: 3, settings: { driver: 'phone' }, tiles: [tile('a', { lastText: texts[11] }), null, tile('b')] },
    { now: 4, settings: { msgLines: { normal: 0, wide: '7' } }, tiles: [tile('a', { lastText: texts[11] })] },
    { now: 5, tiles: 'not a list' },
    null,
  ];
  const remote = [];
  for (const meta of metas) for (const desk of [false, true]) remote.push({ meta, desk, out: remoteMeta(meta, desk) });
  const deskCases = [];
  for (const meta of [metas[0], metas[1], metas[3]]) {      // (deskMeta is only ever handed real tiles)
    for (const [shown, hidden, phoneHas] of [[['a'], false, []], [['a', 'b', 'c', 'd'], false, []], [['a', 'b'], true, []], [['a', 'b'], false, ['a']], [[], false, ['b']], [['c', 'zz'], false, ['c']]]) {
      const ws = { hidden, shown: new Set(shown) };
      const self = { phoneWatching: id => phoneHas.includes(id) };
      deskCases.push({ meta, shown, hidden, phoneHas, out: PhoneServer.prototype.deskMeta.call(self, ws, meta) });
    }
  }
  write('remote', { clip, remote, desk: deskCases });
}

// ---------------------------------------------------------------- wire.json: digests, the handshake, frames, the page
{
  const inputs = ['', 'abc', 'The quick brown fox jumps over the lazy dog', 'a'.repeat(55), 'a'.repeat(56), 'a'.repeat(63), 'a'.repeat(64), 'a'.repeat(65), 'a'.repeat(1000),
    Buffer.from([0, 255, 128, 1, 2, 3]).toString('latin1'), '\u00e9\u2014\ud83d\ude00'];
  const digests = inputs.map(s => { const b = Buffer.from(s, s.charCodeAt(0) === 0 ? 'latin1' : 'utf8');
    return { hex: b.toString('hex'), sha1: crypto.createHash('sha1').update(b).digest('hex'), sha256: crypto.createHash('sha256').update(b).digest('hex'), base64: b.toString('base64') }; });
  const hmacs = [['', ''], ['key', 'The quick brown fox jumps over the lazy dog'], ['k'.repeat(64), 'm'], ['k'.repeat(65), 'm'.repeat(200)], ['0123456789abcdef', 'deck pages']]
    .map(([k, m]) => ({ key: k, msg: m, hex: crypto.createHmac('sha256', k).update(m).digest('hex') }));
  const keys = ['dGhlIHNhbXBsZSBub25jZQ==', crypto.randomBytes(16).toString('base64'), crypto.randomBytes(16).toString('base64')];
  const accepts = keys.map(k => ({ key: k, accept: crypto.createHash('sha1').update(k + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64') }));
  const pageKeys = ['0123456789abcdef', 'tok', ''].map(token => { const s = Object.create(PhoneServer.prototype); s.token = token; return { token, key: s.pageKey() }; });

  // frames, as the `ws` package's own Sender writes them (a client masks; a server does not)
  const Sender = require(path.join(ROOT, 'node_modules', 'ws', 'lib', 'sender.js'));
  const frames = [];
  const mk = (payload, opcode, fin, mask, maskBytes) => {
    const opts = { fin, opcode, mask, readOnly: true, rsv1: false };
    if (mask) { opts.maskBuffer = Buffer.alloc(4); opts.generateMask = b => { Buffer.from(maskBytes).copy(b); }; }
    return Buffer.concat(Sender.frame(Buffer.from(payload), opts)).toString('hex');
  };
  let seed = 7;
  const bytes = n => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; b[i] = seed & 0xff; } return b; };
  const sha = h => crypto.createHash('sha256').update(Buffer.from(h, 'hex')).digest('hex');
  for (const n of [0, 1, 5, 125, 126, 127, 65535, 65536, 70000]) {
    // (the big payloads are a repeated pattern, so the fixture carries their digest, not 70 KB of hex four times)
    const payload = Buffer.from('0123456789abcdef'.repeat(Math.ceil(n / 16)).slice(0, n));
    for (const op of [1, 2]) {
      const maskBytes = [0x12 + n % 7, 0x34, 0x56, 0x78];
      const unmasked = mk(payload, op, true, false), masked = mk(payload, op, true, true, maskBytes);
      if (n <= 200) frames.push({ n, op, unmasked, masked, maskKey: maskBytes });
      else frames.push({ n, op, unmaskedSha: sha(unmasked), maskedSha: sha(masked), unmaskedHead: unmasked.slice(0, 40), maskedHead: masked.slice(0, 40), maskKey: maskBytes });
    }
  }
  const control = [
    { op: 9, payload: hex('hi'), unmasked: mk(Buffer.from('hi'), 9, true, false), masked: mk(Buffer.from('hi'), 9, true, true, [1, 2, 3, 4]), maskKey: [1, 2, 3, 4] },
    { op: 10, payload: '', unmasked: mk(Buffer.alloc(0), 10, true, false), masked: mk(Buffer.alloc(0), 10, true, true, [9, 9, 9, 9]), maskKey: [9, 9, 9, 9] },
    { op: 8, payload: '03e8', unmasked: mk(Buffer.from([3, 232]), 8, true, false), masked: mk(Buffer.from([3, 232]), 8, true, true, [0, 0, 0, 1]), maskKey: [0, 0, 0, 1] },
  ];
  // one message in three fragments, with a ping between two of them
  const frag = { text: 'frag-one|frag-two|frag-three\u00e9', wire: mk(Buffer.from('frag-one|'), 1, false, true, [5, 6, 7, 8]) + mk(Buffer.from('p'), 9, true, true, [1, 1, 1, 1])
    + mk(Buffer.from('frag-two|'), 0, false, true, [9, 8, 7, 6]) + mk(Buffer.from('frag-three\u00e9'), 0, true, true, [4, 4, 4, 4]) };

  // the desk page: renderer/index.html as lib/phone.js serves it, and which files it may load
  const server = new PhoneServer({});
  const indexHtml = fs.readFileSync(path.join(ROOT, 'renderer', 'index.html'), 'utf8');
  const variants = [indexHtml, indexHtml.replace("frame-src 'self' file:", "frame-src 'self' file:; connect-src 'self'"), '<html><head><title>x</title></head></html>', 'no title at all'];
  const pages = variants.map(html => {
    const s = Object.create(PhoneServer.prototype); s.repo = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-fx-page-'));
    fs.mkdirSync(path.join(s.repo, 'renderer')); fs.writeFileSync(path.join(s.repo, 'renderer', 'index.html'), html);
    const out = s.deskPage(); fs.rmSync(s.repo, { recursive: true, force: true });
    return { html, out };
  });
  const rels = ['renderer/style.css', 'renderer/desk/bridge.js', 'lib/layout.js', 'lib/phone.js', 'main.js', 'renderer/js/10-state.js', 'renderer/js/a.b-c_d.js', 'renderer/js/', 'renderer/js/.js',
    'renderer/js/x/y.js', 'renderer/js/x.jsx', 'renderer/js/x y.js', 'renderer/phone/app.js', 'node_modules/@xterm/xterm/lib/xterm.js', 'node_modules/@xterm/xterm/css/xterm.css',
    'node_modules/@xterm/addon-fit/lib/addon-fit.js', 'node_modules/ws/index.js', 'assets/fonts/fonts.css', 'assets/fonts/CascadiaMono/CascadiaMono[wght].ttf', 'assets/fonts/a/b/c.ttf',
    'assets/fonts/x.woff2', 'assets/fonts/x.woff', 'assets/fonts/.css', 'assets/fonts//x.css', 'assets/fonts/d.ir/Font,Bold.woff2', 'assets/icon.png', '../main.js', 'renderer/../main.js', ''];
  const deskFiles = rels.map(rel => ({ rel, ok: !!server.deskFile(rel), norm: path.posix.normalize(rel) }));

  // who is let in (lib/phone.js authed)
  const token = '0123456789abcdef';
  const authCases = [
    ['/?t=0123456789abcdef', ''], ['/?t=0123456789abcdeF', ''], ['/?t=', ''], ['/', ''], ['/?x=1&t=0123456789abcdef', ''], ['/?t=wrong&t=0123456789abcdef', ''], ['/desk/?t=0123456789abcdef#frag', ''],
    ['/', 'deck_phone=0123456789abcdef'], ['/', 'a=b; deck_phone=0123456789abcdef; c=d'], ['/', 'deck_phone=0123456789abcde'], ['/', 'deck_phoneX=0123456789abcdef'], ['/', ' deck_phone=0123456789abcdef '],
    ['/', 'deck_phone=wrong; deck_phone=0123456789abcdef'], ['/', 'deck_phone=%30123456789abcdef'], ['/?t=0123456789abcdef', 'deck_phone=wrong'], ['/x?t=%30123456789abcdef', ''], ['/x?t=0123456789abcdef+', ''],
  ].map(([url, cookie]) => { const s = Object.create(PhoneServer.prototype); s.token = token; return { url, cookie, how: s.authed({ url, headers: cookie ? { cookie } : {} }) }; });

  // which address: tailscaleAddress() over fake interface lists
  const nets = [
    { Ethernet: [{ family: 'IPv4', internal: false, address: '192.168.1.20' }], Tailscale: [{ family: 'IPv6', internal: false, address: 'fd7a::1' }, { family: 'IPv4', internal: false, address: '100.101.102.103' }] },
    { Ethernet: [{ family: 'IPv4', internal: false, address: '100.63.1.1' }, { family: 'IPv4', internal: false, address: '100.128.0.1' }, { family: 'IPv4', internal: false, address: '10.100.64.1' }] },
    { lo: [{ family: 'IPv4', internal: true, address: '100.64.0.1' }], a: [{ family: 'IPv4', internal: false, address: '100.64.0.2' }, { family: 'IPv4', internal: false, address: '100.127.255.254' }] },
    {},
  ];
  const realNets = os.networkInterfaces;
  const addresses = nets.map(n => { os.networkInterfaces = () => n; const address = phoneLib.tailscaleAddress();
    const ipv4 = []; for (const list of Object.values(n)) for (const ni of list) if (ni.family === 'IPv4' && !ni.internal) ipv4.push(ni.address);
    return { ipv4, address }; });
  os.networkInterfaces = realNets;

  // the Serve link: tailscaleServeUrl() over fake `tailscale serve status --json` outputs (the exec is replaced)
  const cp = require('child_process');
  const realExec = cp.execFileSync, realNow = Date.now;
  let clock = 1_800_000_000_000;
  Date.now = () => (clock += 61_000);            // past the one-minute cache on every call
  const statuses = [
    JSON.stringify({ TCP: { 443: { HTTPS: true } }, Web: { 'box.tail1234.ts.net:443': { Handlers: { '/': { Proxy: 'http://localhost:8788' } } } } }),
    JSON.stringify({ Web: { 'box.tail1234.ts.net:8443': { Handlers: { '/deck': { Proxy: 'http://127.0.0.1:8788/' } } } } }),
    JSON.stringify({ Web: { 'box.tail1234.ts.net:443': { Handlers: { '/': { Proxy: 'http://localhost:3000' }, '/d/': { Proxy: 'https://localhost:8788' } } }, 'other.ts.net': { Handlers: { '/x': { Proxy: 'http://localhost:8788' } } } } }),
    JSON.stringify({ Web: { 'box.ts.net:443': { Handlers: { '/': { Proxy: 'http://localhost:87880' } } } } }),
    JSON.stringify({ Web: { 'box.ts.net:443': { Handlers: { '/': { Path: 'C:\\x' } } } } }),
    JSON.stringify({}), 'not json', '',
  ];
  const serve = [];
  for (const text of statuses) for (const port of [8788, 3000]) {
    cp.execFileSync = () => { if (!text) throw new Error('no tailscale'); return text; };
    serve.push({ status: text, port, url: phoneLib.tailscaleServeUrl(port) });
  }
  cp.execFileSync = realExec; Date.now = realNow;

  write('wire', { digests, hmacs, accepts, pageKeys, frames, control, frag, pages, deskFiles, token, authCases, addresses, serve,
    cookie: `deck_phone=${encodeURIComponent(token)}; Path=/; Max-Age=31536000; SameSite=Lax; HttpOnly` });
}

// ---------------------------------------------------------------- startup.json: Claude Code's dialogs before a session is up
{
  const SP = require(path.join(ROOT, 'lib', 'startupprompts.js'));
  const raws = fs.readdirSync(path.join(ROOT, 'test', 'fixtures')).filter(n => /^startup-.*\.raw$/.test(n)).sort();
  if (raws.length < 3) throw new Error('test/fixtures/startup-*.raw: the recorded dialogs are missing');
  const exists = p => /settings\.json$/.test(p) && !/ /.test(p);     // a made-up rule, the same on both sides (the C++ test repeats it)
  const pieces = (s, n) => { const out = []; for (let i = 0; i < s.length; i += n) out.push(s.slice(i, i + n)); return out; };
  const streams = [];
  for (const name of raws) {
    const raw = fs.readFileSync(path.join(ROOT, 'test', 'fixtures', name), 'utf8');
    for (const size of [raw.length + 1, 4096, 97, 13]) {
      const chunks = pieces(raw, size);
      const w = new SP.StartupWatch(1000, { exists });
      let t = 1000;
      for (const c of chunks) w.feed(c, (t += 5));
      streams.push({ name, size, chunks, prompt: w.prompt, describe: SP.describeWarning(w.prompt), waitingSoon: w.waitingOn(t + 100), waitingLater: w.waitingOn(t + 4000),
        wants: w.wantsContinue({ enabled: true, lastKeyAt: 0 }), wantsOff: w.wantsContinue({ enabled: false, lastKeyAt: 0 }), wantsTyped: w.wantsContinue({ enabled: true, lastKeyAt: 2000 }),
        wantsOldKey: w.wantsContinue({ enabled: true, lastKeyAt: 999 }) });
    }
  }
  const texts = ['', 'plain \x1b[1mbold\x1b[0m', 'a\x1b[3Cb\x1b[Cc\x1b[0Cd\x1b[999Ce', 'x\x1b[5;10Hy\x1b[Hz\x1b[7fq', '\x1b]0;title\x07after\x1b]8;;http://x\x1b\\link', 'cut \x1b[?25l\x1b[2J\x1b=\x1b>end',
    '\x1b[38;2;1;2;3mrgb\x1b[m \x1b[ q \x1b[!p', 'half \x1b[12', 'osc open \x1b]0;never closed', '\u276f 1. Continue\u00a0x'];
  const screen = texts.map(t => ({ in: t, out: SP.screenText(t) }));
  const split = [...texts, 'done\x1b[1m', 'tail \x1b', 'tail \x1b[', 'tail \x1b]', 'tail \x1bM', 'tail \x1b\x1b', 'z'.repeat(5000) + '\x1b[', '\x1b[1' + 'z'.repeat(5000), 'two \x1b]0;a\x07 and \x1b]0;b']
    .map(t => { const [a, b] = SP.splitIncomplete(t); return { in: t, whole: a, rest: b }; });
  const dialogs = [
    'nothing here', 'Enter to confirm', 'Do you trust?\n\u276f Yes, I trust this folder\n  No, exit\nEnter to confirm', 'New MCP server found\nEnter to confirm',
    'Settings Warning\n C:\\Users\\x\\.claude\\settings.json\n  \u2514 Invalid permission rule "gh workflow\n run apply.yml"\n  \u2514 second   error\nThe values listed will be skipped\n\u276f 1. Continue\n  2. Fix with Claude\n  3. Exit and fix manually\nEnter to confirm',
    'Settings Warning\n C:\\Users\\x y\\.claude\\set\ntings.json\n  \u2514 broken thing\n  1. Continue\n\u276f 3. Exit and fix manually\nEnter to confirm',
    'Settings  Warning with no elbow\nExit and fix manually\nEnter  to  confirm', 'Settings Warning\n\u2514\u2514 x\nExit and fix manually Enter to confirm',
  ];
  const classify = dialogs.map(t => { const p = SP.classify(t, { exists }); return { in: t, out: p, describe: SP.describeWarning(p) }; });

  // the trust prompt's own patterns, as main.js spells them (spawnTile's trustWatch)
  const seenRe = new RegExp(lift(/!w\.phase && \/(Quick\\s\*safety\\s\*check\|Yes,\\s\*I\\s\*trust\\s\*this\\s\*folder)\/\.test\(w\.text\)/, 'the trust prompt pattern'));
  const optionRe = new RegExp(lift(/if \(!\/(Yes,\\s\*I\\s\*trust\\s\*this\\s\*folder)\/\.test\(w\.text\)\) \{ w\.done = true; return; \}/, 'the trust option pattern'));
  const markerSrc = lift(/w\.text\.matchAll\(\/(\\n\[ \\t\]\*\[\\u276f>\]\[ \\t\]\+\(No,\\s\*exit\|Yes,\\s\*I\\s\*trust\))\/g\)/, 'the trust marker pattern');
  const stripSrc = lift(/w\.text = \(w\.text \+ d\.replace\(\/(\\x1b\\\[\[0-9;\?\]\*\[A-Za-z\])\/g, ''\)\)\.slice\(-8000\)/, 'the trust text rule');
  const markerLine = text => { const found = [...text.matchAll(new RegExp(markerSrc, 'g'))]; return found.length ? (/^Yes/.test(found[found.length - 1][1]) ? 'yes' : 'no') : ''; };
  const trustTexts = ['', 'Quick safety check', 'Quick\n safety  check: is this a project you trust?', '\n \u276f Yes, I trust this folder\n   No, exit', '\n   Yes, I trust this folder\n > No, exit',
    '\n> No, exit\n\u276f Yes, I trust this folder', '\n\u276fYes, I trust', 'x\n\t\u276f\tNo,\n exit', '\u276f Yes, I trust this folder', 'Yes, I\u00a0trust this folder', 'Yes, I trust this  folderish'];
  const trustRaw = fs.readFileSync(path.join(ROOT, 'test', 'fixtures', 'startup-trust-prompt.raw'), 'utf8');
  const append = (text, d) => (text + d.replace(new RegExp(stripSrc, 'g'), '')).slice(-8000);
  let acc = ''; for (const c of pieces(trustRaw, 211)) acc = append(acc, c);
  trustTexts.push(acc);
  const trust = { cases: trustTexts.map(t => ({ text: t, seen: seenRe.test(t), option: optionRe.test(t), marker: markerLine(t) })),
    append: [['', 'a\x1b[1mb\x1b[0mc'], ['x'.repeat(7999), 'yz\x1b[2J!'], ['', '\x1b[?25lq\x1b[12;3Hr\x1b]0;t\x07'], ['\ud83d\ude00'.repeat(3999), 'ab']].map(([text, d]) => ({ text, d, out: append(text, d) })),
    raw: { chunks: pieces(trustRaw, 211), text: acc } };
  write('startup', { streams, screen, split, classify, trust });
}

// ---------------------------------------------------------------- held.json: special prompts (lib/queue.js)
{
  const Q = require(path.join(ROOT, 'lib', 'queue.js'));
  const visible = ['', '> ', '\n> hello', 'text\n \u276f\u00a0', '\u2500\u2500 \u276f ', 'x> y', '\x1b[2K\x1b[1G\u276f \x1b[0m', '\x1b]0;title\x07\u276f\t', 'no marker here\n>x', '  \t>\t', 'a\u276f b', '\x1b[1m>\x1b[0m ', 'z'.repeat(300000) + '\n> ']
    .map(t => ({ in: t, out: Q.promptVisible(t) }));
  const windows = []; for (const [m, s, e] of [[0, 0, 0], [10, 5, 20], [20, 5, 20], [4, 5, 20], [1430, 1380, 60], [30, 1380, 60], [700, 1380, 60], [60, 1380, 60]]) windows.push({ m, s, e, out: Q.inWindow(m, s, e) });

  // scripted runs: every step is an operation on the real class, with the clock, the ids and the tiles given
  const realNow = Date.now, realUuid = crypto.randomUUID;
  const scenario = (name, steps) => {
    let clock = 0, n = 0;
    Date.now = () => clock;
    crypto.randomUUID = () => `id-${++n}`;
    const file = path.join(os.tmpdir(), `deck-fx-held-${process.pid}-${name}.json`);
    try { fs.rmSync(file, { force: true }); } catch { /* none */ }
    const tiles = {}; const writes = []; const risen = {}; let changes = 0;
    const deps = { file, confirmMs: 0, tileById: id => tiles[id] || null, statusOf: t => t.status, recentOutput: t => t.recent || '', write: (t, d) => writes.push([t.id, d]),
      noteInput: t => { t.noted = (t.noted || 0) + 1; }, triggerRisenAt: nm => risen[nm] || 0, onChange: () => { changes++; } };
    let q = new Q.HeldPrompts(deps);
    const out = [];
    for (const s of steps) {
      const rec = { ...s };
      clock = s.now != null ? s.now : clock;
      try {
        if (s.op === 'tile') tiles[s.id] = s.tile ? { id: s.id, ...s.tile } : undefined;
        else if (s.op === 'add') rec.result = q.add(s.tileId, s.spec);
        else if (s.op === 'remove') rec.result = q.remove(s.qid);
        else if (s.op === 'forget') q.forgetTile(s.tileId);
        else if (s.op === 'tick') q.tick(clock);
        else if (s.op === 'risen') risen[s.name] = s.at;
        else if (s.op === 'reload') q = new Q.HeldPrompts(deps);
        else if (s.op === 'file') fs.writeFileSync(file, s.text);
      } catch (e) { rec.error = String(e.message || e); }
      rec.list = q.list(); rec.writes = writes.slice(); rec.changes = changes;
      rec.forTile = s.tileId ? q.forTile(s.tileId) : undefined;
      rec.saved = null; if (fs.existsSync(file)) { try { rec.saved = JSON.parse(fs.readFileSync(file, 'utf8')); } catch { rec.saved = 'unreadable'; } }
      out.push(rec);
    }
    try { fs.rmSync(file, { force: true }); } catch { /* none */ }
    return { name, steps: out };
  };
  const idle = { kind: 'claude', sessionId: 's1', pty: true, exited: false, status: 'idle', recent: '\n> ' };
  const H = 3600000;
  const scenarios = [
    scenario('validation', [
      { op: 'add', tileId: 'none', spec: { kind: 'next', text: 'x' } },
      { op: 'tile', id: 'sh', tile: { kind: 'shell', pty: true, status: 'shell' } }, { op: 'add', tileId: 'sh', spec: { kind: 'next', text: 'x' } },
      { op: 'tile', id: 't1', tile: idle },
      { op: 'add', tileId: 't1', spec: { kind: 'next', text: '   \r\n ' } }, { op: 'add', tileId: 't1', spec: { kind: 'next', text: 'x'.repeat(20001) } },
      { op: 'add', tileId: 't1', spec: { text: 'no time' } }, { op: 'add', tileId: 't1', spec: { kind: 'timed', text: 'no time', at: 0 } },
      { op: 'add', tileId: 't1', spec: { kind: 'recurring', text: 'r', everyMin: 0 } }, { op: 'add', tileId: 't1', spec: { kind: 'triggered', text: 't', trigger: '  ' } },
      { op: 'add', tileId: 't1', spec: { kind: 'weird', text: 't' } }, { op: 'add', tileId: 't1', spec: null },
      { op: 'add', now: 1000, tileId: 't1', spec: { kind: 'recurring', text: ' every \r\nday\r', everyMin: 15.4, startMin: -30, endMin: 1500 } },
      { op: 'add', now: 2000, tileId: 't1', spec: { kind: 'triggered', text: 'go', trigger: ' ci_green ' } },
      { op: 'add', now: 3000, tileId: 't1', spec: { kind: 'timed', text: 'later', at: '90000' } },
      { op: 'remove', qid: 'id-1' }, { op: 'remove', qid: 'nope' }, { op: 'forget', tileId: 't1' },
    ]),
    scenario('next-and-timed', [
      { op: 'tile', id: 't1', tile: { ...idle, status: 'busy' } },
      { op: 'add', now: 1000, tileId: 't1', spec: { kind: 'next', text: 'when you are done' } },
      { op: 'add', now: 1000, tileId: 't1', spec: { kind: 'timed', text: 'at five', at: 5000 } },
      { op: 'tick', now: 1500, tileId: 't1' },
      { op: 'tile', id: 't1', tile: { ...idle, status: 'waiting' } }, { op: 'tick', now: 2000 },
      { op: 'tile', id: 't1', tile: { ...idle, status: 'starting' } }, { op: 'tick', now: 2500 },
      { op: 'tile', id: 't1', tile: { ...idle, status: 'asleep' } }, { op: 'tick', now: 2600 },
      { op: 'tile', id: 't1', tile: { ...idle, status: 'odd' } }, { op: 'tick', now: 2700 },
      { op: 'tile', id: 't1', tile: { ...idle, recent: 'no prompt drawn' } }, { op: 'tick', now: 3000 },
      { op: 'tile', id: 't1', tile: { ...idle, pty: false } }, { op: 'tick', now: 3500 },
      { op: 'tile', id: 't1', tile: idle }, { op: 'tick', now: 4000, tileId: 't1' },
      { op: 'tick', now: 6000, tileId: 't1' }, { op: 'tick', now: 6500 },
      { op: 'reload' }, { op: 'tick', now: 6000 + 25 * H },
    ]),
    scenario('gives-up', [
      { op: 'tile', id: 't1', tile: { ...idle, status: 'busy' } },
      { op: 'add', now: 1000, tileId: 't1', spec: { kind: 'timed', text: 'never lands', at: 2000 } },
      { op: 'tick', now: 3000 }, { op: 'tick', now: 2000 + 6 * H + 1 },
      { op: 'add', now: 2000 + 6 * H + 2, tileId: 't1', spec: { kind: 'timed', text: 'not running', at: 10 } },
      { op: 'tile', id: 't1', tile: { ...idle, pty: false, exited: true } }, { op: 'tick', now: 2000 + 13 * H },
      { op: 'add', tileId: 't1', spec: { kind: 'next', text: 'orphan' } }, { op: 'tile', id: 't1', tile: null }, { op: 'tick', now: 2000 + 13 * H + 5 },
    ]),
    scenario('recurring-and-triggered', [
      { op: 'tile', id: 't1', tile: idle }, { op: 'tile', id: 't2', tile: { ...idle, sessionId: 's2' } },
      { op: 'add', now: 1000, tileId: 't1', spec: { kind: 'recurring', text: 'check in', everyMin: 10, startMin: 0, endMin: 0 } },
      { op: 'add', now: 1000, tileId: 't2', spec: { kind: 'triggered', text: 'the build is green', trigger: 'ci_green' } },
      { op: 'tick', now: 2000 }, { op: 'tick', now: 3000 }, { op: 'tick', now: 2000 + 10 * 60000 },
      { op: 'risen', name: 'ci_green', at: 700000 }, { op: 'tick', now: 700100 }, { op: 'tick', now: 700200 },
      { op: 'risen', name: 'ci_green', at: 0 }, { op: 'tick', now: 700300 }, { op: 'risen', name: 'ci_green', at: 800000 }, { op: 'tick', now: 800100, tileId: 't2' },
      { op: 'add', now: 800200, tileId: 't1', spec: { kind: 'next', text: 'second for the same tile' } }, { op: 'tick', now: 2000 + 20 * 60000 + 800000 },
    ]),
    scenario('old-file', [
      { op: 'file', text: JSON.stringify({ items: [{ id: 'old-1', tileId: 't1', text: 'from before kinds', at: 500, state: 'sending', tries: 1, createdAt: 100 }, { id: '', tileId: 't1', text: 'no id' }, { id: 'x', tileId: 't1', text: 7 }, null] }) },
      { op: 'reload' }, { op: 'tile', id: 't1', tile: idle }, { op: 'tick', now: 600 },
      { op: 'file', text: 'not json' }, { op: 'reload' },
    ]),
  ];
  Date.now = realNow; crypto.randomUUID = realUuid;
  write('held', { visible, windows, scenarios });
}

// ---------------------------------------------------------------- usage.json: lib/usage.js against a fake fetch and fake credentials
{
  const platform = require(path.join(ROOT, 'lib', 'platform.js'));
  const creds = {}, stamps = {};
  const real = { readOAuth: platform.readOAuth, credentialsStamp: platform.credentialsStamp, fetch: global.fetch, now: Date.now };
  platform.readOAuth = dir => (dir in creds ? creds[dir] : null);
  platform.credentialsStamp = dir => (dir in stamps ? stamps[dir] : null);
  delete require.cache[require.resolve(path.join(ROOT, 'lib', 'usage.js'))];
  const { UsagePoller } = require(path.join(ROOT, 'lib', 'usage.js'));
  let clock = 1_000_000;
  Date.now = () => clock;
  const calls = [];
  let responses = {};
  global.fetch = async (url, opts) => {
    const auth = opts.headers.Authorization;
    calls.push({ url, auth: auth.replace(/^Bearer .*/, 'Bearer <token>'), beta: opts.headers['anthropic-beta'], type: opts.headers['Content-Type'] });
    const r = responses[auth.replace('Bearer ', '')];
    if (!r) throw new Error('fetch failed');
    return { status: r.status, ok: r.status >= 200 && r.status < 300, headers: { get: k => (r.headers || {})[k.toLowerCase()] ?? null }, json: async () => { if (r.text !== undefined) return JSON.parse(r.text); return r.json; } };
  };
  const cacheFile = path.join(os.tmpdir(), `deck-fx-usage-${process.pid}.json`);
  try { fs.rmSync(cacheFile, { force: true }); } catch { /* none */ }
  const accounts = [{ id: 'a1', dir: 'D:/fake/a1' }, { id: 'a2', dir: 'D:/fake/a2' }, { id: 'a3', dir: 'D:/fake/a3' }];
  const okBody = { five_hour: { utilization: 12.5, resets_at: '2026-11-18T10:00:00Z' }, seven_day: { utilization: 40, resets_at: '2026-11-22T00:00:00Z' },
    limits: [{ kind: 'weekly_scoped', scope: { model: { display_name: 'Fable', id: 'fable' } }, percent: 7, resets_at: '2026-11-22T00:00:00Z' }, { kind: 'weekly_scoped', scope: { model: { id: 'opus' } }, percent: 'x' }, { kind: 'other' }, null] };
  const steps = [];
  let poller = new UsagePoller(accounts, cacheFile);
  const step = async (label, set, force) => {
    if (set.clock != null) clock = set.clock;
    Object.assign(creds, set.creds || {}); Object.assign(stamps, set.stamps || {});
    for (const k of set.dropStamps || []) delete stamps[k];
    if (set.responses) responses = set.responses;
    if (set.reload) poller = new UsagePoller(accounts, cacheFile);
    const before = calls.length;
    await poller.pollAll(!!force);
    steps.push({ label, clock, force: !!force, reload: !!set.reload, creds: JSON.parse(JSON.stringify(creds)), stamps: { ...stamps }, responses: JSON.parse(JSON.stringify(responses)),
      requests: calls.length - before, calls: calls.slice(before), data: JSON.parse(JSON.stringify(poller.data)),
      cache: fs.existsSync(cacheFile) ? JSON.parse(fs.readFileSync(cacheFile, 'utf8')) : null });
  };
  const tok = (t, more) => ({ claudeAiOauth: { accessToken: t, subscriptionType: 'max', expiresAt: 9_000_000_000_000, ...more } });
  (async () => {
    await step('no credentials anywhere', {});
    await step('first poll: ok / 401 / 429 with retry-after', { clock: 1_010_000, creds: { 'D:/fake/a1': tok('T1'), 'D:/fake/a2': tok('T2', { subscriptionType: undefined }), 'D:/fake/a3': tok('T3') },
      stamps: { 'D:/fake/a1': 's1', 'D:/fake/a2': 's2', 'D:/fake/a3': 's3' },
      responses: { T1: { status: 200, json: okBody }, T2: { status: 401 }, T3: { status: 429, headers: { 'retry-after': '120' } } } });
    await step('ten seconds later: nothing is asked again', { clock: 1_020_000 });
    await step('the credentials changed for a2: asked at once; a3 is still backing off', { clock: 1_030_000, stamps: { 'D:/fake/a2': 's2b' }, responses: { T1: { status: 200, json: okBody }, T2: { status: 500 }, T3: { status: 200, json: okBody } } });
    await step('forced: a1 again, a3 still backing off', { clock: 1_040_000 }, true);
    await step('after the retry-after: a3 is asked; 429 with no header backs off ten minutes', { clock: 1_010_000 + 121_000, responses: { T1: { status: 200, json: okBody }, T2: { status: 200, json: {} }, T3: { status: 429 } } });
    await step('three minutes on: a1 and a2 refresh, a3 waits', { clock: 1_010_000 + 121_000 + 181_000 });
    await step('an expired token, a missing token, a network error', { clock: 2_500_000, creds: { 'D:/fake/a1': tok('T1', { expiresAt: 5 }), 'D:/fake/a2': { claudeAiOauth: {} }, 'D:/fake/a3': tok('T9') },
      stamps: { 'D:/fake/a1': 'x1', 'D:/fake/a2': 'x2', 'D:/fake/a3': 'x3' }, responses: {} });
    await step('a body that is not JSON', { clock: 3_500_000, creds: { 'D:/fake/a3': tok('T3') }, stamps: { 'D:/fake/a3': 'x4' }, responses: { T3: { status: 200, text: '<html>' } } });
    await step('a new poller reads the cache', { clock: 3_600_000, reload: true, dropStamps: ['D:/fake/a1', 'D:/fake/a2', 'D:/fake/a3'] });
    Object.assign(platform, { readOAuth: real.readOAuth, credentialsStamp: real.credentialsStamp }); global.fetch = real.fetch; Date.now = real.now;
    try { fs.rmSync(cacheFile, { force: true }); } catch { /* none */ }
    write('usage', { accounts, steps });
    more();
  })().catch(e => { console.error(e); process.exit(1); });
}

function more() {
  // ---------------------------------------------------------------- rules.json: main.js patterns and lib/triggers.js, lib/platform.js
  const terminalReply = new RegExp(lift(/const TERMINAL_REPLY = \/(.+)\/;\n/, 'TERMINAL_REPLY'));
  const usageNotice = new RegExp(lift(/const USAGE_NOTICE = \/(.+)\/i;\n/, 'USAGE_NOTICE'), 'i');
  const replyCases = ['', 'a', '\x1b[I', '\x1b[O', '\x1b[I\x1b[O', '\x1b[12;40R', '\x1b[?1;2c', '\x1b[>0;276;0c', '\x1b[0n', '\x1b]11;rgb:0c0c/0c0c/0c0c\x07', '\x1b]10;x\x1b\\', '\x1b[<64;40;12M', '\x1b[<0;1;1m',
    '\x1b[Mabc', '\x1b[M\u00e9\u00e9\u00e9', '\x1b[Mab', '\x1b[<;M', '\x1b[A', '\x1b[I ', 'x\x1b[I', '\x1b', '\x1b[', '\x1b]never', '\x1b[<64;40;12M4;40;12M', '\r', '\x1b[200~paste\x1b[201~', '\x1b[R', '\x1b[?c\x1b[I\x1b[<1;2;3M'];
  const noticeCases = ['', 'all good', 'You hit your spend limit', 'Usage limit reached', 'RATE LIMIT exceeded', 'your limit resets at 5pm', 'see /usage-credits', 'You hit your weekly limit', "hit your org's monthly cap",
    'hit your orgs session', 'hit your daily', 'out of usage', 'out of credits', 'out of ideas', 'usage will reset soon', 'usage\nreset', 'reset of usage', 'the usage figures\r\nreset tomorrow'];
  const { cleanName } = require(path.join(ROOT, 'lib', 'triggers.js'));
  const names = ['ci_green', ' padded ', 'a.b-c_d', '', 'has space', 'x'.repeat(64), 'x'.repeat(65), 'slash/no', 'dot.', '\u00e9', null, 7, 'tab\tno'];
  const platform = require(path.join(ROOT, 'lib', 'platform.js'));
  const keys = [];
  for (const key of ['F5', 'F6', 'F7', 'F9', 'F10', 'F11', 'F12', 'p', 'P', 'i', '`', 'a', 'Enter']) for (const mods of [{}, { control: true }, { control: true, shift: true }, { control: true, shift: true, alt: true }, { alt: true }, { meta: true }]) {
    const input = { key, code: key === '`' ? 'Backquote' : 'Key' + key.toUpperCase(), ...mods };
    keys.push({ input, action: platform.keyAction(input) });
  }
  write('rules', { terminalReply: replyCases.map(s => ({ in: s, out: terminalReply.test(s) })), usageNotice: noticeCases.map(s => ({ in: s, out: usageNotice.test(s) })),
    triggerNames: names.map(n => ({ in: n, out: cleanName(n) })), os: process.platform, keys, keyHelp: platform.keyHelp, fontStack: platform.defaultFontStack });

  // ---------------------------------------------------------------- transcript-page.json: the phone's text view (lib/transcript-page.js)
  {
    const TP = require(path.join(ROOT, 'lib', 'transcript-page.js'));
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-fx-tp-'));
    const ts = n => `2027-01-15T08:${String(n).padStart(2, '0')}:00.000Z`;
    const user = (content, extra = {}) => ({ type: 'user', timestamp: ts(extra.n || 1), message: { role: 'user', content }, ...extra });
    const asst = (content, extra = {}) => ({ type: 'assistant', timestamp: ts(extra.n || 2), effort: 'high', message: { role: 'assistant', model: 'claude-fable-5-1', content }, ...extra });
    const use = (id, name, input) => ({ type: 'tool_use', id, name, input });
    const result = (id, content) => ({ type: 'tool_result', tool_use_id: id, content });
    const docs = [
      [user('hello there'), asst([{ type: 'text', text: 'Hi.\r\nSecond line.' }]), user([{ type: 'text', text: 'and a block' }, { type: 'text', text: 'second block' }])],
      [user('do some work'),
        asst([{ type: 'text', text: 'On it.' }, use('t1', 'Bash', { command: 'ls -la\nsecond line', description: 'list' }), use('t2', 'mcp__slack__slack_search_users', { query: 'who' })]),
        user([result('t1', 'file-a\nfile-b'), result('t2', [{ type: 'text', text: 'one' }, { type: 'image' }, { type: 'text', text: 'two' }])]),
        asst([use('t3', 'Read', { file_path: 'C:\\x\\y.txt' }), use('t4', 'Grep', { pattern: 'needle', path: 'src' }), use('t5', 'Bash', { command: 'x'.repeat(5000) })]),
        user([result('t3', 'r'.repeat(4500)), { type: 'text', text: 'mixed text with a result' }]),
        asst([use('t6', 'Write', { file_path: 'a.txt', content: 'written body' }), use('t7', 'Edit', { file_path: 'a.txt', old_string: 'old', new_string: 'new' }), use('t8', 'Task', { prompt: 'sub task' }),
          use('t9', 'NotebookEdit', { new_source: 'cell' }), use('t10', 'WebFetch', { url: 'https://example.com' }), use('t11', 'Odd', { n: 5, later: '  ', text: ' first string value ' }), use(null, null, 'not an object'), use('t12', 'Empty', {})]),
        asst([{ type: 'text', text: 'All done.' }], { n: 9, effort: '', message: { role: 'assistant', model: '', content: [{ type: 'text', text: 'All done.' }] } })],
      [user('<system-reminder>be careful</system-reminder>'), user('<bash-stdout>out</bash-stdout><bash-stderr>err</bash-stderr>'), user('a compact summary', { isCompactSummary: true }), user('visible only', { isVisibleInTranscriptOnly: true }),
        user('<command-name> /clear </command-name><command-message>clear</command-message><command-args></command-args>'), user('<command-name></command-name> nothing named'), user('<local-command-stdout>done</local-command-stdout>'),
        user('[Request interrupted by user]'), user('Caveat: The messages below were generated'), user('meta context', { isMeta: true }),
        user('Base directory for this skill: C:\\Users\\x\\.claude\\skills\\ask-page\n\n# Ask page'), user('text\nBase directory for this skill:\n\n  /home/x/skills/deck-triggers  \nmore'), user('with a result object', { toolUseResult: { ok: true } }),
        user('<pasted_content id="1">\npasted text\nline two\n</pasted_content id="1">'), user('before <bash-input> ls -la </bash-input> after'), user('\n\nleading and trailing newlines\n\n'), user('<pasted_contents>not the tag</pasted_contents>'),
        user('<artifact-content-authored-by-others/>\n\n\n\nkept <system-reminder>x</system-reminder>'), user('   '), user([]), user(null),
        { type: 'user', isSidechain: true, message: { content: 'a subagent' } }, { type: 'summary', summary: 'x' }, { type: 'assistant', message: { content: [{ type: 'thinking', thinking: 'hm' }] } }, { type: 'user' }],
      [],
    ];
    const raws = docs.map(d => d.map(r => JSON.stringify(r)).join('\n') + (d.length ? '\n' : ''));
    raws.push('not json\n{"type":"user","message":{"content":"after junk"}}\n\n7\n"x"\n[1]\n{"type":"assistant","message":{"content":"a string body"}}');
    raws.push(Array.from({ length: 70 }, (_, i) => JSON.stringify(i % 2 ? asst([{ type: 'text', text: 'answer ' + i }]) : user('question ' + i))).join('\r\n'));
    const cases = raws.map((raw, i) => {
      const file = path.join(dir, `t${i}.jsonl`); fs.writeFileSync(file, raw);
      const pages = [{}, { limit: 5 }, { before: 3 }, { before: 40, limit: 10 }, { before: 0 }, { before: 9999, limit: 500 }, { limit: 0 }, { before: -4 }, { limit: null }].map(opts => ({ opts, out: TP.page(file, opts) }));
      return { raw, entries: TP.parse(file), pages };
    });
    fs.rmSync(dir, { recursive: true, force: true });
    write('transcript-page', { cases });
  }

  if (process.argv.includes('--routes')) routes().catch(e => { console.error(e); process.exit(1); });
}

// ---------------------------------------------------------------- routes.json: the real PhoneServer, asked over loopback
async function routes() {
  const http = require('http');
  const token = '0123456789abcdef', port = 8812;
  const uploads = [];
  const server = new PhoneServer({
    getInit: () => ({ home: 'C:\\fake\\home', meta: { now: 1, tiles: [] }, platform: { os: 'win32' } }),
    replayFor: () => null, call: () => null,
    saveUpload: (name, buf) => { uploads.push({ name, size: buf.length }); return `UPLOADED:${name}:${buf.length}`; },
  });
  await server.start({ port, token, bind: 'localhost' });
  const ask = (method, target, headers = {}, body = null) => new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, method, path: target, headers, agent: false }, res => {
      const chunks = []; res.on('data', c => chunks.push(c)); res.on('end', () => {
        const buf = Buffer.concat(chunks);
        resolve({ status: res.statusCode, type: res.headers['content-type'] || null, cache: res.headers['cache-control'] || null, cookie: (res.headers['set-cookie'] || [null])[0], csp: res.headers['content-security-policy'] || null,
          length: buf.length, sha256: crypto.createHash('sha256').update(buf).digest('hex'), text: buf.length <= 600 ? buf.toString('utf8') : null });
      });
    });
    req.on('error', reject);
    if (body) req.write(body);
    req.end();
  });
  const T = `?t=${token}`, C = { cookie: `deck_phone=${token}` };
  const list = [
    ['GET', '/'], ['GET', '/' + T], ['GET', '/', C], ['GET', '/index.html', C], ['HEAD', '/', C], ['GET', '/app.js', C], ['GET', '/style.css', C], ['GET', '/sw.js', C], ['GET', '/nope.js', C],
    ['GET', '/manifest.webmanifest'], ['GET', '/icon-192.png'], ['GET', '/icon-512.png'], ['GET', '/icon-512.png', C], ['GET', '/manifest.webmanifest' + T], ['GET', '/%6danifest.webmanifest'],
    ['GET', '/vendor/xterm.js', C], ['GET', '/vendor/xterm.css', C], ['GET', '/vendor/addon-fit.js', C], ['GET', '/vendor/nope.js', C], ['GET', '/vendor/xterm.js'],
    ['GET', '/desk/' + T], ['GET', '/desk', C], ['GET', '/desk/', C], ['GET', '/desk/index.html', C], ['GET', '/desk/renderer/', C], ['GET', '/desk/renderer/index.html', C], ['GET', '/desk/'],
    ['GET', '/desk/renderer/style.css', C], ['GET', '/desk/renderer/desk/bridge.js', C], ['GET', '/desk/lib/layout.js', C], ['GET', '/desk/lib/phone.js', C], ['GET', '/desk/main.js', C],
    ['GET', '/desk/renderer/js/10-state.js', C], ['GET', '/desk/renderer/js/nope.js', C], ['GET', '/desk/node_modules/@xterm/xterm/lib/xterm.js', C], ['GET', '/desk/node_modules/@xterm/xterm/css/xterm.css', C],
    ['GET', '/desk/node_modules/@xterm/addon-fit/lib/addon-fit.js', C], ['GET', '/desk/node_modules/ws/index.js', C], ['GET', '/desk/assets/fonts/fonts.css', C], ['GET', '/desk/assets/icon.png', C],
    ['GET', '/desk/renderer/js/../style.css', C], ['GET', '/desk/renderer/%2e%2e/main.js', C], ['GET', '/desk/x/..%2f..%2fmain.js', C], ['POST', '/desk/', C], ['HEAD', '/desk/', C],
    ['GET', '/../main.js', C], ['GET', '/..%2fmain.js', C], ['GET', '/..%5c..%5cmain.js', C], ['GET', '/%2e%2e/%2e%2e/main.js', C], ['GET', '/a/../app.js', C], ['GET', '/./app.js', C], ['GET', '//app.js', C],
    ['GET', '/ask/ask_abc-1.html', C], ['GET', '/ask/ask_abc-1.html'], ['POST', '/ask/ask_abc-1.html', C], ['GET', '/ask/nope.html', C], ['GET', '/ask/ask_a.b.html', C],
    ['GET', '/view/0123456789abcdef/0123456789abcdef/page.html'], ['GET', '/view/x'], ['POST', '/view/0123456789abcdef/0123456789abcdef/page.html'], ['GET', '/view/' + server.pageKey() + '/0123456789abcdef/page.html', C],
    ['POST', '/upload', { ...C, 'x-file-name': 'my%20notes%20(1).txt' }, 'hello upload'], ['POST', '/upload', C, 'no name'], ['POST', '/upload', {}, 'refused'], ['GET', '/upload', C],
    ['GET', '/?t=wrong'], ['GET', '/', { cookie: 'deck_phone=wrong' }], ['DELETE', '/', C], ['GET', '/app.js?x=1&t=' + token + '&y=2'],
  ];
  const out = [];
  for (const [method, target, headers, body] of list) out.push({ method, target, headers: headers || {}, body: body || null, res: await ask(method, target, headers || {}, body) });
  // the socket: refused without the token, accepted with it (the hello is the first frame)
  const WebSocket = require(path.join(ROOT, 'node_modules', 'ws'));
  const sock = (target, headers) => new Promise(resolve => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}${target}`, { headers });
    ws.on('unexpected-response', (_req, res) => { resolve({ status: res.statusCode }); res.destroy(); });
    ws.on('message', raw => { const m = JSON.parse(raw); resolve({ status: 101, first: m.t, keys: Object.keys(m).sort() }); ws.close(); });
    ws.on('error', e => resolve({ error: String(e.message) }));
  });
  const sockets = [];
  for (const [target, headers] of [['/', {}], ['/?t=wrong', {}], ['/' + T, {}], ['/any/path/at/all' + T, {}], ['/', C], ['/desk/', C]]) sockets.push({ target, headers, res: await sock(target, headers) });
  await server.stop();
  write('routes', { token, pageKey: server.pageKey(), requests: out, sockets, uploads });
  process.exit(0);
}
