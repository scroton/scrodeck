# windows/host-test/serve.ps1 — the native deck's SERVER, for real: sockets on 127.0.0.1, real pseudoconsoles, the
# real phone page and browser desk in a headless Chromium, and a second ClaudeDeckNative.exe driving it as a desk.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/host-test/serve.ps1 [-Skip browser,native,app] [-Keep] [-Exe <path>]
#   -Exe: the ClaudeDeckNative.exe parts 4 and 5 run (default windows\build\ClaudeDeckNative.exe) — a privately
#   linked one when the app's own tree is mid-change.
# What it runs, in order (each prints PASS / FAIL lines):
#   1. deck-serve-host.exe (serve_host.cpp: the LocalDeck with no window) on 127.0.0.1:8811, state in a temp folder
#   2. serve-client.js   a Node phone and desk: pages, token + cookie, hello, a shell tile, typing, the one-desk rule,
#                        a slow client and the 4 MB skip rule, the heartbeat dropping a silent socket
#   3. serve-browser.js  the real phone page and the real browser desk, loaded from it          -> out/serve-*.png
#   4. ClaudeDeckNative.exe in REMOTE mode (CLAUDE_DECK_LINK in the environment, --shot) as a desk: sidebar, pane,
#      typing                                                                                    -> out/serve-native-desk.png
#   5. ClaudeDeckNative.exe --local --serve 8812: the app itself serving (the switch windows/src/localtransport.hpp
#      reads), asked for its hello and a tile by a Node phone                                    -> out/serve-app-local.png
# Rules it keeps: loopback only, ports 8811-8819 only; the token is read from the temp state's settings.json and
# handed on in the environment, never printed or put on a command line; nothing appears on screen (the host is a
# console exe started in this hidden shell's own console, the app runs --shot); Claude is never started. At the
# end: nothing it started is still running and nothing listens on the ports.
param([string[]]$Skip = @(), [switch]$Keep, [string]$Exe = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$out = Join-Path $PSScriptRoot 'out'; New-Item -ItemType Directory -Force $out | Out-Null
$fail = 0

# ---- build: the host test's objects (everything but its main), then serve_host.cpp
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') -NoRun | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'host-test build failed' }
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) { $gpp = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' | ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1) }
$obj = Join-Path $env:TEMP 'deck-host-test-obj'
$hostExe = Join-Path $env:TEMP 'deck-serve-host.exe'
$o = Join-Path $obj 'windows_host-test_serve_host.o'
& $gpp -std=c++17 -O1 -Wall -Wno-unknown-pragmas -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -c (Join-Path $PSScriptRoot 'serve_host.cpp') -o $o
if ($LASTEXITCODE -ne 0) { throw 'compile failed: serve_host.cpp' }
$objects = @(Get-ChildItem $obj -Filter *.o | Where-Object { $_.Name -notmatch 'host_test\.o$' -and $_.Name -notmatch '^core_vt_' } | ForEach-Object { $_.FullName })
& $gpp -static -static-libgcc -static-libstdc++ @objects -o $hostExe -lshlwapi -lole32 -luser32 -lws2_32 -liphlpapi -lwinhttp
if ($LASTEXITCODE -ne 0) { throw 'link failed: deck-serve-host.exe' }

function Wait-File([string]$path, [int]$ms) { $until = (Get-Date).AddMilliseconds($ms); while (-not (Test-Path $path)) { if ((Get-Date) -gt $until) { return $false }; Start-Sleep -Milliseconds 100 }; return $true }
function Listening([int]$port) { return @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue).Count -gt 0 }
foreach ($p in 8811, 8812) { if (Listening $p) { throw "port $p is already in use: not starting" } }

