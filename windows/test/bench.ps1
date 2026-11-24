# windows/test/bench.ps1 — what a frame costs: ClaudeDeckNative.exe --bench (no deck, no socket, nothing on screen).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/bench.ps1 [-Name bench] [-Frames 200] [-Size 1400x900] [-Dpi 0]
# Draws the sixty-tile fixture in the sidebar and four panes of text, case by case, on the off-screen bitmap and on
# the real window target (a window parked off every screen, never activated). Writes windows/test/out/<Name>.txt.
param([string]$Name = 'bench', [int]$Frames = 200, [string]$Size = '1400x900', [int]$Dpi = 0, [string]$Fixture = 'meta-sixty.json')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = Join-Path $root ('windows\build\' + $DeckEdition.Exe)
$out = Join-Path $root "windows\test\out\$Name.txt"
Remove-Item $out -ErrorAction SilentlyContinue
$env:CLAUDE_DECK_LINK = $null
$a = "--no-save --bench `"$out`" --fixture `"$(Join-Path $root "windows\test\fixtures\$Fixture")`" --frames $Frames --size $Size"
if ($Dpi) { $a += " --dpi $Dpi" }
$p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
"exit $($p.ExitCode)"
if (Test-Path $out) { Get-Content $out }
