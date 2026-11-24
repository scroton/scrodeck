'use strict';
// Generate core/test/fixtures/deck/cloud.json from the CANONICAL JavaScript: lib/cloud.js is required as it is,
// and main.js cloudSpec (the task's text rules) is lifted out of main.js's source text and run.
//
//   node core/test/deck/gen-cloud.js
//
// NOTHING REAL IS READ OR CALLED. lib/cloud.js talks to Anthropic's API in the real app; here platform.readOAuth
// is replaced BEFORE lib/cloud.js is loaded (it keeps its own reference) by a fake over made-up tokens, every
// poller's `fetch` seam is replaced by a fake that answers from a table, and Date.now is pinned. No file is
// written but the fixture; no socket is opened.
process.env.TZ = 'UTC';   // pinned (no fixture date depends on it: every date carries its zone)
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..', '..');
const FX = path.join(__dirname, '..', 'fixtures', 'deck');
fs.mkdirSync(FX, { recursive: true });

const platform = require(path.join(ROOT, 'lib', 'platform.js'));
const creds = {};
const real = { readOAuth: platform.readOAuth, now: Date.now };
platform.readOAuth = dir => (dir in creds ? creds[dir] : null);
delete require.cache[require.resolve(path.join(ROOT, 'lib', 'cloud.js'))];
const C = require(path.join(ROOT, 'lib', 'cloud.js'));
const clone = x => (x === undefined ? null : JSON.parse(JSON.stringify(x)));

// ---------------------------------------------------------------- ids
const idCases = ['', 'session_01SyntheticCloudId000000', 'cse_01SyntheticCloudId000000', 'session_', 'cse_', 'session_1234567', 'session_12345678', 'cse_1234567', 'cse_12345678',
  '01SyntheticCloudId000000', '9c1f-uuid', 'session_a"b', 'session_abcdefgh\n', 'session_abcd efgh', 'Session_abcdefgh1', 'CSE_abcdefgh1', 'session_cse_abcdefgh', 'cse_session_abcdefgh',
  'session_session_abcdefgh', ' session_abcdefgh1', 'session_abcdefgh1 ', 'session_abcd_efgh', 'session_\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9', 'xsession_abcdefgh1', 'cse', 'sess', 's', 'cse_abcdefgh-1',
  'session_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789', 'null', '0', 'session_\ud83d\ude00abcdefgh'];
const ids = idCases.map(s => ({ in: s, key: C.cloudKey(s), webId: C.webId(s), webUrl: C.webUrl(s), isCloudId: C.isCloudId(s) }));

// ---------------------------------------------------------------- parseCreated
const E = '\x1b';
const printed = `${E}[?25l${E}[2J${E}[mCreated cloud session: Hello reply\r\n`
  + 'View: https://claude.ai/code/session_01SyntheticCloudId000000?from=cli&m=0\r\n'
  + 'Resume with: claude --teleport session_01SyntheticCloudId000000\r\n'
  + 'This folder is not a git repository, so the cloud session starts empty: nothing from this folder is copied into it.\r\n';
