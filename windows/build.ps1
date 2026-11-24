# windows/build.ps1 — build ClaudeDeckNative.exe: one static file, GUI subsystem (no console window, ever).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/build.ps1 [-Debug]
# Needs the WinLibs MinGW-w64 g++ (winget install BrechtSanders.WinLibs.POSIX.UCRT). Objects are cached under
# windows/build/obj by source time stamp, because one translation unit with the JSON header takes a while.
param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
function Find-Bin {
  $c = (Get-Command g++ -ErrorAction SilentlyContinue).Source
  if ($c) { return (Split-Path $c) }
  $hit = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'mingw64\bin' } | Where-Object { Test-Path (Join-Path $_ 'g++.exe') } | Select-Object -First 1
  if ($hit) { return $hit }
  throw 'no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT'
}
$bin = Find-Bin
$gpp = Join-Path $bin 'g++.exe'; $windres = Join-Path $bin 'windres.exe'
$obj = Join-Path $root 'windows\build\obj'; New-Item -ItemType Directory -Force $obj | Out-Null
. (Join-Path $root 'windows\edition.ps1')   # the edition's names (core/deck/edition.hpp): the exe is $DeckEdition.Exe
$out = Join-Path $root ('windows\build\' + $DeckEdition.Exe)
$opt = if ($Debug) { @('-O0', '-g') } else { @('-O2', '-s') }
$flags = @('-std=c++17', '-municode', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-Wall', '-Wno-unknown-pragmas') + $opt

$sources = @('windows/src/app.cpp', 'windows/src/sidebar.cpp') +
  # the ask pane's web view (WebView2 through its COM interfaces; no loader DLL, no new link library: WEB-NOTES.md)
  @('windows/web/webview2_loader.cpp', 'windows/web/webpane.cpp') +
  (Get-ChildItem (Join-Path $root 'core\vt') -Filter *.cpp | ForEach-Object { 'core/vt/' + $_.Name }) +
  @('core/model/layout.cpp') +
  # local mode: the in-process deck (windows/host, HOST-NOTES.md) and the session rules it runs on (core/session)
  (Get-ChildItem (Join-Path $root 'windows\host') -Filter *.cpp | ForEach-Object { 'windows/host/' + $_.Name }) +
  (Get-ChildItem (Join-Path $root 'core\session') -Filter *.cpp | ForEach-Object { 'core/session/' + $_.Name }) +
  # the deck itself and its server, shared with the Swift shell (core/deck: pure, over the seams windows/host implements)
  (Get-ChildItem (Join-Path $root 'core\deck') -Filter *.cpp | ForEach-Object { 'core/deck/' + $_.Name })
# anything a source may include: a newer header rebuilds every object
$headers = Get-ChildItem (Join-Path $root 'windows\src'), (Join-Path $root 'windows\web'), (Join-Path $root 'windows\third_party\webview2'), (Join-Path $root 'windows\host'), (Join-Path $root 'core\vt'), (Join-Path $root 'core\model'), (Join-Path $root 'core\session'), (Join-Path $root 'core\deck'), (Join-Path $root 'core\proto'), (Join-Path $root 'core\input') -Include *.hpp, *.h -Recurse
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
$res = Join-Path $obj 'app.res.o'
Push-Location (Join-Path $root 'windows\res')
& $windres app.rc -O coff -o $res
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { throw 'windres failed' }

# ONE manifest, app.manifest (2026-11-23). g++ links the toolchain's own lib/default-manifest.o into every exe (its
# spec: %:if-exists(default-manifest.o%s); longPathAware + asInvoker only). ld drops that one for the app's only when
# it is language-neutral, and WinLibs builds it as LANGUAGE 9,1 - so the link warned ".rsrc merge failure: multiple
# non-default manifests" and the exe carried BOTH under RT_MANIFEST 1. An empty default-manifest.o, found first
# through -B (gcc searches its -B folders before its own), stands in for the toolchain's; nothing else is looked up
# there. The exe's manifest is then app.manifest alone: PerMonitorV2, Common-Controls 6, UTF-8 code page, asInvoker.
$noDefault = Join-Path $obj 'no-default-manifest'
# gcc's spec splits a -B folder at its spaces (a checkout under "D:\My Projects" failed to link): such a folder is
# named by its 8.3 short name, else the stub goes to a folder of TEMP's that has none
if ($noDefault -match ' ') {
  New-Item -ItemType Directory -Force $noDefault | Out-Null
  try { $short = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($noDefault).ShortPath } catch { $short = '' }
  if ($short -and $short -notmatch ' ') { $noDefault = $short } else { $noDefault = Join-Path $env:TEMP 'deck-no-default-manifest' }
  if ($noDefault -match ' ') { throw "the build folder has spaces in it and no short name: $noDefault" }
}
$stub = Join-Path $noDefault 'default-manifest.o'
if (-not (Test-Path $stub)) {
  New-Item -ItemType Directory -Force $noDefault | Out-Null
  $empty = Join-Path $noDefault 'empty.c'; Set-Content -Path $empty -Value '' -Encoding ascii
  & $gpp -x c -c $empty -o $stub
  if ($LASTEXITCODE -ne 0) { throw 'the empty default-manifest.o failed' }
}

Write-Host 'link'
& $gpp @opt -municode -mwindows -static -static-libgcc -static-libstdc++ ('-B' + ($noDefault -replace '\\', '/') + '/') @objects $res -o $out `
  -ld2d1 -ldwrite -lwindowscodecs -lwinhttp -lcrypt32 -ldwmapi -lcomctl32 -lshell32 -lole32 -luuid -lgdi32 -luser32 -lshlwapi -limm32 -lws2_32 -liphlpapi -lsecur32
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
$f = Get-Item $out
Write-Host ("built {0} ({1:N0} bytes)" -f $f.FullName, $f.Length)
