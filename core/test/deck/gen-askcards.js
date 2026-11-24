'use strict';
// Generate core/test/fixtures/deck/askcards.json for core/deck/askcards (the native ask cards):
//   node core/test/deck/gen-askcards.js [<the ask-page skill folder>]
//     (default: %USERPROFILE%\.claude-skill-sources\ask-page\skills\ask-page; it is only READ)
// 1. ask pages built by the REAL ask_page.py (`build --no-open`) in a TEMP state of their own (ASK_PAGE_STATE_DIR,
//    _PAGES_DIR, _DOWNLOADS, _SETTINGS and APPDATA all under one temp folder, removed afterwards): the skill's own
//    sample spec (tests/fixtures/spec5.json) and the specs below (every block kind, multi, a group, one option,
//    three options, no options, a custom reply order, a second round that carries the first round's cards);
//    plus the pages windows/web-test/fixtures has (made by older templates; the old-skill one has no askdata).
// 2. page.html's OWN script run on each page under a small DOM made here, the way the Claude Deck runs it: framed
//    (window.parent is a host), localStorage refused as in a sandboxed frame, the host bridge's load / get / clear
//    messages; scripted clicks (a click on a disabled button does nothing, as in a browser) and typing (the box's
//    value, then its input event). Recorded after every step: S (the page's `change` message), its counts, every
//    card's state chip / aria-checked buttons / box text; at the end answers() and asText() (`get`) with the clock
//    pinned, and JSON.stringify(answers, null, 1) - the answers file the page's own Done writes.
// core/test/deck/askcards-read.js reuses SPECS / SCENARIOS (module.exports) to check the C++'s answers with
// `ask_page.py read`.
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { spawnSync } = require('child_process');

const REPO = path.join(__dirname, '..', '..', '..');
const SKILL_DEFAULT = path.join(os.homedir(), '.claude-skill-sources', 'ask-page', 'skills', 'ask-page');
const SAVED_AT = '2026-11-23T17:00:00.000Z';
const SAVED_MS = Date.parse(SAVED_AT);

// ---------------------------------------------------------------- the specs
const RICH = {
  summary: ['Built **two** things today: `todo-list.xlsx` and the [run log](https://example.invalid/log).', 'Paths: C:/work/a.txt and `D:/work files/b.txt`.'],
  fyi: ['Nothing was sent to **anyone**.'],
  items: [
    { title: 'which lines', multi: true,
      context: ['Pick every line that runs **Saturday**.', '- Node 2: 3 people', '- Node 5: `5 people`', '', '> Alex: keep Node 3 off', '> it is down'],
      options: [{ key: 'a', label: 'Node 2', consequence: 'needs Quinn' }, { key: 'b', label: 'Node 3' }, { key: 'c', label: 'Node 5', consequence: 'the **big** one' }],
      question: 'Which lines run on Saturday?' },
    { title: 'classifier: USPS', group: 'cls', context: ['The classifier reads the label.', '```', 'if job: DEV', '  else: QA', '```'],
      options: [{ key: 'a', label: 'keep it', consequence: 'as now' }, { key: 'b', label: 'switch it off' }], recommend: 'a', question: 'Keep the USPS check?' },
    { title: 'classifier: fallback', group: 'cls', context: 'Only when the label has no job name.',
      options: [{ key: 'a', label: 'on' }, { key: 'b', label: 'off' }], recommend: 'b', question: 'Fall back to the URL?' },
    { title: 'numbers', context: ['| job | MB | cost |', '|---|---|---|', '| Apollo | 2,675 MB | $1,180 |', '| Rose | 30 | - |', '| Total | 2705 | 12.5% |'],
      question: 'Anything to add before the order goes out?' },
    { title: 'one way', context: 'See http://example.invalid/po/1 and C:\\Users\\x\\file.txt for it.',
      options: [{ key: 'a', label: 'go ahead' }], recommend: 'a', question: 'Go ahead with the **one** way?' },
    { title: 'été plan', context: 'Accents: café, Ünïcode, кириллица, emoji 🚀.',
      options: [{ key: 'a', label: 'Été plan' }, { key: 'b', label: 'План Б' }, { key: 'c', label: 'ship it - now' }], recommend: 'c', question: 'Which plan?' },
    { title: 'order of things', context: 'A card for the reply buttons.', options: [{ key: 'a', label: 'first' }, { key: 'b', label: 'second' }], recommend: 'a',
      question: 'First or second?' },
  ],
};
const ROUND2 = {
  summary: ['Round two.'], fyi: [],
  items: [
    { title: 'new one', context: 'A fact.', options: [{ key: 'a', label: 'yes', consequence: 'x' }, { key: 'b', label: 'no' }], recommend: 'a', question: 'Yes?' },
    { title: 'new two', context: 'Another fact.', question: 'Anything to add?' },
  ],
};
// [page name, session, spec ('spec5' = the skill's own), settings (reply_order), build flags]
const SPECS = [
  { name: 'spec5', sid: 'aaaaaaaa-1111-4111-8111-111111111111', session: 'fixture-a', spec: 'spec5', settings: {}, flags: ['--no-carry'] },
  { name: 'rich', sid: 'bbbbbbbb-2222-4222-8222-222222222222', session: 'fixture-b', spec: RICH, settings: { reply_order: ['later', 'yourcall', 'explain', 'check'] }, flags: ['--no-carry'] },
  { name: 'round2', sid: 'bbbbbbbb-2222-4222-8222-222222222222', session: 'fixture-b', spec: ROUND2, settings: { reply_order: ['later', 'yourcall', 'explain', 'check'] }, flags: [] },
];

