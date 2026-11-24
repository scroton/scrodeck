// windows/src/sidebar.cpp — the deck's sidebar, drawn from the same `meta` the Electron renderer gets.
//
// A port of what renderer/index.html + style.css + js/30-tiles.js, 35-groups.js, 40-new-tile.js, 46-search.js,
// 49-asks.js (the asks TILE only), 80-sidebar.js and 82-restart.js put on the screen. Every number below is a
// number from style.css; the comment beside a block names the css rule or the JS function it ports, so the two
// can be read side by side. Nothing here is a redesign. Notes, gaps and the API this file would like from
// gfx.hpp / ui.hpp: windows/SIDEBAR-NOTES.md.
#include <sstream>
#include <memory>
#include <regex>
#include "ui.hpp"
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace deck { namespace ui {

namespace {

// ---------------------------------------------------------------- css variables (:root in style.css)
const Rgba kSb = Rgba::hex(0x0f0f0f), kTile = Rgba::hex(0x171717), kBox = Rgba::hex(0x242424);
const Rgba kFg = Rgba::hex(0xececec), kDim = Rgba::hex(0x9a9a9a), kFaint = Rgba::hex(0x6a6a6a);
const Rgba kGreen = Rgba::hex(0x3ecf5a), kTrack = Rgba::hex(0x3c3c3c), kBtn = Rgba::hex(0x262626);
const Rgba kBtnBorder = Rgba::hex(0x383838), kDanger = Rgba::hex(0xc94a4a), kAmber = Rgba::hex(0xf0b429);
const Rgba kAsk = Rgba::hex(0x6aa9ff), kAskInk = Rgba::hex(0x0b1220), kAskDim = Rgba::hex(0x4a6c99);
const Rgba kCloud = Rgba::hex(0x8fb8e8), kNone(0, 0, 0, 0);
const float kPadX = 4, kPadY = 6;   // --pad-x, --pad-y

// ---------------------------------------------------------------- json, read the way the JS reads it
const json& nul() { static const json j; return j; }
const json& O(const json& j, const char* k) { if (!j.is_object()) return nul(); auto it = j.find(k); return it == j.end() ? nul() : *it; }
const json& O(const json& j, const std::string& k) { return O(j, k.c_str()); }
std::string S(const json& j, const char* k) { const json& v = O(j, k); return v.is_string() ? v.get<std::string>() : std::string(); }
bool isNum(const json& j, const char* k) { return O(j, k).is_number(); }
double N(const json& j, const char* k, double def = 0) { const json& v = O(j, k); return v.is_number() ? v.get<double>() : def; }
// JS truthiness
bool truthy(const json& v) {
  if (v.is_null()) return false;
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number()) return v.get<double>() != 0;
  if (v.is_string()) return !v.get_ref<const std::string&>().empty();
  return true;
}
bool truthy(const json& j, const char* k) { return truthy(O(j, k)); }
std::wstring W(const std::string& s) { return widen(s); }
std::string lower(const std::string& s) {
  std::wstring w = widen(s);
  if (!w.empty()) CharLowerBuffW(&w[0], (DWORD)w.size());
  return narrow(w);
}
std::wstring oneLine(std::wstring s) { for (auto& c : s) if (c == L'\n' || c == L'\r' || c == L'\t') c = L' '; return s; }

// ---------------------------------------------------------------- time (10-format.js)
// an ISO string ("2026-11-18T19:00:00.123Z", "+00:00" offsets) or epoch ms -> epoch ms
bool parseTime(const json& v, double& ms) {
  if (v.is_number()) { ms = v.get<double>(); return true; }
  if (!v.is_string()) return false;
  const std::string& s = v.get_ref<const std::string&>();
  int Y, M, D, h = 0, m = 0; double sec = 0;
  int n = sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &m, &sec);
  if (n < 3) return false;
  // days from civil (Howard Hinnant)
  int y = Y - (M <= 2); int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  double days = (double)era * 146097 + (double)doe - 719468;
  double t = days * 86400000.0 + h * 3600000.0 + m * 60000.0 + sec * 1000.0;
  size_t tz = s.find_first_of("Zz+-", 11);
  if (tz == std::string::npos) {           // no zone: local time, as Date.parse reads a date-time without one
    struct tm lt{}; lt.tm_year = Y - 1900; lt.tm_mon = M - 1; lt.tm_mday = D; lt.tm_hour = h; lt.tm_min = m; lt.tm_sec = (int)sec; lt.tm_isdst = -1;
    time_t tt = mktime(&lt); if (tt == (time_t)-1) return false;
    ms = (double)tt * 1000.0 + (sec - std::floor(sec)) * 1000.0; return true;
  }
  if (s[tz] == '+' || s[tz] == '-') {
    int oh = 0, om = 0; sscanf(s.c_str() + tz + 1, "%d:%d", &oh, &om);
    if (oh > 99) { om = oh % 100; oh /= 100; }   // +0530
    t -= (s[tz] == '+' ? 1 : -1) * (oh * 3600000.0 + om * 60000.0);
  }
  ms = t; return true;
}
struct tm localTm(double ms) { time_t t = (time_t)std::floor(ms / 1000.0); struct tm r{}; localtime_s(&r, &t); return r; }
std::wstring fmt(const wchar_t* f, ...) { wchar_t b[256]; va_list ap; va_start(ap, f); _vsnwprintf(b, 255, f, ap); va_end(ap); b[255] = 0; return b; }

std::wstring fmtClock(double ms) { struct tm d = localTm(ms); int h = d.tm_hour % 12; if (!h) h = 12; return fmt(L"%d:%02d", h, d.tm_min); }
std::wstring fmtAge(double ms) {
  double s = std::max(0.0, std::floor(ms / 1000.0));
  if (s < 60) return L"now";
  double m = std::floor(s / 60); if (m < 60) return fmt(L"%dm", (int)m);
  double h = std::floor(m / 60); if (h < 24) return fmt(L"%dh", (int)h);
  return fmt(L"%dd", (int)std::floor(h / 24));
}
const wchar_t* kDash = L"—";
std::wstring fmtResetTime(const json& iso) {
  double ms; if (!truthy(iso) || !parseTime(iso, ms)) return kDash;
  struct tm d = localTm(ms); int h = d.tm_hour;
  return fmt(L"%d:%02d%lc", h % 12 ? h % 12 : 12, d.tm_min, h >= 12 ? L'p' : L'a');
}
std::wstring fmtResetDay(const json& iso) {
  double ms; if (!truthy(iso) || !parseTime(iso, ms)) return kDash;
  struct tm d = localTm(ms); return fmt(L"%d.%d", d.tm_mon + 1, d.tm_mday);
}
std::wstring fmtUntilHours(const json& iso, double now) {
  double t; if (!truthy(iso) || !parseTime(iso, t)) return kDash;
  double ms = t - now; if (ms <= 0) return L"now";
  int m = (int)std::ceil(ms / 60000.0), h = m / 60;
  return h >= 1 ? fmt(L"%dh %02dm", h, m % 60) : fmt(L"%dm", m);
}
std::wstring fmtUntilDays(const json& iso, double now) {
  double t; if (!truthy(iso) || !parseTime(iso, t)) return kDash;
  double ms = t - now; if (ms < 24 * 3600000.0) return fmtUntilHours(iso, now);
  return fmt(L"%dd", (int)std::ceil(ms / 86400000.0));
}
// toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) in en-US
std::wstring fmtLocaleTime(double ms) { struct tm d = localTm(ms); int h = d.tm_hour % 12; if (!h) h = 12; return fmt(L"%d:%02d %ls", h, d.tm_min, d.tm_hour >= 12 ? L"PM" : L"AM"); }
std::wstring fmtLocaleString(double ms) { struct tm d = localTm(ms); int h = d.tm_hour % 12; if (!h) h = 12;
  return fmt(L"%d/%d/%d, %d:%02d:%02d %ls", d.tm_mon + 1, d.tm_mday, d.tm_year + 1900, h, d.tm_min, d.tm_sec, d.tm_hour >= 12 ? L"PM" : L"AM"); }
// 30-tiles.js fmtHeldTime: 06:30 today, 'tomorrow 06:30', 'tue 06:30' further out
std::wstring fmtHeldTime(double ms, double now) {
  struct tm d = localTm(ms), n = localTm(now), tmw = localTm(now + 86400000.0);
  std::wstring hm = fmt(L"%02d:%02d", d.tm_hour, d.tm_min);
  auto same = [](const struct tm& a, const struct tm& b) { return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday; };
  if (same(d, n)) return hm;
  if (same(d, tmw)) return L"tomorrow " + hm;
  static const wchar_t* days[] = { L"sun", L"mon", L"tue", L"wed", L"thu", L"fri", L"sat" };
  return std::wstring(days[d.tm_wday % 7]) + L" " + hm;
}
std::wstring HHMM(double m) { int mm = (int)m; return fmt(L"%02d:%02d", (((mm % 1440) + 1440) % 1440) / 60, ((mm % 60) + 60) % 60); }

// ---------------------------------------------------------------- colour ramps (30-tiles.js hexToHsl / rampAt / rampColor)
void hexToHsl(const std::string& hex, double out[3]) {
  out[0] = 0; out[1] = 0; out[2] = 50;
  const char* p = hex.c_str(); if (*p == '#') ++p;
  if (strlen(p) != 6 || strspn(p, "0123456789abcdefABCDEF") != 6) return;
  unsigned v = (unsigned)strtoul(p, nullptr, 16);
  double r = ((v >> 16) & 255) / 255.0, g = ((v >> 8) & 255) / 255.0, b = (v & 255) / 255.0;
  double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), l = (mx + mn) / 2;
  if (mx == mn) { out[2] = l * 100; return; }
  double d = mx - mn, s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  double h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
  out[0] = h * 60; out[1] = s * 100; out[2] = l * 100;
}
Rgba hslToRgb(double h, double s, double l) {
  s /= 100; l /= 100; h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
  auto f = [&](double n) { double k = std::fmod(n + h / 30.0, 12.0); double a = s * std::min(l, 1 - l); return l - a * std::max(-1.0, std::min(std::min(k - 3, 9 - k), 1.0)); };
  return Rgba((float)f(0), (float)f(8), (float)f(4));
}
Rgba rampAt(const std::string& from, const std::string& to, double t) {
  double a[3], b[3]; hexToHsl(from, a); hexToHsl(to, b);
  double dh = b[0] - a[0]; if (dh > 180) dh -= 360; if (dh < -180) dh += 360;
  double h = std::fmod(a[0] + dh * t + 360, 360.0), s = a[1] + (b[1] - a[1]) * t, l = a[2] + (b[2] - a[2]) * t;
  return hslToRgb(std::round(h), std::round(s), std::round(l));   // the JS writes hsl() with toFixed(0)
}
struct BarCfg { bool show; std::string name, from, to; };
BarCfg defaultBar(const std::string& k) {
  if (k == "session") return { true, "Session (5 h)", "#22c55e", "#ef4444" };
  if (k == "week") return { true, "Week", "#22c55e", "#ef4444" };
  if (k == "fable") return { true, "Fable (if applicable)", "#22d3ee", "#d946ef" };
  return { true, "Context", "#fbbf24", "#f97316" };
}

const std::vector<std::pair<float, Rgba>>& rainbow() {
  static const std::vector<std::pair<float, Rgba>> r = { { 0, Rgba::hex(0xff5f6d) }, { .25f, Rgba::hex(0xffc371) }, { .5f, Rgba::hex(0xc4f26a) }, { .75f, Rgba::hex(0x4fd1ff) }, { 1, Rgba::hex(0xb388ff) } };
  return r;
}
const std::vector<std::pair<float, Rgba>>& purple() {
  static const std::vector<std::pair<float, Rgba>> r = { { 0, Rgba::hex(0xc084fc) }, { 1, Rgba::hex(0x6d28d9) } };
  return r;
}

// ---------------------------------------------------------------- svg path data -> a Direct2D geometry
// M m L l H h V v C c S s A a Z z, with implicit repeats and the compact number forms the icons use ("-2.2 2.2.7 4.3",
// "0 1 1-2.8 2.8"). Enough for every icon in index.html, the pin and the cloud.
Com<ID2D1PathGeometry> svgGeom(ID2D1Factory* fac, const char* d, bool filled) {
  Com<ID2D1PathGeometry> geo;
  if (FAILED(fac->CreatePathGeometry(geo.put()))) return geo;
  Com<ID2D1GeometrySink> sink;
  if (FAILED(geo->Open(sink.put()))) { geo.reset(); return geo; }
  sink->SetFillMode(D2D1_FILL_MODE_WINDING);
  const char* p = d; char cmd = 0; float cx = 0, cy = 0, sx = 0, sy = 0; bool open = false;
  float qx = 0, qy = 0; bool curved = false;      // the last cubic's second control point (S / s mirror it), and whether the last segment was one
  auto ws = [&] { while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p; };
  auto num = [&](float& v) -> bool { ws(); char* e = nullptr; v = strtof(p, &e); if (e == p) return false; p = e; return true; };
  auto flag = [&](bool& v) -> bool { ws(); if (*p != '0' && *p != '1') return false; v = *p == '1'; ++p; return true; };
  auto begin = [&](float x, float y) {
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure(D2D1::Point2F(x, y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    open = true; sx = cx = x; sy = cy = y;
  };
  auto lineTo = [&](float x, float y) {
    if (!open) begin(cx, cy);
    if (x == cx && y == cy) x += 0.01f;      // "v0" with a round cap is a dot: give Direct2D something to cap
    sink->AddLine(D2D1::Point2F(x, y)); cx = x; cy = y;
  };
  for (;;) {
    ws(); if (!*p) break;
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
      cmd = *p++;
      if (cmd == 'Z' || cmd == 'z') { if (open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; } cx = sx; cy = sy; continue; }
    }
    float a = 0, b = 0; bool ok = true;
    const bool wasCurve = curved; curved = false;
    switch (cmd) {
      case 'C': case 'c': case 'S': case 's': {       // the eye of the driver button: "M2.5 12s3.5-6.5 9.5-6.5 ... S2.5 12 2.5 12z"
        const bool rel = cmd == 'c' || cmd == 's', smooth = cmd == 'S' || cmd == 's';
        float x1 = wasCurve ? 2 * cx - qx : cx, y1 = wasCurve ? 2 * cy - qy : cy, x2, y2, x, y;
        if (!smooth) { ok = num(x1) && num(y1); if (ok && rel) { x1 += cx; y1 += cy; } }
        ok = ok && num(x2) && num(y2) && num(x) && num(y);
        if (!ok) break;
        if (rel) { x2 += cx; y2 += cy; x += cx; y += cy; }
        if (!open) begin(cx, cy);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x, y)));
        qx = x2; qy = y2; cx = x; cy = y; curved = true; break;
      }
      case 'M': ok = num(a) && num(b); if (ok) { begin(a, b); cmd = 'L'; } break;
      case 'm': ok = num(a) && num(b); if (ok) { begin(cx + a, cy + b); cmd = 'l'; } break;
      case 'L': ok = num(a) && num(b); if (ok) lineTo(a, b); break;
      case 'l': ok = num(a) && num(b); if (ok) lineTo(cx + a, cy + b); break;
      case 'H': ok = num(a); if (ok) lineTo(a, cy); break;
      case 'h': ok = num(a); if (ok) lineTo(cx + a, cy); break;
      case 'V': ok = num(a); if (ok) lineTo(cx, a); break;
      case 'v': ok = num(a); if (ok) lineTo(cx, cy + a); break;
      case 'A': case 'a': {
        float rx, ry, rot, x, y; bool large, sweep;
        ok = num(rx) && num(ry) && num(rot) && flag(large) && flag(sweep) && num(x) && num(y);
        if (!ok) break;
        if (cmd == 'a') { x += cx; y += cy; }
        if (!open) begin(cx, cy);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(rx, ry), rot,
          sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        cx = x; cy = y; break;
      }
      default: ok = false;
    }
    if (!ok) break;
  }
  if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  return geo;
}

// An icon's geometry, kept: it depends on nothing but its path text (a string literal: its address is the key).
ID2D1PathGeometry* svgKept(ID2D1Factory* fac, const char* d, bool filled) {
  static std::map<std::pair<const char*, bool>, Com<ID2D1PathGeometry>> kept;
  auto it = kept.find({ d, filled });
  if (it != kept.end()) return it->second.p;
  Com<ID2D1PathGeometry> geo = svgGeom(fac, d, filled);
  ID2D1PathGeometry* p = geo.p; kept[{ d, filled }] = geo;
  return p;
}

// ---------------------------------------------------------------- the sidebar's text, kept between frames
// The Electron renderer keeps a tile's DOM and touches only what changed (30-tiles.js: `sig`, `actionSig`,
// `heldSig`, `markSig`). The same idea here, one level down: every measurement and every laid-out line the
// sidebar asks DirectWrite for is kept under ITS inputs — the text, the font, the width, the spacing, the
// alignment — so a frame whose inputs did not change (a hover, a scroll, a tick of the clock on sixty tiles of
// which one age changed) lays nothing out again. A layout is device-independent and is drawn exactly as a fresh
// one would be: the picture cannot differ (the 35 sidebar shots and --verify-scene check that it does not).
struct LineKept { Com<IDWriteTextLayout> lay; float baseline = 0; };
struct WrapKept { Com<IDWriteTextLayout> all, head, last; unsigned lines = 0; bool clampReady = false; };
struct TextKept {
  std::unordered_map<std::wstring, float> widths;
  std::unordered_map<std::wstring, LineKept> lines;
  std::unordered_map<std::wstring, WrapKept> wraps;
  // between frames only (a painter holds pointers into these while it draws)
  void trim() { if (widths.size() > 60000) widths.clear(); if (lines.size() > 12000) lines.clear(); if (wraps.size() > 6000) wraps.clear(); }
};
TextKept& textKept() { static TextKept k; return k; }
inline void keyFloat(std::wstring& k, float v) { uint32_t u; memcpy(&u, &v, 4); k.push_back((wchar_t)(1 + (u & 0x7fff))); k.push_back((wchar_t)(1 + ((u >> 15) & 0x7fff))); k.push_back((wchar_t)(1 + (u >> 30))); }
inline void keyFont(std::wstring& k, const Font& f) { keyFloat(k, f.size); k.push_back((wchar_t)(1 + f.weight + (f.italic ? 2000 : 0))); }

const char* kPinPath = "M9.5 1.5 14.5 6.5 12 7l-2.2 2.2.7 4.3L9 15l-3-3-4 4-1-1 4-4-3-3 1.5-1.5 4.3.7L10 5z";                     // 30-tiles.js PIN_SVG, viewBox 16
const char* kCloudPath = "M4.4 13.2h7.6a3.2 3.2 0 0 0 .55-6.35A4.4 4.4 0 0 0 4.06 6.3 3.45 3.45 0 0 0 4.4 13.2z";                  // 48-cloud.js CLOUD_SVG, viewBox 16
const char* kGearPath = "M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z";

struct AskMark { bool any = false; std::string kind; std::wstring text, title; };

}  // namespace