const createdCases = [
  '', printed, printed.replace(/\r\n/g, '\n'), printed.replace(/\r\n/g, '\r'),
  `${E}[1;1HCreated cloud session: Fix it${E}[2;1HView: https://claude.ai/code/session_ABCDEFGH123?x`,
  'Error: --cloud requires a description.\r\nUsage: claude --cloud "your task description"',
  'Created cloud session: a title but no link\r\n',
  'View: https://claude.ai/code/session_NoTitle0001\r\n',
  'Created cloud session:\r\nView: https://claude.ai/code/session_EmptyTitle1\r\n',
  'Created cloud session: \t  \r\nView: https://claude.ai/code/session_BlankTitle1\r\n',
  'Created cloud session:NoSpace\nclaude.ai/code/session_x',
  'Created cloud session: \t padded title \t \u00a0\r\nhttps://claude.ai/code/session_Pad1',
  'Created cloud session: caf\u00e9 \u2014 na\u00efve \ud83d\ude00 title\r\nView: https://claude.ai/code/session_Uni1\r\n',
  'Created cloud session: first\nCreated cloud session: second\nhttps://claude.ai/code/session_One1 https://claude.ai/code/session_Two2',
  'https://claude.ai/code/session_Before1\nCreated cloud session: title after the link',
  // the link split by an escape, by a line end, and whole
  `claude.ai/code/sess${E}[5Cion_Split0001`, 'claude.ai/code/session_\nabc', 'claude.ai/code/session_ then claude.ai/code/session_Second1',
  'claude.ai/code/session_abc-def', 'claude.ai/code/session_abc_def', 'https://claude.ai/code/cse_NotThis01 https://claude.ai/code/session_This1',
  'claudexai/code/session_Dot1', 'CLAUDE.AI/code/session_Case1', 'claude.ai/code/Session_Case2',
  // OSC: ended by BEL, by ESC \, never ended, ended by another ESC that is not ST
  `${E}]0;window title\x07Created cloud session: after an OSC\r\n${E}]8;;https://claude.ai/code/session_InsideOsc1\x07link text${E}]8;;\x07 claude.ai/code/session_Outside1`,
  `${E}]0;Created cloud session: hidden${E}\\Created cloud session: shown${E}[K\r\nclaude.ai/code/session_St1`,
  `${E}]0;never ended Created cloud session: kept claude.ai/code/session_Open1`,
  `${E}]0;broken${E}[31mCreated cloud session: red${E}[0m\nclaude.ai/code/session_Broken1`,
  `${E}]0;a${E}]0;b\x07Created cloud session: nested\nclaude.ai/code/session_Nested1`,
  `${E}]\x07${E}]${E}\\Created cloud session: empty OSCs\nclaude.ai/code/session_EmptyOsc1`,
  // CSI: every parameter character, no final letter, a non-letter final, an ESC at the very end
  `Created cloud session: a${E}[0;1;?<>=25hb\nclaude.ai/code/session_Csi1`,
  `Created cloud session: a${E}[12;b\nclaude.ai/code/session_Csi2`,
  `Created cloud session: a${E}[12~b${E}[ c${E}[\nclaude.ai/code/session_Csi3`,
  `Created cloud session: colour ${E}[1m${E}[36mbold cyan${E}[0m end\r\nView: ${E}[4mhttps://claude.ai/code/session_Styled01${E}[24m\r\n`,
  `Created cloud session: ends with an escape\nclaude.ai/code/session_End1${E}`, `claude.ai/code/session_End2${E}[`, `claude.ai/code/session_End3${E}]`,
  `${E}[?2004h${E}[?1004h${E}[?25l${E}[1;1H${E}[38;2;215;119;87m\u273b${E}[39m Created cloud session: Port the cloud module${E}[K${E}[2;1HView: https://claude.ai/code/session_01AbCdEfGhIjKlMnOpQrStUv?from=cli&m=0${E}[K${E}[3;1HResume with: claude --teleport session_01AbCdEfGhIjKlMnOpQrStUv${E}[K${E}[?25h`,
  'Created cloud session: tab\there\x00nul\x0bvt\x0cff\u2028ls end \u3000\r\nclaude.ai/code/session_Odd1',
  'Created cloud session: \ufeff\u2003  \nclaude.ai/code/session_OnlySpaces1',
  'created cloud session: lower case\nclaude.ai/code/session_Lower1',
  'x'.repeat(5000) + 'Created cloud session: ' + 'long title '.repeat(200) + '\r\n' + 'y'.repeat(5000) + 'claude.ai/code/session_Long1' + 'z'.repeat(50),
];
const created = createdCases.map(s => ({ in: s, out: C.parseCreated(s) }));

