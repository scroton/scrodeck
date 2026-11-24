# setup.ps1 - the first run of the native Claude Deck: the critical settings first, then "set up the rest with Claude
# Code". Per user, NO admin: the one step that needs it (a machine-wide Tailscale install) is elevated alone, after
# saying why, and only when you say yes.
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup.ps1 [-Report]
#       [-StateDir <folder>] [-HomeDir <folder>] [-ProgramDir <folder>] [-OtherStateDirs "<folder>;<folder>"|none]
#       [-Accounts "e-mail|label[|folder[|keyword]];..."] [-UseFound yes|no|import]
#       [-InstallClaude yes|no] [-InstallTailscale yes|no] [-Chrome yes|no] [-AskMode necessary|as-you-go]
#       [-Session terminal|paste|later] [-Command <command line>] [-ClaudeExe <path>] [-TailscaleExe <path>] [-WingetExe <path>]
#       [-Defaults] [-NoStart] [-NoGui]
# What it does, in order:
#   0. an earlier run's report (<StateDir>\setup\setup-report.md, written by the Claude session): its STATUS line and
#      its "For you to do" list. -Report prints the whole report and stops.
#   1. THE ACCOUNTS, only when the state folder has no settings.json. Found ones are offered first: the account
#      labels in another deck's settings.json `aliases` (-OtherStateDirs, default the Electron deck's
#      %APPDATA%\claude-deck), and the alias file ~\.claude-code-account-aliases with each folder's signed-in e-mail.
#      Else (or as well) asked for: the login e-mail, a short label (vv / ms / ts), the config folder, the keyword.
#      Written as <StateDir>\settings.json { "aliases": { "<e-mail>": "<label>", "<keyword>": "<label>" } }: the shape
#      main.js and core/deck read (refreshAccounts: the label is found by the signed-in e-mail, before the sign-in by
#      the account's keyword). Not while that deck runs (its running.json names a live process).
#   2. Claude Code missing: Anthropic's own per-user installer (irm https://claude.ai/install.ps1 | iex) is offered,
#      run in THIS window.
#   3. Tailscale missing: the ONE elevated step is offered - its machine-wide install (winget, Start-Process -Verb
#      RunAs: one UAC prompt, the console hidden). Declined or impossible: the Claude session walks you through it.
#   4. "Set up the rest with Claude Code": Claude in Chrome is asked about (a browser extension cannot be installed by
#      a script: its Web Store page is opened for you), then the ask mode; the prompt is written to
#      <StateDir>\setup\claude-setup-prompt.md, filled in from the program folder's setup\claude-setup-prompt.md; the
#      command line is shown in an editable box and started in a NORMAL PowerShell window (deliberately visible: it is
#      your session; never as administrator), or the file's path is shown for a session you open yourself.
#   5. <StateDir>\setup\setup-installer.json: what was found, chosen and done (read by the next run and the tests).
# -NoStart: nothing is started, opened or elevated - the files are written and the plan printed (the tests use it
#   with every folder in TEMP). -Defaults: a question no parameter answers takes its default, so nothing is asked.
# Writes nothing outside <StateDir>; reads the alias file and each account folder's .claude.json, changes neither.
param(
  [switch]$Report,
  [string]$StateDir = '',
  [string]$HomeDir = '',
  [string]$ProgramDir = '',
  [string]$OtherStateDirs = '',
  [string]$Accounts = '',
  [ValidateSet('', 'yes', 'no', 'import')][string]$UseFound = '',
  [ValidateSet('', 'yes', 'no')][string]$InstallClaude = '',
  [ValidateSet('', 'yes', 'no')][string]$InstallTailscale = '',
  [ValidateSet('', 'yes', 'no')][string]$Chrome = '',
  [ValidateSet('', 'necessary', 'as-you-go')][string]$AskMode = '',
  [ValidateSet('', 'terminal', 'paste', 'later')][string]$Session = '',
  [string]$Command = '',
  [string]$ClaudeExe = '',
  [string]$TailscaleExe = '',
  [string]$WingetExe = '',
  [switch]$Defaults,
  [switch]$NoStart,
  [switch]$NoGui
)
$ErrorActionPreference = 'Stop'
. $(if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'edition.ps1')) { Join-Path $PSScriptRoot 'edition.ps1' } else { Join-Path $PSScriptRoot '..\edition.ps1' })   # the edition's names (beside it in a package; windows\edition.ps1 in a checkout)
$ChromeUrl = 'https://chromewebstore.google.com/detail/claude/fcoeoabgfenejglbffodgkkbkcdhcgfn'
$ClaudeInstall = 'irm https://claude.ai/install.ps1 | iex'
$WingetTailscale = 'install --id Tailscale.Tailscale -e --silent --accept-source-agreements --accept-package-agreements'
$utf8 = New-Object Text.UTF8Encoding $false

