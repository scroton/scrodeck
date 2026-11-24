'use strict';
// core/test/deck/gen-onedeck.js - lib/onedeck.js's own answers, for core/deck/onedeck.cpp to replay
// (core/test/fixtures/deck/onedeck.json; the core deck test's section `onedeck`):
//   node core/test/deck/gen-onedeck.js
// THE ROUND TRIP the fixture closes: an Electron deck's files -> the shared file (this JS) -> the native deck's files
// (C++ fromShared) -> the shared file again (C++ toShared, must equal the JS one but for who wrote it and when) ->
// the Electron deck's files (fromShared, both languages: every tile, session, title, group, window cell, pause mark,
// held prompt, trigger and behaviour setting as they were; the phone, its token, the fonts and each deck's window
// bounds stay its own). Plus decide() over every case the start-up decision has, and the marker.
const fs = require('fs');
const path = require('path');
const O = require(path.join(__dirname, '..', '..', '..', 'lib', 'onedeck.js'));

const AT = Date.UTC(2026, 10, 24, 20, 0, 0);
const electron = {
  tilesJson: {
    tiles: [
      { id: 'a1', kind: 'claude', account: 'claudem', sessionId: '11111111-1111-4111-8111-111111111111', cwd: 'C:\\Users\\demo', pinned: true, group: 'floor', createdAt: 1790000000000, title: 'Receiving', userTitle: 'Receiving (Avery)', asleep: false, sleptAt: null, browser: true,
        pause: { state: 'paused', at: 1790000100000, untilRestart: true, want: false, file: 'C:\\Users\\demo\\AppData\\Roaming\\claude-deck\\pause\\1111.md' } },
      { id: 'a2', kind: 'claude', account: 'claudev', sessionId: '22222222-2222-4222-8222-222222222222', cwd: 'C:\\x', pinned: false, group: null, createdAt: 1790000000001, title: 'Pasted pause', userTitle: null, asleep: true, sleptAt: 1790000200000, browser: null,
        pause: { state: 'paused', at: 1790000300000, untilRestart: false, want: false, by: 'prompt', sid: '22222222-2222-4222-8222-222222222222' } },
      { id: 'a3', kind: 'claude', account: 'claudem', sessionId: '33333333-3333-4333-8333-333333333333', cwd: 'C:\\x', pinned: false, group: 'floor', createdAt: 1790000000002, title: 'Auditor', userTitle: null, asleep: false, sleptAt: null, browser: false,
        auditor: { string: 'AUDIT', last: 3 }, desk: 'security' },
      { id: 'a4', kind: 'claude', account: 'claudem', sessionId: '44444444-4444-4444-8444-444444444444', cwd: 'C:\\x', pinned: false, group: null, createdAt: 1790000000003, title: 'Child', userTitle: 'kid', asleep: false, sleptAt: null, browser: null,
        child: { parent: 'a3', n: 2 } },
      { id: 'a5', kind: 'cloud', account: 'claudem', sessionId: null, cwd: 'C:\\x', pinned: false, group: 'cloud', createdAt: 1790000000004, title: 'A cloud task', userTitle: null, asleep: false, sleptAt: null, browser: null, cloudId: 'session_01ABCDEF' },
    ],
    groups: ['floor', 'cloud'],
    windows: [
      { bounds: { x: 10, y: 20, width: 1600, height: 960, maximized: true }, layout: { a1: [0, 1], a3: [2], a4: [3] } },
      { bounds: { x: 1700, y: 20, width: 1200, height: 900, maximized: false }, layout: { a2: [0, 1, 2, 3] } },
    ],
    deskLayout: { a5: [0] },
  },
  held: { items: [{ id: 'h1', tileId: 'a1', sessionId: '11111111-1111-4111-8111-111111111111', text: 'RESUME later', when: { kind: 'idle' }, state: 'held', at: 1790000400000 }] },
  triggers: { triggers: { ci_green: { value: false, at: 1790000500000, by: 'desk' } } },
  settings: { grid: { rows: 3, cols: 4 }, oneWindow: true, hibernate: { enabled: true, idleMinutes: 30 }, resumeAfterRestart: true, driver: 'office',
    asks: { deckThreshold: 0, picker: 'alongside' }, phone: { enabled: true, port: 8788, bind: 'localhost', token: 'ELECTRONTOKEN000' }, pages: [{ label: 'r', file: 'C:\\r.html' }],
    fontSize: 10, fontFamily: 'Reddit Mono', cwd: 'C:\\Users\\demo', aliases: { 'm@x': 'demo' } },
};
// the native deck before the import: its own windows' bounds, its own phone and token, something stale
const nativeOwn = {
  tilesJson: { tiles: [{ id: 'old', kind: 'claude', account: 'claudem', sessionId: '99999999-9999-4999-8999-999999999999', title: 'stale' }], groups: [], windows: [{ bounds: { x: 0, y: 0, width: 1400, height: 900 }, layout: { old: [0] } }] },
  settings: { grid: { rows: 2, cols: 2 }, phone: { enabled: false, port: 8788, bind: 'tailscale', token: 'NATIVETOKEN00000' }, fontSize: 13, driver: 'auto' },
};
const shared = O.toShared(electron, { deck: 'electron', at: AT, realChange: 7, lastChange: { what: 'renamed a tile', at: new Date(AT - 60000).toISOString(), by: 'electron' } });
const intoNative = O.fromShared(shared, nativeOwn.tilesJson, nativeOwn.settings);
const backToElectron = O.fromShared(shared, electron.tilesJson, electron.settings);

const sync = (n, seen) => ({ v: 1, local: { n, last: n ? { what: 'a new tile', at: new Date(AT - 5000).toISOString(), by: 'native' } : null }, seen });
const seen = O.seenOf(shared, 2);
const older = { ...shared, realChange: 5, writtenAt: new Date(AT - 3600000).toISOString() };
const decideCases = [
  { name: 'no shared file yet', shared: null, sync: sync(0, null), own: { hasState: true, savedAt: AT - 1000 } },
  { name: 'nothing new on either side', shared, sync: sync(2, seen), own: { hasState: true } },
  { name: 'only this deck changed (a write failed)', shared, sync: sync(3, seen), own: { hasState: true } },
  { name: 'the shared file is newer, nothing here', shared, sync: sync(2, O.seenOf(older, 2)), own: { hasState: true } },
  { name: 'both changed', shared, sync: sync(3, O.seenOf(older, 2)), own: { hasState: true } },
  { name: 'the shared file went back', shared: older, sync: sync(2, seen), own: { hasState: true } },
  { name: 'never synced, tiles of its own', shared, sync: sync(0, null), own: { hasState: true, savedAt: AT - 7200000 } },
  { name: 'never synced, empty', shared, sync: sync(0, null), own: { hasState: false } },
].map(c => ({ ...c, out: O.decide(c) }));

const marker = O.marker({ deck: 'native', running: true, pid: 4242, exe: 'C:\\p\\ClaudeDeckNative.exe', args: ['--local'], stateDir: 'C:\\s', startedAt: new Date(AT).toISOString(), heartbeatAt: new Date(AT).toISOString(), liveSessions: 3 });
const out = { at: AT, electron, nativeOwn, shared, intoNative, backToElectron, decideCases, marker, localSettings: O.LOCAL_SETTINGS, blocked: [0, 1, 4].map(n => O.blockedText(n)) };
const file = path.join(__dirname, '..', '..', 'test', 'fixtures', 'deck', 'onedeck.json');
fs.writeFileSync(file, JSON.stringify(out, null, 1) + '\n');
console.log('wrote ' + file);
