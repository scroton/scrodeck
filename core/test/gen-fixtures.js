'use strict';
// Generate language-neutral acceptance fixtures for the native core from the CANONICAL JS model modules.
// These JSON files are the oracle the C++ core and the Swift/Windows shells must reproduce exactly: same
// input, same output. The JS modules are the source of truth (they carry the unit tests), so generating
// the fixtures FROM them keeps the two in lockstep — regenerate after any model change.
//
//   node core/test/gen-fixtures.js
//
// The native test harness loads these and checks its own port against `out`. Nothing here is Electron.
const fs = require('fs');
const path = require('path');
const L = require(path.join(__dirname, '..', '..', 'lib', 'layout.js'));
const H = require(path.join(__dirname, '..', '..', 'lib', 'hibernate.js'));
const Q = require(path.join(__dirname, '..', '..', 'lib', 'queue.js'));

const FX = path.join(__dirname, 'fixtures');
function writeModule(module, source, cases) {
  const f = path.join(FX, module + '.json');
  fs.writeFileSync(f, JSON.stringify({ module, generatedFrom: source, cases }, null, 2) + '\n');
  console.log(`wrote ${cases.length} ${module} fixtures -> ${path.relative(process.cwd(), f)}`);
}

// ---------------------------------------------------------------- layout
(() => {
  const cases = [];
  const add = (fn, args, note) => cases.push({ fn, args, out: L[fn](...args), note: note || '' });
  const g22 = { rows: 2, cols: 2 }, g33 = { rows: 3, cols: 3 }, g13 = { rows: 1, cols: 3 }, g23 = { rows: 2, cols: 3 };
  const g44 = { rows: 4, cols: 4 }, g34 = { rows: 3, cols: 4 };
  add('fullSet', [g22]); add('fullSet', [g33]); add('fullSet', [g13]);
  add('isLegal', [[1, 2, 4, 5], g33], 'a 2x2 block'); add('isLegal', [[1, 3], g33], 'gap at 2');
  add('legalize', [[1, 4], g22], 'R1: diagonal -> full 2x2');
  add('legalize', [[1, 9], g33], 'opposite corners -> whole grid');
  add('legalize', [[1, 5], g33], 'top-left 2x2 block');
  add('legalize', [[4, 6], g33], 'a middle row');
  add('rect', [[1], g22]); add('rect', [[1, 2], g22]); add('rect', [[5], g33], 'centre cell');
  add('rect', [[3], g33], 'top-right'); add('rect', [[6], g23], 'bottom-right of a 2x3');
  add('place', [{ a: [1, 2, 3, 4] }, 'b', [1], g22], 'R3: full 2x2 losing {1} -> bottom half');
  add('place', [{ a: [1, 2, 3, 4] }, 'b', [3], g22], 'R3: losing {3} -> top half');
  add('place', [{ a: [1] }, 'b', [1, 2], g22], 'R6: covered tile evicted');
  add('place', [{ a: L.fullSet(g33) }, 'b', [5], g33], '3x3: full loses the centre cell');
  add('place', [{ a: [1, 2, 4, 5], b: [3] }, 'c', [2, 5], g33], 'partial overlap shrinks, disjoint kept');
  add('shrink', [L.fullSet(g33), [5], g33], 'largest strip clear of the centre');
  add('dropTarget', [{ a: [1, 2, 3, 4] }, 0.5, 0.1, g22], 'R15: 2x2 full, top triangle -> top half');
  add('dropTarget', [{ a: [1, 2, 3, 4] }, 0.5, 0.5, g22], 'R15: 2x2 full, centre -> whole');
  add('dropTarget', [{ a: [1, 2] }, 0.1, 0.25, g22], 'R15: 2x2 top half, left -> {1}');
  add('directionalHalf', [[1, 3], 0.5, 0.1], 'R15: tall half, top -> {1}');
  add('directionalHalf', [[1, 2, 3, 4], 0.05, 0.05], 'R15: full pane top-left corner -> {1}');
  add('directionalHalf', [[1, 2, 3, 4], 0.95, 0.95], 'R15: full pane bottom-right corner -> {4}');
  add('directionalHalf', [[1, 2], 0.45, 0.06], 'a half: mid-axis near the edge is a split, not whole (centre is a square)');
  add('directionalHalf', [[1, 3], 0.5, 0.5], 'a half: the centre square is whole');
  add('resolveDrop', [{ y: [1] }, 'y', [2], g22], 'placed tile onto a neighbour -> grows into the rectangle');
  add('resolveDrop', [{ y: [1] }, 'y', [4], g22], 'placed tile onto the diagonal -> moves');
  add('resolveDrop', [{ y: [1, 2, 3, 4] }, 'y', [3, 4], g22], 'placed tile onto part of itself -> shrinks');
  add('resolveDrop', [{ x: [1, 2] }, 'y', [3], g22], 'unplaced tile -> the set as is');
  add('regrid', [{ a: [1], b: [2], c: [3, 4] }, g22, g33], 'grid change 2x2 -> 3x3: blocks keep rows/cols, cells renumbered');
  add('regrid', [{ a: [1], b: [3], c: [4, 5], d: [9] }, g33, g22], 'grid change 3x3 -> 2x2: blocks that no longer fit are dropped');
  add('dropTarget', [{ a: [1], b: [2], c: [3, 4] }, 0.5, 0.5, g22], 'junction: the centre crossing of a split 2x2 -> whole');
  add('dropTarget', [{ a: [1], b: [2], c: [3, 4] }, 0.5, 0.04, g22], 'junction: top border between two quadrants -> the pair');
  add('dropTarget', [{ a: L.fullSet(g33) }, 2 / 3, 2 / 3, g33], 'junction: 3x3 crossing -> the 2x2 block {5,6,8,9}');
  add('junctionAt', [0.02, 0.02, g22], 'an outer corner is not a junction -> null');
  add('dropTarget', [{ a: L.fullSet(g33) }, 0.5, 0.5, g33], 'R16: 3x3 -> the cell under the cursor (5)');
  add('dropTarget', [{ a: L.fullSet(g33) }, 0.9, 0.1, g33], 'R16: 3x3 top-right -> cell 3');
  add('dropTarget', [{}, 0.5, 0.5, g33], 'empty layout -> whole grid');
  add('cellAt', [0.9, 0.5, g13], '1x3 right third -> cell 3');
  add('norm', [[4, 1, 4, 9, 0, 2], g22], 'drops out-of-range, dedupes, sorts');
  add('norm', [[1, 5, 9], g33], 'all valid on 3x3');
  add('dims', [{ rows: 0, cols: 9 }], 'clamps to 1..4');
  add('dims', [{ rows: 5, cols: 5 }], '4x4 is the ceiling');
  add('fullSet', [g44]); add('fullSet', [g34]);
  add('legalize', [[6, 11], g44], '4x4: the centre 2x2 block');
  add('legalize', [[1, 16], g44], '4x4: opposite corners -> whole grid');
  add('rect', [[16], g44], '4x4: bottom-right cell'); add('rect', [[12], g34], '3x4: bottom-right cell');
  add('place', [{ a: L.fullSet(g44) }, 'b', [6, 7, 10, 11], g44], '4x4: full loses the centre block -> top row');
  add('dropTarget', [{ a: L.fullSet(g44) }, 0.5, 0.5, g44], '4x4: centre crossing -> the four cells meeting there');
  add('dropTarget', [{ a: L.fullSet(g44) }, 0.9, 0.9, g44], '4x4: the cell under the cursor (16)');
  add('regrid', [{ a: [1], b: [5, 6, 8, 9] }, g33, g44], 'grid change 3x3 -> 4x4: blocks keep rows/cols');
  add('regrid', [{ a: [1], b: [6, 7, 10, 11], d: [16] }, g44, g33], 'grid change 4x4 -> 3x3: what no longer fits drops');
  add('norm', [[9, 10, 16, 17], g44], '4x4: 10..16 valid, 17 not');
  add('prune', [{ a: [1], gone: [2], bad: [1, 3] }, ['a', 'bad'], g33], 'drop dead + illegal');
  add('occupancy', [{ a: [1, 2] }, 'a', g22]);
  add('same', [[2, 1], [1, 2], g22], 'order-insensitive');
  writeModule('layout', 'lib/layout.js', cases);
})();

