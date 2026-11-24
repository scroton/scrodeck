'use strict';
// Generate core/test/fixtures/deck/presence.json from lib/presence.js — the deck's contract with the sessions it
// hosts (deck-state.json). The module is on the deck's review branch today and on main after it merges, so its
// path is an ARGUMENT:
//   node core/test/deck/gen-presence.js C:\Users\demo\Documents\GitHub\_wt-deck-remote-rule\lib\presence.js
//   node core/test/deck/gen-presence.js lib/presence.js                      (once main has it)
// It only requires that file (pure: no Electron, no fs, no clock) and runs scripted sequences through it.
const fs = require('fs');
const path = require('path');
const file = process.argv[2];
if (!file || !fs.existsSync(file)) { console.error('usage: node core/test/deck/gen-presence.js <path to lib/presence.js>'); process.exit(2); }
const P = require(path.resolve(file));
const T0 = Date.parse('2026-11-18T22:00:00.000Z'), MIN = 60000;
const deck = { pid: 4242, startedAt: T0, version: '1.2.3' };

function run(name, opts, steps) {
  const p = new P.Presence({ deck: opts.deck || deck, now: opts.now ?? T0, awayMinutes: opts.awayMinutes, prev: opts.prev, pcName: opts.pcName });
  const out = [{ op: 'new', state: p.state(), text: p.text() }];
  for (const s of steps) {
    let changed;
    if (s.op === 'input') changed = p.input(s.kind, s.now);
    else if (s.op === 'connections') changed = p.connections({ remote: s.remote, phone: s.phone }, s.now);
    else if (s.op === 'tick') changed = p.tick(s.now);
    else if (s.op === 'away') changed = p.setAwayMinutes(s.value, s.now);
    out.push({ ...s, changed, state: p.state(), text: p.text() });
  }
  return { name, opts: { deck: opts.deck || deck, now: opts.now ?? T0, awayMinutes: opts.awayMinutes ?? null, prev: opts.prev ?? null, ...(opts.pcName ? { pcName: opts.pcName } : {}) }, steps: out };
}
const I = (kind, now) => ({ op: 'input', kind, now }), C = (remote, phone, now) => ({ op: 'connections', remote, phone, now }), K = now => ({ op: 'tick', now }), A = (value, now) => ({ op: 'away', value, now });
const scenarios = [
  // a remote desk on the deck's OWN PC (office check 2026-11-20): its name is this PC's, in any case
  run('remote on this PC', { pcName: 'demo-pc-01' }, [C({ name: 'demo-pc-01', kind: 'native' }, false, T0 + 1000), I('remote', T0 + 2000), C({ name: 'HOME-PC-2', kind: 'native' }, false, T0 + 3000), C({ name: 'demo-pc-01', kind: 'browser' }, false, T0 + 4000)]),
  run('input decides where', {}, [I('office', T0 + 1000), I('office', T0 + 2000), K(T0 + 60000), I('remote', T0 + 5 * MIN), I('remote', T0 + 5 * MIN + 500), I('phone', T0 + 6 * MIN), I('office', T0 + 7 * MIN), I('deck', T0 + 8 * MIN)]),
  run('what is connected', {}, [C({ name: 'laptop', kind: 'browser' }, false, T0 + 1000), C({ name: 'laptop', kind: 'browser' }, false, T0 + 3000), I('remote', T0 + 4000), C({ name: 'HOME-PC', kind: 'native' }, false, T0 + 5000),
    C({}, false, T0 + 6000), C({}, true, T0 + 7000), C(null, true, T0 + 8000), I('phone', T0 + 9000), C(null, false, T0 + 10000), C({ name: '  a\u0007 very long name that goes on and on past forty characters  ', kind: 'weird' }, false, T0 + 11000), C({ name: 7 }, false, T0 + 12000)]),
  run('away', {}, [K(T0 + 29 * MIN), K(T0 + 30 * MIN), K(T0 + 31 * MIN), C({ name: 'laptop', kind: 'native' }, true, T0 + 32 * MIN), I('remote', T0 + 40 * MIN), K(T0 + 69 * MIN), K(T0 + 70 * MIN), C(null, true, T0 + 71 * MIN), C(null, false, T0 + 72 * MIN),
    I('phone', T0 + 80 * MIN), K(T0 + 111 * MIN), I('office', T0 + 120 * MIN), K(T0 + 151 * MIN)]),
  run('the setting', { awayMinutes: 5 }, [K(T0 + 4 * MIN), K(T0 + 5 * MIN), A(5, T0 + 6 * MIN), A(0, T0 + 7 * MIN), K(T0 + 500 * MIN), A('2', T0 + 501 * MIN), A(99999, T0 + 502 * MIN), A('junk', T0 + 503 * MIN), A(null, T0 + 504 * MIN)]),
  run('never away', { awayMinutes: 0 }, [K(T0 + 5000 * MIN), I('phone', T0 + 5001 * MIN)]),
  run('a restart from a remote desk', { now: T0 + 10 * MIN, prev: { v: 1, where: 'remote', since: '2026-11-18T21:00:00.000Z', lastInputFrom: 'remote' } }, [K(T0 + 11 * MIN), C({ name: 'laptop' }, false, T0 + 12 * MIN), K(T0 + 41 * MIN)]),
  run('a restart while away', { now: T0, prev: { v: 1, where: 'away', since: '2026-11-18T21:00:00.000Z', lastInputFrom: 'phone' } }, [I('phone', T0 + 1000)]),
  run('a restart: since in the future is not kept', { now: T0, prev: { v: 1, where: 'office', since: '2030-01-01T00:00:00.000Z', lastInputFrom: 'office' } }, []),
  run('a previous file of another version', { prev: { v: 2, where: 'remote', since: '2026-11-18T21:00:00.000Z', lastInputFrom: 'remote' } }, []),
  run('a previous file that is junk', { prev: { v: 1, lastInputFrom: 'deck' } }, []),
  run('the deck block', { deck: { pid: 7, startedAt: '2026-11-01T10:00:00.000Z' } }, []),
  run('the deck block, nothing given', { deck: {} }, []),
];
const away = [undefined, null, '', 0, '0', 5, '5', 5.4, 5.5, -3, 1440, 1441, 'x', NaN, true, '  12  '].filter(v => v !== undefined && !(typeof v === 'number' && Number.isNaN(v))).map(v => ({ in: v, out: P.awayMinutes(v) }));
const kinds = ['native', 'browser', 'NATIVE', '', null, 7].map(v => ({ in: v, out: P.deskKind(v) }));
const out = path.join(__dirname, '..', 'fixtures', 'deck', 'presence.json');
fs.writeFileSync(out, JSON.stringify({ source: path.basename(path.dirname(path.dirname(path.resolve(file)))) + '/lib/presence.js', scenarios, away, kinds, env: P.tileEnv('tile-1', 'C:\\state\\deck-state.json'), defaultAway: P.DEFAULT_AWAY_MINUTES }, null, 1) + '\n');
console.log(`wrote presence -> ${out} (${fs.statSync(out).size} bytes)`);
