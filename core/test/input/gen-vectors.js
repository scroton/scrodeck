// core/test/input/gen-vectors.js — the mouse-report vectors of core/input/mouse.hpp, taken from xterm.js itself.
//   node core/test/input/gen-vectors.js        -> core/test/input/mouse-vectors.json
// Drives the REAL CoreMouseService of @xterm/headless 6.0.0 (the oracle package core/test/vt already uses): for
// every protocol (9 / 1000 / 1002 / 1003) and encoding (default / SGR 1006 / SGR-pixels 1016) a sequence of
// events goes through triggerMouseEvent, and what it sent (onData / onBinary) and returned is written down.
// A sequence, not single events: the service drops a move that equals the last event it saw.
const path = require('path');
const fs = require('fs');
const { Terminal } = require(path.join(__dirname, '..', 'vt', 'oracle', 'node_modules', '@xterm', 'headless'));

const COLS = 300, ROWS = 250;
const term = new Terminal({ cols: COLS, rows: ROWS, allowProposedApi: true });
const svc = term._core.coreMouseService;
let sent = '';
term.onData(d => { sent += d; });
term.onBinary(d => { sent += d; });

const B = { LEFT: 0, MIDDLE: 1, RIGHT: 2, NONE: 3, WHEEL: 4 };
const A = { UP: 0, DOWN: 1, LEFT: 2, RIGHT: 3, MOVE: 32 };
const ev = (col, row, button, action, mods = '', x, y) => ({ col, row, x: x === undefined ? col * 8 + 3 : x, y: y === undefined ? row * 17 + 5 : y, button, action,
  ctrl: mods.includes('c'), alt: mods.includes('a'), shift: mods.includes('s') });
const seq = [
  ev(0, 0, B.LEFT, A.DOWN), ev(0, 0, B.LEFT, A.UP),
  ev(10, 5, B.LEFT, A.DOWN), ev(11, 5, B.LEFT, A.MOVE), ev(11, 5, B.LEFT, A.MOVE), ev(11, 5, B.LEFT, A.MOVE, '', 95, 91), ev(12, 6, B.LEFT, A.MOVE), ev(12, 6, B.LEFT, A.UP),
  ev(3, 4, B.MIDDLE, A.DOWN), ev(3, 4, B.MIDDLE, A.UP), ev(3, 4, B.RIGHT, A.DOWN), ev(3, 4, B.RIGHT, A.UP),
  ev(7, 8, B.NONE, A.MOVE), ev(7, 8, B.NONE, A.MOVE), ev(8, 8, B.NONE, A.MOVE), ev(8, 9, B.NONE, A.MOVE, 's'),
  ev(20, 10, B.WHEEL, A.UP), ev(20, 10, B.WHEEL, A.DOWN), ev(20, 10, B.WHEEL, A.UP, 'c'), ev(20, 10, B.WHEEL, A.DOWN, 'a'), ev(20, 10, B.WHEEL, A.LEFT), ev(20, 10, B.WHEEL, A.RIGHT),
  ev(1, 2, B.LEFT, A.DOWN, 's'), ev(1, 2, B.LEFT, A.DOWN, 'c'), ev(1, 2, B.LEFT, A.DOWN, 'a'), ev(1, 2, B.LEFT, A.DOWN, 'sca'), ev(1, 2, B.LEFT, A.UP, 'sca'),
  ev(1, 2, B.RIGHT, A.MOVE, 'c'), ev(2, 2, B.MIDDLE, A.MOVE, 'a'),
  ev(222, 222, B.LEFT, A.DOWN), ev(223, 10, B.LEFT, A.DOWN), ev(10, 223, B.LEFT, A.DOWN), ev(94, 94, B.LEFT, A.DOWN), ev(95, 95, B.LEFT, A.UP), ev(127, 127, B.LEFT, A.DOWN), ev(299, 249, B.RIGHT, A.DOWN),
  ev(-1, 0, B.LEFT, A.DOWN), ev(0, -1, B.LEFT, A.DOWN), ev(300, 0, B.LEFT, A.DOWN), ev(0, 250, B.LEFT, A.DOWN),
  ev(5, 5, B.WHEEL, A.MOVE), ev(5, 5, B.NONE, A.DOWN), ev(5, 5, B.NONE, A.UP), ev(5, 5, B.LEFT, A.LEFT), ev(5, 5, B.LEFT, A.RIGHT),
  ev(6, 6, B.LEFT, A.MOVE), ev(6, 6, B.LEFT, A.DOWN), ev(6, 6, B.LEFT, A.MOVE), ev(6, 6, B.LEFT, A.UP), ev(6, 6, B.NONE, A.MOVE)
];
const out = { xterm: require(path.join(__dirname, '..', 'vt', 'oracle', 'node_modules', '@xterm', 'headless', 'package.json')).version, cols: COLS, rows: ROWS, cases: [] };
const modes = { NONE: 0, X10: 9, VT200: 1000, DRAG: 1002, ANY: 1003 }, encs = { DEFAULT: 0, SGR: 1006, SGR_PIXELS: 1016 };
for (const p of Object.keys(modes)) for (const e of Object.keys(encs)) {
  svc.reset(); svc.activeProtocol = p; svc.activeEncoding = e;
  const steps = [];
  for (const s of seq) {
    sent = '';
    const copy = Object.assign({}, s);
    const ok = svc.triggerMouseEvent(copy);
    steps.push({ col: s.col, row: s.row, x: s.x, y: s.y, button: s.button, action: s.action, ctrl: s.ctrl, alt: s.alt, shift: s.shift, ok, hex: Buffer.from(sent, 'latin1').toString('hex') });
  }
  out.cases.push({ mode: modes[p], encoding: encs[e], steps });
}
fs.writeFileSync(path.join(__dirname, 'mouse-vectors.json'), JSON.stringify(out, null, 0).replace(/\{"col"/g, '\n{"col"') + '\n');
let n = 0; for (const c of out.cases) n += c.steps.length;
console.log('wrote mouse-vectors.json: ' + out.cases.length + ' cases, ' + n + ' steps, xterm ' + out.xterm);