// ================================================================================================
struct SidebarPainter {
  Gfx& g; Sidebar& sb; const json& meta; const State& st; RectF area;
  const json& settings;
  double now = 0;
  bool sbRight = false, effortTag = true, effortColor = false;
  bool clipOn = false; RectF clip;
  bool onlyOn = false;                // Sidebar::drawAnim: one part is being drawn again by itself (nothing is recorded)
  Sidebar::AnimPart part{};           // the part being drawn by paint() just now (kind -1 = none): what animRegion() records
  float iconRot = 0;                  // the next tool icon, turned by this many degrees about its middle (css rs-turn)
  std::wstring fam; float ascR = 1.0791f, descR = 0.251f;
  Com<ID2D1StrokeStyle> ssRound, ssMiter, ssDash;
  std::string shell = "PowerShell";   // lib/platform.js shellCommand().name on Windows (state.platform.shellName)

  SidebarPainter(Gfx& g_, Sidebar& sb_, const json& m, const State& s, const RectF& a)
    : g(g_), sb(sb_), meta(m), st(s), area(a), settings(O(m, "settings")) {
    now = N(meta, "now") + (double)st.clockSkew;
    sbRight = S(settings, "sidebarSide") == "right";
    std::string es = S(settings, "effortStyle"); if (es.empty()) es = "tag";
    effortTag = es == "tag"; effortColor = es == "color";
    if (!st.shellName.empty()) shell = st.shellName;
    resolveFont(); buildFallback();
    { float dx = 96, dy = 96; g.rt->GetDpi(&dx, &dy); K = dx > 0 ? dx / 96.f : 1; }
    // the stroke styles belong to the factory, not to a target: made once
    static Com<ID2D1StrokeStyle> sRound, sMiter, sDash;
    if (!sRound) {
      g.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, sRound.put());
      g.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_MITER), nullptr, 0, sMiter.put());
      const float dashes[] = { 3, 2 };   // Blink: a 1px dashed border is 3 on, 2 off
      g.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0), dashes, 2, sDash.put());
    }
    ssRound = sRound; ssMiter = sMiter; ssDash = sDash;
  }

  // body { font-family: "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif }: the first that exists
  void resolveFont() {
    static std::wstring cachedFam; static float cachedAsc = 0, cachedDesc = 0;
    if (cachedFam.empty()) {
      cachedFam = L"Segoe UI"; cachedAsc = 1.0791f; cachedDesc = 0.251f;
      Com<IDWriteFontCollection> coll;
      if (SUCCEEDED(g.dw->GetSystemFontCollection(coll.put(), FALSE)) && coll) {
        for (const wchar_t* name : { L"Segoe UI Variable Text", L"Segoe UI" }) {
          UINT32 idx = 0; BOOL exists = FALSE;
          if (FAILED(coll->FindFamilyName(name, &idx, &exists)) || !exists) continue;
          cachedFam = name;
          Com<IDWriteFontFamily> ff; Com<IDWriteFont> font;
          if (SUCCEEDED(coll->GetFontFamily(idx, ff.put())) && ff &&
              SUCCEEDED(ff->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.put())) && font) {
            DWRITE_FONT_METRICS fm; font->GetMetrics(&fm);
            if (fm.designUnitsPerEm) { cachedAsc = (float)fm.ascent / fm.designUnitsPerEm; cachedDesc = (float)fm.descent / fm.designUnitsPerEm; }
          }
          break;
        }
      }
    }
    fam = cachedFam; ascR = cachedAsc; descR = cachedDesc;
  }

  // ---- font + line metrics. Blink rounds a font's ascent and descent to whole px; `line-height: normal` is their sum.
  // Blink lays out in PHYSICAL pixels (the device scale is a zoom), so every rounding below is done at the
  // target's scale K: at 100 % they are whole css px, at 150 % / 200 % thirds and halves, exactly as Chromium.
  Font F(float size, int weight = 400, bool italic = false) const { Font f; f.family = fam; f.size = size; f.weight = weight; f.italic = italic; return f; }
  float K = 1;
  float rnd(float v) const { return std::floor(v * K + 0.5f) / K; }
  float A(float s) const { return rnd(ascR * s); }
  float D(float s) const { return rnd(descR * s); }
  float LN(float s) const { return A(s) + D(s); }
  // where the baseline sits inside a line box of height L (the half-leading above the text is floored: NG CalculateLeadingSpace)
  float baseIn(float L, float s) const { return std::floor((L - LN(s)) / 2 * K) / K + A(s); }
  // a unitless line-height (1.25 × the font size) stays fractional (14px → 17.5): tile tops down the list land on
  // half pixels and are snapped only when painted, which is why nothing here rounds a tile's y
  float lhMul(float mult, float s) const { return mult * s; }
  // the same for a label centred in a button's box (centred, not led)
  float baseMid(float L, float s) const { return (L - LN(s)) / 2 + A(s); }
  Com<IDWriteTypography> tnumTy;
  // font-variant-numeric: tabular-nums (the clock, ages, bar labels, counts)
  void tabular(IDWriteTextLayout* lay, size_t len) {
    if (!tnumTy) { g.dw->CreateTypography(tnumTy.put()); if (tnumTy) { DWRITE_FONT_FEATURE ff = { DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 }; tnumTy->AddFontFeature(ff); } }
    if (tnumTy && lay) { DWRITE_TEXT_RANGE rg = { 0, (UINT32)len }; lay->SetTypography(tnumTy.p, rg); }
  }
  // Font fallback as Chromium does it on Windows (third_party/blink font_fallback_win.cc), where DirectWrite's own
  // would differ. Measured against headless Edge on this box (en-US): kana are drawn in Yu Gothic and Han in
  // Microsoft YaHei, where DirectWrite alone picks Yu Gothic UI, whose kana are narrower, so lines wrapped in other
  // places. Everything else falls through to the system's fallback. (Chromium chooses the Han font by locale.)
  Com<IDWriteFontFallback> fallback;
  void buildFallback() {
    static Com<IDWriteFontFallback> cached; static bool tried = false;
    if (!tried) {
      tried = true;
      Com<IDWriteFactory2> f2;
      if (SUCCEEDED(g.dw->QueryInterface(__uuidof(IDWriteFactory2), (void**)f2.put())) && f2) {
        Com<IDWriteFontFallbackBuilder> b; Com<IDWriteFontFallback> sys;
        if (SUCCEEDED(f2->CreateFontFallbackBuilder(b.put())) && b) {
          const DWRITE_UNICODE_RANGE kana[] = { { 0x3000, 0x30FF }, { 0x31F0, 0x31FF }, { 0xFF00, 0xFFEF } };
          const DWRITE_UNICODE_RANGE hanr[] = { { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xF900, 0xFAFF } };
          const wchar_t* kfams[] = { L"Yu Gothic" }; const wchar_t* hfams[] = { L"Microsoft YaHei" };
          b->AddMapping(kana, 3, kfams, 1, nullptr, nullptr, nullptr, 1.0f);
          b->AddMapping(hanr, 3, hfams, 1, nullptr, nullptr, nullptr, 1.0f);
          if (SUCCEEDED(f2->GetSystemFontFallback(sys.put())) && sys) b->AddMappings(sys.p);
          b->CreateFontFallback(cached.put());
        }
      }
    }
    fallback = cached;
  }
  // every text layout of the sidebar is made here
  Com<IDWriteTextLayout> mkLayout(const std::wstring& s, const Font& f, float w, float h, bool wrapOn, bool ell) {
    Com<IDWriteTextLayout> lay = g.layout(s, f, w, h, wrapOn, ell, 0);
    if (lay && fallback) {
      Com<IDWriteTextLayout2> l2;
      if (SUCCEEDED(lay->QueryInterface(__uuidof(IDWriteTextLayout2), (void**)l2.put())) && l2) l2->SetFontFallback(fallback.p);
    }
    return lay;
  }
  // letter-spacing (css adds it after every character); set `tracking` around the calls that need it
  float tracking = 0;
  void track(IDWriteTextLayout* lay, size_t len) {
    if (tracking == 0 || !lay) return;
    Com<IDWriteTextLayout1> l1;
    if (SUCCEEDED(lay->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)l1.put())) && l1) { DWRITE_TEXT_RANGE rg = { 0, (UINT32)len }; l1->SetCharacterSpacing(0, tracking, 0, rg); }
  }
  float tw(const std::wstring& s, const Font& f, bool tnum = false) {
    if (s.empty()) return 0;
    std::wstring k; k.reserve(s.size() + 10); keyFont(k, f); keyFloat(k, tracking); k.push_back(tnum ? L'\x2' : L'\x1'); k += s;
    auto& widths = textKept().widths;
    auto it = widths.find(k); if (it != widths.end()) return it->second;
    Com<IDWriteTextLayout> lay = mkLayout(s, f, 100000, 1000, false, false);
    if (!lay) return 0;
    track(lay.p, s.size());
    if (tnum) tabular(lay.p, s.size());
    DWRITE_TEXT_METRICS m; lay->GetMetrics(&m);
    widths.emplace(std::move(k), m.widthIncludingTrailingWhitespace);
    return m.widthIncludingTrailingWhitespace;
  }

  // ---- pixel-snapped boxes, the way Blink paints them
  RectF snap(const RectF& r) const { float x0 = rnd(r.x), y0 = rnd(r.y), x1 = rnd(r.r()), y1 = rnd(r.b()); return RectF(x0, y0, x1 - x0, y1 - y0); }
  void fill(const RectF& r, const Rgba& c) { if (c.a <= 0) return; RectF q = snap(r); if (q.w > 0 && q.h > 0) g.fill(q, c); }
  void border(const RectF& r, const Rgba& c, float wdt = 1) {
    if (c.a <= 0) return; RectF q = snap(r);
    g.fill(RectF(q.x, q.y, q.w, wdt), c); g.fill(RectF(q.x, q.b() - wdt, q.w, wdt), c);
    g.fill(RectF(q.x, q.y + wdt, wdt, q.h - 2 * wdt), c); g.fill(RectF(q.r() - wdt, q.y + wdt, wdt, q.h - 2 * wdt), c);
  }
  Com<ID2D1LinearGradientBrush> lin(const std::vector<std::pair<float, Rgba>>& stops, float x0, float y0, float x1, float y1) {
    Com<ID2D1LinearGradientBrush> lg;                   // kept by the surface (gfx.hpp gradient): made once per look
    lg.p = g.gradient(stops, x0, y0, x1, y1); if (lg.p) lg.p->AddRef();
    return lg;
  }
  void fillGrad(const RectF& r, const std::vector<std::pair<float, Rgba>>& stops, float x0, float y0, float x1, float y1) {
    RectF q = snap(r); if (q.w <= 0 || q.h <= 0) return;
    Com<ID2D1LinearGradientBrush> b = lin(stops, x0, y0, x1, y1);
    if (b) g.rt->FillRectangle(Gfx::d(q), b.p);
  }
  void pushOpacity(const RectF& r, float op) {
    g.rt->PushLayer(D2D1::LayerParameters(Gfx::d(r), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), op), nullptr);
  }
  void popOpacity() { g.rt->PopLayer(); }

  // ---- text
  // One line with its BASELINE at `base` (css aligns the pieces of a tile line by baseline). `brush` overrides the colour.
  void tb(const std::wstring& s0, float x, float base, float w, const Font& f, const Rgba& c, Align al = Align::Left, bool ell = true, ID2D1Brush* brush = nullptr, bool tnum = false) {
    if (s0.empty() || w <= 0) return;
    std::wstring s = oneLine(s0);
    std::wstring k; k.reserve(s.size() + 14); keyFont(k, f); keyFloat(k, tracking); keyFloat(k, w);
    k.push_back((wchar_t)(1 + (tnum ? 1 : 0) + (ell ? 2 : 0) + 4 * (int)al)); k += s;
    auto& lines = textKept().lines;
    auto it = lines.find(k);
    if (it == lines.end()) {
      LineKept e; e.lay = mkLayout(s, f, w, 1000, false, ell);
      if (!e.lay) return;
      track(e.lay.p, s.size());
      if (tnum) tabular(e.lay.p, s.size());
      e.lay->SetTextAlignment(al == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : al == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
      DWRITE_LINE_METRICS lm[2]; UINT32 n = 0; e.lay->GetLineMetrics(lm, 2, &n);
      e.baseline = n ? lm[0].baseline : ascR * f.size;
      it = lines.emplace(std::move(k), e).first;
    }
    IDWriteTextLayout* lay = it->second.lay.p; const float bl = it->second.baseline;
    ID2D1Brush* br = brush ? brush : (ID2D1Brush*)g.b(c);
    g.rt->DrawTextLayout(D2D1::Point2F(x, rnd(base) - bl), lay, br, (D2D1_DRAW_TEXT_OPTIONS)(D2D1_DRAW_TEXT_OPTIONS_CLIP | 4 /*ENABLE_COLOR_FONT*/));
  }
  // one line centred vertically in a box (a button's label)
  void tIn(const std::wstring& s, const RectF& r, const Font& f, const Rgba& c, Align al = Align::Center, bool ell = false, bool tnum = false) {
    tb(s, r.x, r.y + baseMid(r.h, f.size), r.w, f, c, al, ell, nullptr, tnum);
  }
  // tabular-nums text by baseline
  void tbn(const std::wstring& s, float x, float base, float w, const Font& f, const Rgba& c, Align al = Align::Left) { tb(s, x, base, w, f, c, al, false, nullptr, true); }
  // Wrapped text with a fixed line height; maxLines > 0 clamps like -webkit-line-clamp (an ellipsis ends the last
  // line). `anywhere` = overflow-wrap: anywhere. Returns the height used; draw=false only measures.
  float wrap(const std::wstring& s, float x, float y, float w, const Font& f, const Rgba& c, float lineH, int maxLines, bool anywhere, bool draw, int* linesOut = nullptr, Align al = Align::Left) {
    if (linesOut) *linesOut = 0;
    if (s.empty() || w <= 0) return 0;
    const float base = baseIn(lineH, f.size);
    auto mk = [&](const std::wstring& str, bool wrapOn, bool ell) {
      Com<IDWriteTextLayout> lay = mkLayout(str, f, w, 100000, wrapOn, ell);
      if (lay) {
        if (wrapOn) lay->SetWordWrapping(anywhere ? (DWRITE_WORD_WRAPPING)2 /*EMERGENCY_BREAK*/ : DWRITE_WORD_WRAPPING_WRAP);
        lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, base);
        if (al != Align::Left) lay->SetTextAlignment(al == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
      }
      return lay;
    };
    // kept under everything the layouts are made from (the clamp too: the head and the clamped line depend on it)
    std::wstring k; k.reserve(s.size() + 20); keyFont(k, f); keyFloat(k, w); keyFloat(k, lineH); keyFloat(k, base);
    k.push_back((wchar_t)(1 + (anywhere ? 1 : 0) + 2 * (int)al)); k.push_back((wchar_t)(1 + std::max(0, std::min(1000, maxLines)))); k += s;
    auto& wraps = textKept().wraps;
    auto it = wraps.find(k);
    if (it == wraps.end()) {
      WrapKept e; e.all = mk(s, true, false);
      if (!e.all) return 0;
      DWRITE_TEXT_METRICS m; e.all->GetMetrics(&m); e.lines = m.lineCount;
      it = wraps.emplace(std::move(k), e).first;
    }
    WrapKept& e = it->second;
    const UINT32 lines = e.lines;
    if (maxLines <= 0 || (int)lines <= maxLines) {
      if (linesOut) *linesOut = (int)lines;
      if (draw) { g.pushClip(RectF(x, y, w, lineH * lines + 0.5f)); g.rt->DrawTextLayout(D2D1::Point2F(x, rnd(y)), e.all.p, g.b(c), (D2D1_DRAW_TEXT_OPTIONS)4); g.popClip(); }
      return lineH * lines;
    }
    if (linesOut) *linesOut = maxLines;
    if (draw) {
      if (!e.clampReady) {
        e.clampReady = true;
        std::vector<DWRITE_LINE_METRICS> lm(lines); UINT32 got = 0; e.all->GetLineMetrics(lm.data(), lines, &got);
        UINT32 head = 0; for (int i = 0; i < maxLines - 1 && i < (int)got; i++) head += lm[i].length;
        if (head) {
          std::wstring first = s.substr(0, head);
          while (!first.empty() && (first.back() == L'\n')) first.pop_back();
          e.head = mk(first, true, false);
        }
        // the clamped line: the rest of its paragraph on one line, ending in an ellipsis
        std::wstring last = s.substr(head);
        size_t nl = last.find(L'\n'); if (nl != std::wstring::npos) last = last.substr(0, nl);
        while (!last.empty() && (last.back() == L' ' || last.back() == L'\t')) last.pop_back();
        if (tw(last, f) <= w) last += L"…";
        e.last = mk(last, false, true);
      }
      const float yy = rnd(y);
      if (e.head) { g.pushClip(RectF(x, y, w, lineH * (maxLines - 1) + 0.5f)); g.rt->DrawTextLayout(D2D1::Point2F(x, yy), e.head.p, g.b(c), (D2D1_DRAW_TEXT_OPTIONS)4); g.popClip(); }
      if (e.last) g.rt->DrawTextLayout(D2D1::Point2F(x, yy + lineH * (maxLines - 1)), e.last.p, g.b(c), (D2D1_DRAW_TEXT_OPTIONS)(D2D1_DRAW_TEXT_OPTIONS_CLIP | 4));
    }
    return lineH * maxLines;
  }

  // ---- css transitions (State::transOn / transMs; ui.hpp). The value `key` has now on its way to `target`, `dur` ms
  // with css `ease`. A value seen for the first time is simply there (an element's first style never transitions);
  // a changed target turns from wherever the value is now. Off: the target, at once. Only the sidebar's own draw()
  // keeps them (a scratch painter - Settings, the move menu, drawAnim - has nothing kept and jumps).
  float trans(const std::string& key, float target, double dur) {
    if (!st.transOn) return target;
    auto it = sb.trans_.find(key);
    if (it == sb.trans_.end()) { Sidebar::Trans t; t.from = t.to = target; t.start = st.transMs; t.seen = true; sb.trans_[key] = t; return target; }
    Sidebar::Trans& t = it->second; t.seen = true;
    auto at = [&]() -> float {
      const double q = dur > 0 ? (st.transMs - t.start) / dur : 1;
      if (q >= 1 || t.from == t.to) return t.to;
      return t.from + (t.to - t.from) * cubicBezier(0.25f, 0.1f, 0.25f, 1, (float)std::max(0.0, q));
    };
    if (t.to != target) { const float cur = at(); t.from = cur; t.to = target; t.start = st.transMs; }
    const float v = at();
    if (v != t.to) sb.transBusy_ = true;
    return v;
  }

  // ---- animations (css body.anim-on): every phase comes from the one clock, State::animMs
  // called by the part that is being drawn when it finds it moves: where it is, and how to draw it alone again
  void animRegion(const RectF& r) {
    if (onlyOn || part.kind < 0) return;
    for (const auto& a : sb.anim_) if (a.kind == part.kind && a.id == part.id) return;
    Sidebar::AnimPart a = part; a.region = r; a.clip = clipOn ? clip : area;
    sb.anim_.push_back(a);
  }
  // css `ease-in-out` = cubic-bezier(0.42, 0, 0.58, 1): y for x
  static float easeInOut(float x) {
    if (x <= 0) return 0; if (x >= 1) return 1;
    float t = x;
    for (int i = 0; i < 8; i++) {
      const float u = 1 - t, bx = 3 * u * u * t * 0.42f + 3 * u * t * t * 0.58f + t * t * t - x;
      const float dx = 3 * u * u * 0.42f + 6 * u * t * (0.58f - 0.42f) + 3 * t * t * (1 - 0.58f);
      if (std::fabs(bx) < 1e-5f || dx == 0) break;
      t = std::max(0.f, std::min(1.f, t - bx / dx));
    }
    return 3 * (1 - t) * t * t + t * t * t;              // y1 = 0, y2 = 1
  }
  // a keyframe pair `0%, 100% { a } 50% { b }` with ease-in-out on each half: 0 at the ends, 1 in the middle
  float pulse(double periodMs) const { const double q = std::fmod(st.animMs, periodMs) / periodMs; return easeInOut((float)(q < 0.5 ? q * 2 : (1 - q) * 2)); }
  // `rainbow-hue … linear infinite`: the angle now, in 2 degree steps (each angle is a brush of its own)
  float hueDeg(double periodMs) const { return (float)(std::floor(std::fmod(st.animMs, periodMs) / periodMs * 180) * 2); }
  // css filter hue-rotate(deg), on one colour (the filter is linear, so a gradient of rotated stops is the rotated gradient)
  static Rgba hueRot(const Rgba& c, float deg) {
    const float a = deg * 3.14159265f / 180, co = std::cos(a), si = std::sin(a);
    const float r = c.r * (0.213f + co * 0.787f - si * 0.213f) + c.g * (0.715f - co * 0.715f - si * 0.715f) + c.b * (0.072f - co * 0.072f + si * 0.928f);
    const float gg = c.r * (0.213f - co * 0.213f + si * 0.143f) + c.g * (0.715f + co * 0.285f + si * 0.140f) + c.b * (0.072f - co * 0.072f - si * 0.283f);
    const float b = c.r * (0.213f - co * 0.213f - si * 0.787f) + c.g * (0.715f - co * 0.715f + si * 0.715f) + c.b * (0.072f + co * 0.928f + si * 0.072f);
    auto cl = [](float v) { return std::max(0.f, std::min(1.f, v)); };
    return Rgba(cl(r), cl(gg), cl(b), c.a);
  }
  static std::vector<std::pair<float, Rgba>> hueStops(const std::vector<std::pair<float, Rgba>>& in, float deg) { std::vector<std::pair<float, Rgba>> o = in; if (deg != 0) for (auto& s : o) s.second = hueRot(s.second, deg); return o; }
  static std::vector<std::pair<float, Rgba>> alphaStops(const std::vector<std::pair<float, Rgba>>& in, float a) { std::vector<std::pair<float, Rgba>> o = in; if (a != 1) for (auto& s : o) s.second.a *= a; return o; }
  // css ask-pulse: `box-shadow: 0 0 0 <2p>px rgba(240, 180, 41, <.55p>)` round a box
  void pulseRing(const RectF& box, float p) {
    if (p <= 0.001f) return;
    const RectF b = snap(box); const float sp = 2 * p;
    g.rt->DrawRectangle(Gfx::d(RectF(b.x - sp / 2, b.y - sp / 2, b.w + sp, b.h + sp)), g.b(Rgba(240 / 255.f, 180 / 255.f, 41 / 255.f, 0.55f * p)), sp);
  }

  // ---- icons: svg path data stroked (or filled) in its viewBox, scaled to the icon's css size
  struct Xf { SidebarPainter& p; Xf(SidebarPainter& p_, float x, float y, float scale, bool mirror = false, float vb = 24, float rot = 0) : p(p_) {
      D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x, y);
      if (mirror) m = D2D1::Matrix3x2F::Translation(-vb, 0) * D2D1::Matrix3x2F::Scale(-1, 1) * m;
      if (rot != 0) m = m * D2D1::Matrix3x2F::Rotation(rot, D2D1::Point2F(x + vb * scale / 2, y + vb * scale / 2));   // css transform on the svg: about its middle
      p.g.rt->SetTransform(m); }
    ~Xf() { p.g.rt->SetTransform(D2D1::Matrix3x2F::Identity()); } };
  void strokeD(const char* d, float sw, const Rgba& c, bool roundJoin = true) {
    ID2D1PathGeometry* geo = svgKept(g.d2d.p, d, false);
    if (geo) g.rt->DrawGeometry(geo, g.b(c), sw, roundJoin ? ssRound.p : ssMiter.p);
  }
  void fillD(const char* d, const Rgba& c) {
    ID2D1PathGeometry* geo = svgKept(g.d2d.p, d, true);
    if (geo) g.rt->FillGeometry(geo, g.b(c));
  }
  // an #sb-tools icon, by element id (index.html); `off` = the .off look of msgs-btn / bars-btn
  void toolIcon(const std::string& id, float x, float y, const Rgba& c, bool off) {
    if (id == "collapse" || id == "expand") {
      // #collapse: M15 6l-6 6 6 6 · #expand: M9 6l6 6-6 6 · body.sb-right mirrors both
      float size = id == "collapse" ? 14.f : 11.f;
      Xf xf(*this, x, y, size / 24, sbRight);
      strokeD(id == "collapse" ? "M15 6l-6 6 6 6" : "M9 6l6 6-6 6", 2.2f, c);
    } else if (id == "search-btn") {
      Xf xf(*this, x, y, 15.f / 24);
      g.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(11, 11), 6.5f, 6.5f), g.b(c), 2, ssMiter.p);
      strokeD("M16 16l5 5", 2, c, false);
    } else if (id == "groups-btn") {
      Xf xf(*this, x, y, 15.f / 24);
      strokeD("M4 6h16M4 12h16M4 18h16", 2, c, false);
      strokeD("M8 9v0M8 15v0", 3, c, false);
    } else if (id == "msgs-btn") {
      Xf xf(*this, x, y, 15.f / 24);
      strokeD("M21 12a8 8 0 0 1-8 8H7l-4 3V12a8 8 0 0 1 8-8h2a8 8 0 0 1 8 8z", 1.9f, c);
      if (!off) strokeD("M8 11h8M8 14h5", 1.9f, c);                       // #msgs-btn.off .msgs-line { display: none }
    } else if (id == "bars-btn") {
      Xf xf(*this, x, y, 15.f / 24);
      strokeD("M4 7h16M4 12h11M4 17h7", 2, off ? c.alpha(0.45f) : c, false);   // off: the bars stay, dimmed
      if (off) strokeD("M5 20L19 4", 2.2f, c, false);                     // ... with a strike through them
    } else if (id == "moveall-btn") {
      Xf xf(*this, x, y, 15.f / 24);
      strokeD("M4 8h13M13 4l4 4-4 4", 1.9f, c);
      strokeD("M20 16H7M11 12l-4 4 4 4", 1.9f, c);
    } else if (id == "drive-btn") {
      Xf xf(*this, x, y, 15.f / 24);
      g.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(7, 2.5f, 17, 21.5f), 2, 2), g.b(c), 1.9f, ssRound.p);
      strokeD("M11 18h2", 1.9f, c);
    } else if (id == "drive-office") {                  // the claim's icons (the branch's index.html .drv-office / -remote / -auto; .drv-phone is the one above)
      Xf xf(*this, x, y, 15.f / 24);
      g.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3, 4, 21, 16.5f), 1.5f, 1.5f), g.b(c), 1.9f, ssRound.p);
      strokeD("M8.5 20.5h7M12 16.5v4", 1.9f, c);
    } else if (id == "drive-remote") {
      Xf xf(*this, x, y, 15.f / 24);
      g.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8.5f, 3.5f, 21, 13), 1.5f, 1.5f), g.b(c), 1.9f, ssRound.p);
      strokeD("M5.5 8.5h-1A1.5 1.5 0 0 0 3 10v8a1.5 1.5 0 0 0 1.5 1.5h10A1.5 1.5 0 0 0 16 18v-2", 1.9f, c);
    } else if (id == "drive-auto") {
      Xf xf(*this, x, y, 15.f / 24);
      strokeD("M2.5 12s3.5-6.5 9.5-6.5 9.5 6.5 9.5 6.5-3.5 6.5-9.5 6.5S2.5 12 2.5 12z", 1.9f, c);
      g.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 2.6f, 2.6f), g.b(c), 1.9f, ssRound.p);
    } else if (id == "restart-btn") {
      Xf xf(*this, x, y, 15.f / 24, false, 24, iconRot);
      strokeD("M20 12a8 8 0 1 1-2.34-5.66", 1.9f, c);
      strokeD("M20 4.5v4.5h-4.5", 1.9f, c);
    } else if (id == "settings-btn") {
      Xf xf(*this, x, y, 16.f / 24);
      g.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 3, 3), g.b(c), 1.8f, ssRound.p);
      strokeD(kGearPath, 1.8f, c);
    }
  }

  // ---- zones
  void zone(const char* kind, const std::string& id, RectF r, const std::wstring& tip = L"", const std::string& arg = "") {
    if (clipOn) {
      float x0 = std::max(r.x, clip.x), y0 = std::max(r.y, clip.y), x1 = std::min(r.r(), clip.r()), y1 = std::min(r.b(), clip.b());
      if (x1 <= x0 || y1 <= y0) return;
      r = RectF(x0, y0, x1 - x0, y1 - y0);
    }
    Sidebar::Zone z; z.hit.kind = kind; z.hit.id = id; z.hit.arg = arg; z.hit.rect = r; z.tip = tip;
    sb.zones_.push_back(z);
  }
  bool hov(const char* kind, const std::string& id) const { return st.hoverPart == kind && st.hover == id; }

  // ---- model helpers (00-state.js, 30-tiles.js)
  const json* acct(const std::string& id) const {
    const json& as = O(meta, "accounts");
    if (as.is_array()) for (auto& a : as) if (S(a, "id") == id) return &a;
    return nullptr;
  }
  std::string acctLabel(const std::string& id) const { const json* a = acct(id); return a ? S(*a, "label") : id; }
  std::string tileTitle(const json& t) const {
    std::string title = S(t, "title"); if (!title.empty()) return title;
    std::string k = S(t, "kind");
    return k == "shell" ? shell : k == "cloud" ? "New cloud session" : "New session";
  }
  const json* tileOfSession(const std::string& sid) const {
    const json& ts = O(meta, "tiles");
    if (ts.is_array()) for (auto& t : ts) if (S(t, "kind") == "claude" && S(t, "sessionId") == sid) return &t;
    return nullptr;
  }
  BarCfg barCfg(const char* key) const {
    BarCfg c = defaultBar(key);
    const json& b = O(O(settings, "bars"), key);
    if (b.is_object()) {
      c.show = truthy(b, "show"); c.name = S(b, "name"); c.from = S(b, "from"); c.to = S(b, "to");
    }
    return c;
  }
  // 46-search.js tileMatches
  bool matches(const json& t, const std::vector<std::string>& words) const {
    if (words.empty()) return true;
    std::string hay = lower(S(t, "title") + " " + S(t, "lastText") + " " + acctLabel(S(t, "account")) + " " + S(t, "effort") + " " +
      (S(t, "kind") == "shell" ? "shell terminal " + shell : std::string("claude")));
    for (auto& w : words) if (hay.find(w) == std::string::npos) return false;
    return true;
  }
  std::vector<std::string> searchWords() const {
    std::vector<std::string> out; std::string cur;
    for (char ch : lower(st.search)) { if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) out.push_back(cur);
    return out;
  }

  // ---- 49-asks.js: the mark
  static int askOpenCount(const json& s) { int n = 0; const json& rs = O(s, "rounds"); if (rs.is_array()) for (auto& r : rs) n += std::max(0, (int)N(r, "total") - (int)N(r, "answered")); return n; }
  static int askDrafted(const json& s) { int n = 0; const json& rs = O(s, "rounds"); if (rs.is_array()) for (auto& r : rs) if (!truthy(r, "sent")) n += (int)N(r, "drafted"); return n; }
  static bool askUnsent(const json& s) { const json& rs = O(s, "rounds"); if (rs.is_array()) for (auto& r : rs) if (!truthy(r, "sent")) return true; return false; }
  AskMark askMark(const std::string& sessionId) const {
    AskMark m; if (sessionId.empty()) return m;
    const json& asks = O(meta, "asks"); if (!asks.is_array()) return m;
    for (auto& s : asks) {
      if (S(s, "sessionId") != sessionId) continue;
      int open = askOpenCount(s), drafted = askDrafted(s);
      m.any = true; m.kind = !askUnsent(s) ? "sent" : drafted ? "drafted" : "waiting";
      m.text = m.kind == "sent" ? L"✓" : std::to_wstring(open);
      m.title = m.kind == "sent" ? L"answers sent; the session has not read them yet"
        : m.kind == "drafted" ? fmt(L"%d of %d answered on the page, not sent yet (F7)", drafted, open)
        : fmt(L"%d question%ls waiting on an ask page (F7)", open, open == 1 ? L"" : L"s");
      return m;
    }
    return m;
  }
  // .t-ask: min-width 15, height 13, padding 0 3, 1px border, 10px/11px 700; ::before "?" (margin-right 1, opacity .75)
  float markW(const AskMark& m) {
    Font f = F(10, 700);
    float w = tw(m.text, f, true) + (m.kind == "sent" ? 0 : tw(L"?", f) + 1) + 6 + 2;
    return std::max(15.f, w);
  }
  void drawMark(const AskMark& m, float x, float top) {
    float w = markW(m); RectF r(x, top, w, 13);
    Rgba col = m.kind == "sent" ? kAskDim : kAsk;
    if (m.kind == "drafted") { fill(r, kAsk); col = kAskInk; } else border(r, col);
    if (m.kind == "drafted") border(r, kAsk);
    Font f = F(10, 700);
    float qw = m.kind == "sent" ? 0 : tw(L"?", f) + 1, textW = tw(m.text, f, true);
    float sx = x + (w - (qw + textW)) / 2, base = top + 1 + baseIn(11, 10);
    if (qw > 0) tb(L"?", sx, base, qw + 2, f, col.alpha(0.75f), Align::Left, false);
    tbn(m.text, sx + qw, base, textW + 2, f, col);
  }

  // =============================================================================================
  // bars (30-tiles.js applyBars + BARS_HTML; css .t-bars / .bars-lbls / .bars-rows / .bar / .fill)
  float bars(const json& t, float x, float y, float w, bool draw, bool dim, const std::string& tileId) {
    const std::string accountId = S(t, "account");
    const json& u = O(O(meta, "usage"), accountId);
    const bool ok = truthy(u, "session") && truthy(u, "week");
    const json& lbl = O(settings, "barLabels");
    const bool lTime = lbl.is_object() ? truthy(lbl, "time") : true, lDay = lbl.is_object() ? truthy(lbl, "day") : true;
    const bool countdown = S(settings, "barLabelMode") == "countdown";
    bool sameReset = false;
    if (countdown && ok) {
      double a, b;
      if (truthy(O(u, "session"), "resetsAt") && truthy(O(u, "week"), "resetsAt") && parseTime(O(O(u, "session"), "resetsAt"), a) && parseTime(O(O(u, "week"), "resetsAt"), b))
        sameReset = std::fabs(a - b) < 60000;
    }
    const bool sLbl = lTime, wLbl = lDay && !sameReset, lblCol = lTime || lDay;
    // the Fable-scoped weekly window, only on a tile whose model is Fable
    const json* fable = nullptr;
    if (lower(S(t, "model")).find("fable") != std::string::npos && O(u, "scoped").is_array())
      for (auto& x2 : O(u, "scoped")) if (lower(S(x2, "model")).find("fable") != std::string::npos) { fable = &x2; break; }
    const json& ctx = O(t, "context");
    struct Row { const char* key; const json* seg; double used; bool has; bool avail; };
    std::vector<Row> rows;
    auto add = [&](const char* key, const json* seg, bool avail, bool isCtx) {
      BarCfg cfg = barCfg(key);
      bool hidden = !cfg.show || (!strcmp(key, "fable") && !seg) || (isCtx && ctx.is_null());
      if (hidden) return;
      Row r{ key, seg, 0, false, avail };
      if (isCtx) { r.has = ctx.is_number(); r.used = r.has ? ctx.get<double>() : 0; }
      else if (avail && seg && isNum(*seg, "used")) { r.has = true; r.used = N(*seg, "used"); }
      r.used = std::max(0.0, std::min(100.0, r.used));
      rows.push_back(r);
    };
    const json& sess = O(u, "session"); const json& week = O(u, "week");
    add("session", truthy(sess) ? &sess : nullptr, ok, false);
    add("week", truthy(week) ? &week : nullptr, ok, false);
    add("fable", fable, ok, false);
    add("context", nullptr, true, true);

    const float lblsH = (sLbl ? 9.f : 0) + (wLbl ? 9.f : 0);
    const float rowsH = rows.empty() ? 0 : 4.f * rows.size() + 3.f * (rows.size() - 1);
    const float H = std::max(lblsH, rowsH);
    // the note under the bars (flex: 1 1 100% wraps it to its own line; the block's 6px gap is the row gap too)
    std::wstring note;
    const std::string ustate = S(u, "state");
    if (ustate == "expired") note = L"usage unavailable: login expired, a running session in this account refreshes it";
    else if (ustate == "no-credentials") note = L"usage unavailable: not logged in";
    else if (ustate == "error" && !ok) note = L"usage unavailable: " + W(S(u, "error").empty() ? "error" : S(u, "error"));
    else if (ustate == "rate-limited" && !ok) note = L"usage: rate limited, retrying later";
    Font nf = F(10);
    float noteH = note.empty() ? 0 : wrap(note, x, y, w, nf, kFaint, LN(10), 0, false, false);
    const float total = H + (note.empty() ? 0 : 6 + noteH);
    if (!draw) return total;

    if (dim) pushOpacity(RectF(x - 1, y - 1, w + 2, total + 2), 0.45f);   // .tile.asleep .t-bars { opacity: 0.45 }
    float bx = x, bw = w;
    if (lblCol) {
      Font lf = F(9);
      auto labelText = [&](const json& seg, bool session) -> std::wstring {
        if (!(ok && truthy(seg) && isNum(seg, "used"))) return kDash;
        const json& iso = O(seg, "resetsAt");
        if (session) return countdown ? fmtUntilHours(iso, now) : fmtResetTime(iso);
        return countdown ? fmtUntilDays(iso, now) : fmtResetDay(iso);
      };
      // column, justify-content: space-between: the first at the top, the last at the bottom
      if (sLbl) tbn(labelText(sess, true), x, y + baseIn(9, 9), 60, lf, kDim);
      if (wLbl) tbn(labelText(week, false), x, (sLbl ? y + H - 9 : y) + baseIn(9, 9), 60, lf, kDim);
      bx = x + 34 + 6; bw = w - 40;
    }
    float ry = y + (H - rowsH) / 2;
    for (auto& r : rows) {
      BarCfg cfg = barCfg(r.key);
      RectF bar(bx, ry, bw, 4);
      if (!ok) pushOpacity(snap(bar), 0.5f);                               // .t-bars.unavailable .bar { opacity: 0.5 }
      fill(bar, kTrack);
      // css .fill { transition: width 400ms ease, background 400ms ease }: the width (and with it the ramp's colours)
      const double usedNow = tileId.empty() ? r.used : (double)trans(tileId + "|bar|" + r.key, r.has ? (float)r.used : 0.f, 400);
      if ((r.has && r.used > 0) || usedNow > 0.01) {
        RectF q = snap(bar); float fw = q.w * (float)(usedNow / 100.0);
        double tt = usedNow / 100.0;
        Rgba c1 = rampAt(cfg.from, cfg.to, std::max(0.0, tt - 0.12)), c2 = rampAt(cfg.from, cfg.to, tt);
        const RectF fr = snap(RectF(q.x, q.y, fw, q.h));
        if (fr.w > 0) g.fillGradientH(fr, { { 0, c1 }, { 1, c2 } }, q.x, q.x + fw);
      }
      if (!ok) popOpacity();
      zone("tile", tileId, bar, r.has ? W(cfg.name) + fmt(L": %d%%", (int)std::floor(r.used + 0.5)) : W(cfg.name));
      ry += 7;
    }
    if (!note.empty()) wrap(note, x, y + H + 6, w, nf, kFaint, LN(10), 0, false, true);
    if (dim) popOpacity();
    return total;
  }

  // =============================================================================================
  // one session tile (30-tiles.js ensureTileEl + updateTile + renderHeld + renderActions; css .tile …)
  // icon: "sleep" | "close" draws that icon in place of the word (2026-11-23; 30-tiles.js TILE_ICONS)
  struct Btn { std::wstring label; std::string kind, arg; std::wstring tip; bool danger = false, primary = false; std::string icon; };
  std::vector<Btn> actionButtons(const json& t) const {
    std::vector<Btn> out;
    const std::string kind = S(t, "kind"), status = S(t, "status");
    const bool exited = truthy(t, "exited"), asleep = status == "asleep";
    if (kind == "cloud") {
      if (truthy(t, "cloudId")) { out.push_back({ L"reload", "tile-action", "reload", L"Load the session's page again" }); out.push_back({ L"browser", "tile-action", "browser", L"Open the session on claude.ai in your browser" }); }
      else if (exited) out.push_back({ L"retry", "tile-action", "retry", L"Run claude --cloud with the same task again" });
      out.push_back({ L"", "tile-close", "", truthy(t, "cloudId") ? L"Remove the tile. The session keeps running in the cloud; Elsewhere lists it." : exited ? L"Remove" : L"Stop and remove the tile", false, false, "close" });
      return out;
    }
    if (kind == "claude") {
      // the same-session guard: open in a terminal -> "Bring it here" ends that process and resumes it here
      if (exited && S(t, "elsewhere") == "terminal") out.push_back({ L"bring it here", "tile-action", "bring-here", L"This session runs in another window: end it there and resume it here" });
      if (exited) out.push_back({ L"resume", "tile-action", "resume", !S(t, "elsewhere").empty() ? L"Look again: resumed here only if nothing else has it open" : L"" });
      else if (asleep) out.push_back({ L"wake", "tile-action", "wake", L"" });
      else if (truthy(t, "sessionId")) out.push_back({ L"", "tile-action", "sleep", L"Sleep", false, false, "sleep" });
      const bool hasBrowser = t.is_object() && t.find("browser") != t.end();
      const std::wstring phrase = O(t, "browser").is_boolean() && O(t, "browser").get<bool>() ? L"without the browser" : L"with the browser";
      const json& as = O(meta, "accounts");
      if (as.is_array()) for (auto& a : as) {
        if (S(a, "id") == S(t, "account")) continue;
        std::wstring lab = W(S(a, "label"));
        // "↪ vv" (U+21AA, the curved arrow - 2026-11-23); the Special prompt is in the tile menu now, not on the strip
        out.push_back({ L"\x21AA " + lab, "tile-action", "open:" + S(a, "id"), L"Move to " + lab + (hasBrowser ? L"\nright-click: move to " + lab + L" " + phrase : L"") });
      }
      if (!exited && !asleep) out.push_back({ L"refresh", "tile-action", "refresh", hasBrowser ? L"right-click: refresh " + phrase : L"" });
    } else {
      if (exited) out.push_back({ L"restart", "tile-action", "restart", L"" });
      if (!exited && !asleep) out.push_back({ L"refresh", "tile-action", "refresh", L"" });
    }
    out.push_back({ L"", "tile-close", "", exited ? L"Remove" : L"Close", false, false, "close" });
    return out;
  }
  // the strip's icons, drawn in an s x s box at (x, y) on a button of colour bg (the 24-unit shapes of TILE_ICONS):
  // sleep = a crescent moon (a disc with a disc of the button's colour over it) and a "z" in its hollow; close = an X
  void drawIcon(const std::string& icon, float x, float y, float s, const Rgba& c, const Rgba& bg) {
    const float k = s / 24.f;
    auto X = [&](float u) { return x + u * k; }; auto Y = [&](float u) { return y + u * k; };
    if (icon == "sleep") {
      g.ellipse(X(12), Y(12), 9 * k, 9 * k, c);
      g.ellipse(X(16.85f), Y(7.15f), 7 * k, 7 * k, bg);
      const float w = std::max(1.f, 2.2f * k);
      g.line(X(14.5f), Y(2), X(21.5f), Y(2), c, w); g.line(X(21.5f), Y(2), X(14.5f), Y(9), c, w); g.line(X(14.5f), Y(9), X(21.5f), Y(9), c, w);
    } else if (icon == "close") {
      const float w = std::max(1.2f, 2.6f * k);
      g.line(X(5.5f), Y(5.5f), X(18.5f), Y(18.5f), c, w); g.line(X(18.5f), Y(5.5f), X(5.5f), Y(18.5f), c, w);
    }
  }

  // .t-actions of the selected tile (30-tiles.js renderActions): one row of buttons; a question (state.confirm:
  // move / refresh / close) puts its two buttons in the row and its text on a line of its own under it; the
  // Special prompt form (kind hold) replaces the row.
  float actions(const json& t, float cx, float y, float cw, bool draw, const State::Confirm* c) {
    const std::string id = S(t, "id"), kind = S(t, "kind");
    if (kind == "cloud") c = nullptr;                                   // a cloud tile asks nothing
    if (c && c->kind == "hold") { if (st.hold && st.hold->tile == id) return holdForm(t, cx, y, cw, draw); c = nullptr; }
    std::vector<Btn> bs; std::wstring question;
    const bool confirming = c && (c->kind == "move" || c->kind == "refresh" || c->kind == "close");
    if (confirming) {
      const std::wstring bw = c->browser == 1 ? L" with the browser on" : c->browser == 0 ? L" with the browser off" : L"";
      const bool exited = truthy(t, "exited");
      if (c->kind == "move") {
        const std::wstring lab = W(acctLabel(c->account));
        question = L"Move this session to " + lab + bw + L"? This window closes and reopens there.";
        bs.push_back({ L"Move to " + lab, "confirm-yes", "", L"", true });
      } else if (c->kind == "refresh") {
        question = S(t, "status") == "busy" ? L"Refresh this session" + bw + L"? It is busy, so the turn in progress is lost."
          : L"Refresh this session" + bw + L"? The process restarts and resumes where it was.";
        bs.push_back({ L"Refresh", "confirm-yes", "", L"", true });
      } else {
        question = exited ? L"Remove this tile?" : L"Close this session? The process is killed.";
        bs.push_back({ exited ? L"Remove" : L"Close", "confirm-yes", "", L"", true });
      }
      bs.push_back({ L"Cancel", "confirm-no", "", L"" });
    } else bs = actionButtons(t);
    // .btn-row: one line, gap 4, clipped; fitButtons() shrinks the font (11 → 7.5px) until the plain row fits
    float size = 11, pad = 3;
    auto bwOf = [&](const Btn& b) { return btnW(b, size, pad); };
    auto total = [&] { return rowW(bs, size, pad); };
    if (!confirming) for (int guard = 0; guard < 20 && total() > cw + 0.5f && size > 7.5f; guard++) { size -= 0.5f; pad = size < 9.5f ? 2 : 3; }
    if (draw) {
      float bx = cx;
      g.pushClip(RectF(cx, y, cw, 17));
      for (auto& b : bs) {
        const float bw = bwOf(b); const RectF r(bx, y, bw, 17);
        const bool hot = st.hover == id && st.hoverPart == b.kind && st.hoverArg == b.arg;
        const Rgba bg = hot ? (b.danger ? Rgba::hex(0x3a1f1f) : Rgba::hex(0x303030)) : kBtn;
        fill(r, bg); border(r, b.danger ? kDanger : Rgba::hex(0x4e4e4e));
        if (!b.icon.empty()) drawIcon(b.icon, r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, size, kFg, bg);
        else tIn(b.label, r, F(size), b.danger ? Rgba::hex(0xf2b3b3) : kFg);
        bx += bw + 4;
      }
      g.popClip();
      bx = cx;
      for (auto& b : bs) { const float bw = bwOf(b); if (bx < cx + cw) zone(b.kind.c_str(), id, RectF(bx, y, std::min(bw, cx + cw - bx), 17), b.tip, b.arg); bx += bw + 4; }
    }
    float h = 17;
    if (confirming) { h += 2; h += wrap(question, cx, y + h, cw, F(11), kDim, 11 * 1.3f, 0, false, draw); }   // .confirm-text
    return h;
  }
  // a strip button's width at this font size and word padding; an icon button: the icon is 1em (the font size) with a
  // little more padding than the words (css .t-actions button.icon); + the 1px border each side
  float btnW(const Btn& b, float size, float pad) { return b.icon.empty() ? tw(b.label, F(size)) + 2 * pad + 2 : size + 2 * (size < 9.5f ? 4.f : 6.f) + 2; }
  float rowW(const std::vector<Btn>& bs, float size, float pad) { float sum = 0; for (auto& b : bs) sum += btnW(b, size, pad); return sum + 4.f * (bs.empty() ? 0 : bs.size() - 1); }   // gap 4
  // Sidebar::minWidth: the widest plain strip at 11px - a Claude tile with a session, awake, on each account in turn
  float widestStrip() {
    std::vector<std::string> owners;
    const json& as = O(meta, "accounts");
    if (as.is_array()) for (auto& a : as) if (!S(a, "id").empty()) owners.push_back(S(a, "id"));
    if (owners.empty()) owners.push_back("");
    float best = 0;
    for (const auto& o : owners) {
      const json t = { { "kind", "claude" }, { "account", o }, { "sessionId", "-" }, { "status", "idle" } };
      best = std::max(best, rowW(actionButtons(t), 11, 3));
    }
    return best;
  }
  float holdForm(const json& t, float cx, float y, float cw, bool draw);

  float tile(const json& t, float x, float y, float w, bool draw, float knownH) {
    const std::string id = S(t, "id"), kind = S(t, "kind"), status = S(t, "status");
    const bool isActive = !id.empty() && id == st.selected;
    const bool hasA = kind == "claude" || kind == "cloud", isCloud = kind == "cloud";
    const json* a = hasA ? acct(S(t, "account")) : nullptr;
    const std::string abbr = a ? S(*a, "abbr") : "";
    const bool busy = status == "busy", exited = truthy(t, "exited"), asleep = status == "asleep", pinned = truthy(t, "pinned");
    const bool ultra = kind == "claude" && truthy(t, "ultracode"), ultraCls = truthy(t, "ultracode");
    const std::string effort = S(t, "effort"), level = kind == "claude" ? lower(effort) : "";
    const bool hovered = st.hover == id && !id.empty() && (st.hoverPart.rfind("tile", 0) == 0 || st.hoverPart == "held-x");

    // where a placed session sits (.t-quad). The layout is this WINDOW's (25-layout.js state.layout), which `meta`
    // does not carry: the app may add it as meta.layout = { tileId: [cells] } (see SIDEBAR-NOTES.md).
    const json& grid = O(settings, "grid");
    const int gRows = std::max(1, (int)N(grid, "rows", 2)), gCols = std::max(1, (int)N(grid, "cols", 2));
    std::vector<int> occ;
    { const json& c = O(O(meta, "layout"), id); if (c.is_array()) for (auto& v : c) if (v.is_number()) occ.push_back(v.get<int>()); }
    const bool hasOcc = !occ.empty();
    const bool elsewhere = !st.ownLayoutOnly && !hasOcc && isNum(t, "inWindow") && (int)N(t, "inWindow") != st.windowIndex;
    const bool quad = hasOcc || elsewhere;
    const float quadSz = (gRows > 3 || gCols > 3) ? 15.f : 11.f;

    const float cx = x + 1 + kPadX, cw = w - 2 - 2 * kPadX, top = y + 1 + kPadY;

    // ---- line 1: [abbr] [cloud] title [ask] [effort] [quad] age   (.t-top: baseline, gap 6, min-height 17)
    const Font abbrF = F(13, 600), ageF = F(12);
    const std::wstring abbrW = W(abbr), ageText = fmtAge(now - (isNum(t, "lastAt") ? N(t, "lastAt") : now));
    const bool cloudTop = isCloud && !abbr.empty(), cloudSub = isCloud && abbr.empty();
    AskMark mark; if (kind == "claude" && !(O(O(settings, "asks"), "showMarks").is_boolean() && !O(O(settings, "asks"), "showMarks").get<bool>())) mark = askMark(S(t, "sessionId"));
    const std::wstring effortText = ultra ? L"ultracode" : W(effort);
    const bool showEffort = effortTag && kind == "claude" && !effortText.empty();
    const bool maxLvl = level == "max";
    const Font effF = F(10, maxLvl ? 700 : 600);
    const float abbrWd = abbr.empty() ? 0 : tw(abbrW, abbrF), ageWd = tw(ageText, ageF, true);
    tracking = 0.2f;   // .t-effort { letter-spacing: 0.2px }
    const float effWd = showEffort ? tw(effortText, effF) + 8 + 2 : 0;
    tracking = 0;
    const float markWd = mark.any ? markW(mark) : 0;
    // .tile.renaming: the abbreviation and the age are gone and the title is a 28px input the width of what is left
    const bool renamingT = !id.empty() && st.renaming == id && st.renameEdit;
    const bool showAbbr = !abbr.empty() && !renamingT;
    float fixed = renamingT ? 0 : ageWd; int items = renamingT ? 1 : 2;   // title + age
    if (showAbbr) { fixed += abbrWd; items++; }
    if (cloudTop) { fixed += 13 - 3; items++; }            // .t-cloud: 13px, margin-left -3
    if (mark.any) { fixed += markWd; items++; }
    if (showEffort) { fixed += effWd; items++; }
    if (quad) { fixed += quadSz; items++; }
    const float titleAvail = std::max(1.f, cw - fixed - 6.f * (items - 1));
    const std::wstring title = oneLine(W(tileTitle(t)));
    const bool untitled = S(t, "title").empty() && kind != "shell";
    // fitTitle: 14px when it fits one line, otherwise the largest size down to 8px that does; at 8px it wraps
    float tsize = 14;
    for (int guard = 0; guard < 20 && tsize > 8 && tw(title, F(tsize, 400, untitled)) > titleAvail; guard++) tsize -= 0.5f;
    const Font titleF = F(tsize, 400, untitled);
    const float titleLH = lhMul(1.25f, tsize);
    int titleLines = 1;
    if (!renamingT && tw(title, titleF) > titleAvail) wrap(title, 0, 0, titleAvail, titleF, kFg, titleLH, 0, true, false, &titleLines);
    if (titleLines < 1) titleLines = 1;
    // baseline alignment: each piece's rise above / drop below the shared baseline
    // (the rename input: border 1, padding 2, its 13.5px text centred in the 22px left)
    const float titleAsc = renamingT ? 3 + std::floor((22 - LN(13.5f)) / 2) + A(13.5f) : baseIn(titleLH, tsize);
    float asc = std::max(titleAsc, A(12)), desc = std::max((renamingT ? 28 : titleLH * titleLines) - titleAsc, D(12));
    if (showAbbr) { asc = std::max(asc, A(13)); desc = std::max(desc, D(13)); }
    const float markAsc = 1 + baseIn(11, 10), effAsc = 1 + baseIn(13, 10);
    if (mark.any) { asc = std::max(asc, markAsc); desc = std::max(desc, 13 - markAsc); }
    if (showEffort) { asc = std::max(asc, effAsc); desc = std::max(desc, 15 - effAsc); }
    const float quadAsc = (quadSz - (gRows - 1)) / gRows;   // a grid with no text: its baseline is the first cell's bottom edge
    if (quad) { asc = std::max(asc, quadAsc); desc = std::max(desc, quadSz - quadAsc); }
    const float topH = std::max(17.f, asc + desc), base = top + asc;

    // ---- line 2: account label when there is no abbreviation, plus status words (.t-sub)
    std::wstring statusText;
    if (exited && !S(t, "elsewhere").empty()) statusText = S(t, "elsewhere") == "deck" ? L"open in the other deck" : L"open elsewhere";   // the same-session guard
    else if (exited) statusText = isCloud && !truthy(t, "cloudId") ? L"no cloud session made" : (O(t, "exitCode").is_null() || N(t, "exitCode") == 0) ? L"exited" : fmt(L"exited (%d)", (int)N(t, "exitCode"));
    else if (asleep) statusText = L"asleep";
    else if (status == "waiting") statusText = L"waiting for you";
    else if (status == "starting") statusText = isCloud ? L"making the cloud session" : L"starting";
    else if (status == "archived") statusText = L"archived";
    // Pause / Resume (lib/pause.js view; 30-tiles.js pauseWords, ui.hpp): the deck's word goes first, with its glyph
    const std::string pauseW = pauseWords(t);
    if (!pauseW.empty()) { const std::wstring w = widen(pauseW); statusText = statusText.empty() ? w : w + L" \x00b7 " + statusText; }
    const bool showName = hasA && abbr.empty();
    const bool showSub = showName || !statusText.empty();
    if (newDriverWords(meta)) {
      // the branch's 30-tiles.js: under auto, WHICH screen sizes this session is per-session news; under a claim
      // every session follows the same screen and the driver button says so
      if (truthy(t, "mirror") && driverMode(S(settings, "driver")) == "auto") {
        const std::string by = S(t, "sizeBy");
        const std::wstring w = by == "remote" ? L"on the remote desk" : by == "office" ? L"sized by the office deck" : L"on your phone";
        statusText = statusText.empty() ? w : statusText + L" · " + w;
      }
    } else if (truthy(t, "mirror")) statusText = statusText.empty() ? L"on your phone" : statusText + L" · on your phone";
    if (!statusText.empty() && showName) statusText = L"· " + statusText;

    // ---- the rest of the column
    const bool showBars = hasA && st.showBars;
    const std::string lastText = S(t, "lastText");
    const bool showMsg = hasA && !lastText.empty() && !busy && st.showMsgs;
    std::wstring msg;
    if (showMsg) {   // lastText without \r, each line right-trimmed, blank lines dropped
      std::wstring wsrc = W(lastText), line; std::vector<std::wstring> lines;
      auto flush = [&] {
        while (!line.empty() && iswspace(line.back())) line.pop_back();
        bool blank = true; for (wchar_t c : line) if (!iswspace(c)) { blank = false; break; }
        if (!blank) lines.push_back(line);
        line.clear();
      };
      for (wchar_t c : wsrc) { if (c == L'\r') continue; if (c == L'\n') flush(); else line += c; }
      flush();
      for (size_t i = 0; i < lines.size(); i++) { if (i) msg += L'\n'; msg += lines[i]; }
    }
    const json& ml = O(settings, "msgLines");
    const bool wide = area.w > 400;   // body.sb-wide
    int clampN = (int)N(ml, wide ? "wide" : "normal", wide ? 10 : 3); if (clampN < 1) clampN = wide ? 10 : 3;
    const Font msgF = F(10);
    const json& held = O(t, "held");
    const size_t heldN0 = held.is_array() ? held.size() : 0;
    const json& amail = O(t, "auditorMail");                          // a child's files from its auditor (30-tiles.js auditorMailRow), above the held rows
    const size_t mailN = amail.is_array() ? amail.size() : 0;
    const size_t heldN = heldN0 + mailN;

    float yy = top + topH;
    float subY = 0, barsY = 0, msgY = 0, msgH = 0, heldY = 0, actY = 0;
    if (showSub) { yy += 1 - 2; subY = yy; yy += 12; }                 // gap 1, margin-top -2, 12px line
    if (showBars) { yy += 1 + 2; barsY = yy; yy += bars(t, cx, yy, cw, false, false, id) + 1; }   // margin 2px 0 1px
    if (showMsg) { yy += 1; msgY = yy; msgH = wrap(msg, cx, yy, cw, msgF, kFg, 13, clampN, true, false) + 2; yy += msgH; }
    if (heldN) { yy += 1 + 1; heldY = yy; yy += 13.f * heldN + (heldN - 1); }
    const State::Confirm* cf = nullptr; { auto ci = st.confirm.find(id); if (ci != st.confirm.end()) cf = &ci->second; }
    if (isActive) { yy += 1 + 1; actY = yy; yy += actions(t, cx, 0, cw, false, cf); }
    yy += kPadY + 1;
    const float h = yy - y;
    if (!draw) return h;
    (void)knownH;
    // .tile.dragging { opacity: 0.45 }; .drop-before / .drop-after: a 3px bar of --fg-dim outside the tile (box-shadow)
    const bool dragged = st.dragging == id && !id.empty();
    if (st.dropMark == id && !id.empty()) fill(RectF(x, st.dropBefore ? y - 3 : y + h, w, 3), kDim);
    if (dragged) pushOpacity(RectF(x - 6, y - 6, w + 12, h + 12), 0.45f);

    // ---- the box: background, border, effort colour
    const RectF box(x, y, w, h);
    // css body.asks-focus #tiles .tile.active: while the asks tile is the selection, the selected session's tile is drawn plain
    const bool litActive = isActive && !st.asksFocus;
    fill(box, litActive ? Rgba::hex(0x222222) : hovered && !isActive ? Rgba::hex(0x1b1b1b) : kTile);
    Rgba bAll = litActive ? Rgba::hex(0xbdbdbd) : kNone;
    if (truthy(t, "attention")) bAll = kAmber;                          // .tile.desk-attn
    bool rainbowBorder = false;
    Rgba bEdge = kNone;                                                 // the outer edge only (effort colour, unselected)
    if (effortColor) {
      Rgba lv = level == "low" ? Rgba::hex(0x4a4a4a) : level == "medium" ? Rgba::hex(0x8a8a8a) : level == "xhigh" ? Rgba::hex(0xffffff) : kNone;
      if (isActive) {
        if (lv.a > 0) bAll = lv;
        if (maxLvl) { bAll = kNone; rainbowBorder = true; }
        if (ultraCls) { bAll = Rgba::hex(0xa855f7); rainbowBorder = false; }
      } else bEdge = lv;
    }
    // body.anim-on: the selected ultracode tile's border pulses (ultra-pulse-border); a desk the on-call opened pulses
    // amber with a ring round it (ask-pulse); the max rainbow drifts (rainbow-hue); the ultracode strip breathes
    const bool an = st.animOn, attnT = truthy(t, "attention");
    const RectF box0(x, y, w, h);
    float attnP = -1;
    if (an) {
      if (effortColor && isActive && ultraCls) bAll = Rgba::mix(Rgba::hex(0x6d28d9), Rgba::hex(0xd8b4fe), pulse(2400));
      else if (attnT) { attnP = pulse(1600); bAll = Rgba::mix(Rgba::hex(0x8a6a1c), kAmber, attnP); }
      // (the ring of ask-pulse reaches 2px out of the box; everything else that moves is inside it)
      if (attnP >= 0) { const RectF sb0 = snap(box0); animRegion(RectF(sb0.x - 2, sb0.y - 2, sb0.w + 4, sb0.h + 4)); }
      else if (attnT || ((ultraCls || maxLvl) && (effortColor || showEffort))) animRegion(box0);
    }
    const float hue = an ? hueDeg(4500) : 0, breath = an ? 0.45f + 0.55f * pulse(2400) : 1.f;   // rainbow-hue; ultra-pulse (opacity)
    border(box, bAll);
    if (attnP >= 0) pulseRing(box, attnP);
    const RectF sbox = snap(box);
    if (bEdge.a > 0) fill(RectF(sbRight ? sbox.r() - 1 : sbox.x, sbox.y, 1, sbox.h), bEdge);
    if (effortColor && sbRight && isActive && (level == "low" || level == "medium" || level == "xhigh")) {
      // body.sb-right.effort-color .tile.effort-<level> comes later in the sheet with the same weight as the selected
      // rules, so on a right-hand sidebar it wins for the two side borders even on the selected tile: the inner
      // (left) one transparent, the outer (right) one the level's colour
      const Rgba lv = level == "low" ? Rgba::hex(0x4a4a4a) : level == "medium" ? Rgba::hex(0x8a8a8a) : Rgba::hex(0xffffff);
      fill(RectF(sbox.x, sbox.y + 1, 1, sbox.h - 2), Rgba::hex(0x222222));
      fill(RectF(sbox.r() - 1, sbox.y + 1, 1, sbox.h - 2), lv);
    }
    if (rainbowBorder) {   // .tile.active.effort-max::after: a 1px border-image, linear-gradient(135deg, …)
      Com<ID2D1LinearGradientBrush> br = lin(hueStops(rainbow(), hue), sbox.x, sbox.y, sbox.r(), sbox.b());
      if (br) g.rt->DrawRectangle(Gfx::d(sbox.inset(0.5f)), br.p, 1);
    }
    if (effortColor && !isActive && (maxLvl || ultraCls)) {
      // ::before strip on the outer edge: 2px (the border pixel plus one inward); ultracode at max 3px = 2 purple + 1 rainbow
      if (ultraCls && maxLvl) {
        float px = sbRight ? sbox.r() - 2 : sbox.x, rx = sbRight ? sbox.r() - 3 : sbox.x + 2;
        fillGrad(RectF(px, sbox.y, 2, sbox.h), alphaStops(purple(), breath), 0, sbox.y, 0, sbox.b());
        fillGrad(RectF(rx, sbox.y, 1, sbox.h), alphaStops(rainbow(), breath), 0, sbox.y, 0, sbox.b());
      } else {
        fillGrad(RectF(sbRight ? sbox.r() - 2 : sbox.x, sbox.y, 2, sbox.h), ultraCls ? alphaStops(purple(), breath) : hueStops(rainbow(), hue), 0, sbox.y, 0, sbox.b());
      }
    }
    if (effortColor && isActive && maxLvl && ultraCls)                  // selected ultracode at max: a rainbow line just inside the purple edge
      fillGrad(RectF(sbRight ? sbox.r() - 3 : sbox.x + 1, sbox.y + 1, 2, sbox.h - 2), alphaStops(rainbow(), breath), 0, sbox.y, 0, sbox.b());
    zone("tile", id, box);

    // ---- line 1
    float px = cx;
    if (showAbbr) {
      if (asleep) pushOpacity(RectF(px - 1, top - 2, abbrWd + 2, topH + 4), 0.45f);
      tb(abbrW, px, base, abbrWd + 2, abbrF, busy ? kGreen : kDim, Align::Left, false);
      if (asleep) popOpacity();
      zone("tile-acct", id, RectF(px, top, abbrWd, topH));
      px += abbrWd + 6;
    }
    if (cloudTop) {
      { Xf xf(*this, rnd(px - 3), rnd(top + (topH - 13) / 2), 13.f / 16); fillD(kCloudPath, busy ? kGreen : kCloud); }
      zone("tile", id, RectF(px - 3, top, 13, topH), L"cloud session: runs on claude.ai, not on this machine");
      px += 10 + 6;
    }
    if (renamingT) {
      // .t-title input.rename: 28px, #0f0f0f, 1px #555, padding 2px 8px, 13.5px; focused: a 1px #555 outline round it
      const RectF in = snap(RectF(px, base - titleAsc, titleAvail, 28));
      const bool foc = st.focusEdit == st.renameEdit;
      if (foc) border(RectF(in.x - 1, in.y - 1, in.w + 2, in.h + 2), Rgba::hex(0x555555));
      fill(in, Rgba::hex(0x0f0f0f)); border(in, Rgba::hex(0x555555));
      st.renameEdit->placeholder = L"tile title";
      st.renameEdit->draw(g, RectF(in.x + 9, in.y + 3, std::max(0.f, in.w - 18), 22), F(13.5f), kFg, Rgba::hex(0x757575), foc, st.blinkOn);
      zone("edit", "rename", in);
      px += titleAvail + 6;
    } else {
      const Rgba tc = (untitled || asleep || elsewhere) ? kDim : kFg;
      if (titleLines > 1) wrap(title, px, base - titleAsc, titleAvail, titleF, tc, titleLH, 0, true, true);
      else tb(title, px, base, titleAvail, titleF, tc, Align::Left, false);
      px += titleAvail + 6;
    }
    if (mark.any) { drawMark(mark, px, base - markAsc); zone("tile", id, RectF(px, base - markAsc, markWd, 13), mark.title); px += markWd + 6; }
    if (showEffort) {
      const RectF er(px, base - effAsc, effWd, 15); const RectF es = snap(er);
      Rgba ec = Rgba::hex(0xcfcbc4), eb = Rgba::hex(0x4a4a4a), ebg = Rgba::hex(0x1f1f1f);
      if (level == "low") { ec = Rgba::hex(0x7a7a7a); eb = Rgba::hex(0x3a3a3a); }
      else if (level == "medium") { ec = Rgba::hex(0xa8a8a8); eb = Rgba::hex(0x4a4a4a); }
      else if (level == "high") { ec = Rgba::hex(0xd8d4cc); eb = Rgba::hex(0x555555); }
      else if (level == "xhigh") { ec = Rgba::hex(0xffffff); eb = Rgba::hex(0x8a8a8a); ebg = Rgba::hex(0x2a2a2a); }
      else if (maxLvl) { eb = Rgba::hex(0x7a7a7a); ebg = kNone; }      // the background is clipped to the text
      if (ultraCls) { ec = Rgba::hex(0xd8b4fe); eb = Rgba::hex(0x7c3aed); ebg = Rgba::hex(0x1d1530); }
      if (an && ultraCls) { const float up = pulse(2400); ec = Rgba::mix(Rgba::hex(0xa78bfa), Rgba::hex(0xf3e8ff), up); eb = Rgba::mix(Rgba::hex(0x5b21b6), Rgba::hex(0xc084fc), up); }   // ultra-pulse-text
      fill(er, ebg);
      if (maxLvl && ultraCls) {   // .tile.effort-max.ultracode .t-effort: the ultracode text in a rainbow frame
        Com<ID2D1LinearGradientBrush> br = lin(rainbow(), es.x, 0, es.r(), 0);
        if (br) g.rt->DrawRectangle(Gfx::d(es.inset(0.5f)), br.p, 1);
      } else border(er, eb);
      const float tx = es.x + 1 + 4, tbase = es.y + effAsc;
      tracking = 0.2f;
      if (maxLvl && !ultraCls) {
        // max: the text is a window onto linear-gradient(90deg, 5 colours + the first again) three boxes wide
        float gw = (es.w - 2) * 3;
        // (body.anim-on: the whole tag turns through the hues, rainbow-hue; its grey border is the same at every angle)
        Com<ID2D1LinearGradientBrush> br = lin(hueStops({ { 0, Rgba::hex(0xff5f6d) }, { .2f, Rgba::hex(0xffc371) }, { .4f, Rgba::hex(0xc4f26a) }, { .6f, Rgba::hex(0x4fd1ff) }, { .8f, Rgba::hex(0xb388ff) }, { 1, Rgba::hex(0xff5f6d) } }, hue),
          es.x + 1, 0, es.x + 1 + gw, 0);
        tb(effortText, tx, tbase, effWd, effF, ec, Align::Left, false, br ? (ID2D1Brush*)br.p : nullptr);
      } else tb(effortText, tx, tbase, effWd, effF, ec, Align::Left, false);
      tracking = 0;
      zone("tile", id, er, ultra ? L"ultracode on · effort " + (effort.empty() ? std::wstring(L"?") : W(effort)) : std::wstring(L"effort level of the last answer"));
      px += effWd + 6;
    }
    if (quad) {
      // .t-quad: an R×C grid of cells, 1px gaps, the occupied ones lit; baseline = its first cell's bottom
      const RectF qr = snap(RectF(px, base - quadAsc, quadSz, quadSz));
      const float cwid = (quadSz - (gCols - 1)) / gCols, chei = (quadSz - (gRows - 1)) / gRows;
      for (int r = 0; r < gRows; r++) for (int c = 0; c < gCols; c++) {
        const int k = r * gCols + c + 1;
        const bool on = std::find(occ.begin(), occ.end(), k) != occ.end();
        fill(RectF(qr.x + c * (cwid + 1), qr.y + r * (chei + 1), cwid, chei), on ? Rgba::hex(0xbdbdbd) : Rgba::hex(0x2a2a2a));
      }
      if (elsewhere) border(RectF(qr.x - 1, qr.y - 1, qr.w + 2, qr.h + 2), Rgba::hex(0x6f7fa0));   // outline: 1px
      zone("tile", id, qr, elsewhere ? fmt(L"in deck window %d — select to bring it here", (int)N(t, "inWindow") + 1) : std::wstring(L"where this session sits in the layout"));
      px += quadSz + 6;
    }
    if (!renamingT) {
      tbn(ageText, px, base, ageWd + 2, ageF, kDim);
      std::wstring tip;
      if (kind == "claude") {
        if (asleep) tip = L"asleep since " + (isNum(t, "sleptAt") && N(t, "sleptAt") ? fmtLocaleTime(N(t, "sleptAt")) : std::wstring(L"?"));
        else if (!S(t, "sleepBlocker").empty()) tip = L"awake: " + W(S(t, "sleepBlocker"));
        else tip = L"awake: may sleep at the next check";
      } else if (isCloud) tip = !S(t, "cloudError").empty() ? L"cloud session; its status could not be read: " + W(S(t, "cloudError")) : std::wstring(L"cloud session: runs on claude.ai, never sleeps");
      if (!tip.empty()) zone("tile", id, RectF(px, top, ageWd, topH), tip);
    }

    // ---- line 2
    if (showSub) {
      const Font nameF = F(11.5f, 500);
      const float sbase = subY + baseIn(12, 11.5f);
      float sx = cx;
      if (showName) {
        const std::wstring name = W(acctLabel(S(t, "account"))); const float nw = tw(name, nameF);
        tb(name, sx, sbase, cw, nameF, busy ? kGreen : kDim, Align::Left, true);
        zone("tile-acct", id, RectF(sx, subY, std::min(nw, cw), 12));
        sx += nw + 6;
        if (cloudSub) {
          { Xf xf(*this, rnd(sx - 3), rnd(subY + (12 - 11.5f) / 2), 11.5f / 16); fillD(kCloudPath, busy ? kGreen : kCloud); }
          sx += 11.5f - 3 + 6;
        }
      }
      if (!statusText.empty()) {
        const bool waiting = !exited && status == "waiting";
        // css order: paused, exited, asleep, waiting - later wins (.t-status.paused: soft blue, a PAUSE prompt went in)
        const Rgba sc = waiting ? kAmber : asleep ? Rgba::hex(0x6f7fa0) : exited ? kDanger : !pauseW.empty() ? Rgba::hex(0x8fb0d8) : kFaint;
        tb(statusText, sx, sbase, cx + cw - sx, F(11.5f, waiting ? 600 : 400), sc, Align::Left, true);
      }
    }
    if (showBars) bars(t, cx, barsY, cw, true, asleep, id);
    if (showMsg) {
      // .t-msg: the box bleeds 2px into the tile padding so its TEXT starts where every other text starts
      if (asleep) pushOpacity(RectF(cx - 3, msgY - 1, cw + 6, msgH + 2), 0.45f);
      fill(RectF(cx - 2, msgY, cw + 4, msgH), kBox);
      wrap(msg, cx, msgY + 1, cw, msgF, Rgba::hex(0xd0d0d0), 13, clampN, true, true);
      if (asleep) popOpacity();
    }
    if (heldN) {
      // .t-held rows: when · first line of the text · state · ×
      float hy = heldY; const Font hf = F(10);
      if (mailN) for (auto& m : amail) {
        // from auditor 1, 15:02 · its first line · sent | when idle | waits for Send [Send] [×]
        const std::string ms = S(m, "state");
        const RectF row(cx - 2, hy, cw + 4, 13);
        fill(row, kBox);
        const double sentAt = N(m, "sentAt");
        const std::wstring when = L"from " + W(S(m, "from")) + (sentAt > 0 ? L", " + fmtHeldTime(sentAt, now) : std::wstring());
        const std::wstring stateText = ms == "sent" ? L"sent" : ms == "hold" ? L"waits for Send" : L"when idle";
        const Rgba stateC = ms == "sent" ? kGreen : kAmber;
        const float hbase = hy + baseIn(13, 10), inner = cw;
        const float whenW = std::min(tw(when, hf), inner * 0.46f);
        const Font xf = F(12); const bool hold = ms == "hold";
        const float sendW = hold ? tw(L"Send", hf) + 4 : 0, xW = hold ? tw(L"×", xf) + 4 : 0, stW = tw(stateText, hf);
        float lx = cx;
        tb(when, lx, hbase, whenW, hf, Rgba::hex(0x9ecbff), Align::Left, true);
        zone("tile", id, RectF(lx, hy, whenW, 13), W(S(m, "name")) + L": the deck types it as your own message");
        lx += whenW + 4;
        const float textW = std::max(0.f, cx + inner - (hold ? sendW + 4 + xW + 4 : 0) - stW - 4 - lx);
        const std::wstring first = W(S(m, "firstLine").empty() ? S(m, "name") : S(m, "firstLine"));
        tb(first, lx, hbase, textW, hf, Rgba::hex(0xc9c9c9), Align::Left, true);
        zone("tile", id, RectF(lx, hy, textW, 13), first);
        lx += textW + 4;
        tb(stateText, lx, hbase, stW + 2, hf, stateC, Align::Left, false);
        lx += stW + 4;
        if (hold) {
          const bool sHot = st.hoverPart == "amail-send" && st.hover == id, xHot = st.hoverPart == "amail-x" && st.hover == id;
          tb(L"Send", lx + 2, hbase, sendW, hf, sHot ? kGreen : Rgba::hex(0x9ecbff), Align::Left, false);
          zone("amail-send", id, RectF(lx, hy, sendW, 13), L"type it into this session now, or when its turn ends", S(m, "id"));
          lx += sendW + 4;
          tb(L"×", lx + 2, hy + baseIn(13, 12), xW, xf, xHot ? kDanger : kFaint, Align::Left, false);
          zone("amail-x", id, RectF(lx, hy, xW, 13), L"do not send it (the file goes to the inbox's refused folder)", S(m, "id"));
        }
        hy += 14;
      }
      if (heldN0) for (auto& i : held) {
        const std::string hk = S(i, "kind"), hs = S(i, "state");
        const RectF row(cx - 2, hy, cw + 4, 13);
        fill(row, kBox);
        std::wstring when;
        if (hk == "next") when = L"next turn";
        else if (hk == "recurring") when = fmt(L"every %dm", (int)N(i, "everyMin")) + (N(i, "startMin") != N(i, "endMin") ? L" " + HHMM(N(i, "startMin")) + L"–" + HHMM(N(i, "endMin")) : std::wstring());
        else if (hk == "triggered") when = L"⚑ " + W(S(i, "trigger"));
        else when = fmtHeldTime(N(i, "at"), now);
        const bool standing = hk == "recurring" || hk == "triggered";
        const std::wstring stateText = hs == "sending" ? L"typing" : hs == "due" ? L"waiting" : hs == "sent" ? (truthy(i, "note") ? L"typed?" : L"sent") : hs == "failed" ? L"failed" : standing ? L"on" : L"held";
        const Rgba stateC = hs == "due" ? kAmber : hs == "sent" ? kGreen : hs == "failed" ? kDanger : kFaint;
        const float hbase = hy + baseIn(13, 10);
        const float inner = cw;                                        // padding 0 2 inside the bled box
        const float whenW = std::min(tw(when, hf), inner * 0.46f);     // max-width: 46%
        const Font xf = F(12); const float xW = tw(L"×", xf) + 4, stW = tw(stateText, hf);
        float lx = cx;
        tb(when, lx, hbase, whenW, hf, Rgba::hex(0x9ecbff), Align::Left, true);
        zone("tile", id, RectF(lx, hy, whenW, 13), hk == "timed" || hk.empty() ? fmtLocaleString(N(i, "at")) : W(hk) + L" prompt");
        lx += whenW + 4;
        const float textW = std::max(0.f, cx + inner - xW - 4 - stW - 4 - lx);
        std::wstring first = W(S(i, "text")); { size_t nl = first.find(L'\n'); if (nl != std::wstring::npos) first = first.substr(0, nl); if (first.size() > 120) first = first.substr(0, 120); }
        tb(first, lx, hbase, textW, hf, Rgba::hex(0xc9c9c9), Align::Left, true);
        zone("tile", id, RectF(lx, hy, textW, 13), W(S(i, "text")));
        lx += textW + 4;
        tb(stateText, lx, hbase, stW + 2, hf, stateC, Align::Left, false);
        if (truthy(i, "note")) zone("tile", id, RectF(lx, hy, stW, 13), W(S(i, "note")));
        lx += stW + 4;
        const bool xHot = st.hoverPart == "held-x" && st.hover == id;
        tb(L"×", lx + 2, hy + baseIn(13, 12), xW, xf, xHot ? kDanger : kFaint, Align::Left, false);
        zone("held-x", id, RectF(lx, hy, xW, 13), L"forget this prompt", S(i, "id"));
        hy += 14;
      }
    }
    if (isActive) actions(t, cx, actY, cw, true, cf);

    // ---- the pin: on the tile's outer top corner, half outside it (.t-pin 13×13 at -5,-5 of the padding box)
    {
      bool right = sbRight; if (S(settings, "pinSide") == "centre") right = !right;
      const RectF pr = snap(RectF(right ? x + w - 1 + 5 - 13 : x + 1 - 5, y + 1 - 5, 13, 13));
      const bool hot = st.hover == id && st.hoverPart == "tile-pin";
      // unpinned: invisible until the cursor is nearPin the corner (.t-pin-zone, 46×34 at -8,-8: 70 %), full over the pin
      const RectF zr(right ? x + w - 1 + 8 - 46 : x + 1 - 8, y + 1 - 8, 46, 34);
      const bool nearPin = !pinned && !hot && zr.has(st.mouseX, st.mouseY) && (!clipOn || clip.has(st.mouseX, st.mouseY));
      // css .t-pin { opacity: 0; transition: opacity 120ms … }: the fade in and out (the colours switch at once)
      const float pinTarget = pinned || hot ? 1.f : nearPin ? 0.7f : 0.f;
      const float pinOp = onlyOn ? pinTarget : trans(id + "|pin", pinTarget, 120);
      if (pinOp > 0.004f) {
        const Rgba bg = pinned ? (hot ? Rgba::hex(0x5a5a5a) : Rgba::hex(0x3a3a3a)) : hot ? Rgba::hex(0x2e2e2e) : Rgba::hex(0x1f1f1f);
        const Rgba fgc = pinned ? (hot ? Rgba::hex(0xffffff) : Rgba::hex(0xf0f0f0)) : Rgba::hex(0x8a8a8a);
        if (pinOp < 0.996f) pushOpacity(RectF(pr.x - 1, pr.y - 1, pr.w + 2, pr.h + 2), pinOp);
        fill(pr, bg);
        { Xf xf(*this, rnd(pr.x + 2.5f), rnd(pr.y + 2.5f), 8.f / 16);   // the svg box is snapped to the pixel grid
          fillD(kPinPath, fgc); }
        if (pinOp < 0.996f) popOpacity();
      }
      zone("tile-pin", id, pr, L"Pin");
    }
    if (dragged) popOpacity();
    return h;
  }

  // =============================================================================================
  // a group divider (35-groups.js dividerEl; css .group-bar)
  float groupBar(const std::string& name, int count, float x, float y, float w, bool draw) {
    const float h = 2 + LN(11) + 2 + 1;
    if (!draw) return h;
    const bool collapsed = st.collapsedGroups.count(name) > 0;
    const Rgba c = hov("group", name) ? kFg : kDim;
    const RectF box = snap(RectF(x, y, w, h));
    g.fill(RectF(box.x, box.b() - 1, box.w, 1), Rgba::hex(0x262626));   // border-bottom
    const float midY = y + 2 + LN(11) / 2, base = y + 2 + A(11);
    // .g-chev: an 8×5 triangle pointing down; rotated -90° when collapsed
    {
      const float tx = x + 2, ty = midY - 2.5f;
      const float turn = trans("chev|" + name, collapsed ? -90.f : 0.f, 120);   // css .g-chev { transition: transform 120ms }
      D2D1::Matrix3x2F m = turn != 0 ? D2D1::Matrix3x2F::Rotation(turn, D2D1::Point2F(tx + 4, ty + 2.5f)) : D2D1::Matrix3x2F::Identity();
      g.rt->SetTransform(m);
      Com<ID2D1PathGeometry> geo; g.d2d->CreatePathGeometry(geo.put());
      if (geo) {
        Com<ID2D1GeometrySink> sink; geo->Open(sink.put());
        sink->BeginFigure(D2D1::Point2F(tx, ty), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(tx + 8, ty)); sink->AddLine(D2D1::Point2F(tx + 4, ty + 5));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
        g.rt->FillGeometry(geo.p, g.b(c));
      }
      g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    const Font cf = F(11); const std::wstring cnt = std::to_wstring(count); const float cwd = tw(cnt, cf, true);
    const float nx = x + 2 + 8 + 6, nw = std::max(0.f, x + w - 4 - cwd - 6 - nx);
    tracking = 0.2f; tb(W(name), nx, base, nw, F(11, 600), c, Align::Left, true); tracking = 0;   // .g-name { letter-spacing: 0.2px }
    tbn(cnt, x + w - 4 - cwd, base, cwd + 2, cf, kFaint);
    zone("group", name, RectF(x, y, w, h));
    return h;
  }

  // =============================================================================================
  // the asks tile (49-asks.js paintAsksTile; css .asks-tile, .at-*)
  float asksTile(float x, float y, float w, bool draw) {
    const json& asks = O(meta, "asks"); const json& log = O(meta, "askLog");
    const size_t n = asks.is_array() ? asks.size() : 0, nLog = log.is_array() ? log.size() : 0;
    const float rowH = 1 + 1 + LN(12) + 1 + 1;   // border + padding 1px + a 12px line
    const size_t rowsN = n + (nLog ? 1 : 0);
    const float rowsFull = rowsN ? rowH * rowsN + (rowsN - 1) : 0, rowsH = std::min(132.f, rowsFull);
    const float tLH = lhMul(1.25f, 13);
    const float asc = std::max(baseIn(tLH, 13), std::max(A(12), 1 + baseIn(13, 11)));
    const float desc = std::max(tLH - baseIn(tLH, 13), std::max(D(12), 15 - (1 + baseIn(13, 11))));
    const float topH = std::max(17.f, asc + desc);
    const float h = 1 + kPadY + topH + 2 + rowsH + kPadY + 1;
    if (!draw) return h;
    bool asking = false; if (n) for (auto& s : asks) if (askUnsent(s)) { asking = true; break; }
    const bool none = n == 0, hot = st.hoverPart == "asks";
    const RectF box(x, y, w, h);
    // css .asks-tile.active (the tile is the selection: asksSelect): #262013, the amber border
    fill(box, st.asksFocus ? Rgba::hex(0x262013) : hot ? Rgba::hex(0x211d13) : Rgba::hex(0x1a1710));
    // css body.anim-on .asks-tile.asking:not(.active): ask-pulse, 1.6 s
    const bool beat = st.animOn && asking && !st.asksFocus; const float bp = beat ? pulse(1600) : 0;
    border(box, beat ? Rgba::mix(Rgba::hex(0x8a6a1c), kAmber, bp) : asking || st.asksFocus ? kAmber : Rgba::hex(0x3a3020));
    if (beat) { pulseRing(box, bp); const RectF sb0 = snap(box); animRegion(RectF(sb0.x - 2, sb0.y - 2, sb0.w + 4, sb0.h + 4)); }
    // meta.askFeed.state 'guest' (the native core): another deck is serving the ask pages; here they are read-only
    const bool guest = S(O(meta, "askFeed"), "state") == "guest";
    zone("asks", "", box, guest ? L"Another deck is serving the ask pages: they show here read-only. Answer on that deck, or in the session's terminal."
                                : L"Questions from Claude sessions on ask pages. Click to answer them (a row: that session first); F6 or F7 closes. F7 on a session shows just its own.");
    const float cx = x + 1 + kPadX, cw = w - 2 - 2 * kPadX, top = y + 1 + kPadY, base = top + asc;
    // .at-glyph 15×15, 11px/13px 700
    {
      const RectF gr(cx, base - (1 + baseIn(13, 11)), 15, 15);
      const Rgba gc = none ? kFaint : kAmber;
      if (asking) fill(gr, kAmber); else border(gr, gc);
      tb(L"?", gr.x, base, 15, F(11, 700), asking ? Rgba::hex(0x1a1408) : gc, Align::Center, false);
    }
    tb(L"Asks", cx + 15 + 6, base, 60, F(13, none ? 500 : 600), none ? kDim : kFg, Align::Left, false);
    const std::wstring count = guest ? (n ? fmt(L"%d waiting \u00b7 read-only", (int)n) : std::wstring(L"read-only")) : n ? fmt(L"%d waiting", (int)n) : std::wstring(L"nothing waiting");
    tbn(count, cx, base, cw, F(12, 600), none ? kFaint : kAmber, Align::Right);
    // .at-rows
    float ry = top + topH + 2;
    const RectF rowsClip(cx, ry, cw, rowsH);
    if (rowsN) g.pushClip(rowsClip);
    const RectF saveClip = clip; const bool saveOn = clipOn; clip = rowsClip; clipOn = true;
    auto rowBox = [&](const std::string& sid) {
      const RectF r(cx, ry, cw, rowH);
      // css .at-row.picked (the row F6 scrolls to): #2a2414 with a #8a6a1c border; .at-row:hover #262626
      if (st.asksFocus && st.asksPick == sid && !sid.empty()) { fill(r, Rgba::hex(0x2a2414)); border(r, Rgba::hex(0x8a6a1c)); }
      else if (hov("asks", sid)) fill(r, Rgba::hex(0x262626));
      return r;
    };
    const float tbase0 = 1 + 1 + A(12);
    if (n) for (auto& s : asks) {
      const std::string sid = S(s, "sessionId");
      const RectF r = rowBox(sid);
      const json* t = tileOfSession(sid);
      const json* a = t ? acct(S(*t, "account")) : nullptr;
      const std::wstring ab = a ? W(S(*a, "abbr")) : L"";
      const std::wstring name = W(t ? tileTitle(*t) : (!S(s, "title").empty() ? S(s, "title") : S(s, "name")));
      const bool ended = truthy(s, "ended");
      const std::wstring where = ended ? L"session ended" : t ? L"" : L"outside the deck";
      double latest = 0; const std::wstring age = truthy(s, "latest") && parseTime(O(s, "latest"), latest) ? fmtAge(now - latest) : L"";
      const float b = ry + tbase0; float lx = cx + 1 + 2; const float rEdge = cx + cw - 1 - 2;
      AskMark m = askMark(sid);
      if (m.any) { drawMark(m, lx, b - (1 + baseIn(11, 10))); lx += markW(m); }
      lx += 6;
      const Font abF = F(12, 600), smF = F(11);
      const float abW = tw(ab, abF); tb(ab, lx, b, abW + 2, abF, kDim, Align::Left, false); lx += abW + 6;
      const float ageW = tw(age, smF, true), whereW = tw(where, smF);
      const float nameW = std::max(0.f, rEdge - ageW - 6 - whereW - 6 - lx);
      if (ended) pushOpacity(RectF(lx, ry, nameW, rowH), 0.6f);
      tb(name, lx, b, nameW, F(12), kFg, Align::Left, true);
      if (ended) popOpacity();
      tb(where, rEdge - ageW - 6 - whereW, b, whereW + 2, smF, kFaint, Align::Left, false);
      tbn(age, rEdge - ageW, b, ageW + 2, smF, kFaint);
      zone("asks", sid, r, W(S(s, "name")) + L" · " + W(S(s, "account")) + (ended ? L" · its session has ended: Done saves the answers file, read when the session is resumed or another takes the page over" : t ? L"" : L" · runs outside the deck: Done saves its answers file"));
      ry += rowH + 1;
    }
    if (nLog) {
      const RectF r = rowBox("__recent");
      const float b = ry + tbase0, lx = cx + 1 + 2, rEdge = cx + cw - 1 - 2;
      const std::wstring age = fmtAge(now - N(log[0], "at")); const Font smF = F(11); const float ageW = tw(age, smF, true);
      tb(fmt(L"Recently answered (%d)", (int)nLog), lx, b, std::max(0.f, rEdge - ageW - 6 - lx), F(12), kDim, Align::Left, true);
      tbn(age, rEdge - ageW, b, ageW + 2, smF, kFaint);
      zone("asks", "__recent", r, L"The last pages you answered, every session, read-only");
    }
    clip = saveClip; clipOn = saveOn;
    if (rowsN) g.popClip();
    return h;
  }

  // =============================================================================================
  // the "New terminal" tile (index.html #new-tile, 40-new-tile.js; css .new-tile, .new-accts, .new-links)
  struct AcctBtn { std::string id; std::wstring label, tip; float minW; float x, y, w; };
  std::vector<AcctBtn> layoutAccts(float cx, float y0, float cw, float& hOut) {
    std::vector<AcctBtn> bs; const json& as = O(meta, "accounts");
    const Font bf = F(11, 600);
    if (as.is_array()) for (auto& a : as) {
      AcctBtn b; b.id = S(a, "id"); b.label = W(S(a, "label"));
      b.tip = st.claudeOff ? std::wstring(L"Starting a Claude session is switched off in this local deck. Terminal tiles only.")
        : L"New Claude session in " + W(S(a, "email").empty() ? S(a, "id") : S(a, "email"));
      b.minW = tw(b.label, bf) + 6 + 2; b.x = b.y = b.w = 0; bs.push_back(b);
    }
    // flex-wrap, gap 5, each `flex: 1 1 0; min-width: max-content`: a line takes buttons while their natural
    // widths fit, then shares the line's width equally, no button under its own text
    float y = y0; size_t i = 0;
    while (i < bs.size()) {
      size_t j = i; float used = 0;
      while (j < bs.size() && (j == i || used + 5 + bs[j].minW <= cw + 0.01f)) { used += (j == i ? 0 : 5) + bs[j].minW; j++; }
      const size_t cnt = j - i; float avail = cw - 5.f * (cnt - 1);
      std::vector<bool> frozen(cnt, false); float share = 0;
      for (int pass = 0; pass < 8; pass++) {
        float left = avail; size_t freeN = 0;
        for (size_t k = 0; k < cnt; k++) { if (frozen[k]) left -= bs[i + k].minW; else freeN++; }
        share = freeN ? left / freeN : 0; bool again = false;
        for (size_t k = 0; k < cnt; k++) if (!frozen[k] && bs[i + k].minW > share) { frozen[k] = true; again = true; }
        if (!again) break;
      }
      float bx = cx;
      for (size_t k = 0; k < cnt; k++) { AcctBtn& b = bs[i + k]; b.w = frozen[k] ? b.minW : std::max(b.minW, share); b.x = bx; b.y = y; bx += b.w + 5; }
      y += 17; i = j; if (i < bs.size()) y += 5;
    }
    hOut = y - y0;
    return bs;
  }
  float newTile(float x, float y, float w, bool draw) {
    const float cx = x + 1 + kPadX, cw = w - 2 - 2 * kPadX, top = y + 1 + kPadY;
    // .new-tile .t-title 13px (line-height 1.25); .t-age "+" 16px, line-height 1
    const float tLH = lhMul(1.25f, 13), tAsc = baseIn(tLH, 13), pAsc = baseIn(16, 16);
    const float asc = std::max(tAsc, pAsc), desc = std::max(tLH - tAsc, 16 - pAsc), topH = std::max(17.f, asc + desc);
    float acctH = 0; std::vector<AcctBtn> bs = layoutAccts(cx, top + topH + 1 + 4, cw, acctH);
    const float linksY = top + topH + 1 + 4 + acctH + 1 + 6;
    const float h = (linksY + 16 + kPadY + 1) - y;
    if (!draw) return h;
    const RectF box(x, y, w, h), sbox = snap(box);
    const bool hot = st.hoverPart == "new-tile" || st.hoverPart == "new-acct" || st.hoverPart == "elsewhere" || st.hoverPart == "past";
    fill(box, hot ? Rgba::hex(0x1b1b1b) : Rgba::hex(0x171d27));
    g.rt->DrawRectangle(Gfx::d(sbox.inset(0.5f)), g.b(hot ? Rgba::hex(0x3a3a3a) : Rgba::hex(0x2f3a4a)), 1, ssDash.p);
    zone("new-tile", "", box, L"New " + W(shell) + L" terminal");
    const float base = top + asc;
    const Font plusF = F(16); const float plusW = tw(L"+", plusF);
    tb(L"New terminal", cx, base, cw - plusW - 6, F(13), kDim, Align::Left, true);
    tb(L"+", cx + cw - plusW, base, plusW + 2, plusF, kDim, Align::Left, false);
    for (auto& b : bs) {
      const RectF r(b.x, b.y, b.w, 17);
      if (st.claudeOff) pushOpacity(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 0.4f);   // nothing behind it here: the .none look
      fill(r, hov("new-acct", b.id) && !st.claudeOff ? Rgba::hex(0x303030) : kBtn); border(r, Rgba::hex(0x4a4a4a));
      tIn(b.label, r, F(11, 600), kFg);
      if (st.claudeOff) popOpacity();
      zone("new-acct", b.id, r, b.tip);
    }
    // .new-links: Elsewhere | Past, equal halves, gap 4 (.pull-btn 10.5px, height 16)
    const float half = (cw - 4) / 2;
    // the count each found last time it looked: "Elsewhere [2]"; .pull-btn.none (nothing out there) is 40 %
    const std::wstring ew = st.elsewhereCount > 0 ? fmt(L"Elsewhere [%d]", st.elsewhereCount) : std::wstring(L"Elsewhere");
    const std::wstring pw = st.pastCount > 0 ? fmt(L"Past [%d]", st.pastCount) : std::wstring(L"Past");
    const struct { const wchar_t* label; const char* kind; const wchar_t* tip; int count; } links[2] = {
      { ew.c_str(), "elsewhere", L"Claude sessions running in other windows: bring one in here", st.elsewhereCount },
      { pw.c_str(), "past", L"Sessions on disk that are not open: reopen one", st.pastCount } };
    for (int i = 0; i < 2; i++) {
      const RectF r(cx + i * (half + 4), linksY, half, 16); const bool lh = st.hoverPart == links[i].kind;
      const bool none = links[i].count == 0;
      if (none) pushOpacity(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 0.4f);
      if (lh) fill(r, Rgba::hex(0x262626));
      border(r, Rgba::hex(0x4a4a4a));
      tIn(links[i].label, r, F(10.5f), lh ? kFg : kDim);
      if (none) popOpacity();
      zone(links[i].kind, "", r, links[i].tip);
    }
    return h;
  }

  // =============================================================================================
  // the search pane (index.html #search-pane, 46-search.js; css .search-pane)
  float searchPane(float x, float y, float w, bool draw, int shown, int total) {
    const float h = 4 + 26 + 2;
    if (!draw) return h;
    const std::wstring count = searchWords().empty() ? L"" : fmt(L"%d / %d", shown, total);
    const Font hf = F(11.5f); const float cwd = tw(count, hf);
    const float inH = LN(12.5f) + 6 + 2, inW = std::max(0.f, w - 6 - cwd - 6 - 26);
    const RectF in(x, y + 4 + (26 - inH) / 2, inW, inH);
    fill(in, Rgba::hex(0x0f0f0f)); border(in, Rgba::hex(0x333333));
    const RectF txt(in.x + 1 + 6, in.y, std::max(0.f, in.w - 14), in.h);
    if (st.searchEdit) {
      const bool foc = st.focusEdit == st.searchEdit;
      if (foc) border(RectF(in.x - 1, in.y - 1, in.w + 2, in.h + 2), Rgba::hex(0x555555));   // input:focus { outline: 1px solid #555 }
      st.searchEdit->placeholder = L"search sessions (title, message, account)";
      st.searchEdit->draw(g, txt, F(12.5f), kFg, Rgba::hex(0x757575), foc, st.blinkOn);
    } else if (st.search.empty()) tIn(L"search sessions (title, message, account)", txt, F(12.5f), Rgba::hex(0x757575), Align::Left, false);
    else tIn(W(st.search), txt, F(12.5f), kFg, Align::Left, false);
    zone("tool", "search-input", in);
    tIn(count, RectF(x + inW + 6, y + 4, cwd + 2, 26), hf, Rgba::hex(0x8c8c8c), Align::Left, false);
    const RectF clr(x + w - 26, y + 4, 26, 26); const bool hot = hov("tool", "search-clear");
    if (hot) fill(clr, Rgba::hex(0x1f1f1f));
    tIn(L"×", clr, F(13), hot ? kFg : kDim);
    zone("tool", "search-clear", clr, L"Clear");
    return h;
  }

  // =============================================================================================
  // the restart strip (82-restart.js renderRestartStrip; css .restart-strip, .rs-*)
  bool stripShown() const { const json& r = O(meta, "restart"); return r.is_object() && (truthy(r, "armed") || truthy(r, "update")); }
  float restartStrip(float x, float y, float w, bool draw) {
    const json& r = O(meta, "restart");
    const bool armed = truthy(r, "armed");
    const json& blockers = O(r, "blockers");
    const size_t rowsN = armed && blockers.is_array() ? blockers.size() : 0;
    std::wstring files;
    if (O(O(r, "update"), "files").is_array()) for (auto& f : O(O(r, "update"), "files")) { if (!files.empty()) files += L", "; if (f.is_string()) files += W(f.get<std::string>()); }
    std::wstring title;
    if (!armed) title = L"Deck update waiting: " + files;
    else if (rowsN) title = std::wstring(S(r, "why") == "update" ? L"Update: restart" : L"Restart") + fmt(L" when quiet, waiting on %d", (int)rowsN);
    else title = fmt(L"Quiet: restarting in %d s", std::max(1, (int)std::ceil(N(r, "settleLeftMs") / 1000.0)));
    const bool offer = !armed, settling = armed && !rowsN;
    const float cx = x + 1 + 6, cw = w - 2 - 12; float yy = y + 1 + 5;
    const Font tf = F(12, 600), rf = F(12);
    const float lh = LN(12);
    if (draw) {
      // the height is needed first for the box: measure, then paint
      const float h = restartStrip(x, y, w, false);
      fill(RectF(x, y, w, h), offer ? Rgba::hex(0x0d1a10) : Rgba::hex(0x1a1608));
      border(RectF(x, y, w, h), offer ? Rgba::hex(0x1f6b33) : Rgba::hex(0x6b5518));
    }
    yy += wrap(title, cx, yy, cw, tf, offer ? kGreen : settling ? kFg : Rgba::hex(0xfbbf24), lh, 0, true, draw);
    if (rowsN) {
      yy += 3;
      size_t k = 0;
      for (auto& b : blockers) {
        // .rs-row: title (flex 1 1 auto) · reason (flex 0 1 auto, right-aligned); both wrap when the row is narrow
        const std::wstring t = W(S(b, "title")), why = W(S(b, "reason"));
        const float inner = cw - 4; float tW = tw(t, rf), rW = tw(why, rf);
        const float over = tW + rW + 8 - inner;
        if (over > 0 && tW + rW > 0) { const float k2 = over / (tW + rW); tW -= tW * k2; rW -= rW * k2; }
        const float titleW = std::max(1.f, inner - 8 - rW);
        const float h1 = wrap(t, cx + 2, yy + 1, titleW, rf, kFg, lh, 0, true, false), h2 = wrap(why, cx + 2 + titleW + 8, yy + 1, std::max(1.f, rW), rf, kDim, lh, 0, true, false);
        const float rh = std::max(lh, std::max(h1, h2)) + 2;
        if (draw) {
          const std::string bid = S(b, "id");
          if (st.hoverPart == "restart" && st.hover == "row:" + bid) fill(RectF(cx, yy, cw, rh), Rgba::hex(0x2a2410));
          wrap(t, cx + 2, yy + 1, titleW, rf, kFg, lh, 0, true, true);
          if (h2 <= lh) tb(why, cx + 2 + titleW + 8 - 2, yy + 1 + baseIn(lh, 12), std::max(1.f, rW) + 2, rf, kDim, Align::Right, false);
          else wrap(why, cx + 2 + titleW + 8, yy + 1, std::max(1.f, rW), rf, kDim, lh, 0, true, true, nullptr, Align::Right);
          zone("restart", "row", RectF(cx, yy, cw, rh), L"Show this session", bid);
        }
        yy += rh; if (++k < rowsN) yy += 1;
      }
    }
    yy += 3;
    // .rs-actions: flex-wrap, justify flex-end, gap 4px 5px; the note is `flex: 1 1 110px`
    const double shells = N(r, "shells");
    const std::wstring note = shells ? fmt(L"%d terminal%ls restart%ls empty", (int)shells, shells == 1 ? L"" : L"s", shells == 1 ? L"s" : L"") : std::wstring();
    struct B { const wchar_t* label; const char* act; float w; };
    std::vector<B> btns;
    const Font bf = F(11), nf = F(11.5f);
    if (!armed) { btns.push_back({ L"Restart when quiet", "arm", 0 }); btns.push_back({ L"Later", "later", 0 }); }
    const bool confirming = armed && st.restartConfirming && rowsN;      // "Now" past a blocker takes a second click
    const std::wstring stopLabel = fmt(L"Stop %d and restart", (int)rowsN);
    if (armed) { btns.push_back({ confirming ? stopLabel.c_str() : L"Now", "now", 0 }); btns.push_back({ L"Cancel", "cancel", 0 }); }
    for (auto& b : btns) b.w = tw(b.label, bf) + 14 + 2;
    // line 1 starts with the note (110px basis); buttons join while they fit, the rest wrap
    std::vector<std::vector<int>> lines(1); float used = 110; bool noteLine = true;
    for (int i = 0; i < (int)btns.size(); i++) {
      if (used + 5 + btns[i].w > cw + 0.01f && !(lines.back().empty() && !noteLine)) { lines.push_back({}); used = -5; noteLine = false; }
      lines.back().push_back(i); used += 5 + btns[i].w;
    }
    for (size_t li = 0; li < lines.size(); li++) {
      float bw = 0; for (int i : lines[li]) bw += btns[i].w + 5;
      const bool first = li == 0;
      float noteW = first ? std::max(0.f, cw - bw) : 0;
      float lineH = lines[li].empty() ? 0 : 18;
      if (first && !note.empty()) lineH = std::max(lineH, wrap(note, cx, yy, std::max(1.f, noteW), nf, kDim, LN(11.5f), 0, false, false));
      if (first && note.empty() && lines[li].empty()) { continue; }   // an empty note alone takes no height
      if (draw) {
        if (first && !note.empty()) { const float nh = wrap(note, cx, yy, std::max(1.f, noteW), nf, kDim, LN(11.5f), 0, false, false); wrap(note, cx, yy + (lineH - nh) / 2, std::max(1.f, noteW), nf, Rgba::hex(0x8c8c8c), LN(11.5f), 0, false, true); }
        float bx = cx + cw; for (int i : lines[li]) bx -= btns[i].w; bx -= 5.f * (lines[li].empty() ? 0 : lines[li].size() - 1);
        for (int i : lines[li]) {
          const RectF br(bx, yy + (lineH - 18) / 2, btns[i].w, 18); const bool hot = hov("restart", btns[i].act);
          const bool dng = confirming && std::string(btns[i].act) == "now";   // .rs-actions button.danger
          fill(br, hot ? Rgba::hex(0x333333) : kBtn); border(br, dng ? kDanger : kBtnBorder);
          tIn(btns[i].label, br, bf, dng ? Rgba::hex(0xf2b3b3) : kFg);
          zone("restart", btns[i].act, br);
          bx += btns[i].w + 5;
        }
      }
      yy += lineH; if (li + 1 < lines.size()) yy += 4;
    }
    yy += 5 + 1;
    return yy - y;
  }

  // =============================================================================================
  // #sb-tools (index.html; css .collapse-btn, .icon-btn, #…-btn states; 80-sidebar.js, 82-restart.js tooltips)
  void tools(float x, float y, float w) {
    const json& r = O(meta, "restart");
    const bool restartSupported = r.is_object(), armed = truthy(r, "armed"), offer = !armed && truthy(r, "update");
    std::string driver = S(settings, "driver"); if (driver.empty()) driver = "auto";
    struct T { std::string id; float w; std::wstring tip; float minW = 0; };
    const std::wstring clock = fmtClock(now); const Font cf = F(12.5f);
    const float clockW = tw(clock, cf, true);
    std::vector<T> ts;
    ts.push_back({ "collapse", 5 + 14 + 5 + clockW + 8, L"Hide sidebar" });
    ts.push_back({ "search-btn", 26, L"Search sessions (local)" });
    ts.push_back({ "groups-btn", 26, L"Grouped view (left click) · new group (right click)" });
    ts.push_back({ "msgs-btn", 26, L"Show / hide recent messages" });
    ts.push_back({ "bars-btn", 26, L"Show / hide usage bars" });
    if (!deck::edition::kPublic) ts.push_back({ "moveall-btn", 26, L"Open all sessions of one account as another" });   // the public build has no move-all
    // A deck that speaks the claim's words (ui.hpp newDriverWords; branch native-remote-server, 80-sidebar.js
    // driveName / driveTitle / paintDrive): the button is this screen's claim. DRIVE_ME is 'remote' for the exe as a
    // remote desk and 'office' for the local deck's own window (which speaks these words only with the core's
    // driverClaim on). The sentences are the branch's.
    const bool claim = newDriverWords(meta);
    const std::string driveMe = st.officeScreen ? "office" : "remote";
    if (claim) driver = driverMode(driver);
    std::wstring driveTip;
    if (claim) {
      const std::string rn = remoteDeskName(meta);
      const std::wstring office = driveMe == "office" ? L"this PC (the office deck)" : L"the office deck";
      const std::wstring remote = (driveMe == "remote" ? std::wstring(L"this remote desk") : std::wstring(L"the remote desk")) + (rn.empty() ? std::wstring() : L" (" + W(rn) + L")");
      const std::wstring click = driver == driveMe ? L"Click to let whoever is looking drive (auto)." : L"Click to drive from here.";
      if (driver == "auto") driveTip = L"Auto: a phone attached to a session drives its size, else a remote desk showing it, else " + office + L". Click to drive from here.";
      else if (driver == "phone") driveTip = L"Sizes follow: the phone. Every other screen shows its size, and typing at the office deck is off. " + click;
      else if (driver == "remote") driveTip = L"Sizes follow: " + remote + L". That is for the sessions it shows: there the office deck shows its size and cannot type. The office deck sizes the rest. " + click;
      else driveTip = L"Sizes follow: " + office + L". The phone and a remote desk show its size. " + click;
    } else driveTip = driver == "phone" ? L"Phone: the phone drives every size; this window mirrors. Click for desk."
      : driver == "desk" ? L"Desk: the phone only views; this is always the desktop view. Click for auto."
      : L"Auto: the phone drives the tile it is attached to; this window mirrors that one. Click for phone.";
    ts.push_back({ "drive-btn", 26, driveTip });
    if (restartSupported) ts.push_back({ "restart-btn", 26, armed ? L"Restart armed: it goes when every session is quiet. Click to cancel."
      : offer ? L"The deck's code changed on disk. Click to restart when every session is quiet." : L"Restart the deck when every session is quiet" });
    ts.push_back({ "settings-btn", 26, L"Settings" });
    if (st.claudeOff) for (auto& t : ts) {
      if (t.id == "moveall-btn") t.tip = L"Claude sessions are switched off in this local deck: there are none to move";
    }
    if (sbRight) std::reverse(ts.begin(), ts.end());   // body.sb-right #sb-tools { flex-direction: row-reverse }
    // too narrow for the row: the buttons shrink (flex-shrink 1, in proportion to their width) down to their icon;
    // the collapse button is all padding and content, so it keeps its width
    for (auto& t : ts) t.minW = t.id == "collapse" ? t.w : t.id == "settings-btn" ? 16.f : 15.f;
    float sum = 0; for (auto& t : ts) sum += t.w;
    if (sum > w) {
      std::vector<bool> frozen(ts.size(), false);
      for (int pass = 0; pass < 10; pass++) {
        float fixedW = 0, flexW = 0;
        for (size_t i = 0; i < ts.size(); i++) { if (frozen[i]) fixedW += ts[i].minW; else flexW += ts[i].w; }
        if (flexW <= 0) break;
        const float k = std::max(0.f, (w - fixedW) / flexW); bool again = false;
        for (size_t i = 0; i < ts.size(); i++) if (!frozen[i] && ts[i].w * k < ts[i].minW) { frozen[i] = true; again = true; }
        if (!again) { for (size_t i = 0; i < ts.size(); i++) ts[i].w = frozen[i] ? ts[i].minW : ts[i].w * k; break; }
        if (pass == 9) for (size_t i = 0; i < ts.size(); i++) ts[i].w = ts[i].minW;
      }
      { bool all = true; for (bool b : frozen) all = all && b; if (all) for (auto& t : ts) t.w = t.minW; }
      sum = 0; for (auto& t : ts) sum += t.w;
    }
    const float gap = ts.size() > 1 ? std::max(0.f, (w - sum) / (ts.size() - 1)) : 0;   // justify-content: space-between
    float bx = sbRight && sum > w ? x + w - sum : x;
    for (auto& t : ts) {
      const bool hot = hov("tool", t.id);
      if (t.id == "collapse") {
        const RectF br(bx, y + 1, t.w, 24);
        if (hot) fill(br, Rgba::hex(0x1f1f1f));
        const Rgba c = hot ? kFg : kDim;
        toolIcon("collapse", rnd(br.x + 5), rnd(br.y + 5), c, false);
        tIn(clock, RectF(br.x + 5 + 14 + 5, br.y, clockW + 2, 24), cf, c, Align::Left, false, true);
        zone("tool", t.id, br, t.tip);
      } else {
        const RectF br(bx, y, t.w, 26);
        Rgba c = hot ? kFg : kDim, bg = hot ? Rgba::hex(0x1f1f1f) : kNone; bool off = false;
        if (t.id == "search-btn" && st.searchOpen) { c = kFg; bg = Rgba::hex(0x262626); }
        if (t.id == "groups-btn" && st.grouped) { c = kFg; bg = Rgba::hex(0x262626); }
        if (t.id == "msgs-btn" && !st.showMsgs) { c = kFaint; off = true; }
        if (t.id == "bars-btn" && !st.showBars) { c = kFaint; off = true; }
        std::string icon = t.id;
        if (t.id == "drive-btn" && claim) {             // css #drive-btn.drive-mine / .drive-other / .drive-auto; the icon is WHO drives
          if (driver == driveMe) c = kGreen; else if (driver != "auto") c = kFaint; else c = kDim;
          icon = driver == "office" ? "drive-office" : driver == "remote" ? "drive-remote" : driver == "auto" ? "drive-auto" : "drive-btn";
        } else if (t.id == "drive-btn") { if (driver == "phone") c = kGreen; else if (driver == "desk") c = kFaint; }
        if (t.id == "restart-btn") { if (armed) c = Rgba::hex(0xfbbf24); else if (offer) c = kGreen; }
        if (st.claudeOff && t.id == "moveall-btn") { c = Rgba::hex(0x3c3c3c); bg = kNone; }   // not here: the icon stays, unlit
        fill(br, bg);
        const float isz = t.id == "settings-btn" ? 16.f : 15.f;
        // css body.anim-on #restart-btn.armed svg: rs-turn, one turn in 2.4 s, linear
        if (t.id == "restart-btn" && armed && st.animOn) { iconRot = (float)(std::fmod(st.animMs, 2400.0) / 2400.0 * 360.0); animRegion(br); }
        toolIcon(icon, rnd(br.x + (br.w - isz) / 2), rnd(br.y + (26 - isz) / 2), c, off);   // the svg box is snapped to the pixel grid
        iconRot = 0;
        if (t.id == "restart-btn" && offer) fill(RectF(br.r() - 4 - 5, br.y + 4, 5, 5), kGreen);   // #restart-btn.offer::after
        zone("tool", t.id, br, t.tip);
      }
      bx += t.w + gap;
    }
  }

  // =============================================================================================
  struct Item { bool group; const json* t; std::string name; int count; float h; };

  void paint() {
    textKept().trim();
    sb.zones_.clear(); sb.order_.clear(); sb.spans_.clear(); sb.contentH_ = 0; sb.viewH_ = 0; sb.anim_.clear(); part.kind = -1;
    // css transitions: what this draw does not draw is forgotten (a tile that went, a group that was removed)
    sb.transBusy_ = false;
    if (!st.transOn) sb.trans_.clear(); else for (auto& kv : sb.trans_) kv.second.seen = false;
    struct TransPrune { Sidebar& s; ~TransPrune() { for (auto it = s.trans_.begin(); it != s.trans_.end();) { if (!it->second.seen) it = s.trans_.erase(it); else ++it; } } } transPrune{ sb };
    g.pushClip(area);
    fill(area, kSb);

    if (!st.sidebarOpen) {   // #rail: 14px, the whole strip is the button; the arrow sits at the bottom
      const bool hot = hov("tool", "expand");
      if (hot) fill(area, Rgba::hex(0x1a1a1a));
      toolIcon("expand", rnd(area.x + (area.w - 11) / 2), rnd(area.b() - 8 - 22 + (22 - 11) / 2.f), kDim, false);
      zone("tool", "expand", area, L"Show sidebar");
      g.popClip();
      return;
    }

    const float X = area.x, Wd = area.w, inX = X + kPadX, inW = Wd - 2 * kPadX;

    // ---- which tiles, in which order (renderTiles → applySearchFilter → layoutGroups)
    const json& tiles = O(meta, "tiles");
    const std::vector<std::string> words = searchWords();
    std::vector<Item> items; int shown = 0, totalTiles = 0;
    std::vector<const json*> all;
    if (tiles.is_array()) for (auto& t : tiles) all.push_back(&t);
    totalTiles = (int)all.size();
    for (auto* t : all) if (matches(*t, words)) shown++;
    if (!st.grouped) {
      for (auto* t : all) if (matches(*t, words)) items.push_back({ false, t, "", 0, 0 });
    } else {
      std::vector<std::pair<std::string, std::vector<const json*>>> sections;
      sections.push_back({ "pinned", {} });
      const json& groups = O(meta, "groups");
      if (groups.is_array()) for (auto& gname : groups) if (gname.is_string()) {
        bool dup = false; for (auto& s : sections) if (s.first == gname.get<std::string>() && s.first != "pinned") dup = true;
        if (!dup) sections.push_back({ gname.get<std::string>(), {} });
      }
      std::vector<const json*> ungrouped;
      for (auto* t : all) {
        if (truthy(*t, "pinned")) { sections[0].second.push_back(t); continue; }
        const std::string gr = S(*t, "group"); bool placed = false;
        if (!gr.empty()) for (size_t i = 1; i < sections.size(); i++) if (sections[i].first == gr) { sections[i].second.push_back(t); placed = true; break; }
        if (!placed) ungrouped.push_back(t);
      }
      sections.push_back({ "ungrouped", ungrouped });
      for (auto& s : sections) {
        if (s.second.empty()) continue;
        items.push_back({ true, nullptr, s.first, (int)s.second.size(), 0 });
        if (st.collapsedGroups.count(s.first)) continue;                 // .tile.in-collapsed
        for (auto* t : s.second) if (matches(*t, words)) items.push_back({ false, t, "", 0, 0 });
      }
    }

    // ---- the fixed blocks: the asks tile on top, #sb-bottom at the bottom
    float y = area.y;
    const json& showTile = O(O(settings, "asks"), "showTile");
    const bool asksOn = !(showTile.is_boolean() && !showTile.get<bool>());
    float asksH = 0;
    if (asksOn) { asksH = asksTile(inX, y + 8, inW, false); }

    const float newH = newTile(inX, 0, inW, false);
    const float searchH = st.searchOpen ? searchPane(inX, 0, inW, false, 0, 0) : 0;
    const bool strip = stripShown();
    const float stripH = strip ? restartStrip(inX, 0, inW, false) : 0;
    // #sb-bottom: border-top 1px, padding 4px pad-x 6px, gap 4
    float bottomH = 1 + 4 + newH + (st.searchOpen ? 4 + searchH : 0) + (strip ? 4 + 4 + stripH + 2 : 0) + 4 + 26 + 6;
    const float bottomY = area.b() - bottomH;
    sb.bottomTop_ = bottomY;

    // ---- #tiles: padding 8px pad-x 6px, gap 4, scrolled, no scrollbar (#tiles::-webkit-scrollbar { display: none })
    const float listTop = y + (asksOn ? 8 + asksH : 0);
    const RectF view(X, listTop, Wd, std::max(0.f, bottomY - listTop));
    float content = 8;
    for (size_t i = 0; i < items.size(); i++) {
      Item& it = items[i];
      it.h = it.group ? groupBar(it.name, it.count, inX, 0, inW, false) : tile(*it.t, inX, 0, inW, false, 0);
      content += it.h + (i + 1 < items.size() ? 4 : 0);
    }
    content += 6;
    sb.contentH_ = content; sb.viewH_ = view.h;
    const float scroll = std::max(0.f, std::min(st.scroll, content - view.h));

    if (view.h > 0) {
      g.pushClip(view); clip = view; clipOn = true;
      float ly = 8;
      for (auto& it : items) {
        const float sy = listTop + ly - rnd(scroll);
        if (!it.group) { const std::string id = S(*it.t, "id"); sb.order_.push_back(id); sb.spans_[id] = { ly, ly + it.h }; }
        if (sy + it.h + 8 >= view.y && sy - 8 <= view.b()) {
          if (it.group) groupBar(it.name, it.count, inX, sy, inW, true);
          else { part = { 0, S(*it.t, "id"), RectF(), RectF(), inX, sy, inW, it.h }; tile(*it.t, inX, sy, inW, true, it.h); part.kind = -1; }
        }
        ly += it.h + 4;
      }
      clipOn = false; g.popClip();
    }

    if (asksOn) { part = { 1, "", RectF(), RectF(), inX, y + 8, inW, asksH }; asksTile(inX, y + 8, inW, true); part.kind = -1; }

    // ---- #sb-bottom
    fill(RectF(X, bottomY, Wd, bottomH), kSb);
    fill(RectF(X, bottomY, Wd, 1), Rgba::hex(0x161616));
    float by = bottomY + 1 + 4;
    newTile(inX, by, inW, true); by += newH + 4;
    if (st.searchOpen) { searchPane(inX, by, inW, true, shown, totalTiles); by += searchH + 4; }
    if (strip) { by += 4; restartStrip(inX, by, inW, true); by += stripH + 2 + 4; }
    { const float tw2 = st.toolsWidth > 0 ? std::min(inW, st.toolsWidth) : inW; part = { 2, "", RectF(), RectF(), inX, by, tw2, 0 }; tools(inX, by, tw2); part.kind = -1; }

    // ---- #sb-resize: 5px on the inner edge, above everything
    const RectF rz(sbRight ? X : X + Wd - 5, area.y, 5, area.h);
    if (st.hoverPart == "resizer") fill(rz, Rgba::hex(0x2a2a2a, 0x88 / 255.f));
    zone("resizer", "", rz, L"Drag to resize");
    g.popClip();
  }
};

