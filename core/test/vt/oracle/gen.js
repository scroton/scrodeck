// core/test/vt/oracle/gen.js — the parity oracle for core/vt.
//
// For every core/test/fixtures/vt/<name>.bin (a raw pty byte stream) with a sidecar <name>.json
//   { "cols": 80, "rows": 24, "scrollback"?: 10000, "chunks"?: [n, n, ...], "ops"?: [ {"feed": n} | {"resize": [cols, rows]} ... ] }
// replay it through headless xterm.js (the same major the deck renders with) and write
//   <name>.expected.json  — readable dump (rows as token strings, see below)
//   <name>.expected.txt   — the same data in a line format the C++ test reads without a JSON parser
//
// Pipeline mirrored from the Electron deck: node-pty hands the renderer UTF-8 *strings* (Node's StringDecoder:
// ill-formed bytes become U+FFFD), and the renderer calls term.write(string). So the oracle decodes the bytes
// with StringDecoder and writes strings; it does NOT use xterm's own Uint8Array path (whose UTF-8 decoder
// drops bad bytes and mis-joins sequences split across writes).
//
// Terminal options mirrored from renderer/js/20-terminals.js: allowProposedApi, scrollback 10000,
// cursorBlink true, windowsPty { backend: 'conpty', buildNumber: 26200 } (=> reflow ON for the normal buffer,
// OFF for the alt buffer; no Windows wrapping heuristics).
//
//   node core/test/vt/oracle/gen.js            # all fixtures
//   node core/test/vt/oracle/gen.js name ...   # only these
'use strict';
const fs = require('fs');
const path = require('path');
const { StringDecoder } = require('string_decoder');
const { Terminal } = require('@xterm/headless');

const FIX = process.env.VT_FIXTURES || path.resolve(__dirname, '..', '..', 'fixtures', 'vt');   // VT_FIXTURES: scratch dir for debugging

// Cell flag bits — must match deck::vt::CellFlags in core/vt/terminal.hpp.
const F = { Bold: 1, Dim: 2, Italic: 4, Underline: 8, Blink: 16, Inverse: 32, Invisible: 64, Strike: 128, Overline: 256,
  UnderlineStyleShift: 9, Protected: 0x1000, Hyperlink: 0x2000 };

function colorTok(isDefault, isRgb, value) {
  if (isDefault) return 'd';
  return (isRgb ? 'r' : 'p') + value.toString(16);
}

function cellTok(cell) {
  const chars = cell.getChars();
  let cps = '-';
  if (chars.length) cps = Array.from(chars).map(c => c.codePointAt(0).toString(16)).join('+');
  let flags = 0;
  if (cell.isBold()) flags |= F.Bold;
  if (cell.isDim()) flags |= F.Dim;
  if (cell.isItalic()) flags |= F.Italic;
  if (cell.isUnderline()) flags |= F.Underline;
  if (cell.isBlink()) flags |= F.Blink;
  if (cell.isInverse()) flags |= F.Inverse;
  if (cell.isInvisible()) flags |= F.Invisible;
  if (cell.isStrikethrough()) flags |= F.Strike;
  if (cell.isOverline()) flags |= F.Overline;
  const hasExt = !!(cell.bg & 0x10000000);
  const ulStyle = hasExt ? cell.extended.underlineStyle : ((cell.fg & 0x10000000) ? 1 : 0);
  flags |= (ulStyle & 7) << F.UnderlineStyleShift;
  if (cell.bg & 0x20000000) flags |= F.Protected;
  if (hasExt && cell.extended.urlId) flags |= F.Hyperlink;
  return cps + ',' + cell.getWidth() + ',' + colorTok(cell.isFgDefault(), cell.isFgRGB(), cell.getFgColor()) + ',' +
    colorTok(cell.isBgDefault(), cell.isBgRGB(), cell.getBgColor()) + ',' + flags.toString(16);
}

function lineTok(line, cols) {
  const toks = [];
  let prev = null, n = 0;
  for (let x = 0; x < cols; x++) {
    const t = cellTok(line.getCell(x));
    if (t === prev) { n++; continue; }
    if (prev !== null) toks.push(n > 1 ? n + '*' + prev : prev);
    prev = t; n = 1;
  }
  if (prev !== null) toks.push(n > 1 ? n + '*' + prev : prev);
  return (line.isWrapped ? 'w' : 'n') + ' ' + toks.join(' ');
}

function hex(s) { return Buffer.from(s, 'utf8').toString('hex') || '-'; }

