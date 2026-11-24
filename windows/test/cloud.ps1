# windows/test/cloud.ps1 — the cloud panes IN THE APP: ClaudeDeckNative.exe as a desk of the FAKE deck of
# windows/web-test in its `cloud` mode (the real lib/phone.js; four cloud tiles, a 2 x 2 grid) with a LOCAL page
# standing in for claude.ai (the fake deck's second server, port + 1). The real claude.ai is never loaded, no
# cloud session is ever made, no browser is ever opened: a test run maps https://claude.ai onto the stand-in
# (--cloud-base, loopback only), refuses every other address and logs `OPEN <url>` where the default browser
# would have been asked (windows/src/cloud.hpp).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/cloud.ps1 -Name cloud -Script windows/test/cloud-walk.txt
#     [-Do "<steps>"] [-Size 1400x900] [-After 1500] [-Port 8786] [-Shot] [-StateDir <folder>] [-Dpi 144] [-Runtime <folder>] [-NoBase]
# The exe runs with --window-shot (the REAL window, parked off every screen: it has the web views) unless -Shot
# (the off-screen bitmap: no window, no web view: each cloud pane shows the card). -StateDir: keep the state folder
# (and so the cloud profile, <folder>\web-cloud) for a second run: a sign-in must survive. Without it the state is a
# temp folder, removed at the end. -NoBase: no stand-in page (a test run then makes no web view at all).
# Prints the log, what the stand-in was asked for, and whether anything was left behind.
param([string]$Name = 'cloud', [string]$Script = '', [string]$Do = '', [string]$Size = '1400x900', [int]$After = 1500, [int]$Port = 8786,
  [switch]$Shot, [string]$StateDir = '', [int]$Dpi = 0, [string]$Runtime = '', [switch]$NoBase, [string]$Exe = '', [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$outDir = Join-Path $root 'windows\test\out'
$out = Join-Path $outDir "n-$Name.png"
$calls = Join-Path $outDir "n-$Name.calls.json"
Remove-Item $out, "$out.error.txt", "$out.log.txt", $calls -ErrorAction SilentlyContinue
$work = Join-Path $env:TEMP ("cdn-cloud-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force $work | Out-Null
$state = if ($StateDir) { $StateDir } else { Join-Path $work 'state' }
if ($state -like "*\AppData\Roaming\claude-deck*") { throw 'a test never uses the real state folder' }
New-Item -ItemType Directory -Force $state | Out-Null
$portFile = Join-Path $work 'fake.port'
$node = (Get-Command node -ErrorAction Stop).Source
$fakeJs = Join-Path $root 'windows\web-test\fake-deck.js'
$fixtures = Join-Path $root 'windows\web-test\fixtures'
$fake = Start-Process -FilePath $node -ArgumentList ('"{0}" "{1}" "{2}" "{3}" {4} cloud' -f $fakeJs, $portFile, $calls, $fixtures, $Port) -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $work 'fake.log') -RedirectStandardError (Join-Path $work 'fake.err.log')
# only what THIS run made: a browser process whose profile folder is this run's state folder (the ask pane's is the same
# folder now, one browser process tree for both; a run with no state folder falls back to a private temp one)
function Count-WebViews([int]$id) { @(Get-CimInstance Win32_Process -Filter "Name = 'msedgewebview2.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like "*$state\web-cloud*" -or $_.CommandLine -like "*ClaudeDeckNative-web\$id*" }).Count }
$exePid = 0
try {
  for ($i = 0; $i -lt 100 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
  if (-not (Test-Path $portFile)) { throw ('the fake deck did not start: ' + (Get-Content (Join-Path $work 'fake.err.log') -Raw)) }
  $env:CLAUDE_DECK_LINK = "http://127.0.0.1:$Port/?t=faketoken00000001"
  $a = "--no-save --state-dir `"$state`" --after $After --size $Size --web-warm 0"
  if (-not $NoBase) { $a += " --cloud-base http://127.0.0.1:$($Port + 1)" }
  if ($Shot) { $a += " --verify-scene --shot `"$out`"" } else { $a += " --window-shot `"$out`"" }
  if ($Dpi) { $a += " --dpi $Dpi" }
  if ($Runtime) { $a += " --web-runtime `"$Runtime`"" }
  if ($Extra) { $a += " $Extra" }
  if ($Script) { $a += " --do-file `"$((Resolve-Path $Script).Path)`"" } elseif ($Do) { $a += " --do `"$($Do -replace '"', '\"')`"" }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
  $exePid = $p.Id
  "exit $($p.ExitCode) after $($sw.ElapsedMilliseconds) ms  $out"
  if (Test-Path "$out.error.txt") { Get-Content "$out.error.txt" }
  if (Test-Path "$out.log.txt") { Get-Content "$out.log.txt" }
  Start-Sleep -Milliseconds 400
  if (Test-Path $calls) {
    $rec = Get-Content $calls -Raw | ConvertFrom-Json
    $n = @{}; foreach ($c in $rec.calls) { $n[$c.fn] = 1 + [int]$n[$c.fn] }
    'the fake deck received: ' + (($n.Keys | Sort-Object | ForEach-Object { "$_ x$($n[$_])" }) -join ', ') + "; sockets $($rec.sockets)"
    $h = @($rec.cloudHttp)
    "the stand-in for claude.ai was asked $($h.Count) times; by Edge's engine: $(@($h | Where-Object { $_.ua -eq 'edge' }).Count); with the sign-in cookie: $(@($h | Where-Object { $_.signed }).Count)"
    foreach ($r in $h) { "  GET $($r.url)$(if ($r.signed) { '  (signed in)' })" }
  }
} finally { try { if ($fake -and -not $fake.HasExited) { $fake.Kill() } } catch {} }
Start-Sleep -Milliseconds 800
$left = if ($exePid) { Count-WebViews $exePid } else { 0 }
$exes = @(Get-Process -Id $exePid -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq $DeckEdition.ExeBase }).Count
$prof = @(Get-ChildItem (Join-Path $env:TEMP 'ClaudeDeckNative-web') -Directory -Filter "$exePid*" -ErrorAction SilentlyContinue).Count
$cloudDir = Join-Path $state 'web-cloud'
"the cloud profile folder ${cloudDir}: $(if (Test-Path $cloudDir) { 'there (it holds the sign-ins), ' + @(Get-ChildItem $cloudDir -Recurse -File -ErrorAction SilentlyContinue).Count + ' files' } else { 'not made' })"
"left behind: ClaudeDeckNative.exe $exes, msedgewebview2.exe of this run $left, private profile folders $prof" + $(if ($exes + $left + $prof) { '  <-- FAIL' } else { '  (clean)' })
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if (Test-Path $work) { "the temp folder $work could not be removed  <-- FAIL" }