// ---------------------------------------------------------------- the scenarios (steps by card and button)
const pick = (qid, k) => ({ do: 'pick', qid, k });
const type = (qid, text) => ({ do: 'type', qid, text });
const SCENARIOS = {
  spec5: [
    { name: 'walk', read: true, steps: [pick('Q1', 'b'), type('Q1', 'after Alex reviews it - and ask Casey too'), pick('Q1', 'a'), pick('Q1', 'a'),
      pick('Q2', 'r:later'), pick('Q3', 'r:none'), type('Q3', 'the Zebra needs a new head'), type('Q4', 'Plan Saturday, but short'),
      pick('Q5', 'r:both'), pick('Q5', 'a'), pick('Q4', 'r:drop'), pick('Q2', 'r:explain')] },
    { name: 'fills', steps: [pick('Q4', 'r:all'), pick('Q4', 'c'), type('Q4', 'Saturday morning only: from 6'), pick('Q4', 'r:workaround'),
      pick('Q1', 'r:none'), pick('Q1', 'r:none'), type('Q2', '  add 200 MB\n\n  then\nwait '), pick('Q2', 'b'), pick('Q3', 'r:after'), pick('Q3', 'r:remind')] },
    { name: 'clear', steps: [pick('Q1', 'a'), type('Q2', 'x'), { do: 'clear' }, pick('Q3', 'b')] },
  ],
  rich: [
    { name: 'walk', read: true, steps: [pick('Q1', 'a'), pick('Q1', 'c'), type('Q1', 'Node 2, Node 5 - and nothing else'), pick('Q1', 'a'), pick('Q1', 'r:yourcall'), pick('Q1', 'b'),
      pick('Q2a', 'r:yourcall'), pick('Q2b', 'r:check'), type('Q3', 'line one\nline two\n\n  line three  '), pick('Q4', 'a'), pick('Q4', 'r:both'),
      type('Q5', 'ÉTÉ PLAN for sure'), pick('Q6', 'r:explain'), pick('Q6', 'r:explain'), type('Q6', 'later'), pick('Q5', 'c')] },
    { name: 'multi', steps: [pick('Q1', 'c'), pick('Q1', 'b'), pick('Q1', 'a'), pick('Q1', 'b'), pick('Q1', 'c'), pick('Q1', 'a'), type('Q1', 'none after all')] },
  ],
  round2: [
    { name: 'drafts', read: true, init: { state: { Q1: { text: '', pick: null, later: true }, 'r1-Q3': { text: 'from the deck', pick: null }, 'r1-Q2a': { text: 'your call', pick: 'r:yourcall' } }, readonly: false },
      steps: [pick('Q2', 'r:later'), pick('r1-Q1', 'b'), type('r1-Q4', 'yes'), pick('Q1', 'a')] },
    { name: 'readonly', init: { state: { Q1: { text: 'yes', pick: 'a' } }, readonly: true }, steps: [pick('Q1', 'b'), type('Q2', 'nope'), { do: 'clear' }] },
  ],
  // the web-test pages (older templates): a pick and some typing on the first two cards
  web: [{ name: 'basic', steps: [{ do: 'pick-first', card: 0 }, { do: 'type-card', card: 1, text: 'add 150 MB instead' }] }],
};

