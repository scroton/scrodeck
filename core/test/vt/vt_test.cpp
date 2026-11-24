// core/test/vt/vt_test.cpp — parity test for core/vt against xterm.js.
//
// For every core/test/fixtures/vt/<name>.bin with a <name>.expected.txt (written by oracle/gen.js from headless
// xterm.js), replay the bytes through deck::vt::Terminal three ways — whole, one byte at a time, and in random
// chunks — and compare the full state with what xterm.js ended with: every viewport and scrollback cell
// (text, width, colours, flags), wrapped flags, cursor, modes, title and the reply bytes.
//
//   vt_test [fixtures-dir] [--only <substring>] [--bench] [--verbose]
//   vt_test --stress [rounds]      no oracle: sequence + byte soup + resizes, looking for crashes
// Set VT_TRACE=1 to print the engine's internal counters after every op of a fixture.
// Exit code 0 = everything matched.
#include "../../vt/terminal.hpp"
#include "../../vt/selection.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace deck::vt;
namespace fs = std::filesystem;

namespace {

struct Op { bool isResize; long long n; int cols, rows; };

struct Expected {
  int initCols = 80, initRows = 24, scrollback = 10000;
  std::vector<Op> ops;
  int cols = 0, rows = 0, cursorX = 0, cursorY = 0;
  std::string modes, title, replies;
  int scrollbackLen = 0;
  std::vector<std::string> viewport, scrollbackLines;
};

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string unhex(const std::string& h) {
  if (h == "-") return std::string();
  std::string out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back((char)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return out;
}

bool loadExpected(const fs::path& p, Expected& e) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ss(line);
    std::string key;
    ss >> key;
    if (key == "init") ss >> e.initCols >> e.initRows >> e.scrollback;
    else if (key == "op") {
      std::string kind;
      ss >> kind;
      Op op{};
      if (kind == "feed") { op.isResize = false; ss >> op.n; }
      else { op.isResize = true; ss >> op.cols >> op.rows; }
      e.ops.push_back(op);
    }
    else if (key == "size") ss >> e.cols >> e.rows;
    else if (key == "cursor") ss >> e.cursorX >> e.cursorY;
    else if (key == "modes") e.modes = line.substr(6);
    else if (key == "title") { std::string h; ss >> h; e.title = unhex(h); }
    else if (key == "replies") { std::string h; ss >> h; e.replies = unhex(h); }
    else if (key == "scrollback") ss >> e.scrollbackLen;
    else if (key == "V") e.viewport.push_back(line.substr(2));
    else if (key == "S") e.scrollbackLines.push_back(line.substr(2));
  }
  return true;
}

// ---- the same token format oracle/gen.js writes
std::string colorTok(const Color& c) {
  char tmp[16];
  if (c.kind == Color::Default) return "d";
  snprintf(tmp, sizeof tmp, "%c%x", c.kind == Color::RGB ? 'r' : 'p', (unsigned)c.value);
  return tmp;
}

std::string cellTok(const Terminal& t, const Cell& c) {
  std::string s;
  const std::u32string text = t.cellText(c);
  char tmp[32];
  if (text.empty()) s = "-";
  else for (size_t i = 0; i < text.size(); ++i) { snprintf(tmp, sizeof tmp, "%s%x", i ? "+" : "", (unsigned)text[i]); s += tmp; }
  snprintf(tmp, sizeof tmp, ",%d,", (int)c.width);
  s += tmp;
  s += colorTok(c.fg);
  s += ",";
  s += colorTok(c.bg);
  snprintf(tmp, sizeof tmp, ",%x", (unsigned)c.flags);
  s += tmp;
  return s;
}

std::vector<std::string> cellToks(const Terminal& t, const Line& l, int cols) {
  std::vector<std::string> v;
  for (int x = 0; x < cols; ++x) v.push_back(x < (int)l.cells.size() ? cellTok(t, l.cells[(size_t)x]) : std::string("<missing>"));
  return v;
}

// expand "w 3*a,1,d,d,0 b,1,d,d,0" into wrapped flag + per-cell tokens
std::vector<std::string> expand(const std::string& line, bool& wrapped) {
  std::vector<std::string> v;
  std::istringstream ss(line);
  std::string tok;
  ss >> tok;
  wrapped = tok == "w";
  while (ss >> tok) {
    size_t star = tok.find('*');
    int n = 1;
    if (star != std::string::npos) { n = atoi(tok.substr(0, star).c_str()); tok = tok.substr(star + 1); }
    for (int i = 0; i < n; ++i) v.push_back(tok);
  }
  return v;
}

std::string modesString(const Terminal& t) {
  char tmp[512];
  snprintf(tmp, sizeof tmp,
    "alt=%d appCursor=%d appKeypad=%d bracketedPaste=%d mouse=%d mouseEnc=%d focus=%d cursorVisible=%d cursorShape=%d "
    "cursorBlink=%d origin=%d wraparound=%d insert=%d sync=%d scrollTop=%d scrollBottom=%d",
    t.altScreen() ? 1 : 0, t.appCursorKeys() ? 1 : 0, t.appKeypad() ? 1 : 0, t.bracketedPaste() ? 1 : 0, t.mouseMode(),
    t.mouseEncoding(), t.focusReporting() ? 1 : 0, t.cursorVisible() ? 1 : 0, t.cursorShape(), t.cursorBlink(),
    t.originMode() ? 1 : 0, t.autoWrap() ? 1 : 0, t.insertMode() ? 1 : 0, t.synchronizedOutput() ? 1 : 0,
    t.scrollTop(), t.scrollBottom());
  return tmp;
}

