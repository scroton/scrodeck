# windows/edition.ps1 - the edition's names for the PowerShell scripts (build, package, tests): the #define lines of
# core/deck/edition.hpp, read here so they are written in ONE place. Dot-source it:
#     . (Join-Path $root 'windows\edition.ps1')          # $root = the repo's root
# and use $DeckEdition.Exe ('ClaudeDeckNative.exe' in the owner's copy, 'scroDeck.exe' in the public one),
# $DeckEdition.ExeBase (the process name), .Product, .The ("the Claude Deck" in a sentence), .StateDir, .SharedDir, .AppId, .RemoteAppId, .Shortcut,
# .Description, .RemoteName, .ProgramsDir, .ReleasesRepo, .ReleaseKey, .Public.
# A packaged script (no source tree beside it) reads edition.json, which make-package.ps1 writes from this.
function Get-DeckEdition([string]$From) {
  $here = if ($From) { $From } else { $PSScriptRoot }
  $json = Join-Path $here 'edition.json'
  if (Test-Path -LiteralPath $json) { $e = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json; $h = @{}; foreach ($p in $e.PSObject.Properties) { $h[$p.Name] = $p.Value }; return $h }
  $hpp = $null
  foreach ($c in @((Join-Path $here '..\core\deck\edition.hpp'), (Join-Path $here '..\..\core\deck\edition.hpp'), (Join-Path $here 'core\deck\edition.hpp'))) { if (Test-Path -LiteralPath $c) { $hpp = $c; break } }
  if (-not $hpp) { throw "edition: neither edition.json nor core\deck\edition.hpp near $here" }
  $t = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $hpp).Path)
  $d = @{}
  foreach ($m in [regex]::Matches($t, '(?m)^#define DECK_EDITION_([A-Z_]+) "([^"]*)"')) { $d[$m.Groups[1].Value] = $m.Groups[2].Value }
  $pub = [regex]::Match($t, 'constexpr bool kPublic = (true|false);').Groups[1].Value -eq 'true'
  $h = @{
    Public = $pub; Product = $d['PRODUCT']; The = $d['THE']; Exe = $d['EXE']; ExeBase = [IO.Path]::GetFileNameWithoutExtension($d['EXE'])
    StateDir = $d['STATE_DIR']; SharedDir = $d['SHARED_DIR']; AppId = $d['APP_ID']; RemoteAppId = $d['REMOTE_APP_ID']
    Description = $d['DESCRIPTION']; Shortcut = $d['SHORTCUT']; RemoteName = $d['REMOTE_NAME']; ProgramsDir = $d['PROGRAMS_DIR']
    ReleasesRepo = $d['RELEASES_REPO']; ReleaseKey = $d['RELEASE_KEY']
  }
  foreach ($k in 'Product', 'Exe', 'StateDir', 'SharedDir', 'AppId', 'Shortcut', 'ProgramsDir', 'ReleasesRepo', 'ReleaseKey') { if (-not $h[$k]) { throw "edition: $k missing in $hpp" } }
  return $h
}
$DeckEdition = Get-DeckEdition