$work = Join-Path $env:TEMP ('cdn-serve-' + [guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force $work | Out-Null
$stop = Join-Path $work 'stop'
$started = Get-Date
$serverProc = Start-Process -FilePath $hostExe -ArgumentList "--state-dir `"$work`" --port 8811 --assets `"$root`" --stop-file `"$stop`" --seconds 600" -NoNewWindow -PassThru `
  -RedirectStandardOutput (Join-Path $work 'host.out.txt') -RedirectStandardError (Join-Path $work 'host.err.txt')
try {
  if (-not (Wait-File (Join-Path $work 'serve-host.ready') 15000)) { throw 'the host did not come up' }
  $ready = Get-Content (Join-Path $work 'serve-host.ready') -Raw | ConvertFrom-Json
  if (-not $ready.running) { throw "the host is not serving: $($ready.error)" }
  "host: pid $($ready.pid) serving on $($ready.address):$($ready.port)"
  if ($ready.assets -eq 'ok' -and $ready.assetRoot -eq $root) { 'PASS  status: the pages are where the server looks for them (assets ok + the root)' } else { "FAIL  status: assets '$($ready.assets)' root '$($ready.assetRoot)'"; $fail++ }
  $bound = @(Get-NetTCPConnection -State Listen -LocalPort 8811 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty LocalAddress -Unique)
  if ($bound.Count -eq 1 -and $bound[0] -eq '127.0.0.1') { 'PASS  bind: 8811 listens on 127.0.0.1 and on nothing else' } else { "FAIL  bind: 8811 listens on $($bound -join ', ')"; $fail++ }
  $token = (Get-Content (Join-Path $work 'local\settings.json') -Raw | ConvertFrom-Json).phone.token
  if (-not $token -or $token.Length -ne 16) { throw 'no token in the temp settings.json' }
  $env:DECK_PORT = '8811'; $env:DECK_TOKEN = $token; $env:DECK_WORK = $work; $env:DECK_OUT = $out

  '--- 2. a Node phone and desk (serve-client.js)'
  & node (Join-Path $PSScriptRoot 'serve-client.js'); if ($LASTEXITCODE -ne 0) { $fail++ }

  if ($Skip -notcontains 'browser') {
    '--- 3. the real phone page and the real browser desk, headless (serve-browser.js)'
    & node (Join-Path $PSScriptRoot 'serve-browser.js'); if ($LASTEXITCODE -ne 0) { $fail++ }
  }

  if ($Skip -notcontains 'native') {
    '--- 4. a second ClaudeDeckNative.exe, remote mode, as a desk'
    $exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
    $shot = Join-Path $out 'serve-native-desk.png'
    Remove-Item $shot, "$shot.log.txt", "$shot.error.txt" -ErrorAction SilentlyContinue
    $env:CLAUDE_DECK_LINK = "http://127.0.0.1:8811/?t=$token"
    $do = "wait 1200; sel native serve one; wait 1500; key enter; wait 300; text echo native-desk-typed; key enter; wait 1800; dump tiles; dump screen"
    $p = Start-Process -FilePath $exe -ArgumentList "--no-save --shot `"$shot`" --after 1500 --size 1400x900 --desk-name NATIVE-TEST --do `"$do`"" -Wait -PassThru
    $env:CLAUDE_DECK_LINK = $null
    $log = if (Test-Path "$shot.log.txt") { Get-Content "$shot.log.txt" -Raw } else { '' }
    if ($p.ExitCode -eq 0 -and (Test-Path $shot) -and (Get-Item $shot).Length -gt 5000) { "PASS  native desk: ran against the native deck and saved a frame ($shot)" } else { "FAIL  native desk: exit $($p.ExitCode)"; $fail++ }
    if ($log -match 'native serve one') { 'PASS  native desk: its sidebar lists the tile' } else { 'FAIL  native desk: the tile is not in its log'; $fail++ }
    if ($log -match 'native-desk-typed') { 'PASS  native desk: what it typed ran in the shell, and its pane shows the output' } else { 'FAIL  native desk: the typed line is not on its screen'; $fail++; ($log -split "`n" | Select-Object -Last 25) -join "`n" }
    if (Test-Path "$shot.error.txt") { Get-Content "$shot.error.txt" }
  }
}
finally {
  Set-Content -Path $stop -Value 'stop'
  if (-not $serverProc.WaitForExit(20000)) { 'FAIL  the host did not stop by itself'; $fail++; Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue }
  $env:DECK_TOKEN = $null
}
$done = if (Test-Path (Join-Path $work 'serve-host.done')) { Get-Content (Join-Path $work 'serve-host.done') -Raw | ConvertFrom-Json } else { $null }
if ($done -and $done.leftover -eq 0 -and $done.processesStarted -ge 1) { "PASS  the host: every process it started ($($done.processesStarted)) is gone" } else { "FAIL  the host: leftover processes ($($done | ConvertTo-Json -Compress))"; $fail++ }
Start-Sleep -Milliseconds 300
if (Listening 8811) { 'FAIL  8811 is still listening'; $fail++ } else { 'PASS  8811 is free again' }