// ---------------------------------------------------------------- hibernate
// The policy is pure over a plain tile "view"; every function takes the view(s), now, and a config.
(() => {
  const cases = [];
  const NOW = 1_700_000_000_000, MIN = 60_000;
  const on = o => H.config({ enabled: true, ...(o || {}) });
  const tile = (over = {}) => ({ id: 'a', kind: 'claude', sessionId: 's1', resumable: true, status: 'idle', running: true, asleep: false, pinned: false, shownBy: [], held: 0, lastActivityAt: NOW - 120 * MIN, ...over });
  const add = (fn, args, note) => cases.push({ fn, args, out: H[fn](...args), note: note || '' });

  add('config', [{}], 'defaults (ships OFF, 30 min)');
  add('config', [{ rows: 9 }], 'unknown keys ignored, defaults kept');   // config ignores non-keys
  add('sleepBlocker', [tile(), NOW, on({ idleMinutes: 60 })], 'a quiet idle session may sleep -> null');
  add('sleepBlocker', [tile({ status: 'busy' }), NOW, on()], 'busy -> blocked');
  add('sleepBlocker', [tile({ pinned: true }), NOW, on()], 'pinned -> blocked');
  add('sleepBlocker', [tile({ shownBy: ['win:0'] }), NOW, on()], 'on screen -> blocked');
  add('sleepBlocker', [tile({ held: 1 }), NOW, on()], 'holding a prompt -> blocked');
  add('sleepBlocker', [tile({ resumable: false }), NOW, on()], 'never prompted (nothing to resume) -> blocked');
  add('sleepBlocker', [tile({ lastActivityAt: NOW - 10 * MIN }), NOW, on({ idleMinutes: 60 })], 'active recently -> blocked');
  add('maySleep', [tile(), NOW, on({ idleMinutes: 60 })], 'true');
  add('wakeReason', [tile({ asleep: true, shownBy: ['win:1'] })], 'on screen -> wakes');
  add('wakeReason', [tile({ asleep: true, held: 2 })], 'a due prompt -> wakes');
  add('wakeReason', [tile({ asleep: true })], 'nobody wants it -> null');
  const fleet = [
    tile({ id: 'q3', lastActivityAt: NOW - 180 * MIN }),
    tile({ id: 'q4', lastActivityAt: NOW - 240 * MIN }),
    tile({ id: 'busy', status: 'busy' }),
  ];
  add('chooseSleepers', [fleet, NOW, on({ idleMinutes: 60, maxPerTick: 1 })], 'longest-quiet first, capped');
  add('chooseWakers', [[tile({ id: 'w', asleep: true, shownBy: ['win:0'] }), tile({ id: 'x', asleep: true })]], 'only the wanted one');
  writeModule('hibernate', 'lib/hibernate.js', cases);
})();

