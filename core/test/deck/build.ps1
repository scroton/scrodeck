# core/test/deck/build.ps1 — compile and run the core deck's test: core/deck against the JavaScript's fixtures, then
# the deck and its server on fake seams (no process, no socket, no clock of its own; nothing appears on screen).
#   powershell -NoProfile -ExecutionPolicy Bypass -File core/test/deck/build.ps1 [section]
#     sections: remote | wire | startup | held | usage | rules | presence | transcript | asks | askcards | viewer | deskopen | cloud | deck | server | parity |
#               (askcards: core/deck/askcards against page.html's own script, gen-askcards.js; then askcards-read.js:
#                the skill's `ask_page.py read` takes the C++'s answers files. Regenerate after the skill's page.html
#                or ask_page.py changed: node core/test/deck/gen-askcards.js)
#               remoterule | driverrule | ruling   (the review of 2026-11-18: gen-remoterule.js / gen-driverrule.js <path to the JS>)
#               options | hosting                  (core/deck/options + core/deck/hosting: how every shell holds a deck)
#   the sections in $posixSections (deck, hosting, options, remoterule, driverrule, ruling, server) run twice: on the
#   fakes as Windows has them, then on their POSIX twin (--posix: darwin, '/' paths, a login $SHELL - the deck as the
#   Mac configures it)
# Regenerate the fixtures first when lib/phone.js, lib/queue.js, lib/startupprompts.js, lib/usage.js or main.js changed:
#   node core/test/deck/gen-fixtures.js            (add --routes, from PowerShell, for the HTTP routes: it runs the
#                                                   real lib/phone.js server on 127.0.0.1:8812 for a moment)
# Objects are cached under %TEMP%\deck-core-test-obj by time stamp (the JSON header is slow to compile).
param([string]$Only = '', [switch]$NoRun)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) { $gpp = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' | ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1) }
if (-not $gpp) { throw 'no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT' }
$obj = Join-Path $env:TEMP 'deck-core-test-obj'; New-Item -ItemType Directory -Force $obj | Out-Null
$out = Join-Path $env:TEMP 'deck-core-test.exe'
$flags = @('-std=c++17', '-O1', '-Wall', '-Wextra')
$sources = (Get-ChildItem (Join-Path $root 'core\deck') -Filter *.cpp | ForEach-Object { 'core/deck/' + $_.Name }) +
  (Get-ChildItem (Join-Path $root 'core\session') -Filter *.cpp | ForEach-Object { 'core/session/' + $_.Name }) +
  @('core/model/layout.cpp') +
  (Get-ChildItem (Join-Path $root 'core\test\deck') -Filter *.cpp | ForEach-Object { 'core/test/deck/' + $_.Name })
$headers = Get-ChildItem (Join-Path $root 'core\deck'), (Join-Path $root 'core\session'), (Join-Path $root 'core\model'), (Join-Path $root 'core\test\deck') -Include *.hpp -Recurse
$newestHeader = ($headers | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
$objects = @()
foreach ($s in $sources) {
  $src = Join-Path $root $s
  $o = Join-Path $obj (($s -replace '[\\/]', '_') -replace '\.cpp$', '.o')
  $objects += $o
  $stale = -not (Test-Path $o)
  if (-not $stale) { $t = (Get-Item $o).LastWriteTimeUtc; $stale = ((Get-Item $src).LastWriteTimeUtc -gt $t) -or ($newestHeader -gt $t) }
  if ($stale) {
    Write-Host "compile $s"
    & $gpp @flags -c $src -o $o
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
  }
}
& $gpp -static -static-libgcc -static-libstdc++ @objects -o $out
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
if ($NoRun) { Write-Host "built $out"; exit 0 }
$fx = Join-Path $root 'core\test\fixtures\deck'
$run = @($fx, $root); if ($Only) { $run += $Only }
& $out @run; $winExit = $LASTEXITCODE
# the native ask cards' answers files, read by the skill itself (core/test/deck/askcards-read.js: the REAL
# ask_page.py in a TEMP state; skipped with a line when python or the ask-page skill is not on this box)
if (-not $Only -or $Only -eq 'askcards') {
  $py = Get-Command python -ErrorAction SilentlyContinue
  $reader = Join-Path $root 'core\test\deck\askcards-read.js'
  if (-not (Test-Path $reader)) { Write-Host 'askcards-read: SKIPPED (not in this tree: it runs the ask-page skill, which is not part of it)' }
  elseif ($py) { & node $reader $out; if ($LASTEXITCODE -ne 0) { $winExit = 1 } }
  else { Write-Host 'askcards-read: SKIPPED (no python)' }
}
# the same deck on the POSIX twin of the fakes (darwin, '/' paths, $SHELL -l): every section that runs the deck on
# the fakes (the fixture replays are recordings made on this box, with its paths: win32 only)
$posixSections = @('deck', 'hosting', 'options', 'remoterule', 'driverrule', 'ruling', 'server', 'onedeck')
$posixExit = 0
if (-not $Only -or $posixSections -contains $Only) { & $out @run --posix; $posixExit = $LASTEXITCODE }
if ($winExit -ne 0 -or $posixExit -ne 0) { exit 1 }
exit 0
