// core/test/vt/oracle/capture.js — record REAL pty byte streams as fixtures (Windows, ConPTY).
// Uses the repo's node-pty (pass its folder, or run from a checkout that has node_modules/node-pty):
//   node core/test/vt/oracle/capture.js [path-to-node-pty]
// Nothing visible is opened: ConPTY children have no window of their own. Then: node gen.js
'use strict';
const fs = require('fs');
const path = require('path');
const FIX = path.resolve(__dirname, '..', '..', 'fixtures', 'vt');
const ptyPath = process.argv[2] || path.resolve(__dirname, '..', '..', '..', '..', 'node_modules', 'node-pty');
const pty = require(ptyPath);

// file, args, size, optional keystrokes ([delayMs, text]...) and resizes ([delayMs, cols, rows]...)
const JOBS = [
  { name: 'real-powershell-dir-ps', file: 'powershell.exe', cols: 120, rows: 30,
    args: ['-NoProfile', '-Command', 'dir C:\\Windows; Get-Process | Select -First 30; Write-Host -ForegroundColor Green ok'] },
  { name: 'real-cmd-dir', file: 'cmd.exe', cols: 120, rows: 30, args: ['/c', 'dir', 'C:\\Windows'] },
  { name: 'real-cmd-dir-narrow', file: 'cmd.exe', cols: 60, rows: 12, args: ['/c', 'dir C:\\Windows\\System32\\drivers\\etc & echo done & ver'] },
  { name: 'real-powershell-colors', file: 'powershell.exe', cols: 100, rows: 25,
    args: ['-NoProfile', '-Command', "0..15 | % { Write-Host -NoNewline -ForegroundColor $_ -BackgroundColor (15-$_) (' c{0:d2} ' -f $_) }; ''; Write-Host ('=' * 250); $e=[char]27; \"$e[38;2;255;128;0mtruecolor$e[0m $e[4:3mcurly$e[0m $e[1;3;9mstyles$e[0m\"; Write-Host ([char]0x4E2D + [string][char]0x6587 + ' wide ' + [char]0x250C + [char]0x2500 + [char]0x2510); Write-Error 'an error on purpose'; cls; 'after cls'; Get-ChildItem C:\\Windows\\System32\\drivers\\etc | Format-Table Name, Length"] },
  { name: 'real-powershell-interactive', file: 'powershell.exe', cols: 120, rows: 30, args: ['-NoProfile', '-NoLogo'],
    keys: [[2500, 'Get-ChildItem C:\\Windows\\System32\\drivers\\etc'], [600, '\r'], [1500, 'echo "typed then erased"'], [300, '\x7f\x7f\x7f\x7f\x7f\x7f\x7f\x7f'], [300, 'kept"\r'], [1200, "Write-Host -ForegroundColor Cyan ('x' * 300)\r"], [1200, 'cls\r'], [900, '1..40\r'], [1500, 'exit\r']] },
  { name: 'real-powershell-resized', file: 'powershell.exe', cols: 120, rows: 30, args: ['-NoProfile', '-NoLogo'],
    keys: [[2500, "'line of text that is fairly long, long enough to wrap when the window gets narrow ' * 3\r"], [1500, '1..25\r'], [4000, "'after the resizes'\r"], [1500, 'exit\r']],
    resizes: [[5200, 70, 20], [6400, 140, 36]] },
];

function run(job) {
  return new Promise(resolve => {
    const chunks = [];
    let total = 0;
    const ops = [];
    let fedAtLastOp = 0;
    const p = pty.spawn(job.file, job.args, { name: 'xterm-256color', cols: job.cols, rows: job.rows, cwd: 'C:\\Windows', env: process.env, useConpty: true, encoding: null });
    p.onData(d => { const b = Buffer.isBuffer(d) ? d : Buffer.from(d, 'utf8'); chunks.push(b); total += b.length; });
    let t = 0;
    const timers = [];
    for (const [delay, text] of job.keys || []) { t += delay; timers.push(setTimeout(() => { try { p.write(text); } catch (e) { /* gone */ } }, t)); }
    for (const [at, c, r] of job.resizes || []) {
      timers.push(setTimeout(() => {
        try { p.resize(c, r); } catch (e) { return; }
        ops.push({ feed: total - fedAtLastOp }, { resize: [c, r] });
        fedAtLastOp = total;
      }, at));
    }
    let done = false;
    const finish = why => {
      if (done) return;
      done = true;
      timers.forEach(clearTimeout);
      // p.kill() forks node-pty's console-list helper; only do that when the child is really still running
      if (why === 'timeout') { try { p.kill(); } catch (e) { /* already gone */ } }
      const side = { cols: job.cols, rows: job.rows };
      if (ops.length) side.ops = ops;
      fs.writeFileSync(path.join(FIX, job.name + '.bin'), Buffer.concat(chunks));
      fs.writeFileSync(path.join(FIX, job.name + '.json'), JSON.stringify(side) + '\n');
      console.log(job.name + ': ' + total + ' bytes in ' + chunks.length + ' chunks (' + why + ')');
      resolve();
    };
    p.onExit(() => setTimeout(() => finish('exit'), 300));
    setTimeout(() => finish('timeout'), 30000);
  });
}

(async () => {
  const only = process.argv.slice(3);
  for (const job of JOBS) if (!only.length || only.includes(job.name)) await run(job);
  process.exit(0);
})();