// ---------------------------------------------------------------- view, listable
const row = (over = {}) => ({
  id: 'cse_01SyntheticCloudId000000', title: 'Hello reply', status: 'active', worker_status: 'idle', environment_kind: 'anthropic_cloud',
  created_at: '2026-10-12T22:37:30.962408Z', last_event_at: '2026-10-12T22:37:36.853487Z', unread: false,
  config: { model: 'claude-opus-5-5' },
  external_metadata: {
    context_usage: { max_tokens: 1000000, used_tokens: 41474 }, last_served_model: 'claude-opus-5-5',
    post_turn_summary: { needs_action: '', recent_action: 'replied with the single word hello', status_category: 'completed', status_detail: "replied 'hello' as requested; no tools used" },
  },
  ...over,
});
const em = over => ({ external_metadata: { ...row().external_metadata, ...over } });
const ctx = c => em({ context_usage: c });
const rows = [
  null, 0, 1, '', 'cse_abcdefgh12', true, false, [], [{ id: 'cse_abcdefgh12' }], {}, { title: 'x' }, { id: 5 }, { id: null }, { id: ['cse_abcdefgh12'] }, { id: { a: 1 } }, { id: true },
  row(), { id: 'cse_abcdefgh12' }, { id: 'session_abcdefgh12' }, { id: 'abcdefgh12' }, { id: '' }, { id: 'x' },
  // status
  row({ worker_status: 'running' }), row({ worker_status: 'requires_action' }), row({ worker_status: 'WORKER_STATUS_UNSPECIFIED' }), row({ worker_status: null }), row({ worker_status: 7 }), row({ worker_status: ['running'] }),
  row({ status: 'archived' }), row({ status: 'archived', worker_status: 'running' }), row({ status: 'archived', worker_status: 'requires_action', ...em({ pending_action: { tool_name: 'Bash' } }) }),
  row({ status: 'ARCHIVED', worker_status: 'running' }), row({ status: null, worker_status: 'Running' }), row({ status: ['archived'] }),
  // waitingFor
  row({ worker_status: 'requires_action', external_metadata: { pending_action: { tool_name: 'Bash' }, post_turn_summary: { needs_action: '' } } }),
  row({ worker_status: 'requires_action', external_metadata: { pending_action: { tool_name: 'Bash' }, post_turn_summary: { needs_action: 'approve the push' } } }),
  row({ worker_status: 'requires_action', external_metadata: { post_turn_summary: { needs_action: 'answer the question' } } }),
  row({ worker_status: 'requires_action', external_metadata: {} }), row({ worker_status: 'requires_action', external_metadata: { pending_action: 'Bash' } }),
  row({ worker_status: 'requires_action', external_metadata: { pending_action: {} } }), row({ worker_status: 'requires_action', external_metadata: { pending_action: null } }),
  row({ worker_status: 'requires_action', external_metadata: { pending_action: ['Bash'] } }), row({ worker_status: 'requires_action', external_metadata: { pending_action: { tool_name: '' } } }),
  row({ worker_status: 'requires_action', external_metadata: { pending_action: { tool_name: 42 } } }), row({ worker_status: 'requires_action', external_metadata: { pending_action: { tool_name: { deep: 1 } } } }),
  row({ worker_status: 'running', external_metadata: { pending_action: { tool_name: 'Bash' }, post_turn_summary: { needs_action: 'not shown while busy' } } }),
  row({ worker_status: 'requires_action', external_metadata: undefined, post_turn_summary: { needs_action: 'from the row itself' } }),
  // external_metadata of the wrong type, post_turn_summary in either place
  row({ external_metadata: null }), row({ external_metadata: 'text' }), row({ external_metadata: 5 }), row({ external_metadata: [] }), row({ external_metadata: [{ last_served_model: 'no' }] }), row({ external_metadata: true }),
  row({ external_metadata: {}, post_turn_summary: { status_detail: 'row-level detail', recent_action: 'row-level action' } }),
  row({ external_metadata: { post_turn_summary: { recent_action: 'only an action' } }, post_turn_summary: { status_detail: 'unused' } }),
  row({ external_metadata: { post_turn_summary: null }, post_turn_summary: { recent_action: 'row-level action' } }),
  row({ external_metadata: { post_turn_summary: 'a string' } }), row({ external_metadata: { post_turn_summary: 9 } }), row({ external_metadata: { post_turn_summary: [] } }), row({ external_metadata: { post_turn_summary: {} } }),
  row({ external_metadata: { post_turn_summary: { status_detail: '', recent_action: '' } } }), row({ external_metadata: { post_turn_summary: { status_detail: 0, recent_action: 12 } } }),
  row({ external_metadata: { post_turn_summary: { status_detail: { a: 1 } } } }), row({ external_metadata: { post_turn_summary: { status_detail: 'caf\u00e9 \ud83d\ude00\nline two' } } }),
  // title
  row({ title: '' }), row({ title: '   ' }), row({ title: '  padded \t\n' }), row({ title: '\u00a0\u3000nbsp padded\ufeff' }), row({ title: 5 }), row({ title: null }), row({ title: ['x'] }), row({ title: { a: 1 } }), row({ title: true }),
  row({ title: 'caf\u00e9 \u2014 \ud83d\ude00' }), row({ title: undefined }), row({ title: '\n' }),
  // environment kind
  row({ environment_kind: 'bridge' }), row({ environment_kind: '' }), row({ environment_kind: null }), row({ environment_kind: undefined }), row({ environment_kind: 0 }), row({ environment_kind: 3 }), row({ environment_kind: { k: 'bridge' } }), row({ environment_kind: 'BRIDGE' }),
  // dates
  row({ last_event_at: undefined }), row({ last_event_at: undefined, updated_at: '2026-10-12T23:00:00Z' }), row({ last_event_at: 'garbage', updated_at: '2026-10-12T23:00:00Z' }), row({ last_event_at: '', updated_at: '', created_at: '' }),
  row({ last_event_at: null, updated_at: null, created_at: null }), row({ last_event_at: undefined, created_at: undefined }), row({ last_event_at: '1970-01-01T00:00:00Z', updated_at: '2026-10-12' }),
  row({ last_event_at: '1970-01-01T00:00:00Z', updated_at: '1970-01-01T00:00:00.000Z', created_at: '1970-01-01T00:00:00Z' }), row({ last_event_at: '1969-12-31T23:59:59Z' }), row({ last_event_at: '1970-01-01T00:00:00.001Z' }),
  row({ last_event_at: '2026-10-12T22:37:30+02:00', created_at: '2026-10-12T22:37:30-07:30' }), row({ last_event_at: '2026-10-12T22:37Z', created_at: '2026-10-12T22:37:30.5Z' }),
  row({ last_event_at: '2026-10-12T22:37:30.123456789+05:30', created_at: '2026-10-12T22:37:30+0200' }), row({ last_event_at: '2026-13-01T00:00:00Z', created_at: '2026-10-32' }),
  row({ last_event_at: '2026-10-12T22:37:60Z', created_at: '2026-10-12T22:37:30.Z' }), row({ last_event_at: ' 2026-10-12T22:37:30Z', created_at: '2026-10-12T22:37:30Z ' }), row({ last_event_at: '2026-10-12 22:37:30Z', created_at: '2026-10-12t22:37:30z' }),
  row({ last_event_at: true, updated_at: {}, created_at: false }), row({ last_event_at: '2026-10-12T24:00:00Z', created_at: '2024-02-29' }), row({ last_event_at: 'not a date', updated_at: 'nor this', created_at: '2026-10-12T22:37:30Z' }),
  // model
  row(em({ last_served_model: '' })), row(em({ last_served_model: null })), row({ ...em({ last_served_model: undefined }), config: null }), row({ ...em({ last_served_model: undefined }), config: {} }),
  row({ ...em({ last_served_model: undefined }), config: 'text' }), row({ ...em({ last_served_model: undefined }), config: { model: '' } }), row({ ...em({ last_served_model: undefined }), config: 0 }),
  row({ ...em({ last_served_model: undefined }), config: { model: 7 } }), row({ ...em({ last_served_model: undefined }), config: [] }), row({ ...em({ last_served_model: { id: 'obj' } }) }), row({ ...em({ last_served_model: 0 }), config: { model: 'from config' } }),
  // context
  row(ctx({ max_tokens: 1000000, used_tokens: 0 })), row(ctx({ max_tokens: 1000000, used_tokens: 4999 })), row(ctx({ max_tokens: 1000000, used_tokens: 5000 })), row(ctx({ max_tokens: 200, used_tokens: 1 })), row(ctx({ max_tokens: 200, used_tokens: 3 })),
  row(ctx({ max_tokens: 200000, used_tokens: 200000 })), row(ctx({ max_tokens: 200000, used_tokens: 199000 })), row(ctx({ max_tokens: 200000, used_tokens: 250000 })), row(ctx({ max_tokens: 0, used_tokens: 10 })), row(ctx({ max_tokens: -5, used_tokens: 10 })),
  row(ctx({ max_tokens: 100, used_tokens: -1 })), row(ctx({ max_tokens: 100, used_tokens: -0 })), row(ctx({ max_tokens: 100 })), row(ctx({ used_tokens: 100 })), row(ctx({})), row(ctx(null)), row(ctx(undefined)), row(ctx('text')), row(ctx(12)), row(ctx([])), row(ctx([100, 50])),
  row(ctx({ max_tokens: '1000', used_tokens: '250' })), row(ctx({ max_tokens: '1000', used_tokens: 'abc' })), row(ctx({ max_tokens: 'abc', used_tokens: 5 })), row(ctx({ max_tokens: 100, used_tokens: null })), row(ctx({ max_tokens: null, used_tokens: 5 })),
  row(ctx({ max_tokens: true, used_tokens: true })), row(ctx({ max_tokens: 100, used_tokens: false })), row(ctx({ max_tokens: 100, used_tokens: '' })), row(ctx({ max_tokens: ' 400 ', used_tokens: ' 3 ' })), row(ctx({ max_tokens: 100, used_tokens: {} })),
  row(ctx({ max_tokens: [200], used_tokens: [50] })), row(ctx({ max_tokens: 100, used_tokens: [] })), row(ctx({ max_tokens: 100, used_tokens: [1, 2] })), row(ctx({ max_tokens: 3, used_tokens: 1 })), row(ctx({ max_tokens: 3, used_tokens: 2 })),
  row(ctx({ max_tokens: 1000, used_tokens: 5 })), row(ctx({ max_tokens: 1000, used_tokens: 4.99 })), row(ctx({ max_tokens: 0.5, used_tokens: 0.25 })), row(ctx({ max_tokens: 1e300, used_tokens: 1e299 })), row(ctx({ max_tokens: 1, used_tokens: 1e308 })),
  row(ctx({ max_tokens: 1000, used_tokens: 15 })), row(ctx({ max_tokens: 1000, used_tokens: 25 })), row(ctx({ max_tokens: 200, used_tokens: 199 })),
  // unread
  row({ unread: true }), row({ unread: 1 }), row({ unread: 0 }), row({ unread: '' }), row({ unread: 'no' }), row({ unread: null }), row({ unread: undefined }), row({ unread: {} }), row({ unread: [] }),
  // unknown fields ride along unread
  row({ extra: { nested: [1, 2, 3] }, another_field: 'ignored' }),
];
const views = rows.map(r => ({ in: clone(r), out: C.view(clone(r)) }));
const listableIn = [null, 0, '', false, 'text', 1, true, [], {}, { status: 'archived' }, { status: 'idle' }, { environmentKind: 'bridge' }, { status: 'idle', environmentKind: 'bridge' }, { status: 'archived', environmentKind: 'anthropic_cloud' },
  { status: 'busy', environmentKind: 'anthropic_cloud' }, { status: 'waiting', environmentKind: null }, { status: 'Archived', environmentKind: 'Bridge' }, { status: ['archived'] }, { environmentKind: ['bridge'] },
  ...rows.map(r => C.view(clone(r)))];
