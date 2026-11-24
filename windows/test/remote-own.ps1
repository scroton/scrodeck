# windows/test/remote-own.ps1 — a REMOTE keeps to itself (the remote-desk rule, 2026-11-18), checked on the SCRATCH deck.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/remote-own.ps1
# 1. The scratch deck's own window places a shell tile (placeTile(id, [1]) through its CDP port 9777).
# 2. The native exe (headless, remote, its config in a temp --state-dir) clicks that tile (it must show) and places
#    it in its own grid (Alt+2).
# 3. Asserts: the scratch deck's window STILL has the tile placed; the exe kept its layout to itself (its log says
#    so; App::send drops `layout` and `leaving` frames in remote mode); the layout is in its config.
#    (Size is NOT part of this: who drives a pane's size follows the deck's driver setting, as before.)
# 4. The exe runs again on the same state dir: its layout comes back.
# Nothing appears on screen.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = Join-Path $root ('windows\build\' + $DeckEdition.Exe)
$cdp = Join-Path $PSScriptRoot 'cdp-eval.js'
function Deck([string]$expr) { (& node $cdp $expr) }
$state = Join-Path $env:TEMP ("cdn-own-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force $state | Out-Null
$env:CLAUDE_DECK_LINK = 'http://127.0.0.1:8799/?t=nativetest00000001'
$fail = @()

$id = (Deck "state.tiles.find(t => t.title === 'native test one').id") -replace '"', ''
"tile: $id"
Deck "(clearLayout(), placeTile('$id', [1]), isPlaced('$id'))" | ForEach-Object { "deck window placed it: $_" }
Start-Sleep -Milliseconds 1500

function Run([string]$name, [string]$do) {
  $out = Join-Path $root "windows\test\out\n-$name.png"
  Remove-Item $out, "$out.log.txt", "$out.error.txt" -ErrorAction SilentlyContinue
  $a = "--verify-scene --state-dir `"$state`" --shot `"$out`" --after 2500 --size 1400x900 --do `"$do`""
  $p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
  "run $name exit $($p.ExitCode)"
  if (Test-Path "$out.log.txt") { Get-Content "$out.log.txt" } else { @() }
}
$log1 = Run 'own1' "zone tile 'title:native test one'; wait 600; dump layout; shot own1-clicked; tap alt+2; wait 800; dump layout; shot own1-placed; dump state"
$log1 | Where-Object { $_ -match '^(run|own layout|layout |resize|nudge|term|SCENE|scene)' }
$still = Deck "isPlaced('$id')"
"deck window still has it placed: $still "
if ($still -ne 'true') { $fail += 'the deck window lost the tile from its layout' }
if (-not ($log1 | Where-Object { $_ -match "^layout .*$id.*shown .*$id" })) { $fail += 'the exe did not show / place the tile' }
if (-not ($log1 | Where-Object { $_ -match '^own layout kept \(not sent\)' })) { $fail += 'the exe did not keep its own layout' }
$cfg = Get-Content (Join-Path $state 'config.json') -Raw | ConvertFrom-Json
"config layouts: " + ($cfg.layouts | ConvertTo-Json -Compress -Depth 5)
$log2 = Run 'own2' "wait 300; dump layout"
$log2 | Where-Object { $_ -match '^(own layout|layout )' }
if (-not ($log2 | Where-Object { $_ -match "^layout \{.*$id" })) { $fail += 'the layout did not come back from the state dir' }
Deck "(clearLayout(), 'cleared')" | Out-Null
Remove-Item -Recurse -Force $state -ErrorAction SilentlyContinue
if ($fail.Count) { 'VERDICT: FAIL - ' + ($fail -join '; ') } else { 'VERDICT: ok - shown and placed here, untouched there; no layout frame left this machine; the layout came back' }