// ================================================================================================
// Overlays. Each is a model in ui.hpp that the app fills; the painter here draws it with the sidebar's own
// text and box rules (a scratch Sidebar takes the zones a painter lays down, which an overlay reads back).
namespace {
// box-shadow: 0 <dy>px <blur>px rgba(0,0,0,a): a soft dark edge, enough to lift a menu off a dark page
void shadow(Gfx& g, const RectF& r, float dy, float blur, float a) {
  const int n = 6;
  for (int i = n; i >= 1; i--) { const float e = blur * 0.5f * i / n; g.fillRound(RectF(r.x - e, r.y - e + dy, r.w + 2 * e, r.h + 2 * e), e, Rgba(0, 0, 0, a / n * 0.9f)); }
}
}  // namespace

// css .ctx-menu (35-groups.js showCtxMenu / promptInline): #1c1c1c, 1px #3a3a3a, padding 3, min-width 150, 12.5px;
// .ctx-item padding 5px 10px; .ctx-sep 1px #333 with 3px above and below; a checked row starts with a green tick.
void drawCtxMenu(Gfx& g, const json& meta, const State& st, CtxMenu& m, float viewW, float viewH) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, RectF(0, 0, viewW, viewH));
  const Font f = p.F(12.5f);
  const float lh = p.LN(12.5f);
  m.rows.assign(m.items.size(), RectF());
  if (m.prompt) {
    // .ctx-input: 180 wide, #0f0f0f, 1px #333, padding 3px 6px (the menu is as big as the box plus its own padding)
    const float inH = lh + 6 + 2;
    const RectF box = p.snap(RectF(m.x, m.y, 1 + 3 + 180 + 3 + 1, 1 + 3 + inH + 3 + 1));
    shadow(g, box, 8, 24, 0.667f);
    p.fill(box, Rgba::hex(0x1c1c1c)); p.border(box, Rgba::hex(0x3a3a3a));
    const RectF in(box.x + 4, box.y + 4, 180, inH);
    const bool foc = st.focusEdit == &m.input;
    if (foc) p.border(RectF(in.x - 1, in.y - 1, in.w + 2, in.h + 2), Rgba::hex(0x555555));   // input:focus { outline: 1px solid #555 }
    p.fill(in, Rgba::hex(0x0f0f0f)); p.border(in, Rgba::hex(0x333333));
    m.input.draw(g, RectF(in.x + 7, in.y + 1, in.w - 14, in.h - 2), f, kFg, Rgba::hex(0x757575), foc, st.blinkOn);
    m.box = box;
    return;
  }
  const std::wstring tick = L"\x2713 ";
  float w = 150, h = 2 + 6;
  for (auto& it : m.items) {
    if (it.sep) { h += 7; continue; }
    w = std::max(w, std::ceil(p.tw((it.checked ? tick : std::wstring()) + it.label, f)) + 20 + 6 + 2 + (it.sub.empty() ? 0.f : 22.f));
    h += lh + 10;
  }
  const RectF box = p.snap(RectF(std::max(0.f, std::min(m.x, viewW - w - 6)), std::max(0.f, std::min(m.y, viewH - h - 6)), w, h));
  shadow(g, box, 8, 24, 0.667f);
  p.fill(box, Rgba::hex(0x1c1c1c)); p.border(box, Rgba::hex(0x3a3a3a));
  float y = box.y + 4;
  for (size_t i = 0; i < m.items.size(); i++) {
    const MenuItem& it = m.items[i];
    if (it.sep) { p.fill(RectF(box.x + 4, y + 3, box.w - 8, 1), Rgba::hex(0x333333)); y += 7; continue; }
    const RectF row(box.x + 4, y, box.w - 8, lh + 10);
    if ((int)i == m.hover) p.fill(row, Rgba::hex(0x2a2a2a));
    float tx = row.x + 10; const float base = row.y + 5 + p.baseIn(lh, 12.5f);
    if (it.checked) { const float cw = p.tw(tick, f); p.tb(tick, tx, base, cw + 2, f, kGreen, Align::Left, false); tx += cw; }
    p.tb(it.label, tx, base, row.r() - 10 - tx + 2 - (it.sub.empty() ? 0.f : 22.f), f, it.dim ? kDim : kFg, Align::Left, false);
    if (!it.sub.empty()) p.tb(L"\x25B8", row.r() - 10 - 12, base, 12, f, kDim, Align::Right, false);   // .ctx-item.sub::after
    m.rows[i] = row;
    y += lh + 10;
  }
  m.box = box;
}

