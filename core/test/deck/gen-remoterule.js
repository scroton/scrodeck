'use strict';
// Generate core/test/fixtures/deck/remoterule.json from lib/remoterule.js — where a remote desk stops (the
// remote-desk rule of 2026-11-18). The module is on the deck's review branch today and on main after it merges, so its path
// is an ARGUMENT:
//   node core/test/deck/gen-remoterule.js C:\Users\demo\Documents\GitHub\_wt-deck-remote-rule\lib\remoterule.js
//   node core/test/deck/gen-remoterule.js lib/remoterule.js                  (once main has it)
// It only requires that file (pure). What it writes:
//   unit       every case of the branch's test/remoterule-unit.js that is the module's own (its layout sequence,
//              its shownBy rows, its wakesForRemote rows), as calls with what the JS answered
//   sweep      every combination of a small world (two office windows + one remote desk), so the C++ port is
//              compared on cases nobody thought of
// An entry in the file is { index, remote, layout: [ids], shown: [ids], hidden, visible }.
const fs = require('fs');
const path = require('path');
const file = process.argv[2];
if (!file || !fs.existsSync(file)) { console.error('usage: node core/test/deck/gen-remoterule.js <path to lib/remoterule.js>'); process.exit(2); }
const R = require(path.resolve(file));

const visible = w => !!w.visible;
// the fixture's entry -> the JS's (a window has `win`, a remote desk a `remote` socket)
const live = e => e.remote
  ? { index: e.index, win: null, remote: {}, layout: Object.fromEntries(e.layout.map(id => [id, [1]])), shown: [...e.shown], hidden: !!e.hidden, visible: !!e.visible }
  : { index: e.index, win: {}, layout: Object.fromEntries(e.layout.map(id => [id, [1]])), shown: [...e.shown], hidden: !!e.hidden, visible: !!e.visible };
const E = (index, remote, layout = [], shown = [], extra = {}) => ({ index, remote, layout, shown, hidden: false, visible: true, ...extra });
const lay = ids => Object.fromEntries(ids.map(id => [id, [1]]));

function evict(taker, layout, entries, oneWindow) {
  const ls = entries.map(live);
  const t = typeof taker === 'number' ? ls[taker] : live(taker);
  return R.evictions(t, lay(layout), ls, oneWindow).map(x => ({ entry: ls.indexOf(x.entry), ids: x.ids }));
}
function holder(id, entries) { const ls = entries.map(live); const h = R.holder(id, ls); return h ? ls.indexOf(h) : -1; }
function shown(id, entries, phone) {
  const ls = entries.map(live);
  return { office: R.officeShownBy(id, ls, visible), shows: R.officeShows(id, ls, visible), out: R.shownBy(id, ls, visible, phone) };
}
function wakes(prev, next, asleep) { return R.wakesForRemote(prev, next, id => asleep.includes(id)); }

