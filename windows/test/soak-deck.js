'use strict';
// windows/test/soak-deck.js — a deck for the remote exe's MEMORY soak (windows/test/soak.ps1): the REAL lib/phone.js
// PhoneServer (its desk socket, permessage-deflate as the office deck offers it) with fake deps, as fake-deck.js does,
// but with TWELVE tiles in a 3 x 4 grid, every one streaming the way a busy Claude Code session does:
//   - normal-buffer tiles (Claude Code's default): transcript lines in colour, every one different (numbers, words),
//     scrolling into the scroll-back; the spinner / status line at the bottom rewritten in place several times a
//     second with a changing elapsed time and token count (ink: cursor up, erase line, write); now and then a whole
//     redraw of the bottom block
//   - alternate-screen tiles (a full-screen application): every frame a complete redraw from the top, inside
//     synchronized output (DEC 2026), the content moving by a row or two
//   - one normal tile clears its scroll-back now and then (CSI 2 J + CSI 3 J, as /clear does)
// Loopback only, a made-up token, nothing written but the port file. No Claude session, never the live deck.
//   node soak-deck.js <port file> [port] [lines per second per tile] [tiles]
const fs = require('fs');
const path = require('path');
const REPO = path.resolve(__dirname, '..', '..');
const { PhoneServer } = require(path.join(REPO, 'lib', 'phone.js'));

const [portFile, portArg, rateArg, tilesArg] = process.argv.slice(2);
if (!portFile) { console.error('usage: node soak-deck.js <port file> [port] [lines/s] [tiles]'); process.exit(2); }
const PORT = +(portArg || 8797), TOKEN = 'faketoken00000001';
const RATE = Math.max(1, +(rateArg || 12));
const N = Math.max(1, Math.min(12, +(tilesArg || 12)));
const ALT = new Set(['t3', 't7', 't11']);            // full-screen applications
const CLEARS = new Set(['t5']);                     // clears its scroll-back every 90 s

let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const WORDS = ('the a session file reads writes table bug queue todo list worklist label rack count shelf blend drum log ' +
  'migration branch commit test passes fails because which then and with from into every one each line row column ' +
  'Postgres Next.js Tasktracker webhook tier beta dev main deploy runner cache index query join update insert').split(' ');
const sentence = (n) => { const w = []; for (let i = 0; i < n; i++) w.push(WORDS[Math.floor(rnd() * WORDS.length)]); return w.join(' '); };
const SGR = ['\x1b[0m', '\x1b[1m', '\x1b[38;5;174m', '\x1b[38;2;215;119;87m', '\x1b[2m', '\x1b[38;5;246m', '\x1b[32m', '\x1b[31m', '\x1b[36m'];

const tiles = [];
for (let i = 1; i <= N; i++) {
  const id = 't' + i;
  tiles.push({ id, n: 0, frame: 0, rows: 24, cols: 100, replay: '', started: Date.now(), tokens: 0, alt: ALT.has(id), lastText: '' });
}
const byId = Object.fromEntries(tiles.map(t => [t.id, t]));
const uuid = (i) => `${String(i).padStart(8, '0')}-2222-4333-8444-${String(555555555555 + i)}`;
const meta = () => ({
  now: Date.now(), restoring: false,
  accounts: [{ id: 'vv', label: 'vv', abbr: 'vv', email: 'vv@example.com', key: 'vv@example.com', isDefault: false }],
  settings: { grid: { rows: 3, cols: 4 }, msgLines: { normal: 3, wide: 10 }, asks: { deckThreshold: null, picker: 'alongside', usePages: true, background: true, showTile: true, showMarks: true, closeOnDone: true } },
  groups: [], usage: {}, triggers: [], asks: [], askLog: [],
  tiles: tiles.map((t, i) => ({ id: t.id, kind: 'claude', account: 'vv', sessionId: uuid(i + 1), title: `Soak ${i + 1}${t.alt ? ' (full screen)' : ''}`,
    status: 'busy', lastAt: Date.now(), held: [], shownBy: [], lastText: t.lastText })),
});
const platform = { os: 'win32', isMac: false, keyHelp: { zoom: 'F6', asks: 'F7' } };
const layout = Object.fromEntries(tiles.map((t, i) => [t.id, [i + 1]]));

