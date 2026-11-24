# windows/host-test/build.ps1 — build and run the headless test of the native local deck (windows/host).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/host-test/build.ps1 [-NoRun] [-Only conpty|rules|seams|deck] [-Gui]
# -Gui also links the same test as a GUI-subsystem exe (no console at all, as ClaudeDeckNative.exe is), runs it
# hidden with its output in a file, and prints that: the proof that a console-less parent hosts pseudoconsoles.
# Needs the WinLibs MinGW-w64 g++ (winget install BrechtSanders.WinLibs.POSIX.UCRT). Objects are cached under
# %TEMP%\deck-host-test-obj by time stamp (the JSON header is slow to compile). The exe is a console program:
# run it from a shell that is itself hidden (this script, from a tool shell) and nothing appears on screen —
# the shells it starts live in pseudoconsoles, which have no window.
param([switch]$NoRun, [string]$Only = '', [switch]$Gui)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Find-Gpp {
  $c = (Get-Command g++ -ErrorAction SilentlyContinue).Source
  if ($c) { return $c }
  $hit = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($hit) { return $hit }
  throw 'no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT'
}
$gpp = Find-Gpp
$obj = Join-Path $env:TEMP 'deck-host-test-obj'; New-Item -ItemType Directory -Force $obj | Out-Null
$out = Join-Path $env:TEMP 'deck-host-test.exe'
$flags = @('-std=c++17', '-O1', '-Wall', '-Wno-unknown-pragmas', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00')

$sources = @('windows/host-test/host_test.cpp') +
  (Get-ChildItem (Join-Path $root 'windows\host') -Filter *.cpp | ForEach-Object { 'windows/host/' + $_.Name }) +
  (Get-ChildItem (Join-Path $root 'core\session') -Filter *.cpp -ErrorAction SilentlyContinue | ForEach-Object { 'core/session/' + $_.Name }) +
  (Get-ChildItem (Join-Path $root 'core\deck') -Filter *.cpp | ForEach-Object { 'core/deck/' + $_.Name }) +
  (Get-ChildItem (Join-Path $root 'core\vt') -Filter *.cpp | ForEach-Object { 'core/vt/' + $_.Name }) +
  @('core/model/layout.cpp')
$headerDirs = @('windows\host', 'core\deck', 'core\session', 'core\vt', 'core\model', 'core\proto') | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }
$headers = Get-ChildItem $headerDirs -Include *.hpp, *.h, *.inc -Recurse
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
Write-Host 'link'
& $gpp -static -static-libgcc -static-libstdc++ @objects -o $out -lshlwapi -lole32 -luser32 -lws2_32 -liphlpapi -lwinhttp
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
if ($NoRun) { Write-Host "built $out"; exit 0 }
$fx = Join-Path $root 'core\test\fixtures\session'
if ($Only) { & $out $fx $Only } else { & $out $fx }
$code = $LASTEXITCODE
if ($Gui) {
  $guiExe = Join-Path $env:TEMP 'deck-host-test-gui.exe'
  & $gpp -mwindows -static -static-libgcc -static-libstdc++ @objects -o $guiExe -lshlwapi -lole32 -luser32 -lws2_32 -liphlpapi -lwinhttp
  if ($LASTEXITCODE -ne 0) { throw 'link (gui) failed' }
  $log = Join-Path $env:TEMP 'deck-host-test-gui.log'
  $a = @("`"$fx`""); if ($Only) { $a += $Only }
  $p = Start-Process -FilePath $guiExe -ArgumentList $a -WindowStyle Hidden -RedirectStandardOutput $log -PassThru -Wait
  Write-Host '--- the same test as a GUI-subsystem exe (no console):'
  Get-Content $log | Where-Object { $_ -notlike 'PASS*' }
  if ($p.ExitCode -ne 0) { $code = $p.ExitCode }
}
exit $code
