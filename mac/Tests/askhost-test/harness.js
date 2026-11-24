// mac/Tests/askhost-test/harness.js - drives the Mac ask pane's host document (AskHostPage.swift) as AskPane.swift
// does, in a real browser engine (headless Edge on Windows: Blink, not WebKit), with fake ask pages that speak the
// skill's host bridge (fakepage.html). run.ps1 puts the PRELUDE before the host document's own script (it stands in
// for WKScriptMessageHandler: window.webkit.messageHandlers.deck) and the DRIVER after it.
// Each check prints `ok   <name>` or `FAIL <name>  got: ...` into the results element (id askhost-results), which run.ps1 reads.

// ---------------------------------------------------------------- PRELUDE
window.__msgs = [];
window.__reply = null;                        // m -> { ok, value | error }: the deck's answer to a call
window.webkit = { messageHandlers: { deck: { postMessage: m => {
  const c = JSON.parse(JSON.stringify(m));    // what reaches Swift is a copy (WKScriptMessage.body)
  window.__msgs.push(c);
  if (c.ev === 'call' && window.__reply) { const r = window.__reply(c); setTimeout(() => window.deckIn(Object.assign({ op: 'reply', rid: c.rid }, r)), 5); }
} } } };
// ---------------------------------------------------------------- PRELUDE END

