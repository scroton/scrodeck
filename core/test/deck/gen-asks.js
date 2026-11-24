'use strict';
// Generate core/test/fixtures/deck/asks.json from lib/asks.js (the REAL module, required as it is):
//   node core/test/deck/gen-asks.js
// Everything runs in a temp folder that is removed afterwards: a fake ask-page state folder, pages, Downloads, the
// deck's drafts file and a FAKE home (os.homedir is replaced before lib/asks.js is required, so locations() never
// resolves the real one). Pinned: the time zone (America/Bogota, UTC-5, no DST), the clock (deps.now),
// process.pid, every synthetic file's mtime, and the temp root itself: it is rewritten to FX_ROOT in the fixture,
// so the fixture is the same from run to run and names no real folder.
process.env.TZ = 'America/Bogota';
const fs = require('fs');
const os = require('os');
const path = require('path');
const url = require('url');

const PID = 4242;
Object.defineProperty(process, 'pid', { value: PID });
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'gen-asks-'));
if (!/^[A-Za-z]:[\\A-Za-z0-9._-]+$/.test(root)) throw new Error('the temp folder has characters a file URL would encode: ' + root);
if (new Date(0).getTimezoneOffset() !== 300) throw new Error('TZ was not taken');
const FX_ROOT = 'C:\\fx-asks';
const fakeHome = path.join(root, 'home');
fs.mkdirSync(fakeHome, { recursive: true });
os.homedir = () => fakeHome;
delete process.env.ASK_PAGE_STATE_DIR; delete process.env.ASK_PAGE_DOWNLOADS;
const A = require(path.join(__dirname, '..', '..', '..', 'lib', 'asks.js'));

const T0 = Date.parse('2026-11-10T18:00:00.000Z');       // 13:00 in Bogota
const MIN = 60000, HOUR = 3600000, DAY = 86400000;
const plain = v => (v === undefined ? null : JSON.parse(JSON.stringify(v)));

// ---------------------------------------------------------------- the pure functions
const ids = ['ask_test-a_aaaaaaaa_r1_1110-1200_abcd', 'ask_a_00000000_r12_0000-0000_0000', 'ask_A-b-9_0123abcd_r1_1110-1200_ffff', 'ask__aaaaaaaa_r1_1110-1200_abcd',
  'ask_a_b_aaaaaaaa_r1_1110-1200_abcd', 'ask_a_AAAAAAAA_r1_1110-1200_abcd', 'ask_a_aaaaaaa_r1_1110-1200_abcd', 'ask_a_aaaaaaaaa_r1_1110-1200_abcd',
  'ask_a_aaaaaaaa_r_1110-1200_abcd', 'ask_a_aaaaaaaa_r1_111-1200_abcd', 'ask_a_aaaaaaaa_r1_11101200_abcd', 'ask_a_aaaaaaaa_r1_1110-1200_abc',
  'ask_a_aaaaaaaa_r1_1110-1200_abcde', 'ask_a_aaaaaaaa_r1_1110-1200_abcg', 'ask_a_aaaaaaaa_r1_1110-1200_abcd\n', ' ask_a_aaaaaaaa_r1_1110-1200_abcd',
  'Ask_a_aaaaaaaa_r1_1110-1200_abcd', '', 'ask_', '../evil', 'ask_\u00e9_aaaaaaaa_r1_1110-1200_abcd', 'ask_a_aaaaaaaa_R1_1110-1200_abcd', 'ask_a-_aaaaaaaa_r0001_9999-9999_0a0a',
  'ask_a_aaaaaaaa_r1_1110-1200_abcd_r1_1110-1200_abcd', 'ask_a.b_aaaaaaaa_r1_1110-1200_abcd'].map(s => ({ in: s, ok: A.ID_RE.test(s) }));

const id1 = 'ask_test-a_aaaaaaaa_r1_1110-1200_abcd', id2 = 'ask_test-a_aaaaaaaa_r2_1110-1210_beef';
const doneCases = [
  [{ id: id1, lines: ['Q1 (x): yes', 'Q2 (y): later'] }],
  [{ id: id1, lines: ['Q1 (x): yes'] }, { id: id2, lines: ['Q1 (z): no'] }],
  [{ id: 'x', lines: [] }], [{ id: 'p1', lines: [] }, { id: 'p2', lines: ['Q1 (z): no'] }],
  [], null, [null, 0, 'x', { id: 'a' }, { id: 'b', lines: 'Q1' }, { id: 'c', lines: ['  ', '\t\n'] }],
  [{ lines: ['no id'] }, { id: null, lines: ['null id'] }, { id: 7, lines: [1, true, null, 2.5, 'x'] }],
  [{ id: 'u', lines: ['', ' \u00a0 ', 'Q1 (caf\u00e9 \ud83d\ude00): s\u00ed'] }], [{ id: 'm', lines: ['a\nb', ''] }], [{ id: 'n', lines: [0] }, { id: 'o', lines: [''] }],
].map(p => ({ in: p, out: A.composeDone(p) }));
const pointerCases = [
  [{ id: id1, file: 'D:/scratchdir/ask-pages/2026-11-10/a.answers.json', answered: 131, total: 135, later: 4 }],
  [{ id: id1, file: 'x/1.answers.json', answered: 2, total: 2 }, { id: id2, file: 'x/2.answers.json' }],
  [{ id: id1 }], [], null, [null, { file: 'f' }, { id: '', file: 'f' }, { id: 'a', file: '' }],
  [{ id: 'a', file: 'f', total: 3 }], [{ id: 'a', file: 'f', total: 3, answered: 0, later: 0 }], [{ id: 'a', file: 'f', total: 0, answered: 2, later: 1 }],
  [{ id: 'a', file: 'C:\\p q\\\u00e9.answers.json', total: 2.5, answered: '1', later: 'two' }],
  [{ id: 'a', file: 'f1', total: 1 }, { id: 'b', file: 'f2', total: 2, answered: 1 }, { id: 'c', file: 'f3', total: 3, answered: 1, later: 2 }],
  [{ id: 5, file: 6, total: 7, answered: 8, later: 9 }],
].map(p => ({ in: p, out: A.composePointer(p) }));

const screen = '\x1b[1m\u2190 \u2610 beta  \u2610 lines\x1b[0m\r\n\r\nShip the picker build to beta tonight, or wait for\r\n Alex?\r\n\u276f 1. Tonight';
const Q = 'Ship the picker build to beta tonight, or wait for Alex?';
const FILL = 'the quick brown fox jumps over the lazy dog 0123456789 \r\n';
const big = n => [[FILL, Math.floor(n / FILL.length)], [FILL.slice(0, n % FILL.length), 1]];   // n units of filler, as [chunk, times] parts
const pickerCases = [
  [screen, [Q]], ['Do you want to run this command?\r\n\u276f 1. Yes', [Q]], ['ok', ['ok']], ['', [Q]], [screen, []], [screen, ['', 'nothing like it at all', Q]],
  ['1234567', ['1234567']], ['12345678', ['12345678']], ['x 12345678 y', ['  12345678  ']], ['a\x1b[31mb\x1b[0mc defgh', ['a b c defgh']],
  ['a\x1b]0;window title\x07b cdefgh', ['ab cdefgh']], ['a\x1b]0;never closed b cdefgh', ['ab cdefgh']], ['a\x1b[?25lb\x1b[<1;2;3Mc\x1b[=1hd efgh', ['a b c d efgh']],
  ['a\x1b[12;b cdefgh', ['a\x1b[12;b cdefgh']], ['tab\tand\u00a0nbsp\u3000wide  spaces', ['tab and nbsp wide spaces']],
  ['\u00bfEnviamos el pedido de caf\u00e9 ma\u00f1ana? \ud83d\ude00 s\u00ed', ['\u00bfEnviamos el pedido de caf\u00e9 ma\u00f1ana? \ud83d\ude00 s\u00ed, claro que s\u00ed, y m\u00e1s texto']],
  ['0123456789 0123456789 0123456789 0123456789 and then it differs', ['0123456789 0123456789 0123456789 0123456789 but only the first forty count']],
  ['0123456789 0123456789 0123456789 012345678X', ['0123456789 0123456789 0123456789 0123456789']],
  [[[Q, 1], ...big(65536 - 10)], [Q]], [[[Q, 1], ...big(65536 - Q.length)], [Q]], [[[Q, 1], ...big(65536 - Q.length + 1)], [Q]], [[[Q, 1], ...big(70000)], [Q]], [[...big(70000), [Q, 1]], [Q]],
  [[[Q, 1], ['\u00e9', 65536 - Q.length]], [Q]], [[[Q, 1], ['\u00e9', 65536 - Q.length + 1]], [Q]], [[[Q, 1], ['\ud83d\ude00', (65536 - Q.length) / 2]], [Q]], [[[Q, 1], ['\ud83d\ude00', (65536 - Q.length) / 2 + 1]], [Q]],
  [[['x', 1], [Q, 1], ['\ud83d\ude00', 32768 - Q.length / 2]], [Q]],
  ['\x1b[2J\x1b[H' + Q.split(' ').join('\x1b[1C'), [Q]], [screen, [Q.toUpperCase()]], ['esc at the end \x1b', ['esc at the end']], ['esc bracket at the end \x1b[', ['esc bracket at the end']],
].map(([recent, questions]) => { const parts = typeof recent === 'string' ? [[recent, 1]] : recent; return { parts, questions, out: A.pickerOnScreen(parts.map(([c, n]) => c.repeat(n)).join(''), questions) }; });
if (Q.length % 2) throw new Error('the picker cases want an even question length');

