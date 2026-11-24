# windows/test/shot.ps1 — one headless run of ClaudeDeckNative.exe against the SCRATCH deck (never the live one).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/shot.ps1 -Name confirm -Do "zone tile-close; wait 200" [-Size 1400x900] [-Select "Power"] [-Place] [-After 2500]
# Writes windows/test/out/n-<Name>.png (and n-<Name>.png.log.txt when the script logs). Nothing appears on screen:
# --shot never shows a window. The link comes from the environment, so the token is never on a command line.
param([Parameter(Mandatory = $true)][string]$Name, [string]$Do = '', [string]$Size = '1400x900', [string]$Select = '', [switch]$Place, [int]$After = 2500,
  [string]$Link = 'http://127.0.0.1:8799/?t=nativetest00000001', [int]$Dpi = 0, [switch]$NoScene, [string]$Exe = '')
$ErrorActionPreference = 'Stop'
if ($Link -notmatch '^http://127\.0\.0\.1:8799/') { throw 'shot.ps1 talks to the scratch deck only (127.0.0.1:8799)' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
# (PowerShell names are case-blind: $exe IS the -Exe parameter)
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$out = Join-Path $root "windows\test\out\n-$Name.png"
Remove-Item $out, "$out.error.txt", "$out.log.txt" -ErrorAction SilentlyContinue
$env:CLAUDE_DECK_LINK = $Link
$a = "--no-save --verify-scene --shot `"$out`" --after $After --size $Size"
if ($Dpi) { $a += " --dpi $Dpi" }
if ($NoScene) { $a += ' --no-scene' }
if ($Select) { $a += " --select `"$Select`"" }
if ($Place) { $a += ' --place' }
if ($Do) { $a += " --do `"$($Do -replace '"', '\"')`"" }
$p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
"exit $($p.ExitCode)  $out"
if (Test-Path "$out.error.txt") { Get-Content "$out.error.txt" }
if (Test-Path "$out.log.txt") { Get-Content "$out.log.txt" }
