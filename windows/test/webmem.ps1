# windows/test/webmem.ps1 — what the web views COST: ClaudeDeckNative.exe as a desk of the fake deck of
# windows/web-test (`cloud4` mode: asks waiting, four cloud tiles with a session, a local page standing in for
# claude.ai), walked through the states below; at each one the exe itself counts and weighs the msedgewebview2.exe
# processes that DESCEND from it (`webmem`, app.cpp: the process tree is walked from the exe's own pid; other
# programs' msedgewebview2.exe are never counted and nothing is ever done to any process).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/webmem.ps1
#     [-Exe <path>] [-AskGrace 3000] [-CloudGrace 5000] [-Flags "<browser arguments>"] [-Warm 1] [-Port 8813] [-Name webmem] [-Keep] [-AskPane native|web]
# -AskPane (default native, the app's default since 2026-11-23): native = the ask pane draws its cards itself
# (askcards.hpp), and state B (the ask pane open) must have NO msedgewebview2.exe at all; web = the WebView2 pane
# (one browser tree in B). The crash run below is the web pane's (its cookie on disk), whatever -AskPane says.
# The graces are the test-only `--web-grace <ask ms>,<cloud ms>` (a real run: 20 s and 2 min, asks.hpp ASK_WEB_GRACE_MS,
# cloud.hpp CLOUD_OFFSCREEN_CLOSE_MS); each "after the grace" state is measured a little past it, so the whole walk
# takes about a minute instead of four. -Flags replaces the browser arguments (to measure one flag at a time).
# -Warm 1 is what a real run does (the default here: the ask pane may warm its web view when it has a reason to).
# (State D waits 7.5 s: the first view EVER made in a new named profile takes about 5 s to come up, WEB-NOTES.md;
# a test's state folder is new every time, a person's is not.)
# Prints the table, PASS / FAIL against the target (no process at all while nothing web is on screen; one browser
# process tree while something is), what of the deck's token and cookie is on disk, and what was left behind.
# Never the live deck, never claude.ai, never a Claude session; nothing shows on screen (the window is parked).
param([string]$Exe = '', [int]$AskGrace = 3000, [int]$CloudGrace = 5000, [string]$Flags = '', [int]$Warm = 1, [int]$Port = 8813,
  [string]$Name = 'webmem', [switch]$Keep, [switch]$Quiet, [string]$AskPane = 'native')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$outDir = Join-Path $root 'windows\test\out'
