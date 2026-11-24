# windows/package/test-package.ps1 - the package, end to end, WITHOUT installing anything on this PC.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/package/test-package.ps1 [-Exe <ClaudeDeckNative.exe>]
#       [-Work <temp folder>] [-Port 8813] [-Keep]
# What it runs (each prints PASS / FAIL lines; exit 1 when anything failed):
#   1. make-package.ps1 (with -Exe when given: nothing is compiled) and what is / is not in the folder and the zip
#   2. the ZIP unpacked into a temp folder, install.ps1 from there with EVERY destination in the temp folder
#      (a fake Programs folder, a fake Start-menu folder, a fake Startup folder), -AutoStart on; the shortcuts read
#      back: target, arguments, AppUserModelID (compared with windows/src/notify.hpp)
#   2b. setup.ps1 from the installed folder with -NoStart, every folder in TEMP (a fake home with an alias file, a
#      fake other deck's settings, stand-in claude / winget files that are never run): the accounts it writes, the
#      prompt it fills in (each ask mode), the command line for each choice - run once with a stand-in `claude`
#      function to prove the quoting - the one elevated step planned and not run; install.ps1 -Setup
#   3. the INSTALLED exe, headless (`--shot`, no window), `--local --serve <port>` with a temp --state-dir and a temp
#      --home, with NO --assets and NO --fonts-dir: the pages, the desk, the vendor files, a font, the manifest -
#      with the token, and without it (401); its log must say it loaded the fonts from beside itself. Should the exe
#      not find them beside itself, the same again WITH the two switches, and the first run stays a FAIL (that is
#      the request to the exe's owners)
#   4. import-from-electron.ps1 on synthetic files in temp folders (never a real state folder)
#   5. uninstall.ps1: the temp destinations are empty, a file it did not install stays, the state folder stays
#   6. the REAL Start menu, Startup folder, %LOCALAPPDATA%\Programs, HKCU Run key and %APPDATA%\claude-deck-native
#      are listed before and after: unchanged
# Rules it keeps: loopback only, port 8811-8819; the token is read from the temp settings.json and never printed;
# nothing appears on screen; Claude is never started (--no-claude, a temp home); nothing of the live deck is read.
param([string]$Exe = '', [string]$Work = '', [int]$Port = 8813, [switch]$Keep)
$ErrorActionPreference = 'Stop'
if ($Port -lt 8811 -or $Port -gt 8819) { throw 'tests use ports 8811-8819 only' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe, the folders, the shortcut
$script:fail = 0
function Check([bool]$ok, [string]$what, [string]$detail = '') {
  if ($ok) { Write-Host "PASS  $what" } else { Write-Host "FAIL  $what$(if ($detail) { ': ' + $detail })"; $script:fail++ }
}
# a child PowerShell, its output as lines and its exit code in $script:code (its stderr must not stop THIS script)
function Invoke-Child([string[]]$a) {
  $ErrorActionPreference = 'Continue'
  $o = & powershell -NoProfile -ExecutionPolicy Bypass -File @a 2>&1 | ForEach-Object { "$_" }
  $script:code = $LASTEXITCODE
  return ,@($o)
}
if (-not $Work) { $Work = Join-Path $env:TEMP ('cdn-pkg-' + [guid]::NewGuid().ToString('N').Substring(0, 10)) }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path -LiteralPath $Work).Path
foreach ($forbidden in (Join-Path $env:LOCALAPPDATA 'Programs'), $env:APPDATA) { if ($Work.StartsWith($forbidden, [StringComparison]::OrdinalIgnoreCase)) { throw "the work folder may not be under $forbidden" } }

# ---- 0. the real locations, before
function Real-State {
  $o = @()
  $programs = [Environment]::GetFolderPath('Programs'); $startup = [Environment]::GetFolderPath('Startup')
  foreach ($dir in $programs, $startup) {
    if (Test-Path -LiteralPath $dir) { foreach ($f in [IO.Directory]::GetFiles($dir, '*', 'AllDirectories')) { $o += "$f|$((Get-Item -LiteralPath $f -Force).LastWriteTimeUtc.Ticks)" } }
  }
  $lp = Join-Path $env:LOCALAPPDATA 'Programs'
  if (Test-Path -LiteralPath $lp) { foreach ($e in [IO.Directory]::GetFileSystemEntries($lp)) { $o += "programs|$e" } }
  $o += "ClaudeDeckNative in Programs|$(Test-Path -LiteralPath (Join-Path $lp $DeckEdition.ProgramsDir))"
  $run = Get-Item -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue
  if ($run) { foreach ($n in $run.GetValueNames()) { $o += "run|$n|$($run.GetValue($n))".GetHashCode() } }
  $native = Join-Path $env:APPDATA $DeckEdition.StateDir
  $o += "native state exists|$(Test-Path -LiteralPath $native)"
  if (Test-Path -LiteralPath $native) { foreach ($e in [IO.Directory]::GetFileSystemEntries($native)) { $o += "native|$e|$((Get-Item -LiteralPath $e -Force).LastWriteTimeUtc.Ticks)" } }
  return ($o | Sort-Object) -join "`n"
}
$before = Real-State

