// core/test/input/keys_test.cpp — core/input/keys.hpp: the one key table of every shell.
// Where the rows come from:
//   [win]  the Windows shell's own unit checks of the window keys (windows/web-test/asks_test.cpp "whose key is
//          it"), restated on the neutral Key — the VK_* → Key mapping itself stays checked there;
//   [mac]  the Mac draft's bridge test rows (mac/Tests/bridge_test.cpp "keys: …"), which were written against
//          its own twin of the table: the same answers from this one is what "the two agree" means;
//   [js]   every Key x every modifier set x both platforms against core/deck/platform.cpp keyAction, the port of
//          lib/platform.js keyAction that the deck test checks against the JavaScript.
// No process, no file, nothing on screen.
#include "../../input/keys.hpp"
#include "../../deck/platform.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace deck::input;
using nlohmann::json;

static int pass = 0, fail = 0;
static void check(const char* what, bool ok) { if (ok) pass++; else { fail++; printf("FAIL %s\n", what); } }
static std::string bytes(Key k, int mods, bool app) { std::string o; return keyToBytes(k, Mods::fromBits(mods), app, o) ? o : std::string("<none>"); }
static Mods M(bool c, bool s, bool a) { Mods m; m.ctrl = c; m.shift = s; m.alt = a; return m; }

