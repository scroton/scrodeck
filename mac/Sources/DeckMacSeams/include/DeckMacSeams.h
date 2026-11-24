/* mac/Sources/DeckMacSeams/include/DeckMacSeams.h — the macOS side of the deck, as Swift sees it: one call that
 * makes a DBHost (every seam of core/deck/seams.hpp that is an OS call), and the few process-level things the
 * shell's Lifecycle needs. C99; the Swift module is this header only (module.modulemap).
 *
 * UNBUILT, UNTESTED — written on Windows, never compiled. Windows counterpart: windows/host/winseams.hpp,
 * conpty.hpp, listener.hpp, winhttp.hpp, statefile.hpp (the seams) and windows/src/lifecycle.hpp (the process).
 * JS mirrored: lib/platform.js (the OS half), node-pty, lib/statefile.js, lib/phone.js (tailscale).
 */
#ifndef CLAUDE_DECK_MAC_SEAMS_H
#define CLAUDE_DECK_MAC_SEAMS_H

#include "DeckBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DMSOptions {
  /* 1 = the children's environment is the LOGIN SHELL's (`$SHELL -lc env`, read once, 5 s at most): an app
   * started from Finder or the Dock has launchd's bare PATH, with no Homebrew and no ~/.local/bin, so `claude`
   * and the user's tools would not be found (tools/mac-make-app.sh starts the Electron deck through the login
   * shell for the same reason). 0 = this process's own environment (started from a terminal; tests). */
  int login_environment;
  /* 1 = make the Http (URLSession) and Credentials (Keychain, read-only) seams. They are only USED when
   * DBOptions.usage_polling / cloud_polling say so — reading the accounts' sign-in tokens is the owner's to allow.
   * With 0 the Keychain is never touched at all. */
  int http_and_credentials;
  /* 1 = make the Listener + Net seams (the phone / remote-desk server). 0 = this deck serves nobody. */
  int server;
  /* tests: when > 0 the listener refuses to bind anything but 127.0.0.1 on [test_port_low, test_port_high], and
   * Net reports no addresses and never runs `tailscale` (windows/src/deckhost.hpp TestListener / TestNet). */
  int test_port_low, test_port_high;
} DMSOptions;

/* Never NULL. Give it to db_deck_create; set its wake with db_host_set_wake; destroy it (db_host_destroy) after
 * the deck. Also ignores SIGPIPE for the process (a pty or socket whose peer went away must not kill the deck). */
DBHost* dms_host_create(const DMSOptions* options);

/* ~/Library/Application Support/claude-deck-native/local — the native deck's own state folder, NEVER the Electron
 * deck's (~/Library/Application Support/claude-deck). Valid until the next call. */
const char* dms_default_state_dir(void);
/* ~/Library/Application Support/claude-deck-native — config.json, the instance lock, relaunch.json */
const char* dms_config_dir(void);

/* One deck per config folder (windows/src/lifecycle.hpp's single-instance lock; app.requestSingleInstanceLock).
 * Takes an exclusive flock on <config_dir>/instance.lock and keeps it until the process ends.
 * 1 = this is the only one; 0 = another deck holds it (the caller activates that one and exits). */
int dms_single_instance_lock(const char* config_dir);

/* main.js armExitWatchdog: `ms` from now, SIGKILL every child process group this process started and _exit(0),
 * whatever the main thread is stuck in. Arms once. */
void dms_arm_exit_watchdog(double ms);

/* Lifecycle::restart: start `file` with `args` (NULL-terminated) detached from this process (its own session), with
 * --wait-pid <this pid> appended when `append_wait_pid`; 1 = started. The caller then quits. */
int dms_relaunch(const char* file, const char* const* args, int append_wait_pid);
/* A relaunched deck waits for the old one to be gone (up to `timeout_ms`) before it takes the lock. */
void dms_wait_for_pid_exit(int pid, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
