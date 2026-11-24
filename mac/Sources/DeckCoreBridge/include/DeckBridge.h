/* mac/Sources/DeckCoreBridge/include/DeckBridge.h — the deck's core as a C ABI, for a shell written in Swift.
 *
 * WHAT THIS IS. core/deck (the deck), core/vt (the terminal engine), core/proto (the frames) and core/model
 * (the layout rules) are C++17 with std::function, nlohmann::json and templates in their public headers — all
 * things Swift's C++ interop imports badly or not at all. This header is the whole surface the Swift shell sees
 * instead: opaque handles, plain value structs, UTF-8 bytes with lengths, C function pointers with a context
 * pointer. Every constant is a #define of ONE plain literal (Swift imports those as Int32 / UInt32 constants; it
 * does not import macros that are expressions). It is valid C99 and valid C++, so it imports the same with or without `-cxx-interoperability-mode`.
 * No exception crosses it (DeckBridge.cpp catches at every entry).
 *
 * THE WINDOWS COUNTERPART is windows/src/deckhost.hpp + windows/host/localdeck.{hpp,cpp} (the same wiring, written
 * against HWNDs); the JS it stands for is main.js (the deck), preload.js / renderer/desk/bridge.js (a window's
 * connection) and renderer/js/20-terminals.js (xterm's place).
 *
 * BUILT AND TESTED ON WINDOWS with g++ against the real core: mac/Tests/build.ps1 (bridge_test.cpp, fake seams).
 * Never compiled with clang or imported by a Swift compiler: see mac/README.md "First-build checklist".
 *
 * THREADS. One thread owns everything here (the main thread). The only thread-safe thing is the host's inbox,
 * which pty and socket threads push to (DeckBridgeHost.hpp); its wake callback is the one function that may be
 * called on another thread. Every DBSink / DBShellCallbacks function is called on the owning thread, and never
 * from inside the call that caused it unless stated: frames for a window are queued and delivered when the
 * outermost bridge call returns, so a reply cannot re-enter a window from inside its own send
 * (windows/src/deckhost.hpp posts them for the same reason).
 *
 * STRINGS. `const char*` in = NUL-terminated UTF-8 (NULL reads as ""). A `const char*` OUT stays valid until the
 * next bridge call on the same handle (or, for the handle-less helpers, the next call of the same function on the
 * same thread): copy it at once (`String(cString:)`).
 */
#ifndef CLAUDE_DECK_BRIDGE_H
#define CLAUDE_DECK_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DBHost DBHost;       /* the OS side: seams + the inbox (DeckBridgeHost.hpp makes one; DeckMacSeams on a Mac) */
typedef struct DBDeck DBDeck;       /* the in-process deck: core::Deck + core::Server over a DBHost */
typedef struct DBConn DBConn;       /* one window's connection to a deck: local (in-process) or remote (a websocket's text) */
typedef struct DBTerm DBTerm;       /* one terminal screen (core/vt) */
typedef struct DBLayout DBLayout;   /* one window's grid layout (core/model/layout) */
typedef struct DBMouse DBMouse;     /* one terminal's mouse-report state (xterm's CoreMouseService) */

int db_abi_version(void);           /* bumped when a struct below changes shape: 1 */
const char* db_core_version(void);  /* core/deck/version.hpp kNativeDeckVersion */

