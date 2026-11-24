#!/usr/bin/env bash
# mac/tools/open-setup-session.sh - the Mac's first run of Claude Deck Native: the critical settings first, then "set
# up the rest with Claude Code" in Terminal. The same shape as windows/package/setup.ps1. NO admin: nothing here asks
# for a password; a step of the session that needs one asks you itself.
# UNRUN ON A MAC (written on Windows). Its plain-bash part runs under Git Bash in windows/package/test-package.ps1
# (--dry-run, a temp HOME); every osascript dialog and the Terminal hand-over are untested. bash 3.2 (macOS) only:
# no associative arrays, no ${x,,}.
#
#   "~/Applications/Claude Deck Native.app/Contents/Resources/setup/open-setup-session.sh" [--dry-run]
#   (make-app.sh puts it there, with claude-setup-prompt.md beside it; from a checkout: mac/tools/open-setup-session.sh)
#
#   1. THE ACCOUNTS, only when the state folder has no settings.json: the ones in ~/.claude-code-account-aliases are
#      offered (label guessed from the folder, ~/.claude-ms -> ms; e-mail from the folder's .claude.json), else asked
#      for in dialogs (login e-mail, short label; folder ~/.claude-<label>, keyword claude<label>). Written as
#      settings.json { "aliases": { "<e-mail>": "<label>", "<keyword>": "<label>" } } - the shape core/deck reads.
#      Not while the deck runs (its running.json names a live process).
#   2. Claude in Chrome: asked (a browser extension cannot be installed by a script: its Web Store page is opened);
#      the ask mode: "only when necessary" (the prompt's permission block) or "as you go".
#   3. the prompt, filled in: <state>/setup/claude-setup-prompt.md (the session writes <state>/setup/setup-report.md).
#   4. the command line in an editable dialog, then Terminal opens and runs it (osascript `do script`: Terminal is the
#      one window meant to be seen - it is your session). Claude Code missing: Anthropic's installer
#      (curl -fsSL https://claude.ai/install.sh | bash, per user) is put in front of it, in the dialog for you to see.
#      Or "paste": the file's path and the line to paste into a session you open yourself.
# Answers without dialogs (a test, or an unattended run): SETUP_USE_FOUND=yes|no  SETUP_ACCOUNTS="e-mail|label;..."
#   SETUP_CHROME=yes|no  SETUP_ASK_MODE=necessary|as-you-go  SETUP_SESSION=terminal|paste|later
#   SETUP_COMMAND=<command line>  SETUP_CLAUDE=<path>|none. With --dry-run every unanswered question takes its default.
# --dry-run: nothing is opened or started; the files are written and the plan printed. STATE_DIR / PROGRAM_DIR override
# the folders (default ~/Library/Application Support/claude-deck-native/local and the folder above this script).
set -euo pipefail
DRY=0; [ "${1:-}" = "--dry-run" ] && DRY=1
if [ "$(uname -s)" != Darwin ] && [ "$DRY" = 0 ]; then echo "open-setup-session: macOS only (or --dry-run)" >&2; exit 1; fi
CHROME_URL='https://chromewebstore.google.com/detail/claude/fcoeoabgfenejglbffodgkkbkcdhcgfn'
HERE="$(cd "$(dirname "$0")" && pwd)"
TEMPLATE="$HERE/claude-setup-prompt.md"
[ -f "$TEMPLATE" ] || TEMPLATE="$HERE/../../setup/claude-setup-prompt.md"        # run from a checkout (mac/tools)
[ -f "$TEMPLATE" ] || { echo "open-setup-session: no claude-setup-prompt.md beside it" >&2; exit 1; }
STATE="${STATE_DIR:-$HOME/Library/Application Support/claude-deck-native/local}"
PROGRAM="${PROGRAM_DIR:-$(cd "$HERE/.." && pwd)}"
SETUP="$STATE/setup"; REPORT="$SETUP/setup-report.md"; PROMPT="$SETUP/claude-setup-prompt.md"
SETTINGS="$STATE/settings.json"
mkdir -p "$SETUP"
NOTES=""
note() { NOTES="$NOTES- $1
"; }
say() { printf '%s\n' "$*"; }

# ---- dialogs (osascript), or the SETUP_* answer, or (--dry-run) the default
dialog_text() {   # question default -> the text typed ("" on Cancel)
  osascript - "$1" "$2" <<'AS' 2>/dev/null || true
on run argv
  set r to display dialog (item 1 of argv) default answer (item 2 of argv) with title "Claude Deck setup"
  return text returned of r
end run
AS
}
dialog_buttons() { # question button... (at most 3; the last is the default) -> the button clicked
  osascript - "$@" <<'AS' 2>/dev/null || true
on run argv
  set bs to items 2 thru -1 of argv
  set r to display dialog (item 1 of argv) buttons bs default button (count of bs) with title "Claude Deck setup"
  return button returned of r
end run
AS
}
answer() {        # given default question button-for-each-choice... ; choices map in order: "value=Button"
  local given="$1" def="$2" q="$3"; shift 3
  if [ -n "$given" ]; then printf '%s' "$given"; return; fi
  if [ "$DRY" = 1 ]; then printf '%s' "$def"; return; fi
  local buttons=() pair
  for pair in "$@"; do buttons+=("${pair#*=}"); done
  local hit; hit="$(dialog_buttons "$q" "${buttons[@]}")"
  for pair in "$@"; do [ "${pair#*=}" = "$hit" ] && { printf '%s' "${pair%%=*}"; return; }; done
  printf '%s' "$def"
}
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
RE_LABEL='^[A-Za-z0-9]{1,8}$'
RE_KEYWORD='^[A-Za-z0-9_]+$'
RE_EMAIL='^[^[:space:]"\\|]+@[^[:space:]"\\|]+$'

say "Claude Deck Native - setup"
say "  state folder  $STATE"
if [ -f "$REPORT" ]; then
  st="$(sed -n 's/^STATUS:[[:space:]]*\([^[:space:]]*\).*/\1/p' "$REPORT" | head -1)"
  say "  an earlier setup session reported: STATUS ${st:-(none)}  ($REPORT)"
  awk '/^## For you to do/{on=1;next} /^## /{on=0} on && NF {print "    " $0}' "$REPORT"
fi

# ---- 1. the accounts (only when the deck has no settings)
ROWS=""; KEYS=","; LABELS=""; LABELLED=0
add_label() { case "$KEYS" in *",$1,"*) return;; esac; KEYS="$KEYS$1,"; LABELS="$LABELS${LABELS:+, }\"$1\": \"$2\""; }
add_account() {   # e-mail label folder keyword
  ROWS="$ROWS| $2 | $4 | $3 | ${1:-(ask me)} |
"
  [ -n "$1" ] && add_label "$1" "$2"
  add_label "$4" "$2"
  LABELLED=$((LABELLED + 1))
}
deck_runs() {
  [ -f "$STATE/running.json" ] || return 1
  local pid; pid="$(sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9]\{1,\}\).*/\1/p' "$STATE/running.json" | head -1)"
  [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}
if [ -f "$SETTINGS" ]; then
  say ""; say "1. Accounts: the deck has settings already ($SETTINGS) - left as they are."
  note "The deck had settings already ($SETTINGS): the installer changed no account label. Check them in step 2d."
else
  say ""; say "1. Accounts: the deck has no settings yet. The accounts and their short labels come first."
  FOUND=()          # "keyword|folder|e-mail|label", one per folder, its shortest keyword
  ALIASES="$HOME/.claude-code-account-aliases"
  if [ -f "$ALIASES" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      line="${line%$'\r'}"
      kw="$(printf '%s' "$line" | sed -n 's/^[[:space:]]*alias[[:space:]]\{1,\}\([A-Za-z0-9_]\{1,\}\)=.*/\1/p')"
      dir="$(printf '%s' "$line" | sed -n 's/.*CLAUDE_CONFIG_DIR="\([^"]*\)".*/\1/p')"
      [ -n "$kw" ] && [ -n "$dir" ] || continue
      case "$dir" in '$HOME'*) dir="$HOME${dir#\$HOME}";; '~'*) dir="$HOME${dir#\~}";; esac
      [ "$dir" = "$HOME/.claude" ] && continue
      i=0; dup=-1
      for f in ${FOUND[@]+"${FOUND[@]}"}; do
        fdir="${f#*|}"; fdir="${fdir%%|*}"; [ "$fdir" = "$dir" ] && dup=$i; i=$((i + 1))
      done
      if [ "$dup" -ge 0 ]; then
        old="${FOUND[$dup]%%|*}"; [ "${#kw}" -lt "${#old}" ] && FOUND[$dup]="$kw|${FOUND[$dup]#*|}"
        continue
      fi
      email="$(grep -o '"emailAddress"[[:space:]]*:[[:space:]]*"[^"]*"' "$dir/.claude.json" 2>/dev/null | head -1 | sed 's/.*"\([^"]*\)"$/\1/' || true)"
      [[ "$email" =~ $RE_EMAIL ]] || email=""
      label="$(basename "$dir")"; label="${label#.claude-}"; label="${label#.claude}"
      [[ "$label" =~ $RE_LABEL ]] || label="${kw#claude}"
      [[ "$label" =~ $RE_LABEL ]] || label=""
      FOUND+=("$kw|$dir|$email|$label")
    done < "$ALIASES"
  fi
  use=no
  if [ "${#FOUND[@]}" -gt 0 ]; then
    list=""
    for f in "${FOUND[@]}"; do IFS='|' read -r kw dir email label <<<"$f"; list="$list${label:-?}  $kw  $dir  ${email:-(not signed in yet)}
"; done
    say "  Found on this Mac:"; printf '%s' "$list" | sed 's/^/  /'
    use="$(answer "${SETUP_USE_FOUND:-}" yes "Use these Claude accounts?
$list" "no=Type them in" "yes=Use these")"
  fi
  if [ "$use" = yes ]; then
    for f in "${FOUND[@]}"; do
      IFS='|' read -r kw dir email label <<<"$f"
      if [ -z "$label" ]; then say "  $kw: no usable label - left out (the session can add it)"; continue; fi
      add_account "$email" "$label" "$dir" "$kw"
    done
  fi
  typed="${SETUP_ACCOUNTS:-}"
  if [ -z "$typed" ] && [ "$DRY" = 0 ] && [ -z "${SETUP_USE_FOUND:-}" ]; then
    while :; do
      e="$(dialog_text "A Claude account's login e-mail (leave it empty when there are no more):" "")"
      [ -n "$e" ] || break
      l="$(dialog_text "A short label for $e - two or three letters shown on the tiles (vv, ms, ts):" "")"
      typed="$typed${typed:+;}$e|$l"
    done
  fi
  if [ -n "$typed" ]; then
    IFS=';' read -r -a specs <<<"$typed"
    for spec in "${specs[@]}"; do
      IFS='|' read -r e l <<<"$spec"
      e="$(lower "$e")"; lc="$(lower "$l")"
      if ! [[ "$l" =~ $RE_LABEL ]] || { [ -n "$e" ] && ! [[ "$e" =~ $RE_EMAIL ]]; }; then say "  not added: '$spec' (a label is 1-8 letters or digits; an e-mail has no spaces or quotes)"; [ "$DRY" = 1 ] && exit 2; continue; fi
      case "$KEYS" in *",claude$lc,"*) say "  not added: '$spec' (the keyword claude$lc is taken)"; [ "$DRY" = 1 ] && exit 2; continue;; esac
      add_account "$e" "$l" "$HOME/.claude-$lc" "claude$lc"
    done
  fi
  if [ "$LABELLED" = 0 ]; then
    say "  No accounts named: the Claude session will ask for them."
    note "No accounts were named in the installer: ask me for them (step 2)."
  elif deck_runs; then
    say "  The deck is running: its settings were NOT written (it would write its own over them)."
    note "The deck was running, so the installer did NOT write the account labels: do step 2d once it is closed."
  else
    printf '{\n  "aliases": { %s }\n}\n' "$LABELS" > "$SETTINGS.writing" && mv "$SETTINGS.writing" "$SETTINGS"
    say "  wrote the labels to $SETTINGS"
    note "The installer wrote the account labels to $SETTINGS (\`aliases\`: e-mail and keyword -> label). Check them in step 2d; do not write them again."
  fi