const rowSeqs = [
  [[{ sessionId: 's1', name: 'one', rounds: [{ id: 'p1', total: 3 }] }],
    [{ sessionId: 's1', name: 'one', rounds: [{ id: 'p1', total: 3 }, { id: 'p2', total: 2 }] }, { sessionId: 's2', name: 'two', rounds: [{ id: 'p3', total: 1, background: true }, { id: 'p4', total: 4, sent: { at: 1 } }] }],
    [{ sessionId: 's1', name: 'one', rounds: [{ id: 'p1', total: 3 }, { id: 'p2', total: 2 }] }, { sessionId: 's2', name: 'two', rounds: [{ id: 'p3', total: 1, background: true }, { id: 'p4', total: 4, sent: { at: 1 } }] }],
    [{ sessionId: 's1', name: 'one', rounds: [{ id: 'p2', total: 2 }] }], [], [{ sessionId: 's3', rounds: [{ id: 'p9' }, { id: 'p9' }, { id: 'p1' }] }, { sessionId: 's4' }, { sessionId: 's5', rounds: [] }],
    null, [{ sessionId: 's1', rounds: [{ id: 'p1', sent: null, background: 0 }, { id: 'p5', sent: false }] }, { sessionId: 's2', rounds: [{ id: 'p6', background: 'yes' }, { id: 'p7' }] }]],
  [[], [{ sessionId: 'a', rounds: [{ id: 'x1' }] }], [{ sessionId: 'b', rounds: [{ id: 'x1' }] }, { sessionId: 'a', rounds: [{ id: 'x2' }, { id: 'x3' }] }]],
].map(seq => { let seen = null; return seq.map(view => { const r = A.newAskRows(seen, view); seen = r.seen; return { view, seen: [...r.seen], fresh: r.fresh }; }); });

// url.pathToFileURL: { windows } exists in Node 22+; this box is Windows, so without it the win32 cases still run
const hasWinOpt = (() => { try { return url.pathToFileURL('C:\\a', { windows: false }).href !== url.pathToFileURL('C:\\a', { windows: true }).href; } catch { return false; } })();
const winPaths = ['C:\\a b\\c#d.html', 'C:/a//b/../c.html', 'c:\\x\\', 'C:\\', 'C:/', 'C:\\a/', 'C:\\a\\.', 'C:\\a\\b\\\\', 'C:\\a\\..\\..\\b', 'C:\\a:b', 'C:\\a\\b.\\c ',
  'C:\\p%20q\\\u00e9 \u00f1\\\ud83d\ude00.html', 'C:\\a?b\\c^d\\e{f}g\\h|i\\j"k<l>m`n.html', 'C:\\a\tb\nc\rd.html', 'C:\\a;b&c=d+e,f$g@h!i~j\'k(l)m*n[o]p.html', 'C:\\a\\%2e%2e\\b',
  'C:\\a\x7fb\x01c', 'D:\\scratchdir\\ask-pages\\2026-11-10\\ask_test-a_aaaaaaaa_r1_1110-1200_abcd.html', 'z:\\UPPER\\lower\\MiXed.HTML', 'C:\\\u65e5\u672c\u8a9e\\\u0444\u0430\u0439\u043b.html',
  'C:\\a\\.\\b\\..\\c\\', 'C:\\a\\...\\b', 'C:\\a\\.hidden\\..b', 'C:\\Users\\someone\\AppData\\Local\\Temp\\x y\\ask_p_aaaaaaaa_r1_1110-1200_abcd.html',
  '\\\\Srv\\share\\a b\\..\\c.html', '\\\\srv\\sh\\%2e%2e\\b', '\\\\srv\\sh\\a\\.\\b\\\\c\\', '\\\\SRV.Example.COM\\s', '\\\\srv\\', '\\\\srv', '\\\\\\x\\y', '\\\\?\\C:\\a\\b', '\\\\?\\UNC\\srv\\sh\\f.html',
  '\\\\?\\UNC\\srv', '\\\\?\\UNC\\\\x', '\\\\srv\\s\\..\\..\\x', '\\\\srv\\s\\a\\..', '\\\\srv\\s\\a\\.', '\\\\srv\\C:\\x', '\\\\srv\\C:\\..\\x', '\\\\srv\\C|\\x', '\\\\srv/share/x', '//srv/share/x', '//srv/share/a b/../c#d/', '//srv/share/x/', '//srv/share/', '//srv/share', '//srv/share/a/b/', '//SRV/share/x', '//s v/share/x', '//srv/sh are/x#/y',
  '\\\\srv\\s\\a b\\', '\\\\srv\\s\\\u00e9#?%.html', '\\\\s v\\x', '\\\\s%v\\x', '\\\\s:v\\x', '\\\\s@v\\x', '\\\\s\tv\\x', '\\\\s#v\\x', '\\\\s?v\\x', '\\\\nas-01.local\\Shared Docs\\ask.html', '\\\\srv\\s\\..', '\\\\srv\\..\\..\\x\\.'];
for (let c = 1; c < 128; c++) { const ch = String.fromCharCode(c); if (ch !== '/' && ch !== '\\') { winPaths.push(`C:\\d\\a${ch}b`); winPaths.push(`\\\\srv\\s\\a${ch}b`); } }
// not '/': with { windows: false } on a Windows host Node compares the last character with the HOST's path.sep and prints file:////
const posixPaths = ['/a b/c#d', '/a/../b//c/', '/a\\b%', '/\u00e9?x', '/a', '/a/', '/a/./b/../../c', '/home/someone/.local/state/ask-pages/ask_a.html', '/a/\ud83d\ude00/~x', '//a//b', '/..', '/a/.../b'];
for (let c = 1; c < 128; c++) { const ch = String.fromCharCode(c); if (ch !== '/') posixPaths.push(`/d/a${ch}b`); }
const urlOf = (p, windows) => { try { return (hasWinOpt ? url.pathToFileURL(p, { windows }) : url.pathToFileURL(p)).href; } catch { return null; } };
const urls = winPaths.map(p => ({ path: p, win: true, out: urlOf(p, true) })).concat(hasWinOpt ? posixPaths.map(p => ({ path: p, win: false, out: urlOf(p, false) })) : []);