/* ------------------------------------------------------------------ frames out (core -> shell) */
#define DB_FRAME_UNKNOWN 0
#define DB_FRAME_INIT 1    /* json = the whole hello { meta, home, platform, build, assets } (a window ignores it) */
#define DB_FRAME_META 2    /* json = the whole frame { meta, build, assets } */
#define DB_FRAME_DATA 3    /* id, bytes / bytes_len (pty bytes, valid UTF-8), replay */
#define DB_FRAME_EXIT 4    /* id, exit_code */
#define DB_FRAME_RESET 5   /* id */
#define DB_FRAME_OK 6      /* rid, json = the result ("null" when none) */
#define DB_FRAME_ERR 7     /* rid, name = the error message */
#define DB_FRAME_EVENT 8   /* name = the channel (layout:evict, tiles:activate ...), json = the payload */
#define DB_FRAME_OPEN 9    /* LOCAL only: the connection is up - call deskInit now (what a socket's `open` is) */
typedef struct DBFrame {
  int kind;
  int rid;
  int exit_code;
  int replay;                 /* 1 = the scrollback window sent on attach */
  const char* id;             /* tile id, "" when the frame has none */
  const char* name;           /* event name / error message, "" otherwise */
  const char* json;           /* UTF-8 JSON text, never NULL ("null") */
  size_t json_len;
  const char* bytes;          /* DATA only */
  size_t bytes_len;
} DBFrame;
/* A window's receiver. `frame` and everything it points to are valid only during the call. */
typedef struct DBSink { void* ctx; void (*frame)(void* ctx, const DBFrame* frame); } DBSink;

/* ------------------------------------------------------------------ the shell's own seams (Notifier, Lifecycle) */
typedef struct DBToast { const char* title; const char* body; int silent; const char* kind; const char* tile_id; const char* session_id; } DBToast;
/* Any function may be NULL (the core then gets the seam's default: nothing happens / "cannot"). The int results
 * are 1 = done, 0 = cannot. Called on the owning thread, possibly from inside db_deck_pump or a db_conn_* call. */
typedef struct DBShellCallbacks {
  void* ctx;
  void (*toast)(void* ctx, const DBToast* toast);                 /* core::Notifier::toast */
  void (*flash)(void* ctx, int window_index);                     /* core::Notifier::flash — dock.bounce on a Mac */
  int (*restart)(void* ctx, const char* why);                     /* core::Lifecycle::restart */
  const char* (*fingerprint_json)(void* ctx);                     /* Lifecycle::fingerprint: {"name":"hash"} or NULL */
  void (*arm_exit_watchdog)(void* ctx, double ms);
  int (*open_external)(void* ctx, const char* url);
  int (*open_path)(void* ctx, const char* path);
  int (*show_in_folder)(void* ctx, const char* path);
  /* A new window for `client`, which the core already registered: make one and call db_deck_adopt_window. */
  int (*open_window)(void* ctx, int client, const char* bounds_json, int inactive);
} DBShellCallbacks;

/* ------------------------------------------------------------------ the host (the OS side) */
/* A DBHost comes from a seam library (dms_host_create in DeckMacSeams.h; a test builds its own). `wake` is called
 * on ANY thread when pty / socket work is waiting, at most once per pump: hop to the owning thread and call
 * db_deck_pump. Destroy the host AFTER the deck. */
void db_host_set_wake(DBHost* host, void (*wake)(void* ctx), void* ctx);
void db_host_destroy(DBHost* host);

/* ------------------------------------------------------------------ the deck */
/* -1 on a tri-state = "the core's own default". Strings may be NULL / "" = resolved as lib/platform.js does. */
typedef struct DBOptions {
  const char* state_dir;        /* REQUIRED: tiles.json, settings.json … (never the Electron deck's folder) */
  const char* home;             /* "" = the host Env's home() */
  const char* alias_file;       /* "" = platform.aliasFiles().primary */
  const char* claude_exe;       /* "" = CLAUDE_DECK_CLAUDE, then platform.claudeExe() */
  const char* asset_root;       /* the folder holding renderer/, lib/, assets/ (the served pages); "" = none: pages 404 */
  const char* resources_dir;    /* the app bundle's Resources ("" = none): bin/userswitch is looked for there */
  const char* version;          /* "" = core/deck/version.hpp */
  const char* ask_state_dir;    /* names the ask folder outright (a test's temp folder) and turns the feed on */
  const char* ask_downloads;    /* with ask_state_dir: the Done folders, '\n'-separated */
  int serve_port;               /* > 0: `--serve <port>` (127.0.0.1:<port> for this run, whatever the settings say) */
  int allow_claude;             /* 0 / 1 — THE SWITCH (core/deck/deck.hpp). The shell decides; default it to 0 */
  int persist_shells;
  int allow_account_edit;
  int usage_polling;            /* needs Http + Credentials in the host; the owner's to allow */
  int cloud_polling;
  int allow_cloud_create;
  int asks;                     /* 1 = the ask feed on the folder lib/asks.js locations() resolves (the owner's real one) */
  int desk_open_inbox;          /* -1 = on */
  int remote_desk_is_a_window;  /* -1 = the core's default (off: the remote-desk rule) */
  int driver_claim;             /* -1 = the core's default (off: a proposal) */
} DBOptions;
void db_options_init(DBOptions* o);    /* every string NULL, every switch 0, every tri-state -1 */