# ---- 1. the package
Write-Host '--- 1. make-package.ps1'
$outDir = Join-Path $PSScriptRoot 'out'
$mk = @((Join-Path $PSScriptRoot 'make-package.ps1'))
if ($Exe) { $mk += @('-Exe', (Resolve-Path -LiteralPath $Exe).Path) }
$made = Invoke-Child $mk
$made | ForEach-Object { Write-Host "  $_" }
Check ($script:code -eq 0) 'make-package.ps1 ran'
$stage = (($made | Where-Object { $_ -match '^package\s+(.+)$' }) -replace '^package\s+', '' | Select-Object -First 1)
$zip = (($made | Where-Object { $_ -match '^zip\s+(.+)$' }) -replace '^zip\s+', '' | Select-Object -First 1)
if (-not $stage -or -not $zip -or -not (Test-Path -LiteralPath $zip)) { Write-Host 'FAIL  no package was made: stopping'; exit 1 }
Check ([bool]($made | Where-Object { $_ -match 'sha256 [0-9a-f]{64}$' })) 'it printed the zip''s sha256'
$rel = @([IO.Directory]::GetFiles($stage, '*', 'AllDirectories') | ForEach-Object { $_.Substring($stage.Length + 1) })
$top = @($rel | ForEach-Object { ($_ -split '\\')[0] } | Sort-Object -Unique)
$allowedTop = $DeckEdition.Exe, 'edition.ps1', 'edition.json', 'assets', 'renderer', 'lib', 'node_modules', 'LICENSES', 'README.txt', 'install.ps1', 'uninstall.ps1', 'import-from-electron.ps1', 'ShortcutTools.ps1', 'setup.ps1', 'setup', 'skills'
Check (-not @($top | Where-Object { $allowedTop -notcontains $_ }).Count) 'the top level holds only what PACKAGING.md lists' (($top | Where-Object { $allowedTop -notcontains $_ }) -join ', ')
Check ((@($rel | Where-Object { $_ -like 'lib\*' }) -join ',') -eq 'lib\layout.js') 'lib\ is layout.js and nothing else'
$nm = @($rel | Where-Object { $_ -like 'node_modules\*' } | Sort-Object)
Check (($nm -join ',') -eq 'node_modules\@xterm\addon-fit\lib\addon-fit.js,node_modules\@xterm\addon-fit\LICENSE,node_modules\@xterm\xterm\css\xterm.css,node_modules\@xterm\xterm\lib\xterm.js,node_modules\@xterm\xterm\LICENSE' -or $nm.Count -eq 5) 'node_modules\ is the three xterm files and their two licences' ($nm -join ', ')
Check (-not @($rel | Where-Object { $_ -match '\.map$|(^|\\)(main|preload)\.js$|(^|\\)package(-lock)?\.json$|(^|\\)(test|tests|docs)\\' }).Count) 'no main.js, preload.js, package.json, source map, test or doc'
$rendererExtra = @($rel | Where-Object { $_ -like 'renderer\*' -and $_ -notmatch '^renderer\\(index\.html|style\.css|desk\\bridge\.js|js\\[\w.-]+\.js|phone\\[^\\]+)$' })
Check (-not $rendererExtra.Count) 'renderer\ is the desk page''s files and the phone page''s folder' ($rendererExtra -join ', ')
$lacks = @()
foreach ($need in 'LICENSES\THIRD-PARTY.txt', 'LICENSES\xterm.js-LICENSE.txt', 'LICENSES\xterm-addon-fit-LICENSE.txt', 'LICENSES\nlohmann-json-LICENSE.MIT.txt', 'LICENSES\WebView2-SDK-LICENSE.txt', 'LICENSES\WebView2-SDK-NOTICE.txt', 'LICENSES\webview-loader-LICENSE.txt', 'LICENSES\fonts\LICENSES.md', 'LICENSES\fonts\ubuntumono-UFL.txt', 'LICENSES\fonts\jetbrainsmono-OFL.txt', 'README.txt') {
  if ($rel -notcontains $need) { $lacks += $need }
}
Check (-not $lacks.Count) 'the licences (xterm, the fonts, nlohmann/json, WebView2, the webview loader) and the README are in it' ($lacks -join ', ')
$fontDirs = @([IO.Directory]::GetDirectories((Join-Path $root 'assets\fonts')))
$fontsOk = $true
foreach ($f in [IO.Directory]::GetFiles((Join-Path $root 'assets\fonts'), '*', 'AllDirectories')) { if (-not (Test-Path -LiteralPath (Join-Path $stage ('assets\fonts\' + $f.Substring((Join-Path $root 'assets\fonts').Length + 1))))) { $fontsOk = $false } }
Check $fontsOk "assets\fonts is whole ($($fontDirs.Count) families, with their licences)"

# ---- 2. unzip, install into temp destinations
Write-Host '--- 2. the zip, unpacked, installed into temp destinations'
$unz = Join-Path $Work 'unzipped'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($zip, $unz)
$pkg = @([IO.Directory]::GetDirectories($unz))[0]
$unzRel = @([IO.Directory]::GetFiles($pkg, '*', 'AllDirectories'))
Check ($unzRel.Count -eq $rel.Count) "the zip unpacks to the same $($rel.Count) files (the fonts with [ ] in their names too)" "$($unzRel.Count) files"
$dest = Join-Path $Work ('Programs\' + $DeckEdition.ProgramsDir); $fakeStart = Join-Path $Work 'StartMenu'; $fakeStartup = Join-Path $Work 'Startup'
New-Item -ItemType Directory -Force -Path $fakeStart, $fakeStartup | Out-Null
$inst = Invoke-Child @((Join-Path $pkg 'install.ps1'), '-Destination', $dest, '-StartMenuDir', $fakeStart, '-StartupDir', $fakeStartup, '-AutoStart')
$inst | ForEach-Object { Write-Host "  $_" }
Check ($script:code -eq 0) 'install.ps1 ran'
$destExe = Join-Path $dest $DeckEdition.Exe
if (-not (Test-Path -LiteralPath $destExe)) { Write-Host 'FAIL  the exe is not in the temp destination: stopping'; exit 1 }
Check $true 'the exe is in the temp destination'
Check (@([IO.Directory]::GetFiles($dest, '*', 'AllDirectories')).Count -eq $rel.Count + 1) 'every file of the package is installed, plus install-manifest.json'
. (Join-Path $PSScriptRoot 'ShortcutTools.ps1')
# the id and the name the NOTIFIER uses, read from its source: the installer must agree with it
$notify = Get-Content -LiteralPath (Join-Path $root 'windows\src\notify.hpp') -Raw
# (notify.hpp takes them from the edition, core/deck/edition.hpp: L"" DECK_EDITION_APP_ID, L"" DECK_EDITION_SHORTCUT L".lnk")
$srcId = if ($notify -match 'kAppId\s*=\s*L""\s*DECK_EDITION_APP_ID\s*;') { $DeckEdition.AppId } else { [regex]::Match($notify, 'kAppId\s*=\s*L"([^"]+)"').Groups[1].Value }
$srcName = if ($notify -match 'kShortcutName\s*=\s*L""\s*DECK_EDITION_SHORTCUT\s*L"\.lnk"\s*;') { $DeckEdition.Shortcut + '.lnk' } else { [regex]::Match($notify, 'kShortcutName\s*=\s*L"([^"]+)"').Groups[1].Value }
Check ($srcId -and $srcId -eq $DeckAppId -and $srcName -eq $DeckShortcutName) "the installer's AppUserModelID and shortcut name are notify.hpp's ($srcId, $srcName)"
foreach ($pair in @(@('Start-menu', $fakeStart), @('auto-start', $fakeStartup))) {
  $lnk = Join-Path $pair[1] $srcName
  if (-not (Test-Path -LiteralPath $lnk)) { Check $false "the $($pair[0]) shortcut exists"; continue }
  $s = Read-DeckShortcut $lnk
  Check (Test-SamePath $s.Target $destExe) "the $($pair[0]) shortcut starts the installed exe itself (no cmd / powershell / wscript)" $s.Target
  Check ($s.Arguments -eq '--local') "the $($pair[0]) shortcut passes --local" $s.Arguments
  Check ($s.AppId -eq $srcId) "the $($pair[0]) shortcut carries the AppUserModelID" $s.AppId
}
Check (@([IO.Directory]::GetFiles($fakeStart)).Count -eq 1 -and @([IO.Directory]::GetFiles($fakeStartup)).Count -eq 1) 'one file in the fake Start menu, one in the fake Startup folder'
# a second install over it (an update) must work and keep one manifest
$inst2 = Invoke-Child @((Join-Path $pkg 'install.ps1'), '-Destination', $dest, '-StartMenuDir', $fakeStart, '-StartupDir', $fakeStartup)
Check ($script:code -eq 0 -and @((Get-Content -LiteralPath (Join-Path $dest 'install-manifest.json') -Raw | ConvertFrom-Json).shortcuts).Count -eq 2) 'installing again over it (an update) works and still knows both shortcuts' ($inst2 -join ' / ')

# ---- 2b. setup.ps1: the first run (accounts first, then the Claude Code session) - TEMP folders only, -NoStart:
# nothing is started, opened or elevated; never a real ~\.claude* folder or deck state folder
Write-Host '--- 2b. setup.ps1 with -NoStart against temp folders'
$u8 = New-Object Text.UTF8Encoding $false
Check (($rel -contains 'setup\claude-setup-prompt.md') -and (Get-FileHash -LiteralPath (Join-Path $stage 'setup\claude-setup-prompt.md')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $root 'setup\claude-setup-prompt.md')).Hash) 'the setup prompt is in the package, as in the repo'
Check (-not @('setup.ps1', 'skills\deck-triggers\SKILL.md', 'skills\deck-triggers\scripts\deck-trigger.js', 'skills\deck-triggers\scripts\README.md' | Where-Object { $rel -notcontains $_ }).Count -and -not @($rel | Where-Object { $_ -like 'skills\*\tests\*' }).Count) 'setup.ps1 and the deck-triggers skill (without its tests) are in it'
Check (-not @([IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'setup.ps1')) | Where-Object { $_ -gt 127 }).Count) 'setup.ps1 is plain ASCII (Windows PowerShell 5.1 reads a BOM-less file as ANSI)'
$su = Join-Path $dest 'setup.ps1'
$sw = Join-Path $Work 'setup'
$sHome = Join-Path $sw 'home'; $sEl = Join-Path $sw 'electron'; $sBin = Join-Path $sw 'bin'
New-Item -ItemType Directory -Force -Path (Join-Path $sHome '.claude-vv'), (Join-Path $sHome '.claude-ms'), $sEl, $sBin | Out-Null
[IO.File]::WriteAllText((Join-Path $sHome '.claude-code-account-aliases'), ("alias claudevv='CLAUDE_CONFIG_DIR=""`$HOME/.claude-vv"" claude'`nalias claudems='CLAUDE_CONFIG_DIR=""`$HOME/.claude-ms"" claude'`n"), $u8)
[IO.File]::WriteAllText((Join-Path $sHome '.claude-vv\.claude.json'), '{"oauthAccount":{"emailAddress":"work@example.com"}}', $u8)
[IO.File]::WriteAllText((Join-Path $sEl 'settings.json'), '{"aliases":{"work@example.com":"vv","claudems":"ms"},"phone":{"enabled":true,"port":8815}}', $u8)
foreach ($n in 'claude.exe', 'winget.exe') { [IO.File]::WriteAllText((Join-Path $sBin $n), '', $u8) }   # stand-ins: never run (-NoStart)
$fakeClaude = Join-Path $sBin 'claude.exe'; $fakeWinget = Join-Path $sBin 'winget.exe'; $noFile = Join-Path $sBin 'not-here.exe'
function Run-Setup([string]$state, [string[]]$more, [string]$claudeExe = $fakeClaude, [string]$scriptPath = $su) {
  $o = Invoke-Child (@($scriptPath, '-StateDir', $state, '-HomeDir', $sHome, '-OtherStateDirs', $sEl, '-ClaudeExe', $claudeExe, '-TailscaleExe', $noFile, '-WingetExe', $fakeWinget, '-NoStart', '-Defaults') + $more)
  $r = $null; $rf = Join-Path $state 'setup\setup-installer.json'
  if (Test-Path -LiteralPath $rf) { $r = Get-Content -LiteralPath $rf -Raw | ConvertFrom-Json }
  $p = ''; $pf = Join-Path $state 'setup\claude-setup-prompt.md'
  if (Test-Path -LiteralPath $pf) { $p = [IO.File]::ReadAllText($pf) }
  return [pscustomobject]@{ Code = $script:code; Out = ($o -join "`n"); R = $r; P = $p; State = $state; PromptFile = $pf }
}
function Aliases-Of([string]$state) { $j = Get-Content -LiteralPath (Join-Path $state 'settings.json') -Raw | ConvertFrom-Json; return $j }
function Keys-Of($o) { return (@($o.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) | Sort-Object) -join ',' }

# A: accounts found (the alias file + another deck's labels), Claude in Chrome, "only when necessary", a terminal
$sa = Run-Setup (Join-Path $sw 'state-a') @('-Chrome', 'yes', '-InstallTailscale', 'yes')
Check ($sa.Code -eq 0 -and $sa.R) 'A: setup.ps1 ran (found accounts, Chrome, ask only when necessary, a terminal)' $sa.Out
$ja = Aliases-Of $sa.State
Check ((Keys-Of $ja.aliases) -eq 'claudems=ms,claudevv=vv,work@example.com=vv' -and @($ja.PSObject.Properties).Count -eq 1) 'A: the found accounts become settings.json { aliases: e-mail / keyword -> label } and nothing else' (Keys-Of $ja.aliases)
Check ($sa.P -and $sa.P -notmatch '\{\{[A-Z_]+\}\}') 'A: the prompt is written to <state>\setup with every placeholder filled in'
Check ($sa.P.StartsWith('ASK MODE: only when necessary') -and $sa.P.Contains('<!-- BEGIN PERMISSION BLOCK -->') -and $sa.P.Contains('You MUST ask me first before')) 'A: "only when necessary": the switch line, and the permission block kept'
Check ($sa.P.Contains("| vv | claudevv | $(Join-Path $sHome '.claude-vv') | work@example.com |") -and $sa.P.Contains("| ms | claudems | $(Join-Path $sHome '.claude-ms') | (ask me) |")) 'A: the prompt lists both accounts (label, keyword, folder, e-mail)'
Check ($sa.P.Contains((Join-Path $sa.State 'setup\setup-report.md')) -and $sa.P.Contains('wrote the account labels')) 'A: the prompt names the report file and says the labels were written'
$ptrA = "Read the file '$($sa.PromptFile)' and do what it says. It is the Claude Deck setup prompt."
Check ($sa.R.command -eq "claude --chrome `"$ptrA`"") 'A: the command line is claude --chrome and a pointer to the filled-in prompt' $sa.R.command
Check ($sa.R.started -eq $false -and $sa.Out -match 'would start, in a NEW NORMAL window') 'A: nothing was started (-NoStart)'
Check ($sa.R.terminal.file -eq 'powershell.exe' -and $sa.R.terminal.args -match '^-NoExit -NoLogo -EncodedCommand [A-Za-z0-9+/=]+$') 'A: the session window is a plain PowerShell window: not hidden, not elevated' $sa.R.terminal.args
$body = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String(($sa.R.terminal.args -split ' ')[-1]))
Check ($body -eq $sa.R.terminal.script) 'A: the encoded command is the script the receipt shows'
# what that window runs, with a stand-in `claude` that only records its arguments: the quoting must survive whole
$argsOut = Join-Path $sw 'claude-args.json'
$stand = "function claude { ConvertTo-Json -InputObject @(`$args | ForEach-Object { [string]`$_ }) | Set-Content -LiteralPath '$argsOut' -Encoding UTF8; Set-Content -LiteralPath '$argsOut.cwd' -Value (Get-Location).Path }`n" + ($body -replace '(?m)^\$host\.UI\.RawUI\.WindowTitle.*$', '')
$null = & powershell -NoProfile -NonInteractive -EncodedCommand ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($stand))) 2>&1
$got = @(); if (Test-Path -LiteralPath $argsOut) { $gj = Get-Content -LiteralPath $argsOut -Raw | ConvertFrom-Json; $got = @($gj) }   # (5.1: never ConvertFrom-Json inside @())
Check ($got.Count -eq 2 -and $got[0] -eq '--chrome' -and $got[1] -eq $ptrA) 'A: run with a stand-in claude, the window passes exactly --chrome and the pointer, its path whole' ($got -join ' | ')
Check ((Test-Path -LiteralPath "$argsOut.cwd") -and (Get-Content -LiteralPath "$argsOut.cwd" -Raw).Trim() -eq $sHome) 'A: and runs in the home folder'
Check (@($sa.R.tailscale.elevated).Count -eq 1 -and "$($sa.R.tailscale.elevated)" -like 'winget install --id Tailscale.Tailscale *' -and $sa.Out -match 'would run ELEVATED') 'A: Tailscale missing + yes: the ONE elevated step is planned (winget install), not run'

# B: typed in, "as you go", no Chrome, paste it yourself
$sb = Run-Setup (Join-Path $sw 'state-b') @('-UseFound', 'no', '-Accounts', 'Me@Example.com|ts;second@example.com|ms2|D:\cfg\ms2|claudem2', '-AskMode', 'as-you-go', '-Chrome', 'no', '-Session', 'paste', '-InstallTailscale', 'no')
Check ($sb.Code -eq 0) 'B: setup.ps1 ran (typed-in accounts, as you go, no Chrome, paste)' $sb.Out
Check ((Keys-Of (Aliases-Of $sb.State).aliases) -eq 'claudem2=ms2,claudets=ts,me@example.com=ts,second@example.com=ms2') 'B: only the typed accounts are written (e-mail lower-cased, default keyword claude<label>)' (Keys-Of (Aliases-Of $sb.State).aliases)
Check ($sb.P.StartsWith('ASK MODE: as you go') -and -not $sb.P.Contains('PERMISSION BLOCK -->') -and $sb.P.Contains('(No permission block:')) 'B: "as you go": the switch line, the permission block left out'
Check ($sb.P.Contains("| ts | claudets | $(Join-Path $sHome '.claude-ts') | me@example.com |") -and $sb.P.Contains('| ms2 | claudem2 | D:\cfg\ms2 | second@example.com |')) 'B: the prompt lists the typed accounts with their folders'
Check ($sb.R.session -eq 'paste' -and $null -eq $sb.R.command -and $null -eq $sb.R.terminal -and $sb.R.commandSuggested -notmatch '--chrome' -and $sb.Out.Contains("Read the file '$($sb.PromptFile)'")) 'B: paste: no window, the line to paste and the file are shown, no --chrome'
Check (-not @($sb.R.tailscale.elevated).Count -and $sb.P.Contains('chose not to install it')) 'B: Tailscale declined: nothing elevated, and the prompt says so'

# C: a deck with settings of its own is left alone; an earlier report is read back
$sc = Join-Path $sw 'state-c'; New-Item -ItemType Directory -Force -Path (Join-Path $sc 'setup') | Out-Null
[IO.File]::WriteAllText((Join-Path $sc 'settings.json'), '{"aliases":{"x@example.com":"xx"},"fontSize":15}', $u8)
[IO.File]::WriteAllText((Join-Path $sc 'setup\setup-report.md'), "# Claude Deck setup report`nSTATUS: partial`nWRITTEN: today`n`n## Done`n- the accounts`n`n## For you to do`n- sign in to ms`n", $u8)
$hc = (Get-FileHash -LiteralPath (Join-Path $sc 'settings.json')).Hash
$rc = Run-Setup $sc @('-Session', 'later')
Check ($rc.Code -eq 0 -and (Get-FileHash -LiteralPath (Join-Path $sc 'settings.json')).Hash -eq $hc -and $rc.R.accounts -eq 'kept') 'C: a deck with a settings.json already: it is not touched'
Check ($rc.Out -match 'reported: STATUS partial' -and $rc.Out -match 'sign in to ms') 'C: an earlier session''s report is read back (STATUS and "For you to do")'
$rr = Run-Setup $sc @('-Report')
Check ($rr.Code -eq 0 -and $rr.Out -match '## Done' -and $rr.Out -match 'STATUS: partial') 'C: -Report prints the whole report'

# D: the deck runs from the state folder: no settings are written under it
$sd = Join-Path $sw 'state-d'; New-Item -ItemType Directory -Force -Path $sd | Out-Null
[IO.File]::WriteAllText((Join-Path $sd 'running.json'), ('{"pid":' + $PID + ',"at":"2026-01-01T00:00:00.000Z"}'), $u8)
$rd = Run-Setup $sd @('-Session', 'later')
Check ($rd.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $sd 'settings.json')) -and $rd.R.accounts -eq 'deck-running' -and $rd.P.Contains('did NOT write the account labels')) 'D: the deck running: settings.json is not written, the prompt hands the labels to the session'

# E: refused accounts (a long label; ~\.claude itself), and "import first"
$se = Run-Setup (Join-Path $sw 'state-e') @('-UseFound', 'no', '-Accounts', 'a@example.com|waytoolonglabel')
Check ($se.Code -eq 2 -and -not (Test-Path -LiteralPath (Join-Path $se.State 'settings.json'))) 'E: a label that is not 1-8 letters or digits is refused, nothing written' $se.Out
$se2 = Run-Setup (Join-Path $sw 'state-e2') @('-UseFound', 'no', '-Accounts', 'a@example.com|vv|~\.claude')
Check ($se2.Code -eq 2 -and $se2.Out -match 'its own folder') 'E: ~\.claude itself as an account''s folder is refused' $se2.Out
$sf = Run-Setup (Join-Path $sw 'state-f') @('-UseFound', 'import')
Check ($sf.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $sf.State 'settings.json')) -and $sf.Out -match 'import-from-electron\.ps1' -and -not $sf.P) 'F: "import first": nothing written, the import command shown, stops there'