// ---------------------------------------------------------------- building pages with the real skill
function skillEnv(root, s) {
  const env = Object.assign({}, process.env, {
    ASK_PAGE_STATE_DIR: path.join(root, 'state'), ASK_PAGE_PAGES_DIR: path.join(root, 'pages'), ASK_PAGE_DOWNLOADS: path.join(root, 'dl'),
    ASK_PAGE_SESSION_ID: s.sid, ASK_PAGE_SESSION_NAME: s.session, ASK_PAGE_ACCOUNT: 'fixture', ASK_PAGE_SETTINGS: path.join(root, 'settings.json'),
    APPDATA: path.join(root, 'appdata'), PYTHONIOENCODING: 'utf-8' });
  for (const k of ['TERM_PROGRAM', 'CLAUDE_CODE_SESSION_ID', 'CLAUDE_CONFIG_DIR']) delete env[k];
  return env;
}
function python(skill, root, s, args, input) {
  const r = spawnSync(process.env.PYTHON || 'python', [path.join(skill, 'scripts', 'ask_page.py'), ...args],
    { env: skillEnv(root, s), encoding: 'utf-8', input, windowsHide: true });
  if (r.error) throw r.error;
  return r;
}
// builds every page of SPECS into `root` (a fresh temp folder); -> [{ name, id, file, sid, session }]
function buildPages(skill, root) {
  for (const d of ['state', 'pages', 'dl', 'appdata']) fs.mkdirSync(path.join(root, d), { recursive: true });
  const out = [];
  for (const s of SPECS) {
    fs.writeFileSync(path.join(root, 'settings.json'), JSON.stringify(Object.assign({ theme_default: 'dark' }, s.settings)));
    const specFile = s.spec === 'spec5' ? path.join(skill, 'tests', 'fixtures', 'spec5.json') : path.join(root, s.name + '.spec.json');
    if (s.spec !== 'spec5') fs.writeFileSync(specFile, JSON.stringify(s.spec, null, 1));
    const r = python(skill, root, s, ['build', specFile, '--no-open', ...s.flags]);
    const m = /^id: (\S+)/m.exec(r.stdout || '');
    if (r.status !== 0 || !m) throw new Error(`ask_page.py build ${s.name} failed (${r.status}): ${r.stdout}\n${r.stderr}`);
    const st = JSON.parse(fs.readFileSync(path.join(root, 'state', s.sid + '.json'), 'utf-8'));
    const rnd = st.rounds.find(x => x.id === m[1]);
    out.push({ name: s.name, id: m[1], file: rnd.page, sid: s.sid, session: s.session, spec: s });
  }
  return out;
}