std::string printable(const std::string& s) {
  std::string o;
  char tmp[8];
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f && c != '\\') o.push_back((char)c);
    else { snprintf(tmp, sizeof tmp, "\\x%02x", c); o += tmp; }
  }
  return o;
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 2654435761u + 88172645463325252ull) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
};

enum Mode { WHOLE = 0, BYTES = 1, RANDOM = 2 };
const char* const MODE_NAME[] = {"whole", "1-byte", "random"};

bool sameCell(const Cell& a, const Cell& b) {
  return a.ch == b.ch && a.width == b.width && a.flags == b.flags && a.fg == b.fg && a.bg == b.bg && a.extra == b.extra;
}

// Damage soundness: every viewport row that differs from the snapshot must lie inside the reported span.
// verify() returns the number of rows that changed without being reported.
struct DamageCheck {
  std::vector<Line> snap;
  void take(const Terminal& t) {
    snap.resize((size_t)t.rows());
    for (int y = 0; y < t.rows(); ++y) snap[(size_t)y] = t.line(y);
  }
  int verify(Terminal& t) {
    int a = 0, b = -1, missed = 0;
    const bool any = t.takeDamage(a, b);
    if ((int)snap.size() == t.rows()) {
      for (int y = 0; y < t.rows(); ++y) {
        const Line& now = t.line(y);
        const Line& was = snap[(size_t)y];
        bool same = now.wrapped == was.wrapped && now.cells.size() == was.cells.size();
        for (size_t x = 0; same && x < now.cells.size(); ++x) same = sameCell(now.cells[x], was.cells[x]);
        if (!same && (!any || y < a || y > b)) missed++;
      }
    }
    take(t);
    return missed;
  }
};

int g_damageMissed = 0;

void feedRange(Terminal& t, const std::string& bin, size_t from, size_t to, Mode mode, Rng& rng, std::string& replies) {
  if (mode == WHOLE) { t.feed(bin.data() + from, to - from); replies += t.takeReplies(); return; }
  DamageCheck dmg;
  if (mode == RANDOM) { int a, b; t.takeDamage(a, b); dmg.take(t); }
  size_t pos = from;
  while (pos < to) {
    size_t n = 1;
    if (mode == RANDOM) {
      const uint32_t r = rng.next();
      n = (r % 8 == 0) ? 1 + (rng.next() % 4096) : 1 + (r >> 4) % 23;
    }
    if (pos + n > to) n = to - pos;
    t.feed(bin.data() + pos, n);
    if (mode == RANDOM) g_damageMissed += dmg.verify(t);
    if (rng.next() % 3 == 0) replies += t.takeReplies();
    pos += n;
  }
  replies += t.takeReplies();
}

// --stress: no oracle — sequence soup, byte soup and resizes down to 2x1, to shake out crashes. Build with
// -D_GLIBCXX_ASSERTIONS (build.ps1 -Stress) so an out-of-range index aborts instead of passing silently.
int stress(int rounds) {
  static const char* const PIECES[] = {
    "\x1b[", "\x1b]", "\x1bP", "\x1b_", "\x1b", "\x1b\\", "\x07", "\r", "\n", "\r\n", "\t", "\x08", "\x0e", "\x0f", "\x18", "\x9b", "\xc2\x9b",
    ";", ":", "?", ">", "!", " ", "$", "'", "\"", "#", "(", ")", "0", "1", "2", "3", "5", "9", "25", "47", "1049", "2004", "99999", "2147483647",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "P", "S", "T", "X", "Z", "@", "`", "a", "b", "c", "d", "e", "f", "g", "h", "l",
    "m", "n", "p", "q", "r", "s", "t", "u", "}", "~", "7", "8", "=", "c",
    "hello world ", "\xe4\xb8\xad\xe6\x96\x87", "e\xcc\x81", "\xf0\x9f\x98\x80", "\xe2\x80\x8d", "\xef\xb8\x8f", "\xff", "\xe4", "\xf0\x9f",
    "38;2;1;2;3m", "48:5:9m", "4:3m", "?1049h", "?1049l", "?6h", "?7l", "?7h", "4h", "4l", "2;5r", "r", "#8", "2J", "3J", "1J", "?47h", "?47l", "!p",
    "$qm\x1b\\", "8;;http://x\x07", "8;;\x07", "2;title\x07", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
  };
  const int NP = (int)(sizeof PIECES / sizeof PIECES[0]);
  long long fed = 0;
  for (int round = 0; round < rounds; ++round) {
    Rng rng((uint64_t)round * 7919 + 1);
    const int sb = (int)(rng.next() % 4 == 0 ? rng.next() % 5 : rng.next() % 300);
    Terminal t(2 + (int)(rng.next() % 60), 1 + (int)(rng.next() % 20), sb);
    for (int step = 0; step < 3000; ++step) {
      const uint32_t r = rng.next();
      if (r % 97 == 0) { t.resize(1 + (int)(rng.next() % 70), (int)(rng.next() % 24)); continue; }
      if (r % 1009 == 0) { t.reset(); continue; }
      std::string s;
      const int k = 1 + (int)(rng.next() % 6);
      for (int i = 0; i < k; ++i) {
        if (rng.next() % 11 == 0) s.push_back((char)(rng.next() & 0xFF));
        else s += PIECES[rng.next() % (uint32_t)NP];
      }
      t.feed(s.data(), s.size());
      fed += (long long)s.size();
      // touch everything a renderer touches
      int a, b;
      t.takeDamage(a, b);
      size_t sum = 0;
      for (int y = 0; y < t.rows(); ++y) {
        const Line& l = t.line(y);
        if ((int)l.cells.size() < t.cols()) { printf("stress: round %d: a viewport line is shorter than cols\n", round); return 1; }
        sum += t.cellText(l.cells[(size_t)(t.cols() - 1)]).size();
      }
      if (t.scrollbackSize() > 0) sum += t.scrollbackLine(t.scrollbackSize() - 1).cells.size() + t.scrollbackLine(0).cells.size();
      if (t.cursorX() < 0 || t.cursorX() >= t.cols() || t.cursorY() < 0 || t.cursorY() >= t.rows()) { printf("stress: round %d: cursor off screen\n", round); return 1; }
      if (t.scrollbackSize() > sb + 64) { printf("stress: round %d: scrollback %d over its limit %d\n", round, t.scrollbackSize(), sb); return 1; }
      (void)sum;
      t.takeReplies();
    }
  }
  printf("stress: %d rounds, %lld bytes, no crash\n", rounds, fed);
  return 0;
}

