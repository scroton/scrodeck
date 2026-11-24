// core/proto/frames.hpp — the core <-> shell message frames of core/PROTOCOL.md, as one codec.
//
// Every frame is a JSON object with a tag `t` (lib/phone.js is the server that speaks them today,
// renderer/desk/bridge.js the client). This header is the single place a native shell builds or reads
// one, so the wire words live in one file: a shell calls frames::input(id, bytes) and never spells
// "kind":"key" itself. Pure: no sockets, no OS. Header-only over the vendored JSON library.
#ifndef CLAUDE_DECK_CORE_PROTO_FRAMES_HPP
#define CLAUDE_DECK_CORE_PROTO_FRAMES_HPP

#include "../third_party/nlohmann/json.hpp"
#include <cstdio>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace deck::proto {

using json = nlohmann::json;

// ---------------------------------------------------------------- shell -> core
// `rid` 0 = fire and forget (no reply is sent)
inline json call(int rid, const std::string& fn, json args = json::array()) {
  if (!args.is_array()) args = json::array({ args });
  return { { "t", "call" }, { "rid", rid }, { "fn", fn }, { "args", args } };
}
inline json attach(const std::string& id, bool since = false) { return { { "t", "attach" }, { "id", id }, { "since", since } }; }
inline json detach(const std::string& id) { return { { "t", "detach" }, { "id", id } }; }
inline json detachAll() { return { { "t", "detach" } }; }
// R8: a remote sends real keystrokes and nothing else, so what a shell sends is kind 'key' (the one exception
// is wheel() below)
inline json input(const std::string& id, const std::string& bytes) { return { { "t", "input" }, { "id", id }, { "d", bytes }, { "kind", "key" } }; }
// R8 narrowed for the WHEEL (2026-11-18): exactly one SGR wheel report, and only while the deck's meta for the
// tile says mouseOn && sgrMouse. Marked kind 'wheel', as renderer/desk/bridge.js marks it. The caller decides
// whether it may go; this only refuses to build anything that is not that one shape (null = send nothing).
inline bool wheelReport(const std::string& d) {
  int code = 0, col = 0, row = 0, used = 0;
  if (sscanf(d.c_str(), "\x1b[<%d;%d;%dM%n", &code, &col, &row, &used) != 3 || used != (int)d.size()) return false;
  for (size_t i = 3; i + 1 < d.size(); i++) if (!((d[i] >= '0' && d[i] <= '9') || d[i] == ';')) return false;
  const int base = code & ~28;                                     // xterm's modifier bits: 4 shift, 8 meta, 16 ctrl
  return (base == 64 || base == 65) && code >= 0;
}
inline json wheel(const std::string& id, const std::string& report) {
  if (!wheelReport(report)) return json();
  return { { "t", "input" }, { "id", id }, { "d", report }, { "kind", "wheel" } };
}
// R8 narrowed for a CLICK (2026-11-19, from the remote: "clicks in the remote don't work, like to tell it how
// well claude is doing, or the yes/no questions"): exactly one SGR LEFT-button press (final M) or release (final m),
// with xterm's modifier bits only, at a cell (1-based). The same gate as the wheel (the caller: the deck's meta says
// mouseOn && sgrMouse, the tile awake). Marked kind 'click', as renderer/desk/bridge.js marks it. A drag, motion,
// the middle or right button, X10 and focus reports stay synthesised and are never sent.
inline bool clickReport(const std::string& d) {
  int code = 0, col = 0, row = 0, used = 0; char fin = 0;
  if (sscanf(d.c_str(), "\x1b[<%d;%d;%d%c%n", &code, &col, &row, &fin, &used) != 4 || used != (int)d.size()) return false;
  if (fin != 'M' && fin != 'm') return false;
  for (size_t i = 3; i + 1 < d.size(); i++) if (!((d[i] >= '0' && d[i] <= '9') || d[i] == ';')) return false;
  return code >= 0 && (code & ~28) == 0 && col >= 1 && row >= 1;
}
inline json click(const std::string& id, const std::string& report) {
  if (!clickReport(report)) return json();
  return { { "t", "input" }, { "id", id }, { "d", report }, { "kind", "click" } };
}
inline json resize(const std::string& id, int cols, int rows) { return { { "t", "resize" }, { "id", id }, { "cols", cols }, { "rows", rows } }; }
inline json shown(const std::vector<std::string>& ids, bool hidden) { return { { "t", "shown" }, { "ids", ids }, { "hidden", hidden } }; }
inline json layout(const std::vector<std::pair<std::string, std::vector<int>>>& map) {
  json o = json::object(); for (const auto& kv : map) o[kv.first] = kv.second;
  return { { "t", "layout" }, { "layout", o } };
}
inline json leaving(const std::string& id, bool on) { return { { "t", "leaving" }, { "id", id }, { "on", on } }; }

