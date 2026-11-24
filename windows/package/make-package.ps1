# windows/package/make-package.ps1 - assemble the native deck as a folder that runs without a checkout, and zip it.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/package/make-package.ps1 [-Exe <ClaudeDeckNative.exe>]
#       [-Version 0.1.0] [-OutDir <folder>] [-IncludeUserswitch] [-NoZip]
# Output: <OutDir>\ClaudeDeckNative-<version>\  and  <OutDir>\ClaudeDeckNative-<version>-win-x64.zip
# (OutDir defaults to windows/package/out, which is git-ignored). Prints the size and the sha256 of the zip.
#
# What goes in is EXACTLY what the exe reads beside itself (PACKAGING.md has the why of every line):
#   ClaudeDeckNative.exe
#   assets\fonts\**                      the bundled console fonts (app.cpp loadBundledFonts: `assets\fonts` beside the
#                                        exe) - also served to a browser desk (/desk/assets/fonts/...)
#   renderer\phone\*                     the phone page, its icons, manifest and service worker (core/deck/server.cpp)
#   renderer\index.html, style.css, js\*.js, desk\bridge.js      the browser desk (deskFileAllowed)
#   lib\layout.js                        the one lib file the desk page loads
#   node_modules\@xterm\...              the three xterm files the routes serve, where the routes look for them
#   LICENSES\, README.txt, install.ps1, uninstall.ps1, import-from-electron.ps1, ShortcutTools.ps1, setup.ps1
#   setup\claude-setup-prompt.md        the prompt setup.ps1 hands to a Claude Code session (from setup\ in the repo)
#   skills\deck-triggers\SKILL.md, scripts\deck-trigger.js, scripts\README.md   a companion the session installs
#                                        into each account (no tests)
# Nothing else of the repository: no main.js, no other lib file, no source maps, no tests.
# Without -Exe the exe is built first (windows/build.ps1); with it nothing is compiled. Nothing is installed,
# nothing is started.
param(
  [string]$Exe = '',
  [string]$Version = '',
  [string]$OutDir = '',
  [string]$Root = '',
  [switch]$IncludeUserswitch,
  [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot 'out' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
. (Join-Path $Root 'windows\edition.ps1')   # the edition's names (core/deck/edition.hpp): the exe, the package's name, ...
$exeName = $DeckEdition.Exe

if (-not $Exe) {
  Write-Host "building $exeName (windows/build.ps1)"
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'windows\build.ps1') | ForEach-Object { Write-Host "  $_" }
  if ($LASTEXITCODE -ne 0) { throw 'the build failed (see PACKAGING.md: build a private export of HEAD and pass -Exe)' }
  $Exe = Join-Path $Root ('windows\build\' + $exeName)
}
if (-not (Test-Path -LiteralPath $Exe)) { throw "no exe at $Exe" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path

if (-not $Version) {
  $vi = (Get-Item -LiteralPath $Exe).VersionInfo
  $Version = ('' + $vi.FileVersion).Trim()
  if (-not $Version) { $Version = '0.0.0' }
}
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') { throw "not a usable version: '$Version'" }

$name = "$($DeckEdition.ProgramsDir)-$Version"
$stage = Join-Path $OutDir $name
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# one file, by literal path (font files are named like `FiraCode[wght].ttf`: never a wildcard)
function Add-File([string]$from, [string]$rel) {
  if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { throw "missing in the tree: $from" }
  $to = Join-Path $stage $rel
  $dir = Split-Path -Parent $to
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  [IO.File]::Copy($from, $to, $true)
}
function Add-Tree([string]$fromDir, [string]$relDir, [string]$skip = '') {
  if (-not (Test-Path -LiteralPath $fromDir -PathType Container)) { throw "missing in the tree: $fromDir" }
  $base = (Resolve-Path -LiteralPath $fromDir).Path.TrimEnd('\')
  foreach ($f in [IO.Directory]::GetFiles($base, '*', 'AllDirectories')) {
    $rel = $f.Substring($base.Length + 1)
    if ($skip -and $rel -match $skip) { continue }
    Add-File $f (Join-Path $relDir $rel)
  }
}

# ---- the exe
Add-File $Exe $exeName

# ---- the fonts (files, licences, fonts.css and families.json: the desk page and the exe both read this folder)
Add-Tree (Join-Path $Root 'assets\fonts') 'assets\fonts'

# ---- the phone page: everything in its folder is the page (the server serves any file below it)
Add-Tree (Join-Path $Root 'renderer\phone') 'renderer\phone' '\.map$'

# ---- the browser desk: exactly what core/deck/server.cpp deskFileAllowed lets through
Add-File (Join-Path $Root 'renderer\index.html') 'renderer\index.html'
Add-File (Join-Path $Root 'renderer\style.css') 'renderer\style.css'
Add-File (Join-Path $Root 'renderer\desk\bridge.js') 'renderer\desk\bridge.js'
foreach ($f in [IO.Directory]::GetFiles((Join-Path $Root 'renderer\js'), '*.js')) {
  $n = [IO.Path]::GetFileName($f)
  if ($n -match '^[\w.-]+\.js$') { Add-File $f (Join-Path 'renderer\js' $n) }
}
Add-File (Join-Path $Root 'lib\layout.js') 'lib\layout.js'

# ---- the vendor files, at the paths the routes read (node_modules\@xterm\...), each package with its licence
$xterm = Join-Path $Root 'node_modules\@xterm'
Add-File (Join-Path $xterm 'xterm\lib\xterm.js') 'node_modules\@xterm\xterm\lib\xterm.js'
Add-File (Join-Path $xterm 'xterm\css\xterm.css') 'node_modules\@xterm\xterm\css\xterm.css'
Add-File (Join-Path $xterm 'xterm\LICENSE') 'node_modules\@xterm\xterm\LICENSE'
Add-File (Join-Path $xterm 'addon-fit\lib\addon-fit.js') 'node_modules\@xterm\addon-fit\lib\addon-fit.js'
Add-File (Join-Path $xterm 'addon-fit\LICENSE') 'node_modules\@xterm\addon-fit\LICENSE'
function Package-Version([string]$pkgJson) { try { return (Get-Content -LiteralPath $pkgJson -Raw | ConvertFrom-Json).version } catch { return '?' } }
$xtermVer = Package-Version (Join-Path $xterm 'xterm\package.json')
$fitVer = Package-Version (Join-Path $xterm 'addon-fit\package.json')

# ---- optional: the account-move script (core/deck/platform.cpp userswitchPath looks in <asset root>\bin after
# ~\.local\bin). Off by default: on the owner's box it is in ~\.local\bin already.
if ($IncludeUserswitch) { Add-File (Join-Path $Root 'bin\userswitch') 'bin\userswitch' }

# ---- licences, collected from the tree
Add-File (Join-Path $xterm 'xterm\LICENSE') 'LICENSES\xterm.js-LICENSE.txt'
Add-File (Join-Path $xterm 'addon-fit\LICENSE') 'LICENSES\xterm-addon-fit-LICENSE.txt'
Add-File (Join-Path $Root 'core\third_party\nlohmann\LICENSE.MIT') 'LICENSES\nlohmann-json-LICENSE.MIT.txt'
Add-File (Join-Path $Root 'core\third_party\miniz-tinfl\LICENSE.Unlicense') 'LICENSES\miniz-tinfl-LICENSE.Unlicense.txt'
$wv = Join-Path $Root 'windows\third_party\webview2'
Add-File (Join-Path $wv 'LICENSE.txt') 'LICENSES\WebView2-SDK-LICENSE.txt'
Add-File (Join-Path $wv 'NOTICE.txt') 'LICENSES\WebView2-SDK-NOTICE.txt'
Add-File (Join-Path $wv 'LICENSE-webview.txt') 'LICENSES\webview-loader-LICENSE.txt'
$wvVer = ''
if (Test-Path -LiteralPath (Join-Path $wv 'VERSION.txt')) { Add-File (Join-Path $wv 'VERSION.txt') 'LICENSES\WebView2-SDK-VERSION.txt'; $wvVer = ((Get-Content -LiteralPath (Join-Path $wv 'VERSION.txt') -TotalCount 1) + '').Trim() }
Add-File (Join-Path $Root 'assets\fonts\LICENSES.md') 'LICENSES\fonts\LICENSES.md'
foreach ($d in [IO.Directory]::GetDirectories((Join-Path $Root 'assets\fonts'))) {
  foreach ($f in [IO.Directory]::GetFiles($d, '*.txt')) { Add-File $f (Join-Path 'LICENSES\fonts' ((Split-Path -Leaf $d) + '-' + [IO.Path]::GetFileName($f))) }
}
$third = @"
$($DeckEdition.Shortcut) $Version - third-party software in this package
===================================================================

In $exeName (compiled in)
  nlohmann/json            MIT                 nlohmann-json-LICENSE.MIT.txt
  miniz tinfl.c v1.11      public domain       miniz-tinfl-LICENSE.Unlicense.txt  (the WebSocket inflater)
  Microsoft WebView2 SDK   BSD-style           WebView2-SDK-LICENSE.txt, WebView2-SDK-NOTICE.txt  (header only; $wvVer)
  webview/webview loader   MIT                 webview-loader-LICENSE.txt  (the loader code the web pane was written from)
  MinGW-w64 runtime, libstdc++, libgcc, winpthreads (WinLibs build; linked statically)
                           their own terms: the GCC Runtime Library Exception for libgcc / libstdc++, the
                           MinGW-w64 runtime licence and the winpthreads licence (MIT / BSD style). Their texts are
                           NOT in the source tree this package was collected from; they ship with the compiler
                           (WinLibs: mingw64\share\licenses, or https://www.mingw-w64.org/).

Served to a browser (files beside the exe)
  xterm.js $xtermVer            MIT                 xterm.js-LICENSE.txt
  @xterm/addon-fit $fitVer     MIT                 xterm-addon-fit-LICENSE.txt

Fonts (assets\fonts, read by the exe and served to a browser desk)
  Reddit Mono, JetBrains Mono, Fira Code, Source Code Pro, IBM Plex Mono, Inconsolata
                           SIL Open Font License 1.1    fonts\<family>-OFL.txt
  Ubuntu Mono              Ubuntu Font Licence 1.0      fonts\ubuntumono-UFL.txt
  fonts\LICENSES.md lists every family with its source.

The WebView2 RUNTIME (Microsoft Edge WebView2) is not in this package: it is part of Windows 11. Without it the
ask pane and the cloud panes show a card with a link instead of a page.
"@
Set-Content -LiteralPath (Join-Path $stage 'LICENSES\THIRD-PARTY.txt') -Value $third -Encoding UTF8

# ---- the scripts that install it, and the README
foreach ($s in 'install.ps1', 'uninstall.ps1', 'import-from-electron.ps1', 'ShortcutTools.ps1', 'setup.ps1') { Add-File (Join-Path $PSScriptRoot $s) $s }
# the edition's names for those scripts (no source tree beside them): windows\edition.ps1 reads edition.json
Add-File (Join-Path $Root 'windows\edition.ps1') 'edition.ps1'
[IO.File]::WriteAllText((Join-Path $stage 'edition.json'), ($DeckEdition | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
# the setup session's prompt and the companion the session puts in each account (setup\claude-setup-prompt.md step 3)
Add-File (Join-Path $Root 'setup\claude-setup-prompt.md') 'setup\claude-setup-prompt.md'
foreach ($s in 'SKILL.md', 'scripts\deck-trigger.js', 'scripts\README.md') { Add-File (Join-Path $Root "skills\deck-triggers\$s") "skills\deck-triggers\$s" }
$readme = @"
$($DeckEdition.Shortcut) $Version
===========================

All Claude Code sessions in one window: the native (C++) $($DeckEdition.Product) for Windows 10 / 11, x64.
One program, $exeName, and the files it reads beside itself. No runtime to install, no admin.

START IT
  As the deck of this PC (sessions run here):
      $exeName --local
  As a remote desk of a deck that runs on another PC:
      $exeName
      and paste that deck's link (on the other PC: Settings -> Phone shows it,
      https://<pc>.<tailnet>.ts.net/?t=...). Tailscale must be on, on both machines.
  Started with no option it asks once which of the two it is and remembers the answer;
  $exeName --forget asks again. Ctrl+Shift+P has "Connect to another deck..." and
  "Use this PC" to change it later.

SET IT UP (accounts first, then the rest with Claude Code)
      powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Setup
  installs, then asks for your Claude accounts and their short labels (vv / ms / ts) when the deck has no
  settings yet - or takes them from the Electron deck - and offers a Claude Code session for what a script
  cannot do: the account folders and sign-ins, the companion files, Tailscale. setup.ps1 alone does the same
  for an installed deck. The prompt it hands over is setup\claude-setup-prompt.md: read it first if you like,
  or paste it into a session you open yourself. Nothing runs as administrator except, if you say yes, the one
  machine-wide Tailscale install (one UAC prompt).

INSTALL IT (optional; per user, no admin)
  Running the exe from this folder is enough. To put it in the Start menu from a fixed place:
      powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
  copies this folder to %LOCALAPPDATA%\Programs\$($DeckEdition.ProgramsDir) and makes the Start-menu entry
  "$($DeckEdition.Shortcut)" (it starts the exe with --local). Add -AutoStart to start the deck when you sign in,
  -Mode ask for an entry without --local. Every destination is a parameter (see the top of install.ps1).
  Run a newer package's install.ps1 over it to update; close the deck first.

KEEP THIS FOLDER TOGETHER
  The exe looks beside itself for assets\fonts (the bundled console fonts) and for renderer\, lib\ and
  node_modules\ (the pages it serves to a phone and to a browser desk). Move the folder, not the exe alone.

WHERE ITS STATE LIVES (never in this folder)
  %APPDATA%\$($DeckEdition.StateDir)\            config.json: the mode, a remembered link (its token protected), windows
  %APPDATA%\$($DeckEdition.StateDir)\local\      the local deck: tiles.json, settings.json, triggers.json,
                                           held-prompts.json, usage-cache.json, uploads\, backups
  %APPDATA%\$($DeckEdition.StateDir)\web-cloud\  the claude.ai sign-ins of the cloud panes (one profile per account)
  It never reads or writes the Electron deck's folder (%APPDATA%\claude-deck). To bring the Electron deck's
  tiles and settings over once, see import-from-electron.ps1 (it copies; it changes nothing at the source).

THE PHONE AND THE BROWSER DESK
  Settings -> Phone, as in the Electron deck. Tailscale in front: bind "localhost only" and
      tailscale serve --bg https / http://localhost:<port>
  Never tailscale funnel.

UNINSTALL
      powershell -NoProfile -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\Programs\$($DeckEdition.ProgramsDir)\uninstall.ps1"
  removes what install.ps1 put there: the program folder's files, the Start-menu entry, the auto-start entry.
  It does NOT remove your state (%APPDATA%\$($DeckEdition.StateDir)): delete that folder yourself if you want it gone.
  Not installed, only unzipped? Delete this folder; the exe made one Start-menu entry of its own the first time
  it ran ("$($DeckEdition.Shortcut)" - delete it too).

LICENCES
  LICENSES\THIRD-PARTY.txt lists what is in here and under which terms.
"@
Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Value $readme -Encoding UTF8

# ---- what must be there, and what must not
$must = $exeName, 'edition.ps1', 'edition.json', 'renderer\phone\index.html', 'renderer\phone\app.js', 'renderer\phone\style.css', 'renderer\phone\sw.js',
  'renderer\phone\manifest.webmanifest', 'renderer\phone\icon-192.png', 'renderer\phone\icon-512.png', 'renderer\index.html', 'renderer\style.css',
  'renderer\desk\bridge.js', 'renderer\js\90-boot.js', 'lib\layout.js', 'assets\fonts\fonts.css', 'assets\fonts\families.json',
  'node_modules\@xterm\xterm\lib\xterm.js', 'node_modules\@xterm\xterm\css\xterm.css', 'node_modules\@xterm\addon-fit\lib\addon-fit.js'
foreach ($m in $must) { if (-not (Test-Path -LiteralPath (Join-Path $stage $m))) { throw "the package lacks $m" } }
# every file the desk page itself names must be in the package (a new script in index.html must not be forgotten)
$index = Get-Content -LiteralPath (Join-Path $stage 'renderer\index.html') -Raw
foreach ($m in [regex]::Matches($index, '(?:src|href)="([^"#?:]+)"')) {
  $ref = $m.Groups[1].Value
  $p = [IO.Path]::GetFullPath((Join-Path (Join-Path $stage 'renderer') ($ref -replace '/', '\')))
  if (-not (Test-Path -LiteralPath $p)) { throw "renderer\index.html names $ref, which is not in the package" }
}
$files = [IO.Directory]::GetFiles($stage, '*', 'AllDirectories')
$bad = @($files | ForEach-Object { '\' + $_.Substring($stage.Length + 1) } | Where-Object { $_ -match '\.map$' -or $_ -match '\\(test|tests|docs|\.git)\\' -or $_ -match '\\(main|preload)\.js$' -or $_ -match '\\package(-lock)?\.json$' })
if ($bad.Count) { throw "the package holds what it must not: $($bad -join ', ')" }
$bytes = 0; foreach ($f in $files) { $bytes += (Get-Item -LiteralPath $f).Length }
Write-Host ("package  {0}" -f $stage)
Write-Host ("  {0} files, {1:N0} bytes ({2:N1} MB) unpacked; exe {3:N0} bytes; version {4}" -f $files.Count, $bytes, ($bytes / 1MB), (Get-Item -LiteralPath (Join-Path $stage $exeName)).Length, $Version)

if (-not $NoZip) {
  $zip = Join-Path $OutDir "$name-win-x64.zip"
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
  # Compress-Archive has lost files with [ ] in their names before: count what is in the zip, and write it with
  # the framework's own zipper if anything is missing.
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  function Zip-FileCount([string]$z) { $a = [IO.Compression.ZipFile]::OpenRead($z); try { return @($a.Entries | Where-Object { $_.FullName -notmatch '[\\/]$' }).Count } finally { $a.Dispose() } }
  if ((Zip-FileCount $zip) -ne $files.Count) {
    Write-Host "  Compress-Archive wrote $(Zip-FileCount $zip) of $($files.Count) files: writing the zip with System.IO.Compression instead"
    Remove-Item -LiteralPath $zip -Force
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    if ((Zip-FileCount $zip) -ne $files.Count) { throw 'the zip does not hold every file of the package' }
  }
  $z = Get-Item -LiteralPath $zip
  Write-Host ("zip      {0}" -f $z.FullName)
  Write-Host ("  {0:N0} bytes ({1:N1} MB), {2} files" -f $z.Length, ($z.Length / 1MB), $files.Count)
  Write-Host ("  sha256 {0}" -f (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower())
}