fi
note "The installer did NOT create account folders, write the alias file or ~/.zshrc, or sign in to any account (a script cannot sign in for you): step 2."
note "The installer did NOT check the companion files (userswitch, the deck-triggers skill, Node.js) or Tailscale: steps 3 and 4. On a Mac there is no administrator terminal: a step that needs the password asks for it."

# ---- 2. Claude in Chrome, the ask mode
say ""; say "2. Claude in Chrome lets the session use your browser. It is a browser extension: this script CANNOT install"
say "   it; you add it from its Web Store page: $CHROME_URL"
CHROME="$(answer "${SETUP_CHROME:-}" no "Use Claude in Chrome? It is a browser extension: this script cannot install it - its Web Store page opens for you to click \"Add to Chrome\"." "no=No" "yes=Yes")"
[ "$CHROME" = yes ] && [ "$DRY" = 0 ] && open "$CHROME_URL"
MODE="$(answer "${SETUP_ASK_MODE:-}" necessary "How should the setup session ask you things?
Only when necessary: it works on its own inside a permission block you can read and edit first.
As you go: it asks before each step." "as-you-go=As you go" "necessary=Only when necessary")"
say "   Chrome: $CHROME   ask mode: $MODE"

# ---- 3. the prompt, filled in
if [ -n "$ROWS" ]; then ACCOUNTS="| label | keyword | config folder | login e-mail |
|---|---|---|---|
${ROWS%
}"; else ACCOUNTS="(none named in the installer: ask me for them)"; fi
CHROME_TEXT="no"; [ "$CHROME" = yes ] && CHROME_TEXT="yes - this session is started with --chrome; if /chrome says the extension is not connected, help me add it (step 3)"
VER="$(defaults read "$PROGRAM/../Info" CFBundleShortVersionString 2>/dev/null || true)"
F_DECK="Claude Deck Native for macOS${VER:+ $VER} (start it: Claude Deck Native in Applications)" \
F_SYSTEM="macOS $(sw_vers -productVersion 2>/dev/null || uname -sr)" F_STATE="$STATE" F_PROGRAM="$PROGRAM" F_REPORT="$REPORT" \
F_CHROME="$CHROME_TEXT" F_ACCOUNTS="$ACCOUNTS" F_NOTES="${NOTES%
}" awk -v mode="$MODE" '
  function rep(s, k, v,   i, out) { out = ""; while ((i = index(s, k)) > 0) { out = out substr(s, 1, i - 1) v; s = substr(s, i + length(k)) } return out s }
  { sub(/\r$/, "") }
  NR == 1 && /^ASK MODE:/ { if (mode == "necessary") print "ASK MODE: only when necessary"; else print "ASK MODE: as you go"; next }
  /<!-- BEGIN PERMISSION BLOCK -->/ && mode != "necessary" { skip = 1; print "(No permission block: in \"as you go\" mode Claude asks you before each step.)"; next }
  skip { if (/<!-- END PERMISSION BLOCK -->/) skip = 0; next }
  { l = $0
    l = rep(l, "{{DECK}}", ENVIRON["F_DECK"]); l = rep(l, "{{SYSTEM}}", ENVIRON["F_SYSTEM"]); l = rep(l, "{{STATE_DIR}}", ENVIRON["F_STATE"])
    l = rep(l, "{{PROGRAM_DIR}}", ENVIRON["F_PROGRAM"]); l = rep(l, "{{REPORT_FILE}}", ENVIRON["F_REPORT"]); l = rep(l, "{{CHROME}}", ENVIRON["F_CHROME"])
    l = rep(l, "{{ACCOUNTS}}", ENVIRON["F_ACCOUNTS"]); l = rep(l, "{{INSTALLER_NOTES}}", ENVIRON["F_NOTES"])
    print l }