/* NULL on failure with a message in `err`. The host must outlive the deck; the deck does not own it. */
DBDeck* db_deck_create(const DBOptions* options, DBHost* host, const DBShellCallbacks* shell, char* err, size_t err_cap);
void db_deck_start(DBDeck* deck);                 /* load settings + state, restore the saved tiles (main.js whenReady) */
/* Drain the host's inbox into the core (pty bytes and exits, socket events, finished HTTP), run the timers that
 * are due, deliver the frames. Call it on the inbox's wake AND at least every db_deck_next_due_ms() ms. */
void db_deck_pump(DBDeck* deck);
int db_deck_next_due_ms(const DBDeck* deck);      /* 0 = now; never more than 2000 */
void db_deck_shutdown(DBDeck* deck);              /* every process killed, the server stopped; the deck is inert after */
void db_deck_destroy(DBDeck* deck);               /* shutdown + free; its connections are freed with it */

/* A window of this deck (Deck::addWindow): the n-th one made takes the n-th saved window's bounds and layout.
 * `bounds_json` = {"x":..,"y":..,"width":..,"height":..} or NULL. The sink gets DB_FRAME_OPEN first. */
DBConn* db_deck_add_window(DBDeck* deck, const char* bounds_json, const DBSink* sink);
/* F5 `window:open`: a new window holding `tile_id` alone. NULL when there is no such tile. */
DBConn* db_deck_open_window(DBDeck* deck, const char* tile_id, const char* bounds_json, const DBSink* sink);
/* The window for a client the core registered itself (DBShellCallbacks.open_window). */
DBConn* db_deck_adopt_window(DBDeck* deck, int client, const DBSink* sink);
/* A clicked toast (main.js note.on('click')): the client id of the window to bring up, 0 = none. That window
 * is sent `tiles:activate` / `asks:open`. Raising it is the shell's. */
int db_deck_toast_clicked(DBDeck* deck, const DBToast* toast);
const char* db_deck_phone_status_json(DBDeck* deck);    /* main.js phoneStatus() + assets / assetRoot; "null" with no server */
const char* db_deck_saved_windows_json(DBDeck* deck);   /* [{ bounds, layout }] the state file holds (after start) */
const char* db_deck_settings_json(DBDeck* deck);
const char* db_deck_relaunch_spec_json(DBDeck* deck);   /* { command, args } or "null" (relaunch.json) */
const char* db_deck_state_dir(DBDeck* deck);
int db_deck_live_count(DBDeck* deck);                   /* live processes the deck started */
/* Where an OLDER ask page's own Done saves its download (main.js hookAskDownloads: the first of these folders that
 * exists): the ask feed's Done folders as a JSON array of paths, in order; "[]" when the feed is off. */
const char* db_deck_ask_downloads_json(DBDeck* deck);
int db_deck_allow_claude(const DBDeck* deck);
/* tests / diagnostics: a tile's pty size as "cols x rows" would be — cols in *cols, rows in *rows (0, 0 = none) */
void db_deck_pty_size(DBDeck* deck, const char* tile_id, int* cols, int* rows);

/* ------------------------------------------------------------------ a connection (one window of the shell) */
/* REMOTE: this window is a desk of a deck on another machine. `send_text` puts one websocket text message on the
 * wire; the shell hands every received text message to db_conn_receive_text. Frames are delivered to the sink
 * from inside db_conn_receive_text (the shell already hopped to its own thread to call it). */