#include "overlays.hpp"
#include "settings.hpp"

float paneAskMark(Gfx& g, const json& meta, const State& st, const std::string& sessionId, float x, float top, bool draw) {
  const json& sm = O(O(O(meta, "settings"), "asks"), "showMarks");
  if (sm.is_boolean() && !sm.get<bool>()) return 0;
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, RectF(0, 0, 100000, 100000));
  const AskMark m = p.askMark(sessionId);
  if (!m.any) return 0;
  if (draw) p.drawMark(m, x, top);
  return p.markW(m);
}
void drawTileGhost(Gfx& g, const json& meta, const State& st0, const std::string& tileId, float x, float y, float w) {
  State st = st0; st.dragging.clear(); st.dropMark.clear(); st.hover.clear(); st.hoverPart.clear(); st.mouseX = st.mouseY = -1;
  if (st.renaming == tileId) st.renaming.clear();
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, RectF(x, y, w, 100000));
  const json& tiles = O(meta, "tiles");
  if (!tiles.is_array()) return;
  for (auto& t : tiles) if (S(t, "id") == tileId) {
    const float h = p.tile(t, x, y, w, false, 0);
    p.pushOpacity(RectF(x - 8, y - 8, w + 16, h + 16), 0.6f);
    p.tile(t, x, y, w, true, h);
    p.popOpacity();
    return;
  }
}
void drawCloudMark(Gfx& g, float x, float y, float size, const Rgba& c) {
  Com<ID2D1PathGeometry> geo = svgGeom(g.d2d.p, kCloudPath, true);
  if (!geo) return;
  g.rt->SetTransform(D2D1::Matrix3x2F::Scale(size / 16, size / 16) * D2D1::Matrix3x2F::Translation(x, y));
  g.rt->FillGeometry(geo.p, g.b(c));
  g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
}
void dropTextKept() { TextKept& k = textKept(); k.widths.clear(); k.lines.clear(); k.wraps.clear(); }
std::string textKeptNote() { const TextKept& k = textKept(); return "widths " + std::to_string(k.widths.size()) + " lines " + std::to_string(k.lines.size()) + " wraps " + std::to_string(k.wraps.size()); }
std::wstring fmtAgeMs(double ms) { return fmtAge(ms); }
std::wstring fmtHeldTimeMs(double ms, double now) { return fmtHeldTime(ms, now); }
bool parseTimeMs(const json& v, double& ms) { return parseTime(v, ms); }