// locations(), with the fake home. Paths are absolute or start with ~ (a relative one resolves against the cwd).
const locCases = [];
function loc(name, files, env) {
  fs.rmSync(fakeHome, { recursive: true, force: true }); fs.mkdirSync(fakeHome, { recursive: true });
  const rec = {};
  for (const [rel, text] of Object.entries(files)) { const f = path.join(fakeHome, rel); fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, text); rec[f] = text; }
  locCases.push({ name, files: rec, env, out: A.locations(Object.assign({}, env)) });
}
const SK = d => `${d}/skills/ask-page/settings.json`;
loc('nothing installed: the defaults', {}, {});
loc('the skill settings in .claude', { [SK('.claude')]: JSON.stringify({ state_dir: '~/state dir/ask-pages', downloads: ['E:/Down loads/', '~\\dl', 'C:\\a\\..\\b\\.\\c'] }) }, {});
loc('.claude does not parse: the next one', { [SK('.claude')]: '{ "state_dir": ', [SK('.claude-thirdacct')]: JSON.stringify({ state_dir: 'D:\\scratchdir\\ask-pages\\state' }) }, {});
loc('.claude is empty: the next but one', { [SK('.claude')]: '', [SK('.claude-demo')]: JSON.stringify({ downloads: ['~'] }) }, {});
loc('the last of the four', { [SK('.claude-vv')]: JSON.stringify({ state_dir: '~', downloads: [] }) }, {});
loc('the first that parses wins, even with nothing in it', { [SK('.claude')]: '{}', [SK('.claude-vv')]: JSON.stringify({ state_dir: 'X:/never' }) }, {});
loc('a BOM does not parse', { [SK('.claude')]: '\ufeff' + JSON.stringify({ state_dir: 'X:/bom' }), [SK('.claude-thirdacct')]: JSON.stringify({ state_dir: 'Y:/after the bom' }) }, {});
loc('settings that are a number or an array', { [SK('.claude')]: '5', [SK('.claude-thirdacct')]: JSON.stringify({ state_dir: 'X:/never' }) }, {});
loc('downloads that is not a list', { [SK('.claude')]: JSON.stringify({ downloads: 'D:/x', state_dir: 'C:/s/' }) }, {});
loc('odd members', { [SK('.claude')]: JSON.stringify({ downloads: ['~/a', 'C:/~b/c', 'C:/~/d', '~', 'C:\\x\\~'], state_dir: 'C:/~x/y' }) }, { ASK_PAGE_STATE_DIR: '', ASK_PAGE_DOWNLOADS: '' });
loc('the environment wins', { [SK('.claude')]: JSON.stringify({ state_dir: 'X:/never', downloads: ['X:/never'] }) },
  { ASK_PAGE_STATE_DIR: '~/scratch/state', ASK_PAGE_DOWNLOADS: 'C:\\one;~/two;D:/three/../four/' });
loc('the environment, one folder', {}, { ASK_PAGE_STATE_DIR: 'Q:\\st\\', ASK_PAGE_DOWNLOADS: '~' });
loc('a UNC state folder', { [SK('.claude')]: JSON.stringify({ state_dir: '//nas/share/ask pages/', downloads: ['\\\\nas\\share\\dl\\..\\Downloads'] }) }, {});

// ---------------------------------------------------------------- the feed, in scripted scenarios
const realSetTimeout = global.setTimeout, realClearTimeout = global.clearTimeout;
let timers = [];
const fakeTimers = on => {
  global.setTimeout = on ? (fn => { const t = { fn, off: false }; timers.push(t); return t; }) : realSetTimeout;
  global.clearTimeout = on ? (t => { if (t && typeof t === 'object' && 'off' in t) t.off = true; }) : realClearTimeout;
};
const runTimers = () => { const run = timers; timers = []; for (const t of run) if (!t.off) t.fn(); };

const scenarios = [];
function scenario(name, opts, script) {
  const dir = path.join(root, name);
  const stateDir = path.join(dir, 'state'), pages = path.join(dir, 'pages'), dl = path.join(dir, 'dl'), draftsFile = path.join(dir, 'ask-drafts.json');
  for (const d of [stateDir, pages, dl]) fs.mkdirSync(d, { recursive: true });
  const downloads = opts.downloads ? opts.downloads.map(d => path.join(dir, d)) : [path.join(dir, 'nope'), dl];
  const steps = [], synthetic = new Set(), watch = new Set(opts.watch || []);
  let clock = T0, mt = T0 - DAY, changes = 0, reads = [], settings = opts.settings === undefined ? { deckThreshold: 0, picker: 'pane', usePages: true, background: false } : opts.settings;
  let liveMode = 'set', liveIds = [], liveCalls = 0, feed = null;
  const mk = () => new A.AskFeed(Object.assign({ stateDir, downloads, draftsFile, now: () => clock, onChange: () => { changes++; }, settings: () => settings,
    onRead: (id, s) => { reads.push([id, plain(s)]); } },
    opts.live ? { liveSessions: () => { liveCalls++; if (liveMode === 'throw') throw new Error('no'); return new Set(liveIds); } } : {}));
  const walk = d => fs.readdirSync(d, { withFileTypes: true }).flatMap(e => (e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]));
  const snap = () => {
    runTimers();
    const view = plain(feed.view()), logView = plain(feed.logView());
    const per = {};
    for (const id of watch) {
      const x = feed.round(id);
      per[id] = { round: x ? { sid: x.sid, answered: x.answered, later: x.later, waiting: x.waiting, items: x.items.length, round: plain(x.round), name: plain(x.state.session_name) } : null,
        draft: plain(feed.draft(id)), logState: plain(feed.logState(id)), pagePath: feed.pagePath(id), answersDir: feed.answersDir(id) };
    }
    const files = {};
    for (const f of walk(dir)) if (!synthetic.has(f)) files[f] = fs.readFileSync(f, 'utf8');
    return { view, logView, per, changes, reads: plain(reads), liveCalls, files };
  };
  const last = {};
  const step = (s, snapIt = true) => {
    if (snapIt) {
      s.after = snap();
      for (const k of ['view', 'logView', 'per', 'files']) { const t = JSON.stringify(s.after[k]); if (last[k] === t) s.after[k] = '='; else last[k] = t; }
    }
    steps.push(s); return s;
  };
  const S = {
    dir, stateDir, pages, dl, draftsFile,
    watch: (...list) => { for (const id of list) watch.add(id); },
    get clock() { return clock; },
    // a synthetic file: written, its mtime pinned (whole seconds, each later than the last unless one is given)
    put(file, text, mtimeMs) {
      fs.mkdirSync(path.dirname(file), { recursive: true });
      fs.writeFileSync(file, text);
      const m = mtimeMs === undefined ? (mt += 1000) : mtimeMs;
      fs.utimesSync(file, m / 1000, m / 1000);
      synthetic.add(file);
      const st = fs.statSync(file);
      if (st.mtimeMs !== m) throw new Error(`mtime not pinned: ${st.mtimeMs} vs ${m}`);
      step({ op: 'put', path: file, text, mtimeMs: st.mtimeMs, size: st.size }, false);
      return st.mtimeMs;
    },
    page(id, folder) { const f = path.join(folder || pages, `${id}.html`); S.put(f, `<html>${id}</html>`); return f; },
    state(sid, obj, mtimeMs) { return S.put(path.join(stateDir, `${sid}.json`), typeof obj === 'string' ? obj : JSON.stringify(obj), mtimeMs); },
    rm(file) { fs.rmSync(file, { force: true }); synthetic.delete(file); step({ op: 'rm', path: file }, false); },
    mkdir(d) { fs.mkdirSync(d, { recursive: true }); step({ op: 'mkdir', path: d }, false); },
    rmdir(d) { fs.rmSync(d, { recursive: true, force: true }); for (const f of [...synthetic]) if (f.startsWith(d + path.sep)) synthetic.delete(f); step({ op: 'rmdir', path: d }, false); },
    at(t) { clock = t; step({ op: 'clock', t }); },
    tick(ms) { S.at(clock + ms); },
    start() { feed = mk(); step({ op: 'new' }); },
    scan() { feed.scan(); step({ op: 'scan' }); },
    setDraft(id, payload) { const ret = feed.setDraft(id, payload); step({ op: 'setDraft', id, payload: plain(payload), ret }); return ret; },
    clearDraft(id) { feed.clearDraft(id); step({ op: 'clearDraft', id }); },
    markSent(list, info) { feed.markSent(list, info); step({ op: 'markSent', ids: list, info: plain(info) }); },
    logDone(id, answers, how, title) { feed.logDone(id, answers, how, title); step({ op: 'logDone', id, answers: plain(answers), how: how || '', title: plain(title) }); },
    answers(id, answers) { let file = null, error = null; try { file = feed.writeAnswersFile(id, answers); } catch (e) { error = e.message; } step({ op: 'answers', id, answers: plain(answers), file, error }); return file; },
    deckFile() { synthetic.delete(path.join(stateDir, 'deck.json')); const body = feed.writeDeckFile(); step({ op: 'deckFile', body: plain(body) }); },
    removeDeckFile() { feed.removeDeckFile(); step({ op: 'removeDeckFile' }); },
    live(mode, list) { liveMode = mode; liveIds = list || []; step({ op: 'live', mode, ids: liveIds }, false); },
    settings(v) { settings = v; step({ op: 'settings', value: plain(v) }, false); },
  };
  fakeTimers(true);
  try { script(S); runTimers(); } finally { fakeTimers(false); }
  scenarios.push({ name, dir, stateDir, downloads, draftsFile, dirs: [stateDir, pages, dl], live: !!opts.live, settings: plain(opts.settings === undefined ? { deckThreshold: 0, picker: 'pane', usePages: true, background: false } : opts.settings),
    watch: [...watch], steps });
}
const card = (qid, key, title, options, extra) => Object.assign({ qid, key, title, question: `${title}?` }, options ? { options } : {}, extra || {});
const opts2 = [{ key: 'a', label: 'tonight' }, { key: 'b', label: 'after Alex reviews it' }];
const iso = (t, off = '-05:00') => { const d = new Date(t - 5 * HOUR).toISOString().slice(0, 23); return d + off; };   // Bogota wall clock + its offset

