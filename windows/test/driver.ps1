# windows/test/driver.ps1 — who drives the size, against BOTH kinds of deck (windows/test/driver-deck.js: the real
# lib/phone.js of each checkout over a fake main; loopback, a made-up token, no process behind a tile).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/driver.ps1 [-ClaimRoot <checkout>] [-Only old|new]
# OLD deck (this checkout's server: auto | phone | desk): the button cycles the three, looks and tooltips as before,
#   nothing extra is sent when a mirror ends, deskInit's { name } is ignored and answered, a wheel frame arrives.
# NEW deck (the claim checkout's server and lib/driverrule.js: auto | office | phone | remote, sizeBy, remoteDesk):
#   one click claims (`remote`), a click on the claim releases (`auto`); the claim says the pane's size although the
#   pane did not change; a pty that drifted under the claim is put back; the name arrives and comes back in meta.
# The exe runs headless (--shot, a temp --state-dir); node runs hidden. Nothing appears on screen.
# Pictures: windows/test/out/n-drv-<old|new>-*.png. Exit 1 when a check fails.
param([string]$ClaimRoot = '', [string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
if (-not $ClaimRoot) { $ClaimRoot = Join-Path (Split-Path -Parent $root) '_wt-deck-remote-rule' }
$exe = Join-Path $root ('windows\build\' + $DeckEdition.Exe)
$outDir = Join-Path $root 'windows\test\out'
$node = (Get-Command node -ErrorAction Stop).Source
$script:fail = @(); $script:pass = 0
function Check([bool]$ok, [string]$what) { if ($ok) { $script:pass++; "  ok   $what" } else { $script:fail += $what; "  FAIL $what" } }

function Run-Deck([string]$kind, [int]$port, [string]$do, [string]$deskName) {
  $work = Join-Path $env:TEMP ("cdn-drv-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
  New-Item -ItemType Directory -Force $work | Out-Null
  $portFile = Join-Path $work 'deck.port'; $rec = Join-Path $outDir "n-drv-$kind.frames.json"
  $out = Join-Path $outDir "n-drv-$kind.png"
  Remove-Item $out, "$out.log.txt", "$out.error.txt", $rec -ErrorAction SilentlyContinue
  $deck = Start-Process -FilePath $node -ArgumentList ('"{0}" {1} "{2}" "{3}" {4} "{5}"' -f (Join-Path $PSScriptRoot 'driver-deck.js'), $kind, $portFile, $rec, $port, $ClaimRoot) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $work 'deck.log') -RedirectStandardError (Join-Path $work 'deck.err.log')
  try {
    for ($i = 0; $i -lt 100 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $portFile)) { throw ("the $kind driver deck did not start: " + (Get-Content (Join-Path $work 'deck.err.log') -Raw)) }
    $env:CLAUDE_DECK_LINK = "http://127.0.0.1:$port/?t=drivertoken000001"
    $a = "--no-save --state-dir `"$work`" --shot `"$out`" --after 1200 --size 1400x900 --do `"$do`""
    if ($deskName) { $a += " --desk-name `"$deskName`"" }
    $p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
    "exe exit $($p.ExitCode)"
    Start-Sleep -Milliseconds 300
    $log = if (Test-Path "$out.log.txt") { @(Get-Content "$out.log.txt") } else { @() }
    $r = if (Test-Path $rec) { Get-Content $rec -Raw | ConvertFrom-Json } else { $null }
    return @{ log = $log; rec = $r; exit = $p.ExitCode }
  } finally {
    try { if (-not $deck.HasExited) { $deck.Kill() } } catch {}
    Start-Sleep -Milliseconds 200
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
  }
}
# the lines `dump drive` wrote after the n-th marker `mark <name>` (the script logs `UNKNOWN STEP: mark` lines as markers)
function After([string[]]$log, [string]$mark) { $i = [array]::IndexOf($log, "UNKNOWN STEP: mark-$mark"); if ($i -lt 0) { return @() } $rest = $log[($i + 1)..($log.Count - 1)]; $j = 0; foreach ($l in $rest) { if ($l -like 'UNKNOWN STEP: mark-*') { break } $j++ }; if ($j -eq 0) { @() } else { $rest[0..($j - 1)] } }
function Has($lines, [string]$rx) { [bool](@($lines) | Where-Object { $_ -match $rx }) }

# ============================================================== the OLD deck
if ($Only -ne 'new') {
  '--- OLD deck (this checkout''s lib/phone.js; auto | phone | desk)'
  $do = @(
    'sel driver one', 'wait 700', 'mark-a', 'dump drive', 'shot drv-old-auto',
    'zone tool drive-btn', 'wait 700', 'mark-b', 'dump drive', 'shot drv-old-phone',
    'zone tool drive-btn', 'wait 700', 'mark-c', 'dump drive', 'shot drv-old-desk',
    "call __office ['`$sel',90,28]", 'wait 700', 'mark-d', 'dump drive',
    'zone tool drive-btn', 'wait 900', 'mark-e', 'dump drive',
    "call __flags ['`$sel',true,true]", 'feed \e[?1000h\e[?1006h', 'wait 700', 'wheel 800 400 120', 'wait 200', 'feed \e[?1000l\e[?1006l', 'click 800 400', 'wait 200', 'text x', 'wait 300',
    # 14b (2026-11-23): this deck names no answerer (the Electron office answers all): no reply; named 'remote': one
    'feed \e[c', 'wait 300', "faketile answerer 'remote'", 'feed \e[6n', 'wait 300', "faketile answerer 'office'", 'feed \e[5n', 'wait 300',
    'zone tool settings-btn', 'wait 700', 'shot drv-old-settings', 'mark-z'
  ) -join '; '
  $r = Run-Deck 'old' 8791 $do ''
  $log = $r.log
  $log | Where-Object { $_ -match '^(drive |call setDriver|resize|deskInit|wheel report|input|BLOCKED|NO |BAD)' }
  Check ($r.exit -eq 0) 'old: the exe ran to the end'
  Check (Has (After $log 'a') '^drive words old driver auto look plain') 'old: the words are told as old; auto looks plain'
  Check (Has (After $log 'a') '^drive tip Auto: the phone drives the tile it is attached to; this window mirrors that one\. Click for phone\.$') 'old: the auto tooltip is today''s'
  Check (Has $log '^call setDriver \["phone"\]') 'old: the first click asks for phone (the cycle)'
  Check (Has (After $log 'b') '^drive words old driver phone look on') 'old: phone looks on (green)'
  Check (Has $log '^call setDriver \["desk"\]') 'old: the second click asks for desk'
  Check (Has (After $log 'c') '^drive words old driver desk look off') 'old: desk looks off (grey)'
  Check (Has (After $log 'c') '^drive tip Desk: the phone only views; this is always the desktop view\. Click for auto\.$') 'old: the desk tooltip is today''s'
  Check (Has (After $log 'd') 'mirror \{"cols":90,"rows":28\}.* draws 90x28') 'old: under desk the pane mirrors the pty (90x28)'
  Check (Has $log '^call setDriver \["auto"\]') 'old: the third click asks for auto'
  Check (-not (Has $log 'became the driver|reassert')) 'old: nothing extra is sent when the mirror ends (behaviour unchanged)'
  Check (-not (Has $log '^call setDriver \["(remote|office)"\]')) 'old: a new word is never sent to an old deck'
  $init = @($r.rec.frames | Where-Object { $_.t -eq 'call' -and $_.fn -eq 'deskInit' })
  Check ($init.Count -ge 1 -and $init[0].args[0].name -eq $env:COMPUTERNAME) "old: deskInit carried this computer's name, and the old server answered it (the exe got its meta)"
  $wheel = @($r.rec.frames | Where-Object { $_.t -eq 'input' -and $_.kind -eq 'wheel' })
  Check ($wheel.Count -eq 1 -and $wheel[0].d -match '^\x1b\[<64;\d+;\d+M$') 'old: one wheel frame arrived, kind wheel, one SGR report'
  $keys = @($r.rec.frames | Where-Object { $_.t -eq 'input' -and $_.kind -eq 'key' })
  Check ($keys.Count -ge 1 -and @($keys | Where-Object { $_.d -eq 'x' }).Count -eq 1) 'old: a typed key is still kind key'
  $da = @($r.rec.frames | Where-Object { $_.t -eq 'input' -and $_.d -match '\x1b\[\?[\d;]*c' }); $cpr = @($r.rec.frames | Where-Object { $_.t -eq 'input' -and $_.d -match '\x1b\[\d+;\d+R' }); $dsr = @($r.rec.frames | Where-Object { $_.t -eq 'input' -and $_.d -match '\x1b\[0n' })
  Check ($da.Count -eq 0) 'old: no answerer named (an Electron office answers all): the remote answers no query'
  Check ($cpr.Count -eq 1) 'old: named the answerer (a C++ office not showing the tile): the remote answers, once'
  Check ($dsr.Count -eq 0) 'old: the office named again: no answer from the remote'
}

# ============================================================== the NEW deck
if ($Only -ne 'old') {
  if (-not (Test-Path (Join-Path $ClaimRoot 'lib\driverrule.js'))) { "no claim checkout at $ClaimRoot : the NEW deck is not tested"; $script:fail += 'no claim checkout' }
  else {
    "--- NEW deck ($ClaimRoot lib/phone.js + lib/driverrule.js; auto | office | phone | remote)"
    $do = @(
      'sel driver one', 'wait 700', 'mark-a', 'dump drive', 'shot drv-new-auto',
      'zone tool drive-btn', 'wait 700', 'mark-b', 'dump drive', 'shot drv-new-remote',
      "call setDriver ['office']", 'wait 700', "call __office ['`$sel',90,28]", 'wait 700', 'mark-c', 'dump drive', 'shot drv-new-office',
      'zone tool drive-btn', 'wait 900', 'mark-d', 'dump drive',
      "call __office ['`$sel',70,20]", 'wait 3200', 'mark-e', 'dump drive',
      'zone tool drive-btn', 'wait 700', 'mark-f', 'dump drive',
      "call setDriver ['phone']", 'wait 700', 'mark-g', 'dump drive', 'shot drv-new-phone',
      "call setDriver ['auto']", 'wait 500',
      "call __flags ['`$sel',true,true]", 'feed \e[?1000h\e[?1006h', 'wait 700', 'wheel 800 400 120', 'wait 200', 'feed \e[?1000l\e[?1006l', 'click 800 400', 'wait 200', 'text x', 'wait 300',
      'zone tool settings-btn', 'wait 700', 'shot drv-new-settings', 'mark-z'
    ) -join '; '
    $r = Run-Deck 'new' 8792 $do 'TESTBOX-9'
    $log = $r.log
    $log | Where-Object { $_ -match '^(drive |call setDriver|resize|deskInit|wheel report|input|BLOCKED|NO |BAD)' }
    Check ($r.exit -eq 0) 'new: the exe ran to the end'
    Check (Has (After $log 'a') '^drive words new driver auto look auto remoteDesk \{.*"name":"TESTBOX-9".*\}') 'new: the words are told as new; auto has the auto look; the name came back in meta.remoteDesk'
    Check (Has (After $log 'a') '^drive tip Auto: a phone attached to a session drives its size, else a remote desk showing it, else the office deck\. Click to drive from here\.$') 'new: the auto tooltip is the branch''s sentence'
    Check (Has (After $log 'a') 'sizeBy "remote" mirror null') 'new: under auto the shown tile is sized by the remote desk'
    Check (Has $log '^call setDriver \["remote"\]') 'new: one click claims (setDriver remote)'
    Check (Has (After $log 'b') '^drive words new driver remote look mine') 'new: the claim looks mine (green)'
    Check (Has (After $log 'b') '^drive tip Sizes follow: this remote desk \(TESTBOX-9\)\. That is for the sessions it shows: there the office deck shows its size and cannot type\. The office deck sizes the rest\. Click to let whoever is looking drive \(auto\)\.$') 'new: the claim''s tooltip names this desk'
    Check (Has (After $log 'c') '^drive words new driver office look other') 'new: office looks other (grey)'
    Check (Has (After $log 'c') 'sizeBy "office" mirror \{"cols":90,"rows":28\}.* draws 90x28') 'new: under office the pane mirrors the pty (90x28)'
    $became = @(After $log 'c' | Where-Object { $_ -match '^resize \S+ (\d+)x(\d+) \(became the driver\)' })
    Check ($became.Count -eq 1) 'new: claiming says the pane''s size although the pane did not change (one resize, "became the driver")'
    $d = @(After $log 'd' | Where-Object { $_ -match '^drive tile' })
    Check ($d.Count -ge 1 -and $d[0] -match 'sizeBy "remote" mirror null pty \{"cols":(\d+),"rows":(\d+)\} draws (\d+)x(\d+)' -and $Matches[1] -eq $Matches[3] -and $Matches[2] -eq $Matches[4] -and $Matches[1] -ne '90') 'new: after the claim the pty has the pane''s size again'
    $re = @($log | Where-Object { $_ -match '^resize \S+ \d+x\d+ \(reassert: the pty is 70x20\)' })
    Check ($re.Count -eq 1) 'new: a pty that drifted under the claim (70x20) is put back, once'
    $e = @(After $log 'e' | Where-Object { $_ -match '^drive tile' })
    Check ($e.Count -ge 1 -and $e[0] -match 'pty \{"cols":(\d+),"rows":(\d+)\} draws (\d+)x(\d+)' -and $Matches[1] -eq $Matches[3] -and $Matches[2] -eq $Matches[4]) 'new: ... and the pty follows the pane again'
    Check (Has (After $log 'f') '^drive words new driver auto look auto') 'new: a click on the claim releases to auto'
    Check (@($log | Where-Object { $_ -match '^call setDriver \["auto"\]' }).Count -ge 1 -and -not (Has $log '^call setDriver \["desk"\]')) 'new: the release asks for auto; the old word desk is never sent'
    Check (Has (After $log 'g') '^drive words new driver phone look other') 'new: phone looks other (grey)'
    Check (Has (After $log 'g') '^drive tip Sizes follow: the phone\. Every other screen shows its size, and typing at the office deck is off\. Click to drive from here\.$') 'new: the phone tooltip is the branch''s sentence'
    $init = @($r.rec.frames | Where-Object { $_.t -eq 'call' -and $_.fn -eq 'deskInit' })
    Check ($init.Count -ge 1 -and $init[0].args[0].name -eq 'TESTBOX-9') 'new: deskInit carried { name }'
    $acc = @($r.rec.sizes | Where-Object { $_.accepted -and $_.from -eq 'remote' -and $_.driver -eq 'remote' })
    Check ($acc.Count -ge 2) 'new: the server took the remote desk''s sizes under the claim (lib/driverrule.js acceptsSize)'
    $wheel = @($r.rec.frames | Where-Object { $_.t -eq 'input' -and $_.kind -eq 'wheel' })
    Check ($wheel.Count -eq 1 -and $wheel[0].d -match '^\x1b\[<64;\d+;\d+M$') 'new: one wheel frame arrived, kind wheel, one SGR report'
    Check (@($r.rec.frames | Where-Object { $_.t -eq 'input' -and $_.kind -eq 'key' -and $_.d -eq 'x' }).Count -eq 1) 'new: a typed key is still kind key'
  }
}
$left = @(Get-Process -Name $DeckEdition.ExeBase -ErrorAction SilentlyContinue).Count
"left behind: ClaudeDeckNative.exe $left"
if ($script:fail.Count) { "VERDICT: FAIL ($($script:pass) ok, $($script:fail.Count) failed) - " + ($script:fail -join '; '); exit 1 }
"VERDICT: ok - $($script:pass) checks"
