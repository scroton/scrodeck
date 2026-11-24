// core/vt/parser.cpp — the VT500 transition table, a literal port of VT500_TRANSITION_TABLE in
// @xterm/xterm src/common/parser/EscapeSequenceParser.ts (Paul Williams' DEC ANSI parser with xterm.js's edits).
// Built once; the order of the add() calls matters (later calls overwrite earlier ones) and is kept.
#include "vt_internal.hpp"

#include <initializer_list>

namespace deck { namespace vt { namespace detail {

namespace {
struct Table {
  uint8_t t[16 * 256];
  void add(int code, int state, int action, int next) { t[state << 8 | code] = (uint8_t)(action << 4 | next); }
  void range(int from, int to, int state, int action, int next) { for (int c = from; c < to; ++c) add(c, state, action, next); }
  void executables(int state, int action, int next) {
    range(0x00, 0x18, state, action, next);
    add(0x19, state, action, next);
    range(0x1c, 0x20, state, action, next);
  }
  void printables(int state, int action, int next) { range(0x20, 0x7f, state, action, next); }
  void many(std::initializer_list<int> codes, int state, int action, int next) { for (int c : codes) add(c, state, action, next); }

  Table() {
    const int NON_ASCII_PRINTABLE = 0xA0;
    for (auto& v : t) v = (uint8_t)(A_ERROR << 4 | GROUND);
    printables(GROUND, A_PRINT, GROUND);
    for (int state = GROUND; state <= DCS_PASSTHROUGH; ++state) {
      many({0x18, 0x1a, 0x99, 0x9a}, state, A_EXECUTE, GROUND);
      range(0x80, 0x90, state, A_EXECUTE, GROUND);
      range(0x90, 0x98, state, A_EXECUTE, GROUND);
      add(0x9c, state, A_IGNORE, GROUND);
      add(0x1b, state, A_CLEAR, ESCAPE);
      add(0x9d, state, A_OSC_START, OSC_STRING);
      many({0x98, 0x9e, 0x9f}, state, A_IGNORE, SOS_PM_APC_STRING);
      add(0x9b, state, A_CLEAR, CSI_ENTRY);
      add(0x90, state, A_CLEAR, DCS_ENTRY);
    }
    executables(GROUND, A_EXECUTE, GROUND);
    executables(ESCAPE, A_EXECUTE, ESCAPE);
    add(0x7f, ESCAPE, A_IGNORE, ESCAPE);
    executables(OSC_STRING, A_IGNORE, OSC_STRING);
    executables(CSI_ENTRY, A_EXECUTE, CSI_ENTRY);
    add(0x7f, CSI_ENTRY, A_IGNORE, CSI_ENTRY);
    executables(CSI_PARAM, A_EXECUTE, CSI_PARAM);
    add(0x7f, CSI_PARAM, A_IGNORE, CSI_PARAM);
    executables(CSI_IGNORE, A_EXECUTE, CSI_IGNORE);
    executables(CSI_INTERMEDIATE, A_EXECUTE, CSI_INTERMEDIATE);
    add(0x7f, CSI_INTERMEDIATE, A_IGNORE, CSI_INTERMEDIATE);
    executables(ESCAPE_INTERMEDIATE, A_EXECUTE, ESCAPE_INTERMEDIATE);
    add(0x7f, ESCAPE_INTERMEDIATE, A_IGNORE, ESCAPE_INTERMEDIATE);
    // osc
    add(0x5d, ESCAPE, A_OSC_START, OSC_STRING);
    printables(OSC_STRING, A_OSC_PUT, OSC_STRING);
    add(0x7f, OSC_STRING, A_OSC_PUT, OSC_STRING);
    many({0x9c, 0x1b, 0x18, 0x1a, 0x07}, OSC_STRING, A_OSC_END, GROUND);
    range(0x1c, 0x20, OSC_STRING, A_IGNORE, OSC_STRING);
    // sos/pm/apc does nothing
    many({0x58, 0x5e, 0x5f}, ESCAPE, A_IGNORE, SOS_PM_APC_STRING);
    printables(SOS_PM_APC_STRING, A_IGNORE, SOS_PM_APC_STRING);
    executables(SOS_PM_APC_STRING, A_IGNORE, SOS_PM_APC_STRING);
    add(0x9c, SOS_PM_APC_STRING, A_IGNORE, GROUND);
    add(0x7f, SOS_PM_APC_STRING, A_IGNORE, SOS_PM_APC_STRING);
    // csi entries
    add(0x5b, ESCAPE, A_CLEAR, CSI_ENTRY);
    range(0x40, 0x7f, CSI_ENTRY, A_CSI_DISPATCH, GROUND);
    range(0x30, 0x3c, CSI_ENTRY, A_PARAM, CSI_PARAM);
    many({0x3c, 0x3d, 0x3e, 0x3f}, CSI_ENTRY, A_COLLECT, CSI_PARAM);
    range(0x30, 0x3c, CSI_PARAM, A_PARAM, CSI_PARAM);
    range(0x40, 0x7f, CSI_PARAM, A_CSI_DISPATCH, GROUND);
    many({0x3c, 0x3d, 0x3e, 0x3f}, CSI_PARAM, A_IGNORE, CSI_IGNORE);
    range(0x20, 0x40, CSI_IGNORE, A_IGNORE, CSI_IGNORE);
    add(0x7f, CSI_IGNORE, A_IGNORE, CSI_IGNORE);
    range(0x40, 0x7f, CSI_IGNORE, A_IGNORE, GROUND);
    range(0x20, 0x30, CSI_ENTRY, A_COLLECT, CSI_INTERMEDIATE);
    range(0x20, 0x30, CSI_INTERMEDIATE, A_COLLECT, CSI_INTERMEDIATE);
    range(0x30, 0x40, CSI_INTERMEDIATE, A_IGNORE, CSI_IGNORE);
    range(0x40, 0x7f, CSI_INTERMEDIATE, A_CSI_DISPATCH, GROUND);
    range(0x20, 0x30, CSI_PARAM, A_COLLECT, CSI_INTERMEDIATE);
    // esc_intermediate
    range(0x20, 0x30, ESCAPE, A_COLLECT, ESCAPE_INTERMEDIATE);
    range(0x20, 0x30, ESCAPE_INTERMEDIATE, A_COLLECT, ESCAPE_INTERMEDIATE);
    range(0x30, 0x7f, ESCAPE_INTERMEDIATE, A_ESC_DISPATCH, GROUND);
    range(0x30, 0x50, ESCAPE, A_ESC_DISPATCH, GROUND);
    range(0x51, 0x58, ESCAPE, A_ESC_DISPATCH, GROUND);
    many({0x59, 0x5a, 0x5c}, ESCAPE, A_ESC_DISPATCH, GROUND);
    range(0x60, 0x7f, ESCAPE, A_ESC_DISPATCH, GROUND);
    // dcs entry
    add(0x50, ESCAPE, A_CLEAR, DCS_ENTRY);
    executables(DCS_ENTRY, A_IGNORE, DCS_ENTRY);
    add(0x7f, DCS_ENTRY, A_IGNORE, DCS_ENTRY);
    range(0x1c, 0x20, DCS_ENTRY, A_IGNORE, DCS_ENTRY);
    range(0x20, 0x30, DCS_ENTRY, A_COLLECT, DCS_INTERMEDIATE);
    range(0x30, 0x3c, DCS_ENTRY, A_PARAM, DCS_PARAM);
    many({0x3c, 0x3d, 0x3e, 0x3f}, DCS_ENTRY, A_COLLECT, DCS_PARAM);
    executables(DCS_IGNORE, A_IGNORE, DCS_IGNORE);
    range(0x20, 0x80, DCS_IGNORE, A_IGNORE, DCS_IGNORE);
    range(0x1c, 0x20, DCS_IGNORE, A_IGNORE, DCS_IGNORE);
    executables(DCS_PARAM, A_IGNORE, DCS_PARAM);
    add(0x7f, DCS_PARAM, A_IGNORE, DCS_PARAM);
    range(0x1c, 0x20, DCS_PARAM, A_IGNORE, DCS_PARAM);
    range(0x30, 0x3c, DCS_PARAM, A_PARAM, DCS_PARAM);
    many({0x3c, 0x3d, 0x3e, 0x3f}, DCS_PARAM, A_IGNORE, DCS_IGNORE);
    range(0x20, 0x30, DCS_PARAM, A_COLLECT, DCS_INTERMEDIATE);
    executables(DCS_INTERMEDIATE, A_IGNORE, DCS_INTERMEDIATE);
    add(0x7f, DCS_INTERMEDIATE, A_IGNORE, DCS_INTERMEDIATE);
    range(0x1c, 0x20, DCS_INTERMEDIATE, A_IGNORE, DCS_INTERMEDIATE);
    range(0x20, 0x30, DCS_INTERMEDIATE, A_COLLECT, DCS_INTERMEDIATE);
    range(0x30, 0x40, DCS_INTERMEDIATE, A_IGNORE, DCS_IGNORE);
    range(0x40, 0x7f, DCS_INTERMEDIATE, A_DCS_HOOK, DCS_PASSTHROUGH);
    range(0x40, 0x7f, DCS_PARAM, A_DCS_HOOK, DCS_PASSTHROUGH);
    range(0x40, 0x7f, DCS_ENTRY, A_DCS_HOOK, DCS_PASSTHROUGH);
    executables(DCS_PASSTHROUGH, A_DCS_PUT, DCS_PASSTHROUGH);
    printables(DCS_PASSTHROUGH, A_DCS_PUT, DCS_PASSTHROUGH);
    add(0x7f, DCS_PASSTHROUGH, A_IGNORE, DCS_PASSTHROUGH);
    many({0x1b, 0x9c, 0x18, 0x1a}, DCS_PASSTHROUGH, A_DCS_UNHOOK, GROUND);
    // special handling of unicode chars
    add(NON_ASCII_PRINTABLE, GROUND, A_PRINT, GROUND);
    add(NON_ASCII_PRINTABLE, OSC_STRING, A_OSC_PUT, OSC_STRING);
    add(NON_ASCII_PRINTABLE, CSI_IGNORE, A_IGNORE, CSI_IGNORE);
    add(NON_ASCII_PRINTABLE, DCS_IGNORE, A_IGNORE, DCS_IGNORE);
    add(NON_ASCII_PRINTABLE, DCS_PASSTHROUGH, A_DCS_PUT, DCS_PASSTHROUGH);
  }
};
}  // namespace

const uint8_t* transitionTable() {
  static const Table table;
  return table.t;
}

}}}  // namespace deck::vt::detail