' "$TEMPLATE" > "$PROMPT.writing" && mv "$PROMPT.writing" "$PROMPT"
say ""; say "3. the prompt: $PROMPT"

# ---- 4. the session
POINTER="Read the file '$PROMPT' and do what it says. It is the Claude Deck setup prompt."
CLAUDE="${SETUP_CLAUDE:-$(command -v claude || true)}"
[ -z "$CLAUDE" ] && [ -x "$HOME/.local/bin/claude" ] && CLAUDE="$HOME/.local/bin/claude"
[ "$CLAUDE" = none ] && CLAUDE=""
CMD="cd ~ && claude"; [ "$CHROME" = yes ] && CMD="$CMD --chrome"
CMD="$CMD \"$POINTER\""
if [ -z "$CLAUDE" ]; then
  CMD="curl -fsSL https://claude.ai/install.sh | bash && export PATH=\"\$HOME/.local/bin:\$PATH\" && $CMD"
  say "   Claude Code is not installed: Anthropic's per-user installer goes first in the command (no admin)."
fi
SESSION="$(answer "${SETUP_SESSION:-}" terminal "A Claude Code session does what this script cannot: the account folders and sign-ins, the companion files, Tailscale. Open it now in Terminal?" "later=Later" "paste=I paste it myself" "terminal=Open Terminal")"
say "4. the session: $SESSION"
FINAL=""
case "$SESSION" in
  terminal)
    FINAL="${SETUP_COMMAND:-}"
    if [ -z "$FINAL" ]; then
      if [ "$DRY" = 1 ]; then FINAL="$CMD"; else FINAL="$(dialog_text "This command starts your Claude Code session in Terminal. Change it if you like. The prompt it points at: $PROMPT" "$CMD")"; fi
    fi
    if [ -z "$FINAL" ]; then say "   not started: the prompt stays at $PROMPT"
    elif [ "$DRY" = 1 ]; then say "   would open Terminal and run: $FINAL"
    else
      osascript - "$FINAL" <<'AS' >/dev/null
on run argv
  tell application "Terminal"
    activate
    do script (item 1 of argv)
  end tell
end run
AS
      say "   Terminal opened. When the session is done it writes $REPORT; run this again to read it back."
    fi;;
  paste)
    cl="claude"; [ "$CHROME" = yes ] && cl="claude --chrome"
    say "   Open Claude Code yourself ($cl, in a normal Terminal) and paste this line, or the whole file:"
    say "     $POINTER"
    say "   the file: $PROMPT";;
  *) say "   later: the prompt stays at $PROMPT";;
esac
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
printf '{ "at": "%s", "stateDir": "%s", "askMode": "%s", "chrome": %s, "session": "%s", "command": "%s", "dryRun": %s }\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(esc "$STATE")" "$MODE" "$([ "$CHROME" = yes ] && echo true || echo false)" "$SESSION" "$(esc "$FINAL")" \
  "$([ "$DRY" = 1 ] && echo true || echo false)" > "$SETUP/setup-installer.json"
say "setup: done ($SETUP/setup-installer.json)"
