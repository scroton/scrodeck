# windows/test/shell.ps1 — the SHELL half of local mode, headless: what windows/src does around the core deck so the
# native exe can stand in for the Electron one on this PC (windows/README.md "Local mode: the shell half").
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/shell.ps1 [-Only undim,phone,windows,restart,single,notify,state,update,asks,claude,tiles,askfeed,askcards,office,register,corewindow,ime,trans,holdform,ghost,strip,sbmin,auditors,pausemark,onedeck,quitwin,setlayout] [-Exe <path>]
#
# Every run is `--shot` (no window is ever shown) on a deck of its own in a TEMP --state-dir with a TEMP --home (so
# accounts, the alias file, sessions and transcripts are never the user's), shells are cmd.exe in pseudoconsoles,
# the notifier only records (no toast, no flash, no Start-menu shortcut), the listener binds 127.0.0.1:8811-8819 or
# refuses (deckhost.hpp TestListener), `tailscale` is never run, and no Claude session can start (the exe has no
# switch for it; a test's newTile for anything but a shell is blocked besides). At the end: no process left.
# Shots: windows/test/out/n-shell-*.png (+ .log.txt).
param([string]$Only = '', [string]$Exe = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$outDir = Join-Path $root 'windows\test\out'
New-Item -ItemType Directory -Force $outDir | Out-Null
$env:CLAUDE_DECK_LINK = $null
$script:pass = 0; $script:fail = 0; $script:pids = @(); $script:temps = @(); $script:procs = @{}
# Every deck this suite starts is KEPT (its Process object, so its handle) until the end: Windows does not hand a
# process id to another process while a handle to it is open. Dropped, a dead deck's id went to some other process on
# a busy PC, and that process's conhost.exe failed "nothing left behind" (3 of 3 full runs, 2026-11-24, release 5).
function Keep-Proc($p) { $script:pids += $p.Id; $script:procs[$p.Id] = $p; try { $null = $p.Handle } catch { } }
# A child of a recorded id is ours unless it was made after that deck had exited (the id then belongs to another).
function Is-OurChild($c) {
  $p = $script:procs[[int]$c.ParentProcessId]
  if (-not $p) { return $true }
  try { if ($p.HasExited -and $c.CreationDate -gt $p.ExitTime) { return $false } } catch { }
  $true
}
function Check([string]$what, $cond) { if ($cond) { $script:pass++; "  ok    $what" } else { $script:fail++; "  FAIL  $what" } }
function New-Temp([string]$tag) { $d = Join-Path $env:TEMP ("cdn-shell-$tag-" + [guid]::NewGuid().ToString('N').Substring(0, 8)); New-Item -ItemType Directory -Force $d | Out-Null; $script:temps += $d; $d }
function Shot([string]$name) { Join-Path $outDir "n-shell-$name.png" }
# One run. Returns its log. -NoWait: started and left running (the caller waits for it); the process is returned.
function Run([string]$name, [string]$do, [string]$state, [string]$homeDir, [string]$extra = '', [switch]$NoWait, [int]$After = 1200, [string]$Size = '1400x900', [switch]$WindowShot) {
  $png = Shot $name
  Remove-Item $png, "$png.error.txt", "$png.log.txt" -ErrorAction SilentlyContinue
  $a = "--local --verify-scene --state-dir `"$state`" --home `"$homeDir`" --after $After --size $Size --shell cmd.exe $(if ($WindowShot) { '--window-shot' } else { '--shot' }) `"$png`""
  if ($extra) { $a += " $extra" }
  if ($do) { $a += " --do `"$($do -replace '"', '\"')`"" }
  if ($NoWait) { $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru; Keep-Proc $p; return $p }
  $p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru
  Keep-Proc $p
  $log = if (Test-Path "$png.log.txt") { Get-Content "$png.log.txt" -Raw -Encoding UTF8 } else { '' }
  if (Test-Path "$png.error.txt") { $log += "`nERROR FILE: " + (Get-Content "$png.error.txt" -Raw) }
  "exit $($p.ExitCode)`n$log"
}
# (`click 60 800` = the New terminal tile's own line: with ONE account, as under a fake home, the tile's centre is its account button)
function Want([string]$t) { -not $Only -or (($Only -split ',') -contains $t) }
function Wait-File([string]$path, [int]$sec) { for ($i = 0; $i -lt $sec * 5; $i++) { if (Test-Path $path) { return $true }; Start-Sleep -Milliseconds 200 }; return $false }
function Wait-Gone([int]$procId, [int]$sec) { for ($i = 0; $i -lt $sec * 5; $i++) { if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { return $true }; Start-Sleep -Milliseconds 200 }; return $false }
function NoMismatch([string]$log) { $log -notmatch 'SCENE MISMATCH' }
# the public build (core/deck/edition.hpp kPublic): no move-all, auditor auto-send off by default
$publicEdition = (Get-Content (Join-Path $root 'core\deck\edition.hpp') -Raw) -match 'constexpr bool kPublic = true'

# ---------------------------------------------------------------- 1. what local mode no longer hides
if (Want 'undim') {
  '== undim: Elsewhere, Past, restart when quiet are live in local mode; only what the core refuses stays off'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  # a past session on the (fake) disk: a transcript of more than 2000 bytes with a title
  $proj = Join-Path $hm '.claude\projects\C--x'; New-Item -ItemType Directory -Force $proj | Out-Null
  $line = '{"type":"user","cwd":"C:\\x","sessionId":"11111111-2222-3333-4444-555555555555","message":{"role":"user","content":"' + ('pad ' * 600) + '"}}'
  [IO.File]::WriteAllText((Join-Path $proj '11111111-2222-3333-4444-555555555555.jsonl'), $line + "`n" + '{"type":"custom-title","customTitle":"a past session of the test"}' + "`n")
  $log = Run 'undim' "click 60 800; wait 1400; hzone elsewhere; shot shell-undim-links; zone elsewhere; wait 500; shot shell-undim-elsewhere; key esc; zone past; wait 600; shot shell-undim-past; key esc; zone tool restart-btn; wait 600; dump restart; shot shell-undim-armed; zone tool restart-btn; wait 500; dump restart; zone tool moveall-btn; wait 200; dump state; zone new-acct -; wait 200; dump state" $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'Elsewhere asks the deck (externalSessions)' ($log -match 'call externalSessions')
  Check 'Past asks the deck (pastSessions)' ($log -match 'call pastSessions')
  Check 'the restart button arms it (restartWhenQuiet, meta.restart.armed)' (($log -match 'call restartWhenQuiet \[true') -and ($log -match 'restart \{"armed":true'))
  Check 'a second click cancels it' ($log -match '(?s)restart \{"armed":true.*restart \{"armed":false')
  Check 'nothing is refused as "not in the local …"' ($log -notmatch 'not in the local')
  if ($publicEdition) { Check 'move-all: not in the public build (no button to press, no note)' ($log -notmatch "note 'Moving sessions between accounts") }
  else { Check 'move-all says the Claude launch is off (allowClaude)' ($log -match "note 'Moving sessions between accounts is switched off") }
  Check 'an account button says the same, and starts nothing' (($log -match "note 'Starting a Claude session is switched off") -and ($log -notmatch 'call newTile \[\{"account'))
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

# ---------------------------------------------------------------- 2. --serve and Settings -> Phone
if (Want 'phone') {
  '== phone: --serve as an option; Settings -> Phone against the native deck (enable, port, bind, new token, the link)'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $p = Run 'serve' 'wait 400; dump phone; wait 6000; dump phone' $st $hm ('--serve 8813 --assets "' + $root + '"') -NoWait
  $code = 0; $ctype = ''; $unauth = 0
  for ($i = 0; $i -lt 25 -and -not $code; $i++) { Start-Sleep -Milliseconds 200; try { $r = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:8813/manifest.webmanifest' -TimeoutSec 3; $code = $r.StatusCode; $ctype = $r.Headers['Content-Type'] } catch { } }
  try { Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:8813/' -TimeoutSec 3 | Out-Null } catch { $unauth = [int]$_.Exception.Response.StatusCode }
  $p.WaitForExit(30000) | Out-Null
  $log = Get-Content ((Shot 'serve') + '.log.txt') -Raw -Encoding UTF8
  Check '--serve 8813: the deck serves on 127.0.0.1:8813 (phoneStatus)' ($log -match '"address":"127.0.0.1".*"port":8813,"running":true')
  Check 'a public file answers 200 over real loopback HTTP' ($code -eq 200)
  Check 'the page itself answers 401 without the token' ($unauth -eq 401)
  Check 'the link shown is the loopback link with the token' ($log -match '"url":"http://127.0.0.1:8813/\?t=<token>"')

  $st = New-Temp 'st'
  $do = "wait 300; dump phone; call saveSettings [{'phone':{'enabled':true,'port':8814,'bind':'localhost'}}]; wait 500; dump phone; zone tool settings-btn; wait 600; wheel 300 400 -6000; wait 200; wheel 300 400 -6000; wait 200; shot shell-phone-panel; ozone set-btn phone-newtoken; wait 300; shot shell-phone-ask; ozone set-btn phone-newtoken; wait 600; dump phone; shot shell-phone-newtoken; ozone set-btn cancel; call saveSettings [{'phone':{'enabled':true,'port':8788,'bind':'tailscale'}}]; wait 400; dump phone; call saveSettings [{'phone':{'enabled':true,'port':8788,'bind':'all'}}]; wait 400; dump phone; call saveSettings [{'phone':{'enabled':false,'port':8814,'bind':'localhost'}}]; wait 400; dump phone"
  $log = Run 'phone' $do $st $hm
  $ph = [regex]::Matches($log, '(?m)^phone (\{.*?\})\s*(TEST BIND REFUSED \S+)?\s*$')
  Check 'six phoneStatus readings' ($ph.Count -eq 6)
  if ($ph.Count -eq 6) {
    $j = $ph | ForEach-Object { $_.Groups[1].Value | ConvertFrom-Json }
    Check 'off at first: enabled false, nothing listens, a token exists' ((-not $j[0].enabled) -and (-not $j[0].running) -and ($j[0].token -match '16 chars'))
    Check 'enabled on localhost:8814: running, address 127.0.0.1, the link shown' ($j[1].running -and $j[1].address -eq '127.0.0.1' -and $j[1].port -eq 8814 -and $j[1].url -eq 'http://127.0.0.1:8814/?t=<token>')
    Check 'clients / desks counts are there' ($j[1].clients -eq 0 -and $j[1].desks -eq 0)
    Check '"new token" (two clicks): another token, still serving' ($j[2].token -ne $j[1].token -and $j[2].running)
    Check 'bind tailscale without a Tailscale address: the error, nothing listens' ((-not $j[3].running) -and ($j[3].error -match 'no Tailscale address'))
    Check 'bind all: a test refuses to bind anything but 127.0.0.1:8811-8819' ((-not $j[4].running) -and ($ph[4].Groups[2].Value -match '0\.0\.0\.0:8788'))
    Check 'switched off again: nothing listens' ((-not $j[5].enabled) -and (-not $j[5].running))
  }
  Check 'the first click only asks' ($log -match '(?s)n-shell-phone-ask\.png.*call saveSettings \[\{"phone":\{"bind":"localhost","enabled":true,"newToken":true')
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

# (in the first window of these runs the sidebar's first tile row is at y 65 and the second at y 122: `click 100 122`
# selects the other tile, `dbl 100 65` the first again. A click selects and never places and a double-click places
# (30-tiles.js), so taking a tile back from another window is a double-click on it.)
# ---------------------------------------------------------------- 3. more than one window
if (Want 'windows') {
  '== windows: Open in a new window, a tile dragged out, every sidebar marks who holds what, closing one'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; text echo one; key enter; wait 300; click 60 800; wait 1400; text echo two; key enter; wait 400; rzone tile sel; shot shell-win-menu; menu Open in a new window; wait 1500; dump windows; win 1 text echo typed-in-window-two; win 1 key enter; wait 600; win 1 dump screen; win 1 shot shell-win-w1; shot shell-win-w0; " +
    "dump tiles; downzone tile sel; move -60 300; up -60 300; wait 1500; dump windows; win 2 shot shell-win-w2; win 1 shot shell-win-w1-emptied; " +
    "win 2 dump layout; click 100 122; wait 400; dbl 100 65; wait 600; dump windows; shot shell-win-w0-took-it-back; win 2 shot shell-win-w2-evicted; " +
    "win 1 close; wait 800; dump windows; dump tiles"
  $log = Run 'windows' $do $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'the menu opens a second window holding the tile, full' ($log -match 'window opened: w1 holding (\S+) at -?\d+,-?\d+, 2 windows')
  $w = [regex]::Matches($log, '(?m)^windows (\d) \(deck clients (\d)\)')
  Check 'two windows = two clients of the one deck' ($w.Count -ge 1 -and $w[0].Groups[1].Value -eq '2' -and $w[0].Groups[2].Value -eq '2')
  Check 'the deck says which window holds the tile (inWindow 1, shownBy win:1)' ($log -match 'inWindow 1 shownBy \["win:1"\]')
  Check 'the second window is its own terminal: what was typed there ran there' ($log -match '(?s)screen \S+ 91x38.*typed-in-window-two')
  Check 'a tile dragged out of the window opens a third (window:open at the drop point)' ($log -match 'window opened: w2 holding \S+ at -?\d+,\d+, 3 windows')
  Check 'one home per tile: the window that had it lost it (layout:evict)' ($log -match '(?s)windows 3.*window w1 inited 1 \S+ selected\s+layout \{\} shown \[\]')
  Check 'a double-click on it in the first window takes it back (the third is evicted)' ($log -match '(?s)windows 3.*windows 3 \(deck clients 3\)\s+window w0 primary inited 1 \S+ selected (\S+) layout \{"\1":\[1,2,3,4\]\} shown \[\1\].*?window w2 inited 1 \S+ selected\s+layout \{\} shown \[\]')
  Check 'closing one window leaves the others and every session' (($w.Count -ge 4) -and ($w[$w.Count - 1].Groups[1].Value -eq '2') -and (([regex]::Matches($log, "(?m)^tile \S+ shell 'cmd' status \S+ pinned 0 exited 0")).Count -ge 4))
  Check 'scene == whole frame in every shot, in every window' (NoMismatch $log)
}

if (Want 'realwin') {
  '== realwin: the same with REAL windows (parked off every screen, never activated): a second HWND, its own target'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; text echo one; key enter; wait 300; click 60 800; wait 1400; rzone tile sel; menu Open in a new window; wait 1800; dump windows; win 1 text echo real-window-two; win 1 key enter; wait 700; win 1 dump screen; win 1 shot shell-realwin-w1; shot shell-realwin-w0; win 1 close; wait 800; dump windows"
  $log = Run 'realwin' $do $st $hm '' -WindowShot
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'a second real window opened as a second client' ($log -match '(?m)^windows 2 \(deck clients 2\)')
  Check 'its terminal is its own' ($log -match '(?s)screen \S+ \d+x\d+.*real-window-two')
  Check 'closing it leaves the first window and both sessions' ($log -match '(?s)windows 1 \(deck clients 1\)')
}

# ---------------------------------------------------------------- 4. restart: relaunch, hand-over, the windows come back
if (Want 'restart') {
  '== restart: the deck restarts itself (relaunch + wait for the old one), tiles, layout and the second window restored'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $png2 = Shot 'restart-after'
  Remove-Item $png2, "$png2.log.txt", "$png2.error.txt" -ErrorAction SilentlyContinue
  # relaunch.json in the deck's own folder says what a restart starts (main.js's file shape). A test may only name this exe.
  $args2 = @('--local', '--verify-scene', '--persist-shells', '--state-dir', $st, '--home', $hm, '--after', '3500', '--size', '1400x900', '--shell', 'cmd.exe', '--shot', $png2,
    '--do', 'wait 800; dump windows; dump tiles; win 1 shot shell-restart-after-w1; shot shell-restart-after-w0; life lock')
  [IO.File]::WriteAllText((Join-Path $st 'relaunch.json'), (@{ command = $exe; args = $args2 } | ConvertTo-Json))
  $do = "click 60 800; wait 1400; text echo one; key enter; wait 300; click 60 800; wait 1400; text echo two; key enter; wait 400; rzone tile sel; menu Open in a new window; wait 1500; zone tile title:cmd; wait 500; dump windows; life spec; shot shell-restart-before-w0; win 1 shot shell-restart-before-w1; call restartNow []; wait 4000"
  $t0 = Get-Date
  $log = Run 'restart' $do $st $hm '--persist-shells'
  $first = $script:pids[-1]
  Check 'the first deck left after restartNow (exit 0, before its script ended)' (($log -match '(?m)^exit 0') -and (((Get-Date) - $t0).TotalSeconds -lt 25))
  Check 'it started its relaunch with --wait-pid <its own pid>' ($log -match "relaunch started: .*--wait-pid $first")
  $ok = Wait-File $png2 40
  Check 'the relaunched deck came up and took its shot' $ok
  Start-Sleep -Milliseconds 800
  $log2 = if (Test-Path "$png2.log.txt") { Get-Content "$png2.log.txt" -Raw -Encoding UTF8 } else { '' }
  $ids = [regex]::Matches($log, "(?m)^window w1 inited 1 \S+ selected (\S+) layout (\{.*?\})") | Select-Object -Last 1
  Check 'it restored the second window with its layout (R7)' ($ids -and ($log2 -match [regex]::Escape("window restored: w1 layout " + $ids.Groups[2].Value)))
  Check 'two windows again, the same tile in the second' ($ids -and ($log2 -match '(?m)^windows 2 \(deck clients 2\)') -and ($log2 -match ('window w1 inited 1 \S+ selected ' + $ids.Groups[1].Value + ' layout ' + [regex]::Escape($ids.Groups[2].Value))))
  Check 'both terminals are back as fresh shells under their ids (--persist-shells)' (([regex]::Matches($log2, "(?m)^tile \S+ shell 'cmd' status \S+ pinned 0 exited 0")).Count -eq 2)
  Check 'the new deck holds the single-instance lock the old one gave up' ($log2 -match 'single instance: lock held 1')
  Check 'scene == whole frame after the restart' (NoMismatch $log2)
  # without relaunch.json a test starts nothing (it would run its own script again, for ever): it says what a real run would start
  $st3 = New-Temp 'st'
  $log3 = Run 'restart-dry' 'wait 300; life spec; call restartNow []; wait 3000' $st3 $hm
  Check 'no relaunch.json: a test logs the command line (this exe, its own arguments, --wait-pid) and starts nothing' (($log3 -match ('relaunch \(not started in a test\): .*' + [regex]::Escape($DeckEdition.Exe) + '"? --local .* --wait-pid \d+')) -and ($log3 -match '(?m)^exit 0'))
}

# ---------------------------------------------------------------- 5. single instance
if (Want 'single') {
  '== single: a second launch on the same folder leaves, and the first is told to come forward'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $p = Run 'single-first' 'wait 300; life lock; wait 7000; life lock' $st $hm -NoWait
  Start-Sleep -Milliseconds 2500
  $log2 = Run 'single-second' 'wait 300' $st $hm
  $p.WaitForExit(40000) | Out-Null
  $log1 = Get-Content ((Shot 'single-first') + '.log.txt') -Raw -Encoding UTF8
  Check 'the second launch exits with 8 and starts no deck' (($log2 -match '(?m)^exit 8') -and ($log2 -match 'another deck holds this folder') -and ($log2 -match 'told to come forward'))
  Check 'the first one hears it (second-instance)' ($log1 -match 'second-instance: the first window comes forward')
  Check 'the first one keeps the lock and its hub the whole time' (([regex]::Matches($log1, 'single instance: lock held 1, a second launch finds the hub 1')).Count -eq 2)
  $st2 = New-Temp 'st'
  $log3 = Run 'single-other' 'wait 200; life lock' $st2 $hm
  Check 'another folder is another deck: it starts' (($log3 -match '(?m)^exit 0') -and ($log3 -match 'lock held 1'))
}

# ---------------------------------------------------------------- 6. the notifier
if (Want 'notify') {
  '== notify: toasts are recorded, never shown; a click brings up the window that holds the tile; the flash'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; click 60 800; wait 1400; rzone tile sel; menu Open in a new window; wait 1500; wait 300; dump windows; win 1 toast waiting; toast flash 1; toast click; wait 300; click 100 122; wait 400; toast done; toast dump; toast click; wait 300; dump windows; call saveSettings [{'notify':{'sound':false}}]; wait 400; toast waiting; toast dump"
  $log = Run 'notify' $do $st $hm
  Check 'a toast reaches the notifier with the deck''s words' (($log -match "toast \[waiting\] tile \S+ silent 0 'test . cmd' / 'waiting for you'") -and ($log -match "toast \[done\] tile \S+ silent 0 '.*' / 'done after 42 s'"))
  Check 'nothing was shown for real' ($log -match 'toasts 2, flashes 1, real toasts shown 0')
  Check 'the flash names the window' ($log -match '(?m)^flash window 1')
  $m = [regex]::Match($log, "toast \[waiting\] tile (\S+) ")
  $w1 = [regex]::Match($log, '(?m)^window w1 inited 1 \S+ selected (\S+) ')
  Check 'a click on a toast of the tile in window 2 brings THAT window up, on that tile' ($m.Success -and $w1.Success -and ($m.Groups[1].Value -eq $w1.Groups[1].Value) -and ($log -match ("toast click -> window w1 comes up on tile " + $m.Groups[1].Value)))
  Check 'a click on a toast of a tile no window holds brings the first window up' ($log -match 'toast click -> window w0 comes up on tile')
  Check 'notify.sound off -> a silent toast' ($log -match "toast \[waiting\] tile \S+ silent 1")
}

# ---------------------------------------------------------------- 7. window state, keys, the version
if (Want 'state') {
  '== state: minimised / focused reach the deck (shownBy), a key anywhere in the window is "office", deck.version'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; dump windows; dump presence; tap f9; tap f9; wait 2300; dump presence; wm min; wait 2300; dump windows; wm restore; wait 2300; dump windows; focus off; wait 300; focus on; wait 300"
  $log = Run 'state' $do $st $hm
  Check 'on screen: the tile is shown by win:0' ($log -match 'shownBy \["win:0"\]')
  Check 'minimised: the deck knows the window shows nothing (shownBy [])' ($log -match '(?s)windowState w0 visible 0 focused 1.*shownBy \[\]')
  Check 'restored: shown again' ($log -match '(?s)windowState w0 visible 1 focused 1.*shownBy \["win:0"\]')
  Check 'focus lost and regained are reported' ($log -match '(?s)windowState w0 visible 1 focused 0.*windowState w0 visible 1 focused 1')
  Check 'a key that reaches no tile (F9) still says the user is at this PC' ($log -match '(?s)deck-state\.json where "office" lastInputFrom null.*deck-state\.json where "office" lastInputFrom "office"')
  Check 'deck-state.json carries the native deck''s version' ($log -match '"version":"0\.1\.0-native"')
}

# ---------------------------------------------------------------- 8. the update offer
if (Want 'update') {
  '== update: the exe changed on disk (its fingerprint) -> the deck offers the restart'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $log = Run 'update' 'click 60 800; wait 1200; dump restart; life bump; wait 32000; dump restart; shot shell-update-offer; zone restart later; wait 600; dump restart' $st $hm
  Check 'no offer while the exe is what it was at boot' ($log -match '(?m)^restart \{.*"update":null')
  Check 'a changed fingerprint is offered as an update naming the exe' ($log -match ('(?s)fingerprint now reads.*restart \{.*"update":\{[^}]*' + [regex]::Escape($DeckEdition.Exe) + ''))
  Check '"Later" puts the offer away' ($log -match '(?s)call updateLater.*restart \{.*"update":null')
  Check 'scene == whole frame' (NoMismatch $log)
  # the self-update's steps go to the exe's own log (<state>\local\deck.log), not only a test's: a check by hand against a
  # test folder whose signature is no good (CLAUDE_DECK_UPDATE_FROM), and an update applied at start (an .old beside the exe)
  $st = New-Temp 'st'; $from = New-Temp 'upd'
  [IO.File]::WriteAllText((Join-Path $from 'SHA256SUMS'), ('0' * 64) + "  $($DeckEdition.Exe)`n")
  [IO.File]::WriteAllText((Join-Path $from 'SHA256SUMS.asc'), "-----BEGIN PGP SIGNATURE-----`nnot a signature`n-----END PGP SIGNATURE-----`n")
  $old = "$exe.old"; [IO.File]::WriteAllText($old, 'the exe before the update (a stand-in)')
  $env:CLAUDE_DECK_UPDATE_FROM = $from
  $log = Run 'update-log' 'click 60 800; wait 800; tap ctrl+shift+P; wait 300; text Check for updates; wait 200; tap enter; wait 6000' $st $hm
  $env:CLAUDE_DECK_UPDATE_FROM = $null
  $dl = Get-Content (Join-Path $st 'local\deck.log') -Raw -ErrorAction SilentlyContinue
  Check 'update log: a bad signature is a line of the deck''s own log' ($dl -match '\[update\] (signature check FAILED|no gpg on this machine)')
  Check 'update log: an update applied at start is a line of it (the .old beside the exe removed)' (($dl -match '\[update\] applied at start') -and -not (Test-Path $old))
}

# ---------------------------------------------------------------- 9. the ask feed in local mode: the honest empty state
if (Want 'asks') {
  '== asks: local mode has no ask feed yet (lib/asks.js is the core''s): the tile and the pane say "nothing waiting"'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $log = Run 'asks' 'click 60 800; wait 1200; ask open all; wait 500; ask dump; shot shell-asks-open; ask close; dump zones' $st $hm
  Check 'the asks tile is there and reads nothing waiting (no fake count)' ($log -match 'zone asks')
  Check 'no ask call is made to a deck that would refuse it' ($log -notmatch 'call ask(Draft|Log|Done|Clear)')
  Check 'scene == whole frame' (NoMismatch $log)
}

# ---------------------------------------------------------------- 10. with the Claude launch on (HOST-NOTES.md edit 6)
if (Want 'claude') {
  '== claude: --allow-claude draws the account buttons and Move as available; a test still starts nothing'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $log = Run 'claude-on' "click 60 800; wait 1200; move 700 450; shot shell-claude-on-buttons; zone new-acct -; wait 300; dump state; zone tool moveall-btn; wait 400; shot shell-claude-on-move; dump state" $st $hm '--allow-claude'
  Check 'an account button is live: it asks for a Claude session (which a test blocks)' ($log -match 'BLOCKED newTile \[\{"account"')
  Check 'nothing says "switched off"' ($log -notmatch 'switched off')
  Check 'scene == whole frame' (NoMismatch $log)
  $log0 = Run 'claude-off' "click 60 800; wait 1200; move 700 450; shot shell-claude-off-buttons" (New-Temp 'st') $hm '--no-claude'
  $a = [IO.File]::ReadAllBytes((Shot 'claude-on-buttons')); $b = [IO.File]::ReadAllBytes((Shot 'claude-off-buttons'))
  Check 'the picture differs from --no-claude (the buttons are no longer dimmed)' (-not [Linq.Enumerable]::SequenceEqual($a, $b))
}

# ---------------------------------------------------------------- 11. a tile leaving `starting`, a sleeping tile
if (Want 'tiles') {
  '== tiles: the pane size is said again when a tile leaves starting; no terminal reply or report for a sleeping tile'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1500; faketile status 'starting'; wait 200; faketile status 'idle'; wait 300; feed \e[6n; wait 200; faketile asleep true; wait 200; feed \e[6n; wait 200; focus off; focus on; wait 200; faketile asleep false; wait 200; feed \e[6n; wait 200"
  $log = Run 'tiles' $do $st $hm
  Check 'leaving starting: the pane''s size is sent to the deck again' ($log -match '(?m)^resize \S+ 126x48 \(left starting\)')
  Check 'awake: the terminal answers a cursor query' (([regex]::Matches($log, '(?m)^reply \S+ \\e\[\d+;\d+R')).Count -eq 2)
  Check 'asleep: the answer is NOT sent (it would wake the session)' (([regex]::Matches($log, 'reply NOT sent for \S+: the tile is asleep')).Count -eq 1)
  # 14b (2026-11-23): exactly one answerer per session, named by the core deck in meta `answerer`; this window (the
  # office) answers unless it names a remote desk or the phone
  $do = "click 60 800; wait 1500; faketile answerer 'office'; feed \e[6n; wait 200; faketile answerer 'remote'; feed \e[6n; wait 200; faketile answerer 'phone'; feed \e[6n; wait 200; faketile answerer 'office'; feed \e[6n; wait 200"
  $log = Run 'tiles-answerer' $do (New-Temp 'st') (New-Temp 'home')
  Check 'answerer office: this window answers the query' (([regex]::Matches($log, '(?m)^reply \S+ \\e\[\d+;\d+R')).Count -eq 2)
  Check 'answerer remote / phone: it does not (the remote desk or the phone does)' (([regex]::Matches($log, 'reply NOT sent for \S+: the deck names (remote|phone)')).Count -eq 2)
}

# ---------------------------------------------------------------- 12. the ask feed in local mode (the core's, HOST-NOTES.md)
# ASK_PAGE_STATE_DIR / ASK_PAGE_DOWNLOADS name TEMP folders (never the real ~/.local/state/ask-pages): a synthetic
# session file with one waiting page (a copy of a web-test fixture page). The app's own ask pane shows it, answers
# it, and Done saves the answers file beside the page. Then the same with another deck's fresh heartbeat in
# deck.json: this deck is a GUEST, the page shows read-only, nothing shared is written.
if (Want 'askfeed') {
  '== askfeed: the core''s ask feed reaches the app''s ask pane in local mode (the WEB pane, --ask-pane web: the fallback); a guest shows the pages read-only'
  $pageId = 'ask_bug-queue_aaaaaaaa_r1_1118-1935_c233'; $sid = 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee'
  function New-AskFolders {
    $w = New-Temp 'ask'; $askDir = Join-Path $w 'ask-state'; $pages = Join-Path $w 'pages'; $dl = Join-Path $w 'dl'
    New-Item -ItemType Directory -Force $askDir, $pages, $dl | Out-Null
    Copy-Item (Join-Path $root "windows\web-test\fixtures\$pageId.html") (Join-Path $pages "$pageId.html")
    $state = @{ session_id = $sid; session_name = 'bug-queue'; account = ''; answers = @{}; rounds = @(@{ id = $pageId; round = 1; status = 'open'; created = (Get-Date).AddMinutes(-5).ToString('o'); title = 'T'
      page = (Join-Path $pages "$pageId.html"); items = @(@{ qid = 'Q1'; key = 'r1-Q1'; title = 'Apollo release' }, @{ qid = 'Q2'; key = 'r1-Q2'; title = 'second' }) }) }
    [IO.File]::WriteAllText((Join-Path $askDir "$sid.json"), ($state | ConvertTo-Json -Depth 8 -Compress))
    @{ askDir = $askDir; pages = $pages; dl = $dl }
  }
  $do = "wait 1500; dump zones; ask open all; wait 4000; ask dump; wait 200; ask in 0 document.querySelectorAll('.card')[0].querySelector('[data-pick]'); wait 1200; ask dump; wait 200; ask host document.querySelector('section.ap-sess .ap-done'); wait 2000; ask dump; wait 200; shot shell-askfeed; ask close; wait 600; toast ask $sid; toast click; wait 2000; ask dump"
  try {
    # ---- this deck serves the pages
    $f = New-AskFolders
    $env:ASK_PAGE_STATE_DIR = $f.askDir; $env:ASK_PAGE_DOWNLOADS = $f.dl
    $log = Run 'askfeed' $do (New-Temp 'st') (New-Temp 'home') '--ask-pane web' -WindowShot
    Check 'the asks tile counts the page (meta.asks from the core''s feed)' ($log -match "ask pane: mode 'all' open 1 title 'Asks . 1 session waiting'")
    Check 'the page loaded in the pane from its file (served from memory at the made-up origin), and said ready' ($log -match 'call askDraftGet \["' + $pageId + '"\]' -and $log -match '"frames":\[\d{2,}\]')
    Check 'an answer picked on the page reached the deck as a draft' ($log -match 'fire askDraft \["' + $pageId + '",\{"answered":1')
    Check 'Done was sent, and the answers file is beside the page (a temp folder)' ($log -match 'call askDone \["' + $sid + '"' -and (Test-Path (Join-Path $f.pages "$pageId.answers.json")))
    Check 'the section reads Sent' ($log -match '"done":"Sent","doneOff":true')
    Check 'a clicked ask toast is routed by the core (asks:open) and opens that session''s questions' ($log -match 'toast click -> window w\d+ comes up on tile  \(an ask toast: session ' + $sid + '\) \(routed by the core\)' -and $log -match "ask pane: mode '$sid' open 1")
    Check 'deck.json was this deck''s and went with it' (-not (Test-Path (Join-Path $f.askDir 'deck.json')))
    # ---- another deck's fresh heartbeat is there first: a guest
    $g = New-AskFolders
    $foreign = '{"kind":"claude-deck-asks","pid":1,"at":"' + [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + '","shows_pages":true}'
    [IO.File]::WriteAllText((Join-Path $g.askDir 'deck.json'), $foreign)
    $env:ASK_PAGE_STATE_DIR = $g.askDir; $env:ASK_PAGE_DOWNLOADS = $g.dl
    $log = Run 'askfeed-guest' $do (New-Temp 'st') (New-Temp 'home') '--ask-pane web' -WindowShot
    Check 'guest: the page still shows' ($log -match '"frames":\[\d{2,}\]')
    Check 'guest: the pane says so in plain words (the bar and the section''s line), Done is off' ($log -match "hint '.*read-only: another deck is serving the ask pages'" -and $log -match '"doneOff":true,"flash":"Read-only here: another deck is serving the ask pages')
    Check 'guest: no draft, no Done, no answers file' ($log -notmatch 'fire askDraft' -and $log -notmatch 'call askDone' -and -not (Test-Path (Join-Path $g.pages "$pageId.answers.json")))
    Check 'guest: the other deck''s deck.json is untouched, byte for byte' ((Test-Path (Join-Path $g.askDir 'deck.json')) -and ([IO.File]::ReadAllText((Join-Path $g.askDir 'deck.json')) -eq $foreign))
    # ---- an OLDER ask page's own Done is a download (main.js hookAskDownloads): `ask_<id>.answers.json` is saved in the
    # first Downloads folder, with no dialog; any other download from an ask page is cancelled
    $h = New-AskFolders
    $env:ASK_PAGE_STATE_DIR = $h.askDir; $env:ASK_PAGE_DOWNLOADS = $h.dl
    $older = 'ask_test-older_00000000_r1_1119-1700_abcd.answers.json'
    $mk = { param($id, $name) "ask eval 0 (a=document.createElement('a'),a.id='$id',a.textContent='$id',a.href='data:application/json,{}',a.download='$name',document.body.prepend(a),'made')" }
    $dlDo = "wait 1500; ask open all; wait 4000; " + (& $mk 'dl1' $older) + "; wait 300; ask in 0 document.getElementById('dl1'); wait 2500; " + (& $mk 'dl2' 'other.txt') + "; wait 300; ask in 0 document.getElementById('dl2'); wait 2500; ask dump"
    $log = Run 'askfeed-dl' $dlDo (New-Temp 'st') (New-Temp 'home') '--ask-pane web' -WindowShot
    Check 'an older page''s own Done: its answers file is saved in the Downloads folder, no dialog' ($log -match ('ask page download ' + [regex]::Escape($older) + ' -> ') -and (Test-Path (Join-Path $h.dl $older)))
    Check '...any other download from an ask page is cancelled' ($log -match 'ask page download other\.txt -> cancelled' -and -not (Test-Path (Join-Path $h.dl 'other.txt')))
  } finally { $env:ASK_PAGE_STATE_DIR = $null; $env:ASK_PAGE_DOWNLOADS = $null }
}

# ---------------------------------------------------------------- 12b. the NATIVE ask pane (askcards.hpp, the default): a REAL page
# A page built by the REAL ask_page.py (the ask-page skill, only read) from its own sample spec, in a TEMP skill state
# (ASK_PAGE_STATE_DIR / _PAGES_DIR / _DOWNLOADS / _SETTINGS, APPDATA: never the real ones). The app's own ask pane
# draws its cards (no web view), takes clicks and typing through the window's own handlers (`ask nclick`, `text`,
# `tap tab`), saves the drafts through the deck (askDraft), Done writes the answers file beside the page, and the
# skill's own `ask_page.py read` takes it. A second run on the same deck state shows the drafts again; a guest shows
# the page read-only. Skipped (one line) when python or the skill is not on this box.
if (Want 'askcards') {
  '== askcards: the native ask pane on a real ask page: cards, picks, typing, Tab, drafts, Done, the skill reads the file; a guest is read-only'
  $skill = Join-Path $env:USERPROFILE '.claude-skill-sources\ask-page\skills\ask-page'
  $py = Get-Command python -ErrorAction SilentlyContinue
  if (-not $py -or -not (Test-Path (Join-Path $skill 'scripts\ask_page.py'))) { '  SKIPPED: no python or no ask-page skill here' }
  else {
    # Q1's option b as the skill's own fixture words it (the skill is not part of this tree; its text is its own)
    $q1b = ((Get-Content (Join-Path $skill 'tests\fixtures\spec5.json') -Raw -Encoding UTF8 | ConvertFrom-Json).items[0].options[1].label)
    $q1bRx = [regex]::Escape($q1b)
    $sid = 'cccccccc-dddd-4eee-8fff-000000000001'
    function New-SkillPage {
      $w = New-Temp 'skill'
      foreach ($d in 'state', 'pages', 'dl', 'appdata') { New-Item -ItemType Directory -Force (Join-Path $w $d) | Out-Null }
      [IO.File]::WriteAllText((Join-Path $w 'settings.json'), '{"theme_default":"dark"}')
      $e = @{ ASK_PAGE_STATE_DIR = (Join-Path $w 'state'); ASK_PAGE_PAGES_DIR = (Join-Path $w 'pages'); ASK_PAGE_DOWNLOADS = (Join-Path $w 'dl'); ASK_PAGE_SETTINGS = (Join-Path $w 'settings.json')
        ASK_PAGE_SESSION_ID = $sid; ASK_PAGE_SESSION_NAME = 'native-cards'; ASK_PAGE_ACCOUNT = 'fixture'; APPDATA = (Join-Path $w 'appdata'); PYTHONIOENCODING = 'utf-8' }
      @{ dir = $w; env = $e }
    }
    function Invoke-Skill($k, [string[]]$skArgs) {
      $keep = @{}; foreach ($n in $k.env.Keys) { $keep[$n] = [Environment]::GetEnvironmentVariable($n); [Environment]::SetEnvironmentVariable($n, $k.env[$n]) }
      $keepTerm = $env:TERM_PROGRAM; $env:TERM_PROGRAM = $null
      try { $o = & $py.Source (Join-Path $skill 'scripts\ask_page.py') @skArgs 2>&1 | Out-String; @{ code = $LASTEXITCODE; out = $o } }
      finally { foreach ($n in $keep.Keys) { [Environment]::SetEnvironmentVariable($n, $keep[$n]) }; $env:TERM_PROGRAM = $keepTerm }
    }
    $k = New-SkillPage
    $b = Invoke-Skill $k @('build', (Join-Path $skill 'tests\fixtures\spec5.json'), '--no-open', '--no-carry')
    $pageId = if ($b.out -match '(?m)^id: (\S+)') { $Matches[1] } else { '' }
    Check 'the skill built a page in its TEMP state' ($b.code -eq 0 -and $pageId)
    $pageFile = Get-ChildItem (Join-Path $k.dir 'pages') -Recurse -Filter "$pageId.html" | Select-Object -First 1
    $env:ASK_PAGE_STATE_DIR = $k.env.ASK_PAGE_STATE_DIR; $env:ASK_PAGE_DOWNLOADS = $k.env.ASK_PAGE_DOWNLOADS
    try {
      $st = New-Temp 'st'; $hm = New-Temp 'home'
      # run 1: picks, typing, Tab to the next box, Copy; no Done (the drafts must survive the run)
      $do = "wait 1500; ask open all; wait 1200; ask native; shot shell-askcards-open; ask nclick pick 0 Q1 b; wait 200; ask nclick pick 0 Q2 r:later; wait 200; ask nclick pick 0 Q5 r:both; wait 200; ask nclick box 0 Q3; text the Zebra needs a new head; tap tab; text Plan Saturday, but short; wait 300; ask native; shot shell-askcards-drafted; tap esc; wait 200; ask native; ask dump; ask nclick copy 0 Q3; wait 300; ask native; tap esc; wait 400; ask dump"
      $log = Run 'askcards' $do $st $hm '' -After 600
      Check 'the native pane is the default (no --ask-pane)' ($log -match 'ask pane: native \(askcards\.hpp\)')
      Check 'its page loaded from the file and drew 5 cards (no web view made)' ($log -match ('ask native: .*"key":"ask:' + [regex]::Escape($pageId) + '".*"st":"ready"') -and ([regex]::Matches(($log -split "`n" | Where-Object { $_ -match '^.*ask native: ' } | Select-Object -First 1), '"qid":"Q\d"').Count -eq 5) -and $log -match 'ask web: state \d+ \(2 = ready\) made 0')
      Check 'it asked the deck for its drafts (askDraftGet), as the web pane does' ($log -match ('call askDraftGet \["' + [regex]::Escape($pageId) + '"\]'))
      Check 'a click on option b of Q1: checked, its label in the box' ($log -match ('"checked":\["b"\],"qid":"Q1","state":"answered","text":"' + $q1bRx + '"'))
      Check 'Later on Q2: the state chip says later' ($log -match '"checked":\["r:later"\],"qid":"Q2","state":"later","text":"later"')
      Check 'typed into the Q3 box (a click on it gave it the keys)' ($log -match '"checked":\[\],"qid":"Q3","state":"answered","text":"the Zebra needs a new head"')
      Check 'Tab took the keys to the next box (Q4), typing went there' ($log -match '"checked":\[\],"qid":"Q4","state":"answered","text":"Plan Saturday, but short"')
      Check 'Both on Q5: its words in the box' ($log -match '"checked":\["r:both"\],"qid":"Q5","state":"answered","text":"both"')
      Check 'every change went to the deck as a draft (askDraft, the page''s own counts)' ($log -match ('fire askDraft \["' + [regex]::Escape($pageId) + '",\{"answered":4,"drafted":5,"later":1,'))
      Check 'Copy on a card: its line on the clipboard' ($log -match 'Copied: Q3 \(label printer\): the Zebra needs a new head')
      Check 'Esc in a box leaves it (the pane stays open), a second Esc closes the pane' ($log -match '(?s)"focus":"ask:[^"]*\|Q4".*?"focus":null.*?ask pane: mode ''all'' open 1.*ask pane: mode '''' open 0')
      # run 2: the same deck state: the drafts come back; then Done
      $do2 = "wait 1500; ask open all; wait 1200; ask native; ask nclick ui done 0; wait 2000; ask dump; ask open all; wait 600; ask native; shot shell-askcards-done"
      $log2 = Run 'askcards-2' $do2 $st $hm '' -After 600
      Check 'the drafts came back from the deck in a new run (the same S the Electron deck keeps)' ($log2 -match ('"checked":\["b"\],"qid":"Q1","state":"answered","text":"' + $q1bRx + '"') -and $log2 -match '"checked":\[\],"qid":"Q4","state":"answered","text":"Plan Saturday, but short"')
      $ans = Join-Path $pageFile.DirectoryName "$pageId.answers.json"
      Check 'Done: askDone went to the deck, the answers file is beside the page' ($log2 -match ('call askDone \["' + $sid + '"') -and (Test-Path $ans))
      Check 'the section reads Sent; the page is read-only now' ($log2 -match '"done":\{"disabled":true,"text":"Sent"' -and $log2 -match '"locked":true')
      $r = Invoke-Skill $k @('read', $pageId)
      $state = Get-Content (Join-Path $k.env.ASK_PAGE_STATE_DIR "$sid.json") -Raw -Encoding UTF8 | ConvertFrom-Json
      $q1 = $state.answers.'r1-Q1'; $q2 = $state.answers.'r1-Q2'; $q4 = $state.answers.'r1-Q4'
      Check 'the skill''s own `ask_page.py read` takes the file, no warning' ($r.code -eq 0 -and $r.out -match '(?m)^file: ' -and $r.out -notmatch 'WARNING')
      Check '...and records each answer as the pane gave it' ($q1.status -eq 'answered' -and $q1.choice -eq 'b' -and $q1.answer -eq $q1b -and $q2.status -eq 'later' -and $q2.reply -eq 'later' -and $q4.choice -eq 'a' -and $q4.answer -eq 'Plan Saturday, but short')
      # a guest (another deck's fresh heartbeat): the page is drawn read-only, nothing is written
      $g = New-SkillPage
      $gb = Invoke-Skill $g @('build', (Join-Path $skill 'tests\fixtures\spec5.json'), '--no-open', '--no-carry')
      $gid = if ($gb.out -match '(?m)^id: (\S+)') { $Matches[1] } else { '' }
      $foreign = '{"kind":"claude-deck-asks","pid":1,"at":"' + [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + '","shows_pages":true}'
      [IO.File]::WriteAllText((Join-Path $g.env.ASK_PAGE_STATE_DIR 'deck.json'), $foreign)
      $env:ASK_PAGE_STATE_DIR = $g.env.ASK_PAGE_STATE_DIR; $env:ASK_PAGE_DOWNLOADS = $g.env.ASK_PAGE_DOWNLOADS
      $log3 = Run 'askcards-guest' "wait 1500; ask open all; wait 1200; ask nclick pick 0 Q1 a; wait 300; ask nclick box 0 Q2; text nope; wait 400; ask native; ask dump; shot shell-askcards-guest" (New-Temp 'st') (New-Temp 'home') '' -After 600
      Check 'guest: drawn read-only (locked), a click and typing change nothing, no draft is sent' ($log3 -match ('"key":"ask:' + [regex]::Escape($gid) + '".*"locked":true') -and $log3 -match '"checked":\[\],"qid":"Q1","state":"open","text":""' -and $log3 -match '"checked":\[\],"qid":"Q2","state":"open","text":""' -and $log3 -notmatch 'fire askDraft')
      Check 'guest: the section says why, Done is off' ($log3 -match '"flash":"Read-only here: another deck is serving the ask pages' -and $log3 -match '"done":\{"disabled":true')
      Check 'scene == whole frame (the native pane drawn by the scene as by a whole frame)' ((NoMismatch $log) -and (NoMismatch $log2) -and (NoMismatch $log3))
    } finally { $env:ASK_PAGE_STATE_DIR = $null; $env:ASK_PAGE_DOWNLOADS = $null }
  }
}

# ---------------------------------------------------------------- 13. the office as a kind of screen: the driver claim, the refusal note, the pages
if (Want 'office') {
  '== office: local mode is kind `office` under the driver claim; input:refused is said in the pane; Settings -> Phone says when the pages are missing'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $log = Run 'office-old' "click 60 800; wait 1400; dump drive; event input:refused {'id':'x','reason':'phone'}; wait 150; shot shell-office-old-note; event input:refused {'id':'x','reason':'phone-attached'}; wait 100; dump state" $st $hm
  Check 'claim off (the default): the old words, the three-state button' ($log -match 'drive words old driver auto look plain')
  Check 'input:refused `phone` is said in the pane, in main''s words' ($log -match "input refused \(phone\): note 'The phone is driving, so typing here is ignored\. Click the green phone button at the bottom of the sidebar to switch to desk\.'")
  Check 'input:refused `phone-attached` likewise' ($log -match "input refused \(phone-attached\): note 'The phone is driving this session \(it is open on the phone\), so typing here is ignored\. Close it on the phone, or set the driver to desk with the phone button at the bottom of the sidebar\.'")
  $st = New-Temp 'st'; $empty = New-Temp 'noassets'
  $do = "click 60 800; wait 1400; dump drive; zone tool drive-btn; wait 400; dump drive; shot shell-office-claim-mine; event input:refused {'id':'x','reason':'remote-showing'}; wait 100; shot shell-office-claim-note; event input:refused {'id':'x','reason':'remote'}; zone tool drive-btn; wait 400; dump drive; call setDriver ['remote']; wait 400; dump drive; call setDriver ['phone']; wait 300; dump drive; zone tool settings-btn; wait 600; dump phone; wheel 300 400 -6000; wait 200; wheel 300 400 -6000; wait 200; shot shell-office-pages-missing"
  $log = Run 'office-claim' $do $st $hm ('--driver-claim --assets "' + $empty + '"')
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'with the claim the deck speaks the new words (sizeBy office)' ($log -match 'drive words new driver auto look auto' -and $log -match 'sizeBy "office"')
  Check 'auto: the tip names this PC as the office deck' ($log -match 'drive tip Auto: a phone attached to a session drives its size, else a remote desk showing it, else this PC \(the office deck\)\. Click to drive from here\.')
  Check 'ONE click claims the size for the OFFICE (setDriver office), the button is "mine"' ($log -match 'call setDriver \["office"\]' -and $log -match 'drive words new driver office look mine')
  Check 'its tip: Sizes follow this PC ... click to give it back' ($log -match 'drive tip Sizes follow: this PC \(the office deck\)\. The phone and a remote desk show its size\. Click to let whoever is looking drive \(auto\)\.')
  Check 'a click on its own claim releases to auto' ($log -match '(?s)call setDriver \["office"\].*call setDriver \["auto"\]')
  Check 'the remote desk''s claim reads "other" here, and one click would take it back' ($log -match 'drive words new driver remote look other' -and $log -match 'drive tip Sizes follow: the remote desk\. That is for the sessions it shows: there the office deck shows its size and cannot type\. The office deck sizes the rest\. Click to drive from here\.')
  Check 'the phone''s claim: the office mirrors (mirror set, sizeBy phone)' ($log -match 'drive tile \S+ sizeBy "phone" mirror \{"cols":\d+,"rows":\d+\}')
  Check 'input:refused `remote-showing` / `remote` in the branch''s words' (($log -match "input refused \(remote-showing\): note 'The remote desk is driving this session \(it is on screen there\), so typing here is ignored\. Hide it there, or: Click the driver button at the bottom of the sidebar to drive from this PC\.'") -and ($log -match "input refused \(remote\): note 'The remote desk is driving this session, so typing here is ignored\. Click the driver button"))
  Check 'phoneStatus.assets is missing, with the folder that was tried' ($log -match '"assetRoot":"[^"]*cdn-shell-noassets-[^"]*","assets":"missing"')
  Check 'Settings -> Phone says so, naming the folder' ($log -match "settings phone-assets 'The pages are not beside the exe \(renderer\\phone\\index\.html\): looked in \S*cdn-shell-noassets-")
  # no --assets: an exe in windows\build of a checkout finds the checkout's pages; any other exe looks beside itself ONLY
  $st = New-Temp 'st'
  $log = Run 'office-assets' 'wait 300; dump phone; ask link https://example.com/a?b=1; ask link file:///C:/Windows/win.ini; ask link file:///C:/Windows/notepad.exe; ask link javascript:alert(1); wait 300' $st $hm
  Check 'a link on an ask page goes through the deck (askOpenLink): http(s) to the browser (a test only logs it)' ($log -match 'call askOpenLink \["https://example\.com/a\?b=1"\]' -and $log -match '(?m)^OPEN https://example\.com/a\?b=1')
  Check 'a file link opens in its own program; something that would RUN is only shown in its folder; anything else is dropped' ($log -match '(?m)^OPEN PATH C:\\Windows\\win\.ini' -and $log -match '(?m)^SHOW IN FOLDER explorer\.exe /select,"C:\\Windows\\notepad\.exe"' -and $log -notmatch '(?m)^OPEN.*javascript')
  if ((Split-Path -Parent $exe) -match '\\windows\\build$') { Check 'no --assets, the exe in windows\build: the checkout''s pages are found' ($log -match '"assets":"ok"') }
  else { Check 'no --assets, an exe outside a checkout: its own folder is the one tried' ($log -match ('"assetRoot":"' + [regex]::Escape((Split-Path -Parent $exe).Replace('\', '\\')) + '"')) }
  $lone = New-Temp 'lone'; $deep = Join-Path $lone 'a\b'; New-Item -ItemType Directory -Force $deep | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $lone 'renderer\phone') | Out-Null; Set-Content -Path (Join-Path $lone 'renderer\phone\index.html') -Value '<!doctype html><p>a page ABOVE the exe' -Encoding ascii
  Copy-Item $exe (Join-Path $deep $DeckEdition.Exe)
  $st = New-Temp 'st'; $png = Shot 'office-lone'; Remove-Item $png, "$png.log.txt" -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath (Join-Path $deep $DeckEdition.Exe) -ArgumentList "--local --state-dir `"$st`" --home `"$hm`" --fonts-dir none --after 600 --shell cmd.exe --shot `"$png`" --do `"wait 300; dump phone`"" -Wait -PassThru
  Keep-Proc $p
  $log = if (Test-Path "$png.log.txt") { Get-Content "$png.log.txt" -Raw -Encoding UTF8 } else { '' }
  Check 'an exe copied elsewhere never climbs: a renderer\ two folders above it is NOT served (assets missing, its own folder tried)' ($p.ExitCode -eq 0 -and $log -match '"assets":"missing"' -and $log -match ('"assetRoot":"' + [regex]::Escape($deep.Replace('\', '\\')) + '"'))
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

# ---------------------------------------------------------------- 14. --register / --unregister
if (Want 'register') {
  '== register: the Start-menu shortcut with the AppUserModelID, written and removed by the exe itself - in a TEMP folder, never the real Start menu'
  . (Join-Path $root 'windows\package\ShortcutTools.ps1')
  $sc = New-Temp 'shortcuts'; $lnk = Join-Path $sc $DeckShortcutName
  function Reg([string]$a) { $p = Start-Process -FilePath $exe -ArgumentList $a -Wait -PassThru; Keep-Proc $p; $p.ExitCode }
  $before = @(Get-Process -Name $DeckEdition.ExeBase -ErrorAction SilentlyContinue).Count
  $code = Reg "--register --shortcut-dir `"$sc`" --shortcut-args `"--local`""
  Check '--register exits 0 and says "created"' ($code -eq 0 -and (Get-Content (Join-Path $sc 'register.log') -Raw) -match 'register: created')
  $r = if (Test-Path $lnk) { Read-DeckShortcut $lnk } else { $null }
  Check 'the shortcut is there, points at this exe, with --local' ($r -and (Test-SamePath $r.Target $exe) -and $r.Arguments -eq '--local')
  Check 'it carries notify.hpp''s AppUserModelID' ($r -and $r.AppId -eq $DeckAppId)
  $code = Reg "--register --shortcut-dir `"$sc`""
  $r = Read-DeckShortcut $lnk
  Check 'again without --shortcut-args: updated, the arguments kept' ($code -eq 0 -and (Get-Content (Join-Path $sc 'register.log') -Raw) -match 'register: updated' -and $r.Arguments -eq '--local')
  $code = Reg "--unregister --shortcut-dir `"$sc`""
  Check '--unregister removes it (exit 0)' ($code -eq 0 -and -not (Test-Path $lnk) -and (Get-Content (Join-Path $sc 'register.log') -Raw) -match 'unregister: removed')
  $code = Reg "--unregister --shortcut-dir `"$sc`""
  Check '--unregister with nothing there: exit 0, "absent"' ($code -eq 0 -and (Get-Content (Join-Path $sc 'register.log') -Raw) -match 'unregister: absent')
  # a shortcut the user pointed at something else: stamped, never re-pointed, never removed
  $other = Join-Path $env:WINDIR 'System32\notepad.exe'
  Write-DeckShortcut $lnk $other '--mine'
  $code = Reg "--register --shortcut-dir `"$sc`" --shortcut-args `"--local`""
  $r = Read-DeckShortcut $lnk
  Check 'a shortcut pointing elsewhere keeps its target and arguments (stamped only)' ($code -eq 0 -and (Test-SamePath $r.Target $other) -and $r.Arguments -eq '--mine' -and $r.AppId -eq $DeckAppId)
  $code = Reg "--unregister --shortcut-dir `"$sc`""
  Check '... and --unregister leaves it (exit 2, "foreign")' ($code -eq 2 -and (Test-Path $lnk))
  Check 'neither mode left a state folder, a window or a process' ((@(Get-ChildItem $sc -Force).Count -eq 2) -and (@(Get-Process -Name $DeckEdition.ExeBase -ErrorAction SilentlyContinue).Count -eq $before))
}

# ---------------------------------------------------------------- 15. a window the CORE asks for, the newer toast kinds, the first window's place
if (Want 'corewindow') {
  '== corewindow: Lifecycle::openWindow (the on-call''s desk with no free cell) makes a window for a client the core registered; desk / memory toasts'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; life deskwindow; wait 1200; dump windows; win 1 text echo in-the-desk-window; win 1 key enter; wait 500; win 1 dump screen; win 1 shot shell-corewindow-w1; toast desk; toast click; wait 200; toast memory; toast dump; win 1 close; wait 600; dump windows; dump tiles"
  $log = Run 'corewindow' $do $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'the core registered the client, the shell made its window (inactive), through core::Lifecycle' ($log -match 'window opened by the deck for client \d+ \(inactive\)' -and $log -match 'desk window: client \d+ registered by the core, the shell made its window, 2 windows')
  Check 'that window is a client of the deck holding the tile (inWindow 1, shownBy win:1)' ($log -match 'windows 2 \(deck clients 2\)' -and $log -match 'inWindow 1 shownBy \["win:1"\]')
  Check 'it is a terminal of its own: typed there, ran there' ($log -match '(?s)screen \S+ \d+x\d+.*in-the-desk-window')
  Check 'a `desk` toast is recorded and its click is routed by the core to the window holding the tile' ($log -match "toast \[desk\] tile \S+ silent 0" -and $log -match 'toast click -> window w1 comes up on tile \S+ \(routed by the core\)')
  Check 'a `memory` toast is recorded; nothing was shown for real' ($log -match "toast \[memory\]" -and $log -match 'toasts 2, flashes 0, real toasts shown 0')
  Check 'closing it leaves the session running in the first window''s list' ($log -match '(?s)windows 1 \(deck clients 1\).*tile \S+ shell ''cmd'' status \S+ pinned 0 exited 0')
  Check 'scene == whole frame in every shot' (NoMismatch $log)
  # the first window's place: the deck's own record (tiles.json windows[0].bounds, what main.js keeps) before config.json's
  $st = New-Temp 'st'; New-Item -ItemType Directory -Force (Join-Path $st 'local') | Out-Null
  [IO.File]::WriteAllText((Join-Path $st 'local\tiles.json'), '{"tiles":[],"groups":[],"windows":[{"bounds":{"x":111,"y":222,"width":1333,"height":777,"maximized":false},"layout":{}}]}')
  [IO.File]::WriteAllText((Join-Path $st 'config.json'), '{"mode":"local","window":[5,6,905,706,false]}')
  $log = Run 'place' 'wait 200' $st $hm
  Check 'tiles.json windows[0].bounds is the first window''s place, over config.json''s' ($log -match 'first window: the deck''s saved place \{"height":777,"maximized":false,"width":1333,"x":111,"y":222\} \(tiles\.json\) is used, not config\.json''s \{"height":700,"maximized":false,"width":900,"x":5,"y":6\}')
  $st = New-Temp 'st'; [IO.File]::WriteAllText((Join-Path $st 'config.json'), '{"mode":"local","window":[5,6,905,706,false]}')
  $log = Run 'place2' 'wait 200' $st $hm
  Check 'without one, config.json''s place stands' ($log -match 'first window: config\.json''s place \{"height":700,"maximized":false,"width":900,"x":5,"y":6\} \(tiles\.json has none\)')
}

# ---------------------------------------------------------------- 16. an input method composing in a terminal
if (Want 'ime') {
  '== ime: the composing text is drawn in the grid at the cursor (xterm''s composition view); what is committed is typed'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; text echo; ime comp nihon; wait 100; shot shell-ime-a-composing; ime comp NIHONGO; wait 100; shot shell-ime-b-converted; ime commit NIHONGO; wait 400; shot shell-ime-c-committed; ime end; dump screen; zone tool search-btn; wait 200; ime comp zz; wait 100"
  $log = Run 'ime' $do $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'the composing text is taken at the cursor, and replaced as the input method converts it' ($log -match "ime composing 'nihon' at \d+,\d+" -and $log -match "ime composing 'NIHONGO' at")
  Check 'nothing is typed while composing' ($log -notmatch '(?s)ime composing ''nihon''.*input \S+ n.*ime commit')
  Check 'the commit is typed character by character and the composition view goes' ($log -match "(?s)ime commit 'NIHONGO'.*input \S+ N.*input \S+ O.*ime composing ''")
  Check 'the committed text reached the shell' ($log -match '(?s)screen .*echoNIHONGO')
  Check 'a text box of the deck keeps Windows'' own composition window' ($log -match 'ime: no terminal has the keys')
  Check 'scene == whole frame in every shot' (NoMismatch $log)
  $a = Shot 'x'; $dir = Split-Path -Parent $a
  Check 'the composition view changed the picture, and went again' ((Test-Path (Join-Path $dir 'n-shell-ime-a-composing.png')) -and ((Get-FileHash (Join-Path $dir 'n-shell-ime-a-composing.png')).Hash -ne (Get-FileHash (Join-Path $dir 'n-shell-ime-c-committed.png')).Hash))
}

# ---------------------------------------------------------------- 17. the css transitions
if (Want 'trans') {
  '== trans: the pin''s 120 ms fade, the Settings panel''s 160 ms rise, the group chevron, on the transition clock; off = they jump'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; trans dump; hzone tile-pin sel; shot shell-trans-off-pin; move 700 400; trans 1000; wait 50; trans dump; hzone tile-pin sel; trans dump; shot shell-trans-pin-0; trans 1060; shot shell-trans-pin-60; trans 1120; shot shell-trans-pin-120; trans dump; trans 2000; zone tool settings-btn; wait 500; trans dump; shot shell-trans-set-0; trans 2080; trans dump; shot shell-trans-set-80; trans 2160; trans dump; shot shell-trans-set-160; ozone set-btn cancel; wait 200; trans 3000; fakeset settings.animations false; wait 100; hzone tile-pin sel; trans dump; trans off; trans dump"
  $log = Run 'trans' $do $st $hm
  $t = [regex]::Matches($log, 'trans: on (\d) clock (\d+) ms, sidebar transitions under way (\d), settings rise ([\d.]+), timer wanted (\d)')
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'nine readings' ($t.Count -eq 9)
  if ($t.Count -eq 9) {
    Check 'a test''s transitions are off until a script sets the clock' ($t[0].Groups[1].Value -eq '0')
    Check 'clock set, nothing changing: nothing under way, no timer wanted' ($t[1].Groups[1].Value -eq '1' -and $t[1].Groups[3].Value -eq '0' -and $t[1].Groups[5].Value -eq '0')
    Check 'the pointer reaches the pin: a transition is under way and the timer is wanted' ($t[2].Groups[3].Value -eq '1' -and $t[2].Groups[5].Value -eq '1')
    Check '120 ms later it has settled' ($t[3].Groups[3].Value -eq '0' -and $t[3].Groups[5].Value -eq '0')
    Check 'Settings opens: the rise starts at 0' ([double]$t[4].Groups[4].Value -eq 0 -and $t[4].Groups[5].Value -eq '1')
    Check 'half way (80 ms, ease-out) it is past half' ([double]$t[5].Groups[4].Value -gt 0.5 -and [double]$t[5].Groups[4].Value -lt 1)
    Check '160 ms: there' ([double]$t[6].Groups[4].Value -eq 1 -and $t[6].Groups[5].Value -eq '0')
    Check 'Settings -> Animations off: no transition (it jumps)' ($t[7].Groups[1].Value -eq '0' -and $t[7].Groups[3].Value -eq '0')
  }
  $dir = Split-Path -Parent (Shot 'x'); $h = @{}; foreach ($n in 'off-pin', 'pin-0', 'pin-60', 'pin-120', 'set-0', 'set-80', 'set-160') { $f = Join-Path $dir "n-shell-trans-$n.png"; $h[$n] = if (Test-Path $f) { (Get-FileHash $f).Hash } else { $n } }
  Check 'the pin fades in: three different pictures, the last one equal to the jump' ($h['pin-0'] -ne $h['pin-60'] -and $h['pin-60'] -ne $h['pin-120'] -and $h['pin-120'] -eq $h['off-pin'])
  Check 'the panel rises: three different pictures' ($h['set-0'] -ne $h['set-80'] -and $h['set-80'] -ne $h['set-160'])
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

# ---------------------------------------------------------------- 18. the special prompt form: the time box, the known triggers
if (Want 'holdform') {
  '== holdform: the time box takes and shows what a datetime-local box does; the known triggers are listed under the trigger box'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $calDay = (Get-Date).AddMonths(1).ToString('yyyy-MM') + '-15'   # the calendar's next month, the 15th
  $do = "stamp 11/19/2030, 03:15 PM; stamp 11/19/2030 03:15 PM; stamp 2030-11-19T15:15; stamp 2030-11-19 15:15; stamp 02/30/2030, 01:00 PM; stamp 11/19/2030, 13:15 PM; stamp 11/19/2030, 03:15; stamp 2030-11-19T15:15:30; stamp 2030-11-19T03:15 PM; stamp garbage; stamp 09:00 AM; stamp 17:00; stamp 12:00 AM; stamp 12:30 PM; stamp 24:00; stamp 9:5; " +
    "click 60 800; wait 1400; faketile kind 'claude'; fakeset triggers [{'name':'ci_green','set':false},{'name':'migration_ok','set':true},{'name':'deploy_done','set':false}]; wait 100; rzone tile sel; menu Special prompt; wait 200; zone hold sel kind:triggered; wait 100; zone edit hold-trig; wait 100; shot shell-hold-trig-all; dump ozones; text mig; wait 100; shot shell-hold-trig-typed; dump ozones; ozone holdtrig migration_ok; wait 100; shot shell-hold-trig-picked; dump ozones; zone edit hold-text; text run the checks; zone hold sel add; wait 200; zone hold sel kind:timed; wait 100; shot shell-hold-timed; zone hold sel cal; wait 100; shot shell-hold-cal; dump ozones; ozone holdpop next; wait 100; ozone holdpop $calDay; wait 100; dump ozones; zone edit hold-time; key ctrl+a; text 01/01/2020, 01:00 AM; zone hold sel add; wait 100; dump state; shot shell-hold-passed"
  $log = Run 'holdform' $do $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'what the box shows (MM/DD/YYYY, hh:mm AM) is taken, with or without the comma' (([regex]::Matches($log, "stamp '11/19/2030,? 03:15 PM' -> 11/19/2030, 03:15 PM \((\d+)\)")).Count -eq 2)
  Check 'the value format (YYYY-MM-DDTHH:MM) is taken and means the same minute' ($log -match "stamp '2030-11-19T15:15' -> 11/19/2030, 03:15 PM" -and $log -match "stamp '2030-11-19 15:15' -> 11/19/2030, 03:15 PM")
  Check 'what the browser''s box could not hold is no value: a day that does not exist, 13 PM, no AM/PM, seconds, words' (([regex]::Matches($log, "stamp '(02/30/2030, 01:00 PM|11/19/2030, 13:15 PM|11/19/2030, 03:15|2030-11-19T15:15:30|2030-11-19T03:15 PM|garbage)' -> no value")).Count -eq 6)
  Check 'times of day: 09:00 AM = 540, 17:00 = 1020, 12:00 AM = 0, 12:30 PM = 750; 24:00 and 9:5 are none' ($log -match "'09:00 AM' -> no value; as a time of day: 540" -and $log -match "'17:00' .* time of day: 1020" -and $log -match "'12:00 AM' .* time of day: 0" -and $log -match "'12:30 PM' .* time of day: 750" -and $log -match "'24:00' .* time of day: -1" -and $log -match "'9:5' .* time of day: -1")
  Check 'the trigger box drops its list of the three known triggers while it has the focus' ($log -match '(?s)popup 2.*holdtrig ci_green .*holdtrig migration_ok .*holdtrig deploy_done ')
  Check 'typing narrows the list' ($log -match '(?s)n-shell-hold-trig-typed\.png.*holdtrig migration_ok' -and $log -notmatch '(?s)n-shell-hold-trig-typed\.png.*holdtrig ci_green')
  Check 'a click on a name fills the box and puts the list away' ($log -match "hold trigger picked migration_ok" -and $log -match "trigger 'migration_ok' popup 0")
  Check 'the calendar icon opens a month; next month, a day: that date in the box, its time kept' ($log -match 'hold calendar open \d+-\d+' -and $log -match ('hold calendar picked ' + $calDay + ' -> \d\d/15/\d{4}, \d\d:\d\d [AP]M') -and $log -match 'holdpop prev ' -and $log -match 'holdpop next ')
  Check 'a click on a name fills the box, and Add sends it (holdAdd ... trigger)' ($log -match 'call holdAdd \["\S+",\{"kind":"triggered","text":"run the checks","trigger":"migration_ok"\}\]')
  Check 'a time that has passed is refused in the form, as in Electron' ($log -notmatch '"kind":"timed"')
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

# ---------------------------------------------------------------- 20. the tile strip and the keyboard menu (2026-11-23)
# sleep / close are icons with their tooltips, a move is "-> <account>", no "special" on the strip (the tile menu has it,
# and a Prompts submenu); Shift+F10 and the Menu key open the selected tile's menu, the arrows walk it.
if (Want 'strip') {
  '== strip: icons, "-> <account>", no special; the tile menu from the keyboard, Up / Down / Right / Left / Esc'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; faketile kind 'claude'; faketile sessionId 'strip-s1'; faketile status 'idle'; wait 300; dump zones; dump tips; shot shell-strip; " +
    "key shift+f10; wait 150; shot shell-kmenu-a; dump ozones; key down; wait 80; shot shell-kmenu-b; dump ozones; menu Prompts; wait 150; shot shell-kmenu-c; dump ozones; " +
    "key down; wait 80; shot shell-kmenu-d; dump ozones; key left; wait 150; shot shell-kmenu-e; dump ozones; key right; wait 150; shot shell-kmenu-f; dump ozones; " +
    "key esc; wait 80; shot shell-kmenu-g; dump ozones; key apps; wait 150; shot shell-kmenu-h; dump ozones; key esc; wait 80; key f10; wait 80; dump ozones"
  $log = Run 'strip' $do $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'the strip: sleep and close, as icons with their tooltips Sleep / Close' ($log -match "(?m)^tip tile-action \S+ \(sleep\) 'Sleep'" -and $log -match "(?m)^tip tile-close \S+ 'Close'")
  Check 'the strip: a move button with the tooltip "Move to <account>"' ($log -match "(?m)^tip tile-action \S+ \(open:\S+\) 'Move to ")
  Check 'the strip: no special' ($log -notmatch '(?m)^zone tile-action \S+ \(special\)')
  $seg = { param($a, $b) $i = $log.IndexOf("n-shell-kmenu-$a.png"); $j = if ($b) { $log.IndexOf("n-shell-kmenu-$b.png") } else { $log.Length }; if ($i -lt 0 -or $j -lt $i) { '' } else { $log.Substring($i, $j - $i) } }
  $a = & $seg 'a' 'b'; $b = & $seg 'b' 'c'; $c = & $seg 'c' 'd'; $d = & $seg 'd' 'e'; $e = & $seg 'e' 'f'; $f = & $seg 'f' 'g'; $g = & $seg 'g' 'h'; $h = & $seg 'h' ''
  $rows = { param($t) @([regex]::Matches($t, '(?m)^  (.+?)( \(sub\))?( \(lit\))? \[\d') | ForEach-Object { $_.Groups[1].Value + $(if ($_.Groups[2].Success) { ' >' } else { '' }) + $(if ($_.Groups[3].Success) { ' *' } else { '' }) }) }
  $ra = & $rows $a; $rb = & $rows $b
  Check 'Shift+F10: the selected tile''s menu, its first row lit' ($a -match '(?m)^ctx ' -and $ra.Count -gt 3 -and $ra[0] -match ' \*$')
  Check 'the menu has Special prompt... and Prompts as a submenu' (($ra -join '|') -match 'Special prompt' -and ($ra -contains 'Prompts >'))
  Check 'Down: the second row lit' ($rb.Count -gt 1 -and $rb[1] -match ' \*$' -and $rb[0] -notmatch ' \*$')
  Check 'a click on Prompts: its submenu - Pause, Pause until restart, Make auditor...' ($c -match '(?m)^  Pause \[' -and $c -match '(?m)^  Pause until restart' -and $c -match '(?m)^  Make auditor')
  Check 'Down in the submenu: its first row lit' ($d -match '(?m)^  Pause \(lit\)')
  Check 'Left: back to the menu, Prompts lit' ($e -match '(?m)^  Prompts \(sub\) \(lit\)' -and $e -match '(?m)^  Rename')
  Check 'Right on Prompts: the submenu, its first row lit' ($f -match '(?m)^  Pause \(lit\)')
  Check 'Esc closes it' ($g -notmatch '(?m)^ctx ')
  Check 'the Menu key opens it too' ($h -match '(?m)^ctx ' -and $h -match '(?m)^  Rename')
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

# ---------------------------------------------------------------- the sidebar's narrowest follows the tile strip
if (Want 'sbmin') {
  '== sbmin: a narrow drag stops where the widest tile strip fits at 11px (2026-11-23; a fixed 220 before); an account more, wider'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $a3 = "[{'id':'vv','label':'vv','abbr':'vv'},{'id':'ms','label':'ms','abbr':'ms'},{'id':'ts','label':'ts','abbr':'ts'}]"
  $a4 = $a3.TrimEnd(']') + ",{'id':'thirdacct','label':'thirdacct','abbr':'tw'}]"
  $drag = "downzone resizer; move 120 400; move 30 400; up 30 400; wait 200"
  $do = "click 60 800; wait 1400; fakeset accounts [{'id':'vv','label':'vv','abbr':'vv'}]; faketile kind 'claude'; faketile account 'vv'; faketile sessionId 'sbmin-s1'; faketile status 'idle'; wait 300; " +
    "$drag; dump sidebar; fakeset accounts $a3; wait 200; $drag; dump sidebar; dump zones; shot shell-sbmin-3; fakeset accounts $a4; wait 200; $drag; dump sidebar; dump zones; shot shell-sbmin-4"
  $log = Run 'sbmin' $do $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  $sb = @([regex]::Matches($log, '(?m)^sidebar width ([\d.]+) min ([\d.]+) remembered ([\d.]+)') | ForEach-Object { , @([double]$_.Groups[1].Value, [double]$_.Groups[2].Value, [double]$_.Groups[3].Value) })
  Check 'three readings of the sidebar' ($sb.Count -eq 3)
  if ($sb.Count -eq 3) {
    "  (the narrowest: 1 account $($sb[0][1]), 3 (vv / ms / ts) $($sb[1][1]), + thirdacct $($sb[2][1]); was 220)"
    Check 'one account: the drag stops at the 160 floor (the title line)' ($sb[0][0] -eq 160 -and $sb[0][1] -eq 160 -and $sb[0][2] -eq 160)
    Check 'three accounts: it stops at the strip''s width, wider than the floor' ($sb[1][0] -eq $sb[1][1] -and $sb[1][2] -eq $sb[1][1] -and $sb[1][1] -gt 160)
    Check 'a long label more: it stops wider again' ($sb[2][0] -eq $sb[2][1] -and $sb[2][1] -gt $sb[1][1])
    # every button of the selected tile's strip whole and at 11px: the close icon 25 wide (11 + 2 x 6 + 2), its right edge inside the tile (the sidebar - 9)
    foreach ($i in 1, 2) {
      $seg = if ($i -eq 1) { $log.Substring(0, $log.IndexOf('n-shell-sbmin-3.png')) } else { $log.Substring($log.IndexOf('n-shell-sbmin-3.png')) }
      $z = [regex]::Matches($seg, '(?m)^zone (tile-action|tile-close) \S+(?: \((\S+)\))? \[(-?[\d.]+),(-?[\d.]+) ([\d.]+)x([\d.]+)\]')
      $close = @($z | Where-Object { $_.Groups[1].Value -eq 'tile-close' }) | Select-Object -Last 1
      $moves = @($z | Where-Object { $_.Groups[2].Value -like 'open:*' })
      $okClose = $close -and ([double]$close.Groups[5].Value -eq 25) -and ([double]$close.Groups[3].Value + 25 -le $sb[$i][0] - 9 + 0.6)
      Check "$(if ($i -eq 1) { 'three' } else { 'four' }) accounts: the strip whole at 11px (the close icon 25 wide, inside the tile; $($moves.Count) moves)" ($okClose -and $moves.Count -eq ($i + 1))
    }
  }
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

# ---------------------------------------------------------------- 19. the dragged tile's picture outside the window
if (Want 'ghost') {
  '== ghost: a tile dragged out of the window carries its picture (a layered window in a real run; a test only renders the bitmap)'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $do = "click 60 800; wait 1400; downzone tile sel; move 400 300; dump ghost shell-ghost-inside; move -80 300; dump ghost shell-ghost-outside; up -80 300; wait 1200; dump windows"
  $log = Run 'ghost' $do $st $hm
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'the picture is rendered for the dragged tile' ($log -match 'ghost \d+x\d+ px for \S+, pointer inside the window, saved 1')
  Check 'outside the window the same picture; a test makes no window for it' ($log -match 'ghost \d+x\d+ px for \S+, pointer outside the window, saved 1 n-shell-ghost-outside\.png, window made 0')
  $f = Join-Path (Split-Path -Parent (Shot 'x')) 'n-shell-ghost-outside.png'
  Check 'the bitmap is a real picture (not empty)' ((Test-Path $f) -and (Get-Item $f).Length -gt 600)
  Check 'the drop outside still opens the window there' ($log -match 'window opened: w1 holding \S+ at -?\d+,\d+, 2 windows')
}

# ---------------------------------------------------------------- auditors and their children, Pause / Resume, through the exe's menus
# Three Claude tiles the deck restores from a seeded tiles.json; what it starts for each is windows/test/fakeclaude.cpp,
# a STAND-IN (never Claude: it writes the live record the deck reads, logs what is typed into it, and turns busy / idle
# as a session would), put at <fake home>\.local\bin\claude.exe and named in CLAUDE_DECK_CLAUDE too, so the real
# claude.exe is never what the deck finds (--allow-claude is on for this one run, the deck's newTile stays blocked).
# The run: the tile menu from the keyboard (Shift+F10 -> Prompts -> Make auditor... -> the name box), "Set as child of"
# (its submenu), a file in the auditor's inbox typed into the child, Settings -> auditorAutoSend off -> the child's
# row "from auditor 1" with Send / x, both clicked; auto-send on again: a file waits while the child's turn runs;
# un-child (the auditor told "CHILD 1 left"), a second child, Pause and Resume from the Prompts submenu, un-make.
if (Want 'auditors') {
  '== auditors: make / child / inbox / Send / x / un-child / un-make and Pause / Resume, through the exe''s menus (a stand-in for claude.exe)'
  $st = New-Temp 'st'; $hm = New-Temp 'home'; $fcb = New-Temp 'fc'
  $gbin = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Directory -Filter 'BrechtSanders.WinLibs*' -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'mingw64\bin' } | Where-Object { Test-Path (Join-Path $_ 'g++.exe') } | Select-Object -First 1
  $fake = Join-Path $hm '.local\bin\claude.exe'
  New-Item -ItemType Directory -Force (Split-Path $fake) | Out-Null
  if ($gbin) {
    $savedPath = $env:PATH; $env:PATH = "$gbin;$env:PATH"
    & (Join-Path $gbin 'g++.exe') -std=c++17 -O1 -static -static-libgcc -static-libstdc++ (Join-Path $PSScriptRoot 'fakeclaude.cpp') -o (Join-Path $fcb 'claude.exe')
    $env:PATH = $savedPath
    if (Test-Path (Join-Path $fcb 'claude.exe')) { Copy-Item (Join-Path $fcb 'claude.exe') $fake }
  }
  Check 'the stand-in claude.exe is built into the fake home' (Test-Path $fake)
  if (Test-Path $fake) {
    $A = 'a0000000-0000-4000-8000-00000000000a'; $C1 = 'c1000000-0000-4000-8000-0000000000c1'; $C2 = 'c2000000-0000-4000-8000-0000000000c2'
    $sA = '5a000000-0000-4000-8000-00000000005a'; $s1 = '51000000-0000-4000-8000-000000000051'; $s2 = '52000000-0000-4000-8000-000000000052'
    $loc = Join-Path $st 'local'; New-Item -ItemType Directory -Force $loc | Out-Null
    $now = [double][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $tl = @(@($A, $sA, 'audit one'), @($C1, $s1, 'child one'), @($C2, $s2, 'child two')) | ForEach-Object { [ordered]@{ id = $_[0]; kind = 'claude'; account = 'claude'; sessionId = $_[1]; cwd = $hm; pinned = $false; createdAt = $now; title = $_[2] } }
    [IO.File]::WriteAllText((Join-Path $loc 'tiles.json'), (@{ tiles = @($tl) } | ConvertTo-Json -Depth 5))
    # (the public build holds every auditor file for Send unless auto-send is on: this walk is written for it on)
    [IO.File]::WriteAllText((Join-Path $loc 'settings.json'), '{"refreshAfterRestore":false,"hibernate":{"enabled":false}' + $(if ($publicEdition) { ',"auditorAutoSend":true' } else { '' }) + '}')
    $inbox = Join-Path $loc "auditors\$A"; $typed = Join-Path $hm '.claude\fake-typed'
    $busy1 = Join-Path $typed "$s1.busy"
    $do = "wait 3500; shot shell-aud-0; dump auditors; " +
      # the keyboard path: Shift+F10, Down to Prompts, Right, Down to Make auditor..., Enter, the name box's default, Enter
      "sel audit one; wait 200; key shift+f10; wait 150; shot shell-aud-1; dump ozones; key down; key down; key down; wait 80; shot shell-aud-2; dump ozones; " +
      "key right; wait 150; shot shell-aud-3; dump ozones; key down; key down; wait 80; shot shell-aud-4; dump ozones; key enter; wait 200; shot shell-aud-5; dump ozones; " +
      "key enter; wait 1000; shot shell-aud-6; dump auditors; wait 4000; " +
      # the child: Prompts -> Set as child of... (a submenu) -> auditor 1
      "sel child one; wait 200; key shift+f10; wait 150; key down; key down; key down; key right; wait 150; shot shell-aud-7; dump ozones; key down; key down; key down; wait 80; shot shell-aud-8; dump ozones; " +
      "key right; wait 150; shot shell-aud-9; dump ozones; key enter; wait 1000; shot shell-aud-10; dump auditors; " +
      # (the test drops to-1.md into the inbox once the child is saved) typed into the child: its row says sent
      "wait 9000; shot shell-aud-11; dump auditors; dump zones; dump tips; " +
      # Settings: the auto-send checkbox (Save itself is never sent from a test: it rewrites the alias file first, which
      # a test may not; the panel is cancelled and the same value saved by the deck call Save ends with)
      "zone tool settings-btn; wait 600; wheel 300 400 -6000; wait 200; wheel 300 400 -6000; wait 200; shot shell-aud-set1; dump ozones; ozone set-check set-auditor-autosend; wait 100; shot shell-aud-set2; dump ozones; " +
      "ozone set-btn cancel; wait 200; call saveSettings [{'auditorAutoSend':false}]; wait 1000; " +
      # (the second file) waits on the child's tile: Send
      "wait 8000; shot shell-aud-12; dump auditors; dump zones; zone amail-send $C1; wait 5000; shot shell-aud-13; dump auditors; " +
      # (the third file) waits: x
      "wait 7000; shot shell-aud-14; dump auditors; zone amail-x $C1; wait 1500; shot shell-aud-15; dump auditors; " +
      # auto-send on again; (the fourth file, while the child's turn runs) typed once it is idle
      "call saveSettings [{'auditorAutoSend':true}]; wait 16000; shot shell-aud-16; dump auditors; " +
      # un-child (the mouse path): the name given back, the auditor told
      "sel auditor 1.1; wait 200; rzone tile sel; wait 150; menu Prompts; wait 150; shot shell-aud-17; dump ozones; menu Un-child; wait 1000; shot shell-aud-18; dump auditors; " +
      # a second child: auditor 1.2
      "sel child two; wait 200; rzone tile sel; wait 150; menu Prompts; wait 150; menu Set as child; wait 150; shot shell-aud-19; dump ozones; menu auditor 1; wait 1000; shot shell-aud-20; dump auditors; wait 6000; " +
      # Pause, then Resume, from its Prompts submenu
      "rzone tile sel; wait 150; menu Prompts; wait 150; menu Pause; wait 3000; shot shell-aud-21; dump auditors; rzone tile sel; wait 150; menu Prompts; wait 150; shot shell-aud-22; dump ozones; menu Resume; wait 7000; shot shell-aud-23; dump auditors; " +
      # un-make: every name given back
      "sel $A; wait 200; rzone tile sel; wait 150; menu Prompts; wait 150; shot shell-aud-24; dump ozones; menu Un-make; wait 1000; shot shell-aud-25; dump auditors"
    $savedClaude = $env:CLAUDE_DECK_CLAUDE; $env:CLAUDE_DECK_CLAUDE = $fake
    try { $p = Run 'auditors' $do $st $hm '--allow-claude' -NoWait } finally { $env:CLAUDE_DECK_CLAUDE = $savedClaude }
    # the auditor's side: files into its inbox, each when the run is ready for it
    $phase = 0; $t0 = Get-Date; $heldInbox = $false; $heldTyped = $false; $tD = $null
    $count = { param($d) if (Test-Path $d) { @(Get-ChildItem $d -File -ErrorAction SilentlyContinue).Count } else { 0 } }
    $setText = { $f = Join-Path $loc 'settings.json'; if (Test-Path $f) { Get-Content $f -Raw } else { '' } }
    while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 170) {
      $tj = if (Test-Path (Join-Path $loc 'tiles.json')) { Get-Content (Join-Path $loc 'tiles.json') -Raw } else { '' }
      if ($phase -eq 0 -and $tj -match '"child"' -and (Test-Path $inbox)) { [IO.File]::WriteAllText((Join-Path $inbox 'to-1.md'), "Do the overnight run.`r`nAll of it.`r`n"); $phase = 1 }
      elseif ($phase -eq 1 -and (& $count (Join-Path $inbox 'sent')) -ge 1 -and (& $setText) -match '"auditorAutoSend":\s*false') { [IO.File]::WriteAllText((Join-Path $inbox 'to-1.md'), 'Second file, held for Send.'); $phase = 2 }
      elseif ($phase -eq 2 -and (& $count (Join-Path $inbox 'sent')) -ge 2) { [IO.File]::WriteAllText((Join-Path $inbox 'to-1.md'), 'Third file, to be dropped.'); $phase = 3 }
      elseif ($phase -eq 3 -and (& $count (Join-Path $inbox 'refused')) -ge 1 -and (& $setText) -match '"auditorAutoSend":\s*true') {
        [IO.File]::WriteAllText($busy1, 'held'); Start-Sleep -Milliseconds 600
        [IO.File]::WriteAllText((Join-Path $inbox 'to-1.md'), 'Fourth file, after the turn.'); $tD = Get-Date; $phase = 4 }
      elseif ($phase -eq 4 -and ((Get-Date) - $tD).TotalSeconds -ge 7) {
        $heldInbox = Test-Path (Join-Path $inbox 'to-1.md')
        $heldTyped = -not ((Get-Content (Join-Path $typed "$s1.txt") -Raw -ErrorAction SilentlyContinue) -match 'Fourth file')
        Remove-Item $busy1 -ErrorAction SilentlyContinue; $phase = 5 }
      Start-Sleep -Milliseconds 200
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $p.WaitForExit(10000) | Out-Null
    $log = "exit $($p.ExitCode)`n" + $(if (Test-Path ((Shot 'auditors') + '.log.txt')) { Get-Content ((Shot 'auditors') + '.log.txt') -Raw -Encoding UTF8 } else { '' })
    $seg = { param($a, $b) $i = $log.IndexOf("n-shell-aud-$a.png"); $j = if ($b) { $log.IndexOf("n-shell-aud-$b.png") } else { $log.Length }; if ($i -lt 0 -or $j -lt $i) { '' } else { $log.Substring($i, $j - $i) } }
    $tA = Get-Content (Join-Path $typed "$sA.txt") -Raw -Encoding UTF8 -ErrorAction SilentlyContinue; if (-not $tA) { $tA = '' }
    $t1 = Get-Content (Join-Path $typed "$s1.txt") -Raw -Encoding UTF8 -ErrorAction SilentlyContinue; if (-not $t1) { $t1 = '' }
    $t2 = Get-Content (Join-Path $typed "$s2.txt") -Raw -Encoding UTF8 -ErrorAction SilentlyContinue; if (-not $t2) { $t2 = '' }
    # kept beside the shots for a look afterwards: what was typed into each stand-in, and the deck's own log
    [IO.File]::WriteAllText((Join-Path $outDir 'n-shell-aud-typed.txt'), "== auditor ($sA)`r`n$tA`r`n== child one ($s1)`r`n$t1`r`n== child two ($s2)`r`n$t2`r`n")
    Copy-Item (Join-Path $loc 'deck.log') (Join-Path $outDir 'n-shell-aud-deck.log') -ErrorAction SilentlyContinue
    foreach ($x in @($sA, $s1, $s2)) { Copy-Item (Join-Path $typed "$x.status") (Join-Path $outDir "n-shell-aud-status-$($x.Substring(0, 2)).txt") -ErrorAction SilentlyContinue }
    Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
    Check 'the inbox side went through every step (5 phases)' ($phase -eq 5)
    Check 'the stand-in ran for each of the three tiles (its marker files), so no real claude.exe did' ((@($sA, $s1, $s2) | Where-Object { Test-Path (Join-Path $typed "$_.started") }).Count -eq 3)
    Check 'the three restored tiles are Claude tiles to the exe' (([regex]::Matches((& $seg '0' '1'), "(?m)^aud tile \S+ '(audit one|child one|child two)' kind claude")).Count -eq 3)
    $s = & $seg '1' '2'; Check 'Shift+F10: the tile menu, its first row lit' ($s -match '(?m)^ctx ' -and $s -match '(?m)^  Rename \(lit\)')
    $s = & $seg '2' '3'; Check 'Down x3: Prompts lit (a submenu)' ($s -match '(?m)^  Prompts \(sub\) \(lit\)')
    $s = & $seg '3' '4'; Check 'Right: the Prompts submenu - Pause lit, Pause until restart, Make auditor...' ($s -match '(?m)^  Pause \(lit\)' -and $s -match '(?m)^  Pause until restart' -and $s -match '(?m)^  Make auditor')
    $s = & $seg '4' '5'; Check 'Down x2: Make auditor... lit' ($s -match '(?m)^  Make auditor\S* \(lit\)')
    $s = & $seg '5' '6'; Check 'Enter: the name box, "auditor 1" in it' ($s -match 'auditor 1')
    $s = & $seg '6' '7'
    Check 'Enter: makeAuditor sent with "auditor 1"' ($log -match ('call makeAuditor \["' + $A + '","auditor 1"\]'))
    Check 'the auditor is "auditor 1": its title and its auditor field' ($s -match ("(?m)^aud tile $A 'auditor 1' kind claude \S+ \S+ auditor \{[^\n]*`"string`":`"auditor 1`""))
    $s = & $seg '7' '8'; Check 'the child''s Prompts: Make auditor... and Set as child of... (a submenu)' ($s -match '(?m)^  Make auditor' -and $s -match '(?m)^  Set as child of\S* \(sub\)')
    $s = & $seg '8' '9'; Check 'Down x3: Set as child of... lit' ($s -match '(?m)^  Set as child of\S* \(sub\) \(lit\)')
    $s = & $seg '9' '10'; Check 'Right: the auditors, auditor 1 lit' ($s -match '(?m)^  auditor 1 \(lit\)')
    $s = & $seg '10' '11'
    Check 'Enter: setAuditorChild sent' ($log -match ('call setAuditorChild \["' + $C1 + '","' + $A + '"\]'))
    Check 'the child is "auditor 1.1" (a string ending in a digit: <string>.<n>)' ($s -match "(?m)^aud tile $C1 'auditor 1\.1' kind claude [^\n]* child \{[^\n]*`"auditorName`":`"auditor 1`"[^\n]*`"n`":1")
    $audHead = if ($publicEdition) { 'AUDITOR: the user made this session an auditor in scroDeck, named "auditor 1"' } else { 'AUDITOR: the user made this session an auditor in the Claude Deck, named "auditor 1"' }
    Check 'the AUDITOR prompt was typed into the auditor, naming it (the deck by edition)' ($tA -match [regex]::Escape($audHead))
    Check 'the CHILD line was typed into the auditor (child 1 = its session, tile "child one")' ($tA -match ('CHILD 1: session ' + $s1 + ', tile "child one"'))
    $s = & $seg '11' 'set1'
    Check 'to-1.md typed into the child (auto-send on)' ($t1 -match 'Do the overnight run\.')
    Check 'the child''s row: from auditor 1, sent, its first line' ($s -match "(?m)^aud mail $C1 \S+ to-1\.md from 'auditor 1' state sent first 'Do the overnight run\.' sentAt yes")
    Check 'the row is drawn (its tooltip names the file)' ($s -match "(?m)^tip tile $C1 'to-1\.md: the deck types it as your own message'")
    Check 'a sent row has no Send / x' ($s -notmatch "(?m)^zone amail-(send|x) $C1")
    Check 'the file went to sent/, the log.md line written' ((& $count (Join-Path $inbox 'sent')) -ge 1 -and (Test-Path (Join-Path $inbox 'log.md')))
    $s = & $seg 'set1' 'set2'; Check 'Settings: the auto-send checkbox, on by default' ($s -match '(?m)^  check set-auditor-autosend 1')
    $s = & $seg 'set2' '12'; Check 'a click on it: off' ($s -match '(?m)^  check set-auditor-autosend 0' -and $log -notmatch 'NO ZONE')
    Check 'auditorAutoSend false saved (saveSettings)' ($log -match 'call saveSettings \[\{"auditorAutoSend":false\}\]')
    $s = & $seg '12' '13'
    Check 'auto-send off: the second file waits on the child''s tile (state hold)' ($s -match "(?m)^aud mail $C1 \S+ to-1\.md from 'auditor 1' state hold first 'Second file, held for Send\.'")
    Check 'its row has Send and x' ($s -match "(?m)^zone amail-send $C1 \(\S+\)" -and $s -match "(?m)^zone amail-x $C1 \(\S+\)")
    $s = & $seg '13' '14'
    Check 'Send: auditorSend sent, the file typed into the child, the row says sent' ($log -match 'call auditorSend \["\S+"\]' -and $t1 -match 'Second file, held for Send\.' -and $s -match "(?m)^aud mail $C1 \S+ to-1\.md from 'auditor 1' state sent first 'Second file")
    $s = & $seg '14' '15'; Check 'the third file waits (hold)' ($s -match "state hold first 'Third file")
    $s = & $seg '15' '16'
    $ref = @(Get-ChildItem (Join-Path $inbox 'refused') -File -ErrorAction SilentlyContinue)
    Check 'x: auditorDrop sent; the row gone; the file in refused/ with its note; never typed' ($log -match 'call auditorDrop \["\S+"\]' -and $s -notmatch "first 'Third file" -and $ref.Count -eq 1 -and ((Get-Content $ref[0].FullName -Raw) -match 'not sent: the user dropped it on the child tile') -and $t1 -notmatch 'Third file')
    Check 'auto-send on again (saveSettings)' ($log -match 'call saveSettings \[\{"auditorAutoSend":true\}\]')
    Check 'auto-send on, the child busy: the fourth file is NOT typed into the running turn (still in the inbox after 7 s)' ($heldInbox -and $heldTyped)
    $s = & $seg '16' '17'
    Check 'the child idle again: typed, the row says sent' ($t1 -match 'Fourth file, after the turn\.' -and $s -match "state sent first 'Fourth file")
    $s = & $seg '17' '18'; Check 'the child''s Prompts: Un-child (child 1 of auditor 1)' ($s -match '(?m)^  Un-child \(child 1 of auditor 1\)')
    $s = & $seg '18' '19'
    Check 'Un-child: unchildTile sent; the name given back, no child field' ($log -match ('call unchildTile \["' + $C1 + '"\]') -and $s -match "(?m)^aud tile $C1 'child one' kind claude [^\n]* child - ")
    Check 'the auditor was told: CHILD 1 left: tile "child one" (un-childed)' ($tA -match 'CHILD 1 left: tile "child one" \(un-childed\)')
    $s = & $seg '19' '20'; Check 'Set as child of... from the mouse: its submenu lists auditor 1' ($s -match '(?m)^  auditor 1 ')
    $s = & $seg '20' '21'; Check 'a second child: "auditor 1.2"' ($s -match "(?m)^aud tile $C2 'auditor 1\.2' kind claude [^\n]*`"n`":2")
    Check 'its CHILD line typed into the auditor' ($tA -match ('CHILD 2: session ' + $s2 + ', tile "child two"'))
    $s = & $seg '21' '22'
    Check 'Prompts -> Pause: pauseTile sent; the PAUSE prompt typed (Esc first); the tile marked paused' ($log -match ('call pauseTile \["' + $C2 + '",\{\}\]') -and $t2 -match ('PAUSE: ' + [regex]::Escape($DeckEdition.The) + ' is about to restart') -and $s -match "(?m)^aud tile $C2 [^\n]* pause \{[^\n]*`"state`":`"pa")
    $s = & $seg '22' '23'; Check 'Prompts after a Pause: Resume, no Pause' ($s -match '(?m)^  Resume ' -and $s -notmatch '(?m)^  Pause ')
    $s = & $seg '23' '24'
    # (resuming -> done needs the session's RESUMED line in its transcript; the stand-in writes none)
    Check 'Resume: resumeTile sent; the RESUME prompt typed at its idle turn; the tile says resuming' ($log -match ('call resumeTile \["' + $C2 + '"\]') -and $t2 -match ('RESUME: ' + [regex]::Escape($DeckEdition.The) + ' restarted') -and $s -match "(?m)^aud tile $C2 [^\n]* pause \{[^\n]*`"state`":`"resuming`"")
    $s = & $seg '24' '25'; Check 'the auditor''s Prompts: Rename auditor..., Un-make auditor (1 child)' ($s -match '(?m)^  Rename auditor' -and $s -match '(?m)^  Un-make auditor \(1 child\)')
    $s = & $seg '25' ''
    Check 'Un-make: unmakeAuditor sent; every name given back, no auditor, no child' ($log -match ('call unmakeAuditor \["' + $A + '"\]') -and $s -match "(?m)^aud tile $A 'audit one' kind claude [^\n]* auditor - child - " -and $s -match "(?m)^aud tile $C2 'child two' kind claude [^\n]* auditor - child - ")
    Check 'nothing refused by the deck along the way' ($log -notmatch 'NO MENU ROW|NO TILE|only a Claude session')
    Check 'scene == whole frame in every shot' (NoMismatch $log)
  }
}

# ---------------------------------------------------------------- the pause mark: a PAUSE the deck did not type
if (Want 'pausemark') {
  '== pausemark: a PAUSE pasted into a session (read off its transcript) marks its tile until the next prompt; saved with the tile'
  $st = New-Temp 'st'; $hm = New-Temp 'home'
  $P1 = 'b1000000-0000-4000-8000-0000000000b1'; $P2 = 'b2000000-0000-4000-8000-0000000000b2'; $P3 = 'b3000000-0000-4000-8000-0000000000b3'
  $q1 = '61000000-0000-4000-8000-000000000061'; $q2 = '62000000-0000-4000-8000-000000000062'; $q3 = '63000000-0000-4000-8000-000000000063'
  $proj = Join-Path $hm '.claude\projects\C--x'; New-Item -ItemType Directory -Force $proj | Out-Null
  $iso = { param($ago) [DateTimeOffset]::UtcNow.AddMinutes(-$ago).ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
  $u = { param($text, $ago) (@{ type = 'user'; timestamp = (& $iso $ago); cwd = 'C:\x'; origin = @{ kind = 'human' }; message = @{ role = 'user'; content = $text } } | ConvertTo-Json -Compress -Depth 5) }
  $as = { param($text, $ago) (@{ type = 'assistant'; timestamp = (& $iso $ago); message = @{ role = 'assistant'; model = 'claude-fable-5-1'; content = @(@{ type = 'text'; text = $text }) } } | ConvertTo-Json -Compress -Depth 6) }
  $pasted = "`n`n<pasted_content id=`"f150`">`nPAUSE: the Claude Deck is about to restart. Stop what you are doing now; start nothing new.`n</pasted_content>"
  # P1: the user pasted PAUSE, the session answered PAUSED; P2: PAUSE, then another prompt; P3: saved as paused by its prompt
  # at the last run, and a prompt came after it since (the next start clears it)
  [IO.File]::WriteAllText((Join-Path $proj "$q1.jsonl"), ((& $u 'start the export' 30), (& $as 'Working.' 29), (& $u $pasted 20), (& $as "Stopped the watcher.`nPAUSED t-1 - C:\x.md" 19), '{"type":"custom-title","customTitle":"pasted pause"}' -join "`n") + "`n")
  [IO.File]::WriteAllText((Join-Path $proj "$q2.jsonl"), ((& $u $pasted 20), (& $as 'PAUSED t-2 - C:\y.md' 19), (& $u 'never mind, carry on' 10), (& $as 'Carrying on.' 9), '{"type":"custom-title","customTitle":"paused then prompted"}' -join "`n") + "`n")
  [IO.File]::WriteAllText((Join-Path $proj "$q3.jsonl"), ((& $u $pasted 40), (& $as 'PAUSED t-3 - C:\z.md' 39), (& $u 'RESUME: go on' 5), (& $as 'RESUMED t-3' 4), '{"type":"custom-title","customTitle":"saved then resumed"}' -join "`n") + "`n")
  $loc = Join-Path $st 'local'; New-Item -ItemType Directory -Force $loc | Out-Null
  $now = [double][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $tl = @(@($P3, $q3, 'saved then resumed'), @($P1, $q1, 'pasted pause'), @($P2, $q2, 'paused then prompted')) | ForEach-Object {
    $o = [ordered]@{ id = $_[0]; kind = 'claude'; account = 'claude'; sessionId = $_[1]; cwd = 'C:\x'; pinned = $false; createdAt = $now; title = $_[2]; asleep = $true; sleptAt = $now - 60000 }
    if ($_[0] -eq $P3) { $o.pause = [ordered]@{ state = 'paused'; at = $now - 40 * 60000; untilRestart = $false; want = $false; by = 'prompt'; sid = $q3 } }
    $o }
  [IO.File]::WriteAllText((Join-Path $loc 'tiles.json'), (@{ tiles = @($tl) } | ConvertTo-Json -Depth 5))
  [IO.File]::WriteAllText((Join-Path $loc 'settings.json'), '{"refreshAfterRestore":false,"hibernate":{"enabled":true}}')
  # then Settings: "Send RESUME after a restart to paused sessions" (on by default) - ticked off on the panel, the panel
  # cancelled (Save itself rewrites the alias file first, which a test may not) and the same value saved by the deck call
  $log = Run 'pausemark' ('wait 3500; shot shell-pausemark; dump auditors; zone tool settings-btn; wait 600; wheel 300 400 -6000; wait 200; wheel 300 400 -6000; wait 200; ' +
    'shot shell-pausemark-set1; dump ozones; ozone set-check set-resume-after-restart; wait 100; shot shell-pausemark-set2; dump ozones; ozone set-btn cancel; wait 200; ' +
    "call saveSettings [{'resumeAfterRestart':false}]; wait 1000") $st $hm
  $glyph = [string][char]0x2016
  $tj = if (Test-Path (Join-Path $loc 'tiles.json')) { Get-Content (Join-Path $loc 'tiles.json') -Raw } else { '' }
  $saved = $null; try { $saved = ($tj | ConvertFrom-Json).tiles } catch { }
  Check 'the run ended cleanly' ($log -match '(?m)^exit 0')
  # (the first tile is the one on show at the start: the deck wakes it, and with no Claude launch in a test it ends; it is
  # P3, whose mark goes either way. The other two stay asleep: no process at all)
  Check 'the other two tiles came back asleep (no process: nothing was started)' (([regex]::Matches($log, "(?m)^aud tile \S+ '(pasted pause|paused then prompted)' kind claude status asleep")).Count -eq 2)
  Check 'a pasted PAUSE answered PAUSED: the tile is paused, by its prompt' ($log -match "(?m)^aud tile $P1 [^\n]* pause \{`"by`":`"prompt`",`"state`":`"paused`",`"untilRestart`":false,`"want`":false\}")
  Check ('...and its second line says "' + $glyph + ' paused"') ($log -match ("(?m)^aud tile $P1 [^\n]* words '" + [regex]::Escape($glyph + ' paused') + "'"))
  Check 'PAUSE, then another prompt: no mark' ($log -match "(?m)^aud tile $P2 [^\n]* pause - words ''")
  Check 'saved as paused at the last run, a RESUME after it since: cleared at this start' ($log -match "(?m)^aud tile $P3 [^\n]* pause - words ''")
  Check 'tiles.json keeps the mark (by prompt, its session id), and only that one' ($saved -and (@($saved | Where-Object { $_.pause }).Count -eq 1) -and (($saved | Where-Object { $_.id -eq $P1 }).pause.by -eq 'prompt') -and (($saved | Where-Object { $_.id -eq $P1 }).pause.sid -eq $q1))
  Check 'the deck log says it (seen once; the saved one cleared)' ((Get-Content (Join-Path $loc 'deck.log') -Raw -ErrorAction SilentlyContinue) -match ("$($P1.Substring(0, 8)): a PAUSE prompt in its transcript") -and (Get-Content (Join-Path $loc 'deck.log') -Raw -ErrorAction SilentlyContinue) -match ("$($P3.Substring(0, 8)): another prompt after the pause"))
  Check 'nothing typed, nothing started (no RESUME for a mark seen at this start: only the next start sends one)' ($log -notmatch 'call (newTile|wakeTile|resumeTile)' -and $log -notmatch 'BLOCKED')
  $i1 = $log.IndexOf('n-shell-pausemark-set1.png'); $i2 = $log.IndexOf('n-shell-pausemark-set2.png')
  $set1 = if ($i1 -ge 0 -and $i2 -gt $i1) { $log.Substring($i1, $i2 - $i1) } else { '' }; $set2 = if ($i2 -ge 0) { $log.Substring($i2) } else { '' }
  Check 'Settings: "Send RESUME after a restart to paused sessions", on by default' ($set1 -match '(?m)^  check set-resume-after-restart 1')
  Check 'a click on it: off (no NO ZONE)' ($set2 -match '(?m)^  check set-resume-after-restart 0' -and $log -notmatch 'NO ZONE')
  $sj = Get-Content (Join-Path $loc 'settings.json') -Raw -ErrorAction SilentlyContinue
  Check 'resumeAfterRestart false saved in settings.json (saveSettings)' ($log -match "call saveSettings \[\{`"resumeAfterRestart`":false\}\]" -and $sj -match '"resumeAfterRestart":\s*false')
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

if (Want 'onedeck') {
  '== onedeck: one deck at a time - the guard and the marker (core/deck/onedeck.hpp; the shared folder a TEMP one)'
  $iso = { [DateTimeOffset]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
  $me = (Get-Process -Id $PID).Path
  $mk = { param($dir, $live) $m = [ordered]@{ v = 1; deck = 'electron'; running = $true; pid = $PID; exe = $me; args = @(); stateDir = (New-Temp 'e-state'); startedAt = (& $iso); heartbeatAt = (& $iso); liveSessions = $live }
    [IO.File]::WriteAllText((Join-Path $dir 'active.json'), ($m | ConvertTo-Json)) }
  # 1. the other deck (here: this PowerShell, named as the Electron deck in the marker) runs with 4 sessions: blocked
  $st = New-Temp 'st'; $hm = New-Temp 'home'; $sh = New-Temp 'shared'
  $loc = Join-Path $st 'local'; New-Item -ItemType Directory -Force $loc | Out-Null
  $tiles = '{"tiles":[{"id":"t1","kind":"claude","account":"claude","sessionId":"5e550000-0000-4000-8000-000000000001","cwd":"C:\\x","title":"one","asleep":true,"sleptAt":1}],"windows":[{"bounds":null,"layout":{"t1":[0]}}]}'
  [IO.File]::WriteAllText((Join-Path $loc 'tiles.json'), $tiles)
  & $mk $sh 4
  $before = Get-Content (Join-Path $sh 'active.json') -Raw
  $env:CLAUDE_DECK_SHARED_DIR = $sh
  $log = Run 'onedeck-blocked' 'wait 200' $st $hm
  Check 'blocked: the run ended cleanly' ($log -match '(?m)^exit 0')
  Check 'blocked: it said so, in the strip''s words' ($log -match 'onedeck: blocked - The other Claude Deck is running with 4 sessions - quit it first \(the Electron Claude Deck runs \(pid \d+\) with 4 live sessions \(its marker\)\)')
  Check 'blocked: the marker is the other deck''s, untouched' ((Get-Content (Join-Path $sh 'active.json') -Raw) -eq $before)
  Check 'blocked: no shared layout' (-not (Test-Path (Join-Path $sh 'shared-layout.json')))
  Check 'blocked: its own tiles.json untouched; no running.json, settings.json, deck-state.json, onedeck.json' (((Get-Content (Join-Path $loc 'tiles.json') -Raw) -eq $tiles) -and
    -not (Test-Path (Join-Path $loc 'running.json')) -and -not (Test-Path (Join-Path $loc 'settings.json')) -and -not (Test-Path (Join-Path $loc 'deck-state.json')) -and -not (Test-Path (Join-Path $loc 'onedeck.json')))
  Check 'blocked: the shot exists (the strip)' (Test-Path (Shot 'onedeck-blocked'))
  # 2. its Quit button (the strip's right end, 1400 DIPs wide: x 1304..1384, y 13..45) ends the deck
  $log = Run 'onedeck-quit' 'click 1344 29' $st $hm
  Check 'blocked: Quit ends it' (($log -match 'onedeck: quit') -and ($log -match '(?m)^exit 0'))
  # 3. the same deck with no live session over there: it starts, takes the marker, and a clean quit leaves running:false
  & $mk $sh 0
  $log = Run 'onedeck-start' 'wait 600' $st $hm
  $m = $null; try { $m = Get-Content (Join-Path $sh 'active.json') -Raw | ConvertFrom-Json } catch { }
  Check 'start: a deck with no sessions over there does not block' ($log -notmatch 'onedeck: blocked' -and $log -match '(?m)^exit 0')
  Check 'start: it took the marker - native, its exe, --local --state-dir, its state folder' ($m -and $m.deck -eq 'native' -and $m.exe -ieq $exe -and $m.args[0] -eq '--local' -and $m.args[1] -eq '--state-dir' -and $m.stateDir -ieq $loc)
  Check 'start: a clean quit left it running:false (never deleted)' ($m -and $m.running -eq $false -and $m.liveSessions -eq 0)
  $env:CLAUDE_DECK_SHARED_DIR = $null
  # 4. no CLAUDE_DECK_SHARED_DIR: a --state-dir deck keeps the shared folder inside its own (never %APPDATA%)
  $st2 = New-Temp 'st'
  $log = Run 'onedeck-own' 'wait 300' $st2 $hm
  Check 'a scratch deck''s marker is <state>\local\onedeck-shared\active.json' ((Test-Path (Join-Path $st2 'local\onedeck-shared\active.json')) -and $log -match '(?m)^exit 0')
  # 5. the same-session guard: a session a live Claude process holds (its record names this PowerShell) is not resumed
  $st3 = New-Temp 'st'; $hm3 = New-Temp 'home'
  $loc3 = Join-Path $st3 'local'; New-Item -ItemType Directory -Force $loc3 | Out-Null
  $S = '5e550000-0000-4000-8000-0000000000f1'
  New-Item -ItemType Directory -Force (Join-Path $hm3 '.claude\sessions') | Out-Null
  [IO.File]::WriteAllText((Join-Path $hm3 ".claude\sessions\$PID.json"), (@{ pid = $PID; sessionId = $S; status = 'idle'; kind = 'interactive' } | ConvertTo-Json))
  [IO.File]::WriteAllText((Join-Path $loc3 'tiles.json'), ('{"tiles":[{"id":"held1","kind":"claude","account":"claude","sessionId":"' + $S + '","cwd":"C:\\x","title":"held elsewhere"}],"windows":[{"bounds":null,"layout":{"held1":[0]}}]}'))
  $log = Run 'onedeck-elsewhere' 'wait 1500' $st3 $hm3
  $dl = Get-Content (Join-Path $loc3 'deck.log') -Raw -ErrorAction SilentlyContinue
  Check 'same session: live in another process, not resumed; the deck log says so' (($dl -match "\[onedeck\] tile held1: session $S is open in another window \(pid $PID\): not resumed here") -and $log -match '(?m)^exit 0')
  Check 'same session: nothing was killed (this PowerShell, the holder, still runs)' ([bool](Get-Process -Id $PID -ErrorAction SilentlyContinue))
  # 6. the shared layout: a stale deck imports the newer shared file BEFORE it writes anything shared
  $newer = { param($tid) '{"v":1,"writtenBy":"electron","writtenAt":"2026-11-24T19:00:00.000Z","realChange":9,"lastChange":{"what":"a new tile","at":"2026-11-24T18:59:00.000Z","by":"electron"},' +
    '"tiles":[{"id":"' + $tid + '","kind":"claude","account":"claude","sessionId":"5e550000-0000-4000-8000-0000000000e9","cwd":"C:\\x","title":"from the electron deck","asleep":true,"sleptAt":1}],' +
    '"windows":[{"index":0,"layout":{}}],"groups":["shared-group"],"heldPrompts":[],"triggers":{},"settings":{"oneWindow":true,"grid":{"rows":2,"cols":3}}}' }
  $stale = '{"tiles":[{"id":"stale1","kind":"claude","account":"claude","sessionId":"5e550000-0000-4000-8000-0000000000e8","cwd":"C:\\x","title":"stale","asleep":true,"sleptAt":1}],"windows":[{"bounds":{"x":10,"y":10,"width":900,"height":700},"layout":{}}]}'
  $st4 = New-Temp 'st'; $hm4 = New-Temp 'home'; $sh4 = New-Temp 'shared'
  $loc4 = Join-Path $st4 'local'; New-Item -ItemType Directory -Force $loc4 | Out-Null
  [IO.File]::WriteAllText((Join-Path $loc4 'tiles.json'), $stale)
  [IO.File]::WriteAllText((Join-Path $loc4 'onedeck.json'), '{"v":1,"local":{"n":2,"last":null},"seen":{"realChange":4,"writtenAt":"2026-11-20T00:00:00.000Z","writtenBy":"native","local":2}}')
  [IO.File]::WriteAllText((Join-Path $sh4 'shared-layout.json'), (& $newer 'fresh1'))
  $env:CLAUDE_DECK_SHARED_DIR = $sh4
  $log = Run 'onedeck-import' 'wait 800' $st4 $hm4
  $env:CLAUDE_DECK_SHARED_DIR = $null
  $tj = $null; try { $tj = Get-Content (Join-Path $loc4 'tiles.json') -Raw | ConvertFrom-Json } catch { }
  $shj = $null; try { $shj = Get-Content (Join-Path $sh4 'shared-layout.json') -Raw | ConvertFrom-Json } catch { }
  $bk = @(Get-ChildItem (Join-Path $loc4 'onedeck-backups') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*-import' })
  Check 'import: a stale deck imports the newer shared layout at start' (($log -match 'onedeck: at start: import') -and $tj -and @($tj.tiles).Count -eq 1 -and $tj.tiles[0].id -eq 'fresh1' -and $tj.groups[0] -eq 'shared-group')
  Check 'import: its own window bounds kept; its own files backed up first' ($tj -and $tj.windows[0].bounds.width -eq 900 -and $bk.Count -eq 1 -and (Test-Path (Join-Path $bk[0].FullName 'tiles.json')))
  Check 'import: the newer shared file is not written over (its tiles, its realChange)' ($shj -and $shj.realChange -eq 9 -and $shj.tiles[0].id -eq 'fresh1')
  # 7. both changed: the question; "Keep it" (this deck's own) - the click on its button, 1400 x 900 DIPs: x 950..1046, y 213..245
  $st5 = New-Temp 'st'; $hm5 = New-Temp 'home'; $sh5 = New-Temp 'shared'
  $loc5 = Join-Path $st5 'local'; New-Item -ItemType Directory -Force $loc5 | Out-Null
  [IO.File]::WriteAllText((Join-Path $loc5 'tiles.json'), $stale)
  [IO.File]::WriteAllText((Join-Path $loc5 'onedeck.json'), '{"v":1,"local":{"n":3,"last":{"what":"renamed a tile","at":"2026-11-24T18:30:00.000Z","by":"native"}},"seen":{"realChange":4,"writtenAt":"2026-11-20T00:00:00.000Z","writtenBy":"native","local":2}}')
  [IO.File]::WriteAllText((Join-Path $sh5 'shared-layout.json'), (& $newer 'fresh2'))
  $env:CLAUDE_DECK_SHARED_DIR = $sh5
  $log = Run 'onedeck-ask' 'wait 300; shot shell-onedeck-question; click 998 229; wait 900' $st5 $hm5
  $env:CLAUDE_DECK_SHARED_DIR = $null
  $tj = $null; try { $tj = Get-Content (Join-Path $loc5 'tiles.json') -Raw | ConvertFrom-Json } catch { }
  $shj = $null; try { $shj = Get-Content (Join-Path $sh5 'shared-layout.json') -Raw | ConvertFrom-Json } catch { }
  $bk = @(Get-ChildItem (Join-Path $loc5 'onedeck-backups') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*-kept-own' })
  Check 'ask: both changed -> the question, each side''s most recent change' (($log -match 'onedeck: ask - both decks changed') -and ($log -match 'shared: a new tile, 2026-11-24 18:59 UTC \(the electron deck\)') -and ($log -match 'this deck: renamed a tile, 2026-11-24 18:30 UTC') -and (Test-Path (Join-Path $outDir 'n-shell-onedeck-question.png')))
  Check 'ask: "Keep it": this deck starts on its own layout' (($log -match 'onedeck: this deck''s own layout wins') -and $tj -and $tj.tiles[0].id -eq 'stale1')
  Check 'ask: ... and replaces the shared file, realChange above the other''s, named; the shared one backed up' ($shj -and $shj.writtenBy -eq 'native' -and $shj.realChange -eq 10 -and $shj.lastChange.what -eq 'kept this deck''s own layout at start' -and $bk.Count -eq 1 -and (Test-Path (Join-Path $bk[0].FullName 'shared-layout.json')))
  Check 'scene == whole frame in every shot' (NoMismatch $log)
}

if (Want 'quitwin') {
  '== quitwin: a normal quit keeps every window - two windows, the second closed, then the main one; both layouts back at the next start, and in the shared layout file (a TEMP one)'
  $st = New-Temp 'st'; $hm = New-Temp 'home'; $sh = New-Temp 'shared'
  $loc = Join-Path $st 'local'; New-Item -ItemType Directory -Force $loc | Out-Null
  # two terminals kept from an earlier run (--persist-shells): qw-one fills the main window, qw-two is not placed
  [IO.File]::WriteAllText((Join-Path $loc 'tiles.json'), '{"tiles":[{"id":"qw-one","kind":"shell","cwd":"C:\\Windows"},{"id":"qw-two","kind":"shell","cwd":"C:\\Windows"}],"groups":[],"windows":[{"bounds":null,"layout":{"qw-one":[1,2,3,4]}}]}')
  $env:CLAUDE_DECK_SHARED_DIR = $sh
  $do = 'wait 2600; rzone tile qw-two; menu Open in a new window; wait 1500; dump windows; win 1 close; wait 800; dump windows; close'
  $log = Run 'quitwin-a' $do $st $hm '--persist-shells'
  Check 'quitwin: two windows (qw-one in the main one, qw-two in the second), the second closed, then the main one (a normal quit, exit 0)' (($log -match '(?m)^exit 0') -and
    ($log -match '(?m)^window w0 primary inited 1 \S+ selected \S* layout \{"qw-one":\[1,2,3,4\]\}') -and ($log -match '(?m)^window w1 inited 1 \S+ selected \S* layout \{"qw-two":\[1,2,3,4\]\}') -and ($log -match '(?m)^windows 1 \(deck clients 1\)'))
  $tj = $null; try { $tj = Get-Content (Join-Path $loc 'tiles.json') -Raw | ConvertFrom-Json } catch { }
  $sl = $null; try { $sl = Get-Content (Join-Path $sh 'shared-layout.json') -Raw | ConvertFrom-Json } catch { }
  Check 'quitwin: tiles.json keeps BOTH windows, each with its tile' ($tj -and @($tj.windows).Count -eq 2 -and ($tj.windows[0].layout.PSObject.Properties.Name -contains 'qw-one') -and ($tj.windows[1].layout.PSObject.Properties.Name -contains 'qw-two'))
  Check 'quitwin: the shared layout file keeps both windows'' cells too' ($sl -and @($sl.windows).Count -eq 2 -and ($sl.windows[0].layout.PSObject.Properties.Name -contains 'qw-one') -and ($sl.windows[1].layout.PSObject.Properties.Name -contains 'qw-two'))
  $dl = Get-ChildItem $st -Recurse -Filter 'deck.log' -ErrorAction SilentlyContinue | Select-Object -First 1
  Check 'quitwin: the deck''s log says the quit kept both' ($dl -and ((Get-Content $dl.FullName -Raw -Encoding UTF8) -match "\[windows\] the quit keeps 2 windows' layouts"))
  $log2 = Run 'quitwin-b' 'wait 1500; dump windows; win 1 shot shell-quitwin-after-w1; shot shell-quitwin-after-w0' $st $hm '--persist-shells'
  $env:CLAUDE_DECK_SHARED_DIR = $null
  Check 'quitwin: the next start opens both windows again' ($log2 -match '(?m)^windows 2 \(deck clients 2\)')
  Check 'quitwin: the second window has its layout back (qw-two)' ($log2 -match '(?m)^window w1 inited 1 \S+ selected \S* layout \{"qw-two":\[1,2,3,4\]\}')
  Check 'quitwin: the main window has its layout back (qw-one)' ($log2 -match '(?m)^window w0 primary inited 1 \S+ selected \S* layout \{"qw-one":\[1,2,3,4\]\}')
  Check 'scene == whole frame in every shot' ((NoMismatch $log) -and (NoMismatch $log2))
}

if (Want 'setlayout') {
  '== setlayout: the Settings panel - every checkbox row is as tall as its label, so a label and the hint under it never overlap (the auditor''s two-line label ran over its hint, 2026-11-24), at the default width and the narrowest'
  # each `dump ozones` lists `box <kind> <id> [x,y wxh]`: the labels, checkboxes and hints drawn whole in view
  $parse = { param($text)
    $blocks = @(); $cur = @(); $in = $false
    foreach ($ln in ($text -split "`r?`n")) {
      if ($ln -match '^settings \[') { if ($in) { $blocks += , $cur }; $cur = @(); $in = $true }
      elseif ($in -and $ln -match '^  box (\S+) (\S+) \[(-?[\d.]+),(-?[\d.]+) ([\d.]+)x([\d.]+)\]') { $cur += , @($Matches[1], $Matches[2], [double]$Matches[3], [double]$Matches[4], [double]$Matches[5], [double]$Matches[6]) }
    }
    if ($in) { $blocks += , $cur }
    , $blocks }
  $overlaps = { param($blocks)
    $bad = @()
    foreach ($b in $blocks) { for ($i = 0; $i -lt $b.Count; $i++) { for ($j = $i + 1; $j -lt $b.Count; $j++) {
      $p = $b[$i]; $q = $b[$j]
      $ox = [Math]::Min($p[2] + $p[4], $q[2] + $q[4]) - [Math]::Max($p[2], $q[2]); $oy = [Math]::Min($p[3] + $p[5], $q[3] + $q[5]) - [Math]::Max($p[3], $q[3])
      if ($ox -gt 0.5 -and $oy -gt 0.5) { $bad += "$($p[0]) $($p[1]) / $($q[0]) $($q[1]) at y $($p[3])" } } } }
    , $bad }
  $steps = ''; for ($k = 1; $k -le 18; $k++) { $steps += "wheel 60 400 -450; wait 150; dump ozones; shot shell-setlayout-{0}-$('{0:D2}' -f $k); " }
  foreach ($wd in @('wide', 'min')) {
    $st = New-Temp 'st'; $hm = New-Temp 'home'
    # Settings widens the sidebar to 600px, or 70 % of the window when that is less: its narrowest is a narrow window
    $size = if ($wd -eq 'min') { '520x900' } else { '1400x900' }
    $log = Run "setlayout-$wd" ('click 60 800; wait 1400; zone tool settings-btn; wait 600; dump ozones; ' + ($steps -replace '\{0\}', $wd) + 'ozone set-btn cancel; wait 200') $st $hm -Size $size
    $blocks = & $parse $log
    $all = @($blocks | ForEach-Object { $_ })
    $aud = @($all | Where-Object { $_[0] -eq 'label' -and $_[1] -eq 'set-auditor-autosend' })
    $bad = & $overlaps $blocks
    Check "setlayout ($wd): the panel was dumped as it scrolled, with labels, checkboxes and hints in it" (($log -match '(?m)^exit 0') -and $blocks.Count -ge 10 -and @($all | Where-Object { $_[0] -eq 'hint' }).Count -gt 5 -and @($all | Where-Object { $_[0] -eq 'check' }).Count -gt 5)
    Check "setlayout ($wd): the auditor's label was seen whole, on more than one line" ($aud.Count -ge 1 -and $aud[0][5] -gt 24)
    Check "setlayout ($wd): no label, checkbox or hint overlaps another, anywhere in the panel" ($bad.Count -eq 0)
    if ($bad.Count) { $bad | Select-Object -First 5 | ForEach-Object { "    overlap: $_" } }
    $pw = [regex]::Match($log, '(?m)^settings \[[\d.-]+,[\d.-]+ ([\d.]+)x'); if ($wd -eq 'min') { Check 'setlayout (min): the panel at its narrowest (70 % of a 520px window: 364px, not 600)' ($pw.Success -and [double]$pw.Groups[1].Value -lt 400) }
    Check 'scene == whole frame in every shot' (NoMismatch $log)
  }
}

# ---------------------------------------------------------------- nothing left behind
Start-Sleep -Milliseconds 600
$left = @(Get-Process -Name $DeckEdition.ExeBase -ErrorAction SilentlyContinue | Where-Object { $script:pids -contains $_.Id })
$kids = @(); foreach ($id in $script:pids) { $kids += @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $id" -ErrorAction SilentlyContinue | Where-Object { $_.Name -match ('^(cmd|powershell|pwsh|conhost|OpenConsole|' + [regex]::Escape($DeckEdition.ExeBase) + '|msedgewebview2|claude)\.exe$') -and (Is-OurChild $_) }) }
# (the relaunched deck is a child of the first one of the restart test: it must be gone too; claude.exe = the auditors test's stand-in)
# (a child made AFTER its deck exited is not the deck's: Windows gave that id to another process - Keep-Proc's note)
Check 'no ClaudeDeckNative.exe of this suite is left running' ($left.Count -eq 0)
Check 'no shell, console host, web view or relaunched deck is left under any of them' ($kids.Count -eq 0)
if ($left.Count -or $kids.Count) { ($left + $kids) | ForEach-Object { try { Stop-Process -Id $(if ($_.ProcessId) { $_.ProcessId } else { $_.Id }) -Force -ErrorAction SilentlyContinue } catch { } } }
foreach ($d in $script:temps) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }
"shell.ps1: $($script:pass) passed, $($script:fail) failed"
if ($script:fail) { exit 1 }
