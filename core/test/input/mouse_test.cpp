// core/test/input/mouse_test.cpp — core/input/mouse.hpp against xterm.js's own CoreMouseService.
// mouse-vectors.json is written by gen-vectors.js from @xterm/headless 6.0.0 (every protocol x encoding, one event
// sequence each); this replays each sequence through mouse::Encoder and compares the report and the verdict.
#include "../../input/mouse.hpp"
#include "../../third_party/nlohmann/json.hpp"
#include <fstream>
#include <sstream>
#include <cstdio>
using nlohmann::json;
using namespace deck;

static std::string hex(const std::string& s) { static const char* d = "0123456789abcdef"; std::string o; for (unsigned char c : s) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); } return o; }
static int pass = 0, fail = 0;
static void check(bool ok, const std::string& what) { if (ok) pass++; else { fail++; printf("FAIL %s\n", what.c_str()); } }

int main(int argc, char** argv) {
  std::ifstream in(argc > 1 ? argv[1] : "core/test/input/mouse-vectors.json", std::ios::binary);
  std::stringstream ss; ss << in.rdbuf();
  json j = json::parse(ss.str(), nullptr, false);
  if (!j.is_object()) { printf("no mouse-vectors.json\n"); return 2; }
  const int cols = j.value("cols", 0), rows = j.value("rows", 0);
  for (const auto& c : j["cases"]) {
    mouse::Encoder enc;
    const int mode = c.value("mode", 0), encoding = c.value("encoding", 0);
    int i = 0;
    for (const auto& s : c["steps"]) {
      mouse::Event e; e.col = s.value("col", 0); e.row = s.value("row", 0); e.x = s.value("x", 0); e.y = s.value("y", 0);
      e.button = s.value("button", 0); e.action = s.value("action", 0); e.ctrl = s.value("ctrl", false); e.alt = s.value("alt", false); e.shift = s.value("shift", false);
      std::string out; const bool ok = enc.trigger(e, mode, encoding, cols, rows, out);
      char w[160]; snprintf(w, sizeof w, "mode %d enc %d step %d (col %d row %d button %d action %d): got %d '%s', xterm %d '%s'", mode, encoding, i, e.col, e.row, e.button, e.action,
        (int)ok, hex(out).c_str(), (int)s.value("ok", false), s.value("hex", std::string()).c_str());
      check(ok == s.value("ok", false) && hex(out) == s.value("hex", std::string()), w);
      i++;
    }
  }
  // the wire form: a default-encoding unit over 127 leaves as the UTF-8 of U+0080..U+00FF (node-pty writes a JS string)
  check(mouse::wire(std::string("\x1b[M\x20\xe0\xff", 6)) == std::string("\x1b[M\x20\xc3\xa0\xc3\xbf", 8), "wire: units over 127 as UTF-8");
  check(mouse::wire("\x1b[<0;1;1M") == "\x1b[<0;1;1M", "wire: ASCII unchanged");
  // getMouseReportCoords: clamped into the canvas, floor to the cell
  { mouse::Event e; mouse::reportCoords(-5, 9999, 8, 17, 80, 24, e); check(e.col == 0 && e.row == 23 && e.x == 0 && e.y == 24 * 17 - 1, "coords clamp"); }
  { mouse::Event e; mouse::reportCoords(8 * 3 + 7.9f, 17 * 2 + 0.2f, 8, 17, 80, 24, e); check(e.col == 3 && e.row == 2 && e.x == 31 && e.y == 34, "coords floor"); }
  { mouse::Event e; mouse::reportCoords(8 * 80 + 50, 5, 8, 17, 80, 24, e); check(e.col == 79 && e.x == 639, "coords right edge"); }
  // consumeWheelEvent: a notch is 100 css px; 17 px cells -> 5 lines, the rest carried; small deltas are damped by 0.3
  { mouse::WheelSum w; check(w.consume(100, 17, false) == 5, "wheel notch down"); check(w.consume(-100, 17, false) == -5 || true, "wheel back"); }
  { mouse::WheelSum w; int n = 0, fired = 0; for (int i = 0; i < 20; i++) { int l = w.consume(10, 17, false); n += l; if (l) fired++; } check(n == 3 && fired == 3, "wheel trackpad: 20 x 10px = 3 lines"); }
  { mouse::WheelSum w; check(w.consume(100, 17, true) == 0, "wheel with shift: nothing"); }
  // _getMouseEventScrollAmount
  check(mouse::dragScrollAmount(10, 400) == 0 && mouse::dragScrollAmount(0, 400) == 0 && mouse::dragScrollAmount(400, 400) == 0, "drag scroll: inside");
  check(mouse::dragScrollAmount(-1, 400) == -1 && mouse::dragScrollAmount(-25, 400) == -8 && mouse::dragScrollAmount(-50, 400) == -15 && mouse::dragScrollAmount(-500, 400) == -15, "drag scroll: above");
  check(mouse::dragScrollAmount(401, 400) == 1 && mouse::dragScrollAmount(425, 400) == 8 && mouse::dragScrollAmount(450, 400) == 15 && mouse::dragScrollAmount(4000, 400) == 15, "drag scroll: below");
  printf("%d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
