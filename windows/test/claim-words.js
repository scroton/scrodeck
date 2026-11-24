'use strict';
// windows/test/claim-words.js — the driver button's WORDS, the exe's against the renderer's own, word for word.
//   node windows/test/claim-words.js [claim checkout]      (after build-sidebar.ps1: it reads out/*.png.txt)
// NEW deck: driverMode / driveName / driveTitle are taken out of the claim checkout's renderer/js/80-sidebar.js
// (branch native-remote-server; default: the sibling worktree _wt-deck-remote-rule, read-only) and run as a remote
// desk (deck._desk); what they answer must be the tooltip the exe drew for each claim-* fixture. The four names of
// Settings > "Size is driven by" in windows/src/settings.hpp must be that checkout's index.html options.
// OLD deck: the three tooltips and the three option names must be THIS checkout's renderer's (80-sidebar.js, index.html).
const fs = require('fs'), path = require('path'), vm = require('vm');
const root = path.resolve(__dirname, '..', '..');
const claimRoot = process.argv[2] || path.resolve(root, '..', '_wt-deck-remote-rule');
const out = path.join(__dirname, 'out');
let pass = 0, fail = 0;
const check = (ok, what, detail) => { if (ok) pass++; else { fail++; console.log('FAIL ' + what + (detail ? '\n     ' + detail : '')); } };
const tipOf = shot => { const m = /^tool drive-btn {2}tip: (.*)$/m.exec(fs.readFileSync(path.join(out, shot + '.png.txt'), 'utf8')); return m ? m[1].trim() : null; };
const options = html => { const m = /<select id="set-driver">(.*?)<\/select>/s.exec(html); return [...m[1].matchAll(/<option value="([^"]*)">([^<]*)<\/option>/g)].map(x => [x[1], x[2]]); };
const unescape = s => s.replace(/\\x([0-9A-Fa-f]{4})/g, (_, h) => String.fromCharCode(parseInt(h, 16))).replace(/" L"/g, '');
const cppOptions = name => {
  const src = fs.readFileSync(path.join(root, 'windows', 'src', 'settings.hpp'), 'utf8');
  const at = src.indexOf(name); const end = src.indexOf('} }', at);
  return [...src.slice(at, end + 3).matchAll(/\{ "([a-z]+)", L"([^"]*)" \}/g)].map(x => [x[1], unescape(x[2])]);
};

// ---- the old deck: this checkout's renderer
{
  const js = fs.readFileSync(path.join(root, 'renderer', 'js', '80-sidebar.js'), 'utf8');
  const fixtures = { 'mixed': 'auto', 'right': 'desk' };            // meta-mixed (auto), meta-right (desk); meta-color (phone) is shot without --dump
  for (const [shot, mode] of Object.entries(fixtures)) {
    const want = mode === 'phone' ? /b\.title = m === 'phone' \? '([^']*)'/.exec(js)[1] : mode === 'desk' ? /: m === 'desk' \? '([^']*)'/.exec(js)[1] : /\n\s*: '(Auto: [^']*)';/.exec(js)[1];
    const got = tipOf(shot);
    check(got === want, `old deck, ${mode}: the tooltip is the renderer's`, `exe      ${got}\n     renderer ${want}`);
  }
  const sb = fs.readFileSync(path.join(root, 'windows', 'src', 'sidebar.cpp'), 'utf8');
  check(sb.includes(/b\.title = m === 'phone' \? '([^']*)'/.exec(js)[1]), 'old deck, phone: the tooltip is in sidebar.cpp');
  const a = JSON.stringify(cppOptions('{ "set-driver", {')), b = JSON.stringify(options(fs.readFileSync(path.join(root, 'renderer', 'index.html'), 'utf8')));
  check(a === b, 'old deck: the three names of "Size is driven by"', `exe      ${a}\n     renderer ${b}`);
}

// ---- the new deck: the claim checkout's renderer, as a remote desk
if (!fs.existsSync(path.join(claimRoot, 'lib', 'driverrule.js'))) console.log(`no claim checkout at ${claimRoot}: the new deck's words are not checked`);
else {
  const js = fs.readFileSync(path.join(claimRoot, 'renderer', 'js', '80-sidebar.js'), 'utf8');
  const from = js.indexOf('const DRIVE_ME'), to = js.indexOf('function paintDrive');
  check(from > 0 && to > from, 'the claim checkout has DRIVE_ME … driveTitle');
  const box = { deck: { _desk: true }, state: { remoteDesk: null } };
  vm.createContext(box);
  vm.runInContext(js.slice(from, to) + '\nthis.driveTitle = driveTitle; this.driverMode = driverMode; this.DRIVE_ME = DRIVE_ME;', box);
  check(box.DRIVE_ME === 'remote', 'the renderer, as a remote desk, is of kind remote');
  const cases = [['claim-auto', 'auto', 'LAPTOP-7'], ['claim-office', 'office', 'LAPTOP-7'], ['claim-phone', 'phone', 'LAPTOP-7'], ['claim-remote', 'remote', 'LAPTOP-7'], ['claim-noname', 'remote', null]];
  for (const [shot, mode, name] of cases) {
    const meta = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures', `meta-${shot}.json`), 'utf8'));
    check(meta.settings.driver === mode && (meta.remoteDesk || {}).name === name, `${shot}: the fixture is ${mode} / ${name}`);
    box.state.remoteDesk = meta.remoteDesk;
    const want = box.driveTitle(box.driverMode(meta.settings.driver)), got = tipOf(shot);
    check(got === want, `new deck, ${mode}${name ? '' : ' (no name)'}: the tooltip is the branch's sentence`, `exe      ${got}\n     renderer ${want}`);
    console.log(`  ${mode.padEnd(7)} ${got}`);
  }
  const a = JSON.stringify(cppOptions('driverClaim = {')), b = JSON.stringify(options(fs.readFileSync(path.join(claimRoot, 'renderer', 'index.html'), 'utf8')));
  check(a === b, 'new deck: the four names of "Size is driven by"', `exe      ${a}\n     renderer ${b}`);
  // the per-tile words under auto (30-tiles.js)
  const tiles = fs.readFileSync(path.join(claimRoot, 'renderer', 'js', '30-tiles.js'), 'utf8'), sb = fs.readFileSync(path.join(root, 'windows', 'src', 'sidebar.cpp'), 'utf8');
  for (const w of ['on the remote desk', 'sized by the office deck', 'on your phone']) check(tiles.includes(`'${w}'`) && sb.includes(`L"${w}"`), `the tile's words: ${w}`);
}
console.log(`${pass}/${pass + fail} passed`);
process.exit(fail ? 1 : 0);
