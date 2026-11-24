'use strict';
// The two routes of lib/phone.js that need the deck behind them — the page viewer (/view/<key>/<folder id>/…,
// lib/viewer.js) and an ask page (/ask/<page id>.html, lib/asks.js) — recorded from the REAL PhoneServer with the
// REAL Viewer and the REAL AskFeed behind it, over loopback, on synthetic files in a temp folder:
//
//   node core/test/deck/gen-routes2.js      (from PowerShell: loopback is blocked in the Bash tool; 127.0.0.1:8813)
//
// Output: core/test/fixtures/deck/routes2.json — every file (path + text or hex), the requests and what was
// answered. core/test/deck/deck_parity_test.cpp loads the same files into memory at the same paths, asks the C++
// server the same requests through the C++ deck, and compares status, headers and the body byte for byte.
// Nothing real is read: the ask state folder is a temp folder, and AskFeed.start() is never called (no deck.json).
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const crypto = require('crypto');

const ROOT = path.join(__dirname, '..', '..', '..');
const { PhoneServer } = require(path.join(ROOT, 'lib', 'phone.js'));
const { Viewer } = require(path.join(ROOT, 'lib', 'viewer.js'));
const Asks = require(path.join(ROOT, 'lib', 'asks.js'));

(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-fx-r2-'));
  const files = {};
  const put = (rel, data) => { const f = path.join(tmp, ...rel.split('/')); fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, data); files[f] = Buffer.isBuffer(data) ? { hex: data.toString('hex') } : { text: data }; return f; };
  const page = put('pages/report.html', '<!doctype html><html><head><link rel="stylesheet" href="sub/style.css"></head><body><h1>Report \u00e9</h1><a href="other.html">next</a></body></html>\n');
  put('pages/other.html', '<p>no closing body tag, so the script is appended');
  put('pages/UPPER.HTML', '<html><BODY>upper</BODY >\n</html>');
  put('pages/a b.html', '<body>a space in the name</body>');
  put('pages/sub/style.css', 'h1 { color: #c33; }\n');
  put('pages/sub/deep/app.mjs', 'export const x = 1;\n');
  put('pages/img.png', Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 1, 2, 3, 0xff, 0xfe]));
  put('pages/data.json', '{"never":"served"}');
  put('pages/.hidden.html', '<body>dotfile</body>');
  put('pages/.git/config.html', '<body>dot folder</body>');
  put('pagesibling/secret.html', '<body>a sibling folder whose name starts the same</body>');
  put('elsewhere/not-shown.html', '<body>its folder was never shown</body>');

  const viewer = new Viewer();
  const shown = viewer.show(page);

  const stateDir = path.join(tmp, 'ask-state');
  fs.mkdirSync(stateDir, { recursive: true });
  const askId = 'ask_deck-test_0123abcd_r1_1118-0930_beef', goneId = 'ask_deck-test_0123abcd_r2_1118-0931_cafe', notHtmlId = 'ask_deck-test_0123abcd_r3_1118-0932_f00d';
  const askPage = put(`ask-pages/${askId}.html`, '<!doctype html><title>Ask</title><body><script>parent.postMessage({ kind: "ask-page" }, "*")</script>Q1?</body>\n');
  const notHtml = put(`ask-pages/${notHtmlId}.txt`, 'a round whose page is not an ask_*.html file');
  const state = { session_id: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee', session_name: 'route test', account: 'claudet', answers: {}, rounds: [
    { id: askId, round: 1, status: 'open', created: '2026-11-18T09:30:00-05:00', title: 'One', page: askPage, items: [{ qid: 'Q1', key: 'k1', title: 'first' }] },
    { id: goneId, round: 2, status: 'open', created: '2026-11-18T09:31:00-05:00', title: 'Two', page: path.join(tmp, 'ask-pages', goneId + '.html'), items: [{ qid: 'Q1', key: 'k2', title: 'second' }] },
    { id: notHtmlId, round: 3, status: 'open', created: '2026-11-18T09:32:00-05:00', title: 'Three', page: notHtml, items: [] },
  ] };
  const stateFile = path.join(stateDir, state.session_id + '.json');
  fs.writeFileSync(stateFile, JSON.stringify(state)); files[stateFile] = { text: JSON.stringify(state) };
  const asks = new Asks.AskFeed({ stateDir, downloads: [], draftsFile: path.join(tmp, 'ask-drafts.json'), settings: () => ({}) });
  asks.scan();

  const token = '0123456789abcdef', port = 8813;
  const server = new PhoneServer({
    getInit: () => ({ home: 'C:\\fake\\home', meta: { now: 1, tiles: [] }, platform: { os: 'win32' } }),
    replayFor: () => null, call: () => null,
    askPage: id => asks.pagePath(String(id || '')),
    viewFile: (id, parts) => viewer.fileFor(id, parts),
  });
  await server.start({ port, token, bind: 'localhost' });
  const ask = (method, target, headers = {}) => new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, method, path: target, headers, agent: false }, res => {
      const chunks = []; res.on('data', c => chunks.push(c)); res.on('end', () => {
        const buf = Buffer.concat(chunks);
        resolve({ status: res.statusCode, type: res.headers['content-type'] || null, cache: res.headers['cache-control'] || null, cookie: (res.headers['set-cookie'] || [null])[0],
          csp: res.headers['content-security-policy'] || null, referrer: res.headers['referrer-policy'] || null, nosniff: res.headers['x-content-type-options'] || null,
          length: buf.length, sha256: crypto.createHash('sha256').update(buf).digest('hex'), text: buf.length <= 1500 ? buf.toString('utf8') : null });
      });
    });
    req.on('error', reject);
    req.end();
  });
  const K = server.pageKey(), D = shown.dir, V = `/view/${K}/${D}`;
  const C = { cookie: `deck_phone=${token}` };
  const list = [
    ['GET', `${V}/report.html`], ['HEAD', `${V}/report.html`], ['GET', `${V}/other.html`], ['GET', `${V}/UPPER.HTML`], ['GET', `${V}/a%20b.html`], ['GET', `${V}/sub/style.css`],
    ['GET', `${V}/sub/deep/app.mjs`], ['GET', `${V}/img.png`], ['GET', `${V}/data.json`], ['GET', `${V}/.hidden.html`], ['GET', `${V}/.git/config.html`], ['GET', `${V}/nope.html`],
    ['GET', `${V}/sub/../report.html`], ['GET', `${V}/sub/%2e%2e/report.html`], ['GET', `${V}/..%2fpagesibling%2fsecret.html`], ['GET', `${V}/../pagesibling/secret.html`], ['GET', `${V}/sub%5cstyle.css`],
    ['GET', `${V}/%zz.html`], ['GET', `${V}/sub/`], ['GET', `${V}/`], ['GET', `${V}`], ['GET', `${V}/report.html?x=1#frag`], ['GET', `${V}/report.html`, C],
    ['GET', `${V}/a/b/c/d/e/f/g/h.html`], ['GET', `${V}/a/b/c/d/e/f/g/h/i.html`], ['GET', `${V}/sub//style.css`],
    ['POST', `${V}/report.html`], ['DELETE', `${V}/report.html`],
    ['GET', `/view/${'0'.repeat(16)}/${D}/report.html`], ['GET', `/view/${K.toUpperCase()}/${D}/report.html`], ['GET', `/view/${K}/${'0'.repeat(16)}/report.html`], ['GET', `/view/${K}/${D.slice(0, 15)}/report.html`],
    ['GET', `/view/${K}/${viewer.idFor(path.join(tmp, 'elsewhere'))}/not-shown.html`], ['GET', '/view/x'], ['GET', '/view/'],
    ['GET', `/ask/${askId}.html`, C], ['HEAD', `/ask/${askId}.html`, C], ['GET', `/ask/${askId}.html?t=${token}`], ['GET', `/ask/${askId}.html`], ['POST', `/ask/${askId}.html`, C],
    ['GET', `/ask/${goneId}.html`, C], ['GET', `/ask/${notHtmlId}.html`, C], ['GET', '/ask/ask_deck-test_0123abcd_r9_1118-0930_beef.html', C], ['GET', '/ask/ask_x.html', C], ['GET', `/ask/${askId}.htm`, C],
  ];
  const requests = [];
  for (const [method, target, headers] of list) requests.push({ method, target, headers: headers || {}, res: await ask(method, target, headers || {}) });
  await server.stop();
  const out = { root: tmp, token, pageKey: K, shown, page, askId, goneId, notHtmlId, stateDir, files, requests };
  const f = path.join(__dirname, '..', 'fixtures', 'deck', 'routes2.json');
  fs.writeFileSync(f, JSON.stringify(out, null, 1) + '\n');
  fs.rmSync(tmp, { recursive: true, force: true });
  console.log(`wrote routes2 -> ${f} (${requests.length} requests)`);
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
