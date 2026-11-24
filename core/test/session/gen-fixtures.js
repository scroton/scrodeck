'use strict';
// Generate the acceptance fixtures for core/session from the CANONICAL JavaScript: lib/transcript.js and
// lib/sessions.js are required as they are, and the parts of main.js that are not a module (the replay
// buffer, the mode tracking, collectMeta, the spawn environment) are lifted out of its source text and run in
// a sandbox — so the expected values are what the Electron deck's own code produces, not a re-typing of it.
// If main.js is refactored so that a piece can no longer be found, this script throws rather than guess.
//
//   node core/test/session/gen-fixtures.js
//
// Everything fed in is SYNTHETIC (invented records in the shapes lib/transcript.js documents); nothing is
// read from a real config dir. Output: core/test/fixtures/session/*.json.
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { StringDecoder } = require('string_decoder');
const { spawnSync } = require('child_process');

const ROOT = path.join(__dirname, '..', '..', '..');
const FX = path.join(__dirname, '..', 'fixtures', 'session');
fs.mkdirSync(FX, { recursive: true });
const MAIN = fs.readFileSync(path.join(ROOT, 'main.js'), 'utf8').replace(/\r\n/g, '\n');
const { TranscriptTail, cronOnceAt } = require(path.join(ROOT, 'lib', 'transcript.js'));
const { readLive } = require(path.join(ROOT, 'lib', 'sessions.js'));
const hibernate = require(path.join(ROOT, 'lib', 'hibernate.js'));
const QR = require(path.join(ROOT, 'lib', 'quietrestart.js'));

function write(name, data) {
  const f = path.join(FX, name + '.json');
  fs.writeFileSync(f, JSON.stringify(data, null, 1) + '\n');
  console.log(`wrote ${name} -> ${path.relative(process.cwd(), f)} (${fs.statSync(f).size} bytes)`);
}
// ---- lifting source out of main.js
function fnSrc(name) {
  const m = new RegExp(`^(?:async )?function ${name}\\(`, 'm').exec(MAIN);
  if (!m) throw new Error(`main.js: function ${name} not found`);
  // a one-line function is its line (braces balance on it); any other ends at the first `}` in column 0
  const line = MAIN.slice(m.index, MAIN.indexOf('\n', m.index));
  if ((line.match(/\{/g) || []).length === (line.match(/\}/g) || []).length) return line + '\n';
  const end = MAIN.indexOf('\n}\n', m.index);
  if (end < 0) throw new Error(`main.js: end of ${name} not found`);
  return MAIN.slice(m.index, end + 3);
}
function constSrc(name) {
  const m = new RegExp(`^(?:const|let) ${name} = `, 'm').exec(MAIN);
  if (!m) throw new Error(`main.js: const ${name} not found`);
  const eol = MAIN.indexOf('\n', m.index);
  const line = MAIN.slice(m.index, eol);
  if (/;\s*(\/\/.*)?$/.test(line)) return line + '\n';
  const end = MAIN.indexOf('\n};\n', m.index);
  if (end < 0) throw new Error(`main.js: end of ${name} not found`);
  return MAIN.slice(m.index, end + 4);
}
function lineSrc(re, what) {
  const m = re.exec(MAIN);
  if (!m) throw new Error(`main.js: ${what} not found`);
  return m[0];
}
const hex = s => Buffer.from(s, 'utf8').toString('hex');
// FNV-1a 64 over the UTF-8 bytes: big strings are compared by this and their length
function fnv(s) {
  let h = 0xcbf29ce484222325n;
  for (const b of Buffer.from(s, 'utf8')) { h ^= BigInt(b); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; }
  return h.toString(16).padStart(16, '0');
}
const digest = s => ({ utf8Bytes: Buffer.byteLength(s, 'utf8'), fnv: fnv(s), ...(s.length <= 400 ? { hex: hex(s) } : {}) });

