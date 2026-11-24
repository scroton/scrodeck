'use strict';
// Generate core/test/fixtures/deck/deskopen.json from the REAL lib/deskopen.js (required as it is).
//   node core/test/deck/gen-deskopen.js
// The pure rules (ID_RE, pickSpot, checkRequest, accountFor, openPlan, placedWords) on synthetic values, and the
// inbox as scripted runs of the real DeskOpenInbox in a temp folder (fs.mkdtempSync under os.tmpdir(), removed
// afterwards) with a fake open() that returns canned results or throws. Pinned: `now` (deps.now, moved by the
// script). After every drain each file the inbox wrote is given the pinned `now` as its mtime, which is what the
// C++ test's in-memory files stamp a write with; every file's text and mtime are recorded after each step.
// No local-time date is used (every expiresAt carries its zone), so no TZ is pinned.
// accountFor / openPlan are also run with path = path.posix (the module loaded a second time), for a Mac's deck.
const fs = require('fs');
const os = require('os');
const path = require('path');
const Module = require('module');

const REPO = path.join(__dirname, '..', '..', '..');
const DESKOPEN = path.join(REPO, 'lib', 'deskopen.js');
const D = require(DESKOPEN);
function loadWith(file, mocks) {
  const orig = Module.prototype.require;
  Module.prototype.require = function (id) { return Object.prototype.hasOwnProperty.call(mocks, id) ? mocks[id] : orig.apply(this, arguments); };
  delete require.cache[require.resolve(file)];
  try { return require(file); } finally { Module.prototype.require = orig; delete require.cache[require.resolve(file)]; }
}
const Dposix = loadWith(DESKOPEN, { path: path.posix });
const plain = x => (x === undefined ? null : JSON.parse(JSON.stringify(x)));   // what a JSON fixture can hold: an undefined field is no field

const ID = 'security-20261116T200000Z-beef', SID = '0f3c2a9e-1b2c-4d5e-8f90-123456789abc';
const NOW = Date.parse('2026-11-16T20:00:30Z');
const good = (o = {}) => ({ v: 1, kind: 'desk-open', id: ID, desk: 'security', title: 'security desk', account: 'C:/Users/tester/.claude-demo', accountName: '.claude-demo',
  cwd: 'C:/Users/tester', sessionId: SID, resume: false, prompt: 'You are the desk.\r\n\r\nOPENED by the Claude on-call in a Claude Deck tile', note: 'The Claude on-call: new security bells - sbell-1.',
  model: 'claude-opus-5', chrome: true, oncallKind: 'security-desk', bells: ['sbell-1'], createdAt: '2026-11-16T20:00:00.000Z', expiresAt: '2026-11-16T20:01:10.000Z', ...o });
const without = (...keys) => { const g = good(); for (const k of keys) delete g[k]; return g; };

// ---------------------------------------------------------------- ID_RE
const ids = [ID, 'web-20261116T200001Z-0002', 'SECURITY-20261116T200000Z-BEEF', 'a-00000000T000000Z-0000', 'a1b2c3d4e5f6g7h8i9j0-20261116T200000Z-abcd', 'a1b2c3d4e5f6g7h8i9j0k-20261116T200000Z-abcd', '-20261116T200000Z-abcd', 'web-20261116t200000z-abcd',
  'web-2026111T200000Z-abcd', 'web-202611160T200000Z-abcd', 'web-20261116T20000Z-abcd', 'web-20261116T200000-abcd', 'web-20261116T200000Z-abc', 'web-20261116T200000Z-abcde', 'web-20261116T200000Z-abcg', 'wdb_x-20261116T200000Z-abcd', 'w db-20261116T200000Z-abcd',
  'web-20261116T200000Z-abcd\n', '\nweb-20261116T200000Z-abcd', ' web-20261116T200000Z-abcd', 'web-20261116T200000Z-abcd ', '', 'web', 'web--20261116T200000Z-abcd', 'w-d-20261116T200000Z-abcd', '\u00e9-20261116T200000Z-abcd', 'web-2026111\u0666T200000Z-abcd', 'web-20261116T200000Z-ABCD', 'web-20261116T200000Z-12ef', '0-20261116T200000Z-0000', 'web-20261116T200000Z-abcd.request']
  .map(id => ({ id, ok: D.ID_RE.test(id) }));