const listable = listableIn.map(v => ({ in: clone(v), out: C.listable(v) }));

// ---------------------------------------------------------------- main.js cloudSpec: the task's text rules, lifted out of the source
const MAIN = fs.readFileSync(path.join(ROOT, 'main.js'), 'utf8').replace(/\r\n/g, '\n');
const lift = (re, what) => { const m = re.exec(MAIN); if (!m) throw new Error(`main.js: ${what} not found`); return m[1]; };
const CLOUD_TASK_MAX = Number(lift(/\nconst CLOUD_TASK_MAX = ([\d_]+);/, 'CLOUD_TASK_MAX').replace(/_/g, ''));
const CLOUD_TICK_MS = Number(lift(/\nconst CLOUD_TICK_MS = ([\d_]+);/, 'CLOUD_TICK_MS').replace(/_/g, ''));
const specSrc = lift(/\n(function cloudSpec\(s\) \{\n[\s\S]*?\n\})\n/, 'cloudSpec');
// every folder exists for this fake fs: only the task's rules are under test
const cloudSpec = new Function('CLOUD_TASK_MAX', 'settings', 'HOME', 'fs', `${specSrc}; return cloudSpec;`)(CLOUD_TASK_MAX, { cwd: 'C:\\fake' }, 'C:\\fake-home', { statSync: () => ({ isDirectory: () => true }) });
const taskIn = ['', '   ', '\r\n\t ', null, 0, false, 'do it', '  do it  ', 'line one\r\nline two\rline three\nline four\r\n\r\n', '\r\r\n\n\rmiddle\r\r\n', '\rlead', 'trail\r', 'a\r\n\r\nb', 'a\n\r\nb\r',
  '\u00a0\u3000 nbsp around \ufeff', 'caf\u00e9 \u2014 \ud83d\ude00\r\nsecond', 7, 12.5, true, ['a', 'b'], [], [null, 1, ['x', null]], { a: 1 }, '\u2028inside\u2028stays\u2028', 'tab\tinside',
];
// the long ones are kept in the fixture as a recipe (pre + unit x times + post), input and cleaned task alike
const big = (unit, times, pre = '', post = '') => ({ pre, unit, times, post });
const grow = r => r.pre + r.unit.repeat(r.times) + r.post;
const M = CLOUD_TASK_MAX;
const longIn = [[big('x', M), big('x', M)], [big('x', M + 1), null], [big('x', M, '  ', '\r\n'), big('x', M)], [big('x', M - 2, '', '\r\ny'), big('x', M - 2, '', '\ny')], [big('x', M - 2, '', '\r\nyz'), null],
  [big('\ud83d\ude00', M / 2), big('\ud83d\ude00', M / 2)], [big('\ud83d\ude00', M / 2, '', 'a'), null], [big('\u00e9', M), big('\u00e9', M)], [big('\u00e9', M + 1), null], [big('\u4e2d', M, '\r', '\r'), big('\u4e2d', M)],
  [big('y', 123456), null], [big('y', 1234567), null], [big('ab\r\n', M / 3 - 1, '', 'ab'), big('ab\n', M / 3 - 1, '', 'ab')], [big('ab\r\n', M / 3, '', 'ab'), null]];
