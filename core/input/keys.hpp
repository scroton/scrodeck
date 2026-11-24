// core/input/keys.hpp — THE key table, once, for every shell: a key press → the bytes xterm.js would send for it,
// what a paste sends, and which presses are the window's own rather than the terminal's.
//
// The Electron deck's terminals are xterm.js, so "what a key sends" is xterm's keyboard table
// (src/common/input/Keyboard.ts) plus the deck's own additions in renderer/js/20-terminals.js (Shift+Enter =
// ESC CR). Text itself arrives from the OS as text (WM_CHAR, NSTextInputClient); this covers the keys that are not
// text. The window's keys are lib/platform.js keyAction (the desktop) and renderer/desk/bridge.js keyAction (a
// remote), which agree — core/test/input/keys_test.cpp checks this table against core/deck/platform.cpp's port of
// the former, press for press, for both platforms.
//
// PURE: no OS header, no state, no allocation beyond the answer. A shell maps its own key codes to `Key`
// (windows/src/keys.hpp: VK_* → Key; the Mac shell: kVK_* / DB_KEY_* → Key) and calls these.
// The numbers of Key 0–27 and of the modifier bits are the Mac bridge's DB_KEY_* / DB_MOD_* (it may static_assert).
#pragma once
#include <string>

namespace deck { namespace input {

enum class Key {
  None = 0,
  Up = 1, Down = 2, Right = 3, Left = 4, Home = 5, End = 6, Insert = 7, Delete = 8, PageUp = 9, PageDown = 10,
  F1 = 11, F2 = 12, F3 = 13, F4 = 14, F5 = 15, F6 = 16, F7 = 17, F8 = 18, F9 = 19, F10 = 20, F11 = 21, F12 = 22,
  Enter = 23, Backspace = 24, Tab = 25, Escape = 26, Space = 27,
  // keys that type text and are in the table only because a window chord uses them (windowKeyOf)
  Backquote = 28, P = 29, B = 30, F = 31, R = 32, A = 33, Comma = 34, BracketLeft = 35, BracketRight = 36,
  Count_
};

enum ModBit { ModShift = 1, ModAlt = 2, ModCtrl = 4, ModMeta = 8 };

// `meta` is Cmd on a Mac (never set by the Windows shell). It is never part of a sequence: code() and any() are
// xterm's modifier parameter over Shift / Alt / Ctrl only.
struct Mods {
  bool shift = false, ctrl = false, alt = false, meta = false;
  int code() const { return 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0); }
  bool any() const { return shift || ctrl || alt; }
  int bits() const { return (shift ? ModShift : 0) | (alt ? ModAlt : 0) | (ctrl ? ModCtrl : 0) | (meta ? ModMeta : 0); }
  static Mods fromBits(int b) { Mods m; m.shift = (b & ModShift) != 0; m.alt = (b & ModAlt) != 0; m.ctrl = (b & ModCtrl) != 0; m.meta = (b & ModMeta) != 0; return m; }
};

// Returns true and fills `out` when the key is one this table answers (the shell then drops the text the OS would
// make of the press, if any). False = not ours: let the OS turn it into text.
inline bool keyToBytes(Key key, const Mods& m, bool appCursor, std::string& out) {
  const std::string esc = "\x1b";
  auto csiMod = [&](const char* final1, const char* plain) {   // ESC [ 1 ; m X   or the plain form
    if (m.any()) out = esc + "[1;" + std::to_string(m.code()) + final1; else out = plain;
  };
  auto tilde = [&](int n) {
    if (m.any()) out = esc + "[" + std::to_string(n) + ";" + std::to_string(m.code()) + "~"; else out = esc + "[" + std::to_string(n) + "~";
  };
  switch (key) {
    case Key::Up:    csiMod("A", appCursor ? "\x1bOA" : "\x1b[A"); return true;
    case Key::Down:  csiMod("B", appCursor ? "\x1bOB" : "\x1b[B"); return true;
    case Key::Right: csiMod("C", appCursor ? "\x1bOC" : "\x1b[C"); return true;
    case Key::Left:  csiMod("D", appCursor ? "\x1bOD" : "\x1b[D"); return true;
    case Key::Home:  csiMod("H", appCursor ? "\x1bOH" : "\x1b[H"); return true;
    case Key::End:   csiMod("F", appCursor ? "\x1bOF" : "\x1b[F"); return true;
    case Key::Insert: if (m.shift || m.ctrl) return false; tilde(2); return true;   // Shift+Insert is a paste (the shell's)
    case Key::Delete: tilde(3); return true;
    case Key::PageUp: tilde(5); return true;
    case Key::PageDown: tilde(6); return true;
    case Key::F1: csiMod("P", "\x1bOP"); return true;
    case Key::F2: csiMod("Q", "\x1bOQ"); return true;
    case Key::F3: csiMod("R", "\x1bOR"); return true;
    case Key::F4: csiMod("S", "\x1bOS"); return true;
    case Key::F5: tilde(15); return true;
    case Key::F6: tilde(17); return true;
    case Key::F7: tilde(18); return true;
    case Key::F8: tilde(19); return true;
    case Key::F9: tilde(20); return true;
    case Key::F10: tilde(21); return true;
    case Key::F11: tilde(23); return true;
    case Key::F12: tilde(24); return true;
    case Key::Enter:
      if (m.shift && !m.ctrl && !m.alt) { out = "\x1b\r"; return true; }     // the deck: a newline in the prompt
      out = m.alt ? "\x1b\r" : "\r"; return true;
    case Key::Backspace:
      out = m.ctrl ? "\x08" : "\x7f"; if (m.alt) out = esc + out; return true;
    case Key::Tab:
      if (m.ctrl) return false;                                              // Ctrl+Tab is the window's
      out = m.shift ? "\x1b[Z" : "\t"; return true;
    case Key::Escape: out = m.alt ? "\x1b\x1b" : "\x1b"; return true;
    case Key::Space:
      if (m.ctrl) { out = std::string(1, '\0'); return true; }
      return false;
    default: break;
  }
  return false;
}

// Alt (Option on a Mac, when it is Meta) + text: ESC, then the text as typed.
inline std::string metaText(const std::string& utf8) { return "\x1b" + utf8; }

// What a paste sends: xterm normalises line ends to CR and brackets the text when the app asked for it.
inline std::string pasteBytes(const std::string& utf8, bool bracketed) {
  std::string t; t.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size(); i++) {
    char c = utf8[i];
    if (c == '\r') { t.push_back('\r'); if (i + 1 < utf8.size() && utf8[i + 1] == '\n') i++; }
    else if (c == '\n') t.push_back('\r');
    else t.push_back(c);
  }
  if (!bracketed) return t;
  // a paste must not be able to end its own bracket
  std::string clean; size_t p = 0;
  for (;;) { size_t q = t.find("\x1b[201~", p); if (q == std::string::npos) { clean += t.substr(p); break; } clean += t.substr(p, q - p); p = q + 6; }
  return "\x1b[200~" + clean + "\x1b[201~";
}