// ---------------------------------------------------------------- page.html's script under a small DOM
function runPage(html, scenario, name) {
  const scripts = [...html.matchAll(/<script(\s[^>]*)?>([\s\S]*?)<\/script>/g)];
  const data = scripts.find(m => /id="askdata"/.test(m[1] || ''));
  if (!data) return null;                          // an older skill's page: no data, nothing to run
  const code = scripts.filter(m => !/application\/json/.test(m[1] || '')).map(m => m[2]);
  // the cards: their buttons (data-pick, disabled), in the page's order
  const cards = [];
  for (const m of html.matchAll(/<article class="([^"]*)" id="([^"]+)" data-qid="([^"]+)">([\s\S]*?)<\/article>/g)) {
    const buttons = [...m[4].matchAll(/<button type="button" class="([^"]*)" data-pick="([^"]+)"([^>]*)>/g)].map(b => ({ cls: b[1], pick: b[2], disabled: / disabled(\s|$|>)/.test(b[3]) || /\sdisabled$/.test(b[3]) }));
    cards.push({ cls: m[1], qid: m[3], buttons, hasCopy: /class="copy"/.test(m[4]) });
  }
  // ---- the DOM
  class ClassList { constructor() { this.s = new Set(); } add(...c) { c.forEach(x => this.s.add(x)); } remove(...c) { c.forEach(x => this.s.delete(x)); }
    toggle(c, on) { if (on === undefined) on = !this.s.has(c); if (on) this.s.add(c); else this.s.delete(c); return on; } contains(c) { return this.s.has(c); } }
  class El {
    constructor(tag, cls) { this.tagName = tag.toUpperCase(); this.classList = new ClassList(); (cls || '').split(/\s+/).filter(Boolean).forEach(c => this.classList.add(c));
      this.dataset = {}; this.attrs = {}; this.style = { setProperty() {} }; this.kids = []; this.listeners = {}; this.textContent = ''; this.value = ''; this.disabled = false; this.readOnly = false; this.id = ''; }
    get scrollHeight() { return 40; } get offsetHeight() { return 30; }
    addEventListener(ev, f) { (this.listeners[ev] = this.listeners[ev] || []).push(f); }
    fire(ev) { for (const f of this.listeners[ev] || []) f({ currentTarget: this, target: this }); }
    setAttribute(k, v) { this.attrs[k] = String(v); } getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; } removeAttribute(k) { delete this.attrs[k]; }
    append(...k) { this.kids.push(...k); } appendChild(k) { this.kids.push(k); return k; } insertBefore(k) { this.kids.unshift(k); return k; } remove() {}
    focus() { doc.activeElement = this; } select() {} click() { this.fire('click'); }
    all() { const o = []; const w = e => { for (const k of e.kids) { o.push(k); w(k); } }; w(this); return o; }
    matches1(sel) {
      const m = /^([a-z]*)((?:\.[\w-]+)*)(\[[\w-]+\])?$/.exec(sel); if (!m) throw new Error('selector ' + sel);
      if (m[1] && this.tagName !== m[1].toUpperCase()) return false;
      for (const c of (m[2] || '').split('.').filter(Boolean)) if (!this.classList.contains(c)) return false;
      if (m[3] && !(m[3].slice(1, -1).replace('data-', '') in this.dataset)) return false;
      return true;
    }
    querySelectorAll(sel) {
      const parts = sel.trim().split(/\s+/);
      let set = [this];
      for (const p of parts) { const next = []; for (const e of set) for (const d of e.all()) if (d.matches1(p) && !next.includes(d)) next.push(d); set = next; }
      return set;
    }
    querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  }
  const byId = {};
  const doc = new El('#document');
  doc.documentElement = new El('html'); doc.body = new El('body'); doc.kids.push(doc.body); doc.activeElement = doc.body;
  doc.getElementById = id => byId[id] || null;
  doc.createElement = t => new El(t);
  const header = new El('header'); doc.body.append(header);
  for (const id of ['count', 'copyAll', 'dlJson', 'dlTxt', 'flash', 'theme']) { const e = new El(id === 'count' || id === 'flash' ? 'span' : 'button'); e.id = id; byId[id] = e; header.append(e); }
  const askdata = new El('script'); askdata.textContent = data[2]; byId.askdata = askdata;
  const els = {};
  for (const c of cards) {
    const a = new El('article', c.cls); a.id = c.qid; a.dataset.qid = c.qid; byId[c.qid] = a;
    const head = new El('div', 'cardhead'); const state = new El('span', 'state chip'); state.textContent = 'open'; head.append(state); a.append(head);
    const btns = c.buttons.map(b => { const e = new El('button', b.cls); e.dataset.pick = b.pick; e.disabled = b.disabled; e.setAttribute('aria-checked', 'false'); a.append(e); return e; });
    const ta = new El('textarea'); a.append(ta);
    const tools = new El('div', 'tools'); const copy = new El('button', 'copy'); const note = new El('span', 'note'); tools.append(copy, note); a.append(tools);
    doc.body.append(a);
    els[c.qid] = { card: a, ta, btns, state };
  }
  // ---- the frame: a host, timers of its own (run on flush), localStorage refused, the clock pinned
  const toHost = [];
  const host = { postMessage(m) { toHost.push(JSON.parse(JSON.stringify(m))); } };
  let timers = [], tid = 0, now = 0;
  const win = { parent: host, listeners: {}, addEventListener(ev, f) { (this.listeners[ev] = this.listeners[ev] || []).push(f); } };
  const RealDate = Date;
  class FakeDate extends RealDate { constructor(...a) { if (a.length) super(...a); else super(SAVED_MS); } static now() { return SAVED_MS; } }
  const ctx = {
    window: win, document: doc, location: { search: '?embed=deck&theme=dark' },
    get localStorage() { throw new Error('SecurityError: the document is sandboxed and lacks the allow-same-origin flag'); },
    navigator: { clipboard: { writeText: () => Promise.resolve() } },
    setTimeout: (f, ms) => { const id = ++tid; timers.push({ id, at: now + (ms || 0), f }); return id; }, clearTimeout: id => { timers = timers.filter(t => t.id !== id); },
    requestAnimationFrame: f => ctx.setTimeout(f, 16), getComputedStyle: () => ({ position: 'sticky' }),
    ResizeObserver: class { observe() {} }, URL: { createObjectURL: () => 'blob:x', revokeObjectURL() {} }, Blob: class {},
    Date: FakeDate, JSON, Object, Array, String, Math, Number, Promise, console,
  };
  ctx.window.window = ctx.window;
  vm.createContext(ctx);
  const flush = () => { for (let n = 0; n < 1000 && timers.length; n++) { timers.sort((a, b) => a.at - b.at || a.id - b.id); const t = timers.shift(); now = Math.max(now, t.at); t.f(); } };
  const send = m => { for (const f of win.listeners.message || []) f({ data: Object.assign({ id: undefined }, m), source: host }); };
  for (const c of code) vm.runInContext(c.replace(/window\.addEventListener/g, 'window.addEventListener'), ctx, { filename: name });
  flush();
  const ready = toHost.find(m => m.askpage === 'ready');
  if (!ready) throw new Error(name + ': the page never said ready');
  const view = () => cards.map(c => { const e = els[c.qid]; return { qid: c.qid, label: e.state.textContent, answered: e.card.classList.contains('answered'), later: e.card.classList.contains('later'),
    checked: e.btns.filter(b => b.getAttribute('aria-checked') === 'true').map(b => b.dataset.pick), text: e.ta.value, readOnly: e.ta.readOnly }; });
  const lastChange = () => { const ch = toHost.filter(m => m.askpage === 'change'); return ch.length ? ch[ch.length - 1] : null; };
  const init = scenario.init || { state: {}, readonly: false };
  toHost.length = 0;
  send({ askpage: 'load', state: init.state, readonly: !!init.readonly }); flush();
  const out = { name: scenario.name, init, steps: [], after: [] };
  const record = () => { const ch = lastChange(); toHost.length = 0;
    return { change: ch ? { state: ch.state, answered: ch.answered, later: ch.later, total: ch.total, drafted: ch.drafted } : null, cards: view() }; };
  out.loaded = record();
  for (const st0 of scenario.steps) {
    let st = st0;
    if (st.do === 'pick-first') { const c = cards[st.card]; st = pick(c.qid, c.buttons[0].pick); }
    if (st.do === 'type-card') { const c = cards[st.card]; st = type(c.qid, st.text); }
    out.steps.push(st);
    if (st.do === 'pick') { const b = els[st.qid].btns.find(x => x.dataset.pick === st.k); if (!b) throw new Error(`${name}: no button ${st.k} on ${st.qid}`); if (!b.disabled) b.fire('click'); }
    else if (st.do === 'type') { const e = els[st.qid]; if (!e.ta.readOnly) { e.ta.value = st.text; e.ta.fire('input'); } }
    else if (st.do === 'clear') send({ askpage: 'clear' });
    flush();
    out.after.push(record());
  }
  send({ askpage: 'get', req: 7 }); flush();
  const ans = toHost.find(m => m.askpage === 'answers');
  if (!ans || ans.req !== 7) throw new Error(name + ': the page did not answer get');
  out.answers = ans.answers; out.text = ans.text; out.locked = ans.locked;
  // the answers file the page's own Done writes: JSON.stringify(answers(), null, 1), run in the page's realm
  out.file = vm.runInContext('JSON.stringify(window.askPage.answers(), null, 1)', ctx);
  out.ready = { answered: ready.answered, later: ready.later, total: ready.total, drafted: ready.drafted, session_id: ready.session_id, round: ready.round };
  return out;
}