New-Item -ItemType Directory -Force $outDir | Out-Null
$out = Join-Path $outDir "n-$Name.png"
$calls = Join-Path $outDir "n-$Name.calls.json"
Remove-Item $out, "$out.error.txt", "$out.log.txt", $calls -ErrorAction SilentlyContinue
$work = Join-Path $env:TEMP ("cdn-webmem-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
$state = Join-Path $work 'state'
New-Item -ItemType Directory -Force $state | Out-Null
$portFile = Join-Path $work 'fake.port'
$node = (Get-Command node -ErrorAction Stop).Source
$fakeJs = Join-Path $root 'windows\web-test\fake-deck.js'
$fixtures = Join-Path $root 'windows\web-test\fixtures'
$token = 'faketoken00000001'
$fake = Start-Process -FilePath $node -ArgumentList ('"{0}" "{1}" "{2}" "{3}" {4} cloud4' -f $fakeJs, $portFile, $calls, $fixtures, $Port) -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $work 'fake.log') -RedirectStandardError (Join-Path $work 'fake.err.log')
# after the run the exe is gone and its tree with it: what is left is found by the profile folder on its command line
function Count-WebViews([int]$id) { @(Get-CimInstance Win32_Process -Filter "Name = 'msedgewebview2.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like "*$state*" -or $_.CommandLine -like "*ClaudeDeckNative-web\$id*" }).Count }
$ag = $AskGrace + [Math]::Max(2500, [int]($AskGrace / 2)); $cg = $CloudGrace + [Math]::Max(3000, [int]($CloudGrace / 2))
$steps = @"
wait 4500; webmem A idle, asks waiting, ask pane closed;
ask open all; wait 4000; ask dump; wait 200; webmem B ask pane open on its pages; webscan deck_phone;
ask close; wait $ag; webmem C ask pane closed, past its grace; webscan deck_phone;
zone tile c1; wait 7500; webmem D one cloud pane; cloud dump;
key alt+1; keyup 1; wait 900; sel Fix the label; wait 300; key alt+2; keyup 2; wait 1500; sel Read the CSV; wait 300; key alt+3; keyup 3; wait 1500; sel Count the racks; wait 300; key alt+4; keyup 4; wait 7500;
cloud dump; wait 200; webmem E four cloud panes;
ask open all; wait 3500; webmem F ask pane open over the four cloud panes; webscan deck_phone; ask close; wait 1200;
rzone tile c1; wait 500; menu Remove from layout; wait 500; rzone tile c2; wait 500; menu Remove from layout; wait 500;
rzone tile c3; wait 500; menu Remove from layout; wait 500; rzone tile c5; wait 500; menu Remove from layout; wait 500;
zone tile t1; wait $cg; cloud dump; wait 200; webmem G cloud panes unplaced, past their grace; webscan deck_phone;
zone tile c2; wait 4000; cloud dump; wait 200; webmem H one cloud pane again (rebuilt); cloud eval c2 document.getElementById('sid').textContent; wait 300;
cloud click c2 document.getElementById('tologin'); wait 1500; cloud click c2 document.getElementById('signin'); wait 2000; cloud eval c2 document.getElementById('login').textContent; wait 300;
close
"@
$stepFile = Join-Path $work 'steps.txt'
[IO.File]::WriteAllText($stepFile, $steps)
$exePid = 0; $rows = @(); $log = @()
try {
  for ($i = 0; $i -lt 100 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
  if (-not (Test-Path $portFile)) { throw ('the fake deck did not start: ' + (Get-Content (Join-Path $work 'fake.err.log') -Raw)) }
  $env:CLAUDE_DECK_LINK = "http://127.0.0.1:$Port/?t=$token"
  $a = "--no-save --state-dir `"$state`" --after 1500 --size 1400x900 --web-warm $Warm --web-grace $AskGrace,$CloudGrace --cloud-base http://127.0.0.1:$($Port + 1) --ask-pane $AskPane --window-shot `"$out`" --do-file `"$stepFile`""
  if ($Flags) { $a += " --web-flags `"$Flags`"" }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
  $exePid = $p.Id
  "exit $($p.ExitCode) after $($sw.ElapsedMilliseconds) ms  ($exe)"
  if (Test-Path "$out.error.txt") { Get-Content "$out.error.txt" }
  if (Test-Path "$out.log.txt") { $log = @(Get-Content "$out.log.txt") }
  if (-not $Quiet) { $log | Where-Object { $_ -match 'WEBSCAN|ask timing|cloud pane (built|dropped|closed)|ask web view|web flags|web views shut|UNKNOWN|NO ZONE|NO MENU' } | ForEach-Object { "  $_" } }
} finally { try { if ($fake -and -not $fake.HasExited) { $fake.Kill() } } catch {} }
foreach ($l in $log) {
  if ($l -match 'WEBMEM (\w) (.*?): processes (\d+), working set ([\d.]+) MB, private ([\d.]+) MB') {
    $rows += [pscustomobject]@{ Key = $Matches[1]; State = $Matches[2]; Processes = [int]$Matches[3]; 'WorkingSet MB' = [double]$Matches[4]; 'Private MB' = [double]$Matches[5] }
  }
}
''
$rows | Format-Table -AutoSize | Out-String -Width 200 | ForEach-Object { $_.TrimEnd() }
$fail = 0
$zero = if ($AskPane -eq 'web') { @('A', 'C', 'G') } else { @('A', 'B', 'C', 'G') }   # native: the ask pane open is no web view at all
foreach ($k in $zero) {
  $r = $rows | Where-Object Key -eq $k
  if (-not $r) { "FAIL: state $k was not measured"; $fail++ }
  elseif ($r.Processes -ne 0) { "FAIL: $($r.Processes) msedgewebview2.exe ($($r.'WorkingSet MB') MB) while nothing web is on screen: $($r.State)"; $fail++ }
  else { "PASS: no msedgewebview2.exe at all: $($r.State)" }
}
foreach ($k in @('B', 'D', 'E', 'F', 'H') | Where-Object { $zero -notcontains $_ }) { $r = $rows | Where-Object Key -eq $k; if (-not $r -or $r.Processes -lt 1) { "FAIL: state $k has no web view at all"; $fail++ } }
# one browser process tree: the exe has ONE msedgewebview2.exe child of its own at each state with a page (the log says)
$trees = @($log | Where-Object { $_ -match 'WEBMEM (\w) .*browser processes \(our own children\) (\d+)' } | ForEach-Object { if ($_ -match 'WEBMEM (\w) .*browser processes \(our own children\) (\d+)') { [pscustomobject]@{ Key = $Matches[1]; N = [int]$Matches[2] } } })
foreach ($t in $trees) { if ($t.N -gt 1) { "FAIL: state $($t.Key) has $($t.N) browser process trees (one is the target)"; $fail++ } }
if ($trees.Count) { "browser process trees per state: " + (($trees | ForEach-Object { "$($_.Key)=$($_.N)" }) -join ' ') }
Start-Sleep -Milliseconds 800
# ---- the deck's token and its cookie: nothing of either may be on disk (the cookie's NAME is looked for because
# Chromium encrypts cookie VALUES on disk: a stored cookie would show by its name and host, not by the token)
function Find-Bytes([string]$dir, [string[]]$words) {
  $hits = @(); $n = 0; $unread = 0
  foreach ($f in @(Get-ChildItem $dir -Recurse -File -Force -ErrorAction SilentlyContinue)) {
    $n++
    try { $fs = [IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite, Delete'); try { $b = New-Object byte[] $fs.Length; [void]$fs.Read($b, 0, $b.Length) } finally { $fs.Close() } } catch { $unread++; continue }
    $t = [Text.Encoding]::GetEncoding(28591).GetString($b)
    foreach ($w in $words) {
      $w16 = (($w.ToCharArray() | ForEach-Object { "$_`0" }) -join '')
      if ($t.Contains($w) -or $t.Contains($w16)) { $hits += "$w in $($f.FullName.Substring($dir.Length))" }
    }
  }
  [pscustomobject]@{ Files = $n; Unreadable = $unread; Hits = $hits }
}
$scan = Find-Bytes $state @($token, 'deck_phone')
"on disk after the run, under the state folder ($($scan.Files) files, $($scan.Unreadable) unreadable): the token or the cookie's name found in $($scan.Hits.Count) file(s)" + $(if ($scan.Hits.Count) { '  <-- FAIL' } else { '  (nothing: PASS)' })
$scan.Hits | ForEach-Object { "  $_" }
if ($scan.Hits.Count) { $fail++ }
# the control: the walk signed in to the stand-in page in one cloud profile (a persistent cookie named fake_login).
# The same search must FIND that name in that profile's cookie store, or the search proves nothing.
$ctl = Find-Bytes $state @('fake_login')
$ctlOk = @($ctl.Hits | Where-Object { $_ -like '*claude-vv*Cookies*' }).Count -gt 0
"  control: the same search finds the cloud sign-in's cookie name 'fake_login' in $($ctl.Hits.Count) file(s)" + $(if ($ctlOk) { ' (the cookie store of that profile among them: the search does see a stored cookie)' } else { '  <-- FAIL: the search cannot see a stored cookie' })
$ctl.Hits | ForEach-Object { "    $_" }
if (-not $ctlOk) { $fail++ }
$cookieFiles = @(Get-ChildItem $state -Recurse -File -Force -Filter 'Cookies' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($state.Length) + " ($($_.Length) bytes)" })
"  cookie stores on disk: $(if ($cookieFiles.Count) { $cookieFiles -join '; ' } else { 'none' })"
# ---- 2026-11-23: every web view that stays (the ask pane's "web" fallback, the cloud panes) is on the LOW memory
# target while hidden and back on NORMAL when shown (webpane.cpp applyMemoryTarget, ICoreWebView2_19), and the ask web
# pane closes 5 s after it is hidden (was 20 s). Neither may lose anything: a re-open of the ask pane after its view
# closed shows the drafts and goes to the place asked for; a cloud pane hidden and shown again within its grace is
# the same page, still signed in. The REAL ask grace here (no --web-grace).
$fake3 = Start-Process -FilePath $node -ArgumentList ('"{0}" "{1}" "{2}" "{3}" {4} cloud4' -f $fakeJs, "$portFile.3", "$calls.3", $fixtures, $Port) -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $work 'fake3.log') -RedirectStandardError (Join-Path $work 'fake3.err.log')
try {
  for ($i = 0; $i -lt 100 -and -not (Test-Path "$portFile.3"); $i++) { Start-Sleep -Milliseconds 100 }
  $out3 = Join-Path $outDir "n-$Name-lowmem.png"; Remove-Item $out3, "$out3.log.txt" -ErrorAction SilentlyContinue
  $steps3 = @'
wait 1500; ask open all; wait 4500; ask dump; wait 200;
ask in 0 document.querySelectorAll('.card')[0].querySelector('[data-pick]'); wait 1200;
ask close; wait 800; ask dump; wait 200;
wait 6000; ask dump; wait 200;
ask open all aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee; wait 4500; ask dump; wait 300;
ask eval 0 (document.querySelector('.card [aria-checked="true"]')||{dataset:{pick:'none'}}).dataset.pick; wait 600;
ask close; wait 500;
zone tile c1; wait 7500; cloud dump;
cloud click c1 document.getElementById('tologin'); wait 1500; cloud click c1 document.getElementById('signin'); wait 2000;
cloud eval c1 (window.__kept='kept',document.getElementById('login').textContent); wait 400;
rzone tile c1; wait 500; menu Remove from layout; wait 1500; cloud dump;
zone tile c1; wait 2500; cloud dump; cloud eval c1 window.__kept+'|'+document.getElementById('login').textContent; wait 400;
close
'@
  [IO.File]::WriteAllText("$stepFile.3", $steps3)
  $a3 = "--no-save --state-dir `"$(Join-Path $work 'state3')`" --after 1500 --size 1400x900 --web-warm 0 --cloud-base http://127.0.0.1:$($Port + 1) --ask-pane web --window-shot `"$out3`" --do-file `"$stepFile.3`""
  $p3 = Start-Process -FilePath $exe -ArgumentList $a3 -Wait -PassThru
  $l3 = if (Test-Path "$out3.log.txt") { Get-Content "$out3.log.txt" -Raw -Encoding UTF8 } else { '' }
  $dumps = @([regex]::Matches($l3, 'ask web: state (\d+) \(2 = ready\) made (\d) host document ready (\d) visible (\d) memory target (-?\d)') | ForEach-Object { [pscustomobject]@{ State = [int]$_.Groups[1].Value; Made = [int]$_.Groups[2].Value; Visible = [int]$_.Groups[4].Value; Mem = [int]$_.Groups[5].Value } })
  "lowmem run: exit $($p3.ExitCode); ask dumps (made / visible / memory target): " + (($dumps | ForEach-Object { "$($_.Made)/$($_.Visible)/$($_.Mem)" }) -join '  ')
  $old = $dumps.Count -ge 1 -and $dumps[0].Mem -eq -1
  if ($old) { '  (this WebView2 runtime has no ICoreWebView2_19: the memory target is skipped quietly, as it should be)' }
  $ok1 = $dumps.Count -ge 4 -and $dumps[0].Made -eq 1 -and $dumps[0].Visible -eq 1 -and ($old -or $dumps[0].Mem -eq 0)
  $ok2 = $dumps.Count -ge 4 -and $dumps[1].Made -eq 1 -and $dumps[1].Visible -eq 0 -and ($old -or $dumps[1].Mem -eq 1)
  $ok3 = $dumps.Count -ge 4 -and $dumps[2].Made -eq 0
  $ok4 = $dumps.Count -ge 4 -and $dumps[3].Made -eq 1 -and $dumps[3].Visible -eq 1 -and ($old -or $dumps[3].Mem -eq 0)
  $drafts = $l3 -match 'ask eval "a"'
  $scrolled = $l3 -match '"scrollTop":[1-9]'
  $cl = @([regex]::Matches($l3, '(?m)^\s+c1 .*?hidden (\d) .*?memory target (-?\d)') | ForEach-Object { [pscustomobject]@{ Hidden = [int]$_.Groups[1].Value; Mem = [int]$_.Groups[2].Value } })
  $cloudOk = $cl.Count -ge 3 -and ($old -or ($cl[0].Mem -eq 0 -and $cl[1].Mem -eq 1 -and $cl[2].Mem -eq 0))   # (shown, out of the layout, back in it)
  $kept = $l3 -match 'cloud eval c1 "kept\|'
  "  ask web pane shown: NORMAL target $(if ($ok1) { 'PASS' } else { 'FAIL' }); hidden (closed, in its grace): LOW target $(if ($ok2) { 'PASS' } else { 'FAIL' }); closed 5 s after (the new grace) $(if ($ok3) { 'PASS' } else { 'FAIL' }); open again: NORMAL $(if ($ok4) { 'PASS' } else { 'FAIL' })"
  "  re-open after the view closed: the draft is back $(if ($drafts) { 'PASS' } else { 'FAIL' }), the place asked for (scrollTop > 0) $(if ($scrolled) { 'PASS' } else { 'FAIL' })"
  "  a cloud pane: shown NORMAL, hidden LOW, shown again NORMAL $(if ($cloudOk) { 'PASS' } else { 'FAIL' }) ($(($cl | ForEach-Object { "hidden $($_.Hidden) target $($_.Mem)" }) -join ', ')); the same page, still signed in $(if ($kept) { 'PASS' } else { 'FAIL' })"
  foreach ($x in $ok1, $ok2, $ok3, $ok4, $drafts, $scrolled, $cloudOk, $kept) { if (-not $x) { $fail++ } }
} finally { try { if ($fake3 -and -not $fake3.HasExited) { $fake3.Kill() } } catch {} }
# ---- the same question after a CRASH with the ask pane open: the process is ended (TerminateProcess, the `crash`
# step) with the pane up and its cookie set, so nothing was closed and nothing was cleared. Whatever the browser
# had put on disk by itself is still there to be found.
$fake2 = Start-Process -FilePath $node -ArgumentList ('"{0}" "{1}" "{2}" "{3}" {4} cloud4' -f $fakeJs, "$portFile.2", "$calls.2", $fixtures, $Port) -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $work 'fake2.log') -RedirectStandardError (Join-Path $work 'fake2.err.log')
try {
  for ($i = 0; $i -lt 100 -and -not (Test-Path "$portFile.2"); $i++) { Start-Sleep -Milliseconds 100 }
  $out2 = Join-Path $outDir "n-$Name-crash.png"; Remove-Item $out2, "$out2.log.txt" -ErrorAction SilentlyContinue
  $steps2 = "wait 1500; ask open all; wait 7500; ask dump; wait 200; ask in 0 document.querySelectorAll('.card')[0].querySelector('[data-pick]'); wait 1200; webmem X ask pane open, about to crash; crash"
  [IO.File]::WriteAllText("$stepFile.2", $steps2)
  $a2 = "--no-save --state-dir `"$state`" --after 1500 --size 1400x900 --web-warm $Warm --cloud-base http://127.0.0.1:$($Port + 1) --ask-pane web --window-shot `"$out2`" --do-file `"$stepFile.2`""
  if ($Flags) { $a2 += " --web-flags `"$Flags`"" }
  $p2 = Start-Process -FilePath $exe -ArgumentList $a2 -Wait -PassThru
  $log2 = if (Test-Path "$out2.log.txt") { @(Get-Content "$out2.log.txt") } else { @() }
  $open = @($log2 | Where-Object { $_ -match "ask web: state 2 .*host document ready 1 .*InPrivate \(the runtime's word\) 1" }).Count -gt 0
  $asked = if (Test-Path "$calls.2") { @((Get-Content "$calls.2" -Raw | ConvertFrom-Json).http | Where-Object { $_.auth -eq 'cookie' }).Count } else { 0 }
  "crash run: exit $($p2.ExitCode) (9 = ended as a crash); the pane was open, InPrivate, its pages loaded: $open; /ask/ requests let in by the cookie: $asked"
  if ($p2.ExitCode -ne 9 -or -not $open -or $asked -lt 1) { '  <-- FAIL: the crash run did not get as far as a page loaded with the cookie'; $fail++ }
  # the browser processes notice their host is gone and leave by themselves
  for ($i = 0; $i -lt 50 -and (Count-WebViews $p2.Id) -gt 0; $i++) { Start-Sleep -Milliseconds 200 }
  "  msedgewebview2.exe of the crashed run still there after $($i * 200) ms: $(Count-WebViews $p2.Id)"
  Start-Sleep -Milliseconds 500
  $scan2 = Find-Bytes $state @($token, 'deck_phone')
  "  on disk after the crash ($($scan2.Files) files, $($scan2.Unreadable) unreadable): the token or the cookie's name found in $($scan2.Hits.Count) file(s)" + $(if ($scan2.Hits.Count) { '  <-- FAIL' } else { '  (nothing: PASS)' })
  $scan2.Hits | ForEach-Object { "    $_" }
  if ($scan2.Hits.Count -or $scan2.Unreadable) { $fail++ }
  $exePid2 = $p2.Id
} finally { try { if ($fake2 -and -not $fake2.HasExited) { $fake2.Kill() } } catch {} }
$left = if ($exePid) { (Count-WebViews $exePid) } else { 0 }
$exes = @(Get-Process -Id $exePid -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq $DeckEdition.ExeBase }).Count
$prof = @(Get-ChildItem (Join-Path $env:TEMP 'ClaudeDeckNative-web') -Directory -Filter "$exePid*" -ErrorAction SilentlyContinue).Count
"left behind: ClaudeDeckNative.exe $exes, msedgewebview2.exe of this run $left, private profile folders $prof" + $(if ($exes + $left + $prof) { '  <-- FAIL' } else { '  (clean)' })
if ($exes + $left + $prof) { $fail++ }
if ($Keep) { "kept: $work" } else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue; if (Test-Path $work) { "the temp folder $work could not be removed  <-- FAIL"; $fail++ } }
"webmem: $(if ($fail) { "$fail FAILED" } else { 'all passed' })"
exit $fail
