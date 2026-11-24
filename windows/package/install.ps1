# install.ps1 - put the native Claude Deck on this PC for the signed-in user. No admin, no registry.
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-AutoStart] [-Mode local|ask]
#       [-Destination <folder>] [-StartMenuDir <folder>] [-StartupDir <folder>] [-Source <package folder>]
#       [-NoShortcut] [-Force] [-Setup [setup.ps1's own options]]
# What it does, and nothing else:
#   1. copies the package (the folder this script is in, or -Source) to -Destination
#      (default %LOCALAPPDATA%\Programs\ClaudeDeckNative);
#   2. writes "Claude Deck Native.lnk" in -StartMenuDir (default: your Start menu's Programs folder) pointing AT THE
#      EXE, carrying the AppUserModelID the deck's toasts are sent under (demo.claude-deck-native; windows/src/
#      notify.hpp) - without that pair Windows shows no toast. -Mode local (default): it starts `--local`, the deck of
#      this PC. -Mode ask: no argument, the exe asks once (this PC, or another deck's link);
#   3. with -AutoStart: the same shortcut in -StartupDir (default: your Startup folder), so the deck starts when you
#      sign in. It starts the exe itself - never cmd, PowerShell or wscript - so no console window can flash;
#   4. writes install-manifest.json in the destination: the list uninstall.ps1 removes, and nothing beyond it.
#   5. with -Setup: runs setup.ps1 from the destination, in this window - the first run's critical settings (the
#      accounts and their labels) and "set up the rest with Claude Code" (setup.ps1 says what it does). Every argument
#      install.ps1 does not know is handed to it (-StateDir, -NoStart, ...). Without -Setup it says how to run it.
# It never touches the deck's state (%APPDATA%\claude-deck-native) or the Electron deck - only setup.ps1 writes
# there, and only with -Setup. It starts nothing (setup.ps1 starts your Claude session only when you say so).
# Run it again from a newer package to update. The deck may be RUNNING: Windows will not overwrite a running exe but
# lets it be renamed, so the running one is moved aside (ClaudeDeckNative.exe.old) and the new one copied in under
# the real name; the running deck then offers the restart itself ("Deck update waiting", within half a minute) and
# removes the .old at its next start. A font or page file the running deck holds open is left when it is unchanged.
param(
  [string]$Source = '',
  [string]$Destination = '',
  [string]$StartMenuDir = '',
  [string]$StartupDir = '',
  [ValidateSet('local', 'ask')][string]$Mode = 'local',
  [switch]$AutoStart,
  [switch]$NoShortcut,
  [switch]$Force,
  [switch]$Setup
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ShortcutTools.ps1')

if (-not $Source) { $Source = $PSScriptRoot }
$Source = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $Source $DeckExeName))) { throw "no $DeckExeName in ${Source}: run this from an unzipped package (or pass -Source)" }
if (-not $Destination) { $Destination = Join-Path $env:LOCALAPPDATA ('Programs\' + $DeckProgramsDir) }
if (-not $StartMenuDir) { $StartMenuDir = [Environment]::GetFolderPath('Programs') }
if (-not $StartupDir) { $StartupDir = [Environment]::GetFolderPath('Startup') }
$Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
$StartMenuDir = [IO.Path]::GetFullPath($StartMenuDir).TrimEnd('\')
$StartupDir = [IO.Path]::GetFullPath($StartupDir).TrimEnd('\')
if (Test-SamePath $Source $Destination) { throw 'the package is already in the destination: unzip it somewhere else and run install.ps1 from there' }
if ($Destination.StartsWith($Source + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'the destination is inside the package folder' }

$manifestFile = Join-Path $Destination 'install-manifest.json'
$destExe = Join-Path $Destination $DeckExeName
$old = $null
if (Test-Path -LiteralPath $Destination) {
  if (Test-Path -LiteralPath $manifestFile) { $old = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json }
  elseif (@([IO.Directory]::GetFileSystemEntries($Destination)).Count -and -not $Force) {
    throw "$Destination exists, is not empty and was not made by this installer (no install-manifest.json). Choose another -Destination, or -Force."
  }
}
# The deck is running from the destination: its exe cannot be overwritten, but it can be moved aside.
$aside = "$destExe.old"; $movedAside = $false
if ((Test-Path -LiteralPath $Destination) -and (Test-FileLocked $destExe)) {
  if (Test-Path -LiteralPath $aside) {
    try { Remove-Item -LiteralPath $aside -Force -ErrorAction Stop }
    catch { throw "the deck is running from the destination and an older copy of it ($DeckExeName.old) is still in use too: restart the deck, or close it, then run this again" }
  }
  try { [IO.File]::Move($destExe, $aside) }
  catch { throw 'the deck is running from the destination and its exe could not be moved aside: close it (all its windows), then run this again' }
  $movedAside = $true
}

# ---- 1. the files
$files = @([IO.Directory]::GetFiles($Source, '*', 'AllDirectories') | ForEach-Object { $_.Substring($Source.Length + 1) } | Where-Object { $_ -ne 'install-manifest.json' })
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
try {
  foreach ($rel in $files) {
    $to = Join-Path $Destination $rel; $from = Join-Path $Source $rel
    $dir = Split-Path -Parent $to
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    try { [IO.File]::Copy($from, $to, $true) }
    catch {
      # a file the running deck holds open (a font it loaded): nothing to do when it is the same file already
      $same = $movedAside -and (Test-Path -LiteralPath $to) -and ((Get-Item -LiteralPath $to).Length -eq (Get-Item -LiteralPath $from).Length)
      if ($same) { try { $same = (Get-FileHash -LiteralPath $to).Hash -eq (Get-FileHash -LiteralPath $from).Hash } catch { $same = $false } }
      if (-not $same) { throw }
    }
  }
} catch {
  # the running deck's exe goes back under its own name: a half-made update must not leave no exe behind
  if ($movedAside) { try { if (Test-Path -LiteralPath $destExe) { Remove-Item -LiteralPath $destExe -Force }; [IO.File]::Move($aside, $destExe) } catch { } }
  throw
}
# an update: what the older version installed and this one no longer has goes
if ($old -and $old.files) {
  foreach ($rel in $old.files) {
    if ($files -notcontains $rel) { $p = Join-Path $Destination $rel; if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p -Force } }
  }
}

# ---- 2. and 3. the shortcuts (always straight at the exe)
$arguments = ''
if ($Mode -eq 'local') { $arguments = '--local' }
$shortcuts = @()
if ($old -and $old.shortcuts) { $shortcuts = @($old.shortcuts) }      # an update keeps removing what the first install made
function Add-Shortcut([string]$dir) {
  $lnk = Join-Path $dir $DeckShortcutName
  Write-DeckShortcut $lnk $destExe $arguments
  if ($script:shortcuts -notcontains $lnk) { $script:shortcuts += $lnk }
  return $lnk
}
$startLnk = $null; $autoLnk = $null
if (-not $NoShortcut) { $startLnk = Add-Shortcut $StartMenuDir }
if ($AutoStart) { $autoLnk = Add-Shortcut $StartupDir }
# (an update run without -AutoStart leaves an auto-start entry an earlier install made: uninstall removes it)
if (-not $autoLnk) { $autoLnk = $shortcuts | Where-Object { Test-SamePath (Split-Path -Parent $_) $StartupDir } | Select-Object -First 1 }

# ---- 4. the manifest
$version = ('' + (Get-Item -LiteralPath $destExe).VersionInfo.FileVersion).Trim()
$manifest = [ordered]@{
  product = $DeckEdition.Shortcut; version = $version; installedAt = (Get-Date).ToString('o')
  destination = $Destination; exe = $destExe; mode = $Mode; appUserModelId = $DeckAppId
  shortcuts = @($shortcuts); autoStart = [bool]$autoLnk
  files = @($files)
}
Set-Content -LiteralPath $manifestFile -Value ($manifest | ConvertTo-Json -Depth 5) -Encoding UTF8

Write-Host "installed $($DeckEdition.Shortcut) $version"
if ($movedAside) { Write-Host ('  the deck is running: it will offer the restart itself ("Deck update waiting", within half a minute). Its old exe was moved to ' + $DeckExeName + '.old and goes at the deck''s next start.') }
Write-Host "  program     $Destination  ($($files.Count) files)"
if ($startLnk) { Write-Host "  Start menu  $startLnk  ->  $DeckExeName $arguments" } else { Write-Host '  Start menu  (no shortcut asked for; the exe makes its own entry the first time it runs)' }
if ($autoLnk) { Write-Host "  auto-start  $autoLnk  (starts the deck when you sign in)" } else { Write-Host '  auto-start  off (run install.ps1 -AutoStart to turn it on)' }
Write-Host "  state       $(Join-Path $env:APPDATA $DeckStateDir)  (made by the deck when it first runs; never touched by this script)"
Write-Host "  uninstall   powershell -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $Destination 'uninstall.ps1')`""

# ---- 5. the first-run setup (setup.ps1), when asked for
$setupScript = Join-Path $Destination 'setup.ps1'
if ($Setup) {
  if (-not (Test-Path -LiteralPath $setupScript)) { throw "no setup.ps1 in $Destination (an older package?)" }
  Write-Host ''
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $setupScript -ProgramDir $Destination @args
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
} elseif (Test-Path -LiteralPath $setupScript) {
  Write-Host "  setup       powershell -NoProfile -ExecutionPolicy Bypass -File `"$setupScript`"  (accounts first, then the rest with Claude Code)"
}
