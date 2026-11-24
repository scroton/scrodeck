# core/test/proto/build.ps1 — compile and run the frame codec's test.   powershell -File core/test/proto/build.ps1
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) { $gpp = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' | ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1) }
if (-not $gpp) { throw 'no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT' }
$out = Join-Path $env:TEMP 'deck-proto-test.exe'
& $gpp -std=c++17 -O1 -Wall -static -static-libgcc -static-libstdc++ (Join-Path $root 'core/test/proto/proto_test.cpp') -o $out
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
& $out (Join-Path $root 'core/test/fixtures')
exit $LASTEXITCODE
