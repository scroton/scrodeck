#!/usr/bin/env bash
# mac/tools/make-app.sh — wrap the SwiftPM build in "Claude Deck Native.app" so it has a bundle identifier (toasts
# need one), a Dock icon and its own name. UNRUN: written on Windows. The Electron deck's counterpart is
# tools/mac-make-app.sh (a launcher bundle); this one holds the real binary.
#
#   cd mac && swift build -c release && tools/make-app.sh            -> ~/Applications/Claude Deck Native.app
#   tools/make-app.sh /Applications
#
# The pages the deck serves to a phone or a remote desk (renderer/, lib/, assets/, node_modules/@xterm) are copied
# into Contents/Resources: that folder is DBOptions.asset_root. bin/userswitch goes to Contents/Resources/bin.
# The first-run setup goes to Contents/Resources/setup: open-setup-session.sh (the critical settings, then a Claude
# Code session in Terminal; no admin) and setup/claude-setup-prompt.md beside it; the deck-triggers skill (a companion
# that session installs into each account) to Contents/Resources/skills/deck-triggers, without its tests.
# NOT signed, NOT notarised: it runs on the machine that built it. Distribution is a later step (mac/README.md).
set -euo pipefail
[ "$(uname -s)" = Darwin ] || { echo "make-app: macOS only" >&2; exit 1; }
MAC="$(cd "$(dirname "$0")/.." && pwd)"; REPO="$(cd "$MAC/.." && pwd)"
DEST="${1:-$HOME/Applications}"
# the names: the edition's (core/deck/edition.hpp: "Claude Deck Native", app.claude-deck.native in this copy)
edname() { sed -n "s/^#define DECK_EDITION_$1 \"\(.*\)\"/\1/p" "$REPO/core/deck/edition.hpp" | head -1; }
APPNAME="$(edname MAC_APP)"; BUNDLE_ID="$(edname MAC_BUNDLE_ID)"; STATE_NAME="$(edname STATE_DIR)"; PRODUCT="$(edname PRODUCT)"
[ -n "$APPNAME" ] && [ -n "$BUNDLE_ID" ] && [ -n "$STATE_NAME" ] || { echo "make-app: no edition names in core/deck/edition.hpp" >&2; exit 1; }
APP="$DEST/$APPNAME.app"
BIN="$MAC/.build/release/ClaudeDeck"
[ -x "$BIN" ] || { echo "make-app: build first:  (cd mac && swift build -c release)" >&2; exit 1; }
VERSION="$(sed -n 's/^#define CLAUDE_DECK_NATIVE_VERSION "\(.*\)"/\1/p' "$REPO/core/deck/version.hpp" | head -1)"
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/bin"
cp "$BIN" "$APP/Contents/MacOS/ClaudeDeck"
for d in renderer lib assets; do [ -d "$REPO/$d" ] && cp -R "$REPO/$d" "$APP/Contents/Resources/$d"; done
if [ -d "$REPO/node_modules/@xterm" ]; then mkdir -p "$APP/Contents/Resources/node_modules"; cp -R "$REPO/node_modules/@xterm" "$APP/Contents/Resources/node_modules/@xterm"; fi
[ -f "$REPO/bin/userswitch" ] && cp "$REPO/bin/userswitch" "$APP/Contents/Resources/bin/userswitch"
mkdir -p "$APP/Contents/Resources/setup" "$APP/Contents/Resources/skills/deck-triggers/scripts"
cp "$REPO/setup/claude-setup-prompt.md" "$APP/Contents/Resources/setup/claude-setup-prompt.md"
# (the edition's names in the copy: identity in this one)
sed -e "s/claude-deck-native/$STATE_NAME/g" -e "s/Claude Deck Native/$APPNAME/g" "$MAC/tools/open-setup-session.sh" > "$APP/Contents/Resources/setup/open-setup-session.sh"
chmod +x "$APP/Contents/Resources/setup/open-setup-session.sh"
cp "$REPO/skills/deck-triggers/SKILL.md" "$APP/Contents/Resources/skills/deck-triggers/SKILL.md"
cp "$REPO/skills/deck-triggers/scripts/deck-trigger.js" "$REPO/skills/deck-triggers/scripts/README.md" "$APP/Contents/Resources/skills/deck-triggers/scripts/"
if [ -f "$REPO/assets/icon.png" ]; then
  ICONSET="$(mktemp -d)/icon.iconset"; mkdir -p "$ICONSET"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$REPO/assets/icon.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$REPO/assets/icon.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/icon.icns"
fi
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$APPNAME</string>
  <key>CFBundleDisplayName</key><string>$APPNAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>ClaudeDeck</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION:-0.1.0}</string>
  <key>CFBundleVersion</key><string>${VERSION:-0.1.0}</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
</dict></plist>
PLIST
echo "built $APP"
echo "first run (accounts, then the rest with Claude Code):  \"$APP/Contents/Resources/setup/open-setup-session.sh\""
