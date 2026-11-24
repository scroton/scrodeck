// mac/Tests/gen-keychain-names.js — vectors for mac/Sources/DeckMacSeams/KeychainNames.hpp, written by the REAL
// lib/platform.js keychainServiceNames. It must run with POSIX path rules and a Mac home, so `path` is swapped for
// path.posix and os.homedir() for a fixed home before the module loads; process.cwd() is pinned the same way.
//   node mac/Tests/gen-keychain-names.js   ->  mac/Tests/keychain-names.json
'use strict';
const path = require('path');
const os = require('os');
const Module = require('module');
const HOME = '/Users/t', CWD = '/Users/t/work';
os.homedir = () => HOME;
const realCwd = process.cwd;
const origLoad = Module._load;
Module._load = function (request, ...rest) { return request === 'path' ? path.posix : origLoad.call(this, request, ...rest); };
process.cwd = () => CWD;
const platform = require('../../lib/platform.js');
const dirs = ['/Users/t/.claude', '/Users/t/.claude/', '/Users/t/.claude-work//', '~/.claude-demo', '.claude-rel', '../up/.claude', '/Users/t/café/.claude', '/a/./b/../c', '', '/'];
const out = { home: HOME, cwd: CWD, cases: dirs.map(dir => ({ dir, names: platform.keychainServiceNames(dir) })) };
process.cwd = realCwd; Module._load = origLoad;
require('fs').writeFileSync(path.join(__dirname, 'keychain-names.json'), JSON.stringify(out, null, 1) + '\n');
console.log(`${out.cases.length} cases`);