// ---------------------------------------------------------------- DRIVER
(async function () {
  const PAGES = '__PAGES__';
  const out = [];
  let passed = 0, failed = 0;
  const check = (name, ok, got) => { if (ok) passed++; else failed++; out.push((ok ? 'ok   ' : 'FAIL ') + name + (ok || got === undefined ? '' : '  got: ' + JSON.stringify(got))); };
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  let cursor = 0;                             // messages before it were looked at already
  const mark = () => { cursor = window.__msgs.length; return cursor; };
  async function waitFor(pred, ms = 4000) {
    const t0 = Date.now();
    for (;;) {
      for (let i = cursor; i < window.__msgs.length; i++) if (pred(window.__msgs[i])) { cursor = i + 1; return window.__msgs[i]; }
      if (Date.now() - t0 > ms) return null;
      await sleep(20);
    }
  }
  const since = (k, pred) => window.__msgs.slice(k).filter(pred);
  // what a framed page says only to the test (__test_state)
  const winMsgs = [];
  window.addEventListener('message', ev => { if (ev.data && ev.data.askpage === '__test_state') winMsgs.push(ev.data); });
  async function pageState(frame, id) {
    const n = winMsgs.length;
    frame.contentWindow.postMessage({ askpage: '__test_state', id }, '*');
    for (let i = 0; i < 100 && winMsgs.length === n; i++) await sleep(20);
    return winMsgs[n] || null;
  }
  const $ = s => document.querySelector(s);
  const pane = document.getElementById('ask-pane');
  const title = () => pane.querySelector('.ap-title').textContent;
  const hint = () => pane.querySelector('.ap-hint').textContent;
  const deckIn = m => window.deckIn(m);

  try {
    check('host: says ready', !!(await waitFor(m => m.ev === 'ready')));
    const drafts = { ask_t1: { q1: { pick: 'o:1', text: '' } }, ask_t3: { q1: { pick: 'o:2', text: '' } } };
    window.__reply = m => {
      if (m.fn === 'askDraftGet') return { ok: true, value: drafts[m.args[0]] || null };
      if (m.fn === 'askLogState') return { ok: true, value: { q1: { pick: 'o:2' } } };
      if (m.fn === 'askDone') return { ok: true, value: { how: 'file', files: ['/Users/t/x.answers.json'], tileId: 'T1' } };
      if (m.fn === 'askClear') return { ok: true, value: true };
      return { ok: false, error: 'unexpected call ' + m.fn };
    };
    const tiles = [{ id: 'T1', kind: 'claude', sessionId: 'S1', title: 'Tile one', account: 'a1', status: 'idle' },
      { id: 'T2', kind: 'claude', sessionId: 'S2', title: 'Tile two', account: 'a1', status: 'busy' }];
    const base = { op: 'state', asks: [], askLog: [], tiles, accounts: [{ id: 'a1', abbr: 'vv' }], cfg: {},
      kh: { asks: 'Cmd+Shift+A', zoom: 'Cmd+Shift+Enter' }, guest: false, guestWhy: '', remote: false, base: PAGES };
    const R1 = { id: 'ask_t1', round: 1, total: 2, answered: 0, drafted: 1, created: Date.now() - 60000 };
    const S1 = { sessionId: 'S1', name: 'sess-one', account: 'vv', rounds: [R1], total: 2, latest: new Date().toISOString() };

    // ---- open: every session's asks
    mark();
    deckIn(Object.assign({}, base, { asks: [S1] }));
    deckIn({ op: 'open', mode: 'all' });
    await sleep(60);
    check('open: the pane shows', !pane.hidden);
    check('open: the bar', title() === 'Asks · 1 session waiting', title());
    check('open: the hint names both keys', hint() === 'Cmd+Shift+Enter or Cmd+Shift+A closes · Done sends that session its answers', hint());
    const sec = $('.ap-sess[data-sid="S1"]');
    check('section: the tile title and the account abbr', !!sec && sec.querySelector('.ap-name').textContent === 'Tile one' && sec.querySelector('.ap-abbr').textContent === 'vv');
    check('section: the meta line', !!sec && /^sess-one · vv · round 1 · asked /.test(sec.querySelector('.ap-meta').textContent), sec && sec.querySelector('.ap-meta').textContent);
    check('section: the mark (1 drafted of 2 open)', !!sec && sec.querySelector('.ap-mark').textContent === '2' && sec.querySelector('.ap-mark').classList.contains('drafted'));
    const fr = sec && sec.querySelector('iframe');
    check('frame: sandboxed with the deck\'s list', !!fr && fr.getAttribute('sandbox') === 'allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads', fr && fr.getAttribute('sandbox'));
    check('frame: the page as the deck frames it', !!fr && fr.src === PAGES + '/ask/ask_t1.html?embed=deck&theme=dark', fr && fr.src);
    const get = await waitFor(m => m.ev === 'call' && m.fn === 'askDraftGet');
    check('ready -> askDraftGet(the page id)', !!get && get.args[0] === 'ask_t1', get);
    const d1 = await waitFor(m => m.ev === 'fire' && m.fn === 'askDraft');
    check('load (the deck\'s draft) -> the page\'s change -> askDraft with it', !!d1 && d1.args[0] === 'ask_t1' && d1.args[1].state.q1.pick === 'o:1' && d1.args[1].total === 2 && d1.args[1].drafted === 1, d1);
    await sleep(60);
    check('height: as the page says', fr.style.height === '222px', fr.style.height);

    // ---- a change on the page
    fr.contentWindow.postMessage({ askpage: '__test_set', id: 'ask_t1', qid: 'q2', text: 'hello' }, '*');
    const d2 = await waitFor(m => m.ev === 'fire' && m.fn === 'askDraft' && m.args[1].state.q2);
    check('a change on the page -> askDraft (state, drafted, answered, later, total)', !!d2 && d2.args[1].state.q2.text === 'hello' && d2.args[1].drafted === 2 && d2.args[1].answered === 2 && d2.args[1].later === 0, d2);
    await sleep(30);
    check('the head counts the page\'s own progress', sec.querySelector('.ap-count').textContent === '2 of 2 answered', sec.querySelector('.ap-count').textContent);

    // ---- what is NOT passed on
    let k = mark();
    fr.contentWindow.postMessage({ askpage: '__test_spoof', id: 'ask_t1' }, '*');
    window.postMessage({ askpage: 'change', id: 'ask_t1', state: { evil: 1 }, drafted: 9 }, '*');    // this window, not a frame
    await sleep(150);
    check('a page naming another page, or a message not from a frame: nothing reaches the deck', since(k, m => m.ev === 'fire').length === 0, since(k, m => m.ev === 'fire'));

    // ---- Copy all
    mark();
    sec.querySelector('.ap-copy').click();
    const cp = await waitFor(m => m.ev === 'clipboard');
    check('Copy all: the page id, then its lines (lib/asks.js composeDone)', !!cp && cp.text === 'ask_t1\nq1: o:1\nq2: hello', cp);
    await sleep(20);
    check('Copy all: the flash', /^Copied 3 lines: paste them into the session's terminal\.$/.test(sec.querySelector('.ap-flash').textContent), sec.querySelector('.ap-flash').textContent);

    // ---- Done
    mark();
    sec.querySelector('.ap-done').click();
    const done = await waitFor(m => m.ev === 'call' && m.fn === 'askDone');
    check('Done -> askDone(session, [{ id, answers }])', !!done && done.args[0] === 'S1' && done.args[1].length === 1 && done.args[1][0].id === 'ask_t1' && done.args[1][0].answers.id === 'ask_t1' && done.args[1][0].answers.lines.length === 2, done);
    const closed = await waitFor(m => m.ev === 'closed');
    check('Done closes the every-session view (closeOnDone; nobody else waits), no tile to go to', !!closed && closed.toTile === null && pane.hidden, closed);
    const note = await waitFor(m => m.ev === 'note');
    check('Done: the note once the pane is closed', !!note && note.text === 'Done: saved the answers file, and one line naming it goes in as your next message.', note);
    const st1 = await pageState(fr, 'ask_t1');
    check('Done locks the page', !!st1 && st1.locked === true, st1);

    // ---- sent: one session's view
    const S1sent = Object.assign({}, S1, { rounds: [Object.assign({}, R1, { sent: { at: Date.now(), how: 'file' } })] });
    mark();
    deckIn(Object.assign({}, base, { asks: [S1sent] }));
    deckIn({ op: 'open', mode: 'S1' });
    await sleep(60);
    check('one session\'s view: the bar', title() === 'Tile one asks', title());
    check('one session\'s view: the hint', hint() === 'Cmd+Shift+A closes · Done sends that session its answers', hint());
    const doneBtn = sec.querySelector('.ap-done');
    check('sent: Done reads Sent and is off', doneBtn.textContent === 'Sent' && doneBtn.disabled);
    check('sent: the round\'s note', /^Sent .*as its answers file: read-only, waiting for the session to read it$/.test(fr.parentNode.querySelector('.ap-rnote').textContent), fr.parentNode.querySelector('.ap-rnote').textContent);

    // ---- the session read it: its last answers, read-only
    const L1 = { id: 'ask_l1', sessionId: 'S1', name: 'sess-one', account: 'vv', title: 'T', round: 1, at: Date.now(), how: 'saved to a file', lines: ['q1: yes', 'q2: no'], url: null };
    mark();
    deckIn(Object.assign({}, base, { asks: [], askLog: [L1] }));
    const md = await waitFor(m => m.ev === 'mode');
    check('its page read: the pane follows to its last answers', !!md && md.mode === 'log:ask_l1', md);
    await sleep(30);
    const lrow = $('.ap-lrow[data-id="ask_l1"]');
    check('log view: the answer lines (no page file)', !!lrow && lrow.querySelector('.ap-lines') && lrow.querySelector('.ap-lines').textContent === 'q1: yes\nq2: no');
    check('log view: the bar says read-only', / · answered .* · read-only$/.test(title()), title());
    check('log view: the hint', hint() === 'Cmd+Shift+A closes · it shows here until the session asks again', hint());
    check('the session\'s section is gone', !$('.ap-sess[data-sid="S1"]'));

    // ---- Esc in the host document closes the pane
    mark();
    pane.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    const c2 = await waitFor(m => m.ev === 'closed');
    check('Esc in the pane closes it', !!c2 && pane.hidden, c2);

    // ---- read, with nothing in the log: closed, and said
    deckIn(Object.assign({}, base, { asks: [S1] }));
    deckIn({ op: 'open', mode: 'S1' });
    await sleep(60);
    mark();
    deckIn(Object.assign({}, base, { asks: [], askLog: [] }));
    const c3 = await waitFor(m => m.ev === 'closed'), n3 = await waitFor(m => m.ev === 'note');
    check('read, nothing in the log: closed, and the note says so', !!c3 && !!n3 && n3.text === 'Tile one has read its answers.', n3);

    // ---- Discard: the confirm, the 700 ms arm, askClear, then the session's terminal
    const S2 = { sessionId: 'S2', name: 'sess-two', account: 'vv', rounds: [{ id: 'ask_t3', round: 1, total: 2, answered: 0, drafted: 1 }], total: 2 };
    mark();
    deckIn(Object.assign({}, base, { asks: [S2] }));
    deckIn({ op: 'open', mode: 'S2' });
    await waitFor(m => m.ev === 'fire' && m.fn === 'askDraft' && m.args[0] === 'ask_t3');
    const sec2 = $('.ap-sess[data-sid="S2"]'), fr3 = sec2.querySelector('iframe');
    check('busy session: Answer in the terminal names the key', sec2.querySelector('.ap-term').textContent === 'Answer in the terminal' && /Cmd\+Shift\+A brings it back/.test(sec2.querySelector('.ap-term').title));
    sec2.querySelector('.ap-discard').click();
    await sleep(30);
    const conf = sec2.querySelector('.ap-confirm'), yes = sec2.querySelector('.ap-discard-yes');
    check('Discard asks first, the safe choice where Done was, Discard not armed yet', !conf.hidden && sec2.querySelector('.ap-row').hidden && yes.disabled && /^Discard the 1 answer in progress for Tile two\?/.test(conf.querySelector('.ap-confirm-text').textContent), conf.querySelector('.ap-confirm-text').textContent);
    yes.click(); await sleep(30);
    check('Discard: a click before the arm does nothing', since(cursor, m => m.ev === 'call' && m.fn === 'askClear').length === 0);
    await sleep(800);
    check('Discard: armed after 700 ms', !yes.disabled);
    mark();
    yes.click();
    const clr = await waitFor(m => m.ev === 'call' && m.fn === 'askClear');
    check('Discard -> askClear(the page)', !!clr && clr.args[0] === 'ask_t3', clr);
    const c4 = await waitFor(m => m.ev === 'closed');
    check('Discard: then the session\'s terminal (the tile)', !!c4 && c4.toTile === 'T2', c4);
    const st3 = await pageState(fr3, 'ask_t3');
    check('Discard clears the page', !!st3 && st3.cleared === 1 && Object.keys(st3.state).length === 0, st3);

    // ---- guest: another deck serves the pages; read-only here
    mark();
    deckIn(Object.assign({}, base, { asks: [S2], guest: true, guestWhy: 'the office deck serves them' }));
    deckIn({ op: 'open', mode: 'S2' });
    await waitFor(m => m.ev === 'call' && m.fn === 'askDraftGet' && m.args[0] === 'ask_t3');
    await sleep(120);
    const fr3g = sec2.querySelector('iframe');
    check('guest: the frame was made afresh', !!fr3g && fr3g !== fr3);
    check('guest: Done and Discard off', sec2.querySelector('.ap-done').disabled && sec2.querySelector('.ap-discard').disabled);
    check('guest: the line says why', sec2.querySelector('.ap-flash').textContent === 'Read-only here: the office deck serves them. Answer on that deck, or in the session\'s terminal.', sec2.querySelector('.ap-flash').textContent);
    check('guest: the hint', hint() === 'Cmd+Shift+A closes · read-only: the office deck serves them', hint());
    const stg = await pageState(fr3g, 'ask_t3');
    check('guest: the page is loaded read-only', !!stg && stg.locked === true, stg);
    k = mark();
    fr3g.contentWindow.postMessage({ askpage: '__test_force_change', id: 'ask_t3', qid: 'q2', text: 'nope' }, '*');
    await sleep(150);
    check('guest: a change on the page never reaches the deck', since(k, m => m.ev === 'fire').length === 0, since(k, m => m.ev === 'fire'));
    deckIn({ op: 'close' });
    await sleep(20);
    check('Swift closes it (op close): hidden, and not told back', pane.hidden && since(k, m => m.ev === 'closed').length === 0);

    // ---- a page made before the bridge (never says ready)
    const S3 = { sessionId: 'S3', name: 'sess-three', account: 'vv', rounds: [{ id: 'ask_legacy1', round: 1, total: 1, answered: 0 }], total: 1 };
    deckIn(Object.assign({}, base, { asks: [S3] }));
    deckIn({ op: 'open', mode: 'S3' });
    await sleep(1900);
    const sec3 = $('.ap-sess[data-sid="S3"]'), fr4 = sec3 && sec3.querySelector('iframe');
    check('an older page: its own bar (75vh) and the note', !!fr4 && fr4.style.height === '75vh' && /^Made by an older ask-page skill/.test(fr4.parentNode.querySelector('.ap-rnote').textContent), fr4 && fr4.style.height);
    check('an older page outside the deck: "Answer in its terminal"', sec3.querySelector('.ap-term').textContent === 'Answer in its terminal');
    k = mark();
    sec3.querySelector('.ap-done').click();
    await sleep(60);
    check('an older page: Done says to use its own buttons, calls nothing', /^This page was made by an older ask-page skill/.test(sec3.querySelector('.ap-flash').textContent) && since(k, m => m.ev === 'call').length === 0, sec3.querySelector('.ap-flash').textContent);

    // ---- a page replaced by another session's newer one: the pane follows, the answers came over
    const S4 = { sessionId: 'S4', name: 'sess-four', account: 'vv', rounds: [{ id: 'ask_t4', round: 1, total: 2, answered: 0 }], total: 2 };
    const S5 = { sessionId: 'S5', name: 'sess-five', account: 'vv', rounds: [{ id: 'ask_t5', round: 2, total: 2, answered: 0, replaces: [{ id: 'ask_t4', sessionId: 'S4', name: 'sess-four', round: 1, moved: 2 }] }], total: 2 };
    deckIn(Object.assign({}, base, { asks: [S4] }));
    deckIn({ op: 'open', mode: 'S4' });
    await sleep(60);
    mark();
    deckIn(Object.assign({}, base, { asks: [S5] }));
    const md5 = await waitFor(m => m.ev === 'mode');
    await sleep(30);
    check('replaced: the pane follows to the newer page', !!md5 && md5.mode === 'S5', md5);
    check('replaced: the note over the pane', pane.querySelector('.ap-note').textContent === 'That page was replaced by sess-five\'s newer one; your 2 answers came over.' && !pane.querySelector('.ap-note').hidden, pane.querySelector('.ap-note').textContent);
    const sec5 = $('.ap-sess[data-sid="S5"]');
    check('replaced: the round note says whose round it replaces', !!sec5 && /Replaces sess-four's round 1: your 2 answers there came over/.test(sec5.querySelector('.ap-rnote').textContent), sec5 && sec5.querySelector('.ap-rnote').textContent);

    // ---- recently answered, in the every-session view: a row opens its page read-only (remote: a frame)
    mark();
    deckIn(Object.assign({}, base, { asks: [], askLog: [Object.assign({}, L1, { id: 'ask_t1' })], remote: true }));
    deckIn({ op: 'open', mode: 'all', scrollTo: '__recent' });
    await sleep(60);
    check('nothing waiting: the bar', title() === 'Asks · nothing waiting', title());
    const rrow = $('.ap-lrow[data-id="ask_t1"]');
    check('recently answered: a row, closed', !!rrow && !rrow.classList.contains('open'));
    rrow.querySelector('.ap-lhead').click();
    const ls = await waitFor(m => m.ev === 'call' && m.fn === 'askLogState');
    check('a row opened: its page read-only (askLogState)', !!ls && ls.args[0] === 'ask_t1' && rrow.classList.contains('open') && !!rrow.querySelector('iframe'), ls);
    const stl = await pageState(rrow.querySelector('iframe'), 'ask_t1');
    check('the answered page is locked, with the log\'s state', !!stl && stl.locked === true && stl.state.q1.pick === 'o:2', stl);
    deckIn({ op: 'close' });

    // ---- a call the deck refuses: said in the section
    window.__reply = m => (m.fn === 'askDone' ? { ok: false, error: 'none of these pages is that session\'s' } : m.fn === 'askDraftGet' ? { ok: true, value: null } : { ok: true, value: null });
    const S6 = { sessionId: 'S1', name: 'sess-one', account: 'vv', rounds: [{ id: 'ask_t6', round: 1, total: 2, answered: 0 }], total: 2 };
    mark();
    deckIn(Object.assign({}, base, { asks: [S6], remote: false }));
    deckIn({ op: 'open', mode: 'S1' });
    await waitFor(m => m.ev === 'fire' && m.fn === 'askDraft' && m.args[0] === 'ask_t6');
    const sec6 = $('.ap-sess[data-sid="S1"]'), fr6 = sec6.querySelector('iframe[src*="ask_t6"]');
    sec6.querySelector('.ap-done').click();
    await sleep(80);
    check('Done with nothing answered: said, nothing sent', /^Nothing answered yet/.test(sec6.querySelector('.ap-flash').textContent), sec6.querySelector('.ap-flash').textContent);
    fr6.contentWindow.postMessage({ askpage: '__test_set', id: 'ask_t6', qid: 'q1', text: 'x' }, '*');
    await waitFor(m => m.ev === 'fire' && m.fn === 'askDraft' && m.args[1].state.q1);
    sec6.querySelector('.ap-done').click();
    await waitFor(m => m.ev === 'call' && m.fn === 'askDone');
    await sleep(60);
    check('a refused Done: the deck\'s words, Done on again, the pane stays', sec6.querySelector('.ap-flash').textContent === 'none of these pages is that session\'s' && !sec6.querySelector('.ap-done').disabled && !pane.hidden, sec6.querySelector('.ap-flash').textContent);
  } catch (e) {
    check('the driver ran to its end', false, String(e && e.stack || e));
  }
  out.push(`${passed} passed, ${failed} failed`);
  const pre = document.createElement('pre'); pre.id = 'askhost-results'; pre.textContent = '\n' + out.join('\n') + '\n';
  document.body.appendChild(pre);
  document.title = `ASKHOST ${passed} passed, ${failed} failed`;
})();
// ---------------------------------------------------------------- DRIVER END
