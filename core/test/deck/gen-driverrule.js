'use strict';
// Generate core/test/fixtures/deck/driverrule.json from lib/driverrule.js — who drives a session's size, as a
// claim by one kind of screen (the design proposal of 2026-11-18). The module is on the deck's review branch
// today and on main if the owner takes it, so its path is an ARGUMENT:
//   node core/test/deck/gen-driverrule.js C:\Users\demo\Documents\GitHub\_wt-deck-remote-rule\lib\driverrule.js
//   node core/test/deck/gen-driverrule.js lib/driverrule.js                  (once main has it)
// It requires that file (pure) and, when lib/phone.js sits beside it and loads (it needs the `ws` package), that
// one's PhoneServer.prototype.deskMeta — what a remote desk is told under the claim and the remote-desk rule.
//   table     the branch's test/driverrule-unit.js table: every driver value x who is looking, with everything
//             the module answers for it (the C++ test runs that file's checks over it, one for one)
//   sweep     the same for values no table row has ('desk', junk, null, a missing `who`, no pty size)
//   words     mode() over old words, new words and junk;  button: click / look for every kind x value
//   deskMeta  lib/phone.js deskMeta over a meta per driver value x what the desk shows x where a phone is
const fs = require('fs');
const path = require('path');
const file = process.argv[2];
if (!file || !fs.existsSync(file)) { console.error('usage: node core/test/deck/gen-driverrule.js <path to lib/driverrule.js>'); process.exit(2); }
const D = require(path.resolve(file));
const PTY = { cols: 111, rows: 33 };

function row(m, who, pty) {
  return {
    m, who, pty,
    mode: D.mode(m),
    driver: D.driverOf(m, who === null ? undefined : who),
    accepts: D.KINDS.filter(k => D.acceptsSize(m, who === null ? undefined : who, k)),
    officeMirror: D.officeMirror(m, who === null ? undefined : who, pty),
    remoteMirror: D.remoteMirror(m, who, pty),
    phoneMirrors: D.phoneMirrors(m),
    locked: Object.fromEntries(D.KINDS.map(k => [k, D.inputLocked(m, who === null ? undefined : who, k)])),
    reason: D.refusedReason(m, who === null ? undefined : who),
  };
}
const table = [];
for (const m of D.MODES) for (const phone of [false, true]) for (const remote of [false, true]) table.push(row(m, { phone, remote }, PTY));
const sweep = [];
for (const m of [...D.MODES, 'desk', 'nonsense', 'Desk', '', null, 7, true, {}, []]) {
  for (const who of [null, {}, { phone: true }, { remote: true }, { phone: false, remote: false }, { phone: true, remote: false }, { phone: false, remote: true }, { phone: true, remote: true }]) {
    for (const pty of [PTY, null, { cols: 80, rows: 24 }]) sweep.push(row(m, who, pty));
  }
}
const words = ['auto', 'office', 'phone', 'remote', 'desk', null, '', 'Desk', 'away', 7, {}, [], true, false, 'AUTO', ' office'].map(v => ({ in: v, out: D.mode(v) }));
const button = [];
for (const me of D.KINDS) for (const m of [...D.MODES, 'desk', 'nonsense', null]) button.push({ me, m, click: D.click(m, me), look: D.look(m, me) });

// ---- lib/phone.js deskMeta, when it loads
let deskMeta = null, phoneNote = 'lib/phone.js was not loaded';
try {
  const { PhoneServer } = require(path.join(path.dirname(path.resolve(file)), 'phone.js'));
  const tiles = () => [
    { id: 'a', inWindow: 0, shownBy: ['win:0'], ptySize: { cols: 100, rows: 30 }, mirror: null, sizeBy: 'office' },
    { id: 'b', inWindow: null, shownBy: [], ptySize: { cols: 80, rows: 24 }, mirror: { cols: 80, rows: 24 }, sizeBy: 'phone' },
    { id: 'c', inWindow: 1, shownBy: ['win:1', 'remote'], ptySize: null, mirror: null },
    { id: 'd', inWindow: 2, shownBy: [], mirror: { cols: 1, rows: 1 } },
  ];
  deskMeta = [];
  const metas = [];
  for (const driver of ['auto', 'office', 'phone', 'remote', 'desk', undefined]) metas.push({ now: 1, settings: driver === undefined ? { msgLines: {} } : { driver, msgLines: {} }, tiles: tiles(), remoteDesk: { name: null, kind: 'browser' } });
  metas.push({ now: 1, tiles: tiles() });                 // no settings at all
  for (const meta of metas) {
    for (const [shown, hidden, phoneHas] of [[['a'], false, []], [['a', 'b', 'c', 'd'], false, []], [['a', 'b'], true, []], [['a', 'b'], false, ['a']], [[], false, ['b']], [['c', 'zz'], false, ['c']], [['a', 'b', 'c', 'd'], false, ['a', 'b', 'c', 'd']]]) {
      const ws = { hidden, shown: new Set(shown) };
      const self = { phoneWatching: id => phoneHas.includes(id) };
      deskMeta.push({ meta, shown, hidden, phoneHas, out: PhoneServer.prototype.deskMeta.call(self, ws, meta) });
    }
  }
  deskMeta.push({ meta: { now: 1 }, shown: [], hidden: false, phoneHas: [], out: PhoneServer.prototype.deskMeta.call({ phoneWatching: () => false }, { hidden: false, shown: new Set() }, { now: 1 }) });
  phoneNote = 'lib/phone.js deskMeta';
} catch (e) { phoneNote = 'lib/phone.js did not load: ' + String(e && e.message || e).split('\n')[0]; }

const out = path.join(__dirname, '..', 'fixtures', 'deck', 'driverrule.json');
fs.writeFileSync(out, JSON.stringify({ source: path.basename(path.dirname(path.dirname(path.resolve(file)))) + '/lib/driverrule.js', MODES: D.MODES, KINDS: D.KINDS, PTY, table, sweep, words, button, deskMeta, phoneNote }) + '\n');
console.log(`wrote driverrule -> ${out} (${fs.statSync(out).size} bytes; table ${table.length}, sweep ${sweep.length}, words ${words.length}, button ${button.length}, deskMeta ${deskMeta ? deskMeta.length : 0}: ${phoneNote})`);