// returns the number of mismatches reported (0 = pass)
int runOne(const std::string& name, const std::string& bin, const Expected& e, Mode mode, bool verbose) {
  Terminal t(e.initCols, e.initRows, e.scrollback);
  Rng rng(0x9E3779B97F4A7C15ull ^ std::hash<std::string>()(name));
  std::string replies;
  size_t pos = 0;
  const bool trace = getenv("VT_TRACE") && mode == WHOLE;      // VT_TRACE=1: print the engine's counters after every op
  for (const Op& op : e.ops) {
    if (op.isResize) {
      t.resize(op.cols, op.rows);
      if (trace) printf("resize %d,%d %s\n", op.cols, op.rows, t.debugState().c_str());
    } else {
      size_t to = pos + (size_t)op.n;
      if (to > bin.size()) to = bin.size();
      feedRange(t, bin, pos, to, mode, rng, replies);
      pos = to;
      if (trace) printf("feed %d %s\n", (int)pos, t.debugState().c_str());
    }
  }
  feedRange(t, bin, pos, bin.size(), mode, rng, replies);
  if (trace) printf("end %s\n", t.debugState().c_str());

  int bad = 0;
  auto fail = [&](const std::string& what) {
    if (bad < 12 || verbose) printf("    [%s/%s] %s\n", name.c_str(), MODE_NAME[mode], what.c_str());
    bad++;
  };
  char tmp[256];
  if (t.cols() != e.cols || t.rows() != e.rows) {
    snprintf(tmp, sizeof tmp, "size %dx%d, expected %dx%d", t.cols(), t.rows(), e.cols, e.rows);
    fail(tmp);
    return bad;
  }
  if (t.cursorXRaw() != e.cursorX || t.cursorY() != e.cursorY) {
    snprintf(tmp, sizeof tmp, "cursor %d,%d expected %d,%d", t.cursorXRaw(), t.cursorY(), e.cursorX, e.cursorY);
    fail(tmp);
  }
  const std::string modes = modesString(t);
  if (modes != e.modes) fail("modes\n      got      " + modes + "\n      expected " + e.modes);
  if (t.title() != e.title) fail("title got \"" + printable(t.title()) + "\" expected \"" + printable(e.title) + "\"");
  if (replies != e.replies) fail("replies got \"" + printable(replies) + "\" expected \"" + printable(e.replies) + "\"");
  if (t.scrollbackSize() != e.scrollbackLen) {
    snprintf(tmp, sizeof tmp, "scrollback length %d expected %d", t.scrollbackSize(), e.scrollbackLen);
    fail(tmp);
  }
  auto cmpLine = [&](const char* what, int idx, const Line& l, const std::string& exp) {
    bool wrapped = false;
    const std::vector<std::string> want = expand(exp, wrapped);
    const std::vector<std::string> got = cellToks(t, l, e.cols);
    if (l.wrapped != wrapped) {
      snprintf(tmp, sizeof tmp, "%s %d wrapped=%d expected %d", what, idx, l.wrapped ? 1 : 0, wrapped ? 1 : 0);
      fail(tmp);
    }
    for (size_t x = 0; x < want.size() && x < got.size(); ++x) {
      if (want[x] != got[x]) {
        snprintf(tmp, sizeof tmp, "%s %d col %d: got ", what, idx, (int)x);
        fail(std::string(tmp) + got[x] + " expected " + want[x] + "   (text,width,fg,bg,flags)");
        break;                                         // one report per row
      }
    }
    if (want.size() != got.size()) { snprintf(tmp, sizeof tmp, "%s %d: %d cells expected %d", what, idx, (int)got.size(), (int)want.size()); fail(tmp); }
  };
  for (int y = 0; y < e.rows && y < (int)e.viewport.size(); ++y) cmpLine("row", y, t.line(y), e.viewport[(size_t)y]);
  const int sb = std::min(t.scrollbackSize(), (int)e.scrollbackLines.size());
  for (int i = 0; i < sb; ++i) cmpLine("scrollback", i, t.scrollbackLine(i), e.scrollbackLines[(size_t)i]);
  if (bad > 12 && !verbose) printf("    [%s/%s] ... %d mismatches in total\n", name.c_str(), MODE_NAME[mode], bad);

  // damage sanity: after a take, nothing is dirty until something is fed
  int a, b;
  t.takeDamage(a, b);
  if (t.takeDamage(a, b)) fail("takeDamage reported damage twice without a feed in between");
  // a resize between two feeds is not covered by the per-chunk check in feedRange(); a resize must report all rows
  if (!e.ops.empty()) {
    t.resize(t.cols() + 1, t.rows());
    if (!t.takeDamage(a, b) || a != 0 || b != t.rows() - 1) fail("resize did not report full damage");
  }
  return bad;
}

