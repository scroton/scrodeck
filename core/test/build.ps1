# core/test/build.ps1 — compile the model core + test runner without CMake, and run it.
#   powershell -File core/test/build.ps1
# Finds a g++ (WinLibs MinGW-w64 was installed via winget: BrechtSanders.WinLibs.POSIX.UCRT). Static-links
# libstdc++ so the exe runs with no MinGW DLLs on PATH.
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path   # repo root (core/test -> core -> root)

function Find-Gpp {
  $c = (Get-Command g++ -ErrorAction SilentlyContinue).Source
  if ($c) { return $c }
  $hits = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter g++.exe -ErrorAction SilentlyContinue
  if ($hits) { return $hits[0].FullName }
  throw "no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT"
}

$gpp = Find-Gpp
Write-Host "g++: $gpp"
$out = Join-Path $env:TEMP 'deck-core-test.exe'
$src = @(
  "core/model/layout.cpp", "core/model/hibernate.cpp", "core/model/prompts.cpp", "core/model/triggers.cpp",
  "core/test/run.cpp"
) | ForEach-Object { Join-Path $root $_ }

& $gpp -std=c++17 -O2 -static -static-libgcc -static-libstdc++ @src -o $out
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& $out
exit $LASTEXITCODE