if (-not $StateDir) { $StateDir = Join-Path $env:APPDATA ($DeckEdition.StateDir + '\local') }
if (-not $HomeDir) { $HomeDir = $env:USERPROFILE }
if (-not $ProgramDir) { $ProgramDir = $PSScriptRoot }
$StateDir = [IO.Path]::GetFullPath($StateDir).TrimEnd('\')
$HomeDir = [IO.Path]::GetFullPath($HomeDir).TrimEnd('\')
$ProgramDir = [IO.Path]::GetFullPath($ProgramDir).TrimEnd('\')
if (-not $OtherStateDirs) {
  $o = @(Join-Path $env:APPDATA 'claude-deck')
  $nativeDefault = Join-Path $env:APPDATA ($DeckEdition.StateDir + '\local')
  if ($nativeDefault.TrimEnd('\') -ne $StateDir) { $o += $nativeDefault }
  $others = $o
} elseif ($OtherStateDirs -eq 'none') { $others = @() }
else { $others = @($OtherStateDirs -split ';' | Where-Object { $_.Trim() } | ForEach-Object { [IO.Path]::GetFullPath($_.Trim()).TrimEnd('\') }) }
$setupDir = Join-Path $StateDir 'setup'
$reportFile = Join-Path $setupDir 'setup-report.md'
$promptOut = Join-Path $setupDir 'claude-setup-prompt.md'
$receiptFile = Join-Path $setupDir 'setup-installer.json'
$settingsFile = Join-Path $StateDir 'settings.json'
$notes = New-Object Collections.ArrayList          # what the installer did / could not do: goes into the prompt
$receipt = [ordered]@{ at = (Get-Date).ToString('o'); stateDir = $StateDir; homeDir = $HomeDir; programDir = $ProgramDir; noStart = [bool]$NoStart }

# ---------------------------------------------------------------- helpers
function Say([string]$s) { Write-Host $s }
function Note([string]$s) { [void]$script:notes.Add($s) }
function Read-Text([string]$path) {               # shared read: a file the deck is writing is never locked
  $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
  try { $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8, $true); return $sr.ReadToEnd() } finally { $fs.Close() }
}
function Read-Json([string]$path) { try { return (Read-Text $path | ConvertFrom-Json) } catch { return $null } }
function Write-Text([string]$path, [string]$text) {
  $dir = Split-Path -Parent $path
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  $tmp = "$path.writing"
  [IO.File]::WriteAllText($tmp, $text, $script:utf8)
  if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
  [IO.File]::Move($tmp, $path)
}
function Ask-YesNo([string]$given, [string]$question, [bool]$default) {
  if ($given) { return ($given -eq 'yes') }
  $d = 'y/N'; if ($default) { $d = 'Y/n' }
  if ($Defaults) { Say "$question [$d] -> $(if ($default) { 'yes' } else { 'no' }) (default)"; return $default }
  while ($true) {
    $a = ('' + (Read-Host "$question [$d]")).Trim().ToLowerInvariant()
    if (-not $a) { return $default }
    if ($a -eq 'y' -or $a -eq 'yes') { return $true }
    if ($a -eq 'n' -or $a -eq 'no') { return $false }
  }
}
function Ask-Choice([string]$given, [string]$question, [string[]]$choices, [string]$default) {
  if ($given) { return $given }
  if ($Defaults) { Say "$question -> $default (default)"; return $default }
  while ($true) {
    $a = ('' + (Read-Host "$question [$($choices -join ' / ')] (Enter = $default)")).Trim().ToLowerInvariant()
    if (-not $a) { return $default }
    $hit = @($choices | Where-Object { $_ -eq $a -or $_.StartsWith($a) })
    if ($hit.Count -eq 1) { return $hit[0] }
  }
}
function Ask-Text([string]$question, [string]$default) {
  if ($Defaults) { return $default }
  $suffix = ''; if ($default) { $suffix = " (Enter = $default)" }
  $a = ('' + (Read-Host "$question$suffix")).Trim()
  if (-not $a) { return $default }
  return $a
}
function Deck-Runs([string]$folder) {             # main.js markRunning: running.json { pid, at } while the deck is up
  $j = Read-Json (Join-Path $folder 'running.json')
  if (-not $j) { return $false }
  try { $procId = [int]$j.pid } catch { return $false }
  return ($procId -gt 0 -and [bool](Get-Process -Id $procId -ErrorAction SilentlyContinue))
}
function Expand-Folder([string]$p) {              # ~, $HOME, ${HOME}, %USERPROFILE% -> the home folder; / -> \
  $p = $p.Trim().Trim('"').Trim("'")
  foreach ($pre in '~', '${HOME}', '$HOME', '%USERPROFILE%') {
    if ($p.StartsWith($pre, [StringComparison]::OrdinalIgnoreCase) -and ($p.Length -eq $pre.Length -or ('\/').IndexOf($p[$pre.Length]) -ge 0)) { $p = $script:HomeDir + $p.Substring($pre.Length); break }
  }
  $p = $p.Replace('/', '\')
  if (-not [IO.Path]::IsPathRooted($p)) { $p = Join-Path $script:HomeDir $p }
  return [IO.Path]::GetFullPath($p).TrimEnd('\')
}
function Email-Of([string]$folder) {              # the account's signed-in e-mail (main.js refreshAccounts)
  $f = Join-Path $folder '.claude.json'
  if (-not (Test-Path -LiteralPath $f)) { return '' }
  $j = Read-Json $f
  if ($j -and $j.oauthAccount -and $j.oauthAccount.emailAddress) { return [string]$j.oauthAccount.emailAddress }
  return ''
}
# the alias file, parsed as core/deck/rules.cpp parseAccounts does: one account per folder, its shortest keyword is
# its id; the plain ~\.claude account is dropped when named ones exist
function Read-AliasAccounts {
  $file = Join-Path $script:HomeDir '.claude-code-account-aliases'
  $out = @()
  if (-not (Test-Path -LiteralPath $file)) { return ,$out }
  $default = Join-Path $script:HomeDir '.claude'
  $byDir = [ordered]@{}
  foreach ($line in (Read-Text $file) -split "`n") {
    $m = [regex]::Match($line.TrimEnd("`r"), '^\s*alias\s+([A-Za-z0-9_]+)=([''"])(.*)\2\s*$')
    if (-not $m.Success) { continue }
    $kw = $m.Groups[1].Value; $rhs = $m.Groups[3].Value
    $dir = $default
    $d = [regex]::Match($rhs, 'CLAUDE_CONFIG_DIR=(?:"([^"]*)"|''([^'']*)''|(\S+))')
    if ($d.Success) { $dir = Expand-Folder ($d.Groups[1].Value + $d.Groups[2].Value + $d.Groups[3].Value) }
    $key = $dir.ToLowerInvariant()
    if (-not $byDir.Contains($key)) { $byDir[$key] = [pscustomobject]@{ Folder = $dir; Keywords = @() } }
    $byDir[$key].Keywords += $kw
  }
  foreach ($a in $byDir.Values) {
    if ($a.Folder -eq $default -and $byDir.Count -gt 1) { continue }
    $id = ($a.Keywords | Sort-Object Length | Select-Object -First 1)
    $out += [pscustomobject]@{ Email = (Email-Of $a.Folder); Label = ''; Folder = $a.Folder; Keyword = $id; Keywords = $a.Keywords; From = 'the alias file' }
  }
  return ,$out
}
function Check-Account($a, $all) {                # '' when fine, else why not
  if ($a.Label -notmatch '^[A-Za-z0-9]{1,8}$') { return "the label '$($a.Label)' must be 1-8 letters or digits (vv, ms, ts)" }
  if ($a.Keyword -notmatch '^[A-Za-z0-9_]+$' -or $a.Keyword -eq 'claude') { return "the keyword '$($a.Keyword)' must be letters, digits and _ only, and not plain 'claude'" }
  if ($a.Email -and $a.Email -notmatch '^[^\s"''\\|]+@[^\s"''\\|]+$') { return "'$($a.Email)' does not look like an e-mail address" }
  if ($a.Folder -eq (Join-Path $script:HomeDir '.claude')) { return 'an account needs its own folder, not ~\.claude itself (the deck hides that one once named accounts exist)' }
  foreach ($b in $all) {
    if ($b.Label -eq $a.Label) { return "the label '$($a.Label)' is taken" }
    if ($b.Keyword -eq $a.Keyword) { return "the keyword '$($a.Keyword)' is taken" }
    if ($b.Folder -eq $a.Folder) { return "the folder $($a.Folder) is taken" }
  }
  return ''
}
function New-Account([string]$email, [string]$label, [string]$folder, [string]$keyword, [string]$from) {
  $label = $label.Trim()
  if (-not $folder) { $folder = '~\.claude-' + $label.ToLowerInvariant() }
  if (-not $keyword) { $keyword = 'claude' + $label.ToLowerInvariant() }
  return [pscustomobject]@{ Email = $email.Trim().ToLowerInvariant(); Label = $label; Folder = (Expand-Folder $folder); Keyword = $keyword.Trim(); Keywords = @($keyword.Trim()); From = $from }
}
function Show-Accounts($list) {
  Say ('  {0,-6} {1,-14} {2,-44} {3}' -f 'label', 'keyword', 'config folder', 'login e-mail')
  foreach ($a in $list) { Say ('  {0,-6} {1,-14} {2,-44} {3}' -f $(if ($a.Label) { $a.Label } else { '?' }), $a.Keyword, $a.Folder, $(if ($a.Email) { $a.Email } else { '(not signed in yet)' })) }
}

# ---------------------------------------------------------------- 0. an earlier run's report
if ($Report) {
  if (-not (Test-Path -LiteralPath $reportFile)) { Say "no setup report yet ($reportFile): the Claude session writes it when it is done"; exit 1 }
  Write-Host (Read-Text $reportFile); exit 0
}
Say ($DeckEdition.Shortcut + ' - setup')
Say "  state folder  $StateDir"
if (Test-Path -LiteralPath $reportFile) {
  $rt = Read-Text $reportFile
  $st = [regex]::Match($rt, '(?m)^STATUS:\s*(\S+)').Groups[1].Value
  $todo = [regex]::Match($rt, '(?ms)^## For you to do\s*\r?\n(.*?)(?=^## |\z)').Groups[1].Value.Trim()
  Say "  an earlier setup session reported: STATUS $(if ($st) { $st } else { '(none)' })  ($reportFile)"
  if ($todo) { foreach ($l in $todo -split "`r?`n") { Say "    $l" } }
  $receipt.earlierReport = [ordered]@{ status = $st; forYou = $todo }
}

# ---------------------------------------------------------------- 1. the accounts (only when the deck has no settings)
$accountList = @()
$labels = [ordered]@{}
$deckRuns = Deck-Runs $StateDir
$aliasAccounts = Read-AliasAccounts
if (Test-Path -LiteralPath $settingsFile) {
  Say "`n1. Accounts: the deck has settings already ($settingsFile) - left as they are."
  $cur = Read-Json $settingsFile
  foreach ($a in $aliasAccounts) {
    if ($cur -and $cur.aliases) { foreach ($k in @($a.Email, $a.Keyword)) { if ($k -and $cur.aliases.$k) { $a.Label = [string]$cur.aliases.$k; break } } }
    $accountList += $a
  }
  if ($accountList.Count) { Show-Accounts $accountList }
  Note "The deck had settings already ($settingsFile): the installer changed no account label. Check them in step 2d."
  $receipt.accounts = 'kept'
} else {
  Say "`n1. Accounts: the deck has no settings yet. The accounts and their short labels come first."
  # the labels other decks already have, by e-mail or keyword
  $found = [ordered]@{}; $foundFrom = @()
  foreach ($d in $others) {
    $sf = Join-Path $d 'settings.json'
    if (-not (Test-Path -LiteralPath $sf)) { continue }
    $j = Read-Json $sf
    if (-not $j -or -not $j.aliases) { continue }
    $n = 0
    foreach ($p in $j.aliases.PSObject.Properties) { if ($p.Value -and -not $found.Contains($p.Name)) { $found[$p.Name] = [string]$p.Value; $n++ } }
    if ($n) { $foundFrom += $sf }
  }
  foreach ($a in $aliasAccounts) {
    foreach ($k in @($a.Email) + @($a.Keywords)) { if ($k -and $found.Contains($k)) { $a.Label = $found[$k]; break } }
  }
  $use = $false
  if ($aliasAccounts.Count -or $found.Count) {
    Say '  Found on this PC:'
    if ($aliasAccounts.Count) { Show-Accounts $aliasAccounts } else { foreach ($k in $found.Keys) { Say "  $($found[$k])  <- $k" } }
    if ($foundFrom.Count) { Say "  (labels from $($foundFrom -join ', '); keywords and folders from $(Join-Path $HomeDir '.claude-code-account-aliases'))" }
    Say '  yes    = use these (you can add more after)'
    Say '  no     = type the accounts in yourself'
    Say '  import = stop here and bring ALL of the Electron deck''s state over first (import-from-electron.ps1)'
    $how = Ask-Choice $UseFound 'Use the accounts found?' @('yes', 'no', 'import') 'yes'
    if ($how -eq 'import') {
      $imp = Join-Path $ProgramDir 'import-from-electron.ps1'
      Say "`nClose BOTH decks first (wait until no electron process is left), then run:"
      Say "  powershell -NoProfile -ExecutionPolicy Bypass -File `"$imp`" -Source `"$(Join-Path $env:APPDATA 'claude-deck')`" -Destination `"$StateDir`" -DryRun"
      Say '  (without -DryRun to do it; windows\PACKAGING.md "Switching from the Electron deck"), then run setup.ps1 again.'
      $receipt.accounts = 'import'
      Write-Text $receiptFile ($receipt | ConvertTo-Json -Depth 6)
      exit 0
    }
    $use = ($how -eq 'yes')
  }
  if ($use) {
    foreach ($k in $found.Keys) { $labels[$k] = $found[$k] }
    foreach ($a in $aliasAccounts) {
      if (-not $a.Label) {
        # a guess: the folder's name after .claude- (~\.claude-ms -> ms), else a keyword after claude (claudems -> ms)
        $guess = (Split-Path -Leaf $a.Folder) -replace '^\.claude-?', ''
        if ($guess -notmatch '^[A-Za-z0-9]{1,8}$') { $guess = @($a.Keywords | ForEach-Object { $_ -replace '^claude', '' } | Where-Object { $_ -match '^[A-Za-z0-9]{1,8}$' } | Sort-Object Length)[0] }
        if (-not $guess) { $guess = '' }
        $a.Label = Ask-Text "  a short label for $($a.Keyword) ($($a.Folder))" $guess
      }
      if ($a.Label -match '^[A-Za-z0-9]{1,8}$') { $accountList += $a } else { Say "  $($a.Keyword): no usable label - left out (the session can add it)" }
    }
  }
  # typed in (or given with -Accounts)
  if ($Accounts) {
    foreach ($spec in ($Accounts -split ';' | Where-Object { $_.Trim() })) {
      $f = @($spec -split '\|')
      while ($f.Count -lt 4) { $f += '' }
      $a = New-Account $f[0] $f[1] $f[2] $f[3] 'typed in'
      $why = Check-Account $a $accountList
      if ($why) { Say "  -Accounts '$spec': $why"; exit 2 }
      $accountList += $a
    }
  } elseif (-not $Defaults -and (-not $use -or (Ask-YesNo '' 'Add another account?' $false))) {
    Say '  One account at a time; an empty e-mail ends the list.'
    Say '  (label: two or three letters shown on the tiles, like vv / ms / ts; keyword: what you type to start it)'
    while ($true) {
      $email = Ask-Text '  login e-mail' ''
      if (-not $email) { break }
      $label = Ask-Text '  short label' ''
      $folder = Ask-Text '  config folder' ('~\.claude-' + $label.ToLowerInvariant())
      $keyword = Ask-Text '  keyword' ('claude' + $label.ToLowerInvariant())
      $a = New-Account $email $label $folder $keyword 'typed in'
      $why = Check-Account $a $accountList
      if ($why) { Say "  not added: $why"; continue }
      $accountList += $a
      Say "  added: $($a.Label)  $($a.Keyword)  $($a.Folder)  $($a.Email)"
    }
  }
  foreach ($a in $accountList) {
    if ($a.Email -and -not $labels.Contains($a.Email)) { $labels[$a.Email] = $a.Label }
    foreach ($k in $a.Keywords) { if (-not $labels.Contains($k)) { $labels[$k] = $a.Label } }
  }
  if (-not $labels.Count) {
    Say '  No accounts named: the Claude session will ask for them.'
    Note 'No accounts were named in the installer: ask me for them (step 2).'
    $receipt.accounts = 'none'
  } elseif ($deckRuns) {
    Say "  The deck is running from $StateDir : its settings were NOT written (it would write its own over them)."
    Note "The deck was running, so the installer did NOT write the account labels: do step 2d once it is closed."
    $receipt.accounts = 'deck-running'
  } else {
    $settings = [ordered]@{ aliases = $labels }
    Write-Text $settingsFile (($settings | ConvertTo-Json -Depth 4) + "`n")
    Say "  wrote $($labels.Count) label(s) to $settingsFile"
    Note "The installer wrote the account labels to $settingsFile (``aliases``: e-mail and keyword -> label). Check them in step 2d; do not write them again."
    $receipt.accounts = 'written'
  }
}
$receipt.accountList = @($accountList | ForEach-Object { [ordered]@{ email = $_.Email; label = $_.Label; folder = $_.Folder; keyword = $_.Keyword; from = $_.From } })
$receipt.labels = $labels
Note 'The installer did NOT create account folders, write the alias file or the PowerShell profile, or sign in to any account (a script cannot sign in for you): step 2.'
Note 'The installer did NOT check the companion files (userswitch, the deck-triggers skill, Git for Windows, Node.js): step 3.'

# ---------------------------------------------------------------- 2. Claude Code
Say "`n2. Claude Code"
function Find-Claude {
  if ($ClaudeExe) { if (Test-Path -LiteralPath $ClaudeExe) { return $ClaudeExe } else { return '' } }
  $c = Get-Command claude -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($c) { return $c.Source }
  $local = Join-Path $HomeDir '.local\bin\claude.exe'
  if (Test-Path -LiteralPath $local) { return $local }
  return ''
}
$claudePath = Find-Claude
$receipt.claude = [ordered]@{ found = $claudePath; installed = $false }
if ($claudePath) { Say "  found: $claudePath" }
else {
  Say '  Claude Code is not installed (or not on the PATH). The session needs it.'
  Say "  Anthropic's installer is per user and needs no admin: $ClaudeInstall"
  if (Ask-YesNo $InstallClaude '  Install Claude Code now (in this window)?' $true) {
    if ($NoStart) { Say "  would run, in this window: powershell -NoProfile -ExecutionPolicy Bypass -Command `"$ClaudeInstall`"" ; $receipt.claude.wouldInstall = $true }
    else {
      & powershell.exe -NoProfile -ExecutionPolicy Bypass -Command $ClaudeInstall
      $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
      $claudePath = Find-Claude
      $receipt.claude.installed = [bool]$claudePath
      if ($claudePath) { Say "  installed: $claudePath" } else { Say '  it did not end up on the PATH: open a new terminal and check `claude --version`' }
    }
  }
  if (-not $claudePath -and -not $NoStart) { Note 'Claude Code was not found by the installer.' }
}

# ---------------------------------------------------------------- 3. Tailscale: the one step that may need admin
Say "`n3. Tailscale (the phone view and remote desks)"
$tsPath = ''
if ($TailscaleExe) { if (Test-Path -LiteralPath $TailscaleExe) { $tsPath = $TailscaleExe } }
else {
  $c = Get-Command tailscale -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($c) { $tsPath = $c.Source } elseif (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe')) { $tsPath = Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe' }
}
$receipt.tailscale = [ordered]@{ found = $tsPath; elevated = @() }
if ($tsPath) { Say "  found: $tsPath"; Note "Tailscale is installed ($tsPath); the installer did not check that it is signed in or serving: step 4." }
else {
  Say '  Tailscale is not installed. Only the phone view and remote desks need it.'
  Say '  Its installer is MACHINE-WIDE, so it needs administrator rights: Windows shows ONE prompt (UAC) for that step'
  Say '  alone. Nothing else in this setup runs as administrator, and your Claude session will not either.'
  $wg = ''
  if ($WingetExe) { if (Test-Path -LiteralPath $WingetExe) { $wg = $WingetExe } }
  else { $c = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1; if ($c) { $wg = $c.Source } }
  if (-not $wg) {
    Say '  winget is not on this PC, so the installer cannot do it: the Claude session walks you through it.'
    Note 'Tailscale is NOT installed and winget is missing, so the installer could not install it: walk me through it (step 4).'
  } elseif (Ask-YesNo $InstallTailscale '  Install Tailscale now (one administrator prompt)?' $false) {
    $receipt.tailscale.elevated += "winget $WingetTailscale"
    if ($NoStart) { Say "  would run ELEVATED (one UAC prompt, console hidden): winget $WingetTailscale" ; Note 'Tailscale: the installer would have installed it (a test run): check it (step 4).' }
    else {
      try {
        $p = Start-Process -FilePath $wg -ArgumentList $WingetTailscale -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        $receipt.tailscale.exitCode = $p.ExitCode
        if ($p.ExitCode -eq 0) { Say '  Tailscale is installed. Signing in comes in the Claude session (step 4).'; Note 'The installer installed Tailscale (winget, elevated). Not signed in yet: step 4 from "Sign in".' }
        else { Say "  winget ended with $($p.ExitCode): the Claude session walks you through it."; Note "The installer's Tailscale install failed (winget exit $($p.ExitCode)): walk me through it (step 4)." }
      } catch {
        Say '  not installed (the administrator prompt was declined or failed): the Claude session walks you through it.'
        Note 'Tailscale is NOT installed (the administrator prompt was declined): walk me through it, or skip it if I do not want the phone view (step 4).'
      }
    }
  } else { Note 'Tailscale is NOT installed; I chose not to install it from the installer: ask whether I want the phone view (step 4).' }
}

# ---------------------------------------------------------------- 4. set up the rest with Claude Code
Say "`n4. Set up the rest with Claude Code"
Say '  A Claude Code session does what this installer cannot: the account folders, the alias file and your terminal'
Say '  keywords, signing in to each account (you sign in; it opens the window), the companion files, Tailscale.'
Say '  Claude in Chrome lets that session (and later ones) use your browser - Chrome or Edge. It is a browser'
Say "  extension: this installer CANNOT install it; you add it from its Web Store page: $ChromeUrl"
$useChrome = Ask-YesNo $Chrome '  Use Claude in Chrome?' $false
if ($useChrome -and -not $NoStart -and -not $Defaults) {
  if (Ask-YesNo '' '  Open its Web Store page now (click "Add to Chrome", then sign in to it)?' $true) { Start-Process $ChromeUrl }
}
Say '  How should the session ask you things?'
Say '    necessary = only when necessary: it works on its own inside a permission block you can read and edit first'
Say '    as-you-go = as you go: it asks before each step, like a normal session'
$mode = Ask-Choice $AskMode '  Ask mode' @('necessary', 'as-you-go') 'necessary'

# the prompt, filled in
$template = Join-Path $ProgramDir 'setup\claude-setup-prompt.md'
if (-not (Test-Path -LiteralPath $template)) { $template = Join-Path $PSScriptRoot '..\..\setup\claude-setup-prompt.md' }     # run from a checkout
if (-not (Test-Path -LiteralPath $template)) { throw "no setup\claude-setup-prompt.md in $ProgramDir" }
$text = Read-Text $template
$text = [regex]::Replace($text, '\A(\uFEFF)?ASK MODE:[^\r\n]*', $(if ($mode -eq 'necessary') { 'ASK MODE: only when necessary' } else { 'ASK MODE: as you go' }))
if ($mode -eq 'as-you-go') {
  $text = [regex]::Replace($text, '(?s)<!-- BEGIN PERMISSION BLOCK -->.*?<!-- END PERMISSION BLOCK -->', '(No permission block: in "as you go" mode Claude asks you before each step.)')
}
$exe = Join-Path $ProgramDir $DeckEdition.Exe
$ver = ''; if (Test-Path -LiteralPath $exe) { $ver = ('' + (Get-Item -LiteralPath $exe).VersionInfo.FileVersion).Trim() }
$deck = "$($DeckEdition.Shortcut) for Windows $ver (start it: Start menu -> $($DeckEdition.Shortcut), or `"$exe`" --local)".Replace('  ', ' ')
if ($accountList.Count) {
  $rows = @('| label | keyword | config folder | login e-mail |', '|---|---|---|---|')
  foreach ($a in $accountList) { $rows += "| $($a.Label) | $($a.Keyword) | $($a.Folder) | $(if ($a.Email) { $a.Email } else { '(ask me)' }) |" }
  $acctText = $rows -join "`n"
} else { $acctText = '(none named in the installer: ask me for them)' }
$chromeText = 'no'
if ($useChrome) { $chromeText = 'yes - this session is started with --chrome; if /chrome says the extension is not connected, help me add it (step 3)' }
$fill = [ordered]@{
  '{{DECK}}' = $deck; '{{SYSTEM}}' = "Windows $([Environment]::OSVersion.Version)"; '{{STATE_DIR}}' = $StateDir; '{{PROGRAM_DIR}}' = $ProgramDir
  '{{REPORT_FILE}}' = $reportFile; '{{CHROME}}' = $chromeText; '{{ACCOUNTS}}' = $acctText
  '{{INSTALLER_NOTES}}' = (@($notes | ForEach-Object { "- $_" }) -join "`n")
}
foreach ($k in $fill.Keys) { $text = $text.Replace($k, $fill[$k]) }
Write-Text $promptOut $text
Say "  the prompt: $promptOut"
$receipt.askMode = $mode; $receipt.chrome = [bool]$useChrome; $receipt.prompt = $promptOut

# the command line
$q = $promptOut.Replace('`', '``').Replace('$', '`$').Replace('"', '`"')
$cmd = 'claude'
if ($useChrome) { $cmd += ' --chrome' }
$cmd += " `"Read the file '$q' and do what it says. It is the Claude Deck setup prompt.`""
$receipt.commandSuggested = $cmd
Say '  how to run it:'
Say '    terminal = start it now in a new PowerShell window (a normal one, not as administrator)'
Say '    paste    = I open Claude Code myself and paste the line (or the whole file)'
Say '    later    = not now (run setup.ps1 again; the prompt file stays)'
$how = Ask-Choice $Session '  Run the session' @('terminal', 'paste', 'later') 'terminal'
$receipt.session = $how
$receipt.started = $false
function Edit-CommandLine([string]$cmdText) {
  if ($Command) { return $Command }
  if ($NoGui -or $Defaults) {
    Say "    $cmdText"
    if ($Defaults) { return $cmdText }
    $a = ('' + (Read-Host '  Enter = start it as shown; or type the command line to start instead; n = do not start')).Trim()
    if (-not $a) { return $cmdText }
    if ($a -eq 'n') { return '' }
    return $a
  }
  Add-Type -AssemblyName System.Windows.Forms, System.Drawing
  $f = New-Object Windows.Forms.Form
  $f.Text = 'Claude Deck setup - your Claude Code session'; $f.StartPosition = 'CenterScreen'; $f.TopMost = $true
  $f.ClientSize = New-Object Drawing.Size(860, 230); $f.FormBorderStyle = 'FixedDialog'; $f.MaximizeBox = $false; $f.MinimizeBox = $false
  $l = New-Object Windows.Forms.Label
  $l.Text = "This command starts your Claude Code session in a new PowerShell window (a normal one, not as administrator).`r`nChange it if you like, then Start. The prompt it points at (read it first if you like):`r`n$promptOut"
  $l.SetBounds(12, 10, 836, 52)
  $t = New-Object Windows.Forms.TextBox
  $t.Multiline = $true; $t.WordWrap = $true; $t.ScrollBars = 'Vertical'; $t.Font = New-Object Drawing.Font('Consolas', 10)
  $t.Text = $cmdText; $t.SetBounds(12, 68, 836, 110)
  $open = New-Object Windows.Forms.Button; $open.Text = 'Open the prompt'; $open.SetBounds(12, 190, 130, 28)
  $open.Add_Click({ Start-Process notepad.exe -ArgumentList "`"$promptOut`"" })
  $ok = New-Object Windows.Forms.Button; $ok.Text = 'Start'; $ok.DialogResult = 'OK'; $ok.SetBounds(640, 190, 100, 28)
  $no = New-Object Windows.Forms.Button; $no.Text = 'Do not start'; $no.DialogResult = 'Cancel'; $no.SetBounds(748, 190, 100, 28)
  $f.Controls.AddRange([Windows.Forms.Control[]]@($l, $t, $open, $ok, $no)); $f.AcceptButton = $ok; $f.CancelButton = $no
  $r = $f.ShowDialog()
  if ($r -ne 'OK') { return '' }
  return $t.Text.Trim()
}
if ($how -eq 'terminal') {
  $final = Edit-CommandLine $cmd
  if (-not $final) { Say '  not started. The prompt stays where it is; run setup.ps1 again, or open it yourself.' }
  else {
    $receipt.command = $final
    # the window's own script: its title, the home folder, the command echoed, then the command. -EncodedCommand
    # carries it whole (no quoting survives Start-Process's argument joining otherwise)
    $echo = $final.Replace("'", "''")
    $homeQ = $HomeDir.Replace("'", "''")
    $body = "`$host.UI.RawUI.WindowTitle = 'Claude Deck setup'`nSet-Location -LiteralPath '$homeQ'`nWrite-Host ('> ' + '$echo')`n$final`n"
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    $termArgs = "-NoExit -NoLogo -EncodedCommand $b64"
    $receipt.terminal = [ordered]@{ file = 'powershell.exe'; args = $termArgs; workingDirectory = $HomeDir; script = $body }
    if ($NoStart) { Say "  would start, in a NEW NORMAL window: powershell.exe -NoExit -NoLogo -EncodedCommand <the command above>" }
    else {
      # the session is a fresh one: none of a Claude session's markers (core/deck/rules.cpp baseEnv drops the same)
      foreach ($v in @(Get-ChildItem Env: | Where-Object { $_.Name -like 'CLAUDECODE*' -or $_.Name -like 'CLAUDE_*' -or $_.Name -eq 'AI_AGENT' })) { Remove-Item -LiteralPath "Env:$($v.Name)" }
      Start-Process -FilePath powershell.exe -ArgumentList $termArgs -WorkingDirectory $HomeDir
      $receipt.started = $true
      Say '  started in a new window. When it is done it writes its report; run setup.ps1 again (or setup.ps1 -Report) to read it.'
    }
  }
} elseif ($how -eq 'paste') {
  Say '  Open Claude Code yourself - in a normal terminal, not as administrator - and paste this line:'
  Say "    Read the file '$promptOut' and do what it says. It is the Claude Deck setup prompt."
  Say "  or paste the whole file: $promptOut"
  if ($useChrome) { Say '  (start it as  claude --chrome  so it can use Claude in Chrome)' }
  if (-not $NoStart -and -not $Defaults -and (Ask-YesNo '' '  Open the prompt file in Notepad now?' $false)) { Start-Process notepad.exe -ArgumentList "`"$promptOut`"" }
} else { Say "  later: the prompt stays at $promptOut" }

# ---------------------------------------------------------------- 5. the receipt
$receipt.notes = @($notes)
Write-Text $receiptFile ($receipt | ConvertTo-Json -Depth 6)
Say "`nsetup: done ($receiptFile)"
