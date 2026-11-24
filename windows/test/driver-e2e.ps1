# windows/test/driver-e2e.ps1 — the driver CLAIM end to end: the native exe as the remote desk of a REAL deck of
# branch native-remote-server — a SCRATCH one, started by that branch's own launcher. Never the live deck.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/driver-e2e.ps1
#     [-ClaimRoot <checkout of the branch>] [-Dir <the scratch deck's folder>] [-KeepDeck]
# 1. Starts the branch's scratch deck (test/remote-rule/scratch-deck.ps1: CDP 9778, phone server 127.0.0.1:8798,
#    its own user-data-dir) unless it runs already; checks its window is minimised. Makes ONE shell tile there.
# 2. The exe (headless, a temp --state-dir, `--desk-name E2EBOX`) shows that tile and clicks the driver button: the claim.
#    While it holds the claim, the deck's OWN window is read over CDP: its button must be the remote icon, grey
#    (`drive-other`), its title "Sizes follow: the remote desk (E2EBOX) …"; the tile's ptySize must be the exe's pane.
# 3. The exe clicks again: released to auto. The tile is closed, the driver put back, the deck stopped (unless
#    it was running before, or -KeepDeck).
# Shell tiles only; nothing is adopted; Settings is never saved. Nothing appears on screen.
param([string]$ClaimRoot = '', [string]$Dir = 'D:\scratchdir\26-11-18 DECK NATIVE CPP\scratch-deck-rule', [switch]$KeepDeck)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
if (-not $ClaimRoot) { $ClaimRoot = Join-Path (Split-Path -Parent $root) '_wt-deck-remote-rule' }
$launcher = Join-Path $ClaimRoot 'test\remote-rule\scratch-deck.ps1'
if (-not (Test-Path $launcher)) { "no claim checkout at $ClaimRoot"; exit 2 }
$exe = Join-Path $root ('windows\build\' + $DeckEdition.Exe)
$outDir = Join-Path $root 'windows\test\out'
$cdpJs = Join-Path $PSScriptRoot 'cdp-eval.js'
$env:CDP_PORT = '9778'
function Deck([string]$expr) { (& node $cdpJs $expr) }
$script:fail = @(); $script:pass = 0
function Check([bool]$ok, [string]$what) { if ($ok) { $script:pass++; "  ok   $what" } else { $script:fail += $what; "  FAIL $what" } }
function Launcher([string]$action) { & powershell -NoProfile -ExecutionPolicy Bypass -File $launcher $action -Dir $Dir }

$status = Launcher 'status' 6>&1 | Out-String
$wasRunning = $status -match '^running'
if (-not $wasRunning) { (Launcher 'start' 6>&1 | Out-String).Trim() }
$id = ''; $driverWas = 'auto'
try {
  $up = $false
  for ($i = 0; $i -lt 80 -and -not $up; $i++) { $v = Deck "typeof state === 'object' && Array.isArray(state.tiles) && typeof paintDrive === 'function' && typeof driveTitle === 'function'"; if ($v -eq 'true') { $up = $true } else { Start-Sleep -Milliseconds 500 } }
  Check $up 'the scratch deck of the branch is up (CDP 9778) and its renderer speaks the claim'
  if (-not $up) { throw 'no deck' }
  Add-Type -Namespace DrvE2E -Name Win -MemberDefinition '[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);'
  $pidFile = Join-Path $Dir 'deck.pid'
  $dp = Get-Process -Id ([int](Get-Content $pidFile -TotalCount 1)) -ErrorAction SilentlyContinue
  Check ($dp -and $dp.MainWindowHandle -ne [IntPtr]::Zero -and [DrvE2E.Win]::IsIconic($dp.MainWindowHandle)) 'its window is minimised'
  $driverWas = (Deck 'state.settings.driver') -replace '"', ''
  Deck "deck.setDriver('auto').then(() => true)" | Out-Null
  $before = Deck "state.tiles.map(t => t.id).join(',')"
  Deck "deck.newTile({ kind: 'shell' }).then(() => true)" | Out-Null
  for ($i = 0; $i -lt 40 -and -not $id; $i++) { Start-Sleep -Milliseconds 400; $now = ((Deck "state.tiles.map(t => t.id).join(',')") -replace '"', '') -split ','; $id = $now | Where-Object { $_ -and (($before -replace '"', '') -split ',') -notcontains $_ } | Select-Object -First 1 }
  Check ([bool]$id) "a shell tile was made in the deck window ($id)"
  if (-not $id) { throw 'no tile' }
  Deck "deck.renameTile('$id', 'claim e2e').then(() => true)" | Out-Null
  Deck "(clearLayout(), activate('$id'), true)" | Out-Null
  Start-Sleep -Milliseconds 1500
  "office before: " + (Deck "(() => { const t = state.tiles.find(t => t.id === '$id'); return JSON.stringify({ pty: t.ptySize, mirror: t.mirror, sizeBy: t.sizeBy, kind: t.kind }); })()")

  # while the exe holds the claim: the deck's own window, read from inside its page (it waits for the claim itself)
  $probe = Join-Path $env:TEMP ("cdn-e2e-probe-" + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.txt')
  $expr = "new Promise(res => { const t0 = Date.now(); const iv = setInterval(() => { const on = state.settings.driver === 'remote' && state.remoteDesk; if (on || Date.now() - t0 > 40000) { clearInterval(iv); setTimeout(() => { const b = document.querySelector('#drive-btn'); const t = state.tiles.find(t => t.id === '$id') || {}; res(JSON.stringify({ claimed: !!on, who: b.dataset.driver, cls: [...b.classList].filter(c => /^drive-/.test(c)).join(' '), title: b.title, remoteDesk: state.remoteDesk, pty: t.ptySize || null, mirror: t.mirror || null, sizeBy: t.sizeBy || null })); }, 2500); } }, 200); })"
  $job = Start-Job -ScriptBlock { param($js, $e, $f) $env:CDP_PORT = '9778'; (& node $js $e) | Set-Content -Path $f -Encoding utf8 } -ArgumentList $cdpJs, $expr, $probe

  $state = Join-Path $env:TEMP ("cdn-e2e-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
  New-Item -ItemType Directory -Force $state | Out-Null
  $out = Join-Path $outDir 'n-drv-e2e.png'
  Remove-Item $out, "$out.log.txt", "$out.error.txt" -ErrorAction SilentlyContinue
  $env:CLAUDE_DECK_LINK = 'http://127.0.0.1:8798/?t=remoterule0000001'
  $do = "zone tile 'title:claim e2e'; wait 1500; mark-a; dump drive; shot drv-e2e-auto; zone tool drive-btn; wait 6000; mark-b; dump drive; shot drv-e2e-claimed; zone tool drive-btn; wait 1500; mark-c; dump drive; shot drv-e2e-released"
  $a = "--no-save --state-dir `"$state`" --shot `"$out`" --after 2500 --size 1400x900 --desk-name E2EBOX --do `"$do`""
  $p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
  "exe exit $($p.ExitCode)"
  $log = if (Test-Path "$out.log.txt") { @(Get-Content "$out.log.txt") } else { @() }
  $log | Where-Object { $_ -match '^(drive |call setDriver|resize|deskInit|UNKNOWN STEP: mark|NO |BAD|BLOCKED)' }
  Wait-Job $job -Timeout 60 | Out-Null; Remove-Job $job -Force
  $raw = if (Test-Path $probe) { (Get-Content $probe -Raw).Trim() } else { '' }
  Remove-Item $probe -ErrorAction SilentlyContinue
  "deck window while claimed: $raw"
  $o = $null; try { $o = ($raw | ConvertFrom-Json) | ConvertFrom-Json } catch {}
  function Seg([string]$m) { $i = [array]::IndexOf($log, "UNKNOWN STEP: mark-$m"); if ($i -lt 0) { return @() } @($log[($i + 1)..($log.Count - 1)] | Select-Object -First 4) }
  $b = Seg 'b'
  Check ($p.ExitCode -eq 0) 'the exe ran to the end'
  Check ([bool](Seg 'a' | Where-Object { $_ -match '^drive words new driver auto look auto remoteDesk \{"name":"E2EBOX"\}' })) 'exe: a new deck, auto, and its own name back in meta.remoteDesk'
  Check ([bool]($log | Where-Object { $_ -match '^call setDriver \["remote"\]' })) 'exe: the click claimed (setDriver remote)'
  Check ([bool]($b | Where-Object { $_ -match '^drive words new driver remote look mine' })) 'exe: the claim shows green (look mine)'
  $tile = $b | Where-Object { $_ -match '^drive tile \S+ sizeBy "remote" mirror null pty \{"cols":(\d+),"rows":(\d+)\} draws (\d+)x(\d+)' } | Select-Object -First 1
  $okSize = $false; $cols = 0; $rows = 0
  if ($tile -and $tile -match 'pty \{"cols":(\d+),"rows":(\d+)\} draws (\d+)x(\d+)') { $cols = [int]$Matches[3]; $rows = [int]$Matches[4]; $okSize = ($Matches[1] -eq $Matches[3] -and $Matches[2] -eq $Matches[4]) }
  Check $okSize "exe: the session's ptySize is the exe's pane (${cols}x${rows}), sizeBy remote, no mirror"
  Check ($o -and $o.claimed -and $o.who -eq 'remote' -and $o.cls -eq 'drive-other') "deck window: the remote icon, grey (drive-other)"
  Check ($o -and $o.title -eq 'Sizes follow: the remote desk (E2EBOX). That is for the sessions it shows: there the office deck shows its size and cannot type. The office deck sizes the rest. Click to drive from here.') 'deck window: the title names the remote desk (E2EBOX)'
  Check ($o -and $o.remoteDesk.name -eq 'E2EBOX') 'deck window: meta.remoteDesk.name is E2EBOX'
  Check ($o -and $o.pty.cols -eq $cols -and $o.pty.rows -eq $rows -and $o.sizeBy -eq 'remote' -and $o.mirror.cols -eq $cols) "deck window: the pty is the exe's pane (${cols}x${rows}) and the office mirrors it"
  Check ([bool](Seg 'c' | Where-Object { $_ -match '^drive words new driver auto look auto' })) 'exe: the second click released to auto'
  $after = (Deck 'state.settings.driver') -replace '"', ''
  Check ($after -eq 'auto') 'deck: the driver is auto again'
  Remove-Item -Recurse -Force $state -ErrorAction SilentlyContinue
} finally {
  try { if ($id) { Deck "deck.closeTile('$id').then(() => true)" | Out-Null } } catch {}
  try { Deck "deck.setDriver('$driverWas').then(() => true)" | Out-Null } catch {}
  try { Deck "(clearLayout(), true)" | Out-Null } catch {}
  if (-not $wasRunning -and -not $KeepDeck) { Start-Sleep -Milliseconds 800; (Launcher 'stop' 6>&1 | Out-String).Trim() }
}
$left = @(Get-Process -Name $DeckEdition.ExeBase -ErrorAction SilentlyContinue).Count
"left behind: ClaudeDeckNative.exe $left"
if ($script:fail.Count) { "VERDICT: FAIL ($($script:pass) ok, $($script:fail.Count) failed) - " + ($script:fail -join '; '); exit 1 }
"VERDICT: ok - $($script:pass) checks"