module.exports = { SPECS, SCENARIOS, buildPages, python, skillEnv, SAVED_AT };

if (require.main === module) {
  const skill = process.argv[2] || SKILL_DEFAULT;
  if (!fs.existsSync(path.join(skill, 'scripts', 'ask_page.py'))) throw new Error('no ask_page.py under ' + skill);
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'gen-askcards-'));
  try {
    const pages = [];
    for (const b of buildPages(skill, root)) {
      const html = fs.readFileSync(b.file, 'utf-8');
      pages.push({ name: b.name, id: b.id, html, scenarios: (SCENARIOS[b.name] || []).map(s => runPage(html, s, b.name)) });
    }
    const webDir = path.join(REPO, 'windows', 'web-test', 'fixtures');
    for (const f of fs.readdirSync(webDir).filter(f => /^ask_.*\.html$/.test(f)).sort()) {
      const html = fs.readFileSync(path.join(webDir, f), 'utf-8');
      const one = runPage(html, SCENARIOS.web[0], f);
      pages.push({ name: 'web:' + f, id: f.replace(/\.html$/, ''), html, scenarios: one ? [one] : [] });
    }
    const tpl = fs.readFileSync(path.join(skill, 'templates', 'page.html'), 'utf-8');
    const fx = { generatedBy: 'core/test/deck/gen-askcards.js', savedAt: SAVED_AT, templateBytes: tpl.length, pages };
    const outFile = path.join(REPO, 'core', 'test', 'fixtures', 'deck', 'askcards.json');
    fs.writeFileSync(outFile, JSON.stringify(fx, null, 1) + '\n');
    console.log(`wrote ${outFile}: ${pages.length} pages, ${pages.reduce((n, p) => n + p.scenarios.length, 0)} scenarios`);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
}
