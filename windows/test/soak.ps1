# windows/test/soak.ps1 — the REMOTE exe's memory over time: ClaudeDeckNative.exe as a desk of the SOAK deck
# (windows/test/soak-deck.js: the real lib/phone.js server, twelve tiles in a 3 x 4 grid streaming like busy Claude
# Code sessions, loopback, a made-up token; never the live deck, never a Claude session).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/soak.ps1 [-Minutes 30] [-Every 60] [-Rate 12]
#     [-Tiles 12] [-Socket auto|raw|winhttp] [-Size 1920x1009] [-Name soak] [-Exe <path>] [-MaxPrivate 0] [-MaxRuns 0]
# The exe runs with --window-shot (its real window, parked off every screen, shown without activation: nothing to
# see, the focus is never taken) and --hang so the test watchdog lets it run the whole soak. Every -Every seconds
# the exe logs `dump mem` (its private bytes, working set, the text-run cache, every terminal's lines and cells) and
# this script samples Get-Process beside it. Writes windows/test/out/n-<Name>.csv and prints the curve.
# -MaxPrivate / -MaxRuns (MB / entries): exit 1 when the LAST sample is over (the short regression form).
param([int]$Minutes = 30, [int]$Every = 60, [int]$Rate = 12, [int]$Tiles = 12, [string]$Socket = '', [string]$Size = '1920x1009',
  [string]$Name = 'soak', [string]$Exe = '', [int]$Port = 8797, [double]$MaxPrivate = 0, [int]$MaxRuns = 0)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$outDir = Join-Path $root 'windows\test\out'; New-Item -ItemType Directory -Force $outDir | Out-Null
$out = Join-Path $outDir "n-$Name.png"; $csv = Join-Path $outDir "n-$Name.csv"
Remove-Item $out, "$out.error.txt", "$out.log.txt", $csv -ErrorAction SilentlyContinue
$work = Join-Path $env:TEMP ("cdn-soak-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force $work | Out-Null
$portFile = Join-Path $work 'soak.port'
$node = (Get-Command node -ErrorAction Stop).Source
$deckJs = Join-Path $PSScriptRoot 'soak-deck.js'
$steps = @('wait 4000', 'dump layout', 'dump mem')
$n = [Math]::Max(1, [int][Math]::Floor($Minutes * 60 / $Every))
for ($i = 0; $i -lt $n; $i++) { $steps += "wait $($Every * 1000)"; $steps += 'dump mem' }
$steps += 'dump mem drop'; $steps += 'wait 3000'; $steps += 'dump mem'
$script = Join-Path $work 'soak.txt'
[IO.File]::WriteAllText($script, ($steps -join '; '), (New-Object Text.UTF8Encoding $false))
$fake = Start-Process -FilePath $node -ArgumentList ('"{0}" "{1}" {2} {3} {4}' -f $deckJs, $portFile, $Port, $Rate, $Tiles) -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $work 'deck.log') -RedirectStandardError (Join-Path $work 'deck.err.log')
$p = $null; $rows = @()
try {
  for ($i = 0; $i -lt 100 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
  if (-not (Test-Path $portFile)) { throw ('the soak deck did not start: ' + (Get-Content (Join-Path $work 'deck.err.log') -Raw)) }
  $env:CLAUDE_DECK_LINK = "http://127.0.0.1:$Port/?t=faketoken00000001"
  $hang = ($Minutes * 60 + 120) * 1000
  $a = "--no-save --state-dir `"$work`" --after 500 --size $Size --hang $hang --window-shot `"$out`" --do-file `"$script`""
  # this run's config.json (its state folder): every tile placed in the 3 x 4 grid, as the remote keeps its own layout
  # (cfg layouts[host:port]); the socket, the exe's own client (raw, permessage-deflate) or WinHTTP (wsClient)
  $lay = (1..$Tiles | ForEach-Object { '"t' + $_ + '":[' + $_ + ']' }) -join ','
  $cfgJson = '{"layouts":{"127.0.0.1:' + $Port + '":{' + $lay + '}}' + $(if ($Socket) { ',"wsClient":"' + $Socket + '"' } else { '' }) + '}'
  [IO.File]::WriteAllText((Join-Path $work 'config.json'), $cfgJson, (New-Object Text.UTF8Encoding $false))
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
  $next = 5
  while (-not $p.HasExited) {
    Start-Sleep -Milliseconds 1000
    if ($sw.Elapsed.TotalSeconds -ge $next) {
      $next += $Every
      try { $q = Get-Process -Id $p.Id -ErrorAction Stop; $rows += [pscustomobject]@{ min = [Math]::Round($sw.Elapsed.TotalMinutes, 1); privateMB = [Math]::Round($q.PrivateMemorySize64 / 1MB, 1); wsMB = [Math]::Round($q.WorkingSet64 / 1MB, 1); handles = $q.HandleCount } } catch {}
    }
  }
  "exit $($p.ExitCode) after $([Math]::Round($sw.Elapsed.TotalMinutes, 1)) min"
} finally { try { if ($fake -and -not $fake.HasExited) { $fake.Kill() } } catch {}; try { if ($p -and -not $p.HasExited) { $p.Kill() } } catch {} }
$rows | Export-Csv -NoTypeInformation -Path $csv
'Get-Process samples (min, private MB, working set MB, handles):'
$rows | ForEach-Object { '  {0,6} {1,8} {2,8} {3,6}' -f $_.min, $_.privateMB, $_.wsMB, $_.handles }
$log = if (Test-Path "$out.log.txt") { @(Get-Content "$out.log.txt" -Encoding UTF8) } else { @() }
if (Test-Path "$out.error.txt") { Get-Content "$out.error.txt" }
'the exe''s own samples:'
$log | Where-Object { $_ -match '^(MEM|layout|ws open)' } | ForEach-Object { "  $_" }
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
$last = @($log | Where-Object { $_ -match '^MEM private' } | Select-Object -Last 1)
$bad = @()
if ($last.Count -and $MaxPrivate -gt 0 -and $last[0] -match 'private ([\d.]+) MB' -and [double]$Matches[1] -gt $MaxPrivate) { $bad += "private $($Matches[1]) MB > $MaxPrivate" }
if ($last.Count -and $MaxRuns -gt 0 -and $last[0] -match 'runs (\d+)' -and [int]$Matches[1] -gt $MaxRuns) { $bad += "runs $($Matches[1]) > $MaxRuns" }
if (-not $last.Count) { $bad += 'no MEM line from the exe' }
if ($bad.Count) { 'FAIL ' + ($bad -join '; '); exit 1 } else { 'ok' }
