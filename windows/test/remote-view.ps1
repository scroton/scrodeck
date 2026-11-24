# windows/test/remote-view.ps1 — what a REMOTE draws and selects, checked: the exe as a desk of the FAKE deck
# (windows/web-test/fake-deck.js through asks.ps1: loopback, a made-up token, never the live deck, no Claude session).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/remote-view.ps1 [-Only size,select]
#   size    a tile another kind of screen sizes (sizeBy) is drawn at the pty's size, mirror or not; none sent
#           (windows/test/size-pty.txt; the home remote's "wrapped line far right", 2026-11-23)
#   select  a drag on the alternate screen goes on while the application scrolls by redrawing: the anchor follows,
#           rows that leave the pane are kept, the copy is exact or cut at a redraw it cannot match, and a drag past
#           the edge asks the application to scroll, throttled (windows/test/select-scroll.txt; 2026-11-24)
#   paste   text that is only a picture's path on THIS machine sends the file, not the path (Ctrl+V, a right click,
#           Shift+Insert); any other text stays text (2026-11-24); ONE picture file copied
#           as in Explorer goes the same way, other files send nothing (the office, 2026-11-24; the script is made here)
#   copy    a copy from a full-screen application leaves out its padding and does not run its rows together; a URL
#           cut by the edge stays whole (the user, hand test R2, 2026-11-24; the script is made here)
#   pause   the pause mark drawn from the office deck's meta.pause: the pause glyph and "paused" while a PAUSE prompt went in (the deck's or
#           pasted by hand, read off the transcript there), gone once the session gets another prompt (2026-11-24)
#   memory  twelve busy tiles for two minutes (windows/test/soak.ps1 -Minutes 2): the text-run cache stays within two
#           generations, every terminal within its 10,000 lines of scroll-back (the home remote, 2026-11-24)
# Needs node; lib/phone.js needs `ws` (a checkout's node_modules, or NODE_PATH). Exit 1 when a check fails.
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Force (Join-Path $root 'windows\test\out') | Out-Null
$script:pass = 0; $script:fail = @()
function Check([bool]$ok, [string]$what, [string]$detail = '') { if ($ok) { $script:pass++; "  ok   $what" } else { $script:fail += $what; "  FAIL $what $detail" } }
function Want([string]$n) { (-not $Only) -or (($Only -split ',') -contains $n) }
function Run-Script([string]$name, [string]$script) {
  $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'asks.ps1') -Name $name -Script (Join-Path $PSScriptRoot $script) -NoAsks -Shot 2>&1 | ForEach-Object { "$_" }
  $log = Join-Path $root "windows\test\out\n-$name.png.log.txt"
  $lines = if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() }
  @{ out = $o; log = $lines; exit0 = [bool]($o | Where-Object { $_ -match '^exit 0 ' }); clean = [bool]($o | Where-Object { $_ -match '\(clean\)$' }) }
}
function Lines($r, [string]$re) { @($r.log | Where-Object { $_ -match $re }) }