void bench() {
  // plain text: 80-column lines of ASCII with CRLF, the bulk of what a pty carries
  std::string chunk;
  for (int i = 0; i < 2000; ++i) {
    for (int c = 0; c < 78; ++c) chunk.push_back((char)('!' + (i * 7 + c) % 90));
    chunk += "\r\n";
  }
  const size_t total = 100u * 1024 * 1024;
  Terminal t(120, 30);
  const auto t0 = std::chrono::steady_clock::now();
  size_t fed = 0;
  while (fed < total) { t.feed(chunk.data(), chunk.size()); fed += chunk.size(); }
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("bench: plain text %.0f MB in %.2f s = %.0f MB/s (scrollback %d lines)\n", fed / 1048576.0, sec, fed / 1048576.0 / sec, t.scrollbackSize());

  // an Ink-like redraw loop: cursor up, erase line, truecolor SGR, text
  std::string frame;
  for (int r = 0; r < 20; ++r) frame += "\x1b[2K\x1b[38;2;200;120;40m\xe2\x94\x82\x1b[0m line \x1b[1mbold\x1b[22m \x1b[48;5;236mcell\x1b[49m text text text\r\n";
  frame += "\x1b[20A";
  Terminal t2(120, 30);
  const auto t1 = std::chrono::steady_clock::now();
  fed = 0;
  while (fed < total / 2) { t2.feed(frame.data(), frame.size()); fed += frame.size(); }
  const double sec2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  printf("bench: redraw stream %.0f MB in %.2f s = %.0f MB/s\n", fed / 1048576.0, sec2, fed / 1048576.0 / sec2);
}

// Things the oracle cannot show (headless xterm.js has no theme, no host): checked against what the BROWSER
// xterm.js does, read from its source (CoreBrowserTerminal._handleColorEvent, XParseColor.toRgbString).
int selfChecks() {
  int bad = 0;
  auto expect = [&](bool ok, const char* what) { if (!ok) { printf("    [self] %s\n", what); bad++; } };
  auto feed = [](Terminal& t, const std::string& s) { t.feed(s.data(), s.size()); };
  {
    Terminal t(20, 5);
    feed(t, "\x1b]10;?\x07\x1b]11;?\x1b\\\x1b]4;1;?\x07");
    expect(t.takeReplies().empty(), "colour queries without a theme must stay unanswered (headless xterm.js)");
    t.setThemeColor(256, 0xD4D4D4);
    t.setThemeColor(257, 0x1E1E1E);
    t.setThemeColor(258, 0xFFFFFF);
    t.setThemeColor(1, 0xCD3131);
    feed(t, "\x1b]10;?\x07");
    expect(t.takeReplies() == "\x1b]10;rgb:d4d4/d4d4/d4d4\x1b\\", "OSC 10 ? reply");
    feed(t, "\x1b]11;?\x1b\\");
    expect(t.takeReplies() == "\x1b]11;rgb:1e1e/1e1e/1e1e\x1b\\", "OSC 11 ? reply");
    feed(t, "\x1b]10;?;?;?\x07");
    expect(t.takeReplies() == "\x1b]10;rgb:d4d4/d4d4/d4d4\x1b\\\x1b]11;rgb:1e1e/1e1e/1e1e\x1b\\\x1b]12;rgb:ffff/ffff/ffff\x1b\\", "OSC 10 ?;?;? walks fg, bg, cursor");
    feed(t, "\x1b]4;1;?;2;?;999;?;1;rgb:00/00/00\x07");
    expect(t.takeReplies() == "\x1b]4;1;rgb:cdcd/3131/3131\x1b\\", "OSC 4 ? reply (set colours only, valid indexes only)");
  }
  {
    Terminal t(10, 3);
    feed(t, "\x1b[5 q");
    expect(t.cursorStyle() == 5 && t.cursorShape() == 3 && t.cursorBlink() == 1, "DECSCUSR 5 = blinking bar");
    feed(t, "\x1b[2 q");
    expect(t.cursorStyle() == 2, "DECSCUSR 2 = steady block");
    feed(t, "\x1b[0 q");
    expect(t.cursorStyle() == 0 && t.cursorBlink() == -1, "DECSCUSR 0 = default");
    expect(!t.takeBell(), "no bell yet");
    feed(t, "\x07");
    expect(t.takeBell() && !t.takeBell(), "BEL is reported once");
    feed(t, "0123456789");
    expect(t.cursorXRaw() == 10 && t.cursorX() == 9, "pending wrap: raw x = cols, drawn x = cols - 1");
    feed(t, "e\xcc\x81");
    const Cell& c = t.line(1).cells[0];
    expect(t.cellText(c) == std::u32string(U"e\u0301") && c.ch == U'e' && c.extra != 0, "cellText = base + combining mark");
    expect(t.cellText(t.line(2).cells[0]).empty(), "cellText of an empty cell is empty");
    int a, b;
    t.takeDamage(a, b);
    t.resize(10, 3);
    expect(!t.takeDamage(a, b), "same-size resize is a no-op");
    t.feed(nullptr, 0);
    t.feed("", 0);
    feed(t, "\x1b]2;title\x07\x1b[?1049h\x1b[?25l\x1b[31m");
    t.reset();
    expect(!t.altScreen() && t.scrollbackSize() == 0 && t.cursorXRaw() == 0 && t.cursorY() == 0, "reset() homes and leaves the alt screen");
    expect(t.cellText(t.line(0).cells[0]).empty(), "reset() clears the screen");
    t.resize(0, 0);
    expect(t.cols() == 2 && t.rows() == 1, "resize clamps to 2 x 1");
    (void)t.scrollbackLine(5);
    (void)t.line(99);
  }
  {
    // scrollback never exceeds its limit
    Terminal t(10, 3, 7);
    for (int i = 0; i < 100; ++i) feed(t, "line\r\n");
    expect(t.scrollbackSize() == 7, "scrollback is capped at its limit");
  }
  return bad;
}