// ================================================================ replay
(() => {
  const onData = lineSrc(/tile\.replay\.push\(d\); tile\.replayBytes \+= d\.length;[^\n]*\n\s*while \(tile\.replayBytes > REPLAY_LIMIT[^\n]*\n/, 'the onData replay lines');
  const cut = lineSrc(/t\.replayCut = t\.replayTotal \|\| 0;/, 'the replayCut line of resizeTile');
  const src = [constSrc('REPLAY_LIMIT'), constSrc('MODE_RE'), fnSrc('trackModes'), fnSrc('modePrefix'), fnSrc('replaySinceResize'),
    `function onData(tile, d) { ${onData} trackModes(tile, d); }`,
    `function markResize(t) { ${cut} }`,
    // the phone server's replayFor and tile:attach build the same thing
    `function replayFor(t, sinceResize) { const body = sinceResize ? replaySinceResize(t) : (t.replay && t.replay.length ? t.replay.join('') : ''); const pre = modePrefix(t); return (pre || body) ? pre + body : null; }`,
    'api = { onData, markResize, replayFor, modePrefix, replaySinceResize, REPLAY_LIMIT };'].join('\n');
  const box = { api: null };
  vm.runInNewContext(src, box);
  const A = box.api;
  const fresh = () => ({ replay: [], replayBytes: 0, replayTotal: 0, replayDropped: 0, replayCut: 0, modes: {} });   // spawnTile
  const expand = op => (op.times ? op.d.repeat(op.times) : op.d);
  const state = t => {
    const all = t.replay.join(''), since = A.replaySinceResize(t), full = A.replayFor(t, false), sinceFor = A.replayFor(t, true);
    return { chunks: t.replay.length, bytes: t.replayBytes, total: t.replayTotal, dropped: t.replayDropped, cut: t.replayCut,
      prefixHex: hex(A.modePrefix(t)), modes: Object.fromEntries(Object.entries(t.modes).filter(([, v]) => v).map(([k]) => [k, true])),
      all: digest(all), since: digest(since), replayFor: full === null ? null : digest(full), replayForSince: sinceFor === null ? null : digest(sinceFor) };
  };
  const cases = [];
  const run = (name, ops, note) => {
    const t = fresh();
    const steps = [];
    for (const op of ops) {
      if (op.op === 'push') A.onData(t, expand(op));
      else if (op.op === 'pushUncounted') t.replay.push(op.d);          // spawnTile's "failed to start" line
      else if (op.op === 'resize') A.markResize(t);
      else if (op.op === 'reset') Object.assign(t, fresh());
      steps.push({ ...(op.op === 'push' || op.op === 'pushUncounted' ? { op: op.op, dHex: hex(op.d), times: op.times || 1 } : { op: op.op }), after: state(t) });
    }
    cases.push({ name, note: note || '', steps });
  };
  const K = 1024;
  run('empty', [{ op: 'resize' }], 'nothing written: no replay at all (replayFor answers null)');
  run('small', [{ op: 'push', d: 'hello ' }, { op: 'push', d: 'world\r\n' }]);
  run('modes', [{ op: 'push', d: '\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[?2004hframe' }, { op: 'push', d: 'more\x1b[?1000l\x1b[?1002h' }, { op: 'push', d: '\x1b[?1049l' }, { op: 'push', d: '\x1b[?1003h\x1b[?25l\x1b[?1004h\x1b[?12h' }],
    'the six tracked modes, set and cleared; untracked private modes are ignored');
  run('mode split across chunks', [{ op: 'push', d: 'a\x1b[?10' }, { op: 'push', d: '49h' }, { op: 'push', d: '\x1b[?1000;1006h' }, { op: 'push', d: '\x1b[?2004' }, { op: 'push', d: 'x\x1b[?\x1b[?1006h' }],
    'main.js scans one chunk at a time, so a split sequence is missed; a combined set is not matched either');
  run('since resize', [{ op: 'push', d: 'before-1 ' }, { op: 'push', d: 'before-2 ' }, { op: 'resize' }, { op: 'push', d: 'after-1 ' }, { op: 'push', d: 'after-2' }, { op: 'resize' }, { op: 'push', d: 'x' }]);
  run('resize with nothing after', [{ op: 'push', d: 'abc' }, { op: 'resize' }]);
  run('trim keeps whole chunks', [{ op: 'push', d: 'A', times: 300 * K }, { op: 'push', d: 'B', times: 200 * K }, { op: 'push', d: 'C', times: 100 * K }, { op: 'push', d: 'D', times: 10 }],
    'over 512 K the oldest chunks go, whole');
  run('one huge chunk is kept', [{ op: 'push', d: 'Z', times: 700 * K }, { op: 'push', d: 'tail' }, { op: 'push', d: 'Y', times: 600 * K }],
    'the window never drops its last chunk, however big');
  run('trim then since', [{ op: 'push', d: 'a', times: 400 * K }, { op: 'resize' }, { op: 'push', d: 'b', times: 100 * K }, { op: 'push', d: 'c', times: 100 * K }, { op: 'push', d: 'd', times: 400 * K }, { op: 'resize' }, { op: 'push', d: 'e' }],
    'replayCut is measured against bytes ever written; replayDropped moves the window under it');
  run('cut inside a chunk that was partly before the resize', [{ op: 'push', d: 'x', times: 520 * K }, { op: 'resize' }, { op: 'push', d: 'tail-after' }, { op: 'push', d: 'q', times: 500 * K }, { op: 'push', d: 'r', times: 20 * K }]);
  run('lengths are UTF-16 units', [{ op: 'push', d: 'h\u00e9llo \u2713 ' }, { op: 'push', d: '\ud83d\ude00 emoji' }, { op: 'resize' }, { op: 'push', d: 'tail \u65e5\u672c' }],
    'é counts 1, an emoji 2: replayBytes is a string length');
  run('the limit counts units, not bytes', [{ op: 'push', d: '\u65e5', times: 300 * K }, { op: 'push', d: '\u672c', times: 212 * K }, { op: 'push', d: '!' }],
    '512 K of three-byte characters is exactly at the limit; one more unit trims');
  run('failed start line', [{ op: 'pushUncounted', d: '\r\n[claude-deck] failed to start powershell.exe: File not found: \r\n' }, { op: 'resize' }],
    'pushed without counting: bytes and total stay 0');
  run('reset on respawn', [{ op: 'push', d: '\x1b[?1049hold screen' }, { op: 'resize' }, { op: 'reset' }, { op: 'push', d: 'new process' }]);

  // sinceResize cut at every offset of a mixed string (incl. inside a surrogate pair: UTF-8 cannot carry the lone half)
  const mixed = 'a\u00e9\u2713\ud83d\ude00z';
  const slices = [];
  for (let k = 0; k <= mixed.length; k++) {
    const s = mixed.slice(k);
    slices.push({ skip: k, hex: Buffer.from(s, 'utf8').toString('hex') });   // Buffer turns a lone surrogate into U+FFFD
  }
  // Node's StringDecoder: what reaches main.js from the pty's bytes
  const decode = [];
  const dec = (name, parts) => {
    const d = new StringDecoder('utf8');
    const out = parts.map(p => hex(d.write(Buffer.from(p, 'hex'))));
    decode.push({ name, partsHex: parts, outHex: out, endHex: hex(d.end()) });
  };
  dec('ascii', ['68656c6c6f']);
  dec('two-byte split', ['68c3', 'a96c']);
  dec('three-byte split 1+2', ['e2', '9c93']);
  dec('three-byte split 2+1', ['e29c', '93']);
  dec('four-byte split every way', ['f0', '9f', '98', '80', 'f09f', '9880', 'f09f98', '80']);
  dec('lone continuation', ['80', '41bf42']);
  dec('overlong and invalid leads', ['c080', 'c1bf', 'f5808080', 'ff41fe']);
  dec('truncated then ascii', ['e29c41', 'f09f9841', 'c341']);
  dec('surrogate encoded', ['eda080', 'edb080', 'ed9fbf']);
  dec('e0 and f0 lower bounds', ['e080af', 'e0a080', 'f0808080', 'f0908080', 'f4908080', 'f48fbfbf']);
  dec('incomplete at end', ['41e29c']);
  dec('incomplete four at end', ['f09f98']);
  dec('split invalid', ['e2', '41', 'f09f', 'e29c93', 'c3', 'c3a9']);
  // random bytes, random splits
  let seed = 12345; const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
  const pool = ['41', 'c3a9', 'e29c93', 'f09f9880', '80', 'c3', 'e2', 'f09f', 'ed', 'a0', 'ff', 'e29c', '0a', '1b5b'];
  for (let c = 0; c < 40; c++) {
    let all = ''; for (let i = 0; i < 30; i++) all += pool[rnd(pool.length)];
    const parts = []; let i = 0; while (i < all.length) { const n = 2 * (1 + rnd(5)); parts.push(all.slice(i, i + n)); i += n; }
    dec('random ' + c, parts);
  }
  write('replay', { module: 'replay', generatedFrom: 'main.js (onData, trackModes, modePrefix, replaySinceResize, replayCut) + Node StringDecoder', limit: A.REPLAY_LIMIT, cases, sliceOf: { hex: hex(mixed), slices }, decode });
})();

// ================================================================ transcript
(() => {
  const realNow = Date.now;
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-fx-tr-'));
  const cases = [];
  const ts = (min, sec = 0) => new Date(Date.UTC(2026, 9, 12, 8, min, sec, 250)).toISOString();   // 2026-10-12T08:mm:ss.250Z
  const L = o => JSON.stringify(o);
  const user = (text, extra = {}) => ({ type: 'user', uuid: 'u-' + Math.abs(hashOf(text)), timestamp: ts(1), cwd: 'C:\\Users\\fixture\\proj', sessionId: 'sid-1', message: { role: 'user', content: text }, ...extra });
  const asst = (blocks, extra = {}) => ({ type: 'assistant', timestamp: ts(2), effort: 'high', message: { role: 'assistant', model: 'claude-fable-5-1', content: blocks,
    usage: { input_tokens: 1200, cache_creation_input_tokens: 3400, cache_read_input_tokens: 95000, output_tokens: 800 } }, ...extra });
  function hashOf(s) { let h = 0; for (const c of s) h = (h * 31 + c.charCodeAt(0)) | 0; return h; }
  const toolUse = (id, name, input) => ({ type: 'tool_use', id, name, input });
  const toolResult = (useId, result, extra = {}) => ({ type: 'user', timestamp: ts(3), message: { role: 'user', content: [{ type: 'tool_result', tool_use_id: useId, content: 'ok' }] }, toolUseResult: result, ...extra });
  const note = (id, status, extra = '') => `<task-notification>\n<task-id>${id}</task-id>\n${extra}${status ? `<status>${status}</status>\n` : ''}<summary>done</summary>\n</task-notification>`;

  // chunks: Buffers appended to the file one poll at a time. queries: [{ since, now }] asked after each poll.
  const run = (name, chunks, { now = Date.UTC(2026, 9, 12, 9, 0, 0), queries = [{ since: 0 }], note: n = '', truncateBefore = [] } = {}) => {
    const file = path.join(tmp, name.replace(/\W+/g, '_') + '.jsonl');
    fs.writeFileSync(file, '');
    const tail = new TranscriptTail(file);
    Date.now = () => now;
    const steps = [];
    chunks.forEach((c, i) => {
      const buf = Buffer.isBuffer(c) ? c : Buffer.from(c, 'utf8');
      const restart = truncateBefore.includes(i);
      if (restart) fs.writeFileSync(file, buf); else fs.appendFileSync(file, buf);
      const polled = tail.poll();
      steps.push({ restart, b64: buf.toString('base64'), polled, after: {
        title: tail.title, aiTitle: tail.aiTitle, customTitle: tail.customTitle, lastText: tail.lastText, lastRole: tail.lastRole, lastAt: tail.lastAt,
        lastAssistantAt: tail.lastAssistantAt, lastModel: tail.lastModel, lastEffort: tail.lastEffort, lastContextTokens: tail.lastContextTokens,
        ultracode: tail.ultracode, ultracodeAt: tail.ultracodeAt, cwd: tail.cwd, lastPromptKind: tail.lastPromptKind, lastPromptAt: tail.lastPromptAt,
        queries: queries.map(q => ({ since: q.since || 0, now: q.now || now, ultracodeSince: tail.ultracodeSince(q.since || 0), background: tail.background(q.since || 0, q.now || now) })),
      } });
    });
    Date.now = realNow;
    cases.push({ name, note: n, now, steps });
  };
  const lines = (...recs) => recs.map(r => (typeof r === 'string' ? r : L(r)) + '\n').join('');

  run('titles', [
    lines({ type: 'ai-title', aiTitle: 'Fix the worklist export' }),
    lines({ type: 'custom-title', customTitle: 'worklist v2' }),
    lines({ type: 'ai-title', aiTitle: 'A newer ai title' }),
    lines({ type: 'custom-title', customTitle: '' }),
    lines({ type: 'ai-title' }),
  ], { note: 'custom-title beats ai-title; an empty one clears it' });

  run('a plain exchange', [
    lines(user('please look at the export'), asst([{ type: 'text', text: '  Looking now.  ' }])),
    lines(asst([{ type: 'thinking', thinking: 'hm' }, { type: 'text', text: 'First part.' }, toolUse('tu_1', 'Read', { file_path: 'a.js' }), { type: 'text', text: '\n  The export drops the header row.\n' }, { type: 'text', text: '   ' }],
      { timestamp: ts(5), effort: 'xhigh', message: { role: 'assistant', model: 'claude-opus-4-8', content: null, usage: { input_tokens: 10, cache_read_input_tokens: 150000 } } })),
    lines(asst([{ type: 'text', text: 'Done \u2713 \u65e5\u672c\u8a9e' }], { timestamp: ts(6), effort: '', message: { role: 'assistant', model: '', content: [{ type: 'text', text: 'Done \u2713 \u65e5\u672c\u8a9e' }], usage: null } })),
    lines(user('thanks', { timestamp: ts(7), cwd: 'D:\\elsewhere' }), user('<meta>', { isMeta: true, timestamp: ts(8) }),
      { type: 'user', timestamp: ts(9), message: { role: 'user', content: [{ type: 'tool_result', tool_use_id: 'tu_1', content: 'x' }] } }),
  ], { note: 'last text is the last non-blank text block, trimmed; model/effort/usage only move when present' });

  // a record split across polls, a multi-byte character split across polls, a blank line, junk, CRLF
  const big = lines(user('h\u00e9llo \ud83d\ude00'), asst([{ type: 'text', text: 'r\u00e9ponse \ud83d\ude00 finale' }]));
  const bb = Buffer.from(big, 'utf8');
  const cutAt = bb.indexOf(Buffer.from('\ud83d\ude00', 'utf8')) + 2;
  const cut2 = bb.lastIndexOf(Buffer.from('\ud83d\ude00', 'utf8')) + 1;
  run('split across polls', [bb.subarray(0, cutAt), bb.subarray(cutAt, cut2), bb.subarray(cut2),
    '\n\nnot json at all\n{"type":"ai-title","aiTitle":"after junk"}\r\n{"type":"custom-title"', ',"customTitle":"joined"}\n', '7\n"str"\n[1,2]\n{}\n{"type":7}\n'],
    { note: 'a line is ingested only once its newline has arrived; junk lines are skipped' });

  run('a file that shrank is read again from the top', [
    lines({ type: 'ai-title', aiTitle: 'first life' }, user('one'), asst([{ type: 'text', text: 'answer one' }])),
    lines({ type: 'ai-title', aiTitle: 'second life' }),
    lines(user('two', { timestamp: ts(20) })),
  ], { truncateBefore: [1], note: 'what was learned stays; the new content is read from offset 0' });

  run('ultracode by attachment', [
    lines(user('go'), { type: 'attachment', timestamp: ts(10), attachment: { type: 'ultra_effort_enter' } }),
    lines({ type: 'attachment', timestamp: ts(30), attachment: { type: 'ultra_effort_enter' } }),
    lines({ type: 'attachment', timestamp: ts(40), attachment: { type: 'ultra_effort_exit' } }),
    lines({ type: 'attachment', attachment: { type: 'ultra_effort_enter' } }),
    lines({ type: 'attachment', timestamp: 'not a date', attachment: { type: 'ultra_effort_enter' } }, { type: 'attachment', timestamp: ts(41), attachment: { type: 'something_else' } }, { type: 'attachment', timestamp: ts(42) }),
  ], { queries: [{ since: 0 }, { since: Date.UTC(2026, 9, 12, 8, 20, 0) }, { since: Date.UTC(2026, 9, 12, 8, 35, 0) }],
    note: 'an "on" written before the running process started says nothing about it; no timestamp = unknown time = counts' });

  const stdout = s => user('<local-command-stdout>' + s, { timestamp: ts(12) });
  run('ultracode by command output', [
    lines(stdout('Ultracode on')), lines(stdout('Ultracode off')), lines(stdout('Set effort level to ultracode (this session only)')),
    lines(stdout('Current effort level: xhigh')), lines(stdout('Set effort level to max \u00b7 Ultracode on</local-command-stdout>')),
    lines(stdout('Set effort level to high \u00b7 Ultracode off')), lines(stdout('Current effort level: max \u00b7 Ultracode on')),
    lines(stdout('Effort level: auto (currently high)')), lines(stdout('Ultracode only')), lines(stdout('Ultracode onward')),
    lines(stdout('note <b>x</b> \u00b7 Ultracode on')), lines(stdout('Set effort level to ultracodes')), lines(stdout('Set effort level to ultracode')),
    lines(stdout('Set effort level to low')), lines(user('Ultracode off', { timestamp: ts(13) })),
    lines({ type: 'user', timestamp: ts(14), message: { role: 'user', content: [{ type: 'text', text: '<local-command-stdout>Ultracode off' }] } }),
  ], { note: 'the wording read out of Claude Code 2.1.287' });

  run('background work', [
    lines(asst([toolUse('tu_b1', 'Bash', { command: 'npm test', description: '  run   the\ttests  ' })], { timestamp: ts(3) }),
      toolResult('tu_b1', { backgroundTaskId: 'bash_7' })),
    lines(asst([toolUse('tu_w', 'Workflow', { prompt: 'x'.repeat(120) })]), toolResult('tu_w', { status: 'async_launched', taskId: 'wf_1', workflowName: 'nightly-build' }),
      asst([toolUse('tu_t', 'Task', { description: 'a plain task' })]), toolResult('tu_t', { status: 'async_launched', taskId: 'task_9' }),
      asst([toolUse('tu_t2', 'Task', {})]), toolResult('tu_t2', { status: 'async_launched', taskId: 'task_10', taskType: 'local_workflow' })),
    lines(asst([toolUse('tu_a', 'Agent', { description: 'review the diff', prompt: 'long' })]), toolResult('tu_a', { status: 'async_launched', agentId: 'agent_1', description: 'reviewer' }),
      asst([toolUse('tu_a2', 'Agent', { prompt: 'second agent prompt' })]), toolResult('tu_a2', { status: 'async_launched', agentId: 'agent_2' })),
    lines(asst([toolUse('tu_m', 'Monitor', { description: 'watch CI' })]), toolResult('tu_m', { taskId: 'mon_1', timeoutMs: 600000 }, { timestamp: ts(4) }),
      asst([toolUse('tu_m2', 'Monitor', { description: 'watch forever' })]), toolResult('tu_m2', { taskId: 'mon_2', timeoutMs: '1000', persistent: true })),
    lines(asst([toolUse('tu_c', 'CronCreate', { prompt: 'check the build' })]), toolResult('tu_c', { id: 'cron_1', durable: false, recurring: true, humanSchedule: '*/5 * * * *' }),
      toolResult('tu_none', { id: 'cron_durable', durable: true }), toolResult('tu_none2', { id: 'cron_nodur' })),
    lines(asst([toolUse('tu_s', 'ScheduleWakeup', { delaySeconds: 600, reason: 'wait for the deploy' })], { timestamp: ts(5) })),
    // ends: an idle-session notification, a queue-operation, a queued_command attachment, a monitor EVENT (no status), TaskStop, CronDelete, wakeup stop
    lines({ type: 'user', timestamp: ts(20), origin: { kind: 'task-notification' }, message: { role: 'user', content: note('bash_7', 'completed') } }),
    lines({ type: 'queue-operation', operation: 'enqueue', timestamp: ts(21), content: note('wf_1', 'failed') + ' and ' + note(' task_9 ', 'killed') },
      { type: 'queue-operation', operation: 'dequeue', content: note('task_10', 'completed') }),
    lines({ type: 'attachment', timestamp: ts(22), attachment: { type: 'queued_command', prompt: [{ type: 'text', text: note('agent_1', 'completed') }] } },
      { type: 'user', timestamp: ts(22), origin: 'task-notification', message: { role: 'user', content: [{ type: 'text', text: note('mon_1', '', '<event>tick</event>\n') }] } }),
    lines(asst([toolUse('tu_sm', 'SendMessage', { to: 'agent_1', message: 'again' }), toolUse('tu_sm2', 'SendMessage', { to: 'agent_2', message: 'still running' }), toolUse('tu_sm3', 'SendMessage', { to: 'nobody' })], { timestamp: ts(23) })),
    lines(toolResult('tu_x', { task_id: 'mon_2', message: 'Task Stopped by user' }), toolResult('tu_y', { task_id: 'task_10', message: 'still going' }),
      asst([toolUse('tu_cd', 'CronDelete', { id: 'cron_1' }), toolUse('tu_s2', 'ScheduleWakeup', { stop: true })])),
    lines({ type: 'attachment', attachment: { type: 'queued_command', prompt: note('agent_1', 'completed') + note('agent_2', 'stopped') + note('task_10', 'completed') } }),
  ], { queries: [{ since: 0 }, { since: 0, now: Date.UTC(2026, 9, 12, 8, 14, 30) }, { since: Date.UTC(2026, 9, 12, 8, 3, 30) }, { since: 0, now: Date.UTC(2026, 9, 12, 8, 16, 45) }],
    note: 'every launch shape and every ending lib/transcript.js lists' });

  // the newest prompt a PERSON sent (lib/pause.js: a PAUSE pasted or typed by hand marks the tile; the next prompt ends it)
  const paste = t => `<pasted_content id="f150">\n${t}\n</pasted_content>`;
  run('the newest prompt a person sent', [
    lines(user('\n\n' + paste('PAUSE: the Claude Deck is about to restart.'), { timestamp: ts(30), origin: { kind: 'human' } })),
    lines(toolResult('tu_p', { stdout: '' }, { timestamp: ts(31) }), user('meta', { isMeta: true, timestamp: ts(32) }),
      user('<task-notification>x</task-notification>', { timestamp: ts(33), origin: { kind: 'task-notification' } }), user('  <local-command-stdout>done', { timestamp: ts(34) }),
      user('<bash-stdout>x', { timestamp: ts(34) }), user('summary', { isCompactSummary: true, timestamp: ts(35) }), user('side', { isSidechain: true, timestamp: ts(35) }),
      user('from a peer', { timestamp: ts(35), origin: { kind: 'peer' } }),
      { type: 'attachment', timestamp: ts(36), attachment: { type: 'queued_command', prompt: 'peer', isMeta: true, origin: { kind: 'peer' } } },
      { type: 'attachment', timestamp: ts(36), attachment: { type: 'queued_command', prompt: note('t1', 'completed'), origin: { kind: 'task-notification' } } },
      { type: 'attachment', timestamp: ts(36), attachment: { type: 'queued_command', prompt: 'no origin' } },
      asst([{ type: 'text', text: 'PAUSED t - f' }], { timestamp: ts(37) })),
    lines({ type: 'attachment', timestamp: ts(38), attachment: { type: 'queued_command', prompt: 'RESUME: go on', origin: { kind: 'human' }, timestamp: ts(39) } }),
    lines({ type: 'user', timestamp: ts(40), origin: { kind: 'human' }, message: { role: 'user', content: [{ type: 'text', text: '> **PAUSE:** again' }, { type: 'image' }] } }),
    lines({ type: 'user', timestamp: ts(41), message: { role: 'user', content: [{ type: 'text', text: 'x' }, { type: 'tool_result', tool_use_id: 'q' }] } }),
    lines({ type: 'attachment', attachment: { type: 'queued_command', prompt: [{ type: 'text', text: '  pause: lower case' }], origin: { kind: 'human' } } }),
    lines(user('</pasted_content>  `RESUME:` y', { timestamp: ts(42) }), user('<pasted_contentx>PAUSE: no', { timestamp: ts(43) })),
    lines(user('<pasted_content id="a"', { timestamp: ts(44) }), user('\u00a0\u3000PAUSE: wide spaces', { timestamp: 'garbage' })),
    lines(user('please PAUSE: now', { timestamp: ts(45), origin: null }), user('<command-name>/effort</command-name>', { timestamp: ts(46), origin: [] })),
  ], { note: 'a person\'s prompts only; its kind after whitespace, markdown and the paste\'s tags; no timestamp = now' });

  run('background: odd shapes are ignored', [
    lines({ type: 'user', toolUseResult: 'a string' }, { type: 'user', toolUseResult: [1] }, { type: 'user', toolUseResult: { backgroundTaskId: 0 } },
      { type: 'assistant', message: { content: [null, { type: 'tool_use' }, { type: 'tool_use', name: 'Bash', id: 'tu_q', input: null }, { type: 'tool_use', name: 'ScheduleWakeup', input: { delaySeconds: 'soon' } }] } },
      { type: 'assistant', message: 'no' }, { type: 'attachment' }, { type: 'queue-operation', operation: 'enqueue' }, { type: 'user', origin: { kind: 'task-notification' } }),
    lines(toolResult('tu_q', { backgroundTaskId: 42 }), { type: 'user', message: { content: [{ type: 'tool_result', tool_use_id: 'tu_zz' }] }, toolUseResult: { taskId: 'no_timeout' } }),
  ], { note: 'a record with no timestamp is stamped with now' });

  // a one-shot session cron: fires at a local time, so the expected `until` depends on the zone the fixtures
  // were generated in. The C++ test skips this case when its own zone offset for that moment differs.
  const cronFrom = new Date(2026, 9, 12, 8, 3, 0, 250).getTime();
  run('one-shot cron (local time)', [
    lines(asst([toolUse('tu_c1', 'CronCreate', { prompt: 'remind me' })], { timestamp: new Date(cronFrom).toISOString() }),
      toolResult('tu_c1', { id: 'cron_once', durable: false, humanSchedule: '30 9 12 10 *' }, { timestamp: new Date(cronFrom).toISOString() }),
      toolResult('tu_c2', { id: 'cron_past', durable: false, humanSchedule: '0 7 12 10 *' }, { timestamp: new Date(cronFrom).toISOString() }),
      toolResult('tu_c3', { id: 'cron_words', durable: false, humanSchedule: 'every day at 9' }, { timestamp: new Date(cronFrom).toISOString() })),
  ], { now: cronFrom + 1000, queries: [{ since: 0 }], note: 'LOCAL-TIME' });
  const cron = [['30 9 12 10 *', cronFrom], ['0 7 12 10 *', cronFrom], ['3 8 12 10 *', cronFrom + 30000], ['*/5 * * * *', cronFrom], ['1 2 3', cronFrom], ['', cronFrom], ['  5   4  3  2  1 ', cronFrom], ['5 4 3 2 x y', cronFrom]]
    .map(([expr, from]) => ({ expr, from, out: cronOnceAt(expr, from) }));
  write('transcript', { module: 'transcript', generatedFrom: 'lib/transcript.js', tzOffsetMin: new Date(cronFrom).getTimezoneOffset(), cases, cron });
  fs.rmSync(tmp, { recursive: true, force: true });
})();

// ================================================================ livestate
(() => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-fx-live-'));
  const alive = process.pid;
  const dead = spawnSync(process.execPath, ['-e', '0'], { windowsHide: true }).pid;   // a pid that has just exited
  const cases = [];
  const run = (name, accounts, note) => {
    const base = path.join(tmp, name.replace(/\W+/g, '_'));
    const accts = accounts.map(a => {
      const dir = path.join(base, a.id);
      if (a.files) { fs.mkdirSync(path.join(dir, 'sessions'), { recursive: true }); for (const [n, text] of Object.entries(a.files)) fs.writeFileSync(path.join(dir, 'sessions', n), text); }
      return { id: a.id, dir };
    });
    const out = readLive(accts);
    // the files in the order lib/sessions.js saw them
    const files = [];
    for (const a of accts) { let names = []; try { names = fs.readdirSync(path.join(a.dir, 'sessions')); } catch { continue; } for (const n of names) files.push({ account: a.id, name: n, text: fs.readFileSync(path.join(a.dir, 'sessions', n), 'utf8') }); }
    cases.push({ name, note: note || '', files, out: [...out].map(([sid, v]) => ({ sessionId: sid, ...v })) });
  };
  const J = JSON.stringify;
  run('one live session', [{ id: 'claudev', files: { [alive + '.json']: J({ pid: alive, sessionId: 's-1', status: 'busy', statusUpdatedAt: 1760000000000, name: 'my session', bridgeSessionId: 'session_abc', kind: 'interactive', cwd: 'C:\\Users\\fixture', startedAt: 1759999990000, procStart: 'Mon Oct 12' }) } }]);
  run('defaults', [{ id: 'a', files: { '1.json': J({ pid: dead, sessionId: 's-2' }), '2.json': J({ pid: alive, sessionId: 's-3', updatedAt: 5, status: '', waitingFor: 'permission prompt' }) } }], 'status idle, time 0, the rest null');
  run('not records', [{ id: 'a', files: { 'a.json': 'not json', 'b.json': J({ pid: alive }), 'c.json': J({ sessionId: 's-x' }), 'd.json': J({ pid: 0, sessionId: 's-0' }), 'e.txt': J({ pid: alive, sessionId: 's-txt' }), 'f.json': 'null', 'g.json': '[1]', 'h.json': '"s"', 'i.json': '', 'j.json': '\ufeff' + J({ pid: alive, sessionId: 's-bom' }), 'k.json': J({ pid: alive, sessionId: 's-ok', status: 'waiting', waitingFor: 'permission prompt' }) } },
    { id: 'nofolder' }]);
  run('live beats dead', [{ id: 'a', files: { '1.json': J({ pid: dead, sessionId: 's', status: 'busy', statusUpdatedAt: 900 }), '2.json': J({ pid: alive, sessionId: 's', status: 'idle', statusUpdatedAt: 100 }) } }]);
  run('live is not replaced by a newer dead one', [{ id: 'a', files: { '1.json': J({ pid: alive, sessionId: 's', status: 'idle', statusUpdatedAt: 100 }), '2.json': J({ pid: dead, sessionId: 's', status: 'busy', statusUpdatedAt: 900 }) } }]);
  run('newer wins between two dead, across accounts', [{ id: 'a', files: { '1.json': J({ pid: dead, sessionId: 's', status: 'idle', statusUpdatedAt: 100 }) } }, { id: 'b', files: { '9.json': J({ pid: dead, sessionId: 's', status: 'waiting', statusUpdatedAt: 200, name: 'from b' }) } }]);
  run('a tie keeps the first seen', [{ id: 'a', files: { '1.json': J({ pid: alive, sessionId: 's', status: 'idle', statusUpdatedAt: 100, name: 'first' }), '2.json': J({ pid: alive, sessionId: 's', status: 'busy', statusUpdatedAt: 100, name: 'second' }) } }]);
  run('several sessions keep first-seen order', [{ id: 'b', files: { '3.json': J({ pid: alive, sessionId: 'z', statusUpdatedAt: 1 }), '4.json': J({ pid: dead, sessionId: 'y', statusUpdatedAt: 2 }) } }, { id: 'a', files: { '1.json': J({ pid: alive, sessionId: 'x', statusUpdatedAt: 3 }), '2.json': J({ pid: alive, sessionId: 'y', statusUpdatedAt: 1 }) } }]);
  run('a pid that is not a number is not running', [{ id: 'a', files: { '1.json': J({ pid: String(alive), sessionId: 's', status: 'busy' }) } }]);
  write('livestate', { module: 'livestate', generatedFrom: 'lib/sessions.js', alivePids: [alive], deadPids: [dead], cases });
  fs.rmSync(tmp, { recursive: true, force: true });
})();