// ---------------------------------------------------------------- the unit file's cases
// main.js's takeLayout with the rule doing the deciding, as test/remoterule-unit.js's `take`: a sequence of takes
// on one registry; each step records what was evicted and every layout afterwards.
function sequence(name, entries, steps, oneWindow = true) {
  const state = entries.map(e => ({ ...e, layout: [...e.layout] }));
  const out = [];
  for (const [taker, layout] of steps) {
    state[taker].layout = [...layout];
    const ev = evict(taker, layout, state, oneWindow);
    for (const { entry, ids } of ev) state[entry].layout = state[entry].layout.filter(id => !ids.includes(id));
    out.push({ taker, layout, evicted: ev, layouts: state.map(e => [...e.layout]), holders: Object.fromEntries(['a', 'b', 'c', 'z'].map(id => [id, holder(id, state)])) });
  }
  return { name, entries, oneWindow, steps: out };
}
const unit = {
  sequences: [
    sequence('a desk places what office panes hold; an office window places what the desk holds', [E(0, false, ['a', 'b']), E(1, false, ['c']), E(2, true, [])],
      [[2, ['a', 'c']], [1, ['c', 'a']], [2, ['a', 'b', 'c']]]),
    sequence('one-window off: nothing is evicted anywhere', [E(0, false, ['a', 'b']), E(1, false, ['c']), E(2, true, ['a'])], [[0, ['a', 'c']], [2, ['c']], [1, ['a', 'b', 'c']]], false),
    sequence('two office windows trade a session while the desk keeps it', [E(0, false, ['a']), E(1, false, []), E(2, true, ['a'])], [[1, ['a']], [0, ['a']], [2, []], [1, ['a']]]),
  ],
  evictions: [
    { name: 'evictions handed the whole registry never names a remote either', taker: 0, layout: ['a', 'c'], entries: [E(0, false, ['b']), E(1, false, ['c', 'a']), E(2, true, ['a', 'b', 'c'])], oneWindow: true },
    { name: 'one-window off', taker: 0, layout: ['a', 'c'], entries: [E(0, false, ['b']), E(1, false, ['c', 'a']), E(2, true, ['a', 'b', 'c'])], oneWindow: false },
    { name: 'a taker that is not in the list (a window being made)', taker: E(7, false, []), layout: ['a', 'b'], entries: [E(0, false, ['b']), E(1, false, ['c', 'a']), E(2, true, ['a'])], oneWindow: true },
    { name: 'a remote taker that is not in the list', taker: E(7, true, []), layout: ['a', 'b'], entries: [E(0, false, ['b']), E(1, false, ['c', 'a'])], oneWindow: true },
    { name: 'no layout at all', taker: 0, layout: [], entries: [E(0, false, ['b']), E(1, false, ['c', 'a'])], oneWindow: true },
  ].map(c => ({ ...c, out: evict(c.taker, c.layout, c.entries, c.oneWindow) })),
  holders: [
    { name: 'the holder of a session is an office window, never the desk', entries: [E(0, false, ['b']), E(1, false, ['c', 'a']), E(2, true, ['a', 'b', 'c'])], ids: ['a', 'b', 'c', 'z'] },
    { name: 'a session only the desk has placed is held by no window', entries: [E(0, false, []), E(1, true, ['z'])], ids: ['z', 'a'] },
    { name: 'no entries', entries: [], ids: ['a'] },
  ].map(c => ({ ...c, out: c.ids.map(id => holder(id, c.entries)) })),
  shown: (() => {
    const all = [E(0, false, [], ['a']), E(1, false, [], ['b'], { visible: false }), E(2, true, [], ['a', 'b', 'c'])];
    const rows = [];
    for (const id of ['a', 'b', 'c', 'zz']) for (const phone of [false, true]) rows.push({ id, phone, entries: all, ...shown(id, all, phone) });
    const noDesk = all.slice(0, 2);
    for (const id of ['a', 'b', 'c']) rows.push({ id, phone: false, entries: noDesk, ...shown(id, noDesk, false) });
    return rows;
  })(),
  wakes: [
    ['a sleeping session coming onto the desk\'s screen is woken', { shown: [], hidden: false }, { shown: ['s1', 'x'], hidden: false }],
    ['an awake one is not touched', { shown: [], hidden: false }, { shown: ['x'], hidden: false }],
    ['one already in a pane is NOT woken when another pane changes', { shown: ['s1'], hidden: false }, { shown: ['s1', 'x'], hidden: false }],
    ['...but a second sleeping arrival is', { shown: ['s1'], hidden: false }, { shown: ['s1', 's2'], hidden: false }],
    ['a hidden tab wakes nothing', { shown: [], hidden: false }, { shown: ['s1'], hidden: true }],
    ['the tab coming back wakes what it shows', { shown: ['s1'], hidden: true }, { shown: ['s1'], hidden: false }],
    ['the first report of a new desk (no previous) wakes its sleeping panes', null, { shown: ['s2'], hidden: false }],
    ['no next at all', { shown: ['s1'], hidden: false }, null],
  ].map(([name, prev, next]) => ({ name, prev, next, asleep: ['s1', 's2'], out: wakes(prev, next, ['s1', 's2']) })),
};

// ---------------------------------------------------------------- the sweep
const subsets = ids => { const out = []; for (let m = 0; m < (1 << ids.length); m++) out.push(ids.filter((_, i) => m & (1 << i))); return out; };
const AB = subsets(['a', 'b']);
const sweep = { evictions: [], holders: [], shown: [], wakes: [] };
for (const l0 of AB) for (const l1 of AB) for (const l2 of AB) {
  const entries = [E(0, false, l0), E(1, false, l1), E(5, true, l2)];
  sweep.holders.push({ l: [l0, l1, l2], out: ['a', 'b', 'z'].map(id => holder(id, entries)) });
  for (let taker = 0; taker < 3; taker++) for (const take of AB) for (const oneWindow of [true, false]) {
    const es = entries.map((e, i) => i === taker ? { ...e, layout: take } : e);
    sweep.evictions.push({ l: [l0, l1, l2], taker, take, oneWindow, out: evict(taker, take, es, oneWindow) });
  }
}
for (const s0 of AB) for (const v0 of [true, false]) for (const s1 of AB) for (const v1 of [true, false]) for (const s2 of AB) for (const h2 of [false, true]) {
  const entries = [E(0, false, [], s0, { visible: v0 }), E(1, false, [], s1, { visible: v1 }), E(5, true, [], s2, { hidden: h2 })];
  for (const id of ['a', 'b']) for (const phone of [false, true]) { const r = shown(id, entries, phone); sweep.shown.push({ s: [s0, s1, s2], v: [v0, v1], h: h2, id, phone, office: r.office, shows: r.shows, out: r.out }); }
}
const SX = subsets(['s1', 's2', 'x']);
const reports = [null]; for (const s of SX) for (const hidden of [false, true]) reports.push({ shown: s, hidden });
for (const prev of reports) for (const next of reports) for (const asleep of [['s1', 's2'], ['x'], []]) sweep.wakes.push({ prev, next, asleep, out: wakes(prev, next, asleep) });

const out = path.join(__dirname, '..', 'fixtures', 'deck', 'remoterule.json');
fs.writeFileSync(out, JSON.stringify({ source: path.basename(path.dirname(path.dirname(path.resolve(file)))) + '/lib/remoterule.js', exports: Object.keys(R).sort(), unit, sweep }) + '\n');
console.log(`wrote remoterule -> ${out} (${fs.statSync(out).size} bytes; sweep: ${sweep.evictions.length} evictions, ${sweep.holders.length} holders, ${sweep.shown.length} shown, ${sweep.wakes.length} wakes)`);