// ---- selection text (Terminal::selectionText): hand cases, then every range of selection-vectors.txt, which
// oracle/gen-selection.js wrote from headless xterm.js (its translateToString per row, SelectionService's join).
int g_selChecks = 0;
int selectionSelf() {
  int bad = 0;
  auto expect = [&](bool ok, const char* what) { g_selChecks++; if (!ok) { printf("    [selection] %s\n", what); bad++; } };
  auto feed = [](Terminal& t, const std::string& s) { t.feed(s.data(), s.size()); };
  { Terminal t(10, 4);
    feed(t, "hello");
    expect(t.selectionText(0, 0, 10, 0) == "hello", "trailing empty cells are dropped");
    expect(t.selectionText(1, 0, 4, 0) == "ell" && t.selectionText(4, 0, 5, 0) == "o", "start inclusive, end exclusive");
    expect(t.selectionText(3, 0, 3, 0).empty() && t.selectionText(5, 0, 2, 0).empty() && t.selectionText(0, 2, 5, 1).empty(), "an empty or backwards range is nothing");
    expect(t.selectionText(-5, 0, 99, 0) == "hello" && t.selectionText(0, -3, 10, 0) == "\n\n\nhello" && t.selectionText(0, 3, 10, 9) == "\n\n\n\n\n\n", "columns are clamped; rows outside the buffer are empty rows");
    expect(t.bufferLines() == 4 && &t.bufferLine(0) == &t.line(0), "bufferLines / bufferLine: no scrollback yet"); }
  { Terminal t(10, 4);
    feed(t, "ab   \r\nc");
    expect(t.selectionText(0, 0, 10, 0) == "ab   ", "a space the app WROTE is text (xterm keeps it)");
    expect(t.selectionText(0, 0, 10, 1) == "ab   \nc" && t.selectionText(0, 0, 10, 1, "\r\n") == "ab   \r\nc", "rows are joined with the newline the shell names");
    expect(t.selectionText32(0, 0, 10, 1, U"\r\n") == std::u32string(U"ab   \r\nc"), "the same as code points"); }
  { Terminal t(10, 4);
    feed(t, "0123456789ABCDE\r\nx");
    expect(t.line(1).wrapped && !t.line(2).wrapped, "(the 11th character wrapped)");
    expect(t.selectionText(0, 0, 10, 2) == "0123456789ABCDE\nx", "a wrapped row continues the row above with nothing between");
    expect(t.selectionText(8, 0, 2, 1) == "89AB", "…also when the selection starts and ends mid row"); }
  { Terminal t(10, 4);
    feed(t, "a\x1b[5C" "b\r\n\r\nc");
    expect(t.selectionText(0, 0, 10, 2) == "a     b\n\nc", "an empty cell inside the text is a space; an empty row is an empty line"); }
  { Terminal t(10, 4);
    feed(t, "\xe4\xb8\xad\xe6\x96\x87x e\xcc\x81 \xc2\xa0z");
    expect(t.selectionText(0, 0, 10, 0) == "\xe4\xb8\xad\xe6\x96\x87x e\xcc\x81  z", "wide characters once, combining marks kept, a no-break space becomes a space");
    expect(t.selectionText(0, 0, 3, 0) == "\xe4\xb8\xad\xe6\x96\x87" && t.selectionText(1, 0, 4, 0) == " \xe6\x96\x87", "a wide character is taken whole from its first cell; its second cell alone is a blank"); }
  { Terminal t(10, 3);
    for (int i = 0; i < 6; ++i) feed(t, "line" + std::to_string(i) + (i < 5 ? "\r\n" : ""));
    expect(t.scrollbackSize() == 3 && t.bufferLines() == 6 && t.bufferTop() == 3, "(three lines scrolled off)");
    expect(t.selectionText(0, 0, 10, 5) == "line0\nline1\nline2\nline3\nline4\nline5", "lines are absolute: scrollback first, then the viewport");
    expect(t.selectionText(4, 2, 4, 3) == "2\nline", "…across the scrollback / viewport edge");
    feed(t, "\x1b[?1049h\x1b[Halt");
    expect(t.bufferLines() == 3 && t.bufferTop() == 0 && t.selectionText(0, 0, 10, 2) == "alt\n\n", "the alt buffer has no scrollback: line 0 is its top row");
    feed(t, "\x1b[?1049l");
    expect(t.selectionText(0, 5, 10, 5) == "line5", "…and the normal buffer is back as it was"); }

  // ---- selection lines and FOLLOWING the text (core/vt/selection.hpp; first written in windows/src/termview.hpp)
  { Terminal t(10, 3);
    for (int i = 0; i < 5; ++i) feed(t, "n" + std::to_string(i) + "\r\n");
    expect(selectionLines(t) == t.bufferLines() && selectionLineOffset(t) == 0 && selectionText32At(t, 0, 2, 10, 2, U"\n") == U"n2", "selection lines: the normal buffer from its oldest line");
    feed(t, "\x1b[?1049h\x1b[Hab   \r\nab");
    expect(selectionLines(t) == 3 && selectionLineOffset(t) == t.bufferTop() && selectionText32At(t, 0, 0, 10, 99, U"\n") == U"ab   \nab\n", "selection lines: the alt buffer from the viewport's top; the end is clamped to the last line");
    expect(followRowText(t, 0) == U"ab" && followRowText(t, 1) == U"ab" && followRowText(t, -1).empty() && followRowText(t, 3).empty(), "followRowText: written and empty trailing spaces alike; nothing outside"); }
  { Terminal t(20, 6);
    auto screen = [](int from, bool clear) { std::string s = clear ? "\x1b[2J\x1b[H" : "\x1b[H"; for (int i = 0; i < 6; i++) { char b[32]; snprintf(b, sizeof b, "row%02d text", from + i); s += b; if (i < 5) s += "\r\n"; } return s; };
    feed(t, "\x1b[?1049h"); feed(t, screen(1, true));
    SelectionFollow f; int shift = 9;
    f.snap(t, 3, 2);
    expect(f.active() && f.top() == 2 && f.follow(t, shift) == SelectionFollow::Stayed && shift == 0, "follow: rows 2..3 (given either way round), nothing moved: stayed");
    feed(t, screen(3, true));
    expect(f.follow(t, shift) == SelectionFollow::Moved && shift == -2 && f.top() == 0, "follow: the app redrew its screen two rows on: the selection moves with its text");
    feed(t, screen(3, false));
    expect(f.follow(t, shift) == SelectionFollow::Stayed && shift == 0, "follow: redrawn in place: stayed");
    feed(t, screen(4, true));
    expect(f.follow(t, shift) == SelectionFollow::Gone && !f.active(), "follow: its first row is gone from the screen: gone (never a highlight over other text)"); }
  { Terminal t(10, 4); SelectionFollow f; int shift = 0;
    f.snap(t, 1, 2);
    expect(f.active() && f.follow(t, shift) == SelectionFollow::Gone && !f.active(), "follow: nothing but blank rows: nothing to recognise, gone");
    expect(f.follow(t, shift) == SelectionFollow::Stayed, "follow: an empty follower is Stayed (nothing to follow)"); }
  { Terminal t(10, 3, 5);
    for (int i = 0; i < 8; ++i) feed(t, std::string(i ? "\r\n" : "") + "L" + std::to_string(i));
    SelectionFollow f; int shift = 0;
    f.snap(t, 2, 2);
    feed(t, "\r\nL8");
    expect(t.bufferLines() == 8 && f.follow(t, shift) == SelectionFollow::Moved && shift == -1 && followRowText(t, f.top()) == U"L2", "follow: a full scrollback trims its top: the normal buffer's rows move up, the selection with them"); }
  { Terminal t(10, 4, 3000);
    for (int i = 0; i < 2100; ++i) feed(t, "x\r\n");
    SelectionFollow f; f.snap(t, 0, SelectionFollow::kMaxRows);
    expect(!f.active(), "follow: a selection over more than 2000 rows is not followed");
    f.snap(t, 0, SelectionFollow::kMaxRows - 1);
    expect(f.active(), "follow: …2000 rows are"); }
  // ---- a DRAG on the alt screen while the app scrolls by redrawing (core/vt/selection.hpp DragSpill, 2026-11-24):
  // 12 rows, rows 0-8 move ("Lnn text"), rows 9-11 a footer that stays put
  { Terminal t(24, 12);
    auto frame = [](int k) { std::string s = "\x1b[2J\x1b[H"; for (int i = 0; i < 9; i++) { char b[32]; snprintf(b, sizeof b, "L%02d text\r\n", k + i + 1); s += b; } return s + "-- footer --\r\n> type\r\nstatus"; };
    feed(t, "\x1b[?1049h"); feed(t, frame(0));
    DragSpill d; d.start(t, 0, 4);                                         // the anchor: L05, col 0
    expect(d.active() && d.where() == 0 && d.anchorLine() == 4, "drag: starts on the alt screen with its anchor on screen");
    feed(t, frame(0));
    expect(d.afterFeed(t) == DragSpill::Same && d.anchorLine() == 4, "drag: redrawn the same: same");
    feed(t, frame(2));
    expect(d.afterFeed(t) == DragSpill::Moved && d.where() == 0 && d.anchorLine() == 2, "drag: the app scrolls 2: the anchor follows its row (4 -> 2), the footer does not count");
    feed(t, frame(5));
    expect(d.afterFeed(t) == DragSpill::Moved && d.where() < 0 && d.anchorLine() == 0 && d.kept() == 1, "drag: 3 more: the anchor's row leaves the top, it is kept; the junction is the block's top");
    feed(t, frame(8));
    expect(d.afterFeed(t) == DragSpill::Moved && d.kept() == 4, "drag: 3 more: every row that leaves is kept");
    expect(d.spilledText(t, 24, 2, U"\n") == U"L05 text\nL06 text\nL07 text\nL08 text\nL09 text\nL10 text\nL11 text", "drag: the text is the kept rows, then the screen up to the head, exactly");
    feed(t, frame(6));
    expect(d.afterFeed(t) == DragSpill::Moved && d.where() < 0 && d.kept() == 2, "drag: 2 come back: checked against the kept ones and taken off the store");
    feed(t, frame(3));
    expect(d.afterFeed(t) == DragSpill::Moved && d.where() == 0 && d.anchorLine() == 1, "drag: the anchor's row comes back: on screen again (row 1)");
    feed(t, "\x1b[2;1HL05 rewritten");
    expect(d.afterFeed(t) == DragSpill::Cut && d.cut() && !d.active(), "drag: the anchor's row rewritten in place: CUT (no guess)");
    expect(d.afterFeed(t) == DragSpill::Same, "drag: a cut drag watches nothing more"); }
  { Terminal t(24, 12);
    auto frame = [](int k) { std::string s = "\x1b[2J\x1b[H"; for (int i = 0; i < 9; i++) { char b[32]; snprintf(b, sizeof b, "L%02d text\r\n", k + i + 1); s += b; } return s + "-- footer --\r\n> type\r\nstatus"; };
    feed(t, "\x1b[?1049h"); feed(t, frame(10));
    DragSpill d; d.start(t, 3, 6);                                         // the anchor: L17, col 3 (a drag upwards)
    feed(t, frame(6));
    expect(d.afterFeed(t) == DragSpill::Moved && d.where() > 0 && d.anchorLine() == 8 && d.kept() == 2, "drag up: the app scrolls up 4: the anchor leaves the BOTTOM of the moving block (row 8, not the footer); L16 and it are kept");
    expect(d.spilledText(t, 0, 7, U"\n") == U"L14 text\nL15 text\nL16 text\nL17", "drag up: the screen from the head, then the kept rows, the anchor's row up to its column");
    feed(t, "\x1b[2J\x1b[Hsomething else");
    expect(d.afterFeed(t) == DragSpill::Cut, "drag up: a clear: CUT"); }
  { Terminal t(24, 12);
    auto frame = [](int k) { std::string s = "\x1b[2J\x1b[H"; for (int i = 0; i < 9; i++) { char b[32]; snprintf(b, sizeof b, "L%02d text\r\n", k + i + 1); s += b; } return s + "-- footer --\r\n> type\r\nstatus"; };
    feed(t, "\x1b[?1049h"); feed(t, frame(0));
    DragSpill d; d.start(t, 0, 4);                                         // L05
    const std::string f = frame(3); const size_t half = f.size() / 2;
    feed(t, "\x1b[?2026h" + f.substr(0, half));                            // a frame in two pieces, inside DEC 2026
    expect(d.afterFeed(t) == DragSpill::Same && d.anchorLine() == 4, "drag: half a frame (synchronized output on): not judged yet");
    feed(t, f.substr(half) + "\x1b[?2026l");
    expect(d.afterFeed(t) == DragSpill::Moved && d.anchorLine() == 1 && d.where() == 0, "drag: the frame complete: judged whole (L05 at row 1), no cut"); }
  { Terminal t(24, 6); DragSpill d; d.start(t, 0, 2);
    expect(!d.active(), "drag: the normal buffer has its own scrollback: no DragSpill"); }
  // ---- a COPY from the alternate screen (core/vt/selection.hpp copyText32At / tidyCopy, 2026-11-24): 10 columns
  { Terminal t(10, 6);
    feed(t, "ab        cd\r\n0123456789abc");                                  // normal buffer: padding wrapped, a word cut
    expect(copyText32At(t, 0, 0, 10, 3, U"\n") == selectionText32At(t, 0, 0, 10, 3, U"\n") && copyText32At(t, 0, 0, 10, 3, U"\n") == U"ab        cd\n0123456789abc",
           "copy: the normal buffer copies exactly as xterm (written spaces kept, wrapped rows joined)");
    feed(t, "\x1b[?1049h\x1b[H");
    feed(t, "ab        cd");                                                   // row 0 padded to the edge, row 1 wrapped
    expect(rowWrapped(t, 1) && copyText32At(t, 0, 0, 10, 1, U"\n") == U"ab\ncd", "copy (alt): a row padded to the edge ends its line; the padding is not text");
    feed(t, "\x1b[2J\x1b[H0123456789  in");                                // runs to the edge, the next row indented
    expect(copyText32At(t, 0, 0, 10, 1, U"\n") == U"0123456789\n  in", "copy (alt): a row that runs to the edge, then one that starts with a space: two lines");
    feed(t, "\x1b[2J\x1b[Hhttps://example.com/a/b");                        // a URL cut by the edge, twice
    expect(copyText32At(t, 0, 0, 10, 2, U"\n") == U"https://example.com/a/b", "copy (alt): a word cut by the edge (non-space on both sides) is joined, as xterm");
    feed(t, "\x1b[2J\x1b[Hab                  cd");                         // a padded row, an all-blank row, a row
    expect(copyText32At(t, 0, 0, 10, 2, U"\n") == U"ab\n\ncd", "copy (alt): an all-blank row is an empty line");
    feed(t, "\x1b[2J\x1b[Hone  \r\ntwo   ");
    expect(copyText32At(t, 0, 0, 5, 1, U"\n") == U"one\ntwo", "copy (alt): written trailing spaces dropped, the last line's too");
    expect(copyText32At(t, 1, 0, 2, 0, U"\n") == U"n", "copy (alt): a part of one row");
    expect(copyText32At(t, 0, 1, 0, 1, U"\n").empty(), "copy (alt): an empty range is empty"); }
  return bad;
}