// ---------------------------------------------------------------- pickSpot
const G22 = { rows: 2, cols: 2 }, G34 = { rows: 3, cols: 4 };
const main = (layout, extra = {}) => ({ index: 0, primary: true, layout, showing: false, ...extra });
const aux = (index, layout, extra = {}) => ({ index, primary: false, layout, showing: false, ...extra });
const full4 = { a: [1, 2, 3, 4] };
const spots = [
  [[main({})], G22], [[main({ a: [1], b: [2] })], G22], [[main({ a: [1], b: [2], c: [3], d: [4], e: [5], f: [7], g: [8], h: [9] })], G34],
  [[main({ t1: [4], t2: [8], t3: [12], t4: [1], t5: [5], t6: [2], t7: [3], t8: [7], t9: [9] })], G34], [[main({ a: [1, 2] })], G22],
  [[main(full4), aux(1, { b: [1] })], G22], [[{ index: 3, primary: false, layout: {} }, main(full4), { index: 2, primary: false, layout: { x: [1, 2, 3] } }], G22],
  [[{ index: 0, primary: false, layout: {} }, { index: 4, primary: true, layout: { a: [1] } }], G22], [[main({}, { showing: true }), aux(1, {})], G22],
  [[main(full4), aux(1, { b: [1, 2, 3, 4] })], G22], [[], G22], [[main({ a: [1] })], { rows: 1, cols: 1 }],
  // the grid, clamped
  [[main({ a: [1] })], null], [[main({ a: [1], b: [2], c: [3], d: [4] })], {}], [[main({ a: [1], b: [2], c: [3], d: [4] })], { rows: '3', cols: 'x' }], [[main(full4)], { rows: 0, cols: 0 }], [[main({ a: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15] })], { rows: 9, cols: 99 }],
  [[main({ a: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16] })], { rows: 9, cols: 99 }], [[main({ a: [1] })], { rows: -3, cols: -1 }], [[main({ a: [1, 2, 3] })], { rows: 2.5, cols: 1.4 }], [[main({ a: [1, 2, 3] })], { rows: 2.4, cols: 1.5 }], [[main({ a: [1, 2] })], { rows: 0.4, cols: 2 }],
  [[main({ a: [1, 2, 3, 4, 5, 6, 7, 8] })], { rows: '0x2', cols: ' 4 ' }], [[main({ a: [1, 2] })], { rows: true, cols: [2] }], [[main({ a: [1, 2, 3, 4] })], { rows: null, cols: '' }], [[main({ a: [1, 2, 3, 4, 5, 6] })], { rows: '1e1', cols: '2.' }], [[main({ a: [1, 2, 3, 4] })], { rows: 'Infinity', cols: '-Infinity' }],
  [[main({ a: [1, 2, 3, 4] })], { rows: '3px', cols: {} }], [[main({ a: [1, 2, 3] })], 5], [[main({ a: [1, 2, 3] })], 'grid'], [[main({ a: [1, 2, 3] })], [3, 3]], [[main({ a: [1, 2, 3] })], { rows: '.5', cols: '+3' }], [[main({ a: [1, 2, 3] })], { rows: [], cols: ['3'] }], [[main({ a: [1, 2, 3] })], { rows: '0b11', cols: '0o2' }], [[main({ a: [1, 2, 3] })], { rows: '1e', cols: '-' }],
  // the windows, as they come
  [null, G22], ['abc', G22], [[null, 5, 'w', [], { primary: true, layout: {} }, { index: 1.5, layout: {} }, { index: '1', layout: {} }, { index: null }, aux(2, { a: [1] })], G22], [[aux(-1, { a: [1] }), aux(7, {})], G22], [[aux(5, {}), aux(2, {}), aux(9, {})], G22],
  [[main(null)], G22], [[{ index: 0, primary: true }], G22], [[main([[1], [2]])], G22], [[main({ a: 1, b: 'x', c: null, d: { 0: 1 }, e: [2] })], G22], [[main({ a: ['1', '2'] })], G22], [[main({ a: ['x'] }, { showing: true }), aux(1, {})], G22],
  [[main({ a: [null] }, { showing: true })], G22], [[main({ a: [true, [2], '3', 4.0] })], G22], [[main({ a: [] }, { showing: true }), aux(3, { z: [] })], G22], [[main({ a: [] }, { showing: 'yes' }), aux(3, { z: [1] }, { showing: 1 })], G22], [[main('layout', { showing: true }), aux(1, 5, { showing: 0 })], G22],
  [[{ index: 2, primary: 1, layout: full4 }, { index: 1, primary: 'yes', layout: { a: [1] } }, aux(0, {})], G22], [[aux(1, full4), aux(1, { a: [1] }), main(full4)], G22], [[main({ a: [0, 5, 6, -1, 1.5] })], G22], [[main({ a: [1, 1, 1], b: [1], c: [2, 2] })], G22],
  [[main(full4), aux(1, full4), aux(2, full4), aux(3, { a: [1, 2, 3] })], G22], [[main(full4, { showing: true }), aux(1, {}, { showing: true }), aux(2, {}, { showing: false })], G22], [[{ index: 1e3, primary: false, layout: {} }], G22],
].map(([windows, grid]) => ({ windows, grid, out: D.pickSpot(windows, grid) }));

