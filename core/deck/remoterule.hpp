// core/deck/remoterule.hpp — where a remote desk stops: lib/remoterule.js, ported. PURE.
//
// The remote-desk rule (2026-11-18): "'it only shows one place' should only be on the same box, it shouldn't care
// about others on other boxes; likewise, the other things panes drive (going to sleep) shouldn't impact it from a
// remote session ... I should be able to open and see all of them here in different panes without impacting the
// desk at all". A remote desk (the browser desk at /desk/, the native exe in remote mode) has a window entry —
// an index, a layout, a "what I show" list — but it is NOT one of the deck's own windows:
//
//   layout   one home per session holds among the deck's own windows only. A remote's layout evicts nothing and
//            is evicted by nothing; no own window is told a remote holds a session, no remote that a window does.
//   sleep    "on screen" is the deck's own visible windows (and a phone, unchanged). The one exception: a session
//            that is ASLEEP is woken when a remote brings it on screen (wakesForRemote), because there is no
//            process to stream otherwise. Once awake it sleeps again by the office's rules.
//   size     NOT here: driverrule.hpp.
//
// Checked against core/test/fixtures/deck/remoterule.json (core/test/deck/gen-remoterule.js <lib/remoterule.js>).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace deck { namespace core { namespace remoterule {

// One entry of the window registry as the rule reads it.
struct Entry {
  int index = 0;
  bool remote = false;
  std::vector<std::string> layout;   // the session ids its layout holds, in the layout's own order
  std::vector<std::string> shown;    // what its panes show
  bool hidden = false;               // a remote: its tab is not visible
  bool visible = true;               // an own window: on screen (not minimised, not hidden) — `visible(entry)` in the JS
};
struct Eviction { size_t entry = 0; std::vector<std::string> ids; };   // `entry`: a position in the list handed in
struct Shown { bool has = false; std::vector<std::string> shown; bool hidden = false; };   // has = false: null

// Which of the deck's OWN windows holds this session in its layout: its position in `entries`, or -1. Never a remote.
int holder(const std::string& id, const std::vector<Entry>& entries);
// entries[taker] has taken `layout` (ids): who loses what. Nothing when one-window is off, nothing when the
// taker is a remote, and a remote never loses anything. `taker` may be past the end (an entry not in the list).
std::vector<Eviction> evictions(size_t taker, bool takerRemote, const std::vector<std::string>& layout, const std::vector<Entry>& entries, bool oneWindow);
// The deck's own windows showing this session right now, as hibernation's tokens (`win:<index>`).
std::vector<std::string> officeShownBy(const std::string& id, const std::vector<Entry>& entries);
bool officeShows(const std::string& id, const std::vector<Entry>& entries);
// hibernation's "on screen" set: the own windows, plus 'remote' for a PHONE attached to it. Never a remote desk.
std::vector<std::string> shownBy(const std::string& id, const std::vector<Entry>& entries, bool phoneWatching);
// Who answers a session's terminal queries (device attributes, cursor position, status, colours, window size):
// EXACTLY ONE (2026-11-19, item 14b: never two - a second answer is junk in Claude's prompt - and never none -
// a stall). The deck's own window that shows it ("office"); else the remote desk whose visible tab shows it in a pane
// ("remote": the native exe or the browser desk); else a phone attached to it ("phone"); else "office" (an office
// terminal it may still have, a minimised window's). The C++ deck sends it per tile as meta `answerer`; main.js sends
// none - the Electron window has a terminal for EVERY session and answers all, so a remote answers nothing there.
// No JS twin: lib/remoterule.js has no such function (the Electron deck needs none).
std::string answerer(bool officeShows, bool remoteShows, bool phoneWatching);
// The sessions a remote desk's new "what I show" wakes: the ones that just came onto its screen and are asleep.
std::vector<std::string> wakesForRemote(const Shown& prev, const Shown& next, const std::function<bool(const std::string&)>& asleep);

}}}  // namespace deck::core::remoterule