# G: Claude Code missing: Anthropic's per-user installer is offered, in this window, and (here) not run
$sg = Run-Setup (Join-Path $sw 'state-g') @('-InstallClaude', 'yes', '-Session', 'later') $noFile
Check ($sg.Code -eq 0 -and $sg.R.claude.wouldInstall -eq $true -and $sg.Out -match 'would run, in this window: .*https://claude\.ai/install\.ps1') 'G: Claude Code missing: its per-user installer is offered (and not run)' $sg.Out

# H: install.ps1 -Setup runs it from the destination, handing on the arguments it does not know
$dest2 = Join-Path $Work ('Programs2\' + $DeckEdition.ProgramsDir); $sh = Join-Path $sw 'state-h'
$oh = Invoke-Child @((Join-Path $pkg 'install.ps1'), '-Destination', $dest2, '-NoShortcut', '-Setup', '-StateDir', $sh, '-HomeDir', $sHome, '-OtherStateDirs', 'none', '-Accounts', 'solo@example.com|so', '-ClaudeExe', $fakeClaude, '-TailscaleExe', $noFile, '-WingetExe', $fakeWinget, '-InstallTailscale', 'no', '-NoStart', '-Defaults')
$hOk = ($script:code -eq 0) -and (Test-Path -LiteralPath (Join-Path $sh 'settings.json')) -and ((Aliases-Of $sh).aliases.'solo@example.com' -eq 'so')
$hr = $null; if (Test-Path -LiteralPath (Join-Path $sh 'setup\setup-installer.json')) { $hr = Get-Content -LiteralPath (Join-Path $sh 'setup\setup-installer.json') -Raw | ConvertFrom-Json }
Check ($hOk -and $hr -and $hr.programDir -eq $dest2) 'H: install.ps1 -Setup runs setup.ps1 from the destination with the arguments it does not know' ($oh -join ' / ')
$null = Invoke-Child @((Join-Path $dest2 'uninstall.ps1'))
Check (-not (Test-Path -LiteralPath (Join-Path $dest2 'setup.ps1'))) 'H: and uninstall.ps1 removes it again'
$i1 = Invoke-Child @((Join-Path $pkg 'install.ps1'), '-Destination', $dest2, '-NoShortcut')
Check ($script:code -eq 0 -and ($i1 -join "`n") -match 'setup\s+powershell .*setup\.ps1') 'H: without -Setup, install.ps1 says how to run the setup'
$null = Invoke-Child @((Join-Path $dest2 'uninstall.ps1'))
Check (-not @(Get-ChildItem -LiteralPath $sw -Recurse -Force -Filter '*.writing').Count) 'every file setup.ps1 wrote was written whole (no .writing left)'

# ---- 2c. the Mac's open-setup-session.sh, its plain-bash part under Git Bash (--dry-run: no osascript, no Terminal)
$gitBash = Join-Path $env:ProgramFiles 'Git\bin\bash.exe'
if (-not (Test-Path -LiteralPath $gitBash)) { Write-Host 'SKIP  2c: no Git Bash on this PC' }
else {
  Write-Host '--- 2c. mac/tools/open-setup-session.sh --dry-run under Git Bash (a temp HOME)'
  $macSh = (Join-Path $root 'mac\tools\open-setup-session.sh').Replace('\', '/')
  function Run-Mac([string]$state, [string]$envs) {
    $st = $state.Replace('\', '/')
    $line = "HOME='$($sHome.Replace('\', '/'))' STATE_DIR='$st' PROGRAM_DIR='$($dest.Replace('\', '/'))' $envs bash '$macSh' --dry-run"
    $ErrorActionPreference = 'Continue'
    $o = & $gitBash -c $line 2>&1 | ForEach-Object { "$_" }
    $c = $LASTEXITCODE
    $p = ''; $pf = Join-Path $state 'setup\claude-setup-prompt.md'; if (Test-Path -LiteralPath $pf) { $p = [IO.File]::ReadAllText($pf) }
    return [pscustomobject]@{ Code = $c; Out = ($o -join "`n"); P = $p; State = $state; PromptFile = $st + '/setup/claude-setup-prompt.md' }
  }
  $ma = Run-Mac (Join-Path $sw 'mac-a') "SETUP_CHROME=yes SETUP_ACCOUNTS='New@Example.com|ts' SETUP_CLAUDE=/bin/true"
  Check ($ma.Code -eq 0) 'mac A: it ran (found + typed accounts, Chrome, only when necessary, Terminal)' $ma.Out
  $mj = $null; try { $mj = Get-Content -LiteralPath (Join-Path $ma.State 'settings.json') -Raw | ConvertFrom-Json } catch { }
  Check ($mj -and (Keys-Of $mj.aliases) -eq 'claudems=ms,claudets=ts,claudevv=vv,new@example.com=ts,work@example.com=vv') 'mac A: settings.json { aliases } as on Windows (labels guessed from the folders, e-mail from .claude.json)' $(if ($mj) { Keys-Of $mj.aliases })
  Check ($ma.P.StartsWith('ASK MODE: only when necessary') -and $ma.P.Contains('<!-- BEGIN PERMISSION BLOCK -->') -and $ma.P -notmatch '\{\{[A-Z_]+\}\}' -and $ma.P.Contains('| ts | claudets |')) 'mac A: the prompt filled in, the permission block kept, the accounts listed'
  Check ($ma.Out.Contains("would open Terminal and run: cd ~ && claude --chrome `"Read the file '$($ma.PromptFile)' and do what it says. It is the Claude Deck setup prompt.`"")) 'mac A: the Terminal command: claude --chrome and the pointer (not run)'
  $mb = Run-Mac (Join-Path $sw 'mac-b') "SETUP_USE_FOUND=no SETUP_ASK_MODE=as-you-go SETUP_SESSION=paste SETUP_ACCOUNTS='a@example.com|zz' SETUP_CLAUDE=none"
  Check ($mb.Code -eq 0 -and $mb.P.StartsWith('ASK MODE: as you go') -and -not $mb.P.Contains('PERMISSION BLOCK -->') -and $mb.Out -match 'paste this line') 'mac B: as you go (no permission block), paste it yourself' $mb.Out
  $mc = Run-Mac (Join-Path $sw 'mac-c') "SETUP_USE_FOUND=no SETUP_ACCOUNTS='a@example.com|waytoolonglabel'"
  Check ($mc.Code -eq 2 -and -not (Test-Path -LiteralPath (Join-Path $mc.State 'settings.json'))) 'mac C: a bad label is refused, nothing written'
  $md = Run-Mac (Join-Path $sw 'mac-d') "SETUP_SESSION=terminal SETUP_CLAUDE=none"
  Check ($md.Out -match 'would open Terminal and run: curl -fsSL https://claude\.ai/install\.sh \| bash && ') 'mac D: Claude Code missing: Anthropic''s installer goes in front of the command, shown'
}

# ---- 3. the installed exe serves from beside itself
function Listening([int]$p) { return @(Get-NetTCPConnection -State Listen -LocalPort $p -ErrorAction SilentlyContinue).Count -gt 0 }
function Fetch([string]$path) {
  $r = [pscustomobject]@{ Status = 0; Type = ''; Length = -1; Body = $null; Cookie = '' }
  $resp = $null
  try {
    $req = [Net.HttpWebRequest]::Create(('http://127.0.0.1:' + $Port + $path)); $req.Timeout = 6000; $req.Proxy = $null; $req.AllowAutoRedirect = $false; $req.KeepAlive = $false
    $resp = $req.GetResponse()
  } catch {
    $ex = $_.Exception
    while ($ex -and -not ($ex -is [Net.WebException])) { $ex = $ex.InnerException }
    if (-not $ex -or -not $ex.Response) { return $r }
    $resp = $ex.Response
  }
  try {
    $r.Status = [int]$resp.StatusCode; $r.Type = '' + $resp.ContentType; $r.Cookie = '' + $resp.Headers['Set-Cookie']
    $ms = New-Object IO.MemoryStream; $resp.GetResponseStream().CopyTo($ms); $r.Body = $ms.ToArray(); $r.Length = $r.Body.Length
  } finally { $resp.Close() }
  return $r
}
function Run-Served([string]$tag, [bool]$explicit) {
  $state = Join-Path $Work "state-$tag"; $homeDir = Join-Path $Work "home-$tag"; New-Item -ItemType Directory -Force -Path $state, $homeDir | Out-Null
  $shot = Join-Path $Work "served-$tag.png"
  $res = [pscustomobject]@{ Assets = $false; Fonts = $false; State = $state }
  if (Listening $Port) { Check $false "port $Port is free before the run"; return $res }
  $extra = ''
  if ($explicit) { $extra = " --assets `"$dest`" --fonts-dir `"$(Join-Path $dest 'assets\fonts')`"" }
  $env:CLAUDE_DECK_LINK = $null
  $started = Get-Date
  $p = Start-Process -FilePath $destExe -PassThru -ArgumentList "--local --serve $Port --state-dir `"$state`" --home `"$homeDir`" --no-claude --shell cmd.exe --shot `"$shot`" --after 500 --size 1200x800$extra --do `"wait 12000; dump fonts`""
  $null = $p.Handle                                  # (so ExitCode can be read after it has gone)
  try {
    $settings = Join-Path $state 'local\settings.json'
    $until = (Get-Date).AddSeconds(10); while (-not (Test-Path -LiteralPath $settings) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path -LiteralPath $settings)) { Check $false "[$tag] the exe wrote its settings in the temp state folder"; return $res }
    $token = ''; $until = (Get-Date).AddSeconds(5)
    while (-not $token -and (Get-Date) -lt $until) { try { $token = (Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json).phone.token } catch { }; if (-not $token) { Start-Sleep -Milliseconds 150 } }
    $until = (Get-Date).AddSeconds(5); while (-not (Listening $Port) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 100 }
    Check ([bool]$token -and (Listening $Port)) "[$tag] it serves on 127.0.0.1:$Port and has a token (not printed)"
    $bound = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue | Select-Object -ExpandProperty LocalAddress -Unique)
    Check ($bound.Count -eq 1 -and $bound[0] -eq '127.0.0.1') "[$tag] the port listens on 127.0.0.1 and nothing else" ($bound -join ', ')
    $t = '?t=' + $token
    $size = { param($relPath) (Get-Item -LiteralPath (Join-Path $dest $relPath)).Length }
    $r = Fetch "/$t"
    $phone = $r.Status -eq 200 -and $r.Type -like 'text/html*' -and $r.Length -eq (& $size 'renderer\phone\index.html')
    Check $phone "[$tag] / with the token is the phone page (200, text/html, the installed file byte for byte in length)" "status $($r.Status), $($r.Length) bytes"
    Check ($r.Cookie -like 'deck_phone=*') "[$tag] the token in the query sets the cookie"
    $r = Fetch '/'; Check ($r.Status -eq 401) "[$tag] / without the token is 401" "status $($r.Status)"
    $r = Fetch '/desk/'; Check ($r.Status -eq 401) "[$tag] /desk/ without the token is 401" "status $($r.Status)"
    $r = Fetch "/desk/$t"
    $deskText = ''; if ($r.Body) { $deskText = [Text.Encoding]::UTF8.GetString($r.Body) }
    $desk = $r.Status -eq 200 -and $deskText.Contains('<base href="/desk/renderer/">') -and $deskText.Contains('/desk/renderer/desk/bridge.js')
    Check $desk "[$tag] /desk/ is the desktop page with its base and bridge added" "status $($r.Status)"
    $all = $phone -and $desk
    $want = @(
      @("/app.js$t", 'renderer\phone\app.js', 'text/javascript*'), @("/style.css$t", 'renderer\phone\style.css', 'text/css*'), @("/sw.js$t", 'renderer\phone\sw.js', 'text/javascript*'),
      @("/vendor/xterm.js$t", 'node_modules\@xterm\xterm\lib\xterm.js', 'text/javascript*'), @("/vendor/xterm.css$t", 'node_modules\@xterm\xterm\css\xterm.css', 'text/css*'),
      @("/vendor/addon-fit.js$t", 'node_modules\@xterm\addon-fit\lib\addon-fit.js', 'text/javascript*'),
      @("/desk/renderer/style.css$t", 'renderer\style.css', 'text/css*'), @("/desk/renderer/js/90-boot.js$t", 'renderer\js\90-boot.js', 'text/javascript*'),
      @("/desk/renderer/desk/bridge.js$t", 'renderer\desk\bridge.js', 'text/javascript*'), @("/desk/lib/layout.js$t", 'lib\layout.js', 'text/javascript*'),
      @("/desk/node_modules/@xterm/xterm/lib/xterm.js$t", 'node_modules\@xterm\xterm\lib\xterm.js', 'text/javascript*'),
      @("/desk/assets/fonts/fonts.css$t", 'assets\fonts\fonts.css', 'text/css*'),
      @("/desk/assets/fonts/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf$t", 'assets\fonts\jetbrainsmono\JetBrainsMono[wght].ttf', 'font/ttf'),
      @('/manifest.webmanifest', 'renderer\phone\manifest.webmanifest', 'application/manifest+json'), @('/icon-192.png', 'renderer\phone\icon-192.png', 'image/png'), @('/icon-512.png', 'renderer\phone\icon-512.png', 'image/png'))
    foreach ($w in $want) {
      $r = Fetch $w[0]
      $ok = $r.Status -eq 200 -and $r.Type -like $w[2] -and $r.Length -eq (& $size $w[1])
      if (-not $ok) { $all = $false }
      $shown = ($w[0] -replace '\?t=.*$', '?t=...')
      Check $ok "[$tag] $shown  ($($w[2]), $($r.Length) bytes)" "status $($r.Status), type $($r.Type), $($r.Length) bytes, the file has $(& $size $w[1])"
    }
    # every script and stylesheet the desk page names must be served (the page would otherwise load half)
    $missing = @()
    foreach ($m in [regex]::Matches($deskText, '(?:src|href)="([^"#?:]+)"')) {
      $u = New-Object System.Uri((New-Object System.Uri('http://127.0.0.1:' + $Port + '/desk/renderer/')), $m.Groups[1].Value)
      $r = Fetch ($u.AbsolutePath + $t); if ($r.Status -ne 200) { $missing += "$($u.AbsolutePath) -> $($r.Status)" }
    }
    Check (-not $missing.Count -and $deskText) "[$tag] every file the desk page names is served" ($missing -join '; ')
    if ($missing.Count) { $all = $false }
    $r = Fetch "/desk/main.js$t"; Check ($r.Status -eq 404) "[$tag] /desk/main.js is 404 (not shipped, not allowed)" "status $($r.Status)"
    $r = Fetch "/desk/install.ps1$t"; Check ($r.Status -eq 404) "[$tag] /desk/install.ps1 is 404 (a packaged script is not a page)" "status $($r.Status)"
    $r = Fetch "/..%2Finstall.ps1$t"; Check ($r.Status -ne 200) "[$tag] the phone route cannot climb out to the package's scripts" "status $($r.Status)"
    $res.Assets = $all
  } finally {
    if (-not $p.WaitForExit(40000)) { Check $false "[$tag] the exe exited by itself"; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  }
  Check ($p.ExitCode -eq 0) "[$tag] exit code 0" "exit $($p.ExitCode)"
  $log = ''; if (Test-Path -LiteralPath "$shot.log.txt") { $log = Get-Content -LiteralPath "$shot.log.txt" -Raw }
  $fontLine = (($log -split "`n") | Where-Object { $_ -match '^bundled fonts:' } | Select-Object -First 1)
  # since 255673b the fonts are inside the exe (RCDATA) and the folder is the fallback: either is relocatable
  $res.Fonts = [bool]($fontLine -and $fontLine -match 'bundled fonts: (\d+) families from' -and [int]$Matches[1] -ge 7 -and ($fontLine -match 'families from inside the exe' -or $fontLine.ToLowerInvariant().Contains((Join-Path $dest 'assets\fonts').ToLowerInvariant())))
  Check $res.Fonts "[$tag] the exe loaded the bundled fonts from inside itself or from the installed assets\fonts" ('' + $fontLine).Trim()
  Start-Sleep -Milliseconds 500
  Check (-not (Listening $Port)) "[$tag] port $Port is free again"
  $left = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($p.Id)" | Where-Object { $_.CreationDate -ge $started })
  Check (-not $left.Count) "[$tag] no child process is left" (($left | ForEach-Object { $_.Name }) -join ' ')
  $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Check (-not @([IO.Directory]::GetFiles($dest, '*', 'AllDirectories') | Where-Object { $_.Substring($dest.Length + 1) -notin ($rel + 'install-manifest.json') }).Count) "[$tag] the run wrote nothing into the program folder"
  return $res
}
Write-Host '--- 3. the installed exe, headless, with NO --assets and NO --fonts-dir (it must find both beside itself)'
$a = Run-Served 'beside' $false
Check $a.Assets 'RELOCATABLE: the served pages are found beside the exe without --assets'
Check $a.Fonts 'RELOCATABLE: the fonts are found beside the exe without --fonts-dir'
if (-not ($a.Assets -and $a.Fonts)) {
  Write-Host '--- 3b. the same with --assets and --fonts-dir given (the exe did not find them by itself: PACKAGING.md, requests)'
  $b = Run-Served 'explicit' $true
  Check ($b.Assets -and $b.Fonts) 'with the explicit switches the package serves and loads its fonts'
}
Check (@([IO.Directory]::GetFiles($fakeStart)).Count -eq 1 -and @([IO.Directory]::GetFiles($fakeStartup)).Count -eq 1) 'a test run of the exe made no shortcut of its own (the fake folders still hold one file each)'

# ---- 3c. updating a RUNNING deck: the exe moved aside, the update offered, the leftover removed at the next start
Write-Host '--- 3c. install.ps1 over a running deck (rename-aside), the update offer, the .old removed at the next start'
$upd = Join-Path $Work 'update-package'
Copy-Item -LiteralPath $pkg -Destination $upd -Recurse
(Get-Item -LiteralPath (Join-Path $upd $DeckEdition.Exe)).LastWriteTime = (Get-Date)       # "a newer build": the same bytes with another time stamp
$stateU = Join-Path $Work 'state-update'; $homeU = Join-Path $Work 'home-update'; New-Item -ItemType Directory -Force -Path $stateU, $homeU | Out-Null
$shotU = Join-Path $Work 'update-running.png'
$env:CLAUDE_DECK_LINK = $null
$pU = Start-Process -FilePath $destExe -PassThru -ArgumentList "--local --state-dir `"$stateU`" --home `"$homeU`" --no-claude --shell cmd.exe --shot `"$shotU`" --after 500 --size 1000x700 --do `"wait 3000; dump restart; wait 36000; dump restart`""
$null = $pU.Handle
try {
  Start-Sleep -Milliseconds 1500
  Check (Test-FileLocked $destExe) 'the installed exe is running (its file is locked)'
  $instU = Invoke-Child @((Join-Path $upd 'install.ps1'), '-Destination', $dest, '-StartMenuDir', $fakeStart, '-StartupDir', $fakeStartup)
  Check ($script:code -eq 0) 'install.ps1 updates over the RUNNING deck (exit 0)' ($instU -join ' / ')
  Check (($instU -join ' ') -match 'the deck is running: it will offer the restart') 'it says the running deck will offer the restart'
  Check (Test-Path -LiteralPath "$destExe.old") 'the running exe was moved aside (ClaudeDeckNative.exe.old)'
  Check ((Test-Path -LiteralPath $destExe) -and -not (Test-FileLocked $destExe)) 'the new exe is in place under the real name, and it is not the running one'
  Check (-not $pU.HasExited) 'the deck kept running through the update'
  Check (@((Get-Content -LiteralPath (Join-Path $dest 'install-manifest.json') -Raw | ConvertFrom-Json).files) -notcontains ($DeckEdition.Exe + '.old')) 'the manifest does not list the .old'
} finally {
  if (-not $pU.WaitForExit(60000)) { try { $pU.Kill() } catch { } ; Check $false 'the running deck ended by itself' }
}
$logU = if (Test-Path -LiteralPath "$shotU.log.txt") { Get-Content -LiteralPath "$shotU.log.txt" -Raw } else { '' }
Check ($logU -match ('(?s)restart \{[^\n]*"update":null.*restart \{[^\n]*"update":\{[^}]*' + [regex]::Escape($DeckEdition.Exe))) 'the running deck saw the new exe and offers the update (meta.restart.update names ClaudeDeckNative.exe)' ($logU -replace '\s+', ' ')
$shotV = Join-Path $Work 'update-next.png'
$pV = Start-Process -FilePath $destExe -PassThru -Wait -ArgumentList "--local --state-dir `"$stateU`" --home `"$homeU`" --no-claude --shell cmd.exe --shot `"$shotV`" --after 400 --size 1000x700"
Check ($pV.ExitCode -eq 0 -and -not (Test-Path -LiteralPath "$destExe.old")) 'the next start of the deck removed ClaudeDeckNative.exe.old'
Check (@([IO.Directory]::GetFiles($dest, '*', 'AllDirectories')).Count -eq $rel.Count + 1) 'the program folder holds the package and its manifest again, nothing else'

# ---- 4. the import, on synthetic files only
Write-Host '--- 4. import-from-electron.ps1 on synthetic state'
$imp = Join-Path $PSScriptRoot 'import-from-electron.ps1'
$src = Join-Path $Work 'fake-electron'; $dst = Join-Path $Work 'fake-native\local'
New-Item -ItemType Directory -Force -Path $src, (Join-Path $src 'uploads'), (Join-Path $src 'tiles-backups') | Out-Null
$utf8 = New-Object Text.UTF8Encoding $false
# main.js saveState's own tile shape; asleep, so that a deck booting on it starts nothing even if it could
$tilesText = '{"tiles":[{"id":"t1","kind":"claude","account":"claudev","sessionId":"00000000-0000-4000-8000-000000000001","cwd":"C:\\nowhere","pinned":false,"group":null,"createdAt":1760000000000,"title":"synthetic \u00e9 one","userTitle":null,"asleep":true,"sleptAt":1760000001000,"browser":null}],"groups":[],"windows":[{"bounds":{"x":10,"y":20,"width":1600,"height":960,"maximized":false},"layout":{"t1":[0,1]}}],"deskLayout":{}}'
$settingsText = "{`n  `"fontSize`": 14,`n  `"grid`": { `"cols`": 2, `"rows`": 2 },`n  `"phone`": { `"enabled`": true, `"port`": 8815, `"bind`": `"localhost`", `"token`": `"syntheticsynthet`" },`n  `"enabled`": true`n}`n"
[IO.File]::WriteAllText((Join-Path $src 'tiles.json'), $tilesText, $utf8)
[IO.File]::WriteAllText((Join-Path $src 'settings.json'), $settingsText, $utf8)
[IO.File]::WriteAllText((Join-Path $src 'triggers.json'), '{"triggers":{}}', $utf8)
[IO.File]::WriteAllText((Join-Path $src 'relaunch.json'), '{"command":"wscript.exe","args":["electron-launcher.vbs"]}', $utf8)
[IO.File]::WriteAllText((Join-Path $src 'uploads\x.txt'), 'x', $utf8)
function Tree-Sig([string]$dir) { return (@([IO.Directory]::GetFiles($dir, '*', 'AllDirectories') | Sort-Object | ForEach-Object { "$_|$((Get-Item -LiteralPath $_).Length)|$((Get-Item -LiteralPath $_).LastWriteTimeUtc.Ticks)|$((Get-FileHash -LiteralPath $_).Hash)" }) -join "`n") }
$srcBefore = Tree-Sig $src
function Run-Import([string[]]$more) { $o = Invoke-Child (@($imp, '-Source', $src, '-Destination', $dst) + $more); return [pscustomobject]@{ Code = $script:code; Out = ($o -join ' / ') } }
$r = Run-Import @('-DryRun'); Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath $dst)) 'a dry run writes nothing' $r.Out
$r = Run-Import @(); Check ($r.Code -eq 0) 'the import ran' $r.Out
Check ((Get-FileHash -LiteralPath (Join-Path $dst 'tiles.json')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $src 'tiles.json')).Hash) 'tiles.json arrives byte for byte'
Check ((Get-FileHash -LiteralPath (Join-Path $dst 'settings.json')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $src 'settings.json')).Hash) 'settings.json arrives byte for byte (the phone settings and token as they were)'
Check ((Test-Path -LiteralPath (Join-Path $dst 'triggers.json')) -and -not (Test-Path -LiteralPath (Join-Path $dst 'relaunch.json')) -and -not (Test-Path -LiteralPath (Join-Path $dst 'uploads'))) 'triggers.json came over; relaunch.json and uploads\ did not'
Check (Test-Path -LiteralPath (Join-Path $dst 'imported-from-electron.json')) 'a receipt says what was imported'
# the native deck itself, headless, on the imported files: it must read an Electron-shaped tiles.json (the port in
# the synthetic settings is a test port, and --serve forces another: never the live deck's)
$bootShot = Join-Path $Work 'imported-boot.png'; $bootHome = Join-Path $Work 'home-import'; New-Item -ItemType Directory -Force -Path $bootHome | Out-Null
# the account the tile names must be one the deck knows: accounts come from the alias file in the (temp) home
[IO.File]::WriteAllText((Join-Path $bootHome '.claude-code-account-aliases'), ('alias claudev=''CLAUDE_CONFIG_DIR="$HOME/.claude-vv" claude''' + "`n"), $utf8)
$bootExe = Join-Path $dest $DeckEdition.Exe
$bp = Start-Process -FilePath $bootExe -PassThru -ArgumentList "--local --serve 8814 --state-dir `"$(Split-Path -Parent $dst)`" --home `"$bootHome`" --no-claude --shell cmd.exe --shot `"$bootShot`" --after 500 --size 1200x800 --do `"wait 2500; dump tiles`""
$null = $bp.Handle
if (-not $bp.WaitForExit(40000)) { Stop-Process -Id $bp.Id -Force -ErrorAction SilentlyContinue }
$bootLog = ''; if (Test-Path -LiteralPath "$bootShot.log.txt") { $bootLog = Get-Content -LiteralPath "$bootShot.log.txt" -Raw }
Check ($bp.ExitCode -eq 0 -and $bootLog -match 'synthetic') 'the native deck boots on the imported state and lists the imported tile (Claude off, nothing started)' "exit $($bp.ExitCode); log: $(($bootLog -split "`n" | Select-Object -Last 6) -join ' | ')"
Check ((Tree-Sig $src) -eq $srcBefore) 'and the source is still as it was'
$r = Run-Import @(); Check ($r.Code -ne 0 -and $r.Out -match 'exists already') 'a second import refuses: the destination has state' $r.Out
[IO.File]::WriteAllText((Join-Path $dst 'tiles.json'), '{"tiles":[],"marker":"native own"}', $utf8)
$r = Run-Import @('-Force', '-PhoneOff'); Check ($r.Code -eq 0) 'with -Force -PhoneOff it runs' $r.Out
$bk = @([IO.Directory]::GetDirectories($dst, 'import-backup-*'))
Check ($bk.Count -eq 1 -and (Get-Content -LiteralPath (Join-Path $bk[0] 'tiles.json') -Raw) -match 'native own') '-Force first moves what was there to import-backup-<time>'
$s2 = Get-Content -LiteralPath (Join-Path $dst 'settings.json') -Raw
$j2 = $s2 | ConvertFrom-Json
Check ($j2.phone.enabled -eq $false -and $j2.enabled -eq $true -and $j2.phone.port -eq 8815 -and $j2.phone.token -eq 'syntheticsynthet' -and $s2.Length -eq $settingsText.Length + 1) '-PhoneOff changes phone.enabled and nothing else (port, token and the other "enabled" stay)'
$r = Run-Import @('-Force', '-Files', 'relaunch.json'); Check ($r.Code -ne 0) 'relaunch.json is refused even when asked for by name' $r.Out
[IO.File]::WriteAllText((Join-Path $src 'held-prompts.json'), '{ this is not json', $utf8)
$srcBefore = Tree-Sig $src
$dstBefore = Tree-Sig $dst
$r = Run-Import @('-Force'); Check ($r.Code -ne 0 -and (Tree-Sig $dst) -eq $dstBefore) 'one unreadable file stops the whole import: nothing is copied' $r.Out
Remove-Item -LiteralPath (Join-Path $src 'held-prompts.json') -Force
[IO.File]::WriteAllText((Join-Path $src 'running.json'), ('{"pid":' + $PID + ',"at":"2026-01-01T00:00:00.000Z"}'), $utf8)
$srcBefore = Tree-Sig $src
$r = Run-Import @('-Force'); Check ($r.Code -ne 0 -and $r.Out -match 'running') 'it refuses while the source deck runs (running.json names a live process)' $r.Out
Check ((Tree-Sig $src) -eq $srcBefore) 'the source folder is unchanged after every import (names, sizes, times, hashes)'
$null = Invoke-Child @($imp, '-Source', $src, '-Destination', (Join-Path $src 'inner'))
Check ($script:code -ne 0 -and -not (Test-Path -LiteralPath (Join-Path $src 'inner'))) 'a destination inside the source is refused'

