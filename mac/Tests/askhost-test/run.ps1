# mac/Tests/askhost-test/run.ps1 - the Mac ask pane's host document (mac/Sources/ClaudeDeck/AskHostPage.swift), run
# ON WINDOWS in headless Edge (or Chrome) against fake ask pages that speak the skill's host bridge (fakepage.html),
# driven the way AskPane.swift drives it (harness.js). What it proves: the JavaScript port of 49-asks.js's pane -
# drafts to the deck (askDraftGet / askDraft), Done (askDone with each page's answers), Copy all, Discard (the arm),
# a sent page, the log view, a read session, a replaced page, an older page, guest read-only, a refused call, and
# that nothing but a framed page naming its own id reaches the deck. What it does NOT prove: WebKit (this is Blink),
# the Swift around it (AskPane.swift: unbuilt here), the scheme handler, links, downloads, keys from AppKit.
#   powershell -NoProfile -ExecutionPolicy Bypass -File mac/Tests/askhost-test/run.ps1 [-Keep]
# Writes only under %TEMP%\deck-mac-askhost-* (removed unless -Keep). No port, no network, no deck, no Claude.
param([switch]$Keep)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$swift = Join-Path $root 'mac\Sources\ClaudeDeck\AskHostPage.swift'
$browser = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
  "$env:ProgramFiles\Google\Chrome\Application\chrome.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $browser) { throw 'no Edge or Chrome found' }

# the host document: the lines between `static let html = #"""` and `"""#`
$lines = Get-Content -LiteralPath $swift -Encoding UTF8
$start = -1; $end = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($start -lt 0 -and $lines[$i] -match 'static let html = #"""\s*$') { $start = $i + 1; continue }
  if ($start -ge 0 -and $lines[$i] -eq '"""#') { $end = $i; break }
}
if ($start -lt 0 -or $end -le $start) { throw 'the host document was not found in AskHostPage.swift' }
$doc = ($lines[$start..($end - 1)] -join "`n")

$work = Join-Path $env:TEMP ('deck-mac-askhost-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$pages = Join-Path $work 'pages\ask'
New-Item -ItemType Directory -Force $pages | Out-Null
$fake = Get-Content -LiteralPath (Join-Path $here 'fakepage.html') -Raw -Encoding UTF8
foreach ($id in 'ask_t1', 'ask_t3', 'ask_t4', 'ask_t5', 'ask_t6', 'ask_legacy1') { [IO.File]::WriteAllText((Join-Path $pages "$id.html"), $fake) }
$h = Get-Content -LiteralPath (Join-Path $here 'harness.js') -Raw -Encoding UTF8
$cut = $h.IndexOf('// ---------------------------------------------------------------- PRELUDE END')
$prelude = $h.Substring(0, $cut)
$driver = $h.Substring($cut)
$pagesUrl = 'file:///' + ((Join-Path $work 'pages') -replace '\\', '/')
$driver = $driver.Replace('__PAGES__', $pagesUrl)
if ($doc.IndexOf('<head>') -lt 0 -or $doc.IndexOf('</body>') -lt 0) { throw 'the host document has no <head> or </body>' }
$doc = $doc.Replace('<head>', "<head><script>`n$prelude`n</script>")
$doc = $doc.Replace('</body>', "<script>`n$driver`n</script></body>")
$hostFile = Join-Path $work 'host.html'
[IO.File]::WriteAllText($hostFile, $doc)

$udd = Join-Path $work 'profile'
$url = 'file:///' + ($hostFile -replace '\\', '/')
$bargs = @('--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--disable-extensions', "--user-data-dir=$udd",
  '--disable-features=IsolateSandboxedIframes,site-per-process,IsolateOrigins', '--disable-site-isolation-trials',
  '--virtual-time-budget=60000', '--dump-dom', $url)   # one renderer process: virtual time then covers the frames' messages too
$dom = & $browser @bargs 2>$null | Out-String
$all = [regex]::Matches($dom, '<pre id="askhost-results">([\s\S]*?)</pre>'); $m = if ($all.Count) { $all[$all.Count - 1] } else { [regex]::Match('', 'x') }
$code = 1
if (-not $m.Success) {
  Write-Host 'FAIL the driver left no results (the page did not run to its end):'
  Write-Host ($dom.Substring(0, [Math]::Min(3000, $dom.Length)))
} else {
  $text = [System.Net.WebUtility]::HtmlDecode($m.Groups[1].Value).Trim()
  Write-Host $text
  if ($text -match '(\d+) passed, 0 failed\s*$') { $code = 0 }
}
if (-not $Keep) { Start-Sleep -Milliseconds 300; Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue } else { Write-Host "kept: $work" }
exit $code
