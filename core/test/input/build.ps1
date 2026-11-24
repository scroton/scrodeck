# core/test/input/build.ps1 — compile and run the input tables' tests: core/input/keys.hpp (the key table of every
# shell) and core/input/mouse.hpp (xterm's mouse service, against vectors from xterm.js itself).
#   powershell -NoProfile -ExecutionPolicy Bypass -File core/test/input/build.ps1 [-Only keys|mouse] [-Regen]
# -Regen first rewrites mouse-vectors.json from @xterm/headless (core/test/vt/oracle/node_modules).
# Console exes run inside this PowerShell's own (hidden) console: nothing appears on screen.
param([string]$Only = '', [switch]$Regen)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if ($Regen) { & node (Join-Path $PSScriptRoot 'gen-vectors.js'); if ($LASTEXITCODE -ne 0) { throw 'gen-vectors failed' } }
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) {
  $gpp = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $gpp) { throw 'no g++ found' }
$bad = 0
if (-not $Only -or $Only -eq 'keys') {
  $out = Join-Path $env:TEMP 'deck-keys-test.exe'
  & $gpp -std=c++17 -O1 -Wall -Wextra -static (Join-Path $PSScriptRoot 'keys_test.cpp') (Join-Path $root 'core\deck\platform.cpp') (Join-Path $root 'core\deck\util.cpp') -o $out
  if ($LASTEXITCODE -ne 0) { throw 'compile failed: keys_test.cpp' }
  & $out; if ($LASTEXITCODE -ne 0) { $bad = 1 }
}
if (-not $Only -or $Only -eq 'mouse') {
  $out = Join-Path $env:TEMP 'deck-mouse-test.exe'
  & $gpp -std=c++17 -O1 -Wall -static (Join-Path $PSScriptRoot 'mouse_test.cpp') -o $out
  if ($LASTEXITCODE -ne 0) { throw 'compile failed: mouse_test.cpp' }
  & $out (Join-Path $PSScriptRoot 'mouse-vectors.json') | Select-Object -Last 30; if ($LASTEXITCODE -ne 0) { $bad = 1 }
}
exit $bad