if ($Skip -notcontains 'app') {
  '--- 5. ClaudeDeckNative.exe --local --serve 8812 (the app itself serving)'
  $exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
  $work2 = Join-Path $env:TEMP ('cdn-serve-app-' + [guid]::NewGuid().ToString('N').Substring(0, 10)); New-Item -ItemType Directory -Force $work2 | Out-Null
  $shot = Join-Path $out 'serve-app-local.png'
  Remove-Item $shot, "$shot.log.txt", "$shot.error.txt" -ErrorAction SilentlyContinue
  $env:CLAUDE_DECK_LINK = $null
  $app = Start-Process -FilePath $exe -ArgumentList "--local --serve 8812 --assets `"$root`" --state-dir `"$work2`" --shell cmd.exe --shot `"$shot`" --after 1000 --size 1400x900 --do `"wait 14000; dump tiles`"" -PassThru
  $settings = Join-Path $work2 'local\settings.json'
  if ((Wait-File $settings 10000)) {
    Start-Sleep -Milliseconds 800
    $env:DECK_PORT = '8812'; $env:DECK_TOKEN = (Get-Content $settings -Raw | ConvertFrom-Json).phone.token; $env:DECK_WORK = $work2
    & node (Join-Path $PSScriptRoot 'serve-app.js'); if ($LASTEXITCODE -ne 0) { $fail++ }
    $env:DECK_TOKEN = $null
  } else { 'FAIL  the app did not write its settings'; $fail++ }
  if (-not $app.WaitForExit(40000)) { 'FAIL  the app did not exit'; $fail++; Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
  $log = if (Test-Path "$shot.log.txt") { Get-Content "$shot.log.txt" -Raw } else { '' }
  if ($log -match 'made by a phone') { "PASS  app --serve: the tile the phone made is in the app's own window ($shot)" } else { 'FAIL  app --serve: the phone''s tile is not in the app''s log'; $fail++ }
  Start-Sleep -Milliseconds 500
  if (Listening 8812) { 'FAIL  8812 is still listening'; $fail++ } else { 'PASS  8812 is free again' }
  $left = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($app.Id)" | Where-Object { $_.Name -match '^(cmd|powershell|pwsh|conhost|OpenConsole)\.exe$' -and $_.CreationDate -ge $started })
  if ($left.Count) { "FAIL  app --serve: leftover children $(($left | ForEach-Object { $_.Name }) -join ' ')"; $fail++; $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } } else { 'PASS  app --serve: no leftover children' }
  if (-not $Keep) { Remove-Item -Recurse -Force $work2 -ErrorAction SilentlyContinue }
}
if (-not $Keep) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue } else { "state kept: $work" }
if ($fail) { "`nSERVE TESTS: $fail part(s) failed"; exit 1 } else { "`nSERVE TESTS: all parts passed"; exit 0 }