// ================================================================================================
void Sidebar::draw(Gfx& g, const RectF& area, const json& meta, const State& st) {
  SidebarPainter p(g, *this, meta, st, area);
  p.paint();
}
// Each part that moves with the animation clock, drawn again by itself: the same painter and the same call draw()
// made for it, at the place draw() put it, under a clip on device pixels (so what it paints is what a whole draw()
// paints there). Nothing is measured or laid out again. The zones and the order draw() recorded stay as they are.
int Sidebar::drawAnim(Gfx& g, const RectF& area, const json& meta, const State& st) const {
  if (anim_.empty()) return 0;
  Sidebar scratch;
  SidebarPainter p(g, scratch, meta, st, area);
  p.onlyOn = true; p.part.kind = -1;
  const json& tiles = O(meta, "tiles");
  int n = 0;
  // a part's rectangle on device pixels, inside the area and the clip it was drawn under
  auto px = [&](const AnimPart& a) {
    const float x0 = std::max(std::max(area.x, a.clip.x), g.snapDown(a.region.x)), y0 = std::max(std::max(area.y, a.clip.y), g.snapDown(a.region.y));
    const float x1 = std::min(std::min(area.r(), a.clip.r()), g.snapUp(a.region.r())), y1 = std::min(std::min(area.b(), a.clip.b()), g.snapUp(a.region.b()));
    return RectF(x0, y0, x1 - x0, y1 - y0);
  };
  auto meets = [](const RectF& a, const RectF& b) { return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.r() && b.x < a.r() && a.y < b.b() && b.y < a.b(); };
  for (const AnimPart& a : anim_) {
    const RectF r = px(a);
    if (r.w <= 0 || r.h <= 0) continue;
    g.pushClip(r);
    g.fill(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), kSb);
    // the part itself, and any other moving part whose rectangle reaches into this one (a ring's edge at a
    // fractional scale), in the order draw() drew them
    for (const AnimPart& b : anim_) {
      if (&b != &a && !meets(px(b), r)) continue;
      if (b.kind == 0) {
        const json* t = nullptr;
        if (tiles.is_array()) for (auto& x : tiles) if (S(x, "id") == b.id) { t = &x; break; }
        if (t) { g.pushClip(b.clip); p.clip = b.clip; p.clipOn = true; p.tile(*t, b.x, b.y, b.w, true, b.h); p.clipOn = false; g.popClip(); }
      } else if (b.kind == 1) p.asksTile(b.x, b.y, b.w, true);
      else p.tools(b.x, b.y, b.w);
    }
    g.popClip();
    n++;
  }
  return n;
}