scenario('basics', {}, S => {
  const sid = 'aaaaaaaa-1111-2222-3333-444444444444', bad = 'ask_x_99999999_r1_1110-0000_0000';
  const page1 = S.page(id1), page2 = S.page(id2);
  S.watch(id1, id2, bad, '../evil');
  const items = [card('Q1', 'r1-Q1', 'fix timing'), card('Q2', 'r1-Q2', 'reorder')];
  const st = { session_id: sid, session_name: 'test-a', account: 'thirdacct', answers: {},
    rounds: [{ round: 1, id: id1, page: page1, created: '2026-11-10T12:00:00.000-05:00', items, status: 'open', source: 'skill', background: false, read_at: null }] };
  S.start();
  S.scan();                                                   // an empty folder
  S.state(sid, st);
  S.put(path.join(S.stateDir, '.hidden.json'), JSON.stringify({ rounds: [{ id: bad, status: 'open', items: [] }] }));
  S.put(path.join(S.stateDir, 'notes.txt'), 'not json at all');
  S.put(path.join(S.stateDir, 'norounds.json'), JSON.stringify({ session_id: 'x', rounds: 'none' }));
  S.put(path.join(S.stateDir, 'anarray.json'), JSON.stringify([{ rounds: [] }]));
  S.put(path.join(S.stateDir, 'bom.json'), '\ufeff' + JSON.stringify({ session_id: 'bom', rounds: [{ id: bad, status: 'open', items: [] }] }));
  S.put(path.join(S.stateDir, 'deck.json'), JSON.stringify({ kind: 'claude-deck-asks', pid: 1, rounds: [{ id: bad, status: 'open', items: [] }] }));
  S.scan();
  S.scan();                                                   // nothing changed: no onChange
  S.removeDeckFile();                                         // another deck's: left alone
  S.setDraft(id1, { state: { Q1: { text: 'yes', pick: 'a' } }, drafted: 1, answered: 1, total: 2 });
  S.setDraft(id1, { state: { Q1: { text: 'yes', pick: 'a' }, Q2: { text: '', pick: null, later: true } }, drafted: '2', answered: 1.4, later: 1.5, total: 1e9 });
  S.setDraft(id1, { state: { Q1: { text: 'yes!', pick: 'a' }, Q2: { text: '', pick: null, later: true } }, drafted: 2, answered: -3, later: 'x', total: null });   // same count: no scan
  S.setDraft(id1, { state: {}, drafted: 0 });
  S.setDraft('../evil', { state: {}, drafted: 1 });
  S.setDraft(id1, 'a string');
  S.setDraft(id1, null);
  S.setDraft(id1, []);
  S.setDraft(id1, { state: 'not an object', drafted: 3.5, total: 2 });
  S.setDraft(id1, { state: ['a', { text: 'in a list' }], drafted: true, total: '2' });
  S.clearDraft(id1);
  S.clearDraft(id1);
  S.clearDraft(bad);
  const ans = { kind: 'ask-page-answers', id: id1, session_id: sid, lines: ['Q1 (fix timing): yes'], answered: 1, saved_at: '2026-11-10T18:00:00.000Z', via: 'claude-deck',
    state: { Q1: { text: 's\u00ed \ud83d\ude00', pick: 'a' } }, nested: { list: [1, 2.5, [], {}], empty: '' } };
  S.answers(id1, ans);
  S.answers(id1, Object.assign({}, ans, { id: id2 }));
  S.answers(id1, Object.assign({}, ans, { kind: 'ask-page-drafts' }));
  S.answers(id1, null);
  S.answers('../evil', ans);
  S.answers(bad, Object.assign({}, ans, { id: bad }));       // no such round: Downloads
  { // the page folder gone: the first Downloads folder that exists
    const keep = st.rounds[0].page; st.rounds[0].page = path.join(S.dir, 'gone', `${id1}.html`); S.state(sid, st); S.scan();
    S.answers(id1, ans);
    S.rmdir(S.dl);
    S.answers(id1, ans);                                       // nothing at all
    S.mkdir(S.dl);
    st.rounds[0].page = keep; S.state(sid, st); S.scan();
  }
  S.setDraft(id1, { state: { Q1: { text: 'yes', pick: 'a' } }, drafted: 1, answered: 1, total: 2 });
  S.markSent([id1], { how: 'file', heldId: 'held-1', file: path.join(S.pages, `${id1}.answers.json`), tileId: 't1' });
  S.logDone(id1, ans, 'typed', 'The fix');
  // a mid-write file keeps the last good read; then the session reads its answers
  S.state(sid, JSON.stringify(st).slice(0, 80));
  S.scan();
  st.answers['r1-Q1'] = { status: 'answered', answer: 'yes', choice: 'a', reply: null, via: 'paste' };
  st.rounds[0].status = 'partial'; st.rounds[0].read_at = '2026-11-10T12:05:00.000-05:00';
  S.state(sid, st); S.scan();
  // a page answered elsewhere (its picker) and read: logged from the session's own record
  st.rounds.push({ round: 2, id: id2, page: page2, created: iso(T0), items: [card('Q1', 'r2-Q1', 'scope')], status: 'answered', source: 'picker', background: true, read_at: iso(T0 + 1000) });
  st.answers['r2-Q1'] = { status: 'answered', answer: 'Code only', choice: null, reply: 'yourcall', via: 'picker' };
  S.state(sid, st); S.scan();
  for (let i = 0; i < 3; i++) { S.tick(1000); S.logDone(id1, ans, i ? '' : 'typed', i === 1 ? 0 : null); }
  S.logDone(bad, ans, 'typed', null);
  S.logDone(id1, null, 'file', 'no answers');
  S.logDone(id1, { lines: 'x', answered: 'many' }, 'file', '');
  S.logDone(id1, { lines: [1, null, 'x'], answered: '3' }, 'file', 'odd lines');
  // same size, same mtime: not read again; a newer mtime: read
  const m = S.state(sid, st);
  S.scan();
  const st2 = JSON.parse(JSON.stringify(st)); st2.session_name = 'TEST-A';
  S.state(sid, st2, m); S.scan();
  S.state(sid, st2, m + 1000); S.scan();
  S.start();                                                  // a restart: the log survives
  S.scan();
  // deck.json
  S.deckFile();
  S.scan();
  for (const v of [{}, null, { deckThreshold: 3, picker: 'alongside', usePages: false, background: true }, { deckThreshold: 2.5 }, { deckThreshold: -1, picker: 'PANE' }, { deckThreshold: '4', usePages: 0, background: null },
    { deckThreshold: 7.0, picker: 'pane', usePages: 'no' }]) { S.settings(v); S.tick(30000); S.deckFile(); }
  S.removeDeckFile();
  S.removeDeckFile();
  S.rm(path.join(S.stateDir, `${sid}.json`)); S.scan();       // the session's file gone: dropped
});

