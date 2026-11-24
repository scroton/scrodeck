# windows/test/cloud-dialog-truth.ps1 — the "New cloud session" dialog as the REAL renderer draws it: renderer/style.css
# and renderer/js/48-cloud.js openCloudDialog in headless Edge (hidden, its own scratch profile, no deck, no network),
# with the texts windows/test/cloud-dialog.txt types. Writes windows/test/out/truth-cdlg.png (1400 x 900) to hold
# beside n-cdlg-g-refused.png.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/cloud-dialog-truth.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = Join-Path $root 'windows\test\out'
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { 'msedge.exe not found'; exit 1 }
$css = 'file:///' + ((Join-Path $root 'renderer\style.css') -replace '\\', '/')
$js = 'file:///' + ((Join-Path $root 'renderer\js\48-cloud.js') -replace '\\', '/')
$page = Join-Path $out 'truth-cdlg.html'
$html = @"
<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="$css"></head><body>
<script>
var state = { tiles: [], accounts: [], settings: { cwd: 'C:\\work\\repo' } }, terms = new Map();
var deck = { newTile: function () { return Promise.reject(new Error("Error invoking remote method 'tile:new': Error: no such folder: C:\\work\\other repo")); } };
function acct() { return null; } function acctLabel(id) { return id; } function isVisible() { return false; } function activate() {} function reportToActive() {} function createTerm() {} function placeNew() {}
</script>
<script src="$js"></script>
<script>
openCloudDialog({ id: 'vv', label: 'vv', email: 'vv@example.com' });
var ta = document.querySelector('.cd-task'), fo = document.querySelector('.cd-folder input');
ta.value = 'Port the worklist page to web.\nKeep the sheet running until the switch.\nline 3 (edited)\nline 4\nline 5\nline 6\nline 7\nline 8\nline 9 is the last';
fo.value = 'C:\\work\\other repo';
document.querySelector('.cloud-dlg button.primary').click();
setTimeout(function () { ta.scrollTop = 0; document.activeElement && document.activeElement.blur(); var b = document.querySelector('.cloud-dlg').getBoundingClientRect(); document.title = JSON.stringify([...document.querySelectorAll('.cloud-dlg, .cd-title, .cd-task, .cd-folder input, .cd-note, .cloud-dlg button, .cd-keys, .cd-err')].map(function (e) { var r = e.getBoundingClientRect(); return [e.className || e.tagName, Math.round(r.left * 10) / 10, Math.round(r.top * 10) / 10, Math.round(r.width * 10) / 10, Math.round(r.height * 10) / 10]; })); }, 300);
</script></body></html>
"@
[IO.File]::WriteAllText($page, $html, (New-Object System.Text.UTF8Encoding($false)))
$profileDir = Join-Path $env:TEMP ('cdn-truth-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$png = Join-Path $out 'truth-cdlg.png'; Remove-Item $png -ErrorAction SilentlyContinue
$url = 'file:///' + ($page -replace '\\', '/')
$ea = @('--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--force-device-scale-factor=1',
  "--user-data-dir=`"$profileDir`"", '--window-size=1400,900', '--virtual-time-budget=3000', "--screenshot=`"$png`"", "`"$url`"")
$p = Start-Process -FilePath $edge -ArgumentList $ea -Wait -PassThru -WindowStyle Hidden
# the boxes, as the page measured them (a second headless run that prints the DOM's title)
$dump = Join-Path $out 'truth-cdlg.dom.txt'
$ea2 = @('--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--force-device-scale-factor=1', "--user-data-dir=`"$profileDir`"", '--window-size=1400,900', '--virtual-time-budget=3000', '--dump-dom', "`"$url`"")
$p2 = Start-Process -FilePath $edge -ArgumentList $ea2 -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $dump
Start-Sleep -Milliseconds 500
Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
if (Test-Path $png) { "truth: $png" } else { "no screenshot (exit $($p.ExitCode))" }
$m = [regex]::Match((Get-Content $dump -Raw), '<title>(.*?)</title>')
if ($m.Success) { ($m.Groups[1].Value -replace '&quot;', '"') -replace '\],\[', "]`n[" }