// ---------------------------------------------------------------- checkRequest
const requests = [];
const R = (raw, id = ID, now = NOW) => requests.push({ raw: plain(raw), id, now, out: plain(D.checkRequest(plain(raw), id, now)) });
R(good()); R(good(), ID, Date.parse('2026-11-16T20:02:00Z')); R(good({ id: 'security-20261116T200000Z-0000' })); R(good({ desk: 'planner' })); R(good({ desk: 'web', title: undefined }));
for (const v of [null, [1], 'str', 5, true, {}, []]) R(v);
for (const k of ['id', 'kind', 'desk', 'title', 'account', 'accountName', 'cwd', 'sessionId', 'resume', 'prompt', 'note', 'model', 'chrome', 'bells', 'expiresAt'])
  for (const v of [undefined, '', null, 0, 5, 1.5, true, false, {}, { a: 1 }, [], ['x', 2], '   ', 'text', '\u65e5\u672c \ud83d\ude00']) {
    // Date.parse(5) and Date.parse(['x', 2]) ("x,2") are legacy dates in JavaScript: not ported, and said so (core/deck/deskopen.cpp)
    if (k === 'expiresAt' && (typeof v === 'number' || (Array.isArray(v) && v.length))) continue;
    R(v === undefined ? without(k) : good({ [k]: v }));
  }
// the id
R(good({ id: 'nope' }), 'nope'); R(good({ id: '' }), ''); R(good({ id: ID.toUpperCase() }), ID.toUpperCase()); R(good({ id: ID.toUpperCase() })); R(good({ id: 7 }), '7'); R(good({ id: [ID] })); R(good({ id: ID + ' ' }), ID + ' ');
R(good({ id: 'a1b2c3d4e5f6g7h8i9j0-20261116T200000Z-abcd' }), 'a1b2c3d4e5f6g7h8i9j0-20261116T200000Z-abcd'); R(good({ id: 'web-20261116t200000z-ABCD', desk: 'web' }), 'web-20261116t200000z-ABCD');
// the kind, in the words of the refusal
for (const kind of ['desk', 'desk-open ', 'DESK-OPEN', 'de"sk\n\t\\ \u65e5\u672c \u0001\u001f\u007f', { a: 1, b: [true, null, 'x'], c: { d: 1.5 } }, [1, 'x', null, [2], {}], 1.5, -0.25, 1e21, 123456789012345, true, null]) R(good({ kind }));
// the desk
for (const desk of ['security', 'web', 'Security', 'web ', ['web'], { desk: 'web' }]) R(good({ desk }));
// the expiry (every form with its zone)
for (const expiresAt of ['2026-11-16T20:00:30Z', '2026-11-16T20:00:30.000Z', '2026-11-16T20:00:29.999Z', '2026-11-16T20:00:30.001Z', '2026-11-16T22:00:29+02:00', '2026-11-16T15:00:31-05:00', '2026-11-16', '2026-11-17', 'garbage', '2026-13-01T00:00:00Z', '2026-11-16T20:00Z', '2026-11-16T25:00:00Z', 'Z', '20261116T200030Z'])
  R(good({ expiresAt }));
R(good({ expiresAt: '2026-11-16T20:00:30.000Z' }), ID, NOW + 1); R(good({ expiresAt: '2026-11-16T20:00:30.000Z' }), ID, NOW - 1); R(good(), ID, 0); R(good(), ID, NOW * 2);
// the session id
for (const sessionId of [SID.toUpperCase(), SID.slice(1), SID + '0', SID.replace('a', 'g'), '-'.repeat(36), 'f'.repeat(36), ' ' + SID.slice(1), SID.slice(0, 35) + '\n', '0F3C2A9E-1B2C-4D5E-8F90-123456789aBc']) R(good({ sessionId }));
// account, folder, names
R(good({ account: '  C:/Users/tester/.claude-vv \n', cwd: '\tD:/work files\u00a0', accountName: '  .claude-vv  ' })); R(good({ account: 5, cwd: 7, accountName: 9 })); R(good({ account: ['C:/a', 'b'], cwd: [' D:/x '] })); R(good({ account: '\u3000\ufeff', cwd: 'x' })); R(good({ cwd: '\u2028' }));
// the first message
R(good({ prompt: '  \r\n one\rtwo\r\nthree\n\rfour\r\r\nfive \r' })); R(good({ prompt: '\r\n\r\n' })); R(good({ prompt: 'x'.repeat(24000) })); R(good({ prompt: 'x'.repeat(24001) })); R(good({ prompt: '  ' + 'x'.repeat(24000) + '\r\n' })); R(good({ prompt: 'x\r\n'.repeat(12000) }));
R(good({ prompt: 'x\r\n'.repeat(12001) })); R(good({ prompt: '\ud83d\ude00'.repeat(12000) })); R(good({ prompt: '\ud83d\ude00'.repeat(12001) })); R(good({ prompt: '\u65e5'.repeat(24000) })); R(good({ prompt: '\u65e5'.repeat(24003) })); R(good({ prompt: 12345 })); R(good({ prompt: ['a', ['b', null], 3] }));
// title, note, model: collapsed, trimmed, cut
R(good({ title: '  a   title\twith \r\n runs\u00a0\u3000of  space  ' })); R(good({ title: 't'.repeat(100) })); R(good({ title: 't'.repeat(78) + '\ud83d\ude00' + 'after' })); R(good({ title: ' ' + '\u65e5'.repeat(90) })); R(good({ title: 'a b '.repeat(30) })); R(good({ title: 123 })); R(good({ title: ['a', 'b'] }));
R(good({ note: ' a\n\nnote \t over  lines ' })); R(good({ note: 'n'.repeat(700) })); R(good({ note: 'n '.repeat(350) })); R(good({ note: '\ud83d\ude00'.repeat(300) + 'x' })); R(good({ note: 99 }));
R(good({ model: '  claude-opus-5  ' })); R(good({ model: 'm'.repeat(100) })); R(good({ model: ' ' + 'm'.repeat(100) })); R(good({ model: 5 })); R(good({ model: ['a'] })); R(good({ model: ' ' }));
// flags and bells
for (const v of ['true', 1, 'yes', [true]]) R(good({ resume: v, chrome: v }));
R(good({ resume: true, chrome: false })); R(good({ bells: Array.from({ length: 60 }, (_, i) => 'bell-' + i) })); R(good({ bells: [1, null, 'a', [1, 2], [null, [3]], { a: 1 }, true, false, 1.5, ''] })); R(good({ bells: 'sbell-1' })); R(good({ bells: { 0: 'a', length: 1 } }));

