# windows/test/wsraw.ps1 — the exe's own WebSocket client (windows/src/wsraw.hpp) against real servers on loopback.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/wsraw.ps1 [-Only unit,inflate,frames,tls,deck,nodeflate,breakraw,exe]
# Builds windows/build/wsraw_test.exe (a console test program: a message-only window, nothing on screen), then:
#   unit      the frame parser, client frames, the extension answer (no network)
#   inflate   PmdInflater against Node's zlib (wsraw-inflate.js)
#   frames    a hand-made server (wsraw-deck.js frames): every length edge, fragments with a ping inside, a frame a
#             byte at a time, compressed messages (fragmented, the window carried, a final block), the server's close
#   tls       the same over TLS (a throwaway self-signed certificate; the test client accepts it), and a client
#             that validates (the default) refusing it
#   deck      the REAL lib/phone.js server: connect, deskInit, a 200 KB replay intact AND compressed (the server's
#             bytes counted), the close handshake; then WinHTTP on the same deck for the comparison
#   nodeflate the server refuses the extension: the client works uncompressed
#   breakraw  the exe's own client cannot read what it gets: auto falls back to WinHTTP (and raw alone does not)
#   exe       ClaudeDeckNative.exe itself (headless, a temp --state-dir): auto = raw + compressed; the fallback logged once
# Node runs in this console (no window); ports 8841-8847 on 127.0.0.1, a made-up token. Exit 1 when a check fails.
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$node = (Get-Command node -ErrorAction Stop).Source
$test = Join-Path $root 'windows\build\wsraw_test.exe'
$work = Join-Path $env:TEMP ("cdn-wsraw-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force $work | Out-Null
$script:fail = @(); $script:pass = 0
function Check([bool]$ok, [string]$what, [string]$detail = '') { if ($ok) { $script:pass++; "  ok   $what" } else { $script:fail += $what; "  FAIL $what $detail" } }
function Want([string]$n) { return (-not $Only) -or (($Only -split ',') -contains $n) }
$token = 'wsrawtoken0000001'
$script:procs = @()

# ---------------------------------------------------------------------------------------------------- the build
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) { $gpp = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' | ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1 }
$srcs = @('windows\src\wsraw.hpp', 'windows\src\ws.hpp', 'windows\test\wsraw_test.cpp', 'core\deck\hash.cpp') | ForEach-Object { Join-Path $root $_ }
$stale = -not (Test-Path $test)
if (-not $stale) { $t = (Get-Item $test).LastWriteTimeUtc; $stale = @($srcs | Where-Object { (Get-Item $_).LastWriteTimeUtc -gt $t }).Count -gt 0 }
if ($stale) {
  'build wsraw_test.exe'
  & $gpp -std=c++17 -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wno-unknown-pragmas -O2 (Join-Path $root 'windows\test\wsraw_test.cpp') (Join-Path $root 'core\deck\hash.cpp') -o $test -static -lws2_32 -lsecur32 -lcrypt32 -lwinhttp
  if ($LASTEXITCODE -ne 0) { throw 'wsraw_test build failed' }
}

function Start-Deck([string]$mode, [int]$port, [string]$certDir = '') {
  $res = Join-Path $work "$mode.json"; Remove-Item $res -ErrorAction SilentlyContinue
  $a = '"{0}" {1} {2} "{3}"' -f (Join-Path $PSScriptRoot 'wsraw-deck.js'), $mode, $port, $res
  if ($certDir) { $a += ' "' + $certDir + '"' }
  $p = Start-Process -FilePath $node -ArgumentList $a -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $work "$mode.out.log") -RedirectStandardError (Join-Path $work "$mode.err.log")
  for ($i = 0; $i -lt 100 -and -not (Test-Path $res); $i++) { Start-Sleep -Milliseconds 100 }
  if (-not (Test-Path $res)) { Get-Content (Join-Path $work "$mode.err.log") -ErrorAction SilentlyContinue; throw "the $mode deck did not start" }
  $script:procs += $p
  return @{ proc = $p; result = $res }
}
function Stop-Deck($d) { if ($d -and $d.proc -and -not $d.proc.HasExited) { Stop-Process -Id $d.proc.Id -Force -ErrorAction SilentlyContinue } }
function Result($d) { Start-Sleep -Milliseconds 300; return (Get-Content $d.result -Raw | ConvertFrom-Json) }
function T([string[]]$a) { $out = & $test @a | ForEach-Object { "$_" }; $out | ForEach-Object { "    | $_" } | Out-Host; return ,$out }
function Has($lines, [string]$re) { return [bool]($lines | Where-Object { $_ -match $re }) }