const spec = t => { try { return { task: cloudSpec({ task: t, cwd: 'C:\\x' }).task, error: null }; } catch (e) { return { task: null, error: e.message }; } };
const tasks = taskIn.map(t => ({ in: t, ...spec(t) }));
const longTasks = longIn.map(([r, want]) => {
  const got = spec(grow(r));
  if ((got.task === null) !== (want === null) || (want && grow(want) !== got.task)) throw new Error('a long task did not clean to its recipe: ' + JSON.stringify(r).slice(0, 80));
  return { in: r, task: want, error: got.error };
});
// lib/cloud.js does not export its intervals: read out of its source
const LIB = fs.readFileSync(path.join(ROOT, 'lib', 'cloud.js'), 'utf8');
const libConst = n => { const m = new RegExp(`\\nconst ${n} = ([\\d_]+);`).exec(LIB); if (!m) throw new Error(`lib/cloud.js: ${n} not found`); return Number(m[1].replace(/_/g, '')); };

// ---------------------------------------------------------------- the poller: the REAL class, a fake fetch, fake credentials, a pinned clock
const API = 'https://api.anthropic.com';
const LIST = `${API}/v1/code/sessions?limit=100&statuses=active`;
const one = id => `${API}/v1/code/sessions/${id}`;
let clock = 0;
Date.now = () => clock;
const tok = (t, more) => ({ claudeAiOauth: { accessToken: t, subscriptionType: 'max', expiresAt: 9_000_000_000_000, ...more } });

function scenario(name, accounts0, script) {
  for (const k of Object.keys(creds)) delete creds[k];
  let accounts = clone(accounts0);
  const poller = new C.CloudPoller(accounts);
  let responses = {}, advance = 0;
  const calls = [];
  poller.fetch = async (url, opts) => {
    const h = opts.headers, auth = String(h.Authorization);
    const t = auth.replace(/^Bearer /, '');
    const dir = Object.keys(creds).find(d => creds[d] && creds[d].claudeAiOauth && String(creds[d].claudeAiOauth.accessToken) === t);
    const acct = accounts.find(a => a.dir === dir);
    // the token itself never reaches the fixture's record of the call
    calls.push({ url, account: acct ? acct.id : null, headers: Object.keys(h).sort(), auth: auth.replace(/^Bearer .*/, 'Bearer <token>'), beta: h['anthropic-beta'], version: h['anthropic-version'], hasSignal: !!opts.signal });
    clock += advance;
    const r = (responses[t] || {})[url];
    if (!r) throw new Error('fetch failed');
    return { status: r.status, ok: r.status >= 200 && r.status < 300, headers: { get: k => (r.headers || {})[k.toLowerCase()] ?? null }, json: async () => (r.text !== undefined ? JSON.parse(r.text) : r.json) };
  };
  const steps = [];
  const step = async (label, set) => {
    clock = set.clock;
    advance = set.advance || 0;
    if (set.accounts) { accounts = clone(set.accounts); poller.accounts = accounts; }
    Object.assign(creds, clone(set.creds || {}));
    if (set.responses) responses = clone(set.responses);
    for (const id of set.soon || []) poller.soon(id);
    const before = calls.length;
    const tiles = set.tiles || [];
    const first = poller.poll(clone(tiles));
    const busyDuring = poller.busy;
    const second = set.concurrent ? await poller.poll(clone(tiles)) : null;
    const changed = await first;
    const sessions = {};
    for (const id of script.probes) sessions[id] = clone(poller.session(id));
    steps.push({ label, clock: set.clock, clockAfter: clock, advance, setAccounts: set.accounts ? clone(accounts) : null, accounts: clone(accounts), creds: clone(creds), responses: clone(responses), soon: set.soon || [], tiles: clone(tiles),
      concurrent: !!set.concurrent, busyDuring, second, changed, calls: calls.slice(before), sessions, lists: clone(poller.lists),
      elsewhere: script.hosted.map(h => ({ hosted: h, out: clone(poller.elsewhere(new Set(h))) })), busyAfter: poller.busy });
  };
  return script.run(step).then(() => ({ name, accounts: clone(accounts0), steps }));
}