// ---------------------------------------------------------------- prompts (the portable due-logic of lib/queue.js)
// deliver() is shell I/O; the portable heart is inWindow / minutesOfDay and the per-kind due gating. The
// gating is a method (dueReason) on HeldPrompts, so drive it through a tiny fake deck to capture vectors.
(() => {
  const cases = [];
  const add = (fn, args, out, note) => cases.push({ fn, args, out, note: note || '' });
  // window helpers (pure functions, exported)
  add('inWindow', [600, 540, 660], Q.inWindow(600, 540, 660), 'inside a normal window');
  add('inWindow', [700, 540, 660], Q.inWindow(700, 540, 660), 'outside');
  add('inWindow', [30, 1380, 120], Q.inWindow(30, 1380, 120), 'wraps past midnight (inside)');
  add('inWindow', [300, 1380, 120], Q.inWindow(300, 1380, 120), 'wraps past midnight (outside)');
  add('inWindow', [0, 500, 500], Q.inWindow(0, 500, 500), 'zero-length window = any time');

  // dueReason per kind, via a fake deck (no I/O). Encodes the item + a trigger edge time + now.
  const os = require('os');
  const mk = () => new Q.HeldPrompts({ file: path.join(os.tmpdir(), 'fx-' + Math.random() + '.json'), tileById: () => null, statusOf: () => 'idle', recentOutput: () => '', write: () => {}, triggerRisenAt: n => (n === 'ready' ? 5000 : 0), confirmMs: 0 });
  const hp = mk();
  const T = 10000;
  const dr = (item) => hp.dueReason(item, T);
  add('dueReason', [{ kind: 'timed', at: T - 1 }, T], dr({ kind: 'timed', at: T - 1 }), 'timed at/after its time -> due');
  add('dueReason', [{ kind: 'timed', at: T + 1 }, T], dr({ kind: 'timed', at: T + 1 }), 'timed before its time -> not due');
  add('dueReason', [{ kind: 'next', at: 0 }, T], dr({ kind: 'next', at: 0 }), 'next -> due');
  add('dueReason', [{ kind: 'recurring', everyMin: 10, startMin: 0, endMin: 0, lastSentAt: 0 }, T], dr({ kind: 'recurring', everyMin: 10, startMin: 0, endMin: 0, lastSentAt: 0 }), 'recurring any-time, never sent -> due');
  add('dueReason', [{ kind: 'recurring', everyMin: 10, startMin: 0, endMin: 0, lastSentAt: T - 60000 }, T], dr({ kind: 'recurring', everyMin: 10, startMin: 0, endMin: 0, lastSentAt: T - 60000 }), 'recurring within the interval -> not due');
  add('dueReason', [{ kind: 'triggered', trigger: 'ready', firedAt: 0 }, T], dr({ kind: 'triggered', trigger: 'ready', firedAt: 0 }), 'triggered, trigger risen, not yet fired -> due');
  add('dueReason', [{ kind: 'triggered', trigger: 'ready', firedAt: 5000 }, T], dr({ kind: 'triggered', trigger: 'ready', firedAt: 5000 }), 'triggered, already fired for this edge -> not due');
  add('dueReason', [{ kind: 'triggered', trigger: 'cold', firedAt: 0 }, T], dr({ kind: 'triggered', trigger: 'cold', firedAt: 0 }), 'triggered, trigger false -> not due');
  writeModule('prompts', 'lib/queue.js', cases);
})();

// triggers is a stateful class (a store + a rising-edge rule); its behaviour is covered by
// test/special-prompts-unit.js. A fixture vector set for a class needs a scripted sequence rather than
// pure input->output; the port note in PORTING.md points the implementer there rather than pretending a
// flat fixture captures it.
console.log('done');
