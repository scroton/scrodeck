// core/deck/edition.hpp — WHICH BUILD THIS IS: the public build.
//
// Every behaviour the public build does differently from the maintainer's own copy asks edition::kPublic; every
// default that would name somebody's own folders is here. (The maintainer's copy differs from this file only.)
//
// What kPublic changes:
//   - usage: the live usage bars read your Claude Code sign-in to fetch your usage from Anthropic's servers - not an
//     official API. They poll only when settings.json `usageLive` is true (Settings: Live usage bars): an opt-in,
//     OFF by default.
//   - there is no "move every session" and no automatic "continue" after a usage notice; a single tile can still be
//     moved to another account by hand, from its menu.
//   - auditors: each file an auditor session writes waits on its child's tile for your one-click Send, unless
//     `auditorAutoSend` is true - you delegate the writing, you approve each prompt.
//   - the ask-page skill's settings are looked for in ~/.claude, then every ~/.claude-* folder by name; ask pages
//     are also looked for in ~/Downloads.
//   - the names below: scroDeck.exe, %APPDATA%\scrodeck (~/Library/Application Support/scrodeck on a Mac), the
//     toasts' AppUserModelID scroton.scrodeck, the Start-menu shortcut "scroDeck".
//   - the self-update reads the releases of scroton/scrodeck and trusts ONLY the key below (KEYS: its public part).
//   - the cloud session list (it reads the same Claude Code sign-in): only when settings.json `cloudLive` is true
//     (Settings: Cloud session list), OFF by default.
//   - the pages the deck serves are told { name, moveAll: false } (meta.edition): no move-all button there.
// Header-only, no OS header.
#pragma once

// THE NAMES: plain #defines, so a resource script
// and the PowerShell scripts (windows/edition.ps1 reads these lines) take them from here too; windres defines
// RC_INVOKED and must not see the C++ below.
// the name the user sees: window titles, notes, toasts, the deck's own messages, the version block
#define DECK_EDITION_PRODUCT "scroDeck"
// the name in a sentence ("PAUSE: <this> is about to restart")
#define DECK_EDITION_THE "scroDeck"
// the exe the build writes (windows/build.ps1), the package ships and the updater swaps
#define DECK_EDITION_EXE "scroDeck.exe"
// the state folder: %APPDATA%\<this> (Windows), ~/Library/Application Support/<this> (Mac), $XDG_CONFIG_HOME/<this>
#define DECK_EDITION_STATE_DIR "scrodeck"
// one deck at a time: the marker + shared layout folder, beside the state folder (onedeck.hpp)
#define DECK_EDITION_SHARED_DIR "scrodeck-shared"
// the toasts' AppUserModelID (and the Start-menu shortcut's, which Windows needs to show them)
#define DECK_EDITION_APP_ID "scroton.scrodeck"
// a remote window's own taskbar identity
#define DECK_EDITION_REMOTE_APP_ID "scroton.scrodeck.remote"
// the Start-menu shortcut (<this>.lnk) and the taskbar's name for a local window
#define DECK_EDITION_SHORTCUT "scroDeck"
// the taskbar's name for a remote window
#define DECK_EDITION_REMOTE_NAME "scroDeck Remote"
// the exe's FileDescription and the Start-menu shortcut's description (before ": all Claude Code sessions ...")
#define DECK_EDITION_DESCRIPTION "scroDeck"
// the install folder under %LOCALAPPDATA%\Programs, and the package's name (<this>-<version>)
#define DECK_EDITION_PROGRAMS_DIR "scroDeck"
// the Mac app: its bundle (<this>.app, the menu bar name) and its bundle identifier (mac/tools/make-app.sh)
#define DECK_EDITION_MAC_APP "scroDeck"
#define DECK_EDITION_MAC_BUNDLE_ID "io.github.scroton.scrodeck"
// the GitHub repo whose releases the updater reads (windows/src/update.hpp)
#define DECK_EDITION_RELEASES_REPO "scroton/scrodeck"
// the ONLY key fingerprint the updater trusts a release's SHA256SUMS.asc from (KEYS holds its public part)
#define DECK_EDITION_RELEASE_KEY "29B24E6B7B4292A358EF2530F9EB954F459F2B02"

#ifndef RC_INVOKED

namespace deck { namespace edition {

constexpr bool kPublic = true;

// The account folders (under the home folder) whose skills/ask-page/settings.json is read, in this order; the first
// that parses wins. kAskScanAccounts: after these, every other `.claude-*` folder in the home folder, sorted by name.
constexpr const char* kAskSkillHomes[] = { ".claude" };
constexpr bool kAskScanAccounts = true;
// The Downloads folders an ask page may also be found in, when neither ASK_PAGE_DOWNLOADS nor the skill's settings
// name any.
constexpr const char* kAskDownloads[] = { "~/Downloads" };

// the names above, as C++ (Windows takes the wide forms with L"" DECK_EDITION_...)
constexpr const char* kProductName = DECK_EDITION_PRODUCT;
constexpr const char* kExeName = DECK_EDITION_EXE;
constexpr const char* kStateDirName = DECK_EDITION_STATE_DIR;
constexpr const char* kSharedDirName = DECK_EDITION_SHARED_DIR;
constexpr const char* kReleasesRepo = DECK_EDITION_RELEASES_REPO;
constexpr const char* kReleaseKey = DECK_EDITION_RELEASE_KEY;

}}  // namespace deck::edition
#endif
