'use strict';
// windows/host-test/serve-browser.js — the REAL phone page and the REAL browser desk, loaded from the native deck's
// server in a headless Chromium (test/phone/where.js finds Playwright and its shell): both must come up and show
// the tile the Node phone left behind. Loopback only; DECK_PORT / DECK_TOKEN / DECK_OUT from the environment.
const path = require('path');
const fs = require('fs');
const ROOT = path.join(__dirname, '..', '..');
const { chromium, SHELL } = require(path.join(ROOT, 'test', 'phone', 'where.js'));

const port = Number(process.env.DECK_PORT), token = process.env.DECK_TOKEN, out = process.env.DECK_OUT;
if (!(port >= 8811 && port <= 8819) || !token || !out) { console.error('DECK_PORT (8811-8819), DECK_TOKEN and DECK_OUT are required'); process.exit(2); }
const TITLE = 'native serve one';
let passed = 0, failed = 0;
function check(name, ok, detail) { if (ok) { passed++; console.log('PASS  ' + name); } else { failed++; console.log('FAIL  ' + name + (detail !== undefined ? '  -- ' + String(detail).slice(0, 500) : '')); } }

(async () => {
  const browser = await chromium.launch({ executablePath: SHELL, headless: true });
  try {
    // ---- the phone page
    const phoneCtx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
    const phone = await phoneCtx.newPage();
    const errors = [];
    phone.on('pageerror', e => errors.push(String(e.message || e)));
    const bad = [];
    phone.on('response', r => { if (r.status() >= 400 && !/favicon/.test(r.url())) bad.push(r.status() + ' ' + r.url().replace(token, '<token>')); });
    await phone.goto(`http://127.0.0.1:${port}/?t=${token}`, { waitUntil: 'load' });
    const listed = await phone.waitForFunction(t => document.body && document.body.innerText.includes(t), TITLE, { timeout: 20000 }).then(() => true, () => false);
    check('phone page: loads from the native deck and lists the tile', listed, (await phone.evaluate(() => document.body.innerText)).slice(0, 300));
    check('phone page: no script error, no failed request', !errors.length && !bad.length, JSON.stringify({ errors, bad }));
    await phone.screenshot({ path: path.join(out, 'serve-phone-list.png') });
    // open the tile: its terminal draws what the shell printed
    await phone.getByText(TITLE, { exact: false }).first().click().catch(() => {});
    const shown = await phone.waitForFunction(() => { const x = document.querySelector('.xterm-rows, .xterm-screen'); return !!x && /serve-phone-ok|after-the-flood/.test(document.body.innerText + ' ' + (x.innerText || '')); }, null, { timeout: 20000 }).then(() => true, () => false);
    check('phone page: opening the tile draws its terminal, with what the shell printed', shown, (await phone.evaluate(() => document.body.innerText)).slice(-300));
    await phone.screenshot({ path: path.join(out, 'serve-phone-tile.png') });
    await phoneCtx.close();

    // ---- the browser desk
    const deskCtx = await browser.newContext({ viewport: { width: 1400, height: 900 } });
    const desk = await deskCtx.newPage();
    const derr = [], dbad = [];
    desk.on('pageerror', e => derr.push(String(e.message || e)));
    desk.on('response', r => { if (r.status() >= 400 && !/favicon/.test(r.url())) dbad.push(r.status() + ' ' + r.url().replace(token, '<token>')); });
    await desk.goto(`http://127.0.0.1:${port}/desk/?t=${token}`, { waitUntil: 'load' });
    const side = await desk.waitForFunction(t => document.body && document.body.innerText.includes(t), TITLE, { timeout: 20000 }).then(() => true, () => false);
    check('browser desk: loads from the native deck (the desktop renderer + bridge) and its sidebar lists the tile', side, (await desk.evaluate(() => document.body.innerText)).slice(0, 300));
    check('browser desk: no script error, no failed request', !derr.length && !dbad.length, JSON.stringify({ derr, dbad }));
    await desk.getByText(TITLE, { exact: false }).first().click().catch(() => {});
    const pane = await desk.waitForFunction(() => /serve-phone-ok|after-the-flood/.test(Array.from(document.querySelectorAll('.xterm-rows')).map(x => x.innerText).join('\n')), null, { timeout: 20000 }).then(() => true, () => false);
    check('browser desk: selecting the tile shows its pane with the shell\'s screen', pane);
    await desk.screenshot({ path: path.join(out, 'serve-browser-desk.png') });
    await deskCtx.close();
  } finally { await browser.close(); }
  for (const f of ['serve-phone-list.png', 'serve-phone-tile.png', 'serve-browser-desk.png']) check('shot written: ' + f, fs.existsSync(path.join(out, f)) && fs.statSync(path.join(out, f)).size > 5000);
  console.log(`\n${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
