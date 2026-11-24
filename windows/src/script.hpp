// windows/src/script.hpp — the test script of `--do "<steps>"` (headless runs only): the words, not the acting.
//
// A script is steps separated by `;`. Each step is carried out by App::runStep through the SAME handlers the
// window messages call (onMouseDown / onMouseUp / onMouseMove / onRightClick / onKeyDown / onChar), so what a
// script proves is what a hand would get. Coordinates are DIPs from the window's top-left.
//
//   wait <ms>                         let replies arrive (every step is followed by a short wait anyway)
//   click|dbl|rclick|move|down|up <x> <y>
//   zone|dzone|rzone|hzone|downzone <kind> [id] [arg]
//                                     the same on the centre of a sidebar zone (ui.hpp Hit kinds). id: a tile id,
//                                     `sel` (the selected tile), `title:<prefix>`, or `-` for any; arg likewise.
//                                     A word with spaces goes in single quotes: zone tile 'title:native test one'
//   ozone|rozone|hozone <kind> [id]   the same on an overlay's zone (a menu row, a palette row, a panel control)
//   menu <label prefix>               click the context-menu row that starts with this
//   key|keyup|tap <spec>              ctrl+shift+P, F11, enter, esc, up, 1 …  (key = down only; tap = down then up)
//   text <anything>                   typed characters (WM_CHAR), to the end of the step
//   sel <tile title prefix>           select that tile, as a click on it would
//   shot <name>                       save a frame now, as n-<name>.png beside the run's own shot
//   dump zones|ozones|state|calls     write to <shot>.log.txt
//   size <w> <h>                      resize the view (a --window-shot run: the real window, through WM_SIZE)
//   call <fn> <json args>             a deck call through App::call (the test blocks apply); JSON may use ' for "
//   wheel <x> <y> <delta>             a wheel message (120 = one notch up)
//   char <code>                       one WM_CHAR by code (0x1b, 3 …)
//   ws drop|open                      the socket's own events, as ws.hpp posts them (a drop, a reconnect)
//   focus off|on                      the window loses / gets the keyboard focus
//   dump tiles|screen                 the deck's tiles and the window title; the selected pane's text
//   dump auditors                     each tile's auditor / child / pause and its files from an auditor (auditorMail)
//   tri <x> <y> | mclick <x> <y>      a triple click; a middle click (rclick is the right one)
//   mods shift|ctrl|alt|none          the modifiers held for the mouse steps that follow
//   feed <bytes>                      bytes into the selected pane's terminal, as if its program wrote them
//                                     (\e = ESC, \r, \n, \s = a semicolon, which would end the step)
//   clip <text> | dump clipboard      the TEST's clipboard (a test never touches the real one): set it, log it
//   clipimg <n> | clipfile <p>|<p>   a picture of n bytes / files copied as in Explorer on it instead, no text
//   dump layout                       this window's layout and what it shows
//   dump sidebar                      the sidebar's drawn width, its narrowest (the tile strip's width) and the remembered one
//   faketile <field> <json>           a field of the selected tile, as a deck in another state would send it
//   fakeset <key|settings.key> <json> a field of the meta itself (settings.effortStyle 'color', restart {'armed':true})
//   anim <ms> | anim off | anim dump  the animation clock set to <ms> (a headless run has no timer and draws the static
//                                     look until a script says this); the static look again; what moves, to the log
//   cloud dump|eval|click|text|key|focus|age …   the cloud panes (app.cpp cloudStep; windows/test/cloud.ps1)
//   cdlg fail <text>                  the New cloud session dialog: the deck's refusal, as its err would arrive
//   wm min|restore|paint              window messages a test cannot cause for real
//   ws silence|count                  the 60 s watchdog at once; how many connections this client has open
//   close                             the window's own WM_CLOSE: the run ends the way a person ends it
//   ask open [all|sel|<session id>] [recent|<session id>]   the ask pane (a --window-shot run: with its web view)
//   ask close | ask warm | ask focus  close it; make its web view ahead of an open (as the pointer resting on the asks tile does); the keyboard into the page
//   ask host <js>                     a REAL mouse click on an element of the pane's own document (js gives the element)
//   ask in <n> <js>                   the same on an element of the n-th ask page on show (its sandboxed frame)
//   ask click <x> <y>                 the same at page coordinates (css px from the web view's corner)
//   ask text <words>                  typed into whatever has the caret in the page (DevTools Input.insertText)
//   ask eval <n|-> <js>               a value from the n-th page (- = the pane's document), to the log. No `;` in js
//   ask key <spec>                    MoveFocus into the page, then the key POSTED to the web view's own windows
//   ask dump                          the pane's state, the web view's start-up times, what the host document shows
//   webmem <label>                    the msedgewebview2.exe processes that DESCEND from this exe, counted and weighed, to the log
//   crash                             the process ends at once, as a crash would (TerminateProcess): nothing is closed
//   webscan [word …]                  is the deck's token (or a word: a cookie's name) in any file of the test's web folders, now
//   toast [waiting|done|ask [session]] | toast click [tile] | toast flash | toast dump   the notifier's test double
//   toast <kind>                      any other kind the core raises (desk, memory, settings-warning), recorded the same way
//   event <name> <json>               an `event` frame as the deck would send it (input:refused {'reason':'remote'})
//   ime comp <text> | ime commit <text> | ime end   an input method composing in the selected terminal, as its messages are handled
//   trans <ms> | trans off | trans dump   the css transitions' clock (a test's are off until a script sets it); what is under way
//   stamp <text>                      what the special prompt form's time box makes of an entry, to the log
//   dump ghost [name]                 the picture a tile being dragged carries outside the window, saved as n-<name>.png
//   life spec|bump|second|lock|deskwindow   the lifecycle: what a restart starts, a rebuilt exe, a second launch, the lock; a window
//                                     for a client the CORE registered (core::Lifecycle::openWindow)
//   ask link <url>                    a link on an ask page followed (LOCAL: the deck's askOpenLink decides; a test only logs)
// (`--do-file <path>` reads the script from a file: js with quotes needs no quoting through a shell.)
// The log also names what the run SENT: `input` (keys, pastes), `report` (mouse / focus, local mode), `reply` (the
// terminal's own answers), `resize`, `nudge`, and `copy (ctrl+c, nothing sent)`.
// Calls that would start or move a Claude session, restart the deck or rewrite accounts are never sent from a
// headless run (App::call logs them as BLOCKED): a test can press the button and read the log.
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "keys.hpp"