// ---------------------------------------------------------------- accountFor
const ACCTS = [{ id: 'claudev', dir: 'C:\\Users\\tester\\.claude-vv' }, { id: 'claudem', dir: 'C:\\Users\\tester\\.claude-demo' }, { id: 'mac', dir: '/Users/tester/.claude' }, { id: 'ecole', dir: 'D:\\\u00c9COLE\\\u0416\u0423\u041a\\cfg\\' }];
const acctCases = [];
const AC = (accounts, dir) => acctCases.push({ accounts: plain(accounts), dir, win: plain(D.accountFor(plain(accounts), dir)), posix: plain(Dposix.accountFor(plain(accounts), dir)) });
for (const dir of ['c:/users/tester/.claude-demo/', 'C:/Users/tester/.claude-x', 'C:\\Users\\tester\\.claude-vv', 'C:\\Users\\tester\\.claude-vv\\\\', 'C:/Users/tester/x/../.claude-vv//', 'C:/Users/TESTER/./.CLAUDE-DEMO', '/Users/tester/.claude', '/Users/tester/.claude/', '/users/TESTER/.claude', '\\Users\\tester\\.claude',
  'd:/\u00e9cole/\u0436\u0443\u043a/cfg', 'D:\\\u00c9cole\\\u0416\u0443\u041a\\CFG\\\\\\', '', '.', './', 'C:', 'C:\\', 'c:/users/tester', 'C:/Users/tester/.claude-demo/sub/..', 'C:/Users/tester/.claude-demo/..'])
  AC(ACCTS, dir);
AC(null, 'C:/x'); AC([], 'C:/x'); AC([null, 5, 'x', {}, { id: 'nodir' }, { id: 'a', dir: 'C:/x/' }], 'c:\\X'); AC([{ id: 'nodir' }, { id: 'empty', dir: '' }], ''); AC([{ id: 'dot', dir: '.' }, { id: 'nodir' }], ''); AC([{ id: 'n', dir: 5 }, { id: 'z', dir: 0 }], '5'); AC([{ id: 'first', dir: 'C:/a' }, { id: 'second', dir: 'c:\\A\\' }], 'C:\\a');
AC([{ id: 'unc', dir: '\\\\srv\\share\\cfg' }], '//SRV/share/cfg/'); AC([{ id: 'rel', dir: 'rel\\cfg' }], 'REL/x/../cfg'); AC([{ id: 'arr', dir: ['C:/arr'] }, { id: 'obj', dir: { a: 1 } }], 'c:/arr'); AC([{ id: 'obj', dir: { a: 1 } }], '[object Object]');

