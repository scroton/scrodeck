'use strict';
// Generate core/test/fixtures/deck/viewer.json from the REAL lib/viewer.js (required as it is), the /view/ route's
// constants in lib/phone.js and main.js cleanPages (neither is exported: their source text is taken out of the
// file and run as it stands).
//   node core/test/deck/gen-viewer.js
// Three worlds, the same script in each:
//   real   lib/viewer.js as the deck loads it, against a temp folder of synthetic files with pinned mtimes
//          (fs.mkdtempSync under os.tmpdir(), removed afterwards). Its root is recorded: the C++ test loads the
//          same files into fake::MemFiles at the same paths.
//   win    lib/viewer.js loaded with `fs` replaced by a map of synthetic files (statSync only), path = path.win32
//   posix  the same with path = path.posix and platform 'linux': what the deck does on a Mac
// Nothing is read from the real home: `home` is always passed. No local-time date is used (every timestamp has a
// zone), so no TZ is pinned.
const fs = require('fs');
const os = require('os');
const path = require('path');
const Module = require('module');

const REPO = path.join(__dirname, '..', '..', '..');
const VIEWER = path.join(REPO, 'lib', 'viewer.js');
const AT = Date.parse('2026-11-17T10:00:00.000Z');       // the tool output's time in the transcripts
const MIN = 60000;

// ---------------------------------------------------------------- lib/viewer.js, three ways
function loadWith(file, mocks) {
  const orig = Module.prototype.require;
  Module.prototype.require = function (id) { return Object.prototype.hasOwnProperty.call(mocks, id) ? mocks[id] : orig.apply(this, arguments); };
  delete require.cache[require.resolve(file)];
  try { return require(file); } finally { Module.prototype.require = orig; delete require.cache[require.resolve(file)]; }
}
function fakeFs(win) {
  const store = new Map();                               // key -> { path, dir, mtimeMs, size }
  const key = p => (win ? p.toLowerCase() : p);
  const P = win ? path.win32 : path.posix;
  return {
    store,
    add(p, e) { store.set(key(p), { path: p, ...e }); for (let d = P.dirname(p); !store.has(key(d)); d = P.dirname(d)) { store.set(key(d), { path: d, dir: true, implied: true, mtimeMs: 0, size: 0 }); if (P.dirname(d) === d) break; } },
    statSync(p) {
      const e = store.get(key(p));
      if (!e) { const err = new Error(`ENOENT: no such file or directory, stat '${p}'`); err.code = 'ENOENT'; throw err; }
      return { isFile: () => !e.dir, isDirectory: () => !!e.dir, mtimeMs: e.mtimeMs, size: e.size };
    },
  };
}
const Vreal = require(VIEWER);
const winFs = fakeFs(true), posixFs = fakeFs(false);
const Vwin = loadWith(VIEWER, { fs: winFs });
const Vposix = loadWith(VIEWER, { fs: posixFs, path: path.posix });

