# core/test/session/build.ps1 — compile and run the session ports' test.
#   powershell -NoProfile -ExecutionPolicy Bypass -File core/test/session/build.ps1 [replay|transcript|livestate|meta]
# Regenerate the fixtures first when lib/transcript.js, lib/sessions.js or main.js changed:
#   node core/test/session/gen-fixtures.js
param([string]$Only = '')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) { $gpp = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' | ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1) }
if (-not $gpp) { throw 'no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT' }
$out = Join-Path $env:TEMP 'deck-session-test.exe'
$src = @('core/session/replay.cpp', 'core/session/transcript.cpp', 'core/session/livestate.cpp', 'core/session/meta.cpp', 'core/test/session/session_test.cpp') | ForEach-Object { Join-Path $root $_ }
& $gpp -std=c++17 -O1 -Wall -static -static-libgcc -static-libstdc++ @src -o $out
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
if ($Only) { & $out (Join-Path $root 'core/test/fixtures/session') $Only } else { & $out (Join-Path $root 'core/test/fixtures/session') }
exit $LASTEXITCODE
