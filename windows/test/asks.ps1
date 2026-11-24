# windows/test/asks.ps1 — the ask pane IN THE APP: ClaudeDeckNative.exe as a desk of the FAKE deck of
# windows/web-test (fake-deck.js: the real lib/phone.js PhoneServer with fake deps, loopback, a made-up token, two
# sample ask pages). Never the live deck, never the scratch deck, never a Claude session.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/asks.ps1 -Name asks -Script windows/test/asks-walk.txt
#     [-Do "<steps>"] [-Size 1400x900] [-After 1500] [-Warm 0|1] [-Port 8796] [-Shot] [-NoAsks] [-Scratch] [-Dpi 144]
#     [-Mode legacy]  the fake deck also has a page from before the host bridge, and a page that replaces an older one
# -Scratch: no fake deck; the exe is a desk of the SCRATCH Electron deck (127.0.0.1:8799, windows/test/scratch-deck.ps1),
# for one end-to-end run with a real main.js / lib/asks.js behind the pane. Its asks are whatever its ASK_PAGE_STATE_DIR holds.
# The exe runs with --window-shot (the REAL window, parked off every screen: it has the web view) unless -Shot
# (the off-screen bitmap: no window, so no web view; the pane draws its bar alone). --test-asks lets the pane's
# Done reach this deck (a test run refuses askDone otherwise); -NoAsks leaves that refusal on.
# Writes windows/test/out/n-<Name>.png, the script's own `shot` pictures beside it, n-<Name>.png.log.txt, and
# n-<Name>.calls.json (every call and every /ask/ request the fake deck received). Prints the log, the ask calls
# the deck received, and whether anything was left behind (the exe, a msedgewebview2.exe of its profile, the
# profile folder). Nothing appears on screen: the window is parked, node runs hidden.
param([string]$Name = 'asks', [string]$Script = '', [string]$Do = '', [string]$Size = '1400x900', [int]$After = 1500, [int]$Warm = 0,
  [int]$Port = 8796, [switch]$Shot, [switch]$NoAsks, [string]$Exe = '', [switch]$Scratch, [string]$Runtime = '', [string]$Mode = '', [int]$Dpi = 0, [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$outDir = Join-Path $root 'windows\test\out'
$out = Join-Path $outDir "n-$Name.png"
$calls = Join-Path $outDir "n-$Name.calls.json"
Remove-Item $out, "$out.error.txt", "$out.log.txt", $calls -ErrorAction SilentlyContinue
$work = Join-Path $env:TEMP ("cdn-asks-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force $work | Out-Null
$portFile = Join-Path $work 'fake.port'
$node = (Get-Command node -ErrorAction Stop).Source
$fakeJs = Join-Path $root 'windows\web-test\fake-deck.js'
$fixtures = Join-Path $root 'windows\web-test\fixtures'
$fake = $null
if (-not $Scratch) { $fake = Start-Process -FilePath $node -ArgumentList ('"{0}" "{1}" "{2}" "{3}" {4} {5}' -f $fakeJs, $portFile, $calls, $fixtures, $Port, $Mode) -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $work 'fake.log') -RedirectStandardError (Join-Path $work 'fake.err.log') }
# only what THIS run's exe made (its web folder is this run's state folder, <work>\web-cloud; a run without one uses
# a temp folder named by its pid): another ClaudeDeckNative.exe may be in use on this box
function Count-WebViews([int]$id) { @(Get-CimInstance Win32_Process -Filter "Name = 'msedgewebview2.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like "*$work*" -or $_.CommandLine -like "*ClaudeDeckNative-web\$id*" }).Count }
$exePid = 0
try {
  for ($i = 0; -not $Scratch -and $i -lt 100 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
  if (-not $Scratch -and -not (Test-Path $portFile)) { throw ('the fake deck did not start: ' + (Get-Content (Join-Path $work 'fake.err.log') -Raw)) }
  $env:CLAUDE_DECK_LINK = if ($Scratch) { 'http://127.0.0.1:8799/?t=nativetest00000001' } else { "http://127.0.0.1:$Port/?t=faketoken00000001" }
  $a = "--no-save --state-dir `"$work`" --after $After --size $Size --web-warm $Warm"
  if ($Shot) { $a += " --shot `"$out`"" } else { $a += " --window-shot `"$out`"" }
  if (-not $NoAsks) { $a += ' --test-asks' }
  if ($Dpi) { $a += " --dpi $Dpi" }                    # the parked window drawn, and its page rendered, as at this scale (144 = 150 %)
  if ($Extra) { $a += " $Extra" }                     # e.g. --web-grace 2000,3000 (how long an unseen web view lives, shortened for a test)
  if ($Runtime) { $a += " --web-runtime `"$Runtime`"" }   # a folder that is not a runtime: what a machine without WebView2 shows
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
    foreach ($c in $rec.calls) { if ($c.fn -in 'askDone', 'askClear', '__read') { $j = $c.args | ConvertTo-Json -Compress -Depth 8; "  DECK GOT $($c.fn) " + $j.Substring(0, [Math]::Min(700, $j.Length)) } }
    $last = @{}; foreach ($c in $rec.calls) { if ($c.fn -eq 'askDraft') { $last[[string]$c.args[0]] = $c.args[1] } }
    foreach ($k in $last.Keys) { $j = $last[$k] | ConvertTo-Json -Compress -Depth 6; "  DECK GOT askDraft (the last for $k) " + $j.Substring(0, [Math]::Min(400, $j.Length)) }
    $h = @($rec.http)
    "  /ask/ requests: $($h.Count); let in by the cookie: $(@($h | Where-Object { $_.auth -eq 'cookie' }).Count); with a token in the URL: $(@($h | Where-Object { $_.url -match '[?&]t=' }).Count)"
  }
} finally { try { if ($fake -and -not $fake.HasExited) { $fake.Kill() } } catch {} }
Start-Sleep -Milliseconds 600
$left = if ($exePid) { Count-WebViews $exePid } else { 0 }
$exes = @(Get-Process -Id $exePid -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq $DeckEdition.ExeBase }).Count
$prof = @(Get-ChildItem (Join-Path $env:TEMP 'ClaudeDeckNative-web') -Directory -Filter "$exePid*" -ErrorAction SilentlyContinue).Count
"left behind: ClaudeDeckNative.exe $exes, msedgewebview2.exe of its profile $left, profile folders $prof" + $(if ($exes + $left + $prof) { '  <-- FAIL' } else { '  (clean)' })
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