scenario('replace', { live: true, downloads: ['dl'] }, S => {
  const sA = 'cccccccc-1111-2222-3333-444444444444', sB = 'dddddddd-1111-2222-3333-444444444444';
  const pA = 'ask_test-c_cccccccc_r1_1110-1300_aaa1', pB = 'ask_test-c_cccccccc_r2_1110-1310_bbb2', pC = 'ask_test-d_dddddddd_r1_1110-1320_ccc3';
  S.watch(pA, pB, pC);
  const A1 = { round: 1, id: pA, page: S.page(pA), created: '2026-11-10T13:00:00.000-05:00', status: 'open', source: 'skill',
    items: [card('Q1', 'r1-Q1', 'fix timing', opts2), card('Q2', 'r1-Q2', 'reorder', [{ key: 'a', label: 'skip' }, { key: 'b', label: 'order' }]), card('Q3', 'r1-Q3', 'printer', opts2),
      card('Q4', 'r1-Q4', 'only a pick', [{ key: 'x', label: 'old x' }]), card('Q5', 'r1-Q5', 'reply button', opts2), card('Q6', 'r1-Q6', 'dropped card', opts2),
      card('Q7', 'r1-Q7', 'later', opts2), card('Q8', 'r1-Q8', 'multi', opts2, { multi: true }), card('Q9', 'r1-Q9', 'nothing entered', opts2)] };
  const B2 = { round: 2, id: pB, page: S.page(pB), created: '2026-11-10T13:10:00.000-05:00', status: 'open', source: 'skill',
    items: [card('Q1', 'r2-Q1', 'fix timing', opts2), card('Q2', 'r2-Q2', 'reorder, smaller', [{ key: 'a', label: '50 MB' }, { key: 'b', label: '100 MB' }]), card('Q3', 'r2-Q3', 'printer', opts2),
      card('Q4', 'r2-Q4', 'only a pick', [{ key: 'x', label: 'new x' }]), card('Q5', 'r2-Q5', 'reply button', [{ key: 'z', label: 'other' }]), card('Q7', 'r2-Q7', 'later', opts2),
      card('Q8', 'r2-Q8', 'multi', opts2, { multi: true }), card('Q9', 'r2-Q9', 'nothing entered', opts2), card('Q10', 'r2-Q10', 'new one', opts2)],
    inherits: { [pA]: { Q1: 'Q1', Q2: 'Q2', Q3: 'Q3', Q4: 'Q4', Q5: 'Q5', Q6: 'Q6', Q7: 'Q7', Q8: 'Q8', Q9: 'Q9' }, 'not-a-page-id': { a: 'b' } } };
  const stA = { session_id: sA, session_name: 'test-c', account: 'thirdacct', answers: {}, rounds: [A1, B2] };
  S.state(sA, stA);
  S.live('set', [sA]);
  S.start(); S.scan();
  S.setDraft(pA, { state: { Q1: { text: 'after Alex - ping him', pick: 'b' }, Q2: { text: 'only if it is cheap\n\nand quick', pick: 'a' }, Q3: { text: 'Saturday', pick: 'b' },
    Q4: { text: '', pick: 'x' }, Q5: { text: '', pick: 'r:yourcall' }, Q6: { text: 'drop this', pick: 'r:drop' }, Q7: { text: '', pick: null, later: true },
    Q8: { text: '', pick: ['a', 'b'] }, Q9: { text: '   ', pick: null } }, drafted: 8, total: 9 });
  S.setDraft(pB, { state: { Q3: { text: 'tonight after all', pick: 'a' } }, drafted: 1, total: 9 });
  S.tick(5000);
  A1.status = 'carried'; A1.superseded_by = pB; A1.carried_into = pB; S.state(sA, stA); S.scan();   // the skill pushes r1 out
  S.scan();
  // a chain: r2 pushed out by another session's page before they sent it
  const C1 = { round: 1, id: pC, page: S.page(pC), created: '2026-11-10T13:20:00.000-05:00', status: 'open', source: 'skill',
    items: [card('Q1', 'r1-Q1', 'fix timing', opts2), card('Q2', 'r1-Q2', 'new one', opts2), card('Q3', 'r1-Q3', 'multi', opts2)], inherits: { [pB]: { Q1: 'Q1', Q10: 'Q2', Q8: 'Q3', Q5: 'Q99' } } };
  S.state(sB, { session_id: sB, session_name: 'test-d', account: 'vv', answers: {}, rounds: [C1] });
  S.tick(61000);
  B2.status = 'superseded'; B2.superseded_by = pC; S.state(sA, stA); S.scan();
  // its session: live, gone, the read throwing; read at most every 15 s
  S.live('set', [sA, sB]); S.tick(14000); S.tick(1000); S.tick(1);
  S.live('set', [sA]); S.tick(15001);
  S.live('throw'); S.tick(15001); S.tick(1);
  S.live('set', []); S.tick(1);
  // a replaced round they had partly answered (read), in Recently answered: labelled with what replaced it
  A1.read_at = iso(S.clock - MIN); stA.answers['r1-Q1'] = { status: 'answered', answer: 'after Alex', choice: 'b', reply: null, via: 'file' };
  S.state(sA, stA); S.scan();
  S.deckFile(); S.removeDeckFile();
});

// fix 8 (2026-11-20): a SHIFTED inherits map (the session counted the old cards without their group letters): the
// draft move puts their words on the card with their own title, and none on a card that is another old card's
scenario('shifted', { live: true }, S => {
  const sS = '99999999-1111-2222-3333-444444444444';
  const p1 = 'ask_test-s_99999999_r1_1120-2245_aaa1', p2 = 'ask_test-s_99999999_r2_1120-2248_bbb2';
  S.watch(p1, p2);
  const on = [{ key: 'a', label: 'Turn off at startup' }, { key: 'b', label: 'Keep starting' }];
  const R1 = { round: 1, id: p1, page: S.page(p1), created: '2026-11-20T22:45:00.000-05:00', status: 'open', source: 'skill',
    items: [card('Q1', 'r1-Q1', 'Copilot app at startup', on), card('Q2', 'r1-Q2', 'Steam  at Startup', on), card('Q3a', 'r1-Q3a', 'Adobe CC at startup', on),
      card('Q3b', 'r1-Q3b', 'Adobe CCX at startup', on), card('Q4', 'r1-Q4', 'Redragon app at startup', on), card('Q5', 'r1-Q5', 'WhatsApp at startup', on),
      card('Q6', 'r1-Q6', 'Security badge', on), card('Q7', 'r1-Q7', 'Reworded card', on)] };
  const R2 = { round: 2, id: p2, page: S.page(p2), created: '2026-11-20T22:48:00.000-05:00', status: 'open', source: 'skill',
    items: [card('Q1', 'r2-Q1', 'BitLocker key', on), card('Q2', 'r2-Q2', 'Copilot app at startup', on), card('Q3', 'r2-Q3', 'steam at startup', on),
      card('Q4a', 'r2-Q4a', 'Adobe CC at startup', on), card('Q4b', 'r2-Q4b', 'Adobe CCX at startup', on), card('Q5', 'r2-Q5', 'Redragon app at startup', on),
      card('Q6', 'r2-Q6', 'WhatsApp at startup', on), card('Q7', 'r2-Q7', 'A new title for it', on), card('r1-Q3a', 'r1-Q3a', 'Adobe CC at startup', on)],
    inherits: { [p1]: { Q3a: 'r1-Q3a', Q1: 'Q2', Q2: 'Q3', Q4: 'Q4b', Q5: 'Q5', Q6: 'Q6', Q7: 'Q7' } } };
  const st = { session_id: sS, session_name: 'test-s', account: 'thirdacct', answers: {}, rounds: [R1, R2] };
  S.state(sS, st);
  S.live('set', [sS]);
  S.start(); S.scan();
  S.setDraft(p1, { state: { Q1: { text: 'Turn off at startup', pick: 'a' }, Q2: { text: 'nah', pick: null }, Q3a: { text: 'Keep starting - how much impact?', pick: 'b' },
    Q4: { text: 'Turn off at startup - no macros, right?', pick: 'a' }, Q5: { text: 'Keep starting', pick: 'b' }, Q6: { text: 'what is the badge?', pick: null },
    Q7: { text: 'reworded, still mine', pick: 'b' } }, drafted: 7, total: 8 });
  S.tick(5000);
  R1.status = 'superseded'; R1.superseded_by = p2; S.state(sS, st); S.scan();
  S.scan();
});

