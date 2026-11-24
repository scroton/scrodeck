# mac/Tests/build.ps1 — compile and run the façade's test ON WINDOWS: mac/Sources/DeckCoreBridge (pure C++17)
# against the real core, driven on fake seams configured as a Mac (bridge_test.cpp). No process, no socket, no
# file, nothing on screen.
#   powershell -NoProfile -ExecutionPolicy Bypass -File mac/Tests/build.ps1 [-NoRun] [-Strict]
# -Strict adds -Wextra -Wpedantic to the bridge and the test (the core is compiled as its own tests compile it).
# Objects are cached under %TEMP%\deck-mac-bridge-obj by time stamp (the JSON header is slow to compile).
# The same build on a Mac: mac/README.md "First-build checklist", step 2.
param([switch]$NoRun, [switch]$Strict)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) { $gpp = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' | ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1) }
if (-not $gpp) { throw 'no g++ found. Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT' }
$obj = Join-Path $env:TEMP 'deck-mac-bridge-obj'; New-Item -ItemType Directory -Force $obj | Out-Null
$out = Join-Path $env:TEMP 'deck-mac-bridge-test.exe'
$inc = @('-I', $root, '-I', (Join-Path $root 'mac\Sources\DeckCoreBridge\include'), '-I', (Join-Path $root 'mac\Sources\DeckMacSeams'))
$flags = @('-std=c++17', '-O1', '-Wall')
$own = @('mac/Sources/DeckCoreBridge/DeckBridge.cpp', 'mac/Tests/bridge_test.cpp')
$sources = (Get-ChildItem (Join-Path $root 'core\deck') -Filter *.cpp | ForEach-Object { 'core/deck/' + $_.Name }) +
  (Get-ChildItem (Join-Path $root 'core\session') -Filter *.cpp | ForEach-Object { 'core/session/' + $_.Name }) +
  (Get-ChildItem (Join-Path $root 'core\vt') -Filter *.cpp | ForEach-Object { 'core/vt/' + $_.Name }) +
  @('core/model/layout.cpp') + $own
$headers = Get-ChildItem (Join-Path $root 'core\deck'), (Join-Path $root 'core\session'), (Join-Path $root 'core\model'), (Join-Path $root 'core\vt'), (Join-Path $root 'core\proto'), (Join-Path $root 'mac\Sources\DeckCoreBridge'), (Join-Path $root 'mac\Sources\DeckMacSeams'), (Join-Path $root 'windows\src\mouse.hpp') -Include *.hpp, *.h, *.inc -Recurse
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
    $f = $flags
    if ($Strict -and ($own -contains $s)) { $f = $flags + @('-Wextra', '-Wpedantic') }
    & $gpp @f @inc -c $src -o $o
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
  }
}
& $gpp -static -static-libgcc -static-libstdc++ @objects -o $out
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
# the C header must also be valid C (what a Swift importer without C++ interop sees)
$gcc = Join-Path (Split-Path $gpp) 'gcc.exe'
if (Test-Path $gcc) {
  $probe = Join-Path $obj 'header_is_c.c'
  Set-Content -Path $probe -Encoding ascii -Value "#include `"DeckBridge.h`"`nint main(void) { DBOptions o; db_options_init(&o); return (int)sizeof(DBCell) - 16; }"
  & $gcc -std=c99 -Wall -Wextra -Wpedantic -fsyntax-only @inc $probe
  if ($LASTEXITCODE -ne 0) { throw 'DeckBridge.h is not valid C99' }
  Write-Host 'DeckBridge.h: valid C99'
}
# The macOS seam classes cannot be BUILT here (forkpty, libproc, sockets), but their declarations can be checked
# against the core's interfaces: every class of MacSeams.hpp overrides real virtuals and leaves none pure.
$seamProbe = Join-Path $obj 'seams_satisfy_core.cpp'
Set-Content -Path $seamProbe -Encoding ascii -Value @'
#include "MacSeams.hpp"
#include <type_traits>
using namespace deck::mac;
static_assert(!std::is_abstract<MacClock>::value && std::is_base_of<deck::core::Clock, MacClock>::value, "Clock");
static_assert(!std::is_abstract<PosixFiles>::value && std::is_base_of<deck::core::Files, PosixFiles>::value, "Files");
static_assert(!std::is_abstract<MacRandom>::value && std::is_base_of<deck::core::Random, MacRandom>::value, "Random");
static_assert(!std::is_abstract<MacEnv>::value && std::is_base_of<deck::core::Env, MacEnv>::value, "Env");
static_assert(!std::is_abstract<MacProcessList>::value && std::is_base_of<deck::core::ProcessList, MacProcessList>::value, "ProcessList");
static_assert(!std::is_abstract<MacNet>::value && std::is_base_of<deck::core::Net, MacNet>::value, "Net");
static_assert(!std::is_abstract<MacPtyHost>::value && std::is_base_of<deck::core::PtyHost, MacPtyHost>::value, "PtyHost");
static_assert(!std::is_abstract<MacListener>::value && std::is_base_of<deck::core::Listener, MacListener>::value, "Listener");
int main() { return 0; }
'@
& $gpp -std=c++17 -Wall -Wextra -fsyntax-only @inc $seamProbe
if ($LASTEXITCODE -ne 0) { throw 'MacSeams.hpp does not satisfy core/deck/seams.hpp' }
Write-Host 'MacSeams.hpp: 8 seam classes satisfy core/deck/seams.hpp (declarations only; the bodies are unbuilt)'
if ($NoRun) { Write-Host "built $out"; exit 0 }
& $out (Join-Path $root 'mac\Tests')
exit $LASTEXITCODE
