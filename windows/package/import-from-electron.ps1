# import-from-electron.ps1 - a ONE-TIME copy of the Electron deck's state files into the native deck's state folder.
#   powershell -NoProfile -ExecutionPolicy Bypass -File import-from-electron.ps1 -Source <folder> -Destination <folder>
#       [-Files tiles.json,settings.json,...] [-PhoneOff] [-Force] [-AllowRunningSource] [-DryRun]
# For the owner's real switch (PACKAGING.md "Switching from the Electron deck"), with BOTH decks closed:
#   -Source "$env:APPDATA\claude-deck"  -Destination "$env:APPDATA\claude-deck-native\local"
# Neither folder has a default on purpose: nothing is read or written that was not named.
#
# The native deck's files are main.js's own shapes (core/deck is a port of it: tiles.json { tiles, groups, windows,
# deskLayout }, settings.json merged over the same defaults, triggers.json, held-prompts.json, usage-cache.json),
# so the import is a copy. Rules it keeps:
#   - it COPIES, never moves; it opens the source files for reading only and writes nothing in -Source;
#   - it refuses while the source deck runs (its running.json names a live process): the tiles would be the same
#     Claude sessions, open in two decks. -AllowRunningSource overrides, for a look at the settings only;
#   - it refuses while the destination deck runs (it would write its own tiles.json over the import);
#   - a file already in the destination is never replaced unless -Force, and then it is first moved to
#     <Destination>\import-backup-<time>\;
#   - each file must parse as JSON (tiles.json must hold a `tiles` list) or nothing at all is copied;
#   - relaunch.json is NEVER imported (it names the Electron deck's launcher: the native deck would restart into
#     the Electron one), nor running.json, uploads\, trigger-inbox\ or any backup folder.
# -PhoneOff: the copied settings.json gets phone.enabled = false. Use it to try the native deck while the Electron
#   one still serves the phone: the port (8788) and the token are copied as they are, and two decks cannot listen
#   on one port. Without it the native deck serves the phone exactly as the Electron one did - the same link.
param(
  [Parameter(Mandatory = $true)][string]$Source,
  [Parameter(Mandatory = $true)][string]$Destination,
  [string[]]$Files = @('tiles.json', 'settings.json', 'triggers.json', 'held-prompts.json', 'usage-cache.json', 'ask-drafts.json'),
  [switch]$PhoneOff,
  [switch]$Force,
  [switch]$AllowRunningSource,
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$never = 'relaunch.json', 'running.json', 'config.json', 'deck-state.json'
if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "no source folder: $Source" }
$Source = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
$Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
$s = $Source.ToLowerInvariant() + '\'; $d = $Destination.ToLowerInvariant() + '\'
if ($s -eq $d -or $d.StartsWith($s) -or $s.StartsWith($d)) { throw 'source and destination must be two separate folders (neither inside the other)' }

function Read-Shared([string]$path) {            # read only, sharing with a writer: the source is never locked
  $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
  try { $b = New-Object byte[] $fs.Length; $n = 0; while ($n -lt $b.Length) { $r = $fs.Read($b, $n, $b.Length - $n); if ($r -le 0) { break }; $n += $r }; return ,$b } finally { $fs.Close() }
}
function Text-Of([byte[]]$b) { $t = [Text.Encoding]::UTF8.GetString($b); if ($t.Length -and $t[0] -eq [char]0xFEFF) { $t = $t.Substring(1) }; return $t }
function Deck-Runs([string]$folder) {            # main.js markRunning: running.json { pid, at } while the deck is up
  $f = Join-Path $folder 'running.json'
  if (-not (Test-Path -LiteralPath $f)) { return $false }
  try { $procId = [int](Text-Of (Read-Shared $f) | ConvertFrom-Json).pid } catch { return $false }
  if ($procId -le 0) { return $false }
  return [bool](Get-Process -Id $procId -ErrorAction SilentlyContinue)
}
if ((Deck-Runs $Source) -and -not $AllowRunningSource) { throw "the deck that owns $Source is running (running.json): close it first - its sessions must not be open in two decks. (-AllowRunningSource overrides.)" }
if (Deck-Runs $Destination) { throw "the deck that owns $Destination is running: close it first (it would write its own files over the import)" }

