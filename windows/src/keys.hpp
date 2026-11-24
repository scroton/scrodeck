// windows/src/keys.hpp — the Windows shell's side of the key table: VK_* → core/input/keys.hpp's Key, and the
// modifiers as Windows reports them (AltGr). THE TABLE ITSELF — what a key sends, what a paste sends, which keys
// are the window's — is core/input/keys.hpp, shared with the Mac shell and tested in core/test/input.
// Text itself arrives as WM_CHAR; this covers the keys that are not text.
#pragma once
#include <windows.h>
#include <string>
#include "../../core/input/keys.hpp"

namespace deck {

using Mods = input::Mods;               // { shift, ctrl, alt, meta (never set here) }, code(), any()
using WinKey = input::WinKey;           // None, Redraw, Zoom, Asks, Sidebar, Fullscreen, Prev, Next, Palette (Settings: the Mac's)
using WebKey = input::WebKey;           // Page, ClosePane, App
using input::pasteBytes;

inline Mods currentMods() {
  Mods m;
  m.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  m.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  m.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  // AltGr is Ctrl+Alt on Windows and types a character: it is not a modifier for a sequence
  if (m.ctrl && m.alt && (GetKeyState(VK_RMENU) & 0x8000)) { m.ctrl = false; m.alt = false; }
  return m;
}

// The modifiers from raw key states (a web view's accelerator event reads them with GetAsyncKeyState): the same
// AltGr rule as currentMods.
inline Mods modsFrom(bool ctrl, bool shift, bool alt, bool rightAlt) {
  Mods m; m.ctrl = ctrl; m.shift = shift; m.alt = alt;
  if (m.ctrl && m.alt && rightAlt) { m.ctrl = false; m.alt = false; }
  return m;
}

// The one mapping this file owns. A virtual key the table has no row for is Key::None (it types text, or nothing).
inline input::Key keyOfVk(UINT vk) {
  using input::Key;
  switch (vk) {
    case VK_UP: return Key::Up; case VK_DOWN: return Key::Down; case VK_RIGHT: return Key::Right; case VK_LEFT: return Key::Left;
    case VK_HOME: return Key::Home; case VK_END: return Key::End; case VK_INSERT: return Key::Insert; case VK_DELETE: return Key::Delete;
    case VK_PRIOR: return Key::PageUp; case VK_NEXT: return Key::PageDown;
    case VK_F1: return Key::F1; case VK_F2: return Key::F2; case VK_F3: return Key::F3; case VK_F4: return Key::F4; case VK_F5: return Key::F5; case VK_F6: return Key::F6;
    case VK_F7: return Key::F7; case VK_F8: return Key::F8; case VK_F9: return Key::F9; case VK_F10: return Key::F10; case VK_F11: return Key::F11; case VK_F12: return Key::F12;
    case VK_RETURN: return Key::Enter; case VK_BACK: return Key::Backspace; case VK_TAB: return Key::Tab; case VK_ESCAPE: return Key::Escape; case VK_SPACE: return Key::Space;
    case VK_OEM_3: return Key::Backquote; case 'P': return Key::P;
    // (B, F, R, A, the comma and the brackets are in the Mac's Cmd map only: here they are text)
    default: return Key::None;
  }
}

// The window's own keys (lib/platform.js keyAction; bridge.js keyAction for a remote): which action a press is.
inline WinKey windowKeyOf(UINT vk, const Mods& m) { return input::windowKeyOf(keyOfVk(vk), m, input::Platform::Windows); }

// A key pressed while the ask pane's web view has the keyboard (core/input/keys.hpp webKeyOf). Esc is judged on
// the RAW modifiers (AltGr+Esc is not a bare Esc), the window's keys on the AltGr-corrected ones.
inline WebKey webKeyOf(UINT vk, bool ctrl, bool shift, bool alt, bool rightAlt, bool focusInFrame) {
  if (vk == VK_ESCAPE && (ctrl || shift || alt)) return WebKey::Page;
  return input::webKeyOf(keyOfVk(vk), modsFrom(ctrl, shift, alt, rightAlt), focusInFrame, input::Platform::Windows);
}

// Returns true and fills `out` when the key is one the table answers (then its WM_CHAR, if any, is dropped by the
// caller). False = let TranslateMessage turn it into WM_CHAR.
inline bool keyToBytes(UINT vk, const Mods& m, bool appCursor, std::string& out) { return input::keyToBytes(keyOfVk(vk), m, appCursor, out); }

}  // namespace deck
