'use strict';
// core/test/proto/gen-fixtures.js — acceptance vectors for core/proto/frames.hpp, from the JS that speaks the
// protocol today. `synthesised` is renderer/desk/bridge.js's SYNTHESISED test (R8), read out of that file so a
// change there fails the C++ until it follows.
//   node core/test/proto/gen-fixtures.js
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..', '..', '..');
const src = fs.readFileSync(path.join(root, 'renderer', 'desk', 'bridge.js'), 'utf8');
const m = /const SYNTHESISED = (\/.*\/);/.exec(src);
if (!m) throw new Error('SYNTHESISED not found in bridge.js');
const SYNTHESISED = eval(m[1]);
const E = '\x1b';
const inputs = [
  '', 'a', '\r', E, E + '[', E + '[A', E + '[I', E + '[O', E + '[I' + E + '[O',
  E + '[<64;40;12M', E + '[<65;1;1M', E + '[<0;10;20m', E + '[<0;10;20M' + E + '[<0;10;20m',
  E + '[<64;40;12', E + '[<64;40M', E + '[<;40;12M', E + '[<64;40;12;7M', E + '[<64;40;12Mx', 'x' + E + '[<64;40;12M',
  E + '[M abc'.slice(0, 6), E + '[M !"', E + '[M !', E + '[M\x00\x01\x02', E + '[M\n\n\n',
  E + '[I' + E + '[<64;40;12M' + E + '[M !"', '4;40;12M', E + '[1;5A', E + '[200~hi' + E + '[201~', E + '[?1;2c', E + '[12;40R',
];
const out = inputs.map(d => ({ d: [...Buffer.from(d, 'latin1')], want: typeof d === 'string' && SYNTHESISED.test(d) }));
fs.writeFileSync(path.join(__dirname, '..', 'fixtures', 'proto-synth.json'), JSON.stringify(out, null, 1) + '\n');
console.log(`proto-synth.json: ${out.length} vectors`);