scenario('hold', {}, S => {
  const sE = 'eeeeeeee-1111-2222-3333-555555555555', sF = 'ffffffff-1111-2222-3333-555555555555';
  const pE = 'ask_test-e_eeeeeeee_r1_1110-1400_eee1', pF = 'ask_test-f_ffffffff_r1_1110-1401_fff1', pG = 'ask_test-e_eeeeeeee_r2_1110-1402_eee2', pH = 'ask_test-e_eeeeeeee_r3_1110-1403_eee3',
    pI = 'ask_test-e_eeeeeeee_r4_1110-1404_eee4', pJ = 'ask_test-e_eeeeeeee_r5_1110-1405_eee5', pK = 'ask_test-e_eeeeeeee_r6_1110-1406_eee6';
  S.watch(pE, pF, pG, pH, pI, pJ, pK);
  const E1 = { round: 1, id: pE, page: S.page(pE), created: '2026-11-10T14:00:00.000-05:00', status: 'open', source: 'skill', items: [card('Q1', 'r1-Q1', 'fix timing', opts2)] };
  const stE = { session_id: sE, session_name: 'test-e', account: 'thirdacct', answers: {}, rounds: [E1] };
  S.state(sE, stE); S.start(); S.scan();
  S.setDraft(pE, { state: { Q1: { text: 'after Alex', pick: 'b' } }, drafted: 1, total: 1 });
  E1.status = 'superseded'; E1.superseded_by = pF; S.state(sE, stE); S.scan();     // the newer page not there yet: held
  S.tick(MIN);
  S.state(sF, { session_id: sF, session_name: 'test-f', account: 'vv', answers: {}, rounds: [
    { round: 1, id: pF, page: S.page(pF), created: '2026-11-10T14:01:00.000-05:00', status: 'open', source: 'skill', items: [card('Q1', 'r1-Q1', 'fix timing', opts2)], inherits: { [pE]: { Q1: 'Q1' } } }] });
  S.scan();                                                    // it appears: they move over
  const E2 = { round: 2, id: pG, page: S.page(pG), created: '2026-11-10T14:02:00.000-05:00', status: 'open', source: 'skill', items: [card('Q1', 'r2-Q1', 'x', opts2), card('Q2', 'r2-Q2', '', opts2)] };
  stE.rounds.push(E2); S.state(sE, stE); S.scan();
  S.setDraft(pG, { state: { Q1: { text: 'keep me', pick: null }, Q2: { text: '', pick: 'b' } }, drafted: 2, total: 2 });
  E2.status = 'superseded'; E2.superseded_by = 'ask_test-z_99999999_r1_1110-1403_9999'; S.state(sE, stE); S.scan();
  S.tick(9 * MIN); S.scan();
  S.tick(MIN - 1); S.scan();                                   // 10 minutes less a millisecond: still held
  S.tick(1); S.scan();                                         // dropped, after a DRAFT copy is saved
  // closed without being replaced or read: a copy says its status; no status at all reads "closed"
  const E3 = { round: 3, id: pH, page: S.page(pH), created: '2026-11-10T14:03:00.000-05:00', status: 'open', source: 'skill', items: [card('Q1', 'r3-Q1', 'reply', opts2), card('Q2', 'r3-Q2', 'none', opts2),
    card('Q3', 'r3-Q3', 'bogus', opts2), card('Q4', 'r3-Q4', 'explain', opts2), card('Q5', 'r3-Q5', 'label of 1', [{ key: 1, label: 'one' }, { key: 'b', label: '' }]), card('Q6', 'r3-Q6', 'none + words', opts2),
    card('Q7', 'r3-Q7', 'later with words', opts2), card('Q8', 'r3-Q8', 'empty list', opts2), card(undefined, 'r3-Q9', 'no qid')] };
  const E4 = { round: 4, id: pI, page: S.page(pI), created: '2026-11-10T14:04:00.000-05:00', status: 'open', items: [card('Q1', 'r4-Q1', 't')] };
  const E5 = { round: 5, id: pJ, page: S.page(pJ), created: '2026-11-10T14:05:00.000-05:00', status: 'open', items: [card('Q1', 'r5-Q1', 't')] };
  stE.rounds.push(E3, E4, E5); S.state(sE, stE); S.scan();
  S.tick(3723000);
  S.setDraft(pH, { state: { Q1: { text: '', pick: 'r:workaround' }, Q2: { text: '', pick: 'r:none' }, Q3: { text: '', pick: 'r:bogus' }, Q4: { text: 'why?', pick: 'r:explain' },
    Q5: { text: '', pick: [1, 'b', 'zz'] }, Q6: { text: 'none of them', pick: 'r:none' }, Q7: { text: 'next week', pick: null, later: true }, Q8: { text: '', pick: [], later: 1 } }, drafted: 8, total: 9 });
  S.setDraft(pI, { state: { Q1: { text: 'caf\u00e9 \ud83d\ude00', pick: null } }, drafted: 1, total: 1 });
  S.setDraft(pJ, { state: { Q1: { text: 'read elsewhere', pick: null } }, drafted: 1, total: 1 });
  E3.status = 'answered'; delete E4.status; E5.status = 'partial'; E5.read_at = iso(S.clock);
  stE.answers['r5-Q1'] = { status: 'later', answer: '', via: 'paste' };
  S.state(sE, stE); S.scan();
  // a round that is GONE with drafts on it: the copy goes to Downloads; a draft with nothing entered leaves none
  const E6 = { round: 6, id: pK, page: S.page(pK), created: '2026-11-10T14:06:00.000-05:00', status: 'open', items: [card('Q1', 'r6-Q1', 't'), card('Q2', 'r6-Q2', 'u')] };
  stE.rounds = [E6]; S.state(sE, stE); S.scan();
  S.setDraft(pK, { state: { Q1: { text: 'first', pick: null }, Q2: { text: '', pick: 'r:after' } }, drafted: 2, total: 2 });
  S.tick(1000);
  S.rm(path.join(S.stateDir, `${sE}.json`)); S.scan();
  S.state(sE, stE); S.scan();
  S.setDraft(pK, { state: { Q1: { text: '  ', pick: null } }, drafted: 1, total: 2 });
  E6.status = 'answered'; S.state(sE, stE); S.scan();
});

