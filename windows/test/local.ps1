# windows/test/local.ps1 — one headless run of ClaudeDeckNative.exe in LOCAL mode (its own in-process deck).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/local.ps1 -Name l1 -Do "zone new-tile; wait 1500; text echo hi; key enter; wait 600; dump screen"
#     [-StateDir <folder>] [-Keep] [-PersistShells] [-Shell cmd.exe] [-Size 1400x900] [-After 2600] [-Dpi 144]
#     [-DefaultShell]  the deck's own default terminal (PowerShell -NoLogo, with the user's profile) instead of cmd.exe
# Writes windows/test/out/n-<Name>.png (+ .log.txt). Nothing appears on screen: --shot never shows a window, and the
# shells the local deck starts run in pseudoconsoles (no window). The deck's state goes to a TEMP folder (never
# %APPDATA%\claude-deck-native): a fresh one per run unless -StateDir names one (then -Keep is implied), so two runs
# on one folder are a restart. Never starts Claude: the exe has no switch for it. At the end the script checks that
# no console child of the exe is left running and says so.
param([Parameter(Mandatory = $true)][string]$Name, [string]$Do = '', [string]$Size = '1400x900', [int]$After = 2600,
  [string]$StateDir = '', [switch]$Keep, [switch]$PersistShells, [string]$Shell = 'cmd.exe', [int]$Dpi = 0, [switch]$WindowShot, [string]$Exe = '', [switch]$NoScene, [switch]$DefaultShell, [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
# (PowerShell names are case-blind: $exe IS the -Exe parameter)
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$out = Join-Path $root "windows\test\out\n-$Name.png"
Remove-Item $out, "$out.error.txt", "$out.log.txt" -ErrorAction SilentlyContinue
$own = -not $StateDir
if ($own) { $StateDir = Join-Path $env:TEMP ("cdn-local-" + [guid]::NewGuid().ToString('N').Substring(0, 10)) }
if ($StateDir -match 'AppData\\Roaming') { throw 'local.ps1 keeps its state in a temp folder, never under %APPDATA%' }
New-Item -ItemType Directory -Force $StateDir | Out-Null
$env:CLAUDE_DECK_LINK = $null
$a = "--local --verify-scene --state-dir `"$StateDir`" --after $After --size $Size"
if ($WindowShot) { $a += " --window-shot `"$out`"" } else { $a += " --shot `"$out`"" }
if ($NoScene) { $a += ' --no-scene' }
if ($Shell -and -not $DefaultShell) { $a += " --shell `"$Shell`"" }
if ($PersistShells) { $a += ' --persist-shells' }
if ($Dpi) { $a += " --dpi $Dpi" }
if ($Extra) { $a += " $Extra" }     # more options for the exe (--fonts-dir "<folder>" …)
if ($Do) { $a += " --do `"$($Do -replace '"', '\"')`"" }
$p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
"exit $($p.ExitCode)  $out"
if (Test-Path "$out.error.txt") { Get-Content "$out.error.txt" }
if (Test-Path "$out.log.txt") { Get-Content "$out.log.txt" }
# nothing the exe started may outlive it
Start-Sleep -Milliseconds 300
$left = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($p.Id)" | Where-Object { $_.Name -match '^(cmd|powershell|pwsh|conhost|OpenConsole)\.exe$' -and $_.CreationDate -ge $p.StartTime })
if ($left.Count) { "LEFTOVER CHILDREN: " + (($left | ForEach-Object { "$($_.Name)#$($_.ProcessId)" }) -join ' '); $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } }
else { 'no leftover children' }
if ($own -and -not $Keep) { Remove-Item -Recurse -Force $StateDir -ErrorAction SilentlyContinue } else { "state: $StateDir" }
