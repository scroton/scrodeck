# core/test/vt/build.ps1 — compile core/vt + its parity test without CMake, and run it.
#   powershell -File core/test/vt/build.ps1 [-Bench] [-Only <substring>] [-Verbose] [-Regen] [-Stress]
# -Regen   first rewrites the *.expected.* files from headless xterm.js (needs `npm install` in core/test/vt/oracle).
# -Bench   also prints feed() throughput.
# -Stress  instead: rebuild with libstdc++ assertions and run the crash hunt (no oracle), then the fixtures under it.
# Finds g++ the way core/test/build.ps1 does (WinLibs MinGW-w64 via winget: BrechtSanders.WinLibs.POSIX.UCRT).
# Links statically: without -static the exe needs MinGW DLLs on PATH and exits silently when they are missing.
param([switch]$Bench, [string]$Only = '', [switch]$Verbose, [switch]$Regen, [switch]$Stress)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path   # core/test/vt -> core/test -> core -> root

function Find-Gpp {
  $c = (Get-Command g++ -ErrorAction SilentlyContinue).Source
  if ($c) { return $c }
  $hits = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter g++.exe -ErrorAction SilentlyContinue
  if ($hits) { return $hits[0].FullName }
  throw "no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT"
}

if ($Regen) {
  & node (Join-Path $root 'core\test\vt\oracle\gen.js')
  if ($LASTEXITCODE -ne 0) { Write-Host "oracle failed"; exit 1 }
}

$gpp = Find-Gpp
Write-Host "g++: $gpp"
$out = Join-Path $env:TEMP 'deck-vt-test.exe'
$src = @(
  "core/vt/terminal.cpp", "core/vt/buffer.cpp", "core/vt/parser.cpp", "core/vt/unicode.cpp",
  "core/test/vt/vt_test.cpp"
) | ForEach-Object { Join-Path $root $_ }

if ($Stress) {
  $out = Join-Path $env:TEMP 'deck-vt-stress.exe'
  & $gpp -std=c++17 -O1 -g -Wall -D_GLIBCXX_ASSERTIONS -static -static-libgcc -static-libstdc++ @src -o $out
  if ($LASTEXITCODE -ne 0) { Write-Host "compile failed"; exit 1 }
  & $out --stress 1000
  if ($LASTEXITCODE -ne 0) { Write-Host "stress failed"; exit 1 }
  & $out (Join-Path $root 'core\test\fixtures\vt')
  exit $LASTEXITCODE
}

& $gpp -std=c++17 -O2 -Wall -static -static-libgcc -static-libstdc++ @src -o $out
if ($LASTEXITCODE -ne 0) { Write-Host "compile failed"; exit 1 }

$testArgs = @((Join-Path $root 'core\test\fixtures\vt'))
if ($Bench) { $testArgs += '--bench' }
if ($Verbose) { $testArgs += '--verbose' }
if ($Only) { $testArgs += @('--only', $Only) }
& $out @testArgs
exit $LASTEXITCODE