DBConn* db_conn_remote(const DBSink* sink, void (*send_text)(void* ctx, const char* text, size_t n), void* send_ctx);
void db_conn_receive_text(DBConn* conn, const char* text, size_t n);
/* The window closed (local: its tiles drop out of that layout and keep running) / the socket is done. Frees it. */
void db_conn_close(DBConn* conn);
int db_conn_is_local(const DBConn* conn);
int db_conn_client(const DBConn* conn);                 /* local: the core's client id; remote: 0 */
int db_conn_window_index(const DBConn* conn);           /* local: the window's index (0 = the first); remote: -1 */
/* LOCAL only (no-ops on a remote): the shell's own facts about the window */
void db_conn_window_state(DBConn* conn, int visible, int focused);
void db_conn_window_bounds(DBConn* conn, const char* bounds_json);
void db_conn_window_key(DBConn* conn);                  /* a key went down in this window: the user is at this machine */

/* ---- frames in (shell -> core): the one place a native shell builds them (core/proto/frames.hpp) */
void db_conn_call(DBConn* conn, int rid, const char* fn, const char* args_json);   /* args_json: a JSON array, NULL = [] ; rid 0 = no reply */
void db_conn_attach(DBConn* conn, const char* id, int since);
void db_conn_detach(DBConn* conn, const char* id);       /* NULL = everything */
void db_conn_input(DBConn* conn, const char* id, const char* bytes, size_t n);     /* a real keystroke (kind 'key') */
/* The one synthesised thing a remote may send: exactly one SGR wheel report. 0 = refused (not that shape). */
int db_conn_wheel(DBConn* conn, const char* id, const char* report, size_t n);
void db_conn_resize(DBConn* conn, const char* id, int cols, int rows);
void db_conn_shown(DBConn* conn, const char* const* ids, int count, int hidden);
void db_conn_layout(DBConn* conn, const DBLayout* layout);
void db_conn_leaving(DBConn* conn, const char* id, int on);
/* R8: is `bytes` nothing but mouse / focus reports a terminal emulator makes by itself (never sent by a remote) */
int db_frame_synthesised(const char* bytes, size_t n);

/* ------------------------------------------------------------------ a terminal (core/vt) */
#define DB_COLOR_DEFAULT 0xFF000000u    /* the theme's foreground / background */
#define DB_COLOR_PALETTE 0x01000000u    /* | index 0-255 */
/* otherwise 0x00RRGGBB */
/* DBCell.flags - core/vt/terminal.hpp CellFlags, bit for bit (DeckBridge.cpp static_asserts it) */
#define DB_BOLD 1
#define DB_DIM 2
#define DB_ITALIC 4
#define DB_UNDERLINE 8
#define DB_BLINK 16
#define DB_INVERSE 32
#define DB_INVISIBLE 64
#define DB_STRIKE 128
#define DB_OVERLINE 256
#define DB_UNDERLINE_STYLE_SHIFT 9      /* bits 9-11 when DB_UNDERLINE is set: 1 single 2 double 3 curly 4 dotted 5 dashed */
#define DB_UNDERLINE_STYLE_MASK 0x0E00
#define DB_PROTECTED 0x1000
#define DB_HYPERLINK 0x2000
typedef struct DBCell {
  uint32_t ch;        /* base code point; 0 = empty */
  uint32_t fg, bg;    /* DB_COLOR_* */
  uint16_t flags;
  uint8_t width;      /* 1, 2, or 0 = the right half of a wide character */
  uint8_t cluster;    /* 1 = more code points follow the base: db_term_cell_cluster */
} DBCell;
typedef struct DBTermState {
  int cols, rows, scrollback;                 /* scrollback = lines above the viewport (normal buffer) */
  int cursor_x, cursor_y, cursor_visible, cursor_shape, cursor_blink;   /* shape 0 default 1 block 2 underline 3 bar; blink -1 default */
  int alt_screen, app_cursor_keys, app_keypad, bracketed_paste;
  int mouse_mode, mouse_encoding, focus_reporting, synchronized_output;
} DBTermState;