# ---- read and check everything before anything is written
$plan = @()
foreach ($name in $Files) {
  if ($name -ne [IO.Path]::GetFileName($name) -or $name -notmatch '\.json$') { throw "not a state file name: $name" }
  if ($never -contains $name.ToLowerInvariant()) { throw "$name is never imported (see the top of this script)" }
  $from = Join-Path $Source $name
  if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { Write-Host "skip     $name  (not in the source)"; continue }
  $bytes = Read-Shared $from
  $text = Text-Of $bytes
  try { $json = $text | ConvertFrom-Json } catch { throw "$from does not parse as JSON: nothing was copied" }
  if ($name -eq 'tiles.json' -and $null -eq $json.tiles) { throw "$from holds no 'tiles' list: nothing was copied" }
  if ($name -eq 'settings.json' -and $PhoneOff) {
    if ($null -ne $json.phone) {
      # only the one value changes: the text is edited, not rewritten (the deck's own writer keeps its formatting)
      $rx = New-Object regex '("phone"\s*:\s*\{[^{}]*?"enabled"\s*:\s*)true'
      if ($json.phone.enabled -eq $true) {
        if (-not $rx.IsMatch($text)) { throw 'settings.json: phone.enabled could not be switched off in place: nothing was copied' }
        $text = $rx.Replace($text, '${1}false', 1)
        if (($text | ConvertFrom-Json).phone.enabled -ne $false) { throw 'settings.json: the edit did not take: nothing was copied' }
        $bytes = (New-Object Text.UTF8Encoding $false).GetBytes($text)
      }
    }
  }
  $to = Join-Path $Destination $name
  $exists = Test-Path -LiteralPath $to
  if ($exists -and -not $Force) { throw "$to exists already: the native deck has state of its own. Nothing was copied. (-Force replaces it, after moving it to an import-backup folder.)" }
  $plan += [pscustomobject]@{ Name = $name; To = $to; Bytes = $bytes; Exists = $exists }
}
if (-not $plan.Count) { Write-Host 'nothing to import'; return }
if ($DryRun) { $plan | ForEach-Object { Write-Host ("would copy  {0}  ({1:N0} bytes){2}" -f $_.Name, $_.Bytes.Length, $(if ($_.Exists) { '  replacing the one there (backed up first)' } else { '' })) }; return }

# ---- write
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$backup = Join-Path $Destination "import-backup-$stamp"
$sha = [Security.Cryptography.SHA256]::Create()
$receipt = @()
foreach ($p in $plan) {
  if ($p.Exists) {
    if (-not (Test-Path -LiteralPath $backup)) { New-Item -ItemType Directory -Force -Path $backup | Out-Null }
    [IO.File]::Move($p.To, (Join-Path $backup $p.Name))
  }
  $tmp = $p.To + '.importing'
  [IO.File]::WriteAllBytes($tmp, $p.Bytes)
  [IO.File]::Move($tmp, $p.To)
  $hash = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($p.To)) | ForEach-Object { $_.ToString('x2') })
  $receipt += [ordered]@{ file = $p.Name; bytes = $p.Bytes.Length; sha256 = $hash; replaced = [bool]$p.Exists }
  Write-Host ("copied   {0}  ({1:N0} bytes){2}" -f $p.Name, $p.Bytes.Length, $(if ($p.Exists) { "  (the one there before: $backup)" } else { '' }))
}
$note = [ordered]@{ importedAt = (Get-Date).ToString('o'); from = $Source; phoneOff = [bool]$PhoneOff; files = @($receipt) }
Set-Content -LiteralPath (Join-Path $Destination 'imported-from-electron.json') -Value ($note | ConvertTo-Json -Depth 5) -Encoding UTF8
Write-Host "done: $($plan.Count) file(s) into $Destination. The source was only read."