# ---- 5. uninstall
Write-Host '--- 5. uninstall.ps1'
$stateKept = Join-Path $Work 'state-beside\local\settings.json'
[IO.File]::WriteAllText((Join-Path $dest 'my-notes.txt'), 'not the installer''s', $utf8)
$un = Invoke-Child @((Join-Path $dest 'uninstall.ps1'))
$un | ForEach-Object { Write-Host "  $_" }
Check ($script:code -eq 0) 'uninstall.ps1 ran (from the installed folder, finding its own manifest)'
Check (-not @([IO.Directory]::GetFiles($fakeStart)).Count -and -not @([IO.Directory]::GetFiles($fakeStartup)).Count) 'the fake Start menu and the fake Startup folder are empty'
$leftFiles = @(); if (Test-Path -LiteralPath $dest) { $leftFiles = @([IO.Directory]::GetFileSystemEntries($dest) | ForEach-Object { Split-Path -Leaf $_ }) }
Check (($leftFiles -join ',') -eq 'my-notes.txt') 'the program folder holds only the file the installer did not put there' ($leftFiles -join ', ')
Check ([bool]($un | Where-Object { $_ -match 'kept\s+your state' })) 'it says the state folder is kept'
Check (Test-Path -LiteralPath $stateKept) 'the (temp) state folder of the runs is untouched'
# once more, clean: install without auto-start, uninstall with -Destination, nothing at all stays
Remove-Item -LiteralPath $dest -Recurse -Force
$null = Invoke-Child @((Join-Path $pkg 'install.ps1'), '-Destination', $dest, '-StartMenuDir', $fakeStart, '-StartupDir', $fakeStartup, '-Mode', 'ask')
$askLnk = Join-Path $fakeStart $srcName
$askOk = (Test-Path -LiteralPath $askLnk) -and ((Read-DeckShortcut $askLnk).Arguments -eq '') -and -not @([IO.Directory]::GetFiles($fakeStartup)).Count
Check $askOk 'install -Mode ask without -AutoStart: a shortcut with no argument, nothing in the Startup folder'
$null = Invoke-Child @((Join-Path $pkg 'uninstall.ps1'), '-Destination', $dest)
Check ($script:code -eq 0 -and -not (Test-Path -LiteralPath $dest) -and -not @([IO.Directory]::GetFiles($fakeStart)).Count) 'uninstall -Destination: the program folder and the shortcut are gone'
$null = Invoke-Child @((Join-Path $pkg 'uninstall.ps1'), '-Destination', $dest)
Check ($script:code -ne 0) 'uninstalling what is not installed fails and removes nothing'

# ---- 6. the real locations, after
Write-Host '--- 6. the real locations'
$after = Real-State
$changed = @(Compare-Object ($before -split "`n") ($after -split "`n") | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" })
Check (-not $changed.Count) 'the real Start menu, Startup folder, %LOCALAPPDATA%\Programs, HKCU Run and %APPDATA%\claude-deck-native are as they were' ($changed -join '; ')
Check (-not (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA ('Programs\' + $DeckEdition.ProgramsDir)))) 'nothing is installed under %LOCALAPPDATA%\Programs'
Check (-not @(Get-Process -Name $DeckEdition.ExeBase -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) }).Count) 'no exe of this test is still running'

if (-not $Keep) { Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue } else { Write-Host "kept: $Work" }
if ($script:fail) { Write-Host "`nPACKAGE TESTS: $($script:fail) check(s) failed"; exit 1 } else { Write-Host "`nPACKAGE TESTS: all checks passed"; exit 0 }