// The bytes a terminal emulator sends on its own and a remote must NOT forward (bridge.js SYNTHESISED):
// mouse reports (SGR and X10) and focus reports. True when `d` is nothing but those.
inline bool synthesised(const std::string& d) {
  if (d.empty()) return false;
  size_t i = 0;
  while (i < d.size()) {
    if (d.compare(i, 3, "\x1b[<") == 0) {                       // ESC [ < n ; n ; n M|m
      size_t j = i + 3; int fields = 0, digits = 0;
      for (; j < d.size(); j++) {
        char c = d[j];
        if (c >= '0' && c <= '9') { digits++; continue; }
        if (c == ';' && digits) { fields++; digits = 0; continue; }
        break;
      }
      if (j >= d.size() || fields != 2 || !digits || (d[j] != 'M' && d[j] != 'm')) return false;
      i = j + 1;
    } else if (d.compare(i, 3, "\x1b[M") == 0) {                // ESC [ M + three bytes
      if (i + 6 > d.size()) return false;
      i += 6;
    } else if (d.compare(i, 3, "\x1b[I") == 0 || d.compare(i, 3, "\x1b[O") == 0) {
      i += 3;
    } else return false;
  }
  return true;
}

// ---------------------------------------------------------------- core -> shell
enum class Kind { Unknown, Init, Meta, Data, Exit, Reset, Ok, Err, Event };

struct Frame {
  Kind kind = Kind::Unknown;
  json raw;
  // data / exit / reset
  std::string id() const { return str("id"); }
  std::string data() const { return str("d"); }
  bool replay() const { auto it = raw.find("replay"); return it != raw.end() && it->is_boolean() && it->get<bool>(); }
  int exitCode() const { auto it = raw.find("exitCode"); return it != raw.end() && it->is_number() ? it->get<int>() : 0; }
  // ok / err
  int rid() const { auto it = raw.find("rid"); return it != raw.end() && it->is_number() ? it->get<int>() : 0; }
  const json& result() const { static const json none; auto it = raw.find("result"); return it != raw.end() ? *it : none; }
  std::string error() const { return str("error"); }
  // meta / init
  const json& meta() const { static const json none; auto it = raw.find("meta"); return it != raw.end() ? *it : none; }
  // event
  std::string name() const { return str("name"); }
  const json& payload() const { static const json none; auto it = raw.find("payload"); return it != raw.end() ? *it : none; }

 private:
  std::string str(const char* k) const { auto it = raw.find(k); return it != raw.end() && it->is_string() ? it->get<std::string>() : std::string(); }
};

inline Frame parse(const std::string& text) {
  Frame f;
  f.raw = json::parse(text, nullptr, false);
  if (!f.raw.is_object()) { f.raw = json::object(); return f; }
  auto it = f.raw.find("t");
  const std::string t = it != f.raw.end() && it->is_string() ? it->get<std::string>() : "";
  f.kind = t == "init" ? Kind::Init : t == "meta" ? Kind::Meta : t == "data" ? Kind::Data : t == "exit" ? Kind::Exit
    : t == "reset" ? Kind::Reset : t == "ok" ? Kind::Ok : t == "err" ? Kind::Err : t == "event" ? Kind::Event : Kind::Unknown;
  return f;
}

// what goes on the wire: invalid UTF-8 (a pty can split a character) is replaced, never thrown on
inline std::string encode(const json& frame) { return frame.dump(-1, ' ', false, json::error_handler_t::replace); }

}  // namespace deck::proto

#endif  // CLAUDE_DECK_CORE_PROTO_FRAMES_HPP
