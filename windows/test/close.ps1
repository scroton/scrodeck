# windows/test/close.ps1 — does the SCRATCH deck see a native remote client leave? (never the live deck)
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/close.ps1 [-Name close] [-Do "<script>"] [-Kill] [-Exe <other exe>]
# One headless remote run against the scratch deck (127.0.0.1:8799) while windows/test/deskcount.js samples the
# deck's own phoneStatus over its CDP port (9777) every 50 ms. Default script: connect, wait, `close` (the window's
# own WM_CLOSE path). Prints the timeline, and the verdict:
#   desks never above 1 while it ran, and desks 0 within 2 s of the process exiting.
# -Kill ends the process with TerminateProcess instead (no goodbye at all): what the deck sees of a crash.
# Nothing appears on screen: --shot shows no window, node runs inside this shell.
param([string]$Name = 'close', [string]$Do = 'wait 1200; ws count; close', [switch]$Kill, [string]$Exe = '', [int]$After = 1500)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')   # the edition's names: the exe ($DeckEdition.Exe), ...
$exe = if ($Exe) { $Exe } else { Join-Path $root ('windows\build\' + $DeckEdition.Exe) }
$out = Join-Path $root "windows\test\out\n-$Name.png"
$samples = Join-Path $root "windows\test\out\n-$Name.desks.txt"
Remove-Item $out, "$out.error.txt", "$out.log.txt", $samples -ErrorAction SilentlyContinue
$env:CLAUDE_DECK_LINK = 'http://127.0.0.1:8799/?t=nativetest00000001'
$node = (Get-Command node).Source
$sampler = Start-Process -FilePath $node -ArgumentList "`"$(Join-Path $PSScriptRoot 'deskcount.js')`" 20000 50" -WindowStyle Hidden -RedirectStandardOutput $samples -PassThru
Start-Sleep -Milliseconds 700
$a = "--no-save --shot `"$out`" --after $After --size 1000x700"
if ($Do) { $a += " --do `"$($Do -replace '"', '\"')`"" }
$p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
if ($Kill) { Start-Sleep -Milliseconds 3500; Stop-Process -Id $p.Id -Force }
$p.WaitForExit()
$exitAt = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
Start-Sleep -Milliseconds 3000
Stop-Process -Id $sampler.Id -Force -ErrorAction SilentlyContinue
"exit $($p.ExitCode) at @$exitAt"
if (Test-Path "$out.log.txt") { Get-Content "$out.log.txt" | Where-Object { $_ -match '^ws ' } }
$max = 0; $freeAt = $null; $lines = @(Get-Content $samples)
foreach ($l in $lines) {
  if ($l -match '^(\d+) clients (\d+) desks (\d+) @(\d+)') {
    $d = [int]$Matches[3]; $at = [long]$Matches[4]
    if ($d -gt $max) { $max = $d }
    "  {0,6} ms  clients {1} desks {2}  ({3:+0;-0} ms from the exit)" -f $Matches[1], $Matches[2], $d, ($at - $exitAt)
    if ($d -eq 0 -and $max -gt 0 -and $null -eq $freeAt) { $freeAt = $at - $exitAt } elseif ($d -gt 0) { $freeAt = $null }
  }
}
if ($max -eq 0) { 'VERDICT: FAIL - the deck never saw a desk (did it connect?)' }
elseif ($max -gt 1) { "VERDICT: FAIL - the deck held $max desks at once" }
elseif ($null -eq $freeAt) { 'VERDICT: FAIL - the desk slot was still held 3 s after the exit' }
elseif ($freeAt -gt 2000) { "VERDICT: FAIL - the desk slot was freed $freeAt ms after the exit" }
else { "VERDICT: ok - desks never above 1; the slot was free $freeAt ms from the exit (negative = before the process was gone)" }