const S1 = 'session_01AAAAAAAAAAAAAAAAAAAAAA', S2 = 'cse_01BBBBBBBBBBBBBBBBBBBBBB', S3 = 'session_01CCCCCCCCCCCCCCCCCCCCCC', S4 = 'session_01DDDDDDDDDDDDDDDDDDDDDD', S5 = 'cse_01EEEEEEEEEEEEEEEEEEEEEE', S6 = 'cse_01FFFFFFFFFFFFFFFFFFFFFF';
const W = id => C.webId(id), K = id => C.cloudKey(id);
const odd = 'session_a b/\u00fc?&=#\'(x)*!~.-_%';
const T = 1_800_000_000_000;
const A3 = [{ id: 'a1', dir: 'D:/fake/a1' }, { id: 'a2', dir: 'D:/fake/a2' }, { id: 'a3', dir: 'D:/fake/a3' }];
const ok = (json, headers) => ({ status: 200, json, ...(headers ? { headers } : {}) });
const r1 = over => row({ id: 'cse_' + K(S1), title: 'First', ...over });
const r2 = over => row({ id: 'cse_' + K(S2), title: 'Second', ...over });
const r3 = over => row({ id: 'cse_' + K(S3), title: 'Third', ...over });
const r5 = over => row({ id: S5, title: 'Fifth, listed only', worker_status: 'running', ...over });
const r6 = over => row({ id: S6, title: 'Sixth', ...over });
const tiles3 = [{ cloudId: S1, account: 'a1' }, { cloudId: S2, account: 'a2' }, { cloudId: S3, account: 'a3' }];