DBTerm* db_term_create(int cols, int rows, int scrollback);   /* the deck passes 10000 */
void db_term_destroy(DBTerm* term);
void db_term_feed(DBTerm* term, const char* bytes, size_t n);
void db_term_resize(DBTerm* term, int cols, int rows);
void db_term_reset(DBTerm* term);
void db_term_state(const DBTerm* term, DBTermState* out);
/* 1 with the inclusive viewport row span that may have changed since the last take; 0 = nothing did */
int db_term_take_damage(DBTerm* term, int* first_row, int* last_row);
int db_term_take_bell(DBTerm* term);
const char* db_term_title(DBTerm* term);
/* bytes the terminal answers with (DSR, DA …). LOCAL mode writes them to the tile as input; a REMOTE drops them (R8) */
const char* db_term_take_replies(DBTerm* term, size_t* n);
/* One line's cells. `abs_line`: 0 = the oldest scrollback line, `scrollback` = the viewport's top row.
 * Returns the cells written (min(cols, cap)), 0 when the line does not exist. `wrapped`: it continues the line above. */
int db_term_read_line(const DBTerm* term, int abs_line, DBCell* out, int cap, int* wrapped);
/* The full code point sequence of a cell with `cluster` set (base + combining marks); the count written */
int db_term_cell_cluster(const DBTerm* term, int abs_line, int col, uint32_t* out, int cap);
/* The text between two cells, inclusive, as a selection copies it: trailing blanks of a line dropped, a wrapped
 * line joined to the one above, '\n' between the rest. (x0,y0) <= (x1,y1), y in abs lines. */
const char* db_term_text(DBTerm* term, int x0, int y0, int x1, int y1, size_t* n);
void db_term_set_theme_color(DBTerm* term, int index, uint32_t rgb);   /* 0-255 palette, 256 fg, 257 bg, 258 cursor */

/* ------------------------------------------------------------------ keys (ONE table for both native shells) */
/* What windows/src/keys.hpp keyToBytes answers, keyed by a platform-neutral key instead of a VK_ code.
 * xterm.js's Keyboard.ts + the deck's Shift+Enter = ESC CR (renderer/js/20-terminals.js). */
#define DB_KEY_NONE 0
#define DB_KEY_UP 1
#define DB_KEY_DOWN 2
#define DB_KEY_RIGHT 3
#define DB_KEY_LEFT 4
#define DB_KEY_HOME 5
#define DB_KEY_END 6
#define DB_KEY_INSERT 7
#define DB_KEY_DELETE 8
#define DB_KEY_PAGE_UP 9
#define DB_KEY_PAGE_DOWN 10
#define DB_KEY_F1 11
#define DB_KEY_F2 12
#define DB_KEY_F3 13
#define DB_KEY_F4 14
#define DB_KEY_F5 15
#define DB_KEY_F6 16
#define DB_KEY_F7 17
#define DB_KEY_F8 18
#define DB_KEY_F9 19
#define DB_KEY_F10 20
#define DB_KEY_F11 21
#define DB_KEY_F12 22
#define DB_KEY_ENTER 23
#define DB_KEY_BACKSPACE 24
#define DB_KEY_TAB 25
#define DB_KEY_ESCAPE 26
#define DB_KEY_SPACE 27
/* ALT = Option on a Mac; META = Cmd (never part of a sequence) */
#define DB_MOD_SHIFT 1
#define DB_MOD_ALT 2
#define DB_MOD_CTRL 4
#define DB_MOD_META 8
/* The bytes for a key that is not text. Returns their count, or -1 = this table does not answer it (let the text
 * system type it). `out` needs 16 bytes. */
int db_key_bytes(int key, int mods, int app_cursor_keys, char* out, int cap);
/* Text typed with Option held, Option acting as Meta (xterm's macOptionIsMeta): ESC + the text. Count or -1. */
int db_meta_text(const char* utf8, size_t n, char* out, int cap);
/* What a paste sends: line ends -> CR, bracketed when the app asked (a paste cannot end its own bracket) */
const char* db_paste_bytes(const char* utf8, size_t n, int bracketed, size_t* out_n);
/* The window's own keys — lib/platform.js keyAction through core/deck/platform.cpp. `os`: "darwin" | "win32".
 * `key` is the DOM key name ("p", "F9", "Enter", "["), `code` the DOM code ("Backquote", may be ""). Returns
 * "palette" | "sidebar" | "fullscreen" | "prev" | "next" | "devtools" | "settings" | "redraw" | "zoom" | "asks" | "". */