// ---------------------------------------------------------------- openPlan
const REQ = D.checkRequest(good(), ID, NOW).req;
const plans = [];
const OP = (req, tiles, accounts, isDir = 'all') => {
  const fn = isDir === 'all' ? () => true : isDir === 'none' ? () => false : d => isDir.includes(d);
  const opts = { ...(tiles === undefined ? {} : { tiles: plain(tiles) }), ...(accounts === undefined ? {} : { accounts: plain(accounts) }), isDir: fn };
  plans.push({ req: plain(req), tiles: plain(tiles === undefined ? [] : tiles), accounts: plain(accounts === undefined ? [] : accounts), isDir, win: plain(D.openPlan(plain(req), opts)), posix: plain(Dposix.openPlan(plain(req), opts)) });
};
OP(REQ, [{ id: 'other', kind: 'claude', sessionId: 'aaaaaaaa-1b2c-4d5e-8f90-123456789abc', running: true }], ACCTS);
OP({ ...REQ, chrome: false }, undefined, ACCTS); OP(REQ, [{ id: 'T1', kind: 'claude', account: 'claudem', sessionId: SID.toUpperCase(), running: true }], ACCTS);
OP({ ...REQ, note: '' }, [{ id: 'T1', kind: 'claude', sessionId: SID, running: true }], ACCTS); OP({ ...REQ, resume: true }, [{ id: 'T2', kind: 'claude', account: 'claudev', sessionId: SID, cwd: 'D:/work', running: false }], ACCTS);
OP({ ...REQ, account: 'C:/Users/tester/.claude-x' }, [], ACCTS); OP(REQ, [], ACCTS, 'none'); OP(REQ, [{ id: 'S', kind: 'shell', sessionId: SID, running: true }], ACCTS); OP(REQ, [], undefined); OP(REQ, null, null);
OP(REQ, [], ACCTS, ['C:/Users/tester']); OP(REQ, [], ACCTS, ['C:\\Users\\tester']); OP({ ...REQ, cwd: 'D:/gone' }, [], ACCTS, ['C:/Users/tester']);
OP(REQ, [null, 5, 'x', { kind: 'claude' }, { kind: 'claude', sessionId: '' }, { kind: 'claude', sessionId: 0 }, { kind: 'Claude', sessionId: SID }, { id: 'T3', kind: 'claude', sessionId: SID }, { id: 'T4', kind: 'claude', sessionId: SID, running: true }], ACCTS);
OP(REQ, [{ kind: 'claude', sessionId: SID, running: 1 }], ACCTS); OP(REQ, [{ kind: 'claude', sessionId: SID, running: 0, cwd: '' }], ACCTS); OP(REQ, [{ id: 'T5', kind: 'claude', sessionId: SID, running: false, account: null, cwd: null }], ACCTS);
OP({ ...REQ, model: null, desk: 'web', title: 'web desk' }, [], ACCTS); OP({ ...REQ, account: '/users/tester/.claude/', cwd: '/Users/tester' }, [{ id: 'T6', kind: 'claude', sessionId: SID, running: false, account: 'mac', cwd: '/elsewhere' }], ACCTS);
OP({ ...REQ, account: '/Users/tester/.claude' }, [], ACCTS); OP({ ...REQ, account: 'd:/\u00e9cole/\u0436\u0443\u043a/CFG' }, [], ACCTS); OP(REQ, [{ id: 'T7', kind: 'claude', sessionId: SID, running: true }], ACCTS, 'none'); OP({ ...REQ, note: '' }, [{ id: 'T8', kind: 'claude', sessionId: SID, running: false }], ACCTS);
OP(REQ, [{ id: 'first', kind: 'claude', sessionId: SID, running: false }, { id: 'second', kind: 'claude', sessionId: SID, running: true }], ACCTS); OP(REQ, [], [{ dir: 'C:/Users/tester/.claude-demo' }]);

// ---------------------------------------------------------------- placedWords
const words = [null, false, 0, '', {}, 5, 'p', [], { cells: [] }, { primary: true, window: 0, cells: [6] }, { primary: false, window: 2, cells: [1] }, { newWindow: true, window: 3, cells: [1] }, { cells: [1, 2, 5] }, { cells: [null, 'x', 3, [4, 5], true], primary: 1 },
  { primary: false, cells: [1] }, { window: null, cells: [2] }, { window: 'w', cells: null }, { window: 1.5 }, { newWindow: 1 }, { newWindow: true, primary: true, cells: [] }, { primary: true }, { cells: { 0: 1 }, window: 4 }, { window: [1, 2], cells: [0] }, { window: true, cells: [1.5] }]
  .map(p => ({ p, out: D.placedWords(p) }));