async function run(name) {
  const bin = fs.readFileSync(path.join(FIX, name + '.bin'));
  const side = JSON.parse(fs.readFileSync(path.join(FIX, name + '.json'), 'utf8'));
  const term = new Terminal({
    cols: side.cols, rows: side.rows, allowProposedApi: true,
    scrollback: side.scrollback === undefined ? 10000 : side.scrollback,
    cursorBlink: true,
    windowsPty: { backend: 'conpty', buildNumber: 26200 },
    logLevel: 'off',
  });
  let replies = '';
  term.onData(d => { replies += d; });
  let title = '';
  term.onTitleChange(t => { title = t; });
  const dec = new StringDecoder('utf8');
  const write = buf => new Promise(res => {
    const s = dec.write(buf);
    if (!s.length) return res();
    term.write(s, res);
  });
  let pos = 0;
  const feed = async n => {
    const end = Math.min(bin.length, pos + n);
    if (side.chunks && side.chunks.length) {
      let i = 0;
      while (pos < end) {
        const c = Math.max(1, side.chunks[i++ % side.chunks.length]);
        const e = Math.min(end, pos + c);
        await write(bin.subarray(pos, e));
        pos = e;
      }
    } else {
      await write(bin.subarray(pos, end));
      pos = end;
    }
  };
  for (const op of side.ops || []) {
    if (op.feed !== undefined) await feed(op.feed);
    else if (op.resize) term.resize(op.resize[0], op.resize[1]);
  }
  await feed(bin.length - pos);

  const core = term._core;
  const buf = term.buffer.active;
  const normal = term.buffer.normal;
  const cols = term.cols, rows = term.rows;
  const dm = core.coreService.decPrivateModes;
  const mouseMap = { NONE: 0, X10: 9, VT200: 1000, DRAG: 1002, ANY: 1003 };
  const encMap = { DEFAULT: 0, SGR: 1006, SGR_PIXELS: 1016 };
  const shapeMap = { block: 1, underline: 2, bar: 3 };
  const modes = {
    alt: buf.type === 'alternate' ? 1 : 0,
    appCursor: dm.applicationCursorKeys ? 1 : 0,
    appKeypad: dm.applicationKeypad ? 1 : 0,
    bracketedPaste: dm.bracketedPasteMode ? 1 : 0,
    mouse: mouseMap[core.coreMouseService.activeProtocol],
    mouseEnc: encMap[core.coreMouseService.activeEncoding],
    focus: dm.sendFocus ? 1 : 0,
    cursorVisible: core.coreService.isCursorHidden ? 0 : 1,
    cursorShape: dm.cursorStyle === undefined ? 0 : shapeMap[dm.cursorStyle],
    cursorBlink: dm.cursorBlink === undefined ? -1 : (dm.cursorBlink ? 1 : 0),
    origin: dm.origin ? 1 : 0,
    wraparound: dm.wraparound ? 1 : 0,
    insert: core.coreService.modes.insertMode ? 1 : 0,
    sync: dm.synchronizedOutput ? 1 : 0,
    scrollTop: core.buffer.scrollTop,
    scrollBottom: core.buffer.scrollBottom,
  };
  const viewport = [];
  for (let y = 0; y < rows; y++) viewport.push(lineTok(buf.getLine(buf.baseY + y), cols));
  const scrollback = [];
  for (let i = 0; i < normal.baseY; i++) scrollback.push(lineTok(normal.getLine(i), cols));

  const out = {
    fixture: name, xterm: require('@xterm/headless/package.json').version,
    cols, rows, cursor: { x: buf.cursorX, y: buf.cursorY }, modes, title, replies,
    scrollbackLength: normal.baseY, viewport, scrollback,
  };
  fs.writeFileSync(path.join(FIX, name + '.expected.json'), JSON.stringify(out, null, 1) + '\n');

  const L = [];
  L.push('init ' + side.cols + ' ' + side.rows + ' ' + (side.scrollback === undefined ? 10000 : side.scrollback));
  for (const op of side.ops || []) {
    if (op.feed !== undefined) L.push('op feed ' + op.feed);
    else if (op.resize) L.push('op resize ' + op.resize[0] + ' ' + op.resize[1]);
  }
  L.push('size ' + cols + ' ' + rows);
  L.push('cursor ' + buf.cursorX + ' ' + buf.cursorY);
  L.push('modes ' + Object.keys(modes).map(k => k + '=' + modes[k]).join(' '));
  L.push('title ' + hex(title));
  L.push('replies ' + hex(replies));
  L.push('scrollback ' + normal.baseY);
  for (const v of viewport) L.push('V ' + v);
  for (const s of scrollback) L.push('S ' + s);
  fs.writeFileSync(path.join(FIX, name + '.expected.txt'), L.join('\n') + '\n');
  term.dispose();
}

// xterm.js can throw from inside a write (it runs on a timer, so the error surfaces here): such a stream asks
// for something xterm itself cannot survive. Report the fixture, skip it, carry on with the next.
let current = '';
let abortCurrent = null;
process.on('uncaughtException', e => {
  console.error('xterm.js threw while replaying ' + current + ': ' + String(e && e.stack || e).split('\n').slice(0, 3).join(' | '));
  if (abortCurrent) abortCurrent(e); else process.exit(3);
});

(async () => {
  let names = process.argv.slice(2);
  if (!names.length) names = fs.readdirSync(FIX).filter(f => f.endsWith('.bin')).map(f => f.slice(0, -4)).sort();
  let bad = 0;
  for (const n of names) {
    current = n;
    try {
      await Promise.race([run(n), new Promise((_, rej) => { abortCurrent = rej; })]);
    } catch (e) {
      bad++;
      for (const ext of ['.expected.json', '.expected.txt']) fs.rmSync(path.join(FIX, n + ext), { force: true });
      console.error('FAIL ' + n);
    }
  }
  console.log('oracle: wrote ' + (names.length - bad) + ' expected file(s), ' + bad + ' failed, xterm ' + require('@xterm/headless/package.json').version);
  process.exit(bad ? 1 : 0);
})();
