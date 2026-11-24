# windows/test/scratch-deck.ps1 — a SCRATCH Electron deck for the native client's tests. Never the live deck.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/scratch-deck.ps1 start [-Dir <folder>]
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/scratch-deck.ps1 stop
#   ... status
# Its own --user-data-dir, CDP on 9777 (the live deck's is 9333), the phone server on 127.0.0.1:8799 (the live
# one's is 8788) with a throwaway token, notifications off, and ASK_PAGE_STATE_DIR pointed into the scratch
# folder: without that, lib/asks.js writes deck.json into the real ask-page state folder and overwrites the
# owner's ask settings. Minimised as soon as its window exists (SW_SHOWMINNOACTIVE), so nothing stays on the screen. The scratch deck lists the live
# deck's sessions under "Elsewhere": never adopt one.
param([Parameter(Position = 0)][string]$Action = 'status', [string]$Dir = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Dir) { $Dir = Join-Path $env:TEMP 'claude-deck-native-scratch' }
$udd = Join-Path $Dir 'udd'
$pidFile = Join-Path $Dir 'deck.pid'
$Token = 'nativetest00000001'
$Port = 8799
$Cdp = 9777

function Get-Deck {
  if (-not (Test-Path $pidFile)) { return $null }
  $id = [int](Get-Content $pidFile -TotalCount 1)
  $p = Get-Process -Id $id -ErrorAction SilentlyContinue
  if ($p -and $p.ProcessName -eq 'electron') { return $p }
  return $null
}

switch ($Action) {
  'start' {
    if (Get-Deck) { Write-Host "already running (pid $((Get-Deck).Id))"; break }
    New-Item -ItemType Directory -Force $udd, (Join-Path $Dir 'ask-state'), (Join-Path $Dir 'home') | Out-Null
    $settings = Join-Path $udd 'settings.json'
    if (-not (Test-Path $settings)) {
      $json = '{"phone":{"enabled":true,"port":' + $Port + ',"bind":"localhost","token":"' + $Token + '"},' +
        '"driver":"auto","notify":{"waiting":false,"done":false,"asks":false,"minTurnSec":20,"sound":false},' +
        '"hibernate":{"enabled":false}}'
      [IO.File]::WriteAllText($settings, $json, (New-Object Text.UTF8Encoding $false))
    }
    $electron = Join-Path $root 'node_modules\electron\dist\electron.exe'
    $env:ASK_PAGE_STATE_DIR = Join-Path $Dir 'ask-state'
    $env:CLAUDE_DECK_CWD = Join-Path $Dir 'home'
    # one quoted string: Start-Process joins an argument ARRAY with spaces and quotes nothing, so a data folder
    # with a space in its path was cut at the space and the deck booted on a different folder
    $argLine = '"{0}" "--user-data-dir={1}" --remote-debugging-port={2}' -f $root, $udd, $Cdp
    $p = Start-Process -FilePath $electron -ArgumentList $argLine -WorkingDirectory $root -WindowStyle Minimized -PassThru
    Set-Content -Path $pidFile -Value $p.Id -Encoding ascii
    # -WindowStyle Minimized does not hold: Electron shows its window itself, and on 2026-11-18 the scratch deck
    # sat visible on the screen for hours. Minimise its top-level windows once they exist, without activating them.
    Add-Type -Namespace ScratchDeck -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool IsIconic(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindowAsync(System.IntPtr h, int c);
public static int Minimise(uint want) { int n = 0; EnumWindows((h, l) => { uint q; GetWindowThreadProcessId(h, out q); if (q == want && IsWindowVisible(h) && !IsIconic(h)) { ShowWindowAsync(h, 7); n++; } return true; }, System.IntPtr.Zero); return n; }
'@ -ErrorAction SilentlyContinue
    foreach ($i in 1..40) { Start-Sleep -Milliseconds 250; if ([ScratchDeck.Win]::Minimise([uint32]$p.Id) -gt 0) { Start-Sleep -Milliseconds 400; [void][ScratchDeck.Win]::Minimise([uint32]$p.Id); break } }
    Write-Host "started pid $($p.Id); desk link http://127.0.0.1:$Port/?t=$Token ; CDP $Cdp ; data $udd"
  }
  'stop' {
    $p = Get-Deck
    if (-not $p) { Write-Host 'not running'; break }
    # the whole tree: the deck's helpers and the shells its tiles host
    & taskkill.exe /PID $p.Id /T /F | Out-Null
    Remove-Item $pidFile -ErrorAction SilentlyContinue
    Write-Host "stopped pid $($p.Id)"
  }
  default {
    $p = Get-Deck
    if ($p) { Write-Host "running pid $($p.Id); desk link http://127.0.0.1:$Port/?t=$Token" } else { Write-Host 'not running' }
  }
}
