// windows/test/deskcount.js — what the SCRATCH deck's phone server holds, read from the deck's own side.
//   node windows/test/deskcount.js [ms=0] [every=100]
// Over the scratch deck's CDP port (9777 — never the live deck's 9333) it asks the deck's window for
// deck.getSettings().phoneStatus and prints `<ms since start> clients <n> desks <n>`: once, or every `every` ms
// for `ms` ms (a line whenever the numbers change, and a last one). The desk tests read this to prove a native
// client that left is GONE for the deck (windows/test/close.ps1).
const CDP = 9777;
const total = parseInt(process.argv[2] || '0', 10), every = parseInt(process.argv[3] || '100', 10);

(async () => {
  const list = await (await fetch(`http://127.0.0.1:${CDP}/json`)).json();
  const page = list.find(t => t.type === 'page' && /index\.html/.test(t.url)) || list.find(t => t.type === 'page');
  if (!page) { console.log('no deck page on CDP ' + CDP); process.exit(2); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('cdp socket')); });
  let id = 0; const waiting = new Map();
  ws.onmessage = ev => { const m = JSON.parse(ev.data); const w = waiting.get(m.id); if (w) { waiting.delete(m.id); w(m); } };
  const status = () => new Promise(res => {
    const n = ++id; waiting.set(n, m => { try { res(JSON.parse(m.result.result.value)); } catch { res(null); } });
    ws.send(JSON.stringify({ id: n, method: 'Runtime.evaluate', params: { expression: 'deck.getSettings().then(s => JSON.stringify(s.phoneStatus))', awaitPromise: true, returnByValue: true } }));
  });
  const t0 = Date.now(); let last = '';
  for (;;) {
    const s = await status();
    const line = s ? `clients ${s.clients} desks ${s.desks}` : 'no status';
    const at = Date.now() - t0;
    if (line !== last || at >= total) { console.log(`${at} ${line} @${Date.now()}`); last = line; }
    if (at >= total) break;
    await new Promise(r => setTimeout(r, every));
  }
  ws.close(); process.exit(0);
})().catch(e => { console.log('deskcount failed: ' + e.message); process.exit(1); });
