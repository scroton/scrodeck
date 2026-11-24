# windows/package/sign-release.ps1 - sign a release's files with the EDITION's release key (core/deck/edition.hpp
# DECK_EDITION_RELEASE_KEY: the one key the exe's updater trusts, windows/src/update.hpp; KEYS at the repo root holds
# its public part). For each file: <file>.asc (a detached, armored signature); for the set: SHA256SUMS (sha256sum's
# format) and SHA256SUMS.asc, written beside the first file. Anyone can check them with gpg and KEYS.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/package/sign-release.ps1 -Files <exe>,<zip> [-Public] [-Key <fingerprint>]
#   -Public  in the maintainer's private checkout: the PUBLIC edition's key (tools/public-export/overlay/core/deck/
#            edition.hpp), for a release of the public build; a public checkout is that edition already
#   -Key     any other fingerprint (a test key)
# The key has no passphrase: nothing is asked. It must be in this machine's GPG keyring (Git for Windows' gpg,
# ~/.gnupg); the script stops if it is not. Nothing secret is printed or written.
param([Parameter(Mandatory = $true)][string[]]$Files, [switch]$Public, [string]$Key = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'windows\edition.ps1')
$fpr = $DeckEdition.ReleaseKey
if ($Public) {
  if ($DeckEdition.Public) { $fpr = $DeckEdition.ReleaseKey }
  else {
    $over = Join-Path $root 'tools\public-export\overlay\core\deck'
    if (-not (Test-Path -LiteralPath (Join-Path $over 'edition.hpp'))) { throw "-Public: no public edition file in $over" }
    $fpr = (Get-DeckEdition -From (Join-Path $root 'tools\public-export\overlay')).ReleaseKey
  }
}
if ($Key) { $fpr = $Key }
if ($fpr -notmatch '^[0-9A-F]{40}$') { throw "not a key fingerprint: '$fpr'" }
"key      $fpr ($(if ($Key) { '-Key' } elseif ($Public -or $DeckEdition.Public) { 'the public edition' } else { 'this edition' }))"
# Git for Windows' gpg first (its keyring is ~/.gnupg); a gpg on PATH (Gpg4win: %APPDATA%\gnupg) only without it
$gpg = 'C:\Program Files\Git\usr\bin\gpg.exe'
if (-not (Test-Path $gpg)) { $gpg = (Get-Command gpg -ErrorAction SilentlyContinue).Source }
if (-not $gpg -or -not (Test-Path $gpg)) { throw 'gpg not found (Git for Windows ships it)' }
& $gpg --batch --list-secret-keys $fpr *> $null
if ($LASTEXITCODE -ne 0) { throw "the release key $fpr is not in this machine's keyring" }
# (`powershell -File` hands `-Files a,b` over as ONE string: split it)
$full = @($Files | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() } | ForEach-Object { (Resolve-Path $_.Trim()).Path })
$dir = Split-Path $full[0] -Parent
$lines = @()
foreach ($f in $full) {
  $asc = "$f.asc"; Remove-Item $asc -ErrorAction SilentlyContinue
  & $gpg --batch --yes --pinentry-mode loopback --passphrase '' --local-user $fpr --armor --detach-sign --output $asc $f
  if ($LASTEXITCODE -ne 0) { throw "signing $f failed" }
  $h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLowerInvariant()
  $lines += "$h  $(Split-Path $f -Leaf)"
  "signed   $(Split-Path $f -Leaf)  sha256 $h"
}
$sums = Join-Path $dir 'SHA256SUMS'
[IO.File]::WriteAllText($sums, (($lines -join "`n") + "`n"))
Remove-Item "$sums.asc" -ErrorAction SilentlyContinue
& $gpg --batch --yes --pinentry-mode loopback --passphrase '' --local-user $fpr --armor --detach-sign --output "$sums.asc" $sums
if ($LASTEXITCODE -ne 0) { throw 'signing SHA256SUMS failed' }
"signed   SHA256SUMS ($($lines.Count) files)"
# (gpg reports on stderr; Windows PowerShell 5.1 would make each of those lines an error under 'Stop')
$ErrorActionPreference = 'Continue'
foreach ($f in $full + @($sums)) {
  $out = & $gpg --batch --status-fd 1 --verify "$f.asc" $f 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) { throw "the signature of $f does not verify" }
  # the signature must be the edition's key's, as the updater asks (VALIDSIG <fingerprint>)
  if (-not ($out | Where-Object { $_ -match "^\[GNUPG:\] VALIDSIG $fpr " })) { throw "the signature of $f is not by $fpr" }
}
'verified every signature'