try {
  if (Want 'unit') {
    '== unit'
    $o = T @('unit'); Check ($LASTEXITCODE -eq 0 -and (Has $o '^unit passed \d+ failed 0$')) 'the frame parser, client frames, extension answers'
  }
  if (Want 'inflate') {
    '== inflate'
    & $node (Join-Path $PSScriptRoot 'wsraw-inflate.js') (Join-Path $work 'cases.json') | Out-Host
    $o = T @('inflate', (Join-Path $work 'cases.json')); Check ($LASTEXITCODE -eq 0 -and (Has $o '^inflate passed \d+ failed 0$')) 'the inflater against Node''s zlib'
  }
  foreach ($kind in @('frames', 'tls')) {
    if (-not (Want $kind)) { continue }
    "== $kind"
    $cert = ''
    if ($kind -eq 'tls') {
      $ossl = (Get-Command openssl -ErrorAction SilentlyContinue).Source
      if (-not $ossl) { $ossl = @("$env:ProgramFiles\Git\usr\bin\openssl.exe", "$env:ProgramFiles\Git\mingw64\bin\openssl.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1 }
      if (-not $ossl) { Check $false 'openssl found (for the throwaway certificate)'; continue }
      $cert = Join-Path $work 'cert'; New-Item -ItemType Directory -Force $cert | Out-Null
      # in this console (no window); its progress dots go to a file, not PowerShell's error stream
      $q = Start-Process -FilePath $ossl -ArgumentList ('req -x509 -newkey rsa:2048 -nodes -keyout "{0}" -out "{1}" -days 1 -subj /CN=127.0.0.1' -f (Join-Path $cert 'key.pem'), (Join-Path $cert 'cert.pem')) -NoNewWindow -Wait -PassThru -RedirectStandardError (Join-Path $cert 'openssl.err.log') -RedirectStandardOutput (Join-Path $cert 'openssl.out.log')
      if (-not (Test-Path (Join-Path $cert 'cert.pem'))) { Check $false 'the throwaway certificate was made'; continue }
    }
    $port = if ($kind -eq 'tls') { 8842 } else { 8841 }
    $d = Start-Deck $kind $port $cert
    try {
      $scheme = if ($kind -eq 'tls') { 'https' } else { 'http' }
      $a = @('echo', "${scheme}://127.0.0.1:$port/?t=$token", '--mode', 'raw', '--seconds', '40'); if ($kind -eq 'tls') { $a += '--insecure' }
      $o = T $a
      $r = Result $d
      foreach ($s in $r.steps) { Check ([bool]$s.ok) ("${kind}: " + $s.name) $s.detail }
      Check ($r.steps.Count -ge 20) "${kind}: the whole script ran ($($r.steps.Count) steps)"
      Check ($r.errors.Count -eq 0) "${kind}: the server raised nothing" ($r.errors -join ' ')
      Check ([bool]$r.sockets[0].masked -and [bool]$r.sockets[0].fin) "${kind}: every client frame masked and FIN"
      Check ($r.sockets[0].offered -match '^permessage-deflate; client_max_window_bits$') "${kind}: the offer is permessage-deflate; client_max_window_bits" $r.sockets[0].offered
      $tlsBit = if ($kind -eq 'tls') { '1' } else { '0' }
      Check (Has $o ('^stats: tls ' + $tlsBit + ' deflate 1')) "${kind}: tls $tlsBit, deflate agreed"
      if ($kind -eq 'tls') {
        # the default: Schannel validates, and a self-signed certificate is refused (auto would then try WinHTTP)
        $o2 = T @('echo', "https://127.0.0.1:$port/?t=$token", '--mode', 'raw', '--seconds', '15')
        Check (Has $o2 '^failed: TLS failed \(0x800903') 'tls: a client that validates refuses the self-signed certificate' (($o2 | Where-Object { $_ -match '^failed' }) -join ' ')
      }
    } finally { Stop-Deck $d }
  }
  if (Want 'deck') {
    '== deck (the real lib/phone.js server)'
    $d = Start-Deck 'deck' 8843
    try {
      $o = T @('deck', "http://127.0.0.1:8843/?t=$token", '--mode', 'auto')
      $r = Result $d
      $sha = $r.replay.sha256
      Check (Has $o '^open 1 via raw, permessage-deflate \(permessage-deflate; server_max_window_bits=13') 'auto connects raw and the deck agrees permessage-deflate (server_max_window_bits=13)'
      Check (Has $o '^deskInit ok$') 'deskInit answered'
      Check (Has $o ("^replay bytes " + $r.replay.bytes + " sha256 " + $sha + '$')) "the 200 KB replay arrives intact ($($r.replay.bytes) bytes, sha256 the server's)"
      Check (Has $o '^stop: close frame sent 1, closed by the reader after the deck''s answer 1, closed hard 0') 'the goodbye: our close frame, the deck''s answer, closed by the reader'
      $s = $r.sockets | Where-Object { $_.via -eq 'raw' } | Select-Object -First 1
      Check ($s -and $s.closeFrameReceived -and $s.closeFrameSent -and $s.closeCode -eq 1000) 'the server saw the close handshake (received 1000, answered)' ($s | ConvertTo-Json -Compress -Depth 2)
      $ratio = if ($s.wireBytes) { [math]::Round($s.payloadBytes / $s.wireBytes, 2) } else { 0 }
      "  measured: payload $($s.payloadBytes) bytes in $($s.messages) messages, the server wrote $($s.wireBytes) bytes after the upgrade: ratio ${ratio}x"
      Check ($ratio -ge 3) "compressed on the wire (${ratio}x; the terminal replay squeezes)"
      $st = $o | Where-Object { $_ -match '^stats:' } | Select-Object -First 1
      Check ($st -match 'compressed [1-9]') 'the client inflated compressed messages' $st
      $script:rawRatio = $ratio; $script:rawWire = $s.wireBytes; $script:payload = $s.payloadBytes
      # the same deck over WinHTTP, for the comparison
      $o = T @('deck', "http://127.0.0.1:8843/?t=$token", '--mode', 'winhttp')
      $r = Result $d
      $w = $r.sockets | Where-Object { $_.via -eq 'winhttp' } | Select-Object -First 1
      Check (Has $o ("^replay bytes " + $r.replay.bytes + " sha256 " + $sha + '$')) 'winhttp: the replay arrives intact too'
      Check ($w -and $w.closeFrameReceived -and $w.closeCode -eq 1000) 'winhttp: its close handshake as before'
      if ($w -and $w.wireBytes) { "  measured: winhttp - the server wrote $($w.wireBytes) bytes for $($w.payloadBytes) bytes of payload ($([math]::Round($w.payloadBytes / $w.wireBytes, 2))x)" }
    } finally { Stop-Deck $d }
  }
  if (Want 'nodeflate') {
    '== nodeflate (the server refuses the extension)'
    $d = Start-Deck 'nodeflate' 8844
    try {
      $o = T @('deck', "http://127.0.0.1:8844/?t=$token", '--mode', 'auto')
      $r = Result $d
      Check (Has $o '^open 1 via raw, no compression offered by the deck') 'the extension refused: the raw client goes on uncompressed'
      Check (Has $o ("^replay bytes " + $r.replay.bytes + " sha256 " + $r.replay.sha256 + '$')) 'the replay arrives intact, uncompressed'
      Check (-not (Has $o '^fallback')) 'no fallback: nothing failed'
    } finally { Stop-Deck $d }
  }
  if (Want 'breakraw') {
    '== breakraw (the raw client cannot read what the deck sends)'
    $d = Start-Deck 'breakraw' 8845
    try {
      $o = T @('deck', "http://127.0.0.1:8845/?t=$token", '--mode', 'auto')
      $r = Result $d
      Check (Has $o '^fallback: a compressed message that does not inflate$') 'auto: the raw path fails (a message that does not inflate) -> WsFallback'
      Check (Has $o '^open 2 via winhttp') 'auto: WinHTTP connects for the rest of the run'
      Check (Has $o ("^replay bytes " + $r.replay.bytes + " sha256 " + $r.replay.sha256 + '$')) 'auto: deskInit and the replay over WinHTTP'
      Check (@($r.sockets | Where-Object { $_.broken }).Count -eq 1 -and @($r.sockets | Where-Object { $_.via -eq 'winhttp' }).Count -eq 1) 'the deck saw one raw socket (broken) and one WinHTTP socket'
      $raw = $r.sockets | Where-Object { $_.broken } | Select-Object -First 1
      Check ($raw.closeFrameReceived -and $raw.closeCode -eq 1002) 'the raw client told the deck why (close 1002, protocol error)' ($raw | ConvertTo-Json -Compress -Depth 2)
      $o = T @('deck', "http://127.0.0.1:8845/?t=$token", '--mode', 'raw', '--seconds', '10')
      Check ((Has $o '^failed: the exe''s own socket: a compressed message that does not inflate$') -and -not (Has $o '^fallback')) '"raw": the failure is reported, no fallback'
    } finally { Stop-Deck $d }
  }
  if (Want 'exe') {
    '== exe (ClaudeDeckNative.exe, headless)'
    $exe = Join-Path $root ('windows\build\' + $DeckEdition.Exe)
    foreach ($case in @(@{ mode = 'deck'; port = 8846; cfg = $null }, @{ mode = 'breakraw'; port = 8847; cfg = $null }, @{ mode = 'deck'; port = 8846; cfg = 'winhttp' })) {
      $d = Start-Deck $case.mode $case.port
      try {
        $st = Join-Path $work ("exe-" + [guid]::NewGuid().ToString('N').Substring(0, 6)); New-Item -ItemType Directory -Force $st | Out-Null
        if ($case.cfg) { Set-Content -Path (Join-Path $st 'config.json') -Value ('{"wsClient":"' + $case.cfg + '"}') -Encoding ascii }
        $env:CLAUDE_DECK_LINK = "http://127.0.0.1:$($case.port)/?t=$token"
        $png = Join-Path $work ("exe-" + $case.mode + "-" + ($case.cfg -replace '^$', 'auto') + ".png")
        $p = Start-Process -FilePath $exe -ArgumentList "--verify-scene --state-dir `"$st`" --shot `"$png`" --after 2500 --size 1000x700 --do `"wait 1500; ws count`"" -Wait -PassThru
        $log = if (Test-Path "$png.log.txt") { Get-Content "$png.log.txt" } else { @() }
        $log | Where-Object { $_ -match '^ws[ :]' } | ForEach-Object { "    | $_" }
        $name = "$($case.mode) / wsClient $(if ($case.cfg) { $case.cfg } else { '(default auto)' })"
        # ws.log beside config.json: the run's first open, ONE line, in every run (a real one too, 2026-11-24)
        $wl = Join-Path $st 'ws.log'; $wlines = if (Test-Path $wl) { @(Get-Content $wl) } else { @() }
        $wlines | ForEach-Object { "    ws.log | $_" }
        $opens = @($wlines | Where-Object { $_ -match '^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d ws open: ' })
        Check ($opens.Count -eq 1) "exe ${name}: ws.log holds the run's first open, once" "$($opens.Count) lines"
        if ($case.mode -eq 'breakraw') {
          Check (@($log | Where-Object { $_ -match '^ws: the exe''s own socket failed' }).Count -eq 1) "exe ${name}: the fallback is logged once"
          Check (Has $log '^ws open: winhttp') "exe ${name}: then WinHTTP opens"
          Check (Test-Path (Join-Path $st 'ws.log')) "exe ${name}: the line is in the state folder's ws.log"
        } elseif ($case.cfg -eq 'winhttp') {
          Check ((Has $log '^ws open: winhttp') -and -not (Has $log '^ws open: raw')) "exe ${name}: WinHTTP only"
          Check ($opens.Count -eq 1 -and $opens[0] -match 'ws open: winhttp') "exe ${name}: ws.log says WinHTTP (no compression)"
        } else {
          Check (Has $log '^ws open: raw, permessage-deflate') "exe ${name}: the exe's own client, compressed"
          Check ($opens.Count -eq 1 -and $opens[0] -match 'ws open: raw, permessage-deflate') "exe ${name}: ws.log says the exe's own client, compressed"
          Check (Has $log '^ws stop: close frame sent 1, closed by the reader after the deck''s answer 1, closed hard 0') "exe ${name}: the goodbye as before"
        }
        Check ($p.ExitCode -eq 0) "exe ${name}: exit 0" "exit $($p.ExitCode)"
      } finally { Stop-Deck $d; Remove-Item Env:CLAUDE_DECK_LINK -ErrorAction SilentlyContinue }
    }
  }
} finally {
  foreach ($p in $script:procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }   # every deck this run started
}
""
"wsraw: $($script:pass) passed, $($script:fail.Count) failed"
if ($script:fail.Count) { $script:fail | ForEach-Object { "  - $_" }; Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue; exit 1 }
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
exit 0