// ---------------------------------------------------------------- the inbox, in a temp folder
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-deskopen-'));
let runNo = 0;
function inboxRun(label, script, sub = '') {
  const dir = path.join(tempRoot, 'run' + (++runNo), sub);
  if (!sub) fs.mkdirSync(dir, { recursive: true });       // with `sub`, start() makes the folder
  let now = NOW, answer = { kind: 'value', value: { ok: true, sessionId: SID, tileId: 'tile-1', pid: 4242, window: 0, primary: true, cells: [6], newWindow: false, how: 'opened' } };
  let opened = [];
  const box = new D.DeskOpenInbox({ dir, now: () => now, open: (req) => {
    opened.push(plain(req));
    if (answer.kind === 'throw') throw new Error(answer.message);
    if (answer.kind === 'reenter') return { ok: true, how: 'opened', pid: box.drain().length + 100 };   // a drain inside open(): the pass under way answers [] to it
    return answer.value;
  } });
  const stamp = new Map();                                  // name -> the text and mtime last seen
  const snapshot = (stampNew) => {
    const files = [];
    let names = []; try { names = fs.readdirSync(dir).sort(); } catch { names = []; }
    for (const n of names) {
      const f = path.join(dir, n);
      const body = fs.readFileSync(f);
      const key = body.toString('hex');
      const seen = stamp.get(n);
      if (stampNew && (!seen || seen.key !== key || seen.mtime !== fs.statSync(f).mtimeMs)) fs.utimesSync(f, new Date(now), new Date(now));   // written (or written again) by this drain
      const mtime = fs.statSync(f).mtimeMs;
      stamp.set(n, { key, mtime });
      files.push({ name: n, text: body.toString('utf8'), mtime });
    }
    for (const n of [...stamp.keys()]) if (!names.includes(n)) stamp.delete(n);
    return files;
  };
  const steps = [];
  for (const s of script) {
    const f = s.name ? path.join(dir, s.name) : null;
    if (s.op === 'now') { now = s.t; steps.push(s); }
    else if (s.op === 'answer') { answer = s; steps.push(s); }
    else if (s.op === 'put') {
      const body = s.hex !== undefined ? Buffer.from(s.hex, 'hex') : Buffer.from(typeof s.body === 'string' ? s.body : JSON.stringify(s.body), 'utf8');
      const tmp = f + '.gen';
      fs.writeFileSync(tmp, body);                          // whole, as the on-call writes it: a temp file renamed in
      const at = s.mtime === undefined ? now : s.mtime;
      fs.utimesSync(tmp, new Date(at), new Date(at));
      fs.renameSync(tmp, f);
      steps.push({ op: 'put', name: s.name, hex: body.toString('hex'), mtime: fs.statSync(f).mtimeMs });
      stamp.set(s.name, { key: body.toString('hex'), mtime: fs.statSync(f).mtimeMs });
    }
    else if (s.op === 'rename') { fs.renameSync(f, path.join(dir, s.to)); const v = stamp.get(s.name); stamp.delete(s.name); if (v) stamp.set(s.to, v); steps.push(s); }
    else if (s.op === 'remove') { fs.unlinkSync(f); stamp.delete(s.name); steps.push(s); }
    else if (s.op === 'drain' || s.op === 'start') {
      opened = [];
      let out = null;
      if (s.op === 'start') { box.start(3600e3); box.stop(); } else out = plain(box.drain());
      steps.push({ op: s.op, out, opened, files: snapshot(true) });
    }
  }
  return { label, sub, now0: NOW, steps };
}
const P = (name, body, extra = {}) => ({ op: 'put', name, body, ...extra });
const req = id => `${id}.request.json`;
const HOUR = 3600e3;
const DRAIN = { op: 'drain' };
const inbox = [];
try {
  { const ID2 = 'web-20261116T200001Z-0002', ID3 = 'security-20261116T200002Z-0003', ID4 = 'security-20261116T200003Z-0004', ID5 = 'security-20261116T200004Z-0005', ID6 = 'security-20261116T200005Z-0006';
    inbox.push(inboxRun('the unit test\'s run', [
      P(req(ID), good()), P(req(ID) + '.tmp', 'half a file'), DRAIN, DRAIN,
      P(req(ID2), good({ id: ID2, desk: 'web', expiresAt: '2026-11-16T19:00:00.000Z' })), DRAIN,
      P(req(ID3), '{ not json'), DRAIN,
      { op: 'answer', kind: 'value', value: { ok: false, error: 'no deck account has the folder C:/x' } }, P(req(ID4), good({ id: ID4 })), DRAIN,
      { op: 'answer', kind: 'throw', message: 'pty failed' }, P(req(ID5), good({ id: ID5 })), DRAIN,
      { op: 'answer', kind: 'value', value: { ok: true, pid: 1, how: 'opened' } }, P(req(ID6), good({ id: ID6 })), { op: 'rename', name: req(ID6), to: `${ID6}.withdrawn.json` }, DRAIN,
      P('notes.txt', 'x'), P('security-20261115T100000Z-0007.ack.json', '{}', { mtime: NOW - 25 * HOUR }), { op: 'remove', name: `${ID6}.withdrawn.json` }, P(`${ID6}.withdrawn.json`, good({ id: ID6 }), { mtime: NOW - 25 * HOUR }), DRAIN,
    ])); }
  { const id = n => `security-20261116T2001${String(n).padStart(2, '0')}Z-00${String(n).padStart(2, '0')}`;
    const A = value => ({ op: 'answer', kind: 'value', value });
    let n = 0;
    const one = (value, body) => { const i = id(++n); return [A(value), P(req(i), body === undefined ? good({ id: i }) : typeof body === 'function' ? body(i) : body), DRAIN]; };
    inbox.push(inboxRun('what open() answers', [
      ...one({ ok: true, sessionId: SID, tileId: 'tile-1', pid: 4242, window: 0, primary: true, cells: [6], newWindow: false, how: 'opened' }),
      ...one({ ok: true }), ...one({ ok: true, sessionId: '', tileId: 0, pid: 0, window: 0, primary: 0, cells: null, newWindow: 0, how: '' }),
      ...one({ ok: true, sessionId: SID, tileId: 'tile-2', pid: 77, window: 3, primary: false, cells: [1], newWindow: true, how: 'opened', extra: 'not copied' }),
      ...one({ ok: true, sessionId: SID, tileId: 'tile-3', pid: 78, window: 2, primary: false, cells: [1, 2, 5, 6], newWindow: false, how: 'woken' }),
      ...one({ ok: true, sessionId: SID, tileId: 'tile-4', pid: 79, window: null, primary: 'yes', cells: [], newWindow: 'yes', how: 'typed' }),
      ...one({ ok: true, tileId: 'tile-5', pid: 80, primary: true, cells: [3] }), ...one({ ok: 1, tileId: 'tile-6', window: 'w', cells: ['c'], pid: 'pid', how: 5 }), ...one({ ok: 'yes', window: 1.5, pid: 2.5, cells: [1.5, 'a', null] }),
      ...one({ ok: false }), ...one({ ok: false, error: '' }), ...one({ ok: false, error: 'the deck has no window open' }), ...one({ ok: 0, error: 'desk requests are switched off in the deck (settings.json deskOpen)' }), ...one({ error: 'no ok at all' }), ...one({}),
      ...one(null), ...one(false), ...one(0), ...one(''), ...one('a string'), ...one(true), ...one(7), ...one([]), ...one([{ ok: true }]), ...one({ ok: false, error: { a: 1, why: 'an object' } }), ...one({ ok: false, error: 'quotes " and \\ and \u65e5\u672c \ud83d\ude00 and \n\t\u0001' }),
      ...one({ ok: true, tileId: 'unicode \u65e5\u672c "q"', how: 'opened', sessionId: SID.toUpperCase(), pid: 123456789012, window: -1, cells: [[1], [2, 3]] }),
      { op: 'answer', kind: 'throw', message: 'x'.repeat(400) }, P(req(id(++n)), good({ id: id(n) })), DRAIN,
      { op: 'answer', kind: 'throw', message: '' }, P(req(id(++n)), good({ id: id(n) })), DRAIN,
      { op: 'answer', kind: 'throw', message: '\u00e9'.repeat(298) + '\ud83d\ude00' + 'tail' }, P(req(id(++n)), good({ id: id(n) })), DRAIN,
      { op: 'answer', kind: 'throw', message: 'the session did not start (spawn "claude" ENOENT)\n\u65e5\u672c' }, P(req(id(++n)), good({ id: id(n) })), DRAIN,
      { op: 'answer', kind: 'reenter' }, P(req(id(++n)), good({ id: id(n) })), P(req(id(++n)), good({ id: id(n) })), DRAIN,
    ])); }
  { const id = n => `web-20261116T2002${String(n).padStart(2, '0')}Z-0a${String(n).padStart(2, '0')}`;
    let n = 0;
    const one = body => { const i = id(++n); return [P(req(i), typeof body === 'function' ? body(i) : body), DRAIN]; };
    const g = (o) => i => good({ id: i, desk: 'web', title: 'web desk', ...o });
    inbox.push(inboxRun('what a request holds', [
      ...one(g({})), ...one(g({ accountName: '' })), ...one(g({ accountName: undefined, account: '  C:/Users/tester/.claude-vv  ' })), ...one(g({ desk: 'security' })), ...one(g({ desk: 'planner' })), ...one(g({ desk: { z: 1, a: [2, 1.5], m: null } })), ...one(g({ desk: 0 })), ...one(g({ desk: '' })),
      ...one(g({ kind: 'other' })), ...one(g({ kind: { z: 1, a: 2 }, desk: ['web', 1] })), ...one(i => good({ id: i.replace('web', 'xyz'), desk: 'web' })), ...one(g({ sessionId: 'short' })), ...one(g({ account: ' ' })), ...one(g({ cwd: '' })), ...one(g({ prompt: ' \r\n ' })), ...one(g({ prompt: 'x'.repeat(24001) })),
      ...one(g({ expiresAt: '2026-11-16T20:00:29.999Z' })), ...one(g({ expiresAt: '2026-11-16T20:00:30.000Z' })), ...one(g({ expiresAt: 'never' })), ...one(g({ expiresAt: undefined })),
      ...one('5'), ...one('"a string"'), ...one('[1,2]'), ...one('null'), ...one('true'), ...one(''), ...one('   '), ...one('{}'), ...one('{"id":'), ...one('{"desk":"web","kind":"desk-open"} trailing'), ...one('["desk"]'),
      ...one(i => '\ufeff' + JSON.stringify(good({ id: i, desk: 'web' }))), ...one(i => '\ufeff\ufeff' + JSON.stringify(good({ id: i, desk: 'web' }))), ...one(i => ' \r\n\t' + JSON.stringify(good({ id: i, desk: 'web' }), null, 2) + '\r\n'),
      ...one(i => '{"desk":"security","id":"' + i + '","desk":"web","kind":"nope","desk":{"b":1,"a":2,"10":3,"2":4}}'),
      ...one(i => JSON.stringify(good({ id: i, desk: 'web', title: 'un t\u00edtulo \u65e5\u672c \ud83d\ude00', accountName: '\u00e9 "quoted" \\ name', prompt: 'l\u00ednea\r\nuno \ud83d\ude00' }))),
    ]));
    // a request that is not UTF-8, and one with a NUL: read as text, then refused or opened by what it parses to
    const i1 = id(60), i2 = id(61);
    inbox.push(inboxRun('bytes', [
      { op: 'put', name: req(i1), hex: Buffer.concat([Buffer.from('{"id":"' + i1 + '","kind":"desk-open","desk":"wd', 'utf8'), Buffer.from([0xff, 0xfe]), Buffer.from('b"}', 'utf8')]).toString('hex') }, DRAIN,
      { op: 'put', name: req(i2), hex: (() => { const [x, y] = JSON.stringify(good({ id: i2, desk: 'web', accountName: 'bad bytes @@' })).split('@@'); return Buffer.concat([Buffer.from(x, 'utf8'), Buffer.from([0xc3, 0x28]), Buffer.from(y, 'utf8')]).toString('hex'); })() }, DRAIN,
    ])); }
  { const a = 'security-20261116T200301Z-00a1', b = 'security-20261116T200302Z-00a2', c = 'web-20261116T200300Z-00a3', up = 'WEB-20261116T200305Z-ABCD', lowT = 'web-20261116t200306z-00a6';
    inbox.push(inboxRun('several at once; names that are not requests', [
      P(req(b), good({ id: b })), P(req(c), good({ id: c, desk: 'web', expiresAt: '2026-11-16T19:00:00Z' })), P(req(a), good({ id: a })),
      P('x.request.json', good()), P('security-20261116T200301Z-00a.request.json', good()), P(req(a) + '.tmp', 'tmp'), P(a + '.request.json.bak', 'bak'), P(a + '.request.jsonx', 'x'), P('request.json', '{}'), P('.request.json', '{}'), P(a + '.REQUEST.JSON.txt', 'x'), P('log.txt', 'x'),
      DRAIN, DRAIN,
      P(req(up), good({ id: up, desk: 'web' })), DRAIN, P(req(lowT), good({ id: lowT, desk: 'web' })), DRAIN,
      P(req(a), good({ id: a })), DRAIN,                    // the same id again: its answer is replaced
    ])); }
  { const id = n => `security-20261116T2004${String(n).padStart(2, '0')}Z-0b${String(n).padStart(2, '0')}`;
    const DAY = 24 * HOUR;
    inbox.push(inboxRun('pruned after a day', [
      P(id(1) + '.ack.json', '{"old":1}', { mtime: NOW - DAY - 1000 }), P(id(2) + '.ack.json', '{"edge":1}', { mtime: NOW - DAY }), P(id(3) + '.ack.json', '{"fresh":1}', { mtime: NOW - DAY + 1000 }), P(id(4) + '.ack.json', '{"future":1}', { mtime: NOW + DAY }),
      P(id(5) + '.withdrawn.json', good({ id: id(5) }), { mtime: NOW - 2 * DAY }), P(id(6) + '.withdrawn.json', good({ id: id(6) }), { mtime: NOW - HOUR }), P(id(7) + '.taken.json', good({ id: id(7) }), { mtime: NOW - 3 * DAY }), P(id(8) + '.taken.json', good({ id: id(8) }), { mtime: NOW - 60000 }),
      P('old notes.txt', 'x', { mtime: NOW - 9 * DAY }), P(id(9) + '.request.json.tmp', 'x', { mtime: NOW - 9 * DAY }), P('x.ack.json', 'not an id, still an answer', { mtime: NOW - 9 * DAY }), P('y.ack.json.bak', 'kept', { mtime: NOW - 9 * DAY }), P('ack.json', 'kept', { mtime: NOW - 9 * DAY }), P('.taken.json', 'pruned', { mtime: NOW - 9 * DAY }),
      P('log.jsonl', '{"at":"earlier","id":"kept as it is"}\n', { mtime: NOW - 9 * DAY }), P(id(10) + '.request.json', good({ id: id(10) }), { mtime: NOW - 9 * DAY }),
      DRAIN, DRAIN,
      { op: 'now', t: NOW + DAY }, DRAIN,                    // exactly a day on: what the first drain wrote is not yet older than a day
      { op: 'now', t: NOW + DAY + 1 }, P(req(id(11)), good({ id: id(11), expiresAt: '2026-11-18T00:00:00Z' })), DRAIN,
      { op: 'now', t: NOW + 2 * DAY + 2 }, DRAIN, DRAIN,
    ])); }
  inbox.push(inboxRun('start(): the folder is made, what waited is drained', [{ op: 'start' }, P(req(ID), good()), { op: 'start' }, DRAIN], path.join('made', 'desk-open')));
} finally { fs.rmSync(tempRoot, { recursive: true, force: true }); }

const out = { note: 'generated by core/test/deck/gen-deskopen.js from lib/deskopen.js', node: process.version, platform: process.platform,
  pinned: { now: NOW, nowIso: new Date(NOW).toISOString(), tz: 'none read: every date carries its zone', mtimes: 'each file the inbox wrote is stamped with the pinned now after the drain' },
  consts: { DIR: D.DIR, PROMPT_MAX: D.PROMPT_MAX, files: D.files('ID') },
  ids, spots, requests, acctCases, plans, words, inbox };
const file = path.join(__dirname, '..', 'fixtures', 'deck', 'deskopen.json');
fs.writeFileSync(file, JSON.stringify(out, null, 1) + '\n');
console.log(`deskopen.json: ${ids.length} ids, ${spots.length} spots, ${requests.length} requests, ${acctCases.length} accounts, ${plans.length} plans, ${words.length} words, ${inbox.length} inbox runs (${inbox.reduce((n, r) => n + r.steps.filter(s => s.op === 'drain' || s.op === 'start').length, 0)} drains), ${fs.statSync(file).size} bytes`);