scenario('chain', {}, S => {
  const s = '11111111-aaaa-bbbb-cccc-000000000001';
  const p1 = 'ask_chain_11111111_r1_1110-1500_0001', p2 = 'ask_chain_11111111_r2_1110-1501_0002', p3 = 'ask_chain_11111111_r3_1110-1502_0003';
  const q1 = 'ask_chain_11111111_r4_1110-1503_0004', q2 = 'ask_chain_11111111_r5_1110-1504_0005', q3 = 'ask_chain_11111111_r6_1110-1505_0006';
  S.watch(p1, p2, p3, q1, q2, q3);
  const other = [{ key: 'a', label: 'tonight' }, { key: 'b', label: 'tomorrow' }];
  const R = (n, id, created, o, extra) => Object.assign({ round: n, id, page: S.page(id), created, status: 'open', source: 'skill', items: [card('Q1', `r${n}-Q1`, 'when', o), card('Q2', `r${n}-Q2`, 'who', o)] }, extra);
  const r1 = R(1, p1, '2026-11-10T15:00:00.000-05:00', opts2), r2 = R(2, p2, '2026-11-10T15:01:00.000-05:00', opts2, { inherits: { [p1]: { Q1: 'Q1', Q2: 'Q2' } } }),
    r3 = R(3, p3, '2026-11-10T15:02:00.000-05:00', opts2, { inherits: { [p2]: { Q2: 'Q2', Q1: 'Q1' } } });
  // the same chain with the options changed at each step, and the rounds listed newest first in the file
  const t1 = R(4, q1, '2026-11-10T15:03:00.000-05:00', opts2), t2 = R(5, q2, '2026-11-10T15:04:00.000-05:00', other, { inherits: { [q1]: { Q1: 'Q1', Q2: 'Q2' } } }),
    t3 = R(6, q3, '2026-11-10T15:05:00.000-05:00', opts2, { inherits: { [q2]: { Q1: 'Q1', Q2: 'Q2' } } });
  const st = { session_id: s, session_name: 'chain', account: '', answers: {}, rounds: [r1, r2, r3, t3, t2, t1] };
  S.state(s, st); S.start(); S.scan();
  S.setDraft(p1, { state: { Q1: { text: 'words', pick: 'b' }, Q2: { text: '', pick: 'a' } }, drafted: 2, total: 2 });
  S.setDraft(q1, { state: { Q1: { text: 'words', pick: 'b' }, Q2: { text: '', pick: 'a' } }, drafted: 2, total: 2 });
  S.setDraft(q2, { state: { Q2: { text: 'mine already', pick: null } }, drafted: 1, total: 2 });
  S.tick(2000);
  // all pushed out at once
  r1.status = 'carried'; r1.superseded_by = p2; r2.status = 'superseded'; r2.superseded_by = p3;
  t1.status = 'carried'; t1.superseded_by = q2; t2.status = 'superseded'; t2.superseded_by = q3;
  S.state(s, st); S.scan(); S.scan();
  S.markSent([p3], { how: 'file' });
  S.clearDraft(p3);
  S.clearDraft(q3);
});

scenario('sent', {}, S => {
  const s = '22222222-aaaa-bbbb-cccc-000000000002';
  const p1 = 'ask_sent_22222222_r1_1110-1600_0001', p2 = 'ask_sent_22222222_r2_1110-1601_0002', p3 = 'ask_sent_22222222_r3_1110-1602_0003', nope = 'ask_sent_22222222_r9_1110-1609_0009';
  S.watch(p1, p2, p3, nope);
  const R = (n, id) => ({ round: n, id, page: S.page(id), created: `2026-11-10T16:0${n}:00.000-05:00`, status: 'open', source: n === 2 ? 'picker' : 'skill', background: n === 2, title: n === 3 ? 'Three' : undefined,
    items: [card('Q1', `r${n}-Q1`, 't', opts2)] });
  const st = { session_id: s, session_name: 'sent', account: 'demo', answers: {}, rounds: [R(1, p1), R(2, p2), R(3, p3)] };
  S.state(s, st); S.start(); S.scan();
  S.markSent([p1, nope], { how: 'file', heldId: 'h1' });      // an id that is not waiting goes at once, with no onRead
  S.tick(1000);
  S.markSent([p2], { how: 'file', at: S.clock - HOUR, heldId: 'h2' });   // info can carry its own `at`
  S.markSent([p3], null);
  S.setDraft(p3, { state: { Q1: { text: 'sent ones get no copy', pick: null } }, drafted: 1, total: 1 });
  S.at(T0 + 5 * HOUR + 1000); S.scan();                        // p2: 6 h exactly: kept
  S.tick(1); S.scan();                                         // p2: a millisecond over: the mark goes (not read: no onRead)
  S.at(T0 + 6 * HOUR); S.scan();                               // p1: exactly 6 h
  S.tick(1); S.scan();                                         // p1 goes
  // p3 answered and read: its mark goes with onRead only when a line was held
  S.markSent([p1], { how: 'file', heldId: 'h1b' }); S.markSent([p2], { how: 'file' });
  st.rounds[0].status = 'answered'; st.rounds[0].read_at = iso(S.clock); st.rounds[1].status = 'answered'; st.rounds[1].read_at = iso(S.clock); st.rounds[2].status = 'answered';
  st.answers = { 'r1-Q1': { status: 'answered', answer: 'tonight', choice: 'a', via: 'file' }, 'r2-Q1': { status: 'dropped', answer: '', reply: 'drop', via: 'picker' } };
  S.state(s, st); S.scan(); S.scan();
});