const main = {
  probes: [S1, 'cse_' + K(S1), S2, W(S2), S3, S4, S5, W(S5), S6, odd, '', 'session_', 'nothing', K(S1)],
  hosted: [[], [K(S1)], [K(S1), K(S5)], [K(S5), K(S6), K(S2), K(S3)], [S1, S5], ['']],
  run: async step => {
    await step('no credentials anywhere: every tile says so, nothing is asked', { clock: T, tiles: [...tiles3, { cloudId: '', account: 'a1' }, { cloudId: S4, account: 'zz' }] });
    await step('five seconds later: nothing is due', { clock: T + 5_000, tiles: tiles3 });
    await step('14.999 s: still inside TILE_MS', { clock: T + 14_999, tiles: tiles3 });
    await step('15 s: ok (response_shape) / 401 / 429 with retry-after; the lists are not due', { clock: T + 15_000, tiles: tiles3,
      creds: { 'D:/fake/a1': tok('T1'), 'D:/fake/a2': tok('T2'), 'D:/fake/a3': tok('T3') },
      responses: { T1: { [one(W(S1))]: ok({ response_shape: r1({ worker_status: 'running' }) }) }, T2: { [one(W(S2))]: { status: 401 } }, T3: { [one(W(S3))]: { status: 429, headers: { 'retry-after': '120' } } } } });
    await step('59.999 s: the lists are still not due (LIST_MS); the tiles are asked again, a3 backing off without a request', { clock: T + 59_999, tiles: tiles3 });
    await step('75 s: the lists come round (a2 refused, a3 backing off without a request); the tiles again', { clock: T + 75_000, tiles: tiles3,
      responses: { T1: { [one(W(S1))]: ok({ session: r1({ worker_status: 'requires_action', external_metadata: { pending_action: { tool_name: 'Bash' } } }) }),
        [LIST]: ok({ data: [r1(), r5(), row({ id: 'cse_bridge00000001', environment_kind: 'bridge' }), row({ id: 'cse_archived000001', status: 'archived' }), null, { title: 'no id' }, 'text', 7, r6({ unread: true })] }) },
        T2: { [one(W(S2))]: { status: 401 }, [LIST]: { status: 401 } }, T3: { [one(W(S3))]: ok({ response_shape: r3() }), [LIST]: ok({ data: [r3()] }) } } });
    await step('soon(): asked at the next pass whatever its age (a bare row answers)', { clock: T + 80_000, tiles: tiles3, soon: ['cse_' + K(S1), 'nothing', ''],
      responses: { T1: { [one(W(S1))]: ok(r1({ title: '  Renamed  ', unread: true })) }, T2: {}, T3: {} } });
    await step('soon() on a session only a list knows: no tile, so nothing is asked', { clock: T + 81_000, tiles: tiles3, soon: [S5] });
    await step('the retry-after is over: a3 is asked; a 429 with no header backs off ten minutes; its list is due but backing off', { clock: T + 15_000 + 120_000, tiles: tiles3,
      responses: { T1: { [one(W(S1))]: ok({ response_shape: r1() }), [LIST]: ok({ data: [r6(), r1({ status: 'archived' })] }) }, T2: { [one(W(S2))]: { status: 500 }, [LIST]: { status: 503 } }, T3: { [one(W(S3))]: { status: 429 }, [LIST]: ok({ data: [r3()] }) } } });
    await step('9 min 59.999 s into the back-off: a3 is left alone', { clock: T + 135_000 + 599_999, tiles: [{ cloudId: S3, account: 'a3' }], responses: { T1: {}, T2: {}, T3: { [one(W(S3))]: ok({ response_shape: r3({ worker_status: 'running' }) }), [LIST]: ok({ data: [r3(), r5({ title: 'Fifth, now on a3' })] }) } } });
    await step('ten minutes: a3 answers (its list was asked for a millisecond ago, while it was backing off)', { clock: T + 135_000 + 600_000, soon: [S3], tiles: [{ cloudId: S3, account: 'a3' }] });
    await step('retry-after that is not a number: ten minutes', { clock: T + 2_000_000, tiles: [{ cloudId: S3, account: 'a3' }], responses: { T3: { [one(W(S3))]: { status: 429, headers: { 'retry-after': 'soon' } } } } });
    await step('... so 9 minutes on nothing is asked of a3', { clock: T + 2_000_000 + 540_000, tiles: [{ cloudId: S3, account: 'a3' }], responses: { T3: { [one(W(S3))]: ok(r3()), [LIST]: ok({ data: [] }) } } });
    await step('... and after ten it is; retry-after " 2 " is two seconds, on the list this time', { clock: T + 2_000_000 + 600_000, tiles: [{ cloudId: S3, account: 'a3' }], responses: { T3: { [one(W(S3))]: ok(r3()), [LIST]: { status: 429, headers: { 'retry-after': ' 2 ' } } } } });
    await step('a network error on a tile keeps its last answer and says why', { clock: T + 3_000_000, tiles: tiles3, responses: {} });
    await step('a body that is not JSON (tile and list)', { clock: T + 3_100_000, tiles: tiles3, responses: { T1: { [one(W(S1))]: { status: 200, text: '<html>' }, [LIST]: { status: 200, text: 'not json' } } } });
    await step('JSON that is not a session: nothing is stored, nothing changed by the tile; a list with no data empties', { clock: T + 3_200_000, tiles: tiles3,
      responses: { T1: { [one(W(S1))]: { status: 200, text: 'null' }, [LIST]: ok({ data: { not: 'an array' } }) }, T2: { [one(W(S2))]: { status: 200, text: '5' }, [LIST]: { status: 200, text: '[1,2]' } }, T3: { [one(W(S3))]: ok({ response_shape: {}, session: 'x' }), [LIST]: { status: 200, text: 'null' } } } });
    await step('a wrapper with both names: response_shape wins; an empty wrapper is read as the row; a row with no id is nothing', { clock: T + 3_300_000, tiles: tiles3,
      responses: { T1: { [one(W(S1))]: ok({ response_shape: r1({ title: 'From response_shape' }), session: r1({ title: 'From session' }) }), [LIST]: ok({ data: [r1(), r5()] }) },
        T2: { [one(W(S2))]: ok({ response_shape: null, session: 0, ...r2() }), [LIST]: ok({}) }, T3: { [one(W(S3))]: ok({ response_shape: { title: 'no id' } }), [LIST]: ok({ data: [] }) } } });
    await step('an expired token, a login with no token, no login at all', { clock: T + 3_400_000, tiles: tiles3, creds: { 'D:/fake/a1': tok('T1', { expiresAt: 5 }), 'D:/fake/a2': { claudeAiOauth: {} }, 'D:/fake/a3': null } });
    await step('expiresAt 0 is no expiry; expiresAt as text is compared as a number; a login file with nothing in it', { clock: T + 3_500_000, tiles: tiles3, creds: { 'D:/fake/a1': tok('T1', { expiresAt: 0 }), 'D:/fake/a2': tok('T2', { expiresAt: String(T + 3_500_001) }), 'D:/fake/a3': {} },
      responses: { T1: { [one(W(S1))]: ok({ response_shape: r1() }), [LIST]: ok({ data: [r1(), r5()] }) }, T2: { [one(W(S2))]: ok({ response_shape: r2({ worker_status: 'running' }) }), [LIST]: ok({ data: [r2(), row({ id: '' , title: 'A row whose id is empty' })] }) } } });
    await step('the token expires exactly now: still good (expiresAt < now is false)', { clock: T + 3_600_000, tiles: tiles3, creds: { 'D:/fake/a1': tok('T1', { expiresAt: T + 3_600_000 }), 'D:/fake/a2': tok('T2', { expiresAt: T + 3_599_999 }), 'D:/fake/a3': tok('T3') },
      responses: { T1: { [one(W(S1))]: ok({ response_shape: r1() }), [LIST]: ok({ data: [r1(), r5()] }) }, T2: {}, T3: { [one(W(S3))]: ok({ response_shape: r3({ status: 'archived' }) }), [LIST]: ok({ data: [r3({ status: 'archived' })] }) } } });
  },
};