const char* db_key_action(const char* os, const char* key, const char* code, int mods);
const char* db_key_help_json(const char* os);           /* platform.keyHelp */
const char* db_default_font_stack(const char* os);      /* platform.defaultFontStack */

/* ------------------------------------------------------------------ mouse (LOCAL mode only: R8) */
/* windows/src/mouse.hpp (a port of xterm's CoreMouseService, checked against it), the same file. */
#define DB_BTN_LEFT 0
#define DB_BTN_MIDDLE 1
#define DB_BTN_RIGHT 2
#define DB_BTN_NONE 3
#define DB_BTN_WHEEL 4
#define DB_ACT_UP 0
#define DB_ACT_DOWN 1
#define DB_ACT_LEFT 2
#define DB_ACT_RIGHT 3
#define DB_ACT_MOVE 32
typedef struct DBMouseEvent { int col, row, x, y, button, action, ctrl, alt, shift; } DBMouseEvent;
DBMouse* db_mouse_create(void);
void db_mouse_destroy(DBMouse* m);
void db_mouse_reset(DBMouse* m);
/* The report as it reaches the pty. Count, 0 = passed the protocol but nothing to send, -1 = not for the app. */
int db_mouse_report(DBMouse* m, const DBMouseEvent* e, int mouse_mode, int mouse_encoding, int cols, int rows, char* out, int cap);
/* Whole lines out of a running pixel sum (xterm's consumeWheelEvent); 0 = no report for this event */
int db_mouse_wheel_lines(DBMouse* m, double delta_y_px, double cell_height, int shift);
/* the cell and pixel under a point inside the grid, clamped (fills col, row, x, y) */
void db_mouse_coords(double px, double py, double cell_w, double cell_h, int cols, int rows, DBMouseEvent* e);

/* ------------------------------------------------------------------ the grid layout (core/model/layout) */
/* A window's layout: tile id -> a rectangle of cells, in insertion order (the order place() reports evictions in). */
#define DB_MAX_CELLS 16
DBLayout* db_layout_create(int rows, int cols);
void db_layout_destroy(DBLayout* l);
void db_layout_grid(const DBLayout* l, int* rows, int* cols);
void db_layout_regrid(DBLayout* l, int rows, int cols);                 /* blocks that no longer fit are dropped */
void db_layout_clear(DBLayout* l);
int db_layout_load_json(DBLayout* l, const char* json);                 /* {"id":[cells]} (deskInit's `layout`); 1 = read */
const char* db_layout_json(DBLayout* l);
int db_layout_count(const DBLayout* l);
const char* db_layout_id_at(DBLayout* l, int index);
int db_layout_cells(const DBLayout* l, const char* id, int* out, int cap);                 /* count; 0 = not placed */
int db_layout_rect(const DBLayout* l, const char* id, double* x, double* y, double* w, double* h);   /* fractions; 0 = not placed */
/* F3 / R3 / R6: place `id` on `cells` (legalised first: R1). Evicted ids are then db_layout_evicted_*. Returns 1. */
int db_layout_place(DBLayout* l, const char* id, const int* cells, int count);
int db_layout_evicted_count(const DBLayout* l);
const char* db_layout_evicted_at(DBLayout* l, int index);
void db_layout_unplace(DBLayout* l, const char* id);
void db_layout_prune(DBLayout* l, const char* const* live_ids, int count);
/* What a drop at (px, py) — fractions of the terminal area — takes; then where a placed tile dropped there lands */
int db_layout_drop_target(const DBLayout* l, double px, double py, int* out, int cap);
int db_layout_resolve_drop(const DBLayout* l, const char* id, const int* cells, int count, int* out, int cap);
int db_layout_rect_of_cells(const DBLayout* l, const int* cells, int count, double* x, double* y, double* w, double* h);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif  /* CLAUDE_DECK_BRIDGE_H */