scenario('log', {}, S => {
  const s = '33333333-aaaa-bbbb-cccc-000000000003', s2 = '44444444-aaaa-bbbb-cccc-000000000004';
  const pid = i => `ask_log_33333333_r${i}_1110-1700_${(0x1000 + i).toString(16)}`;
  const many = { session_id: s, session_name: 'log \ud83d\ude00 caf\u00e9', account: 'thirdacct', answers: {}, rounds: [] };
  const add = (i, readAt, answer, extra) => {
    many.rounds.push(Object.assign({ round: i, id: pid(i), page: path.join(S.pages, `${pid(i)}.html`), created: iso(T0 - DAY), items: [card('Q1', `r${i}-Q1`, 't', opts2)], status: 'answered', source: 'skill', read_at: readAt }, extra));
    if (answer) many.answers[`r${i}-Q1`] = answer;
  };
  S.watch(pid(1), pid(10), pid(11), pid(12), pid(60), pid(61), pid(62), pid(63), pid(70));
  S.page(pid(60));                                             // one page exists: its log row carries a url
  for (let i = 1; i <= 60; i++) add(i, iso(T0 - (60 - i) * MIN), { status: 'answered', answer: 'yes', choice: 'a', reply: null, via: 'file' });
  S.state(s, many); S.start(); S.scan();
  // the log is full: one read longer ago than its last entry is not added; a newer one pushes the oldest out
  add(61, iso(T0 - 2 * HOUR), { status: 'answered', answer: 'old', via: 'paste' });
  add(62, iso(T0 + MIN), { status: 'later', answer: '', via: 'paste' });
  add(63, '2026-11-10T18:02:00Z', { status: 'answered', answer: 'utc stamp', via: 'picker' });
  S.state(s, many); S.scan();
  // statuses and vias; LOG_DAYS; read_at shapes
  const st2 = { session_id: '', session_name: '', answers: {}, rounds: [] };
  const R = (n, readAt, items, extra) => { st2.rounds.push(Object.assign({ round: n, id: `ask_log2_44444444_r${n}_1110-1800_${(0x2000 + n).toString(16)}`, created: iso(T0), items, status: 'answered', read_at: readAt }, extra)); };
  const I = (n, k, title) => ({ qid: `Q${k}`, key: `r${n}-Q${k}`, title });
  R(1, iso(T0 + 10 * MIN), [I(1, 1, 'answered'), I(1, 2, 'dropped'), I(1, 3, 'later, no words'), I(1, 4, 'skipped'), I(1, 5, 'no answer at all'), I(1, 6, 'later, with words'), I(1, 7, 'a number'), { key: 'r1-Q8' }]);
  Object.assign(st2.answers, { 'r1-Q1': { status: 'answered', answer: 'tonight', choice: 'a', via: 'file' }, 'r1-Q2': { status: 'dropped', answer: '', reply: 'drop', via: 'file' },
    'r1-Q3': { status: 'later', via: 'file' }, 'r1-Q4': { status: 'skipped', answer: 'never shown' }, 'r1-Q6': { status: 'later', answer: 'next week', reply: 'remind', choice: null },
    'r1-Q7': { status: 'answered', answer: 12, choice: ['a', 'b'] }, 'r1-Q8': { status: 'answered', answer: 'no qid, no title', choice: 0 } });
  R(2, iso(T0 + 11 * MIN), [I(2, 1, 'paste')]); st2.answers['r2-Q1'] = { status: 'answered', answer: 'typed', via: 'paste' };
  R(3, iso(T0 + 12 * MIN), [I(3, 1, 'no via')]); st2.answers['r3-Q1'] = { status: 'answered', answer: 'plain' };
  R(4, iso(T0 + 13 * MIN), [I(4, 1, 'picker beats paste'), I(4, 2, 'x')]); st2.answers['r4-Q1'] = { status: 'answered', answer: 'a', via: 'paste' }; st2.answers['r4-Q2'] = { status: 'answered', answer: 'b', via: 'picker' };
  R(5, iso(T0 + 14 * MIN), [I(5, 1, 'only skipped')]); st2.answers['r5-Q1'] = { status: 'skipped' };
  R(6, iso(T0 - 14 * DAY - 1), [I(6, 1, 'too long ago')]); st2.answers['r6-Q1'] = { status: 'answered', answer: 'old' };
  R(7, 'not a date', [I(7, 1, 'bad stamp')]); st2.answers['r7-Q1'] = { status: 'answered', answer: 'x' };
  R(8, '2026-11-10T13:15:00.123456-05:00', [I(8, 1, 'python microseconds')], { superseded_by: `ask_log2_44444444_r9_1110-1800_${(0x2009).toString(16)}`, status: 'superseded' }); st2.answers['r8-Q1'] = { status: 'answered', answer: 'x', via: 'file' };
  R(9, null, [I(9, 1, 'the one that replaced r8')], { status: 'open', page: path.join(S.dir, 'pages', 'sub dir #1', `ask_log2_44444444_r9_1110-1800_2009.html`) });
  R(10, '2026-11-10T18:16:00Z', [I(10, 1, 'long lines')], { superseded_by: 'ask_gone_44444444_r1_1110-1800_0000', page: S.dl });
  st2.answers['r10-Q1'] = { status: 'answered', answer: 'x'.repeat(380) + '\u00e9'.repeat(30) + ' tail', via: 'file' };
  S.put(path.join(S.dir, 'pages', 'sub dir #1', 'ask_log2_44444444_r9_1110-1800_2009.html'), '<html/>');
  S.state(s2, st2); S.at(T0 + 20 * MIN); S.scan();
  for (const r of st2.rounds) S.watch(r.id);
  S.scan();
  // Done with many lines: the view shows 40, each cut at 400
  const lines = []; for (let i = 0; i < 45; i++) lines.push(`Q${i + 1} (t): ` + (i === 3 ? 'y'.repeat(390) + '\ud83d\ude00\ud83d\ude00 and more' : 'ok'));
  const p9 = st2.rounds[8].id;
  S.setDraft(p9, { state: { Q1: { text: 'on the page', pick: null } }, drafted: 1, total: 1 });
  S.logDone(p9, { lines, answered: 45 }, 'file', 'Forty-five');
  S.start(); S.scan();                                         // the drafts file read back: 50 at most
});

scenario('live absent', { settings: null }, S => {
  const s = '55555555-aaaa-bbbb-cccc-000000000005', p = 'ask_plain_55555555_r1_1110-1900_0001', p2 = 'ask_plain_55555555_r2_1110-1901_0002', p3 = 'ask_plain_55555555_r3_1110-1902_0003';
  S.watch(p, p2, p3);
  // odd rounds: no round number, no status / created / title, an id that is not one, items missing, page not an ask_*.html, a ~ page
  const st = { rounds: [null, 5, 'x', { id: 'nope', status: 'open' }, { id: 7 }, { id: p, status: 'open', page: path.join(S.pages, 'ASK_Upper.HTML'), inherits: [1, 2] },
    { id: p2, status: 'partial', created: 5, title: 7, source: 0, background: 'yes', items: [{ qid: 1, title: null, key: 9 }, 'str', 4, { qid: 'Q2' }], page: path.join(S.pages, 'notanask.html'),
      inherits: { [p]: 'abc', [p3]: null, 'ask_other_55555555_r1_1110-1900_0009': { a: 1, b: 2 } } },
    { id: p3, status: 'partial', read_at: 0, created: '2026-11-10T19:02:00-05:00', page: path.join(S.pages, 'sub', '..', `${p3}.html`), items: 'none' }], answers: { 9: { status: 'answered' } } };
  S.put(path.join(S.pages, 'ASK_Upper.HTML'), 'x'); S.put(path.join(S.pages, 'notanask.html'), 'x'); S.page(p3);
  S.state(s, st); S.start(); S.scan();
  S.deckFile();
  S.setDraft(p2, { state: { 1: { text: 'numbered card', pick: null }, Q2: { text: '', pick: 'k' } }, drafted: 2, total: 4 });
  S.answers(p3, { kind: 'ask-page-answers', id: p3 });
  S.tick(HOUR);
  st.rounds[6].status = 'done'; S.state(s, st); S.scan();
});

// ---------------------------------------------------------------- DRAFT_MAX: JSON.stringify(state).length
const maxFeed = new A.AskFeed({ stateDir: path.join(root, 'max-state'), downloads: [], draftsFile: '', now: () => T0, settings: () => ({}) });
const draftMax = [['x', 10], ['x', 199982], ['x', 199983], ['\u00e9', 199982], ['\u00e9', 199983], ['\ud83d\ude00', 99991], ['\ud83d\ude00', 99992], ['"', 99991], ['"', 99992], ['\n', 99991], ['\n', 99992], ['\u0001', 33330], ['\u0001', 33331]]
  .map(([ch, n]) => { const state = { Q1: { text: ch.repeat(n) } }; return { ch, n, length: JSON.stringify(state).length, ret: maxFeed.setDraft(id1, { state, drafted: 1 }) }; });

// ---------------------------------------------------------------- write it, with the temp root rewritten
fs.rmSync(root, { recursive: true, force: true });
const esc = s => s.replace(/\\/g, '\\\\');
const swaps = [[esc(esc(root)), esc(esc(FX_ROOT))], [esc(root), esc(FX_ROOT)], [root, FX_ROOT], [root.replace(/\\/g, '/'), FX_ROOT.replace(/\\/g, '/')]];
const fix = v => {
  if (typeof v === 'string') { for (const [a, b] of swaps) v = v.split(a).join(b); return v; }
  if (Array.isArray(v)) return v.map(fix);
  if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).map(([k, x]) => [fix(k), fix(x)]));
  return v;
};
const doc = fix({ source: 'lib/asks.js', node: process.version, pinned: { tz: 'America/Bogota', offsetMs: -5 * HOUR, pid: PID, t0: T0, root: FX_ROOT, home: path.join(FX_ROOT, 'home'),
  note: 'the temp root is rewritten to `root`; every synthetic file carries its pinned mtimeMs; setTimeout is captured and run at the end of each step; in a step\'s `after`, "=" means the same as the step before' },
  beat: A.BEAT_S, ids, composeDone: doneCases, composePointer: pointerCases, picker: pickerCases, newAskRows: rowSeqs, urls, locations: locCases, draftMax, scenarios });
const text = JSON.stringify(doc);
if (text.includes(path.basename(root))) throw new Error('the temp root survived in the fixture');
const out = path.join(__dirname, '..', 'fixtures', 'deck', 'asks.json');
fs.writeFileSync(out, text + '\n');
const steps = scenarios.reduce((n, s) => n + s.steps.length, 0);
console.log(`wrote asks -> ${out} (${fs.statSync(out).size} bytes; ${scenarios.length} scenarios, ${steps} steps, ${urls.length} urls)`);