Terminal* replayWhole(const std::string& bin, const Expected& e) {
  Terminal* t = new Terminal(e.initCols, e.initRows, e.scrollback);
  size_t pos = 0;
  for (const Op& op : e.ops) {
    if (op.isResize) t->resize(op.cols, op.rows);
    else { size_t to = std::min(bin.size(), pos + (size_t)op.n); t->feed(bin.data() + pos, to - pos); pos = to; }
  }
  t->feed(bin.data() + pos, bin.size() - pos);
  return t;
}
// returns the ranges that differ; `ranges` counts what was compared (0 = the vectors are missing)
int selectionVectors(const std::string& fixturesDir, const fs::path& vectors, const std::string& only, int& ranges) {
  std::ifstream f(vectors, std::ios::binary);
  ranges = 0;
  if (!f) return 0;
  int bad = 0;
  std::string line, name;
  std::unique_ptr<Terminal> t;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, 2, "F ") == 0) {
      name = line.substr(2); t.reset();
      if (!only.empty() && name.find(only) == std::string::npos) continue;
      Expected e;
      if (!loadExpected(fs::path(fixturesDir) / (name + ".expected.txt"), e)) continue;
      t.reset(replayWhole(readFile(fs::path(fixturesDir) / (name + ".bin")), e));
    } else if (line.compare(0, 2, "S ") == 0 && t) {
      std::istringstream ss(line.substr(2));
      int x0, y0, x1, y1; std::string h;
      ss >> x0 >> y0 >> x1 >> y1 >> h;
      const std::string want = unhex(h), got = t->selectionText(x0, y0, x1, y1);
      ranges++;
      if (got != want) { if (++bad <= 12) printf("    [selection/%s] (%d,%d)-(%d,%d): got \"%s\" expected \"%s\"\n", name.c_str(), x0, y0, x1, y1, printable(got).substr(0, 300).c_str(), printable(want).substr(0, 300).c_str()); }
    }
  }
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "core/test/fixtures/vt";
  std::string only;
  bool doBench = false, verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--stress") { setvbuf(stdout, nullptr, _IONBF, 0); return stress(i + 1 < argc ? atoi(argv[i + 1]) : 300); }
    if (a == "--bench") doBench = true;
    else if (a == "--verbose") verbose = true;
    else if (a == "--only" && i + 1 < argc) only = argv[++i];
    else dir = a;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);

  std::vector<fs::path> bins;
  std::error_code ec;
  for (const auto& ent : fs::directory_iterator(dir, ec)) {
    if (ent.path().extension() == ".bin") bins.push_back(ent.path());
  }
  if (ec || bins.empty()) { printf("vt_test: no fixtures in %s\n", dir.c_str()); return 2; }
  std::sort(bins.begin(), bins.end());

  int fixtures = 0, passed = 0, failedRuns = 0, runs = 0, missing = 0;
  for (const auto& binPath : bins) {
    const std::string name = binPath.stem().string();
    if (!only.empty() && name.find(only) == std::string::npos) continue;
    fs::path expPath = binPath;
    expPath.replace_extension(".expected.txt");
    Expected e;
    if (!loadExpected(expPath, e)) { printf("MISSING %s (run node core/test/vt/oracle/gen.js)\n", expPath.string().c_str()); missing++; continue; }
    const std::string bin = readFile(binPath);
    fixtures++;
    bool ok = true;
    for (int m = 0; m < 3; ++m) {
      runs++;
      if (runOne(name, bin, e, (Mode)m, verbose) != 0) { ok = false; failedRuns++; }
    }
    if (ok) passed++;
    if (!ok || verbose) printf("%s %s\n", ok ? "ok  " : "FAIL", name.c_str());
  }
  printf("vt_test: %d/%d fixtures match xterm.js in all three chunkings (%d runs, %d failed%s)\n",
         passed, fixtures, runs, failedRuns, missing ? ", expected files missing" : "");
  const int selfBad = only.empty() ? selfChecks() : 0;
  printf("vt_test: self checks (colour reports, cursor style, bell, reset, clamps): %s\n", selfBad ? "FAILED" : "ok");
  if (g_damageMissed) printf("vt_test: DAMAGE: %d changed rows were not inside the span takeDamage() reported\n", g_damageMissed);
  else printf("vt_test: damage tracking covered every changed row (checked after each random chunk)\n");
  const int selBad = only.empty() ? selectionSelf() : 0;
  int selRanges = 0;
  const int selVecBad = selectionVectors(dir, fs::path(dir) / ".." / ".." / "vt" / "selection-vectors.txt", only, selRanges);
  printf("vt_test: selection text + following it: %d hand cases %s; %d ranges against xterm.js, %d differ%s\n", g_selChecks, selBad ? "FAILED" : "ok", selRanges, selVecBad,
         selRanges ? "" : "  (NO VECTORS: node core/test/vt/oracle/gen-selection.js)");
  if (doBench) bench();
  return (passed == fixtures && !missing && fixtures > 0 && !g_damageMissed && !selfBad && !selBad && !selVecBad && (selRanges > 0 || !only.empty())) ? 0 : 1;
}