const server = new PhoneServer({
  getInit: () => ({ home: 'h', meta: meta(), platform }),
  // a full-screen tile's replay is its last frame, inside the alternate screen (a desk that attaches late still sees
  // the application full screen, as the office deck's replay of a running TUI does)
  replayFor: (id) => { const t = byId[id]; return !t ? '' : t.alt ? '\x1b[?1049h' + (t.lastFrame || '') : t.replay; },
  askPage: () => null,
  call: (fn, args) => {
    if (fn === 'resize' || fn === 'resizeTile') { const t = byId[String(args[0])]; if (t && args[1] > 1) { t.cols = args[1] | 0; t.rows = args[2] | 0; } }
    return null;
  },
  desk: { open: () => ({ home: 'h', meta: meta(), platform, layout, windowIndex: 1, grid: { rows: 3, cols: 4 } }), close: () => {}, shown: () => {}, layout: () => {}, leaving: () => {} },
});

function emit(t, s) { t.replay = (t.replay + s).slice(-6000); server.tileData(t.id, s); }

// one tick of a normal-buffer Claude Code tile: some transcript lines, then the status block redrawn in place
function tickNormal(t, lines) {
  let s = '';
  // ink: erase the 3-line status block it drew last time (cursor up + erase line), then the new lines, then the block again
  if (t.n) s += '\x1b[2K\x1b[1A\x1b[2K\x1b[1A\x1b[2K\r';
  for (let k = 0; k < lines; k++) {
    t.n++;
    const c = SGR[Math.floor(rnd() * SGR.length)];
    const body = `${c}${t.n % 7 === 0 ? '● ' : '  '}${sentence(3 + Math.floor(rnd() * 22))} #${t.n}\x1b[0m`;
    s += body + '\r\n';
    if (t.n % 5 === 0) t.lastText = sentence(10 + Math.floor(rnd() * 40)) + ' ' + t.n;
  }
  t.tokens += Math.floor(rnd() * 400);
  const secs = Math.floor((Date.now() - t.started) / 1000);
  s += `\x1b[38;2;215;119;87m✻ Working… \x1b[2m(${secs}s · ↑ ${(t.tokens / 1000).toFixed(1)}k tokens · esc to interrupt)\x1b[0m\r\n`;
  s += '\x1b[2m' + '─'.repeat(Math.min(80, t.cols)) + '\x1b[0m\r\n';
  s += `> \x1b[2m${t.n % 3 ? 'Try "fix the todo list"' : ''}\x1b[0m`;
  if (CLEARS.has(t.id) && t.n % (RATE * 90) < lines) s = '\x1b[2J\x1b[3J\x1b[H' + s;
  emit(t, s);
}
// one frame of a full-screen tile: the whole screen again, from the top, inside synchronized output
function tickAlt(t) {
  t.frame++;
  let s = (t.frame === 1 ? '\x1b[?1049h' : '') + '\x1b[?2026h\x1b[H';
  const rows = Math.max(4, t.rows), base = t.frame * 2;
  for (let r = 0; r < rows - 2; r++) {
    const n = base + r;
    s += `\x1b[2K${SGR[n % SGR.length]}${String(n).padStart(6)} ${sentence(2 + (n * 7) % 14)}\x1b[0m\r\n`;
  }
  s += `\x1b[2K\x1b[7m frame ${t.frame}  ${new Date().toISOString()} \x1b[0m\r\n\x1b[2K> `;
  s += '\x1b[?2026l';
  t.lastFrame = s.replace('\x1b[?1049h', '');
  emit(t, s);
}

server.start({ port: PORT, token: TOKEN, bind: 'localhost' }).then(() => {
  const TICK = 250;                                   // 4 ticks a second; RATE lines a second per normal tile
  let acc = 0;
  setInterval(() => {
    acc += RATE * TICK / 1000;
    const lines = Math.floor(acc); acc -= lines;
    for (const t of tiles) { if (t.alt) tickAlt(t); else if (lines) tickNormal(t, lines); }
  }, TICK);
  setInterval(() => server.sendMeta(meta()), 2000);
  fs.writeFileSync(portFile, String(PORT));
  console.log(`soak deck on 127.0.0.1:${PORT}, ${N} tiles, ${RATE} lines/s`);
}).catch(e => { console.error('soak deck:', e.message); process.exit(1); });
