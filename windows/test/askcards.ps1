# windows/test/askcards.ps1 — the NATIVE ask pane (windows/src/askcards.hpp) in the app, as a REMOTE: ClaudeDeckNative.exe
# as a desk of the FAKE deck of windows/web-test (asks.ps1: the real lib/phone.js server with fake deps, loopback, a
# made-up token, two sample ask pages), walked by windows/test/asks-native.txt with real clicks through the window's
# own handler (`ask nclick`) and typing (`text`). Checks: the pages came over HTTP with the deck's cookie and never a
# token in a URL, no web view was made, picks and typing went to the deck as drafts, a link went to the browser
# (logged only, a test run), Done sent the page's answers (page.html's shape) and the pane went on; and the same
# walk with --ask-pane web still runs the WebView2 fallback (its own page answers).
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/askcards.ps1 [-Exe <path>] [-Port 8797]
param([string]$Exe = '', [int]$Port = 8797)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$outDir = Join-Path $root 'windows\test\out'
$script:pass = 0; $script:fail = 0
function Check([string]$what, $cond) { if ($cond) { $script:pass++; "  ok    $what" } else { $script:fail++; "  FAIL  $what" } }
$asks = Join-Path $PSScriptRoot 'asks.ps1'
$P1 = 'ask_native-test_11111111_r1_1118-1935_c8f2'; $P2 = 'ask_bug-queue_aaaaaaaa_r1_1118-1935_c233'; $S2 = 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee'
$a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $asks, '-Name', 'askcards', '-Script', (Join-Path $PSScriptRoot 'asks-native.txt'), '-Port', "$Port", '-Warm', '1')
if ($Exe) { $a += @('-Exe', $Exe) }
$out = & powershell @a 2>&1 | Out-String
$log = Get-Content (Join-Path $outDir 'n-askcards.png.log.txt') -Raw -Encoding UTF8
$rec = Get-Content (Join-Path $outDir 'n-askcards.calls.json') -Raw | ConvertFrom-Json
'== askcards: the native ask pane as a remote of a (fake) deck'
Check 'the native pane (the default), no web view made' ($log -match 'ask pane: native \(askcards\.hpp\)' -and $log -match 'ask web: state \d+ \(2 = ready\) made 0' -and $out -match 'msedgewebview2\.exe of its profile 0')
$h = @($rec.http)
Check 'both pages came from the deck over HTTP, let in by the cookie, never a token in a URL' (@($h | Where-Object { $_.url -eq "/ask/$P1.html" -and $_.auth -eq 'cookie' }).Count -ge 1 -and @($h | Where-Object { $_.url -eq "/ask/$P2.html" -and $_.auth -eq 'cookie' }).Count -ge 1 -and @($h | Where-Object { $_.url -match '[?&]t=' }).Count -eq 0)
Check 'the pages drew their cards (5 and 2)' ($log -match ('"key":"ask:' + $P1 + '"') -and $log -match ('"key":"ask:' + $P2 + '"') -and $log -match '"count":"0 of 5 answered"')
$drafts = @($rec.calls | Where-Object { $_.fn -eq 'askDraft' -and $_.args[0] -eq $P2 })
$last = if ($drafts.Count) { $drafts[-1].args[1] } else { $null }
Check 'a pick and typing reached the deck as the page''s draft (S and its counts)' ($last -and $last.answered -eq 2 -and $last.state.Q1.pick -eq 'a' -and $last.state.Q2.text -eq 'add 150 MB instead')
Check 'a link on a page goes where the app sends links (a remote: the browser, http(s) only; a test only logs it)' ($log -match 'OPEN \(remote: http\(s\) only\) (https?|file):')
Check 'the wheel scrolls the pane' ($log -match '"scroll":[1-9]\d*')
$done = @($rec.calls | Where-Object { $_.fn -eq 'askDone' })
$A = if ($done.Count) { $done[0].args[1][0].answers } else { $null }
Check 'Done sent that session the page''s answers, in page.html''s shape' ($done.Count -eq 1 -and $done[0].args[0] -eq $S2 -and $A.kind -eq 'ask-page-answers' -and $A.version -eq 2 -and $A.id -eq $P2 -and $A.answered -eq 2 -and $A.total -eq 2 -and @($A.lines).Count -eq 2 -and $A.answers[0].choice -eq 'a')
Check '...and the pane said so (the other session still waits: it stays open)' ($log -match "Done: saved the answers file" -or $log -match '"flash":"Done: saved the answers file')
Check 'Copy all: the other session''s lines on the clipboard, said in its head' ($log -match '"flash":"Copied 2 lines: paste them into the session')
Check 'Discard answers…: the confirm row (Discard arms after a moment), then askClear for that page, and the pane goes (the session runs outside: its terminal)' ($log -match '"confirm":\{"armed":(true|false),"text":"Discard the 1 answer in progress for native-test' -and @($rec.calls | Where-Object { $_.fn -eq 'askClear' -and $_.args[0] -eq $P1 }).Count -eq 1 -and $log -match "ask pane: mode '' open 0")
Check 'F7 on the session''s tile: its page, sent, read-only' ($log -match "ask pane: mode '$S2' open 1" -and $log -match ('"key":"ask:' + $P2 + '","locked":true'))
Check 'nothing left behind' ($out -match '\(clean\)')
# ---- the fallback: the same deck, --ask-pane web, the old walk's first steps
'== askcards: --ask-pane web still runs the WebView2 pane (the fallback)'
$a2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $asks, '-Name', 'askcards-web', '-Port', "$Port", '-Extra', '--ask-pane web',
  '-Do', "wait 700; ask open all; wait 4000; ask dump; wait 300; ask in 0 document.querySelectorAll('.card')[0].querySelector('[data-pick]'); wait 1200; ask dump; close")
if ($Exe) { $a2 += @('-Exe', $Exe) }
$out2 = & powershell @a2 2>&1 | Out-String
$log2 = Get-Content (Join-Path $outDir 'n-askcards-web.png.log.txt') -Raw -Encoding UTF8
Check 'the web pane: its host document and its pages came up, a click on a page made a draft' ($log2 -match 'ask pane: web \(WebView2\)' -and $log2 -match 'host document ready 1' -and $out2 -match 'DECK GOT askDraft')
Check '...and its web view went with it' ($out2 -match '\(clean\)')
"askcards.ps1: $script:pass passed, $script:fail failed"
exit $script:fail