if (Want 'size') {
  '== size: drawn at the pty''s size when another screen drives (mirror or not)'
  $r = Run-Script 'rv-size' 'size-pty.txt'
  $d = Lines $r '^drive tile t1 '
  $d | ForEach-Object { "    | $_" }
  Check $r.exit0 'the run ended cleanly'
  Check ($d.Count -eq 5) 'five drive dumps' "$($d.Count)"
  if ($d.Count -eq 5) {
    Check ($d[0] -match 'sizeBy - .* draws (\d+)x(\d+)') 'a deck that says no sizeBy: fitted to the pane, as before'
    $own = if ($d[0] -match 'draws (\d+x\d+)') { $Matches[1] } else { '?' }
    Check ($d[1] -match 'sizeBy "office" mirror null pty \{"cols":100,"rows":30\} draws 100x30') 'sizeBy office, no mirror: drawn at the pty''s 100x30'
    Check ($d[2] -match 'pty \{"cols":120,"rows":40\} draws 120x40') 'the pty grows to 120x40: the pane follows'
    Check ($d[3] -match "sizeBy `"remote`" .* draws $own") "sizeBy remote: this window drives again, fitted to its pane ($own)"
    Check ($d[4] -match 'sizeBy "phone" mirror \{"cols":60,"rows":20\} .* draws 60x20') 'a mirror still wins (the phone drives)'
  }
  $sent = Lines $r '^resize t1 '
  Check (@($sent | Where-Object { $_ -match '^resize t1 (100x30|120x40)\b' }).Count -eq 0) 'no size of the pty''s is ever sent back (none while another screen drives)'
  Check (@($sent | Where-Object { $_ -match 'reassert: the pty is 120x40' }).Count -eq 1) 'driving again: its own size is said at once (reassert)'
  Check ((Lines $r '^pty size t1 100x30 \(sized by office').Count -eq 1) 'the log says why (pty size ... sized by office)'
  Check $r.clean 'nothing left behind'
}

if (Want 'select') {
  '== select: a drag on the alternate screen while the application scrolls by redrawing'
  # select-scroll.txt: 48 rows, rows 0-41 "L<nnn> transcript line" (frame k shows L(k+1)..), a 6-row footer kept in
  # its own scroll region (the fake deck's once-a-second "tick" lands there, as a status line would). Scenes, each
  # ended by `dump grid`: A drag down while the app scrolls 3, 9, 8 rows; B rows come back; C a clear; D the anchor's
  # row rewritten in place; E drag up while the app scrolls up 15; F past the bottom edge, near then far; G an app
  # that does not take the wheel.
  $r = Run-Script 'rv-select' 'select-scroll.txt'
  $raw = ($r.log -join "`n")
  $nl = "`r`n"
  $rows = { param($a, $b) (($a..$b) | ForEach-Object { 'L{0:D3} transcript line' -f $_ }) -join $nl }
  $clips = @([regex]::Matches($raw, "(?s)clipboard '(.*?)'\n+(?=grid )") | ForEach-Object { $_.Groups[1].Value -replace "`n", $nl -replace "`r`r", "`r" })
  Check $r.exit0 'the run ended cleanly'
  $grid = @(); for ($i = 0; $i -lt $r.log.Count; $i++) { if ($r.log[$i] -match '^grid \d') { $grid += $i } }
  Check ($grid.Count -eq 9) 'nine scenes ran (grid markers)' "$($grid.Count)"
  # A
  Check ($raw -match 'drag follows: anchor on screen, row 7, 0 rows kept') 'A: the app scrolls 3 rows - the anchor follows its row (10 -> 7)'
  Check ($raw -match 'drag follows: anchor kept above, junction row 0, 2 rows kept') 'A: it scrolls 9 more - the anchor''s row leaves the top, it and the row after it are kept'
  Check ($raw -match 'drag follows: anchor kept above, junction row 0, 10 rows kept') 'A: 8 more - every row that leaves is kept (10)'
  Check ($raw -match 'drag ended past the screen: 10 rows kept, 350 chars held') 'A: the release holds the text (kept rows + the screen''s, 350 chars)'
  Check ($clips.Count -ge 1 -and $clips[0] -ceq (& $rows 11 26)) 'A: copied EXACTLY L011..L026 (kept rows, then the screen up to the head)' $(if ($clips.Count) { "got $($clips[0].Length) chars" })
  # B
  Check ($raw -match '(?s)drag follows: anchor kept above, junction row 0, 2 rows kept\ndrag follows: anchor on screen, row 10, 0 rows kept') 'B: the rows come back and match the kept ones - the anchor is on screen again (row 10)'
  Check ($raw -match "selection 0,10 - 40,15 \(top line 0\) '$([regex]::Escape(((& $rows 11 16) -replace "`r", '')))'") 'B: the selection is L011..L016, as if it had never left'
  # C, D
  Check ($raw -match 'drag cut at the redraw: 196 chars as it stood') 'C: a clear it cannot match CUTS the drag (no guess)'
  Check ($raw -match 'drag ended cut: copied 196 chars') 'C: the release copies what was selected before the clear, and says so'
  Check ($clips.Count -ge 2 -and $clips[1] -ceq (& $rows 11 19)) 'C: copied EXACTLY L011..L019 (the selection as it stood)'
  Check ($raw -match 'drag cut at the redraw: 130 chars as it stood') 'D: the anchor''s row rewritten in place cuts it too'
  # E
  Check ($raw -match 'drag follows: anchor kept below, junction row 41, 4 rows kept') 'E: dragging up while the app scrolls up 15 - the anchor leaves the bottom of the moving block (not the footer)'
  Check ($raw -match 'drag ended past the screen: 4 rows kept, 880 chars held') 'E: the release holds 880 chars'
  Check ($clips.Count -ge 3 -and $clips[2] -ceq ((& $rows 11 50) + $nl)) 'E: copied EXACTLY L011..L050 + the anchor''s row up to its column (empty)'
  # F, G: past the edge
  $asked = { param($a, $b) @($r.log[($a + 1)..($b - 1)] | Where-Object { $_ -match '^drag past the bottom: the application asked to scroll' }).Count }
  if ($grid.Count -eq 9) {
    $near = & $asked $grid[5] $grid[6]; $far = & $asked $grid[6] $grid[7]
    "    past the bottom: just past (1 line a tick) $near reports in 600 ms, far past (15 a tick) $far"
    Check ($near -ge 2 -and $near -le 6) 'F: just past the edge - about one wheel report every 3 ticks (2..6 in 600 ms)' "$near"
    Check ($far -ge 8 -and $far -le 14 -and $far -gt $near) 'F: far past - at most one a tick (8..14 in 600 ms), more than near' "$far"
    $relF = -1; for ($i = $grid[7]; $i -lt $grid[8]; $i++) { if ($r.log[$i] -match '^copy on select') { $relF = $i; break } }
    Check ($relF -gt 0 -and (& $asked $relF $grid[8]) -eq 0) 'G: an application that does not take the wheel is asked nothing (no report, no arrow key)'
  }
  $reports = @($r.log | Where-Object { $_ -match '^wheel report t1 ' })
  Check ($reports.Count -gt 0 -and @($reports | Where-Object { $_ -notmatch '^wheel report t1 \\e\[<65;\d+;48M$' }).Count -eq 0) 'every report is ONE SGR wheel-down at the bottom row (nothing else is sent)'
  Check (-not ($r.log | Where-Object { $_ -match '^(input|click report)' -and $_ -notmatch 'click report t1' })) 'no keys were typed into the session'
  Check $r.clean 'nothing left behind'
}

if (Want 'paste') {
  '== paste: a picture''s path on this machine sends the picture'
  # Files made here (windows/test/out/paste-path): a png, a jpeg whose name has spaces (pasted in quotes, as a macro
  # may), a bmp (not a type the deck takes). The script names them by full path; a path with \r or \n in it (the
  # script's own escapes) is written with forward slashes, which the remote takes too.
  $dir = Join-Path $root 'windows\test\out\paste-path'; New-Item -ItemType Directory -Force $dir | Out-Null
  $png = Join-Path $dir 'shot.png'; $jpg = Join-Path $dir 'my shot 2.JPG'; $bmp = Join-Path $dir 'old.bmp'
  [IO.File]::WriteAllBytes($png, [byte[]](@(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A) + @(0x70) * 2992))
  [IO.File]::WriteAllBytes($jpg, [byte[]](@(0xFF, 0xD8, 0xFF, 0xE0) + @(0x6A) * 1196))
  [IO.File]::WriteAllBytes($bmp, [byte[]](@(0x42, 0x4D) + @(0x62) * 498))
  $p = { param($f) if ($f -match '\\[rn]') { $f -replace '\\', '/' } else { $f } }
  $none = (& $p (Join-Path $dir 'gone.png'))
  $steps = @(
    'wait 900; tap f6; wait 300; click 700 400; wait 300; feed \e[?2004h; wait 100;',
    "clip $(& $p $png); tap ctrl+V; wait 1500;",
    "clip `"$(& $p $jpg)`"; rclick 700 400; wait 1500;",
    "clip $(& $p $png)\r\n; tap shift+ins; wait 1500;",
    "clip $none; tap ctrl+V; wait 300;",
    "clip $(& $p $bmp); tap ctrl+V; wait 300;",
    "clip see $(& $p $png) please; tap ctrl+V; wait 300;",
    "clipfile $png; tap ctrl+V; wait 1500;",
    "clipfile $jpg; rclick 700 400; wait 1500;",
    "clipfile $bmp; tap ctrl+V; wait 300;",
    "clipfile $png|$jpg; tap ctrl+V; wait 300;",
    "clipimg 0; tap ctrl+V; wait 300")
  $script = Join-Path $PSScriptRoot 'out\paste-path.txt'
  [IO.File]::WriteAllText($script, ($steps -join "`r`n") + "`r`n")
  $r = Run-Script 'rv-paste' 'out\paste-path.txt'
  $inputs = @($r.log | Where-Object { $_ -match '^input t1 ' })
  $inputs | ForEach-Object { "    | $_" }
  Check $r.exit0 'the run ended cleanly'
  Check ((Lines $r '^paste: the text is a picture''s path on this machine \(shot\.png\)').Count -eq 2) 'Ctrl+V and Shift+Insert (a trailing new line too): the png''s path is taken for the picture'
  Check ((Lines $r '^paste: the text is a picture''s path on this machine \(my shot 2\.JPG\)').Count -eq 1) 'a right click: the quoted jpeg path with spaces, any case'
  Check ((Lines $r '^paste: the copied file is a picture on this machine \(shot\.png\)').Count -eq 1) 'a png FILE copied as in Explorer, Ctrl+V: the picture goes'
  Check ((Lines $r '^paste: the copied file is a picture on this machine \(my shot 2\.JPG\)').Count -eq 1) 'a jpeg file copied, a right click: the picture goes'
  Check ((Lines $r '^paste: 1 copied file\(s\), not one picture file: nothing sent').Count -eq 1) 'a bmp file copied: nothing sent (the note says what can be)'
  Check ((Lines $r '^paste: 2 copied file\(s\), not one picture file: nothing sent').Count -eq 1) 'two files copied: nothing sent'
  Check ((Lines $r '^paste: no text on this machine''s clipboard, nothing sent \(remote\)').Count -eq 1) 'an empty clipboard: nothing to paste, as before'
  Check ((Lines $r '^paste: a picture \(3000 bytes, image/png\) sent to the deck').Count -eq 3) 'the png''s own 3000 bytes go to the deck, as image/png (two pastes of its path, one of the file)'
  Check ((Lines $r '^paste: a picture \(1200 bytes, image/jpeg\) sent to the deck').Count -eq 2) 'the jpeg''s 1200 bytes go as image/jpeg (its path, its file)'
  Check ((Lines $r '^paste: the deck saved it as C:\\fake\\paste\\pasted-3000\.png; that path pasted').Count -eq 3 -and (Lines $r '^paste: the deck saved it as C:\\fake\\paste\\pasted-1200\.jpg; that path pasted').Count -eq 2) 'the deck''s saved paths are what is pasted'
  Check (@($inputs | Where-Object { $_ -match '^input t1 \\e\[200~C:\\fake\\paste\\pasted-(3000\.png|1200\.jpg)\\e\[201~$' }).Count -eq 5) 'each as one bracketed paste of the deck''s path'
  Check (@($inputs | Where-Object { $_ -match 'shot\.png|my shot 2' -and $_ -notmatch 'see .*please' }).Count -eq 0) 'this machine''s own path is never sent as text'
  Check (@($inputs | Where-Object { $_ -match 'gone\.png' }).Count -eq 1) 'a path to no file stays text'
  Check (@($inputs | Where-Object { $_ -match 'old\.bmp' }).Count -eq 1) 'a type the deck does not take (bmp) stays text'
  Check (@($inputs | Where-Object { $_ -match 'see .*shot\.png please' }).Count -eq 1) 'a sentence with a path in it stays text'
  Check ((Lines $r '^call pasteImage ').Count -eq 5) 'five pictures asked of the deck, no more'
  Check $r.clean 'nothing left behind'
}

if (Want 'copy') {
  '== copy: a copy from a full-screen application, tidied'
  # Claude Code's way on the alternate screen, in the fake deck's 126 columns: rows padded with WRITTEN spaces to the
  # edge and the next row written straight on (the terminal marks it wrapped), a blank row of spaces, a URL longer
  # than the row. The fake deck's ticks land in a footer region (rows 43-48). A drag from row 0 to row 5, released:
  # copy-on-select puts it on the test's clipboard.
  $W = 126
  $pad = { param($s) $s + (' ' * ($W - $s.Length)) }
  $url = 'https://example.com/' + ('a' * ($W - 20)) + '/end'
  $body = (& $pad '  Here is the plan:') + (& $pad '  - press Ctrl+V in') + (& $pad '    my tile.') + (' ' * $W) + $url
  $steps = @(
    "wait 900; tap f6; wait 300; click 700 400; wait 300; feed \e[?1049h\e[43\s48r\e[H$body\e[48\s1H; wait 200;",
    'down 352 31; move 400 121; up 400 121; wait 300; dump clipboard; dump grid')
  [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'out\copy-tidy.txt'), ($steps -join "`r`n") + "`r`n")
  $r = Run-Script 'rv-copy' 'out\copy-tidy.txt'
  $raw = ($r.log -join "`n")
  $nl = "`r`n"
  $clips = @([regex]::Matches($raw, "(?s)clipboard '(.*?)'\n+(?=grid )") | ForEach-Object { $_.Groups[1].Value -replace "`n", $nl -replace "`r`r", "`r" })
  $want = @('  Here is the plan:', '  - press Ctrl+V in', '    my tile.', '', $url) -join $nl
  Check $r.exit0 'the run ended cleanly'
  Check ($clips.Count -eq 1) 'one copy made (copy on select)' "$($clips.Count)"
  if ($clips.Count) {
    ($clips[0] -split "`r`n") | ForEach-Object { "    | [$_]" }
    Check (-not ($clips[0] -split "`r`n" | Where-Object { $_ -match ' $' })) 'no line ends in the padding'
    Check ($clips[0] -cmatch "Ctrl\+V in`r`n    my tile\.") 'a row padded to the edge ends its line (not "in      my tile.")'
    Check ($clips[0] -cmatch "my tile\.`r`n`r`nhttps://") 'the blank row is an empty line'
    Check ($clips[0].Contains($url)) 'the URL cut by the edge is whole'
    Check ($clips[0] -ceq $want) 'the copy is EXACTLY the five lines as the application drew them' "got $($clips[0].Length) chars, want $($want.Length)"
  }
  Check $r.clean 'nothing left behind'
}

if (Want 'pause') {
  '== pause: the pause mark, as the office deck''s meta says it (lib/pause.js view; drawn, never worked out here)'
  $r = Run-Script 'rv-pause' 'pause-mark.txt'
  $a = Lines $r '^aud tile t1 '
  $a | ForEach-Object { "    | $_" }
  $g = [string][char]0x2016
  Check $r.exit0 'the run ended cleanly'
  Check ($a.Count -eq 4) 'four dumps' "$($a.Count)"
  if ($a.Count -eq 4) {
    Check ($a[0] -match "pause - words ''$") 'no pause in the meta: nothing drawn'
    Check ($a[1] -match ('"by":"prompt"[^\n]* words ''' + [regex]::Escape($g + ' paused') + "'$")) ('a PAUSE seen in the office session: "' + $g + ' paused"')
    Check ($a[2] -match ("words '" + [regex]::Escape($g + ' pausing until restart') + "'$")) 'the deck''s own Pause until restart: its words'
    Check ($a[3] -match "pause - words ''$") 'another prompt there (the meta drops it): gone'
  }
  Check $r.clean 'nothing left behind'
}

if (Want 'memory') {
  '== memory: twelve busy tiles for two minutes; the text-run cache stays bounded, the scroll-back at its 10,000 lines'
  # windows/test/soak.ps1's short form (the long one: -Minutes 30; the home remote, 2026-11-24: 68 -> 305 MB private
  # in 2.5 h). Before the fix the run cache reached ~17,000 layouts in the first minute and was dropped whole at 30,000.
  $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'soak.ps1') -Minutes 2 -Every 30 -Name rv-memory -Port 8794 2>&1 | ForEach-Object { "$_" }
  $mem = @($o | Where-Object { $_ -match '^\s*MEM private' })
  $mem | ForEach-Object { "    | $($_.Trim())" }
  Check (@($o | Where-Object { $_ -match '^exit 0 ' }).Count -eq 1) 'the run ended cleanly'
  Check ($mem.Count -ge 5) 'a sample every 30 s' "$($mem.Count)"
  $runs = @($mem | ForEach-Object { if ($_ -match 'runs (\d+) ') { [int]$Matches[1] } })
  $max = ($runs | Measure-Object -Maximum).Maximum
  Check ($max -le 8192) 'the run cache never holds more than two generations (8,192 layouts)' "max $max"
  $turns = if ($mem.Count -and $mem[-2] -match '(\d+) generations let go') { [int]$Matches[1] } else { 0 }
  Check ($turns -ge 2) 'the stream outgrew a generation (the bound was reached and kept)' "$turns"
  $kept = @($mem | Select-Object -Skip 1 | Select-Object -SkipLast 1 | Where-Object { $_ -match 'runs (\d+) ' -and [int]$Matches[1] -ge 1000 })
  Check ($kept.Count -eq [Math]::Max(0, $mem.Count - 2)) 'never dropped whole (each sample keeps at least a screen of runs)'
  $lines = @($mem | ForEach-Object { if ($_ -match 'terms (\d+) lines (\d+)') { [int]$Matches[2] / [Math]::Max(1, [int]$Matches[1]) } })
  Check ((($lines | Measure-Object -Maximum).Maximum) -le 10100) 'no terminal holds more than its 10,000 lines of scroll-back + the screen'
  $fr = @($mem | ForEach-Object { if ($_ -match 'frames (\d+)') { [int]$Matches[1] } })
  Check ($fr.Count -ge 2 -and $fr[-1] -gt $fr[0] + 100) 'the panes were painted all along' "$($fr -join ',')"
  Check (@($o | Where-Object { $_ -match 'MEM drop runs' }).Count -eq 1) 'the cache can be emptied (dump mem drop)'
}

''
"remote-view: $($script:pass) passed, $($script:fail.Count) failed"
if ($script:fail.Count) { $script:fail | ForEach-Object { "  - $_" }; exit 1 }
exit 0
