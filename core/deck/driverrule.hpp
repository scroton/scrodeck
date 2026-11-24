// core/deck/driverrule.hpp — who drives a session's size, as a CLAIM by one kind of screen: lib/driverrule.js,
// ported. PURE. A PROPOSAL (2026-11-18) the owner has not accepted yet: the deck uses it only with
// DeckOptions::driverClaim (deck.hpp).
//
// A pty has ONE size. Three KINDS of screen can show a session: `office` (the deck's own windows), `phone` (the
// phone page, attached to one session at a time), `remote` (a remote desk). One kind drives a session: the pty
// takes that screen's size, the others render at the pty's size and send no size of their own.
//
//   'office'  the office deck drives every session; the phone and a remote desk mirror.        (was 'desk')
//   'phone'   the phone drives every session, attached or not; everything else mirrors.
//   'remote'  the remote desk drives the sessions it SHOWS; one it does not show falls back to the office.
//   'auto'    whoever is LOOKING drives, per session: a phone attached to it, else a remote desk showing it,
//             else the office deck.
//
// Every `m` is the raw setting (any JSON value): 'desk' reads as 'office', anything that is no value as 'auto'.
// Checked against core/test/fixtures/deck/driverrule.json (core/test/deck/gen-driverrule.js <lib/driverrule.js>).
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <string>

namespace deck { namespace core { namespace driverrule {

using json = nlohmann::json;

extern const char* const MODES[4];   // auto, office, phone, remote
extern const char* const KINDS[3];   // office, phone, remote

// what is known about one session right now
struct Who { bool phone = false; bool remote = false; };   // a phone is attached to it; a remote desk shows it on a visible tab

// a driver value from a file or a client -> one of MODES, or "" when it is not one ('desk' -> 'office')
std::string mode(const json& v);
// the kind of screen that drives this session
std::string driverOf(const json& m, const Who& who);
// a size from a screen of kind `from` reaches the pty only when that kind drives
bool acceptsSize(const json& m, const Who& who, const std::string& from);
// what the office renders a session at: null when the office drives, else the pty's size
json officeMirror(const json& m, const Who& who, const json& ptySize);
// the same for the remote desk, for a session it shows: null when it drives, null for one it does not show
json remoteMirror(const json& m, const Who& who, const json& ptySize);
// the phone page mirrors exactly under 'office' and 'remote'
bool phoneMirrors(const json& m);
// R10: the OFFICE is read-only on a session another screen drives; a phone's and a remote desk's typing never is
bool inputLocked(const json& m, const Who& who, const std::string& from);
// why the office's typing was refused: 'phone' | 'remote' (the setting), 'phone-attached' | 'remote-showing' (auto); "" = not refused
std::string refusedReason(const json& m, const Who& who);
// the button, the same on every screen (`me`: the kind of the screen it is on)
std::string click(const json& m, const std::string& me);
std::string look(const json& m, const std::string& me);    // 'mine' | 'other' | 'auto'

}}}  // namespace deck::core::driverrule
