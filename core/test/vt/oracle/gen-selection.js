// core/test/vt/oracle/gen-selection.js — vectors for vt::Terminal::selectionText, from headless xterm.js.
//
//   node core/test/vt/oracle/gen-selection.js      -> core/test/vt/selection-vectors.txt
//
// Headless xterm.js has no SelectionService (that lives in the browser build), but the part of a copy that is
// terminal logic is BufferLine.translateToString(trimRight, startCol, endCol) and the line's isWrapped flag, and
// both are in the headless API. So every fixture of core/test/fixtures/vt is replayed exactly as gen.js replays
// it, and for a handful of ranges the text is built the way @xterm/xterm 6.0.0
// src/browser/services/SelectionService.ts `get selectionText()` builds it (normal mode: first row, middle rows,
// final row; a wrapped row is appended to the one before; no-break spaces become spaces), with xterm's own
// translateToString doing each row. The join is LF here (the browser picks CRLF on Windows; the C++ takes the
// newline as a parameter).
//
// Line format (read by vt_test.cpp without a JSON parser):
//   F <fixture>
//   S <startCol> <startLine> <endCol> <endLine> <hex of the UTF-8 text, or ->
'use strict';
const fs = require('fs');
const path = require('path');
const { StringDecoder } = require('string_decoder');
const { Terminal } = require('@xterm/headless');

const FIX = path.resolve(__dirname, '..', '..', 'fixtures', 'vt');
const OUT = path.resolve(__dirname, '..', 'selection-vectors.txt');

async function replay(name) {
  const bin = fs.readFileSync(path.join(FIX, name + '.bin'));
  const side = JSON.parse(fs.readFileSync(path.join(FIX, name + '.json'), 'utf8'));
  const term = new Terminal({ cols: side.cols, rows: side.rows, allowProposedApi: true, scrollback: side.scrollback === undefined ? 10000 : side.scrollback,
    cursorBlink: true, windowsPty: { backend: 'conpty', buildNumber: 26200 }, logLevel: 'off' });
  const dec = new StringDecoder('utf8');
  const write = buf => new Promise(res => { const s = dec.write(buf); if (!s.length) return res(); term.write(s, res); });
  let pos = 0;
  const feed = async n => {
    const end = Math.min(bin.length, pos + n);
    if (side.chunks && side.chunks.length) {
      let i = 0;
      while (pos < end) { const c = Math.max(1, side.chunks[i++ % side.chunks.length]); const e = Math.min(end, pos + c); await write(bin.subarray(pos, e)); pos = e; }
    } else { await write(bin.subarray(pos, end)); pos = end; }
  };
  for (const op of side.ops || []) { if (op.feed !== undefined) await feed(op.feed); else if (op.resize) term.resize(op.resize[0], op.resize[1]); }
  await feed(bin.length - pos);
  return term;
}

// SelectionService.selectionText, normal mode; start inclusive, end exclusive, [col, line]
function selectionText(buf, start, end) {
  if (start[1] > end[1] || (start[1] === end[1] && start[0] >= end[0])) return '';
  const row = (i, s, e) => { const l = buf.getLine(i); return l ? l.translateToString(true, s, e) : ''; };
  const result = [];
  result.push(row(start[1], start[0], start[1] === end[1] ? end[0] : undefined));
  for (let i = start[1] + 1; i <= end[1] - 1; i++) {
    const l = buf.getLine(i); const text = row(i);
    if (l && l.isWrapped) result[result.length - 1] += text; else result.push(text);
  }
  if (start[1] !== end[1]) {
    const l = buf.getLine(end[1]); const text = row(end[1], 0, end[0]);
    if (l && l.isWrapped) result[result.length - 1] += text; else result.push(text);
  }
  return result.map(line => line.replace(/ /g, ' ')).join('\n');
}

(async () => {
  const names = fs.readdirSync(FIX).filter(f => f.endsWith('.bin')).map(f => f.slice(0, -4)).sort();
  const L = ['# written by core/test/vt/oracle/gen-selection.js from @xterm/headless ' + require('@xterm/headless/package.json').version + ' - do not edit'];
  let vectors = 0, skipped = 0;
  for (const name of names) {
    let term;
    try { term = await replay(name); } catch (e) { skipped++; continue; }
    const buf = term.buffer.active, cols = term.cols, rows = term.rows;
    const lines = buf.baseY + rows;
    if (buf.length !== lines) { skipped++; term.dispose(); continue; }      // (never seen; the C++ side counts lines this way)
    let seed = 0; for (const ch of name) seed = (seed * 31 + ch.charCodeAt(0)) >>> 0;
    const rnd = n => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return n <= 0 ? 0 : (seed >>> 8) % n; };
    const ranges = [];
    ranges.push([[0, buf.baseY], [cols, buf.baseY + rows - 1]]);                       // the whole viewport
    ranges.push([[0, buf.baseY + buf.cursorY], [cols, buf.baseY + buf.cursorY]]);     // the cursor's row
    if (lines <= 120) ranges.push([[0, 0], [cols, lines - 1]]);                        // everything
    for (let i = 0; i < 9; i++) {
      const y0 = rnd(lines), span = i < 3 ? 0 : rnd(Math.min(10, lines - y0));
      const x0 = rnd(cols + 1); let x1 = rnd(cols + 1);
      if (span === 0 && x1 < x0) x1 = Math.min(cols, x0 + rnd(cols - x0 + 1));
      ranges.push([[x0, y0], [x1, y0 + span]]);
    }
    L.push('F ' + name);
    for (const [s, e] of ranges) {
      const text = selectionText(buf, s, e);
      L.push('S ' + s[0] + ' ' + s[1] + ' ' + e[0] + ' ' + e[1] + ' ' + (Buffer.from(text, 'utf8').toString('hex') || '-'));
      vectors++;
    }
    term.dispose();
  }
  fs.writeFileSync(OUT, L.join('\n') + '\n');
  console.log('wrote selection-vectors.txt: ' + vectors + ' ranges over ' + (names.length - skipped) + ' fixtures (' + skipped + ' skipped), xterm ' + require('@xterm/headless/package.json').version);
})();