function rng(seed) { let a = seed >>> 0; return () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
const pick = (r, list) => list[Math.floor(r() * list.length)];
const fuzz = (r, tokens, min, max) => { let s = ''; const n = min + Math.floor(r() * (max - min + 1)); for (let i = 0; i < n; i++) s += pick(r, tokens); return s; };

// ---------------------------------------------------------------- pathsIn
const pathTexts = [
  'Page: `D:\\scratchdir\\26-11-17 X\\page.html` - open it', 'Page: `/Users/me/26-11-17 X/page.html` - open it',
  'Written to C:\\a\\b.html.', 'C:/a/b.html and /c/Users/me/x.htm and ~/y/z.html', 'C:\\a\\one.html, C:\\a\\two.html',
  'see https://example.com/a/b.html', 'http://host:8788/view/x.html', 'open report.html beside it', 'C:\\a\\b.txt and C:\\a\\c.htmlx',
  '', 'no page here', '.htm', '.html', 'C:\\.html', 'C:\\.htm', '/.html', '~/.htm', '~\\x.html', 'C:\\a\\b.HTML', 'C:\\a\\b.Html and C:\\a\\c.html',
  'C:\\a\\b.html', ' C:\\a\\b.html', 'xC:\\a\\b.html', '(C:\\a\\b.html)', '[C:\\a\\b.html]', '"C:\\a\\b.html"', "'C:\\a\\b.html'", '=C:\\a\\b.html;', 'href=C:\\a\\b.html&x', '<a>C:\\a\\b.html</a>',
  'a:C:\\a\\b.html', 'x,C:\\a\\b.html,y', 'C:\\a\\b.html!', 'C:\\a\\b.html?', 'C:\\a\\b.html: done', 'C:\\a\\b.html. Next', 'C:\\a\\b.html.\nNext', 'C:\\a\\b.html.x', 'C:\\a\\b.html..', 'C:\\a\\b.html. ',
  'C:\\a\\b.htmlC:\\c.html', 'C:\\a\\b.htm.html', 'C:\\a\\b.html.html', 'C:\\a\\b.html.htm x', 'C:\\a\\b.htm and.html', 'C:\\a b\\c d.html and more.html here',
  '//server/share/x.html', ' //server/share/x.html', '/a//b.html', 'x /a/b.html', 'x/a/b.html', '\n/a/b.html\n/c/d.htm\r\n~/e.html\r~/f.htm', 'line\u2028/a/b.html\u2029/c/d.html', '\u00a0/a/b.html\u3000C:/x.htm\ufeff~/y.html',
  'C:\\a\\b.html\u00a0next', 'C:\\a\\b.html\u2028next', 'C:\\a\\b.html.\u2028next', 'C:\\a\\b.html.\u00a0', 'C:\\a\\b.html\ttab', 'C:\\a\\b.html\u200bzero',
  'C:\\a|b.html', 'C:\\a*b.html', 'C:\\a?b.html', 'C:\\a<b.html', 'C:\\a>b.html', 'C:\\a>C:\\b.html', 'C:\\a"b.html', "C:\\a'b.html", 'C:\\a`b.html', 'C:\\a\nb.html', 'C:\\a\rb.html',
  'C:\\' + 'x'.repeat(300) + '.html', 'C:\\' + 'x'.repeat(301) + '.html', 'C:\\' + 'x'.repeat(299) + '.html', '/' + 'y'.repeat(300) + '.htm', '/' + 'y'.repeat(301) + '.htm', '~/' + 'z'.repeat(300) + '.html', '~/' + 'z'.repeat(301) + '.html',
  'C:\\' + '\u00e9'.repeat(300) + '.html', 'C:\\' + '\u65e5'.repeat(300) + '.html', 'C:\\' + '\u65e5'.repeat(301) + '.html', 'C:\\' + '\ud83d\ude00'.repeat(150) + '.html', 'C:\\' + '\ud83d\ude00'.repeat(151) + '.html', 'C:\\x' + '\ud83d\ude00'.repeat(150) + '.html', 'C:\\x' + '\ud83d\ude00'.repeat(149) + 'y.html',
  'C:\\' + 'x'.repeat(301) + '.html and C:\\ok.html', 'C:\\' + 'x'.repeat(301) + ' C:\\ok.html', 'C:\\a.htm' + 'x'.repeat(296) + '.html', 'C:\\a.htm' + 'x'.repeat(290) + '.html',
  'C:\\\u65e5\u672c\u8a9e\\\u30da\u30fc\u30b8.html\u3002', 'C:\\\u65e5\u672c\u8a9e\\\u30da\u30fc\u30b8.html \u3002', '\ud83d\ude00C:\\a\\b.html', '\ud83d\ude00 C:\\a\\b.html\ud83d\ude00', 'C:\\a\\\ud83d\ude00.html', '\u00e9:/x.html', 'C:/\u00e9/\u00fc.htm,',
  'Z:/x.html z:\\y.htm 1:/n.html _:/u.html', 'C:x.html', 'C:\\\\x.html', 'C://x.html', '~x/y.html', '~/~/y.html', '~/', '/', 'C:\\', '/.htm.', '/.html.html.', 'a /b.htmlc /d.html',
  'tabs\tC:\\t.html\tend', 'eq=~/q.html;semi;/s.html:colon:/k.htm', '>C:\\gt.html<', 'a>/gt.html', 'x\n\nC:\\blank.html\n\n', '\r\rC:\\cr.html', 'C:\\one.html\nC:\\two.html\rC:\\three.html',
  'The page: `C:\\a folder\\report.html` and a gone one C:\\nope\\gone.html.', 'python make.py > "C:\\a folder\\old.html"', 'page: C:\\a folder\\built by a script.html\nlisting: C:\\a folder\\listed.html',
  'C:\\a\\b.html)', 'C:\\a\\b.html]', 'C:\\a\\b.html}', 'C:\\a\\b.html/', 'C:\\a\\b.html\\', 'C:\\a\\b.html-', 'C:\\a\\b.html_', 'C:\\a\\b.html#top', 'C:\\a\\b.html#top C:\\c.html', 'C:\\a\\b.htm#', 'C:\\a\\b.htmll', 'C:\\a\\b.htmL',
  '.HTM only C:\\a\\b.HTM', 'C:\\a\\b.htm.HTML', '..html', 'C:\\..\\..html', 'C:\\a\\.html.', 'file:///C:/a/b.html', 'file:C:/a/b.html', 'src="/img/a.html"', "url('/a/b.htm')", '(see /a/b.html, /c/d.html; ~/e.htm!)',
];
{ const r = rng(20261117);
  const tokens = ['C:\\', 'C:/', 'd:\\', '~/', '~\\', '/', '//', 'a', 'b c', 'dir\\', 'sub/', '.html', '.htm', '.htmlx', '.HTML', '.', '..', ' ', '  ', '\n', '\r\n', '\u2028', '\t', '\u00a0',
    '`', "'", '"', '(', ')', '[', ']', ',', ';', ':', '!', '?', '&', '<', '>', '|', '*', '=', '\u00e9', '\u65e5\u672c', '\ud83d\ude00', 'x'.repeat(97), 'http://h', 'report', 'page.html', 'C:\\w\\p.html', '/u/p.htm', '~/h/p.html', '-', '_', '#'];
  for (let i = 0; i < 900; i++) pathTexts.push(fuzz(r, tokens, 1, 14)); }
const paths = pathTexts.map(text => ({ text, out: Vreal.pathsIn(text) }));
const pathsOdd = [null, undefined, 5, {}, ['C:\\a.html']].map(v => Vreal.pathsIn(v).length);   // not a string: nothing (the C++ takes a string)

// ---------------------------------------------------------------- Node's path, both platforms
const pathStrings = ['', '.', '..', '/', '\\', 'C:', 'C:\\', 'C:/', 'c:\\a\\b.html', 'C:\\a\\b.html', 'C:/a/b/../c.html', 'C:\\a\\.\\b\\..\\..\\..\\x', 'C:\\a\\b\\', 'C:\\a\\b\\\\', 'C:a', 'C:a\\..\\..', 'C:..', 'C:\\..', 'C:\\a\\..', 'C:\\a\\...\\b',
  '\\\\srv\\share', '\\\\srv\\share\\', '\\\\srv\\share\\a\\..\\b.html', '//srv/share/a/b.html', '\\\\srv', '\\\\srv\\', '\\\\\\srv\\share\\x', '\\a\\b', '/a/b/', '/a/../../b', 'a/b/../../../c', 'a\\b\\c.html', './a', '.\\a\\', '../a/..', 'a/./b/.', 'a//b///c', 'a/b/c/', '/a/b.html', '//a/b.html', '///a',
  'a:b', 'ab:c', 'a/b:c', 'a/b:/c', 'x/../y:z', '..\\C:\\x', 'x/..//C:', 'ab:', '..:', '\u00e9:\\x', 'C:\\\u65e5\u672c\\\ud83d\ude00\\..\\p\u00e9.HTML', '/\u65e5\u672c/\u00e9/../x.htm', 'C:\\a b\\c d.html', 'C:\\a\\.hidden', 'C:\\a\\b.', 'C:\\a\\b..', 'C:\\a\\..b', 'C:\\a\\b.tar.gz', 'C:\\a.b\\c', '.html', 'a.html', 'a.', 'a..', '..a', 'x.y.z', 'C:\\dir.html\\', 'C:\\dir.html\\\\', '/dir.html/', '/x/.', '/x/..', 'C:\\x\\.', 'C:\\x\\..',
  'C:\\Users\\Tester\\.claude-demo', 'C:/Users/Tester/.claude-demo/', 'c:/users/tester/.CLAUDE-demo//', '/home/tester/.claude/', '~/x', 'C:\\\\a', 'C:\\a\\\\b', 'C:/a\\b/c\\d', 'é', '\u00e9\u00e9/..', '\u65e5/..', '\u65e5\u672c\u8a9e/..', 'ab/..', 'abc/..', 'a/../..', 'a\\b\\..\\..\\..\\..'];
{ const r = rng(42);
  // (no '?' and no DOS device name here: Node 24's \\?\ , \\.\ and CON: handling is not ported, and says so)
  const tokens = ['C:', 'c:', 'z:', '/', '\\', '/', '\\', '.', '..', 'a', 'B b', '\u00e9', '\u65e5\u672c', '\ud83d\ude00', '.html', '.HTM', 'x.y', ':', ' ', '-'];
  for (let i = 0; i < 500; i++) pathStrings.push(fuzz(r, tokens, 1, 9)); }
// a "\\.\" root is Node 24's device namespace: left out (see core/deck/viewer.hpp)
const deviceRoot = s => /^[\\/]{2}\.[\\/]*$/.test(s) || /^[\\/]{2}\.[\\/]+[^\\/]*$/.test(s);
const pathFns = [...new Set(pathStrings)].filter(s => !deviceRoot(s)).map(p => ({ p,
  win: { normalize: path.win32.normalize(p), dirname: path.win32.dirname(p), basename: path.win32.basename(p), extname: path.win32.extname(p), isAbsolute: path.win32.isAbsolute(p) },
  posix: { normalize: path.posix.normalize(p), dirname: path.posix.dirname(p), basename: path.posix.basename(p), extname: path.posix.extname(p), isAbsolute: path.posix.isAbsolute(p) } }));
const joinParts = [['C:\\a', 'b.html'], ['C:\\a\\', 'b', 'c.html'], ['C:\\a', '..', '..', 'x'], ['', 'a'], ['', ''], [], ['C:\\', 'x.html'], ['C:', 'x'], ['\\\\srv\\share', 'a', 'b.css'], ['//srv', 'share', 'x'], ['/', '/a'], ['\\', '\\a'], ['/a', '', 'b/'], ['a', '../..', 'b'],
  ['C:\\Users\\tester', ''], ['C:\\Users\\tester', 'x/y.html'], ['C:\\Users\\tester\\', '/x//y.html'], ['/home/tester', 'x/../y.html'], ['/home/tester/', ''], ['rel', 'a.html'], ['C:\\\u65e5\u672c', '\ud83d\ude00', 'p.html'], ['/', 'a b', 'c.js'], ['//', 'a'], ['///x', 'y'], ['\\\\\\x', 'y'], ['\\\\x', '\\y'], ['.', 'a'], ['..', 'a'], ['a:b', 'c'], ['x', 'C:\\y']];
const joins = joinParts.map(parts => ({ parts, win: path.win32.join(...parts), posix: path.posix.join(...parts) }));
const lower = ['ABC xyz 123', 'C:\\Users\\\u00c9COLE\\\u00dcn\u00ef\\\u00c0\u00de\u00d7\u00f7\u00df', '\u0416\u0423\u041a \u0401\u0436 \u042f\u0410\u040f\u0400', '\u65e5\u672c ABC \ud83d\ude00 Z', ''].map(s => ({ s, out: s.toLowerCase() }));
const utf8 = [];
{ const r = rng(7);
  const hand = ['', '41', 'c3a9', 'c3', 'c341', 'e697a5', 'e697', 'e69741', 'e0a080', 'e08080', 'eda080', 'ed9fbf', 'f09f9880', 'f09f98', 'f09f', 'f0', 'f08f8080', 'f4908080', 'f48fbfbf', 'f5808080', 'c080', 'c1bf', 'ff', 'fe', '80', 'bf', '80bf80', 'efbbbf41', 'efbfbd', 'e2', 'e282', 'e282ac', 'e28241', 'c3a9c3', 'f09f988041f09f', 'eda0bdedb880', 'f8888080', 'fc84808080'];
  const bytes = ['41', '7a', '0a', '80', 'bf', '9f', 'a0', '90', '8f', 'c2', 'c3', 'df', 'e0', 'e1', 'ed', 'ee', 'ef', 'f0', 'f1', 'f4', 'f5', 'c0', 'c1', 'ff', 'c3a9', 'e697a5', 'f09f9880'];
  for (let i = 0; i < 260; i++) hand.push(fuzz(r, bytes, 1, 8));
  for (const hex of hand) utf8.push({ hex, out: Buffer.from(Buffer.from(hex, 'hex').toString('utf8'), 'utf8').toString('hex') }); }
const jsString = [null, true, false, 0, 1, -1, 1.5, 1e21, 123456789012, 'x', '', [], [1, 2], [1, [2, null], 'a', true, null], {}, { a: 1 }, [{}], [[]], [null], 'C:\\a.html', 0.1, 0.5, -3].map(v => ({ v, out: String(v) }));

// ---------------------------------------------------------------- resolvePath (no file is touched)
const resolve = [];
{ const homes = { win: ['C:\\Users\\tester', 'C:\\Users\\tester\\', 'C:/U/../home dir', 'rel\\home', 'D:\\'], posix: ['/home/tester', '/home/tester/', '/h/../home dir', 'rel/home', '/'] };
  const raws = ['/c/Users/me/x.html', '/C/Users/me/x.html', '/c/', '/c', '/cc/x.html', '/1/x.html', 'C:/Users/me/x.html', 'c:\\users\\ME\\..\\x.html', '/page/0/x.html', '~/x/y.html', '~\\x\\y.html', '~/', '~', '~x/y.html', '~/../up.html', '~//abs.html', '~/C:\\x.html',
    'report.html', '.\\report.html', '..\\report.html', '', ' ', '"', '`\'"', '"C:\\a b\\c.html"', '`C:\\a\\b.html`', "'/a/b.html'", '  "\'`C:\\a.html`\'"  ', '" C:\\a.html "', 'C:\\a"b.html', '"C:\\a.html', 'C:\\a.html"\'', '\u00a0C:\\a.html\u3000', '\ufeffC:\\a.html\n',
    'C:\\a\0b.html', '\0', 'C:', 'C:x.html', 'C:\\', 'C:/', 'C:\\a\\..\\..\\b.html', 'C:\\a\\.\\b\\', 'C:\\\u65e5\u672c\\\ud83d\ude00.html', '/\u65e5\u672c/\u00e9.html', '\\\\srv\\share\\x.html', '//srv/share/x.html', '/', '/a/../../b.html', '/a/b/', 'é:\\x.html', '1:\\x.html', 'CC:\\x.html', '/c/Users/me/a/../b.HTM'];
  const r = rng(99);
  const tokens = ['C:', 'c:', '/', '\\', '/', '\\', '~', '..', '.', 'a', 'B b', '\u00e9', '\u65e5\u672c', '\ud83d\ude00', '.html', '.HTM', '"', "'", '`', ' ', '\t', 'd', '/c/', '/Z/'];
  for (let i = 0; i < 400; i++) raws.push(fuzz(r, tokens, 1, 8));
  const deviceish = s => /^\s*[`'"]*[\\/]{2}\./.test(s);
  for (const raw of raws) {
    if (deviceish(raw)) continue;
    const hw = pick(r, homes.win), hp = pick(r, homes.posix);
    resolve.push({ raw, home: hw, win: true, out: Vreal.resolvePath(raw, { home: hw, platform: 'win32' }) });
    resolve.push({ raw, home: hp, win: false, out: Vposix.resolvePath(raw, { home: hp, platform: 'linux' }) });
  } }

// ---------------------------------------------------------------- the three worlds
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-viewer-'));
function world(name) {
  const win = name !== 'posix', sep = win ? '\\' : '/';
  const root = name === 'real' ? tempRoot : name === 'win' ? 'D:\\synthetic\\deck viewer' : '/srv/synthetic/deck viewer';
  const V = name === 'real' ? Vreal : name === 'win' ? Vwin : Vposix;
  const ffs = name === 'win' ? winFs : name === 'posix' ? posixFs : null;
  const opts = { platform: win ? 'win32' : 'linux' };
  const p = rel => root + sep + rel.split('/').join(sep);
  const put = (rel, size, mtimeMs) => {
    const f = rel.startsWith('=') ? rel.slice(1) : p(rel);        // '=<absolute>' for a file outside the root (fake worlds only)
    if (ffs) ffs.add(f, { mtimeMs, size });
    else { fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, 'x'.repeat(size)); fs.utimesSync(f, new Date(mtimeMs), new Date(mtimeMs)); }
    return f;
  };
  const mkdir = rel => { const d = p(rel); if (ffs) ffs.add(d, { dir: true, mtimeMs: 0, size: 0 }); else fs.mkdirSync(d, { recursive: true }); return d; };
  const entries = () => {
    if (ffs) return [...ffs.store.values()].filter(e => !e.implied).map(e => ({ path: e.path, dir: !!e.dir, mtime: e.mtimeMs, size: e.size }));
    const out = [];
    const walk = d => { for (const n of fs.readdirSync(d)) { const f = path.join(d, n); const st = fs.statSync(f); if (st.isDirectory()) { out.push({ path: f, dir: true, mtime: 0, size: 0 }); walk(f); } else out.push({ path: f, dir: false, mtime: st.mtimeMs, size: st.size }); } };
    walk(root);
    return out;
  };
  return { name, win, sep, root, V, opts, p, put, mkdir, entries, home: p('home') };
}
const err = f => { try { return { out: f() }; } catch (e) { return { error: e.message }; } };

function runWorld(name) {
  const w = world(name), { V, p, put, mkdir, win } = w;
  const o = { ...w.opts, home: w.home };
  const OLD = AT - 3 * 86400000;
  // ---- the files
  const PAGE = put('a folder/report.html', 8, AT - 60 * MIN);
  put('a folder/other view.html', 5, AT - 60 * MIN);
  put('a folder/assets/chart.js', 1, OLD); put('a folder/assets/logo.png', 1, OLD); put('a folder/assets/LOGO2.PNG', 2, OLD); put('a folder/style.css', 3, OLD);
  put('a folder/assets/deep/1/2/3/4/5/font.woff2', 4, OLD); put('a folder/assets/deep/1/2/3/4/5/6/too deep.css', 4, OLD);
  put('a folder/secret.json', 2, OLD); put('a folder/.env.html', 1, OLD); put('a folder/.git/config.html', 1, OLD); put('a folder/notes.txt', 1, OLD);
  put('a folder/noext', 1, OLD); put('a folder/\u65e5\u672c\u8a9e \u30da\u30fc\u30b8.htm', 6, OLD); put('a folder/\u00c9cole.HTML', 7, OLD);
  mkdir('a folder/folder.html'); put('a folder/folder.html/inside.html', 2, OLD);
  const OLDP = put('a folder/old.html', 1, OLD);
  const BUILT = put('a folder/built by a script.html', 1, AT);
  const LISTED = put('a folder/listed.html', 1, OLD);
  put('a folder/edge before in.html', 1, AT - 15 * MIN); put('a folder/edge before out.html', 1, AT - 15 * MIN - 1000);
  put('a folder/edge after in.html', 1, AT + 2 * MIN); put('a folder/edge after out.html', 1, AT + 2 * MIN + 1000);
  put('home/x/y.html', 9, OLD); put('home/page.htm', 2, AT);
  for (let i = 0; i < 45; i++) put(`many/p${String(i).padStart(2, '0')}.html`, i, OLD + i * 1000);
  if (name !== 'real') put(win ? '=D:\\rootpage.html' : '=/rootpage.html', 3, OLD);
  const gitBash = win ? '/' + w.root[0].toLowerCase() + w.root.slice(2).replace(/\\/g, '/') : null;   // /c/Users/... (Git Bash)

  // ---- htmlFile
  const htmlRaws = [PAGE, '"' + PAGE + '"', '  `' + PAGE + '`  ', p('a folder/gone.html'), p('a folder/notes.txt'), p('a folder/folder.html'), p('a folder/noext'), p('a folder'), 'report.html', '', '   ', '"', '~/x/y.html', '~/page.htm', '~/nope.html', '~',
    p('a folder/../a folder/./report.html'), p('a folder/\u65e5\u672c\u8a9e \u30da\u30fc\u30b8.htm'), p('a folder/\u00c9cole.HTML'), p('a folder/assets/chart.js'), 'x'.repeat(130), 'relative\\' + '\u65e5'.repeat(125) + '.html', 'rel ' + '\ud83d\ude00'.repeat(70), 'a\0b.html', w.root.replace(/[\\/]/g, '/') + '/a folder/report.html'];
  if (gitBash) htmlRaws.push(gitBash + '/a folder/report.html', gitBash + '/a folder/gone.html', '/page/0/x.html');
  const htmlFile = htmlRaws.map(raw => ({ raw, ...err(() => V.htmlFile(raw, o)) }));

  // ---- the folder registry
  const v = new V.Viewer(w.opts);
  const steps = [];
  const idFor = dir => { const out = v.idFor(dir); steps.push({ op: 'idFor', dir, out }); return out; };
  const show = file => { const out = v.show(file); steps.push({ op: 'show', file, out }); return out; };
  const fileFor = (id, parts) => { steps.push({ op: 'fileFor', id, parts, out: v.fileFor(id, parts) }); };
  const SPACED = p('a folder');
  fileFor(idFor(SPACED), ['report.html']);                // nothing is served before a page in the folder is shown
  const shown = show(PAGE);
  idFor(SPACED + w.sep); idFor(SPACED + w.sep + '.'); idFor(p('a folder/assets/..')); idFor(win ? SPACED.toUpperCase() : SPACED.toUpperCase()); idFor(SPACED.replace(/[\\/]/g, '/'));
  for (const parts of [['report.html'], ['other view.html'], ['assets', 'chart.js'], ['assets', 'logo.png'], ['assets', 'LOGO2.PNG'], ['style.css'], ['secret.json'], ['notes.txt'], ['noext'], ['.env.html'], ['.git', 'config.html'],
    ['..', 'x.html'], ['a\\..\\..\\x.html'], ['a/../x.html'], ['C:', 'x.html'], ['report.html:stream'], ['gone.html'], [], ['assets'], ['folder.html'], ['folder.html', 'inside.html'], ['assets', '.', 'chart.js'], ['assets', '..', 'report.html'],
    ['re*.html'], ['report?.html'], ['"report.html"'], ['<report.html'], ['report>.html'], ['report|.html'], ['report\0.html'], ['', 'report.html'], ['report.html', ''], ['report.html.'], ['report.html '], ['report.'], ['report.HTML'], ['REPORT.html'],
    ['\u65e5\u672c\u8a9e \u30da\u30fc\u30b8.htm'], ['\u00c9cole.HTML'], ['assets', 'deep', '1', '2', '3', '4', '5', 'font.woff2'], ['assets', 'deep', '1', '2', '3', '4', '5', '6', 'too deep.css'], ['style.css', 'x.css'], ['assets', 'chart.JS'], ['assets', 'chart.mjs'], ['x.svg'], ['a.b.c.png']])
    fileFor(shown.dir, parts);
  fileFor('0123456789abcdef', ['report.html']); fileFor('', ['report.html']);
  const home = show(p('home/page.htm')); fileFor(home.dir, ['page.htm']); fileFor(home.dir, ['x', 'y.html']); fileFor(shown.dir, ['page.htm']);
  show(p('a folder/assets/../report.html')); show(p('a folder/nothing here.html')); show(SPACED.replace(/[\\/]/g, '/') + '/x/../other view.html'); show(SPACED + w.sep);
  if (name !== 'real') { const rootShown = show(win ? 'D:\\rootpage.html' : '/rootpage.html'); fileFor(rootShown.dir, ['rootpage.html']); fileFor(rootShown.dir, ['synthetic', 'deck viewer', 'a folder', 'report.html']); }
  const afterSteps = { size: v.dirs.size, ids: [...v.dirs.keys()] };
  // the registry keeps the newest 300 folders; showing one again makes it the newest
  const lru = new V.Viewer(w.opts); const lruShown = [];
  for (let i = 0; i < 305; i++) { lruShown.push(p(`lru/d${i}/x.html`)); if (i === 250) lruShown.push(p('lru/d2/again.html')); }
  for (const file of lruShown) lru.show(file);
  const lruOut = { shown: lruShown, size: lru.dirs.size, ids: [...lru.dirs.keys()], first: lru.idFor(p('lru/d0')), kept: lru.idFor(p('lru/d2')) };

  // ---- a session's pages, from its transcript
  const J = x => JSON.stringify(x);
  const A = (...content) => J({ type: 'assistant', message: { content } });
  const text = t => ({ type: 'text', text: t }), use = (input, nm = 'Write') => ({ type: 'tool_use', name: nm, input });
  const U = (content, timestamp) => J({ type: 'user', ...(timestamp === undefined ? {} : { timestamp }), message: { content } });
  const res = c => ({ type: 'tool_result', content: c });
  const iso = ms => new Date(ms).toISOString();
  const sessions = [];
  const S = (label, jsonl, home2 = w.home) => sessions.push({ label, jsonl, home: home2, out: V.sessionHtml(jsonl, { ...w.opts, home: home2 }) });
  S('the unit test\'s transcript', [
    J({ type: 'user', message: { content: 'look at ' + p('a folder/other view.html') } }),
    A(use({ file_path: OLDP, content: '<a href="/x/y.html">' + p('a folder/style.html') + '</a>' })),
    A(text('The page: `' + PAGE + '` and a gone one ' + p('nope/gone.html') + '.')),
    U([res('listing ' + p('a folder/other view.html'))]),
    A(use({ command: 'python make.py > "' + OLDP + '"' }, 'Bash')),
    U([res([{ type: 'text', text: 'page: ' + BUILT + '\nlisting: ' + LISTED }])], iso(AT)),
  ].join('\n'));
  S('empty', ''); S('broken', '{not json .html'); S('no page named', A(text('nothing here')) + '\n' + A(text('still nothing')));
  S('the window, to the second', [
    U([res(['edge before in.html', 'edge before out.html', 'edge after in.html', 'edge after out.html', 'old.html', 'built by a script.html'].map(n => p('a folder/' + n)).join('\n'))], iso(AT)),
  ].join('\n'));
  S('timestamps', [
    U([res(p('a folder/built by a script.html'))]),                                   // no timestamp: not counted
    U([res(p('a folder/edge before in.html'))], 'not a date'),
    U([res(p('a folder/edge after in.html'))], null),
    U([res(p('a folder/report.html'))], '2026-11-17T12:30:00.000+02:00'),          // = AT + 30 min: report.html (AT - 60 min) is too old
    U([res(p('home/page.htm'))], '2026-11-17T05:00:00-05:00'),                      // = AT
    U([res(p('a folder/old.html'))], iso(AT - 3 * 86400000 + 5000)),
  ].join('\n'));
  S('every way a result holds text', [
    U([res([{ type: 'text', text: p('a folder/built by a script.html') }, null, 'a string ' + p('a folder/edge before in.html'), { text: 5 }, { text: p('home/page.htm') }, 7, [p('a folder/edge after in.html')]]), null, 'x', 5, { type: 'text', text: p('a folder/edge after in.html') }, res({ text: p('a folder/edge after in.html') }), res(null), { type: 'tool_result' }], iso(AT)),
  ].join('\n'));
  S('tool inputs walked; a file body is not', [
    A(use({ file_path: p('a folder/report.html'), content: p('a folder/old.html'), new_string: p('a folder/old.html'), old_string: p('a folder/old.html'), edits: [{ x: p('a folder/old.html') }], new_source: p('a folder/old.html') })),
    A(use({ nested: { deeper: [{ path: p('a folder/other view.html') }, ['see ' + p('home/x/y.html') + ' now', 5, null, true]], content: { inner: p('a folder/listed.html') } } })),
    A(use('  ' + p('a folder/built by a script.html') + '  ')),                    // an input that is one path, with spaces round it
    A(use([p('a folder/edge after in.html'), { content: p('a folder/listed.html') }])),
    A(use(0), use(''), use(null), use(false), use(5), { type: 'tool_use' }),
    A(use({ a_page_with_spaces: p('a folder/built by a script.html'), not_absolute: 'report.html', relative_named: 'see report.html', quoted: '"' + p('a folder/old.html') + '"' })),
  ].join('\n'));
  S('newest mention first; a second mention moves it', [
    A(text(p('many/p00.html') + ' ' + p('many/p01.html') + ' ' + p('many/p02.html'))),
    A(text('again ' + p('many/p00.html'))),
    A(text('`' + p('many/p03.html') + '`, `' + p('many/p01.html').toUpperCase().replace(/\.HTML$/, '.html') + '`')),
  ].join('\n'));
  S('at most 40', A(text(Array.from({ length: 45 }, (_, i) => p(`many/p${String(i).padStart(2, '0')}.html`)).join('\n'))));
  S('at most 40, with gone ones between', A(text(Array.from({ length: 45 }, (_, i) => p(`many/p${String(i).padStart(2, '0')}.html`) + ' ' + p(`many/gone${i}.html`)).join('\n'))));
  S('not files: a folder named .html, a script, a page that is gone', A(text([p('a folder/folder.html'), p('a folder/assets/chart.js'), p('a folder/gone.html'), p('a folder/folder.html/inside.html')].join(' '))));
  S('home and the written forms', [
    A(text('~/x/y.html and "~/page.htm" and ~/nope.html')),
    A(text(w.root.replace(/[\\/]/g, '/') + '/a folder/report.html')),
    ...(gitBash ? [A(text('`' + gitBash + '/a folder/other view.html`'))] : []),
    A(text(p('a folder/assets/../\u65e5\u672c\u8a9e \u30da\u30fc\u30b8.htm') + '\n' + p('a folder/\u00c9cole.HTML') + ' ' + p('a folder/\u00c9cole.htm'))),
  ].join('\r\n'));
  S('lines that are not messages', [
    '".html"', '[".html"]', '5.html', 'null .html', '{"message":null,"x":".html"}', '{"message":{"content":".html ' + J(PAGE).slice(1, -1) + '"},"type":"assistant"}', '{"type":"assistant","x":".html"}',
    J({ type: 'system', message: { content: [text(PAGE)] } }), J({ message: { content: [text(PAGE)] } }), J({ type: 'ASSISTANT', message: { content: [text(PAGE)] } }),
    A(null, 'a string ' + PAGE, 5, { type: 'text', text: 5 }, { type: 'text' }, { type: 'image', text: PAGE }, { text: PAGE }),
    A(text(p('a folder/old.html'))) + '   ', '   ' + A(text(p('a folder/listed.html'))), A(text(p('home/page.htm'))).slice(0, -1),
    'a .HTM line that is not JSON', '',
  ].join('\n'));
  // key order: JavaScript lists the keys that are array indexes first (ascending), then the rest as written; the
  // text below is written by hand so the keys are NOT in that order on the line
  { const q = f => J(p(f));
    S('the order of an object\'s keys', '{"type":"assistant","message":{"content":[{"type":"tool_use","input":{"zeta":' + q('many/p10.html') + ',"10":' + q('many/p11.html') + ',"alpha":' + q('many/p12.html') + ',"2":' + q('many/p13.html') + ',"02":' + q('many/p14.html') + ',"-1":' + q('many/p15.html') + ',"4294967295":' + q('many/p16.html') + ',"4294967294":' + q('many/p17.html') + ',"0":' + q('many/p18.html') + ',"1.5":' + q('many/p19.html') + '}}]}}');
    S('a key written twice: the last value, in the first one\'s place', '{"type":"assistant","message":{"content":[{"type":"tool_use","input":{"a":' + q('many/p20.html') + ',"b":' + q('many/p21.html') + ',"a":' + q('many/p22.html') + '}}]}}');
    S('escapes in the JSON', '{"type":"assistant","message":{"content":[{"type":"text","text":"\\u0060' + J(p('many/p23.html')).slice(1, -1) + '\\u0060 and ' + J(p('a folder/\u65e5\u672c\u8a9e \u30da\u30fc\u30b8.htm')).slice(1, -1).replace(/\u65e5/g, '\\u65e5') + '"}]}}'); }
  // bytes that are not UTF-8 in a line: the deck reads the transcript as text (U+FFFD), the line still parses
  { const line = Buffer.concat([Buffer.from('{"type":"assistant","message":{"content":[{"type":"text","text":"bad \u00e9', 'utf8'), Buffer.from([0xff, 0xc3, 0x28, 0xe6, 0x97]), Buffer.from(' ' + J(p('many/p30.html')).slice(1, -1) + '"}]}}\n', 'utf8'),
      Buffer.from(A(text(p('many/p31.html'))), 'utf8')]);
    const asText = line.toString('utf8');
    sessions.push({ label: 'ill-formed bytes', hex: line.toString('hex'), home: w.home, out: V.sessionHtml(asText, { ...w.opts, home: w.home }) }); }
  S('a byte order mark: that line is not JSON', '\ufeff' + A(text(p('many/p32.html'))) + '\n' + A(text(p('many/p33.html'))));
  S('another home', A(text('~/report.html ~/other view.html')), p('a folder'));

  return { name, win, root: w.root, home: w.home, entries: w.entries(), htmlFile, viewer: { steps, after: afterSteps, lru: lruOut }, sessions };
}
const worlds = [];
try { for (const n of ['real', 'win', 'posix']) worlds.push(runWorld(n)); }
finally { fs.rmSync(tempRoot, { recursive: true, force: true }); }

// idFor over odd folders (no file is touched)
const idDirs = ['C:\\a', 'c:\\A', 'C:\\a\\', 'C:/a/b/..', 'C:\\Users\\\u00c9COLE', 'C:\\users\\\u00e9cole', '\\\\srv\\share\\x', '/home/tester/site', '/home/tester/site/', '.', '', 'rel\\dir', 'C:\\\u0416\u0423\u041a', 'C:\\\u65e5\u672c\\\ud83d\ude00', 'D:\\', 'C:'];
const ids = idDirs.map(dir => ({ dir, win: new Vreal.Viewer({ platform: 'win32' }).idFor(dir), posix: new Vposix.Viewer({ platform: 'linux' }).idFor(dir) }));

// ---------------------------------------------------------------- main.js cleanPages (its source, as it stands)
const mainSrc = fs.readFileSync(path.join(REPO, 'main.js'), 'utf8');
const cleanSrc = /^function cleanPages\(list\) \{\r?\n[\s\S]*?^\}/m.exec(mainSrc);
if (!cleanSrc) throw new Error('main.js cleanPages not found');
const cleanFor = P => new Function('path', cleanSrc[0] + '\nreturn cleanPages;')(P);
const cleanWin = cleanFor(path.win32), cleanPosix = cleanFor(path.posix);
const pageLists = [
  [], [{ label: 'Report', file: 'C:\\site\\report.html' }], [{ file: 'C:\\site\\report.html' }], [{ label: '', file: 'C:/site/My Page.HTM' }], [{ label: '   ', file: '"C:\\site\\quoted.html"' }], [{ label: 'x'.repeat(30), file: '/srv/site/a.html' }],
  [{ label: '  padded  ', file: '  C:\\site\\p.html  ' }], [{ label: 'rel', file: 'site\\p.html' }, { label: 'drive-rel', file: 'C:p.html' }, { label: 'not html', file: 'C:\\site\\p.txt' }, { label: 'blank', file: '' }, { label: 'none' }],
  [null, 5, 'C:\\a.html', ['C:\\a.html'], { file: null }, { file: 0 }, { file: false }, { file: 5 }, { file: ['C:\\arr.html'] }, { file: { a: 1 } }, { label: 7, file: 'C:\\n.html' }, { label: ['a', 'b'], file: 'C:\\l.html' }, { label: { a: 1 }, file: 'C:\\o.html' }, { label: true, file: 'C:\\t.html' }, { label: 0, file: 'C:\\zero.html' }],
  [{ file: '"C:\\one\nline.html"' }, { file: '"C:\\a.html' }, { file: 'C:\\a.html"' }, { file: '""' }, { file: '"' }, { file: '" C:\\spaced.html "' }, { file: '"\\x.html"' }, { file: '\\rooted.html' }, { file: '/rooted.htm' }, { file: '\\\\srv\\share\\u.html' }],
  [{ file: 'C:\\site\\.html' }, { file: 'C:\\site\\a.b.html' }, { file: 'C:\\site\\UPPER.HTML' }, { file: 'C:\\site\\x.htm' }, { file: 'C:\\site\\x.html.htm' }, { file: 'C:\\site\\x.htmlx' }, { file: '/a/b\\c.html' }, { file: 'C:\\\u65e5\u672c\\\u30da\u30fc\u30b8 \ud83d\ude00.html' }],
  [{ label: '\u65e5'.repeat(30), file: 'C:\\j.html' }, { label: '\ud83d\ude00'.repeat(12) + 'tail', file: 'C:\\e.html' }, { label: 'a' + '\ud83d\ude00'.repeat(11) + 'bcd', file: 'C:\\e2.html' }, { label: '\u00a0\u3000 nbsp \ufeff', file: '\u00a0C:\\nb.html\u2028' }],
  Array.from({ length: 15 }, (_, i) => ({ label: 'p' + i, file: `C:\\many\\p${i}.html` })),
  Array.from({ length: 15 }, (_, i) => (i % 3 ? { label: 'p' + i, file: `/many/p${i}.html` } : { label: 'bad' + i, file: 'nope' })),
];
const clean = pageLists.map(list => ({ list, win: cleanWin(list), posix: cleanPosix(list) }));

// ---------------------------------------------------------------- lib/phone.js: PAGE_LINKS, VIEW_CSP, VIEW_TYPES, withPageLinks
const phoneSrc = fs.readFileSync(path.join(REPO, 'lib', 'phone.js'), 'utf8');
const a = phoneSrc.indexOf('const PAGE_LINKS ='), b = phoneSrc.indexOf('\nfunction safeEqual');
if (a < 0 || b < a) throw new Error('lib/phone.js: the page viewer block not found');
const phone = new Function(phoneSrc.slice(a, b) + '\nreturn { PAGE_LINKS, VIEW_CSP, VIEW_TYPES, withPageLinks };')();
const pages = ['', '<p>no body tag</p>', '<html><body><p>x</p></body></html>', '<BODY>x</BODY >', 'x</body\n\t >y</body>z', 'x</ body>y', 'x</body', 'x</bodyx>y</boDY\u00a0\u2028>z', '</body>', 'a</body>b</body>c', 'x</body\u200b>y',
  '\u65e5\u672c\u8a9e</Body>\ud83d\ude00', 'x<body>y', 'x</bod>y</bodyy>', 'x</body >\n', '<script>var s = "</body>";</script></body>', 'x</body\ufeff>y', 'x</body \u3000\t\r\n>y', '</body', '</body ', '<</body>>']
  .map(html => ({ html, out: phone.withPageLinks(Buffer.from(html, 'utf8')) }));

const out = { note: 'generated by core/test/deck/gen-viewer.js from lib/viewer.js, lib/phone.js and main.js cleanPages', node: process.version, platform: process.platform,
  pinned: { at: AT, atIso: new Date(AT).toISOString(), tz: 'none read: every timestamp carries its zone' },
  paths, pathsOdd, pathFns, joins, lower, utf8, jsString, resolve, worlds, ids, clean,
  phone: { PAGE_LINKS: phone.PAGE_LINKS, VIEW_CSP: phone.VIEW_CSP, VIEW_TYPES: phone.VIEW_TYPES, pages } };
const file = path.join(__dirname, '..', 'fixtures', 'deck', 'viewer.json');
fs.writeFileSync(file, JSON.stringify(out, null, 1) + '\n');
const count = paths.length + pathFns.length * 10 + joins.length * 2 + utf8.length + resolve.length + worlds.reduce((n, w) => n + w.htmlFile.length + w.viewer.steps.length + w.sessions.length, 0) + clean.length * 2 + pages.length;
console.log(`viewer.json: ${count} cases, ${fs.statSync(file).size} bytes (${paths.filter(x => x.out.length).length} of ${paths.length} texts name a page)`);