const pacing = {
  probes: [S1, S2, S3, S5, S6, odd, 'session_' + K(odd)],
  hosted: [[], [K(S1)], [K(odd)]],
  run: async step => {
    const two = [{ id: 'a1', dir: 'D:/fake/a1' }, { id: 'a2', dir: 'D:/fake/a2' }];
    await step('the clock moves a second per request: `now` is the pass\'s start, the token is checked at each request', { clock: T, advance: 1_000,
      tiles: [{ cloudId: S1, account: 'a1' }, { cloudId: 'cse_' + K(S1), account: 'a1' }, { cloudId: S5, account: 'a1' }, { cloudId: S6, account: 'a1' }, { cloudId: S2, account: 'a2' }],
      creds: { 'D:/fake/a1': tok('T1', { expiresAt: T + 1_500 }), 'D:/fake/a2': tok('T2') },
      responses: { T1: { [one(W(S1))]: ok({ response_shape: r1() }), [one(W(S5))]: ok({ response_shape: r5() }), [one(W(S6))]: ok({ response_shape: r6() }), [LIST]: ok({ data: [r1(), r5(), r6()] }) },
        T2: { [one(W(S2))]: { status: 429, headers: { 'retry-after': '10' } }, [LIST]: ok({ data: [r2()] }) } } });
    await step('the back-off counted from the moment of the 429, not the start of its pass', { clock: T + 12_500, advance: 1_000, soon: [S2], tiles: [{ cloudId: S2, account: 'a2' }] });
    await step('... 2.5 s more and a2 is asked', { clock: T + 15_000, advance: 250, soon: [S2], tiles: [{ cloudId: S2, account: 'a2' }], creds: { 'D:/fake/a1': tok('T1') },
      responses: { T1: { [one('session_' + encodeURIComponent(K(odd)))]: ok({ response_shape: row({ id: 'cse_' + K(odd), title: 'An odd id' }) }), [LIST]: ok({ data: [r1(), r5(), r6()] }) }, T2: { [one(W(S2))]: ok({ response_shape: r2() }), [LIST]: ok({ data: [r2()] }) } } });
    await step('an id that needs encoding in the URL; the same session twice (both spellings) is asked once', { clock: T + 40_000,
      tiles: [{ cloudId: odd, account: 'a1' }, { cloudId: 'cse_' + K(odd), account: 'a1' }, { cloudId: S1, account: 'a1' }, { cloudId: 'cse_' + K(S1), account: 'a1' }, { cloudId: S1, account: 'a2' }] });
    await step('a pass already running answers false at once', { clock: T + 100_000, concurrent: true, tiles: [{ cloudId: S1, account: 'a1' }] });
    await step('nothing due at all: the pass ends at once, a second one too', { clock: T + 101_000, concurrent: true, tiles: [{ cloudId: S1, account: 'a1' }] });
    await step('no tiles: only the lists, an account with no tile included', { clock: T + 200_000, tiles: [] });
    await step('a tile whose account is unknown is skipped; an account that is new is listed at once; one that left is not', { clock: T + 210_000,
      accounts: [{ id: 'a4', dir: 'D:/fake/a4' }, { id: 'a2', dir: 'D:/fake/a2' }, { id: 'a2', dir: 'D:/fake/a2-again' }],
      soon: [S6, S2], tiles: [{ cloudId: S1, account: 'a1' }, { cloudId: S6, account: 'a4' }, { cloudId: S2, account: 'a2' }], creds: { 'D:/fake/a4': tok('T4') },
      responses: { T1: { [LIST]: ok({ data: [r1(), r5()] }) }, T4: { [one(W(S6))]: ok({ response_shape: r6({ worker_status: 'running' }) }), [LIST]: ok({ data: [r6(), r3()] }) }, T2: { [one(W(S2))]: ok({ response_shape: r2() }), [LIST]: ok({ data: [r2()] }) } } });
    await step('the first account back: its old list is shown again, in the accounts\' order', { clock: T + 220_000, accounts: [{ id: 'a2', dir: 'D:/fake/a2' }, { id: 'a1', dir: 'D:/fake/a1' }, { id: 'a4', dir: 'D:/fake/a4' }], tiles: [] });
    await step('LIST_MS to the millisecond: 59.999 s after the last list', { clock: T + 200_000 + 59_999, accounts: two, tiles: [] });
    await step('... and 60 s', { clock: T + 200_000 + 60_000, accounts: two, tiles: [] });
    await step('no accounts: nothing', { clock: T + 900_000, accounts: [], tiles: [{ cloudId: S1, account: 'a1' }] });
  },
};

(async () => {
  const scenarios = [await scenario('status, errors and back-off', A3, main), await scenario('pacing, ids and accounts', [{ id: 'a1', dir: 'D:/fake/a1' }, { id: 'a2', dir: 'D:/fake/a2' }], pacing)];
  platform.readOAuth = real.readOAuth; Date.now = real.now;
  const data = {
    pinned: { TZ: process.env.TZ, clock: 'Date.now is the step\'s `clock`, plus `advance` ms at every fetch', locale: (1234567).toLocaleString() },
    constants: { TILE_MS: libConst('TILE_MS'), LIST_MS: libConst('LIST_MS'), BACKOFF_MS: libConst('BACKOFF_MS'), CLOUD_TICK_MS, CLOUD_TASK_MAX },
    ids, created, views, listable, tasks, longTasks, scenarios,
  };
  const f = path.join(FX, 'cloud.json');
  fs.writeFileSync(f, JSON.stringify(data, null, 1) + '\n');
  console.log(`wrote cloud -> ${path.relative(process.cwd(), f)} (${fs.statSync(f).size} bytes): ${ids.length} ids, ${created.length} created, ${views.length} views, ${listable.length} listable, ${tasks.length} + ${longTasks.length} tasks, ${scenarios.map(s => s.steps.length).join(' + ')} steps`);
})().catch(e => { console.error(e); process.exit(1); });