float Sidebar::minWidth(Gfx& g, const json& meta, const State& st) {
  if (!g.rt || !g.dw) return 0;
  Sidebar scratch;
  SidebarPainter p(g, scratch, meta, st, RectF());
  // .tile: border 1 + padding pad-x each side; #tiles: padding pad-x each side
  return std::max(160.f, std::ceil(p.widestStrip() + 2 + 4 * kPadX));
}

Hit Sidebar::hitTest(float x, float y) const {
  for (auto it = zones_.rbegin(); it != zones_.rend(); ++it) if (it->hit.rect.has(x, y)) return it->hit;
  return Hit();
}

// the title of the innermost element under the point that has one (a tile part inherits nothing: the tile has no title)
std::wstring Sidebar::tooltipAt(float x, float y) const {
  for (auto it = zones_.rbegin(); it != zones_.rend(); ++it) if (it->hit.rect.has(x, y)) {
    if (!it->tip.empty()) return it->tip;
    if (it->hit.kind != "tile") return L"";
  }
  return L"";
}

bool Sidebar::tileRect(const std::string& id, RectF& out) const {
  for (const auto& z : zones_) if (z.hit.kind == "tile" && z.hit.id == id) { out = z.hit.rect; return true; }   // the box is the tile's first zone
  return false;
}
const Hit* Sidebar::find(const std::string& kind, const std::string& id, const std::string& arg) const {
  const Hit* box = nullptr;
  for (auto it = zones_.rbegin(); it != zones_.rend(); ++it) {
    const Hit& h = it->hit;
    if (h.kind != kind || (id != "-" && h.id != id) || (arg != "-" && h.arg != arg)) continue;
    if (kind != "tile") return &h;
    box = &h;                                             // a tile: its box, which is the first zone it laid down
  }
  return box;
}

bool Sidebar::tileSpan(const std::string& id, float& top, float& bottom) const {
  auto it = spans_.find(id);
  if (it == spans_.end()) return false;
  top = it->second.first; bottom = it->second.second;
  return true;
}

} }  // namespace deck::ui