// what a browser's KeyboardEvent says for a Key (key, code): the input of lib/platform.js keyAction
struct Dom { std::string key, code; };
static Dom domOf(Key k, bool shift) {
  static const char* named[] = { "", "ArrowUp", "ArrowDown", "ArrowRight", "ArrowLeft", "Home", "End", "Insert", "Delete", "PageUp", "PageDown",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Enter", "Backspace", "Tab", "Escape", " " };
  const int i = (int)k;
  if (i <= (int)Key::Space) return { named[i], named[i] };
  switch (k) {
    case Key::Backquote: return { shift ? "~" : "`", "Backquote" };
    case Key::P: return { shift ? "P" : "p", "KeyP" }; case Key::B: return { shift ? "B" : "b", "KeyB" }; case Key::F: return { shift ? "F" : "f", "KeyF" };
    case Key::R: return { shift ? "R" : "r", "KeyR" }; case Key::A: return { shift ? "A" : "a", "KeyA" };
    case Key::Comma: return { shift ? "<" : ",", "Comma" };
    case Key::BracketLeft: return { shift ? "{" : "[", "BracketLeft" }; case Key::BracketRight: return { shift ? "}" : "]", "BracketRight" };
    default: return { "", "" };
  }
}

int main() {
  const std::string none = "<none>";
  // ---------------------------------------------------------------- [mac] the bridge test's rows
  enum { S = ModShift, A = ModAlt, C = ModCtrl, CMD = ModMeta };
  check("[mac] arrows, plain and application mode", bytes(Key::Up, 0, 0) == "\x1b[A" && bytes(Key::Up, 0, 1) == "\x1bOA" && bytes(Key::Left, 0, 0) == "\x1b[D");
  check("[mac] modified arrows (Shift 2, Alt 3, Ctrl 5)", bytes(Key::Right, S, 0) == "\x1b[1;2C" && bytes(Key::Right, A, 1) == "\x1b[1;3C" && bytes(Key::Down, C | S, 0) == "\x1b[1;6B");
  check("[mac] Cmd is never part of a sequence", bytes(Key::Up, CMD, 0) == "\x1b[A");
  check("[mac] Home / End / PageUp / Delete", bytes(Key::Home, 0, 0) == "\x1b[H" && bytes(Key::End, 0, 1) == "\x1bOF" && bytes(Key::PageUp, 0, 0) == "\x1b[5~" && bytes(Key::PageUp, S, 0) == "\x1b[5;2~" && bytes(Key::Delete, 0, 0) == "\x1b[3~");
  check("[mac] F1-F4 SS3, F5-F12 tilde", bytes(Key::F1, 0, 0) == "\x1bOP" && bytes(Key::F4, S, 0) == "\x1b[1;2S" && bytes(Key::F5, 0, 0) == "\x1b[15~" && bytes(Key::F12, 0, 0) == "\x1b[24~" && bytes(Key::F11, C, 0) == "\x1b[23;5~");
  check("[mac] Enter, Shift+Enter (the deck's newline), Alt+Enter", bytes(Key::Enter, 0, 0) == "\r" && bytes(Key::Enter, S, 0) == "\x1b\r" && bytes(Key::Enter, A, 0) == "\x1b\r");
  check("[mac] Backspace, Ctrl+Backspace, Alt+Backspace", bytes(Key::Backspace, 0, 0) == "\x7f" && bytes(Key::Backspace, C, 0) == "\x08" && bytes(Key::Backspace, A, 0) == "\x1b\x7f");
  check("[mac] Tab, Shift+Tab; Ctrl+Tab is the window's", bytes(Key::Tab, 0, 0) == "\t" && bytes(Key::Tab, S, 0) == "\x1b[Z" && bytes(Key::Tab, C, 0) == none);
  check("[mac] Esc, Alt+Esc, Ctrl+Space = NUL, Space is text", bytes(Key::Escape, 0, 0) == "\x1b" && bytes(Key::Escape, A, 0) == "\x1b\x1b" && bytes(Key::Space, C, 0) == std::string(1, '\0') && bytes(Key::Space, 0, 0) == none);
  check("[mac] Insert, and Shift+Insert left to the shell (a paste)", bytes(Key::Insert, 0, 0) == "\x1b[2~" && bytes(Key::Insert, S, 0) == none);
  check("[mac] Option as Meta", metaText("b") == "\x1b" "b");
  check("[mac] paste: line ends become CR", pasteBytes(std::string("a\r\nb\nc", 6), false) == "a\rb\rc");
  check("[mac] paste: bracketed, and it cannot end its own bracket", pasteBytes(std::string("x\x1b[201~y", 8), true) == "\x1b[200~xy\x1b[201~");
  auto name = [](Key k, int mods, Platform p) { return std::string(windowKeyName(windowKeyOf(k, Mods::fromBits(mods), p))); };
  check("[mac] the Mac window keys", name(Key::B, CMD, Platform::Mac) == "sidebar" && name(Key::P, CMD | S, Platform::Mac) == "palette" && name(Key::BracketRight, CMD | S, Platform::Mac) == "next"
    && name(Key::F, CMD | C, Platform::Mac) == "fullscreen" && name(Key::Comma, CMD, Platform::Mac) == "settings" && name(Key::R, CMD, Platform::Mac) == "redraw"
    && name(Key::Enter, CMD | S, Platform::Mac) == "zoom" && name(Key::A, CMD | S, Platform::Mac) == "asks" && name(Key::B, 0, Platform::Mac) == "" && name(Key::F9, 0, Platform::Mac) == "");
  check("[mac] the Windows window keys through the same function", name(Key::F9, 0, Platform::Windows) == "sidebar" && name(Key::Backquote, C, Platform::Windows) == "sidebar");
  check("[mac] platformOf", platformOf("darwin") == Platform::Mac && platformOf("win32") == Platform::Windows && platformOf("linux") == Platform::Windows);

  // ---------------------------------------------------------------- [win] the Windows shell's unit checks
  const Platform W = Platform::Windows;
  check("[win] Shift+F10 is no window key (the tile menu)", windowKeyOf(Key::F10, M(0, 1, 0), W) == WinKey::None);
  check("[win] window key: F5 redraw, F6 zoom, F9 sidebar, F10 fullscreen, F11 / F12 step", windowKeyOf(Key::F5, M(0, 0, 0), W) == WinKey::Redraw && windowKeyOf(Key::F6, M(0, 0, 0), W) == WinKey::Zoom
    && windowKeyOf(Key::F9, M(0, 0, 0), W) == WinKey::Sidebar && windowKeyOf(Key::F10, M(0, 0, 0), W) == WinKey::Fullscreen && windowKeyOf(Key::F11, M(0, 0, 0), W) == WinKey::Prev && windowKeyOf(Key::F12, M(0, 0, 0), W) == WinKey::Next);
  check("[win] window key: F7 asks, also with Shift; not with Ctrl or Alt", windowKeyOf(Key::F7, M(0, 0, 0), W) == WinKey::Asks && windowKeyOf(Key::F7, M(0, 1, 0), W) == WinKey::Asks
    && windowKeyOf(Key::F7, M(1, 0, 0), W) == WinKey::None && windowKeyOf(Key::F7, M(0, 0, 1), W) == WinKey::None);
  check("[win] window key: Ctrl+Shift+P palette; not Ctrl+P, Shift+P, Ctrl+Alt+Shift+P", windowKeyOf(Key::P, M(1, 1, 0), W) == WinKey::Palette && windowKeyOf(Key::P, M(1, 0, 0), W) == WinKey::None
    && windowKeyOf(Key::P, M(0, 1, 0), W) == WinKey::None && windowKeyOf(Key::P, M(1, 1, 1), W) == WinKey::None);
  check("[win] window key: Ctrl+` sidebar; not ` alone, not Ctrl+Shift+`", windowKeyOf(Key::Backquote, M(1, 0, 0), W) == WinKey::Sidebar && windowKeyOf(Key::Backquote, M(0, 0, 0), W) == WinKey::None
    && windowKeyOf(Key::Backquote, M(1, 1, 0), W) == WinKey::None);
  { bool none_ = true;
    for (Key k : { Key::Enter, Key::Tab, Key::Backspace, Key::Left, Key::Up, Key::Home, Key::PageUp, Key::F1, Key::F2, Key::F3, Key::F4, Key::F8, Key::Escape, Key::Space, Key::None, Key::B, Key::A })
      if (windowKeyOf(k, M(0, 0, 0), W) != WinKey::None) none_ = false;
    check("[win] window key: the keys a terminal or a page types with are nobody's", none_); }
  { bool app = true;
    for (Key k : { Key::F5, Key::F6, Key::F7, Key::F9, Key::F10, Key::F11, Key::F12 }) for (bool inFrame : { false, true }) if (webKeyOf(k, M(0, 0, 0), inFrame, W) != WebKey::App) app = false;
    check("[win] page has the keys: the function keys are the app's, wherever the caret is", app); }
  check("[win] page has the keys: Ctrl+Shift+P and Ctrl+` are the app's", webKeyOf(Key::P, M(1, 1, 0), true, W) == WebKey::App && webKeyOf(Key::Backquote, M(1, 0, 0), true, W) == WebKey::App);
  check("[win] page has the keys: Esc closes the pane, unless a page's own box has the caret", webKeyOf(Key::Escape, M(0, 0, 0), false, W) == WebKey::ClosePane && webKeyOf(Key::Escape, M(0, 0, 0), true, W) == WebKey::Page);
  check("[win] page has the keys: Shift+Esc, Ctrl+Esc stay the page's", webKeyOf(Key::Escape, M(0, 1, 0), false, W) == WebKey::Page && webKeyOf(Key::Escape, M(1, 0, 0), false, W) == WebKey::Page);
  { bool page = true;
    for (Key k : { Key::P, Key::A, Key::None }) if (webKeyOf(k, M(1, 0, 0), true, W) != WebKey::Page) page = false;
    for (Key k : { Key::Enter, Key::Tab, Key::Backspace, Key::Left, Key::Up, Key::Home, Key::PageUp, Key::F1, Key::F8 }) if (webKeyOf(k, M(0, 0, 0), true, W) != WebKey::Page) page = false;
    check("[win] page has the keys: what a page types and edits with stays the page's", page); }
  check("[win] page has the keys: Ctrl+F7 and Alt+F7 are not the asks key", webKeyOf(Key::F7, M(1, 0, 0), false, W) == WebKey::Page && webKeyOf(Key::F7, M(0, 0, 1), false, W) == WebKey::Page);
  check("[mac] page has the keys: Cmd+B is the app's, Esc closes, Cmd+Esc is the page's", webKeyOf(Key::B, Mods::fromBits(CMD), true, Platform::Mac) == WebKey::App
    && webKeyOf(Key::Escape, Mods(), false, Platform::Mac) == WebKey::ClosePane && webKeyOf(Key::Escape, Mods::fromBits(CMD), false, Platform::Mac) == WebKey::Page && webKeyOf(Key::F9, Mods(), true, Platform::Mac) == WebKey::Page);

  // ---------------------------------------------------------------- the whole byte table, by its rules
  // every key x every Shift/Alt/Ctrl set x cursor mode: Cmd changes nothing; the modifier parameter is 1+S+2A+4C
  { bool metaFree = true, param = true;
    for (int k = 0; k < (int)Key::Count_; k++) for (int mods = 0; mods < 8; mods++) for (int app = 0; app < 2; app++) {
      const std::string a = bytes((Key)k, mods, app != 0), b = bytes((Key)k, mods | CMD, app != 0);
      if (a != b) metaFree = false;
      if (mods && k >= (int)Key::Up && k <= (int)Key::F12 && a != none) {
        const std::string want = ";" + std::to_string(1 + ((mods & S) ? 1 : 0) + ((mods & A) ? 2 : 0) + ((mods & C) ? 4 : 0));
        if (a.find(want) == std::string::npos || a.compare(0, 2, "\x1b[") != 0) param = false;
      }
    }
    check("table: Cmd never changes what a key sends", metaFree);
    check("table: a modified cursor / editing / function key is CSI with xterm's modifier parameter", param); }
  { bool textKeys = true;
    for (Key k : { Key::None, Key::Backquote, Key::P, Key::B, Key::F, Key::R, Key::A, Key::Comma, Key::BracketLeft, Key::BracketRight }) for (int mods = 0; mods < 16; mods++) if (bytes(k, mods, false) != none) textKeys = false;
    check("table: the chord-only keys never send bytes of their own (they are text)", textKeys); }
  check("table: every function key", bytes(Key::F2, 0, 0) == "\x1bOQ" && bytes(Key::F3, 0, 0) == "\x1bOR" && bytes(Key::F4, 0, 0) == "\x1bOS" && bytes(Key::F6, 0, 0) == "\x1b[17~" && bytes(Key::F7, 0, 0) == "\x1b[18~"
    && bytes(Key::F8, 0, 0) == "\x1b[19~" && bytes(Key::F9, 0, 0) == "\x1b[20~" && bytes(Key::F10, 0, 0) == "\x1b[21~" && bytes(Key::F11, 0, 0) == "\x1b[23~" && bytes(Key::F1, A | C, 0) == "\x1b[1;7P");
  check("table: Down / Right / Home in application mode, PageDown, Ctrl+Insert not ours", bytes(Key::Down, 0, 1) == "\x1bOB" && bytes(Key::Right, 0, 1) == "\x1bOC" && bytes(Key::Home, 0, 1) == "\x1bOH"
    && bytes(Key::PageDown, 0, 0) == "\x1b[6~" && bytes(Key::Insert, C, 0) == none && bytes(Key::Insert, A, 0) == "\x1b[2;3~" && bytes(Key::Delete, C, 0) == "\x1b[3;5~");
  check("table: Ctrl+Enter is CR, Ctrl+Shift+Enter is CR, Alt+Shift+Enter is ESC CR, Ctrl+Alt+Backspace", bytes(Key::Enter, C, 0) == "\r" && bytes(Key::Enter, C | S, 0) == "\r" && bytes(Key::Enter, A | S, 0) == "\x1b\r"
    && bytes(Key::Backspace, C | A, 0) == "\x1b\x08" && bytes(Key::Tab, A, 0) == "\t" && bytes(Key::Space, C | S, 0) == std::string(1, '\0'));
  check("paste: a lone CR stays, CRLF CRLF is two, nothing is nothing, bracketed nothing is an empty bracket", pasteBytes("a\rb", false) == "a\rb" && pasteBytes("\r\n\r\n", false) == "\r\r" && pasteBytes("", false) == ""
    && pasteBytes("", true) == "\x1b[200~\x1b[201~" && pasteBytes("a\nb", true) == "\x1b[200~a\rb\x1b[201~");
  check("mods: bits round-trip, code and any ignore Cmd", Mods::fromBits(15).bits() == 15 && Mods::fromBits(CMD).code() == 1 && !Mods::fromBits(CMD).any() && Mods::fromBits(S | A | C).code() == 8);

  // ---------------------------------------------------------------- [js] against the port of lib/platform.js keyAction
  { int rows = 0, bad = 0;
    for (const char* os : { "win32", "darwin" }) for (int k = 0; k < (int)Key::Count_; k++) for (int mods = 0; mods < 16; mods++) {
      const Mods m = Mods::fromBits(mods);
      const Dom d = domOf((Key)k, m.shift);
      std::string want = deck::core::platform::keyAction(os, json{ { "key", d.key }, { "code", d.code }, { "control", m.ctrl }, { "shift", m.shift }, { "alt", m.alt }, { "meta", m.meta } });
      if (want == "devtools") want = "";                     // Electron's own; no native shell has it
      const std::string got = windowKeyName(windowKeyOf((Key)k, m, platformOf(os)));
      rows++;
      if (got != want) { bad++; if (bad <= 10) printf("FAIL [js] %s key %d mods %d: table '%s', keyAction '%s'\n", os, k, mods, got.c_str(), want.c_str()); }
    }
    pass += rows - bad; fail += bad;
    check("[js] every key x modifiers x platform was compared (2 x 37 x 16)", rows == 2 * (int)Key::Count_ * 16 && rows == 1184); }

  printf("keys: %d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
