# windows/web-test/build.ps1 — build and run the headless tests of the web pane (windows/web) and the ask pane
# (windows/src/asks.hpp). Nothing appears on screen: the test windows are parked off every screen, the exes are
# GUI-subsystem (no console), node runs hidden.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/web-test/build.ps1 [-NoRun] [-Only smoke|asks] [-Out <dir>]
# Needs the WinLibs MinGW-w64 g++ (winget install BrechtSanders.WinLibs.POSIX.UCRT), node, and the Microsoft Edge
# WebView2 Runtime (part of Windows 11). Objects are cached under %TEMP%\deck-web-test-obj by time stamp.
# PNGs and logs go to -Out (default %TEMP%\deck-web-test-out). Exit code 0 = every test passed.
param([switch]$NoRun, [string]$Only = '', [string]$Out = '')
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
$obj = Join-Path $env:TEMP 'deck-web-test-obj'; New-Item -ItemType Directory -Force $obj | Out-Null
if (-not $Out) { $Out = Join-Path $env:TEMP 'deck-web-test-out' }
New-Item -ItemType Directory -Force $Out | Out-Null
$flags = @('-std=c++17', '-O1', '-DNDEBUG', '-municode', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-Wall', '-Wno-unknown-pragmas')
$libs = @('-ld2d1', '-ldwrite', '-lwindowscodecs', '-lwinhttp', '-lcrypt32', '-ldwmapi', '-lcomctl32', '-lshell32', '-lole32', '-luuid', '-lgdi32', '-luser32', '-lshlwapi', '-limm32')   # exactly windows/build.ps1's list: the web pane needs nothing more

$headerDirs = @('windows\web', 'windows\src', 'windows\web-test', 'windows\third_party\webview2', 'core\input', 'core\deck') | ForEach-Object { Join-Path $root $_ }
$newestHeader = (Get-ChildItem $headerDirs -Include *.hpp, *.h -Recurse | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc

function Build-Exe([string]$name, [string[]]$sources) {
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
  $exe = Join-Path $env:TEMP "$name.exe"
  Write-Host "link $name"
  & $gpp -municode -mwindows -static -static-libgcc -static-libstdc++ @objects -o $exe @libs
  if ($LASTEXITCODE -ne 0) { throw "link failed: $name" }
  return $exe
}
# a GUI exe, hidden, waited for; its exit code
function Run-Hidden([string]$exe, [string[]]$a, [int]$timeoutSec = 90) {
  $quoted = $a | ForEach-Object { '"' + $_ + '"' }
  $p = Start-Process -FilePath $exe -ArgumentList $quoted -WindowStyle Hidden -PassThru
  if (-not $p.WaitForExit($timeoutSec * 1000)) { try { $p.Kill() } catch {}; return 124 }
  return $p.ExitCode
}
function Count-WebViews { @(Get-CimInstance Win32_Process -Filter "Name = 'msedgewebview2.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like '*ClaudeDeckNative-web*' -or $_.CommandLine -like '*deck-web-test*' }).Count }

$web = @('windows/web/webview2_loader.cpp', 'windows/web/webpane.cpp')
$fail = 0

if (-not $Only -or $Only -eq 'smoke') {
  $exe = Build-Exe 'deck-web-smoke' (@('windows/web-test/smoke.cpp') + $web)
  if (-not $NoRun) {
    $png = Join-Path $Out 'smoke.png'; $log = Join-Path $Out 'smoke.log'
    Remove-Item $png, $log -ErrorAction SilentlyContinue
    $code = Run-Hidden $exe @($png, $log) 60
    Get-Content $log -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  smoke: $_" }
    if ($code -eq 3) { Write-Host 'smoke: the WebView2 runtime is not installed on this machine' }
    if ($code -ne 0 -or -not (Test-Path $png)) { Write-Host "FAIL smoke (exit $code)"; $fail++ } else { Write-Host "PASS smoke  $png" }
  }
}

if (-not $Only -or $Only -eq 'asks') {
  $asksSrc = Join-Path $root 'windows\web-test\asks_test.cpp'
  # its fake deck is the Electron deck's own server (fake-deck.js: lib/phone.js); a tree without it skips this part
  if ((Test-Path $asksSrc) -and -not (Test-Path (Join-Path $root 'lib\phone.js'))) { Write-Host 'SKIP asks (its fake deck needs lib/phone.js, which is not in this tree)' }
  elseif (Test-Path $asksSrc) {
    $exe = Build-Exe 'deck-web-asks' (@('windows/web-test/asks_test.cpp', 'core/deck/askcards.cpp') + $web)   # (asks.hpp carries the native pane too: core/deck/askcards)
    if (-not $NoRun) {
      $node = (Get-Command node -ErrorAction Stop).Source
      $portFile = Join-Path $Out 'fake.port'; $callsFile = Join-Path $Out 'fake.calls.json'; $fakeLog = Join-Path $Out 'fake.log'
      Remove-Item $portFile, $callsFile, (Join-Path $Out 'asks*.png'), (Join-Path $Out 'asks.log') -ErrorAction SilentlyContinue
      $fake = Start-Process -FilePath $node -ArgumentList @('"' + (Join-Path $PSScriptRoot 'fake-deck.js') + '"', '"' + $portFile + '"', '"' + $callsFile + '"', '"' + (Join-Path $PSScriptRoot 'fixtures') + '"') `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $fakeLog -RedirectStandardError (Join-Path $Out 'fake.err.log')
      try {
        for ($i = 0; $i -lt 100 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path $portFile)) { throw 'the fake deck did not start' }
        $port = (Get-Content $portFile -Raw).Trim()
        $code = Run-Hidden $exe @($port, $Out, $callsFile, (Join-Path $PSScriptRoot 'fixtures')) 150
        Get-Content (Join-Path $Out 'asks.log') -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  asks: $_" }
        if ($code -ne 0) { Write-Host "FAIL asks (exit $code)"; $fail++ } else { Write-Host "PASS asks  $Out\asks-*.png" }
      } finally { try { if (-not $fake.HasExited) { $fake.Kill() } } catch {} }
    }
  }
}

if (-not $NoRun) {
  Start-Sleep -Milliseconds 500
  $left = Count-WebViews
  if ($left) { Write-Host "FAIL: $left msedgewebview2.exe left behind"; $fail++ } else { Write-Host 'PASS no msedgewebview2.exe left behind' }
  $prof = Join-Path $env:TEMP 'ClaudeDeckNative-web'
  $dirs = @(Get-ChildItem $prof -Directory -ErrorAction SilentlyContinue)
  if ($dirs.Count) { Write-Host "FAIL: profile folder(s) left: $($dirs.FullName -join ', ')"; $fail++ } else { Write-Host 'PASS no profile folder left' }
}
exit $fail
