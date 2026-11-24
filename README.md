# scroDeck

**A vibe-coded deck for Claude Code.** One window holds many terminal sessions - Claude Code sessions and plain
shells - as tiles in a sidebar and panes in a grid: see which session is working and which one waits for you;
resume, arrange and group them; drive the same deck from a second PC or a phone.

> scroDeck is an independent project. It is **not made by, endorsed by or affiliated with Anthropic.** "Claude" and
> "Claude Code" are Anthropic's names for their products. scroDeck runs the `claude` program you installed, under
> your own sign-in, and needs it to do anything useful with Claude.

This repository is the **native** deck: a C++ core shared by a Win32 / Direct2D shell (Windows) and a Swift / AppKit
shell (macOS, in progress). Expect rough edges.

| Part | What | State |
|---|---|---|
| `core/` | the deck itself: the terminal engine (a port of xterm.js, tested cell by cell against headless xterm), session rules, layout, the phone / remote-desk server - pure C++17, no OS headers | tested on Windows and macOS |
| `windows/` | `scroDeck.exe`: the Windows shell (Win32, Direct2D, DirectWrite, ConPTY), local or remote mode | in daily use |
| `mac/` | the macOS shell (Swift, AppKit) over the same core | builds in CI; not finished |
| `renderer/`, `lib/layout.js` | the web pages the deck serves to a phone and to a browser desk | |
| `assets/` | the icon and the bundled console fonts (OFL) | |

## Needs

- **Claude Code**, installed and signed in (`claude` on the PATH, or `%USERPROFILE%\.local\bin\claude.exe`).
- Windows 10 / 11 (x64) for the Windows shell; the WebView2 runtime (part of Windows 11) for the ask and cloud panes.
- To build: the WinLibs MinGW-w64 g++ on Windows (`winget install BrechtSanders.WinLibs.POSIX.UCRT`); Xcode's clang
  and Swift on a Mac. Node.js only to fetch the two xterm.js files the served pages use (`npm install`).

## Build and test (Windows)

```
npm install                                         # @xterm/xterm + @xterm/addon-fit: what the served pages load
powershell -File windows/build.ps1                  # -> windows/build/scroDeck.exe
powershell -File core/test/build.ps1                # the model: layout, hibernation, prompts, triggers
powershell -File core/test/vt/build.ps1             # the terminal engine against xterm.js's own grids
powershell -File core/test/proto/build.ps1          # the frame codec
powershell -File core/test/input/build.ps1          # keys and mouse
powershell -File core/test/session/build.ps1        # session rules
powershell -File core/test/deck/build.ps1           # the deck on fake seams: Windows paths, then --posix
powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/shell.ps1    # the exe itself, headless (--shot)
```

On a Mac the core suites run with clang++: `.github/workflows/mac.yml` is the recipe.

## Using it

`scroDeck.exe --local` makes this PC's deck; with no argument the first run asks "this PC, or another deck's
link". Its state lives in `%APPDATA%\scrodeck\` and nowhere else (on a Mac `~/Library/Application Support/scrodeck`). `windows/README.md` is the long form.

**Accounts.** The deck finds your Claude Code accounts from an alias file (`~/.claude-code-account-aliases`, lines
like `alias claudework="CLAUDE_CONFIG_DIR=~/.claude-work claude"`); with none, your one default account. Give each
account a short label in Settings.

**The remote desk and the phone** are off until you switch them on (Settings, Phone). They serve your terminals over
the network, guarded by a token in the link. Use them over **Tailscale only** (the default, "Listen on: Tailscale");
never expose the port to the internet and never put it behind `tailscale funnel`.

## Off by default, and why

| Setting | Default | What it does |
|---|---|---|
| Live usage bars (Settings, Usage) | **off** | reads your Claude Code sign-in to fetch your usage from Anthropic; **not an official API**. Off, the deck reads no token and the usage bars stay empty |
| Cloud session list (Settings, Usage) | **off** | reads your Claude Code sign-in to list your cloud sessions; **not an official API**. Off, the deck reads no token and lists none |
| Auditor auto-send | **off** | an auditor session writes prompts for its child sessions; each waits on the child's tile for your one-click Send. You delegate the writing and approve every prompt |
| Phone / remote-desk server | off | see above |

There is no "move every session to another account" in this build (nor its button on the browser desk or the phone), and the deck never types into a session on its
own after a usage-limit notice. One session can be moved to another of your accounts by hand, from its tile menu.

## Updates

The exe checks this repository's GitHub releases (palette: Check for updates). A release is taken only when its
`SHA256SUMS.asc` is signed by the key in `KEYS` (fingerprint `29B24E6B7B4292A358EF2530F9EB954F459F2B02`,
"scroDeck releases"), and the downloaded exe's SHA-256 is the one that list names. Check a download yourself:

```
gpg --import KEYS
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum -c SHA256SUMS
```

## Contributing, security, licence

`CONTRIBUTING.md`, `SECURITY.md`. MIT (`LICENSE`); third-party parts keep their own licences, listed there.
