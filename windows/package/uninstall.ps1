# uninstall.ps1 - remove what install.ps1 put on this PC, and only that.
#   powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 [-Destination <folder>]
# It reads <Destination>\install-manifest.json (default: the folder this script is in when that holds one, else
# %LOCALAPPDATA%\Programs\ClaudeDeckNative) and removes:
#   - every shortcut the manifest lists (Start menu, auto-start) - but only one that still points at this install's
#     exe: a shortcut you re-pointed at something else is yours and stays;
#   - every file the manifest lists, then the folders that are empty afterwards, then the manifest.
# A file in the program folder that install.ps1 did not put there stays, and the folder with it.
# IT NEVER REMOVES YOUR STATE: %APPDATA%\claude-deck-native (config.json, local\tiles.json, local\settings.json,
# the cloud panes' sign-ins) is the deck's own and stays; delete that folder yourself if you want it gone.
# Nothing of the Electron deck is touched.
param([string]$Destination = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ShortcutTools.ps1')

if (-not $Destination) {
  if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'install-manifest.json')) { $Destination = $PSScriptRoot }
  else { $Destination = Join-Path $env:LOCALAPPDATA ('Programs\' + $DeckProgramsDir) }
}
$Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
$manifestFile = Join-Path $Destination 'install-manifest.json'
if (-not (Test-Path -LiteralPath $manifestFile)) { throw "nothing to uninstall: no install-manifest.json in $Destination" }
$m = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
$exe = Join-Path $Destination $DeckExeName
if (Test-FileLocked $exe) { throw 'the deck is running: close it (all its windows), then run this again' }
Set-Location -LiteralPath ([IO.Path]::GetPathRoot($Destination))      # never stand in the folder being removed

$kept = @()
foreach ($lnk in @($m.shortcuts)) {
  if (-not $lnk -or -not (Test-Path -LiteralPath $lnk -PathType Leaf)) { continue }
  $target = ''
  try { $target = (Read-DeckShortcut $lnk).Target } catch { }
  if (Test-SamePath $target $exe) { Remove-Item -LiteralPath $lnk -Force; Write-Host "removed  $lnk" }
  else { $kept += $lnk; Write-Host "kept     $lnk  (it points at '$target', not at this install)" }
}
$dirs = @{}
foreach ($rel in @($m.files)) {
  if (-not $rel -or [IO.Path]::IsPathRooted($rel) -or $rel -match '(^|\\)\.\.(\\|$)') { continue }     # only below the destination
  $p = Join-Path $Destination $rel
  if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p -Force }
  $d = Split-Path -Parent $p
  while ($d -and $d.Length -gt $Destination.Length) { $dirs[$d] = $true; $d = Split-Path -Parent $d }
}
Remove-Item -LiteralPath $manifestFile -Force
# what an update of a running deck left (install.ps1 moves the running exe aside) and the deck never got to remove
if (Test-Path -LiteralPath "$exe.old" -PathType Leaf) { Remove-Item -LiteralPath "$exe.old" -Force -ErrorAction SilentlyContinue }
foreach ($d in ($dirs.Keys | Sort-Object { $_.Length } -Descending)) {
  if ((Test-Path -LiteralPath $d) -and -not @([IO.Directory]::GetFileSystemEntries($d)).Count) { [IO.Directory]::Delete($d) }
}
$left = @()
if (Test-Path -LiteralPath $Destination) { $left = @([IO.Directory]::GetFileSystemEntries($Destination)) }
if (-not $left.Count) {
  if (Test-Path -LiteralPath $Destination) { [IO.Directory]::Delete($Destination) }
  Write-Host "removed  $Destination"
} else {
  Write-Host "removed  the installed files from $Destination; the folder stays because it holds what this installer did not put there:"
  $left | ForEach-Object { Write-Host "           $_" }
}
Write-Host "kept     your state: $(Join-Path $env:APPDATA $DeckStateDir)  (never removed by this script)"
