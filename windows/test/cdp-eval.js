// windows/test/cdp-eval.js — evaluate one expression in the SCRATCH deck's own window (CDP 9777; never the live
// deck's 9333) and print the result as JSON.   node windows/test/cdp-eval.js "<expression>"
// The expression runs in the renderer's page (its globals: state, placeTile, isPlaced, deck …); a promise is awaited.
// CDP_PORT picks another SCRATCH deck (9778: the claim branch's, windows/test/driver-e2e.ps1). Never 9333.
const CDP = Number(process.env.CDP_PORT || 9777);
if (CDP === 9333) { console.log('refusing: 9333 is the live deck'); process.exit(2); }
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${CDP}/json`)).json();
  const page = list.find(t => t.type === 'page' && /index\.html/.test(t.url)) || list.find(t => t.type === 'page');
  if (!page) { console.log('no deck page on CDP ' + CDP); process.exit(2); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('cdp socket')); });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data); if (m.id !== 1) return;
    const r = m.result || {};
    if (r.exceptionDetails) console.log('EXCEPTION ' + (r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text));
    else console.log(JSON.stringify(r.result ? r.result.value : null));
    ws.close(); process.exit(0);
  };
  ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression: process.argv[2], awaitPromise: true, returnByValue: true } }));
})().catch(e => { console.log('cdp-eval failed: ' + e.message); process.exit(1); });