namespace deck {

struct ScriptStep { std::string cmd, rest; std::vector<std::string> args; };

inline std::vector<ScriptStep> parseScript(const std::string& src) {
  std::vector<ScriptStep> out;
  size_t p = 0;
  while (p <= src.size()) {
    size_t q = src.find(';', p); if (q == std::string::npos) q = src.size();
    std::string s = src.substr(p, q - p); p = q + 1;
    size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) continue;
    s = s.substr(a); while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    ScriptStep st; size_t sp = s.find(' ');
    st.cmd = s.substr(0, sp); st.rest = sp == std::string::npos ? "" : s.substr(sp + 1);
    size_t i = 0;
    while (i < st.rest.size()) {                         // words; 'a quoted one' may hold spaces
      if (st.rest[i] == ' ') { i++; continue; }
      if (st.rest[i] == '\'') { size_t j = st.rest.find('\'', i + 1); if (j == std::string::npos) j = st.rest.size(); st.args.push_back(st.rest.substr(i + 1, j - i - 1)); i = j + 1; continue; }
      size_t j = st.rest.find(' ', i); if (j == std::string::npos) j = st.rest.size();
      st.args.push_back(st.rest.substr(i, j - i)); i = j + 1;
    }
    out.push_back(st);
  }
  return out;
}

// "ctrl+shift+P" -> the virtual key and the modifiers held with it; false when the name is not known
inline bool parseKeySpec(const std::string& spec, UINT& vk, Mods& m) {
  m = Mods(); vk = 0;
  size_t p = 0; std::string last;
  while (p <= spec.size()) {
    size_t q = spec.find('+', p); if (q == std::string::npos || q == spec.size() - 1) q = spec.size();
    std::string t = spec.substr(p, q - p); p = q + 1;
    std::string l; for (char c : t) l.push_back((char)tolower((unsigned char)c));
    if (l == "ctrl") m.ctrl = true; else if (l == "shift") m.shift = true; else if (l == "alt") m.alt = true; else last = l;
  }
  static const struct { const char* n; UINT vk; } names[] = {
    { "enter", VK_RETURN }, { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE }, { "tab", VK_TAB }, { "space", VK_SPACE }, { "up", VK_UP }, { "down", VK_DOWN },
    { "left", VK_LEFT }, { "right", VK_RIGHT }, { "home", VK_HOME }, { "end", VK_END }, { "pgup", VK_PRIOR }, { "pgdn", VK_NEXT }, { "back", VK_BACK },
    { "del", VK_DELETE }, { "backquote", VK_OEM_3 }, { "ins", VK_INSERT }, { "apps", VK_APPS } };
  for (auto& n : names) if (last == n.n) { vk = n.vk; return true; }
  if (last.size() >= 2 && last[0] == 'f') { int n = atoi(last.c_str() + 1); if (n >= 1 && n <= 12) { vk = VK_F1 + n - 1; return true; } }
  if (last.size() == 1) { char c = (char)toupper((unsigned char)last[0]); if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { vk = (UINT)c; return true; } }
  return false;
}

}  // namespace deck