// ------------------------------------------------------------------------------------------ the window's keys
// Which platform's chords: the function keys of Windows / Linux, or the Mac's Cmd map.
enum class Platform { Windows, Mac };
inline Platform platformOf(const std::string& os) { return os == "darwin" ? Platform::Mac : Platform::Windows; }

// lib/platform.js keyAction; bridge.js keyAction for a remote. ONE table for every way a press arrives: a window's
// own key handling and, while an embedded web view has the keyboard, its accelerator event. Pure.
// `devtools` (Ctrl+Shift+I / Cmd+Alt+I) is Electron's own and is not here: a native shell has none.
// `Settings` exists on the Mac only (Cmd+,).
enum class WinKey { None, Redraw, Zoom, Asks, Sidebar, Fullscreen, Prev, Next, Palette, Settings };
inline WinKey windowKeyOf(Key k, const Mods& m, Platform p) {
  if (p == Platform::Mac) {
    if (!m.meta) return WinKey::None;
    if (m.shift && !m.alt && k == Key::P) return WinKey::Palette;
    if (!m.shift && !m.alt && !m.ctrl && k == Key::B) return WinKey::Sidebar;
    if (m.ctrl && k == Key::F) return WinKey::Fullscreen;
    if (m.shift && k == Key::BracketLeft) return WinKey::Prev;
    if (m.shift && k == Key::BracketRight) return WinKey::Next;
    if (!m.shift && !m.alt && !m.ctrl && k == Key::Comma) return WinKey::Settings;
    if (!m.shift && !m.alt && !m.ctrl && k == Key::R) return WinKey::Redraw;
    if (m.shift && !m.alt && !m.ctrl && k == Key::Enter) return WinKey::Zoom;
    if (m.shift && !m.alt && !m.ctrl && k == Key::A) return WinKey::Asks;
    return WinKey::None;
  }
  if (k == Key::F5) return WinKey::Redraw;
  if (k == Key::F6) return WinKey::Zoom;
  if (k == Key::F7 && !m.ctrl && !m.alt && !m.meta) return WinKey::Asks;
  if (k == Key::F9 || (m.ctrl && !m.shift && !m.alt && k == Key::Backquote)) return WinKey::Sidebar;
  if (k == Key::F10 && !m.shift) return WinKey::Fullscreen;   // Shift+F10 is the tile menu (Windows' own key for a context menu)
  if (k == Key::F11) return WinKey::Prev;
  if (k == Key::F12) return WinKey::Next;
  if (m.ctrl && m.shift && !m.alt && k == Key::P) return WinKey::Palette;
  return WinKey::None;
}
// The action's name as lib/platform.js says it ("" for None).
inline const char* windowKeyName(WinKey k) {
  switch (k) {
    case WinKey::Redraw: return "redraw"; case WinKey::Zoom: return "zoom"; case WinKey::Asks: return "asks"; case WinKey::Sidebar: return "sidebar";
    case WinKey::Fullscreen: return "fullscreen"; case WinKey::Prev: return "prev"; case WinKey::Next: return "next"; case WinKey::Palette: return "palette";
    case WinKey::Settings: return "settings"; case WinKey::None: break;
  }
  return "";
}

// A key the page does not type with, pressed while an embedded web view (the ask pane) has the keyboard: whose?
//   App       one of the window's own keys: handled by the app, the page never sees it
//   ClosePane Esc with the keyboard on the pane itself (49-asks.js: the pane's own keydown)
//   Page      everything else — Esc inside a page's own box, Enter, Tab, the arrows, Ctrl+C / V / A / Z …
enum class WebKey { Page, ClosePane, App };
inline WebKey webKeyOf(Key k, const Mods& m, bool focusInFrame, Platform p) {
  if (k == Key::Escape && !m.ctrl && !m.shift && !m.alt && !m.meta) return focusInFrame ? WebKey::Page : WebKey::ClosePane;
  return windowKeyOf(k, m, p) != WinKey::None ? WebKey::App : WebKey::Page;
}

}}  // namespace deck::input