// ================================================================ meta (collectMeta, in a sandbox)
(() => {
  const realNow = Date.now;
  const HOME = 'C:\\Users\\fixture';
  const src = [
    constSrc('DEFAULT_SETTINGS'), constSrc('DESK_ATTENTION_MS'),
    constSrc('winEntries'), constSrc('deskEntries'), constSrc('allEntries'), constSrc('acctById'), constSrc('publicAccounts'),
    fnSrc('windowHolding'), fnSrc('cleanOsc'), fnSrc('winVisible'), fnSrc('shownBy'), fnSrc('lastActivityAt'), fnSrc('hibernateView'),
    fnSrc('backgroundOf'), fnSrc('deskAttention'), fnSrc('quietViews'), fnSrc('quietState'), fnSrc('restartView'), fnSrc('collectMeta'), fnSrc('cloudMeta'),
    fnSrc('baseEnv'), fnSrc('envFor'), fnSrc('browserSpec'), fnSrc('newTileSpec'),
    'api = { collectMeta, cleanOsc, baseEnv, envFor, newTileSpec, DEFAULT_SETTINGS };',
  ].join('\n');
  const notes = [];
  const box = {
    api: null, process: { env: {}, platform: 'win32' }, HOME, hibernate, QR, Date, Math, JSON, Object, Number, String, Array, console,
    TranscriptTail, readLive: null, findTranscript: () => null,
    tiles: [], accounts: [], settings: null, deckGroups: [], wins: new Map(), usage: null, triggers: null, held: null, asks: null, cloud: null, phone: null,
    quiet: new QR.QuietRestart(), update: null, dismissedUpdate: null,
    viewer: { show: f => ({ id: 'view' + String(f).length, name: path.win32.basename(String(f)) }) },
    isResumable: t => !!t._resumable, restoring: () => box._restoring, _restoring: false,
    maybeNotify: (t, status, turnMs) => notes.push({ id: t.id, status, turnMs }), saveState: () => {},
    cloudSpec: () => { throw new Error('cloud'); },
  };
  vm.runInNewContext(src, box);
  const A = box.api;
  const DEFAULTS = JSON.parse(JSON.stringify(A.DEFAULT_SETTINGS));
  write('default-settings', { module: 'default-settings', generatedFrom: 'main.js DEFAULT_SETTINGS (HOME = ' + HOME + ', no CLAUDE_DECK_CWD)', home: HOME, settings: DEFAULTS });

  // ---- cleanOsc
  const osc = ['', null, 'C:\\WINDOWS\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', 'powershell.EXE  ', 'c:\\Users\\x', 'D:/not/a/win/path', 'Claude Code', '\u2733 Claude Code', '  \u280b claude code  ',
    'Claude Code - fixing things', 'x Claude Code', '_ Claude Code', 'Administrator: Windows PowerShell', 'my.exe file', 'notes.exe\t', '\u2733 Fix the export', '*** Claude Code\n', 'Claude Codes']
    .map(t => ({ in: t, out: A.cleanOsc(t) }));

  // ---- the spawn environment (baseEnv / envFor) and newTileSpec
  const envCases = [
    { name: 'a parent Claude session is stripped', parent: { Path: 'C:\\Windows', CLAUDECODE: '1', CLAUDE_CODE_ENTRYPOINT: 'cli', CLAUDE_CONFIG_DIR: 'C:\\Users\\fixture\\.claude-x', claude_code_sse_port: '1', CLAUDEX: 'kept', AI_AGENT: 'x', ai_agent: 'kept-lowercase', NO_COLOR: '1', TERM_PROGRAM: 'vscode', USERPROFILE: HOME, CLAUDE_DECK_CWD: 'stripped too', MYCLAUDE_CODE: 'kept' }, acct: { id: 'claudev', dir: 'C:\\Users\\fixture\\.claude-v', isDefault: false } },
    { name: 'the default account has no CLAUDE_CONFIG_DIR', parent: { Path: 'C:\\Windows', COLORTERM: '24bit' }, acct: { id: 'claude', dir: 'C:\\Users\\fixture\\.claude', isDefault: true } },
    { name: 'shell (baseEnv)', parent: { Path: 'C:\\Windows', SystemRoot: 'C:\\Windows', NO_COLOR: '1', CLAUDE_CODE_FORCE_SESSION_PERSISTENCE: '0' }, acct: null },
  ].map(c => { box.process.env = { ...c.parent }; const out = c.acct ? A.envFor(c.acct) : A.baseEnv(); return { ...c, out }; });
  box.process.env = {};
  const specCases = [{ kind: 'shell' }, { kind: 'shell', cwd: 'D:\\x', account: 'a', browser: true }, { kind: 'claude', account: 'claudev' }, { account: 'claudev', sessionId: 's-1', cwd: 'C:\\p', browser: false },
    { kind: 'claude', account: 'a', browser: null }, { kind: 'claude', account: 'a', browser: 'yes' }, { kind: 'weird', account: 'a' }, null]
    .map(s => ({ in: s, out: JSON.parse(JSON.stringify(A.newTileSpec(s))), hasBrowser: 'browser' in A.newTileSpec(s) }));

  // ---- collectMeta
  const T0 = Date.UTC(2026, 9, 12, 9, 0, 0);
  const iso = ms => new Date(ms).toISOString();
  const L = o => JSON.stringify(o);
  const acct = (id, extra = {}) => ({ id, aliases: [id], dir: HOME + '\\.claude-' + id, isDefault: false, email: id + '@example.com', key: id + '@example.com', abbr: null, label: id + '@example.com', contextWindow: 200000, ...extra });
  const cases = [];
  // tile: the neutral shape (see core/test/session/session_test.cpp tileFrom). Fields left out take spawnTile's values.
  const mkTile = n => {
    const t = {
      id: n.id, kind: n.kind || 'shell', account: n.account ?? null, sessionId: n.sessionId ?? null, pinned: !!n.pinned, group: n.group ?? null,
      userTitle: n.userTitle ?? null, knownTitle: n.knownTitle ?? null, oscTitle: n.oscTitle ?? null,
      pty: n.running ? { cols: n.ptyCols || 120, rows: n.ptyRows || 30 } : null,
      asleep: !!n.asleep, waking: !!n.waking, sleptAt: n.sleptAt || null, startedAt: n.startedAt || 0, lastStatus: n.lastStatus ?? null,
      lastStoppedInit: !!n.lastStoppedInit, lastStoppedAt: n.lastStoppedAt || 0, busySince: n.busySince || undefined,
      browser: n.kind === 'claude' && typeof n.browser === 'boolean' ? n.browser : null, desk: n.desk ?? null, attentionAt: n.attentionAt || undefined,
      lastInputFrom: n.lastInputFrom || undefined, lastInputAt: n.lastInputAt || undefined, lastKeyAt: n.lastKeyAt || undefined, hadInput: !!n.hadInput,
      exited: !!n.exited, exitCode: n.exitCode ?? null, cwd: n.cwd || HOME, modes: { ...(n.modes || {}) },
      phoneSize: n.phoneSize || undefined, bridge: n.bridge || undefined, _resumable: !!n.resumable, _held: n.held || [], tail: null, startup: null,
    };
    if (n.transcript) { t.tail = new TranscriptTail('Z:\\no\\such\\file.jsonl'); for (const line of n.transcript) t.tail.ingest(line); }
    if (n.startup) t.startup = { done: false, since: n.startup.since, _title: n.startup.waitTitle || null, finish() { this.done = true; }, waitingOn() { return this.done || !this._title ? null : { title: this._title }; } };
    return t;
  };
  const applyPatch = (t, p) => {
    for (const [k, v] of Object.entries(p)) {
      if (k === 'running') t.pty = v ? { cols: p.ptyCols || (t.pty && t.pty.cols) || 120, rows: p.ptyRows || (t.pty && t.pty.rows) || 30 } : null;
      else if (k === 'ptyCols' || k === 'ptyRows') { if (t.pty) t.pty[k === 'ptyCols' ? 'cols' : 'rows'] = v; }
      else if (k === 'appendTranscript') { if (!t.tail) t.tail = new TranscriptTail('Z:\\no\\such\\file.jsonl'); for (const line of v) t.tail.ingest(line); }
      else if (k === 'dropTail') t.tail = null;
      else if (k === 'startupWaitTitle') { if (t.startup) t.startup._title = v; }
      else if (k === 'resumable') t._resumable = !!v;
      else if (k === 'held') t._held = v;
      else t[k] = v;
    }
  };
  const run = (name, c, note) => {
    box.settings = { ...JSON.parse(JSON.stringify(DEFAULTS)), ...(c.settings || {}) };
    box.accounts = (c.accounts || [acct('claudev'), acct('claudem', { contextWindow: 1000000, abbr: 'M', label: 'M' })]);
    box.deckGroups = c.groups || [];
    box.tiles = c.tiles.map(mkTile);
    box.held = { forTile: id => (box.tiles.find(t => t.id === id) || {})._held || [] };
    box.wins = new Map((c.windows || []).map((w, i) => ['w' + i, { win: { isDestroyed: () => false, isVisible: () => w.visible !== false, isMinimized: () => false }, index: w.index ?? i, shown: w.shown || [], layout: w.layout || {} }]));
    const watching = new Set(c.remoteWatching || []);
    box.phone = c.remoteWatching ? { running: true, watching: id => watching.has(id) } : null;
    box.quiet = new QR.QuietRestart();
    const ticks = [];
    for (const tk of c.ticks) {
      for (const [id, p] of Object.entries(tk.patch || {})) applyPatch(box.tiles.find(t => t.id === id), p);
      if (tk.reorder) box.tiles = tk.reorder.map(id => box.tiles.find(t => t.id === id));
      box._restoring = !!tk.restoring;
      const live = new Map(Object.entries(tk.live || {}).map(([sid, v]) => [sid, { account: 'claudev', status: 'idle', statusUpdatedAt: 0, alive: true, pid: 4242, name: null, bridgeSessionId: null, waitingFor: null, kind: null, cwd: null, startedAt: 0, procStart: null, ...v }]));
      box.readLive = () => live;
      Date.now = () => tk.now;
      notes.length = 0;
      const meta = JSON.parse(JSON.stringify(box.api.collectMeta()));
      Date.now = realNow;
      ticks.push({ now: tk.now, restoring: !!tk.restoring, patch: tk.patch || {}, reorder: tk.reorder || null, live: Object.fromEntries(live), meta, notify: notes.map(n => ({ ...n })),
        after: Object.fromEntries(box.tiles.map(t => [t.id, { lastStatus: t.lastStatus, busySince: t.busySince || 0, lastStoppedAt: t.lastStoppedAt || 0, lastStoppedInit: !!t.lastStoppedInit,
          waitingFor: t.waitingFor ?? null, knownTitle: t.knownTitle ?? null, bridge: t.bridge ?? null, startupDone: t.startup ? t.startup.done : null }])) });
    }
    cases.push({ name, note: note || '', settings: JSON.parse(JSON.stringify(box.settings)), accounts: box.accounts.map(a => ({ ...a })), publicAccounts: JSON.parse(JSON.stringify(vm.runInNewContext('publicAccounts()', box))),
      groups: box.deckGroups, windows: c.windows || [], remoteWatching: c.remoteWatching || null, tiles: c.tiles, ticks });
  };
  const U = (text, at) => L({ type: 'user', timestamp: iso(at), cwd: HOME, message: { role: 'user', content: text } });
  const As = (text, at, extra = {}) => L({ type: 'assistant', timestamp: iso(at), effort: 'high', message: { role: 'assistant', model: 'claude-fable-5-1', content: [{ type: 'text', text }], usage: { input_tokens: 2000, cache_creation_input_tokens: 8000, cache_read_input_tokens: 90000 } }, ...extra });

  run('one shell tile', { tiles: [{ id: 't-shell', kind: 'shell', running: true, startedAt: T0 - 5000, lastStoppedAt: T0 - 5000, cwd: 'C:\\work' }],
    windows: [{ index: 0, shown: ['t-shell'], layout: { 't-shell': [1, 2, 3, 4] } }],
    ticks: [{ now: T0 }, { now: T0 + 2000, patch: { 't-shell': { oscTitle: 'C:\\WINDOWS\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', hadInput: true, lastInputFrom: 'desktop', lastInputAt: T0 + 1500, lastKeyAt: T0 + 1500 } } },
      { now: T0 + 4000, patch: { 't-shell': { oscTitle: 'Administrator: build', modes: { 1049: true, 1002: true, 1006: true, 2004: true }, ptyCols: 97, ptyRows: 31 } } },
      { now: T0 + 6000, patch: { 't-shell': { running: false, exited: true, exitCode: 3 } } }, { now: T0 + 8000, patch: { 't-shell': { userTitle: 'my shell', pinned: true, group: 'builds' } } }] },
    'a shell is "shell" while it runs and "off" after; its window title counts unless it is only the exe path');

  run('a claude tile through a turn', { tiles: [{ id: 't-1', kind: 'claude', account: 'claudev', sessionId: 's-1', running: true, startedAt: T0 - 60000, lastStoppedAt: T0 - 60000, resumable: false, startup: { since: T0 - 60000 } }],
    windows: [{ index: 0, shown: [], layout: {} }],
    ticks: [
      { now: T0 },                                                                                           // no record yet: starting
      { now: T0 + 2000, patch: { 't-1': { startupWaitTitle: 'Quick safety check' } } },                      // sitting on a dialog: waiting
      { now: T0 + 4000, live: { 's-1': { status: 'idle', statusUpdatedAt: T0 + 3500, name: 'live name', startedAt: T0 - 59000 } } },   // up: the watch ends
      { now: T0 + 6000, live: { 's-1': { status: 'busy', statusUpdatedAt: T0 + 5000, bridgeSessionId: 'session_xyz' } }, patch: { 't-1': { appendTranscript: [L({ type: 'ai-title', aiTitle: 'Fix the export' }), U('do it', T0 + 4500)], resumable: true, hadInput: true, lastInputFrom: 'desktop', lastInputAt: T0 + 4400, lastKeyAt: T0 + 4400 } } },
      { now: T0 + 8000, live: { 's-1': { status: 'busy', statusUpdatedAt: T0 + 5000 } } },
      { now: T0 + 30000, live: { 's-1': { status: 'waiting', statusUpdatedAt: T0 + 29000, waitingFor: 'permission prompt' } }, patch: { 't-1': { appendTranscript: [As('I need to run a command.', T0 + 28000)] } } },
      { now: T0 + 40000, live: { 's-1': { status: 'busy', statusUpdatedAt: T0 + 39000 } } },
      { now: T0 + 90000, live: { 's-1': { status: 'idle', statusUpdatedAt: T0 + 89000 } }, patch: { 't-1': { appendTranscript: [As('All done.', T0 + 88000, { effort: 'xhigh' }), L({ type: 'custom-title', customTitle: 'export fix' })] } } },
      { now: T0 + 92000, live: { 's-1': { status: 'idle', statusUpdatedAt: T0 + 89000, alive: false } } },   // the record went stale: starting, and the old bridge id is remembered
      { now: T0 + 94000, patch: { 't-1': { running: false, exited: true, exitCode: 0 } } },
    ] }, 'starting -> waiting on a startup dialog -> idle -> busy -> waiting -> busy -> idle: lastStoppedAt, busySince, the toasts');

  run('ordering: pinned first in array order, then newest stop', { groups: ['ops', 'builds'], tiles: [
    { id: 'a', kind: 'shell', running: true, startedAt: T0 - 9000, lastStoppedAt: T0 - 9000 },
    { id: 'p2', kind: 'shell', running: true, pinned: true, startedAt: T0 - 8000, lastStoppedAt: T0 - 100, group: 'ops' },
    { id: 'b', kind: 'shell', running: true, startedAt: T0 - 7000, lastStoppedAt: T0 - 1000 },
    { id: 'p1', kind: 'shell', running: true, pinned: true, startedAt: T0 - 6000, lastStoppedAt: T0 - 5000 },
    { id: 'c', kind: 'shell', running: true, startedAt: T0 - 5000, lastStoppedAt: T0 - 9000 },
    { id: 'd', kind: 'shell', running: false, exited: true, exitCode: -1, startedAt: T0 - 4000, lastStoppedAt: T0 - 1000 }],
    ticks: [{ now: T0 }, { now: T0 + 2000, reorder: ['p1', 'a', 'p2', 'd', 'c', 'b'] }, { now: T0 + 4000, restoring: true }] },
    'equal stamps keep tile order (a stable sort)');

  run('asleep, waking, sleep blockers', { settings: { hibernate: { enabled: true, idleMinutes: 30, keepPinned: true, maxPerTick: 3 } },
    windows: [{ index: 0, shown: ['shown'], layout: { shown: [1] } }, { index: 2, shown: ['shown', 'idle-old'], layout: { 'idle-old': [1, 2] }, visible: false }],
    tiles: [
      { id: 'sleeping', kind: 'claude', account: 'claudev', sessionId: 's-sl', asleep: true, sleptAt: T0 - 3600000, startedAt: T0 - 7200000, lastStoppedAt: T0 - 3700000, lastStatus: 'asleep', modes: { 1049: true, 1000: true, 1006: true }, resumable: true, transcript: [L({ type: 'ai-title', aiTitle: 'sleeper' }), U('q', T0 - 3800000), As('answer before sleep', T0 - 3700000)] },
      { id: 'waking', kind: 'claude', account: 'claudev', sessionId: 's-wk', waking: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 3700000, lastStatus: 'asleep', resumable: true },
      { id: 'idle-old', kind: 'claude', account: 'claudev', sessionId: 's-old', running: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, lastStatus: 'idle', resumable: true },
      { id: 'idle-new', kind: 'claude', account: 'claudev', sessionId: 's-new', running: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, lastInputAt: T0 - 61000, lastInputFrom: 'phone', lastStatus: 'idle', resumable: true },
      { id: 'pinned', kind: 'claude', account: 'claudev', sessionId: 's-pin', running: true, pinned: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, lastStatus: 'idle', resumable: true },
      { id: 'shown', kind: 'claude', account: 'claudev', sessionId: 's-shown', running: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, lastStatus: 'idle', resumable: true },
      { id: 'held', kind: 'claude', account: 'claudev', sessionId: 's-held', running: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, lastStatus: 'idle', resumable: true, held: [{ id: 'q1', text: 'continue', at: T0 + 60000, state: 'held', note: null }] },
      { id: 'fresh', kind: 'claude', account: 'claudev', sessionId: 's-fresh', running: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7200000, lastStatus: 'idle', resumable: false },
      { id: 'bg', kind: 'claude', account: 'claudev', sessionId: 's-bg', running: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, lastStatus: 'idle', resumable: true,
        transcript: [L({ type: 'user', timestamp: iso(T0 - 7100000), message: { role: 'user', content: [{ type: 'tool_result', tool_use_id: 'x' }] }, toolUseResult: { backgroundTaskId: 'bash_1' } })] },
      { id: 'stopped', kind: 'claude', account: 'claudev', sessionId: 's-stop', running: false, exited: true, exitCode: 1, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, resumable: true },
      { id: 'nosession', kind: 'claude', account: 'claudev', sessionId: null, running: true, startedAt: T0 - 100, lastStoppedAt: T0 - 100 },
    ],
    ticks: [{ now: T0, live: { 's-old': { status: 'idle' }, 's-new': { status: 'idle' }, 's-pin': { status: 'idle' }, 's-shown': { status: 'idle' }, 's-held': { status: 'idle' }, 's-fresh': { status: 'idle' }, 's-bg': { status: 'idle' }, 's-wk': { status: 'idle', alive: false } } },
      { now: T0 + 2000, patch: { waking: { waking: false, running: true, startedAt: T0 + 1000 } }, live: { 's-old': { status: 'busy' }, 's-new': { status: 'idle' }, 's-pin': { status: 'idle' }, 's-shown': { status: 'idle' }, 's-held': { status: 'idle' }, 's-fresh': { status: 'idle' }, 's-bg': { status: 'idle' } } }] },
    'a sleeping tile reports no mouse modes; a wake is not the end of a turn; each reason a tile may not sleep');

  run('hibernation off, pressure of settings', { settings: { hibernate: { enabled: false }, driver: 'phone', fontFamily: '', zoomFontSize: 0, grid: { rows: 3, cols: 4 }, oneWindow: false, stepEscape: false, animations: false, asks: { deckThreshold: 0, picker: 'pane' },
      pages: [{ label: 'Report', file: 'C:\\pages\\report.html' }] },
    tiles: [{ id: 'x', kind: 'claude', account: 'claudem', sessionId: 's-x', running: true, startedAt: T0 - 7200000, lastStoppedAt: T0 - 7000000, lastStatus: 'idle', resumable: true, phoneSize: { cols: 48, rows: 22 }, browser: true, desk: 'web', attentionAt: T0 - 60000,
      transcript: [U('hi', T0 - 7100000), As('context heavy', T0 - 7050000, { message: { role: 'assistant', model: 'claude-opus-4-8[1m]', content: [{ type: 'text', text: 'context heavy' }], usage: { input_tokens: 5, cache_read_input_tokens: 654321 } } })] }],
    ticks: [{ now: T0, live: { 's-x': { status: 'idle', account: 'claudem' } } }, { now: T0 + 2000, patch: { x: { lastInputAt: T0 + 1000, lastInputFrom: 'deck', hadInput: true } }, live: { 's-x': { status: 'idle', account: 'claudem' } } }] },
    'the settings block; mirror under driver phone; context against a 1M window; attention until typed into');

  run('driver auto with a remote watching, and desk', { remoteWatching: ['m1'], settings: { driver: 'auto' },
    tiles: [{ id: 'm1', kind: 'shell', running: true, startedAt: T0, lastStoppedAt: T0, phoneSize: { cols: 40, rows: 20 } }, { id: 'm2', kind: 'shell', running: true, startedAt: T0, lastStoppedAt: T0, phoneSize: { cols: 41, rows: 21 } }],
    ticks: [{ now: T0 }] }, 'mirror only for the tile the remote shows; shownBy carries "remote"');
  run('driver desk never mirrors', { remoteWatching: ['m1'], settings: { driver: 'desk' },
    tiles: [{ id: 'm1', kind: 'shell', running: true, startedAt: T0, lastStoppedAt: T0, phoneSize: { cols: 40, rows: 20 } }], ticks: [{ now: T0 }] });

  run('titles, ultracode, context, lastAt', { tiles: [
    { id: 'u1', kind: 'claude', account: 'claudev', sessionId: 's-u1', running: true, startedAt: T0 - 600000, lastStoppedAt: T0 - 600000, knownTitle: 'saved title', resumable: true,
      transcript: [U('x', T0 - 500000), L({ type: 'attachment', timestamp: iso(T0 - 400000), attachment: { type: 'ultra_effort_enter' } }), As('ultra answer', T0 - 300000, { effort: 'max' })] },
    { id: 'u2', kind: 'claude', account: 'claudev', sessionId: 's-u2', running: true, startedAt: T0 - 100000, lastStoppedAt: T0 - 100000, oscTitle: '\u2733 Refactor the feed', resumable: true,
      transcript: [L({ type: 'attachment', timestamp: iso(T0 - 400000), attachment: { type: 'ultra_effort_enter' } }), L({ type: 'user', timestamp: 'garbage', message: { role: 'user', content: 'x' } })] },
    { id: 'u3', kind: 'claude', account: 'claudev', sessionId: 's-u3', running: false, exited: true, exitCode: 0, startedAt: T0 - 600000, lastStoppedAt: T0 - 600000, oscTitle: 'Claude Code',
      transcript: [L({ type: 'attachment', timestamp: iso(T0 - 400000), attachment: { type: 'ultra_effort_enter' } })] },
    { id: 'u4', kind: 'claude', account: 'claudev', sessionId: 's-u4', running: true, startedAt: T0 - 600000, lastStoppedAt: T0 - 600000 },
    { id: 'u5', kind: 'claude', account: 'nosuch', sessionId: 's-u5', running: true, startedAt: T0 - 600000, lastStoppedAt: T0 - 100, lastStoppedInit: true, bridge: 'session_old',
      transcript: [As('tiny', T0 - 50000, { message: { role: 'assistant', model: 'm', content: [{ type: 'text', text: 'tiny' }], usage: { input_tokens: 999 } } })] },
  ], ticks: [{ now: T0, live: { 's-u1': { status: 'idle', statusUpdatedAt: T0 - 1000, startedAt: T0 - 450000 }, 's-u2': { status: 'idle', statusUpdatedAt: 0, startedAt: T0 - 90000 }, 's-u4': { status: 'busy', statusUpdatedAt: T0 - 5, name: 'named by claude' } } },
    { now: T0 + 2000, live: { 's-u1': { status: 'idle', statusUpdatedAt: T0 - 1000, startedAt: T0 - 350000 }, 's-u4': { status: 'idle', statusUpdatedAt: T0 + 1000 } } }] },
    'ultracode only from an "on" the running process wrote; lastAt falls back live -> transcript -> start; NaN is null');

  write('meta', { module: 'meta', generatedFrom: 'main.js collectMeta (+ cleanOsc, baseEnv, envFor, newTileSpec), run in a sandbox', home: HOME, cleanOsc: osc, env: envCases, newTileSpec: specCases, cases });
})();
