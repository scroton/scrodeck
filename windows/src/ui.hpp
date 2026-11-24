// windows/src/ui.hpp — what the views share: the view state and the sidebar's interface.
//
// The deck's snapshot (`meta`, main.js collectMeta) is kept as JSON and read by field name, the same
// names renderer/js/** reads, so a view here is checked against the JS line by line. Nothing in a view
// talks to the socket: a click comes back from hitTest() as a Hit and app.cpp turns it into a call.
#pragma once
#include "gfx.hpp"
#include "edit.hpp"
#include "../../core/third_party/nlohmann/json.hpp"
#include "../../core/deck/edition.hpp"               // the private edition or the public build (deck::edition::kPublic)
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>

namespace deck { namespace ui {

using json = nlohmann::json;

// ---- who drives the size: the two sets of words a deck may speak (core/PROTOCOL.md "Who drives the size")
// An OLD deck (origin/main today): meta.settings.driver is auto | phone | desk, and the button cycles them.
// A NEW deck (branch native-remote-server, the driver as a CLAIM; not merged at the time of writing): it is
// auto | office | phone | remote, every tile carries `sizeBy`, and meta carries `remoteDesk` ({ name } or null).
// Told apart by what the meta itself says, so the exe works with either: any one of those three marks = new.
// (All three are on every meta a new deck sends, so nothing has to be remembered between metas.)
inline bool newDriverWords(const json& meta) {
  if (!meta.is_object()) return false;
  if (meta.contains("remoteDesk")) return true;
  auto s = meta.find("settings");
  if (s != meta.end() && s->is_object()) { auto d = s->find("driver"); if (d != s->end() && d->is_string()) { const std::string v = d->get<std::string>(); if (v == "office" || v == "remote") return true; } }
  auto t = meta.find("tiles");
  if (t != meta.end() && t->is_array()) for (const auto& x : *t) if (x.is_object() && x.contains("sizeBy")) return true;
  return false;
}
// 80-sidebar.js driverMode (the branch): one of the four words; 'desk' (the word before the claim) reads as 'office'
inline std::string driverMode(const std::string& v) { return v == "desk" ? "office" : (v == "auto" || v == "office" || v == "phone" || v == "remote") ? v : "auto"; }
// the name the connected remote desk gave (meta.remoteDesk.name), '' when none
inline std::string remoteDeskName(const json& meta) {
  if (!meta.is_object()) return "";
  auto r = meta.find("remoteDesk"); if (r == meta.end() || !r->is_object()) return "";
  auto n = r->find("name"); return n != r->end() && n->is_string() ? n->get<std::string>() : "";
}

// 30-tiles.js pauseWords: a tile's pause as its second line says it (UTF-8), "" when it has none. "‖ pausing" /
// "‖ paused", ", resume queued" / " until restart", or "resuming". The mark stays until the session gets another
// prompt (lib/pause.js; 2026-11-24) - the deck's own PAUSE, a pasted one and a typed one alike; a remote draws
// it from the office deck's meta.pause.
inline std::string pauseWords(const json& t) {
  if (!t.is_object()) return "";
  auto ex = t.find("exited"); if (ex != t.end() && ex->is_boolean() && ex->get<bool>()) return "";
  auto p = t.find("pause"); if (p == t.end() || !p->is_object()) return "";
  auto s = p->find("state"); const std::string st = s != p->end() && s->is_string() ? s->get<std::string>() : "";
  if (st == "resuming") return "resuming";
  auto flag = [&](const char* k) { auto f = p->find(k); return f != p->end() && f->is_boolean() && f->get<bool>(); };
  return "\xe2\x80\x96 " + st + (flag("want") ? ", resume queued" : flag("untilRestart") ? " until restart" : "");
}

// renderer/js/00-state.js `state`, the parts a native view needs
struct State {
  std::string selected;                 // the selected tile id ('' = none)
  std::set<std::string> shown;          // tiles placed in a pane of THIS window
  std::string hover;                    // tile id under the mouse
  std::string hoverPart;                // the Hit.kind under the mouse (buttons light up)
  std::string hoverArg;                 // its Hit.arg (which button of a row)
  std::string pressed;                  // Hit.kind+id held down
  bool sidebarOpen = true;
  float sidebarWidth = 300;             // css: #sidebar width (dragged with the resizer)
  float scroll = 0;                     // the tile list's scroll offset, px
  bool showBars = true;                 // #bars-btn
  bool showMsgs = true;                 // #msgs-btn
  bool grouped = false;                 // #groups-btn
  std::set<std::string> collapsedGroups;
  std::string search;                   // #search-input ('' = no filter)
  bool searchOpen = false;
  bool focusInTerminal = false;         // 85-focus.js: false = the sidebar has the keys
  int windowIndex = 0;                  // this window's index (R5: tiles mark the window that holds them)
  long long clockSkew = 0;              // local now - meta.now at arrival, so ages keep ticking between metas
  bool connected = true;                // remote mode: the socket is up
  std::string notice;                   // a banner over the panes ("The deck is away — reconnecting…")
  std::string noticeDetail;

  // ---- what the Electron tile keeps in the DOM or in state.confirm (30-tiles.js)
  // state.confirm[tileId] = { kind: move | refresh | close | hold, account?, browser? }: the selected tile's
  // action row asks its question in place. browser: -1 = not said, 0 = off, 1 = on (the right-click variants).
  struct Confirm { std::string kind, account; int browser = -1; };
  std::map<std::string, Confirm> confirm;
  std::string renaming;                 // the tile whose title is an <input class="rename"> ('' = none)
  EditBox* renameEdit = nullptr;
  EditBox* searchEdit = nullptr;        // #search-input
  struct HoldForm* hold = nullptr;      // the Special prompt form of the tile in confirm kind `hold`
  EditBox* focusEdit = nullptr;         // the text field that has the caret (nullptr = none)
  bool blinkOn = true;                  // the caret's phase
  std::string dragging;                 // .tile.dragging: the tile being dragged (drawn at 45 %)
  std::string dropMark;                 // .tile.drop-before / .drop-after: the pinned tile the reorder would land at
  bool dropBefore = false;
  float mouseX = -1, mouseY = -1;       // the pointer, for the pin's corner zone (.t-pin-zone:hover + .t-pin)
  bool restartConfirming = false;       // 82-restart.js nowConfirmUntil: "Now" reads "Stop n and restart"
  bool asksFocus = false;               // the asks tile is the selection (49-asks.js)
  std::string asksPick;                 // the row picked in it (a session id, or __recent): .at-row.picked
  float toolsWidth = 0;                 // > 0: the tool row keeps this width (Settings widens the sidebar, the buttons stay put)
  int elsewhereCount = -1, pastCount = -1;   // "Elsewhere [n]" / "Past [n]" (45-elsewhere.js badges); -1 = not asked yet
  // LOCAL mode with the Claude launch switched off (core/deck DeckOptions::allowClaude — the core/host side's
  // switch, read by the shell, never set by it): what that deck then refuses — starting a Claude session, moving
  // sessions between accounts — is drawn the way the Electron UI draws a thing with nothing behind it (the 40 % of
  // `.pull-btn.none`, the faint icon of a tool that is off) with a tooltip that says why, instead of a live button
  // that answers with an error. Everything else the core deck answers (Elsewhere, Past, restart when quiet,
  // special prompts, triggers, sleep / wake) is live in local mode as it is on a remote deck.
  bool claudeOff = false;
  // REMOTE mode (2026-11-18): "it only shows one place" is a rule of ONE box. A remote is another box: no
  // tile is drawn as living in another window (no outlined glyph, no dimmed title), whatever `inWindow` says.
  bool ownLayoutOnly = false;
  // the kind of screen this window is in the driver claim's words (80-sidebar.js DRIVE_ME): false = a remote desk
  // ('remote'), true = the local deck's own window ('office'). Only read when the deck speaks those words.
  bool officeScreen = false;
  std::string shellName;                // state.platform.shellName: what an untitled terminal tile is called ('' = PowerShell)
  // css body.anim-on (meta.settings.animations !== false): the pulses, the hue drift and the turning icon run. Off =
  // the css's static variants. animMs is the ONE clock they all read: the app ticks it only while something that
  // animates is on screen, and a frame never reads the time itself (so a frame can be drawn twice and be the same).
  bool animOn = false;
  double animMs = 0;
  // css TRANSITIONS (not keyframe animations): the pin's 120 ms fade, a usage bar's 400 ms width ease, the group
  // chevron's 120 ms turn (and, in the app, the Settings panel's 160 ms rise). transMs is their clock: the app sets
  // it once per frame while transOn; false = every one of them jumps to its end state (Settings -> Animations off,
  // every test run that does not set the clock itself, the sidebar shot harness).
  bool transOn = false;
  double transMs = 0;
};

// a zone of an overlay (a menu row, a popup's control): what a click there is
struct OZone { RectF rect; std::string kind, id, arg; std::wstring tip; };

// The Special prompt form (30-tiles.js renderHoldForm): a message plus a kind — next turn, at a time,
// recurring, triggered. The fields are text boxes holding what the <input>s hold: `time` "YYYY-MM-DD HH:MM"
// (datetime-local), `from` / `to` "HH:MM" (type=time), `every` a number of minutes.
struct HoldForm {
  std::string tile, kind = "next";
  EditBox text, time, every, from, to, trig;
  std::wstring err;
  struct Pick { std::wstring label; long long ms; };
  std::vector<Pick> picks;
  // The two popups the boxes open, drawn over everything (overlays.hpp drawHoldPop): 1 = the time box's calendar
  // (Chromium's date picker: the icon, or Alt+Down), 2 = the trigger box's list of known triggers (its <datalist>,
  // dropped from the box while it has the focus, narrowed by what is typed).
  int pop = 0;
  RectF timeBox, trigBox;              // where the painter last drew the two boxes: the popups hang from them
  int calY = 0, calM = 0;              // the month the calendar shows (calM 0..11)
  std::vector<std::string> trigList;   // the known triggers, narrowed (the painter fills it)
  int trigIndex = -1;                  // the highlighted row (arrow keys); -1 = none
  bool trigHidden = false;             // Esc put the list away until the next keystroke
  RectF popBox; std::vector<OZone> popZones;
};
void drawHoldPop(Gfx& g, const json& meta, const State& st, HoldForm& f, float viewW, float viewH);

// ---- overlays: small things drawn over the window, each a model the app fills and a painter in sidebar.cpp
// 35-groups.js showCtxMenu / promptInline (css .ctx-menu)
// sub: a submenu (35-groups.js `sub`): Right, Enter or a click opens it in the menu's place; Left goes back
struct MenuItem { std::wstring label; bool sep = false, checked = false, dim = false; std::function<void()> run; std::vector<MenuItem> sub; };
struct CtxMenu {
  bool open = false, prompt = false;    // prompt: the menu is one text box (promptInline)
  float x = 0, y = 0;                   // where it was asked for (the click), before clamping to the window
  std::vector<MenuItem> items;
  int hover = -1;                       // the lit row: under the pointer, or walked to with the arrows
  struct Level { float x = 0, y = 0; std::vector<MenuItem> items; int hover = -1; };
  std::vector<Level> parents;           // the menus a submenu came from (Left goes back to the last)
  EditBox input; std::function<void(const std::wstring&)> onDone;
  std::string owner;                    // 'groups' = the groups button's menu (the button closes its own menu)
  RectF box; std::vector<RectF> rows;   // filled by the painter (rows[i] is empty for a separator)
  int itemAt(float px, float py) const { for (size_t i = 0; i < rows.size(); i++) if (rows[i].w > 0 && rows[i].has(px, py)) return (int)i; return -1; }
};
void drawCtxMenu(Gfx& g, const json& meta, const State& st, CtxMenu& m, float viewW, float viewH);

// 88-palette.js (css .palette): the filtered commands, the selected one, the list's scroll
struct PalItem { std::string group; std::wstring label, hint; std::function<void()> run; };
struct Palette {
  bool open = false; EditBox input; std::vector<PalItem> items; int index = 0; float scroll = 0;
  RectF box; std::vector<RectF> rows;   // filled by the painter (rows[i]: the visible part of item i)
};
void drawPalette(Gfx& g, const json& meta, const State& st, Palette& m, float viewW, float viewH);

// 48-cloud.js openCloudDialog (css .cloud-dlg-back / .cloud-dlg): "New cloud session": the task (its first prompt),
// the folder, Start / Cancel. The app fills the texts and acts on the zones; the painter writes back where things are.
struct CloudDlg {
  bool open = false, starting = false;
  std::string account, label, email;    // the account it is for (state.accounts)
  EditBox task, folder; std::wstring err;
  std::string hover;                    // cdlg-go | cdlg-cancel under the pointer
  RectF box; std::vector<OZone> zones;  // edit cdlg-task / cdlg-folder, cdlg-go, cdlg-cancel
};
void drawCloudDlg(Gfx& g, const json& meta, const State& st, CloudDlg& m, float viewW, float viewH);

// Settings' colour boxes (`<input type=color>`): the picker Chromium opens for one on Windows - a popup under the box
// with a saturation / value square, the current colour, a hue strip and the value as hex. Changes apply as they are
// made (the input event); a click outside or Enter keeps them, Esc puts the colour back. Chromium's eyedropper and
// its RGB / HSL formats are not drawn (PARITY.md "Controls: colour boxes").
struct ColorPick {
  bool open = false; std::string key, before;   // the box it is for (sset.colors), its value when opened
  float h = 0, s = 0, v = 0;                    // hue 0..360, saturation and value 0..1
  int drag = 0;                                 // 1 = the square, 2 = the hue strip
  EditBox hex; RectF anchor, box, sv, hue; std::vector<OZone> zones;
};
void drawColorPick(Gfx& g, const json& meta, const State& st, ColorPick& m, float viewW, float viewH);
// hue 0..360, s / v 0..1 <-> 0xRRGGBB
inline unsigned hsvToRgb(float h, float s, float v) {
  h = std::fmod(std::fmax(0.f, h), 360.f) / 60.f; const int i = (int)h; const float f = h - i;
  const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r = v, g = t, b = p;
  switch (i) { case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break; case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; case 5: r = v; g = p; b = q; break; default: break; }
  auto c = [](float x) { return (unsigned)std::lround(std::fmin(1.f, std::fmax(0.f, x)) * 255); };
  return (c(r) << 16) | (c(g) << 8) | c(b);
}
inline void rgbToHsv(unsigned rgb, float& h, float& s, float& v) {
  const float r = ((rgb >> 16) & 255) / 255.f, g = ((rgb >> 8) & 255) / 255.f, b = (rgb & 255) / 255.f;
  const float mx = std::fmax(r, std::fmax(g, b)), mn = std::fmin(r, std::fmin(g, b)), d = mx - mn;
  v = mx; s = mx > 0 ? d / mx : 0;
  if (d <= 0) return;                           // grey: the hue stays what it was
  if (mx == r) h = 60 * std::fmod((g - b) / d + 6, 6.f); else if (mx == g) h = 60 * ((b - r) / d + 2); else h = 60 * ((r - g) / d + 4);
}

// 45-elsewhere.js / 47-past.js (css #pull-menu): the rows shown, the one asking "bring it here?", the Past filter
struct PullRow { std::wstring label, title, status; bool busy = false, cloud = false; };
struct PullMenu {
  bool open = false; std::string which;             // elsewhere | past
  std::vector<PullRow> rows; int confirm = -1, hover = -1; float scroll = 0, contentH = 0;
  EditBox filter; std::wstring note, empty, question, yesLabel;
  RectF box; std::vector<OZone> zones;
};
void drawPullMenu(Gfx& g, const json& meta, const State& st, PullMenu& m, const RectF& sidebarArea, float bottomTop);

// 60-move.js (css #moveall-menu): open all sessions of one account as another
struct MoveAll {
  bool open = false, advanced = false, cont = false, asking = false;
  std::string from, to; std::map<std::string, bool> picked;   // advanced: tile id -> ticked (absent = ticked)
  std::wstring status, question;                    // #move-all-confirm: a line of text, or the question with its buttons
  float pickScroll = 0;
  RectF box; std::vector<OZone> zones;
};
void drawMoveAll(Gfx& g, const json& meta, const State& st, MoveAll& m, const RectF& sidebarArea);

// 50-settings.js (index.html #settings): what the panel's controls hold, keyed by the page's element ids
struct SettingsAcct { EditBox kw, dir, label; std::string id, email, key; };
struct Settings {
  bool open = false; float scroll = 0, contentH = 0;
  bool newDriver = false;                        // the deck speaks the claim's words (newDriverWords): set-driver lists the four. Set BEFORE settingsFill
  std::map<std::string, bool> checks;            // set-one-window …, b-show:<bar>
  std::map<std::string, std::string> selects;    // set-effort … -> the option's value
  std::map<std::string, EditBox> edits;          // set-font …, b-name:<bar>, set-phone-url, set-phone-pages
  std::map<std::string, std::string> colors;     // <bar>:from / <bar>:to -> #rrggbb
  std::vector<std::unique_ptr<SettingsAcct>> accts;
  std::vector<std::string> fontNames; std::string fontSource = "select";   // 20-terminals.js setupFontControls
  std::vector<std::string> fontHistory;          // fonts used before on this machine (localStorage deck.fonthistory). Set BEFORE settingsFill
  bool bundledFonts = true;                      // the bundled fonts' files were found (Gfx::loadFontFolder): the hint under the control says so when not
  std::wstring status, phoneState;
  std::wstring phoneAssets;                      // phoneStatus.assets === 'missing' (the native deck's own field): the pages the server serves were not found
  RectF box; std::vector<OZone> zones;
  std::vector<OZone> tips;                       // the rows that carry a title="" of their own (index.html): the tooltip there, under any control's own
  std::vector<OZone> boxes;                      // what the last paint drew whole in view: a checkbox row's label + box, every hint (a test's overlap check)
};
void settingsFill(Settings& m, const json& s);                 // openSettings: the controls take the deck's values
void settingsPhone(Settings& m, const json& phoneStatus);      // paintPhone
json settingsPayload(const Settings& m);                       // what Save sends, less `aliases`
const std::vector<std::pair<std::string, std::wstring>>& settingsOptions(const Settings& m, const std::string& key);   // a select's value/label pairs
void drawSettings(Gfx& g, const json& meta, const State& st, Settings& m, const RectF& sidebarArea);

// The ask mark of a session (49-asks.js paintAskMark / askMarkInfo), for the pane's name: draws it at x, top and
// answers its width; 0 = the session has no ask, or Settings hides the marks. draw = false only measures.
float paneAskMark(Gfx& g, const json& meta, const State& st, const std::string& sessionId, float x, float top, bool draw);

// The picture that follows the pointer while a tile is dragged (the browser's drag image: the tile itself, see-through)
void drawTileGhost(Gfx& g, const json& meta, const State& st, const std::string& tileId, float x, float y, float w);
// css .cloud-mark: the small cloud after a cloud session's account, `size` px square, on the pane's name
void drawCloudMark(Gfx& g, float x, float y, float size, const Rgba& c);

// the sidebar's kept measurements and laid-out lines, forgotten (the bench's "before" case; nothing else needs it)
void dropTextKept();
std::string textKeptNote();                    // what the sidebar keeps laid out (windows/test/soak.ps1 `dump mem`)
// 10-format.js / 30-tiles.js helpers the app needs for the rows it prepares
std::wstring fmtAgeMs(double ms);                           // fmtAge
std::wstring fmtHeldTimeMs(double ms, double now);          // fmtHeldTime: 06:30 today, 'tue 06:30' further out
bool parseTimeMs(const json& v, double& ms);                // an ISO time or a number of ms

// What is under a point. kind:
//   ""            nothing
//   "tile"        a session tile (id)                 "tile-close"  its × (id)
//   "tile-pin"    its pin (id)                        "tile-acct"   its account chip (id)
//   "group"       a group header (id = group name)    "asks"        the asks tile
//   "new-tile"    the New terminal tile               "new-acct"    an account button on it (id = account id)
//   "elsewhere" "past"                                the two links on the New terminal tile
//   "tool"        a button of #sb-tools (id = the element id: collapse, search-btn, groups-btn, msgs-btn,
//                 bars-btn, moveall-btn, drive-btn, restart-btn, settings-btn)
//   "restart"     a button of the restart strip (id = arm | later | now | cancel)
//   "resizer"     the sidebar's drag edge             "scrollbar"   the tile list's scrollbar
struct Hit {
  std::string kind, id, arg;
  RectF rect;
  explicit operator bool() const { return !kind.empty(); }
};

// css cubic-bezier(x1, y1, x2, y2): y for x in 0..1 (`ease` = .25 .1 .25 1, `ease-out` = 0 0 .58 1)
inline float cubicBezier(float x1, float y1, float x2, float y2, float x) {
  if (x <= 0) return 0; if (x >= 1) return 1;
  float t = x;
  for (int i = 0; i < 12; i++) {
    const float u = 1 - t, bx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t - x, dx = 3 * u * u * x1 + 6 * u * t * (x2 - x1) + 3 * t * t * (1 - x2);
    if (std::fabs(bx) < 1e-5f || dx == 0) break;
    t = std::max(0.f, std::min(1.f, t - bx / dx));
  }
  const float u = 1 - t;
  return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t;
}

class Sidebar {
 public:
  // Draws the whole sidebar into `area` and records the rects hitTest() answers from.
  void draw(Gfx& g, const RectF& area, const json& meta, const State& st);
  Hit hitTest(float x, float y) const;
  std::wstring tooltipAt(float x, float y) const;   // the element's title="" text, '' when none
  float contentHeight() const { return contentH_; } // the tile list's full height
  float viewHeight() const { return viewH_; }       // the tile list's visible height
  float bottomTop() const { return bottomTop_; }    // the top edge of #sb-bottom (the Elsewhere / Past menu hangs above it)
  // the y range of a tile in list coordinates (for scrollIntoView on F11/F12); false when not listed
  bool tileSpan(const std::string& id, float& top, float& bottom) const;
  // tile ids in the order drawn (what F11/F12 step through: 80-sidebar.js)
  const std::vector<std::string>& order() const { return order_; }
  // a tile's whole box as last drawn (the reorder mark needs its middle); false when it was not drawn
  bool tileRect(const std::string& id, RectF& out) const;
  // the topmost zone of this kind / id / arg ("-" = any), for the scripted tests; nullptr when none
  const Hit* find(const std::string& kind, const std::string& id, const std::string& arg) const;
  std::vector<Hit> all() const { std::vector<Hit> out; for (const auto& z : zones_) out.push_back(z.hit); return out; }
  // The parts that move with State::animMs, as the last draw() placed them (none with animations off or nothing
  // animated in view): a tile that pulses or drifts, the asks tile asking, the restart icon turning. When only the
  // clock moved, the app calls drawAnim: each part is drawn again by itself, where draw() put it and exactly as
  // draw() draws it, under a clip on device pixels; nothing is laid out again, and the zones draw() recorded stay.
  // Valid until anything else the sidebar is drawn from changes (then draw() runs and records them anew).
  size_t animCount() const { return anim_.size(); }
  // a css transition was still under way in the last draw(): the app keeps the frames coming until it settles
  bool transActive() const { return transBusy_; }
  int drawAnim(Gfx& g, const RectF& area, const json& meta, const State& st) const;   // -> how many parts it drew
  // The narrowest the sidebar may be (2026-11-23; a fixed 220 before): the widest tile strip at its natural size
  // (a Claude tile on each account in turn: sleep, one "↪ <label>" per other account, refresh, close, at 11px, nothing
  // shrunk) + the tile's border and padding + the list's padding, never under 160 (the title line). 80-sidebar.js
  // measureSidebarMin is the same rule. 0 when there is no target to measure with yet.
  static float minWidth(Gfx& g, const json& meta, const State& st);

 private:
  struct Zone { Hit hit; std::wstring tip; };
  std::vector<Zone> zones_;
  std::vector<std::string> order_;
  std::map<std::string, std::pair<float, float>> spans_;
  float contentH_ = 0, viewH_ = 0, bottomTop_ = 0;
  struct AnimPart { int kind; std::string id; RectF region, clip; float x, y, w, h; };   // kind: 0 a tile, 1 the asks tile, 2 the tool row
  std::vector<AnimPart> anim_;
  // one css transition per (element, property): where it came from, where it goes, when it turned (State::transMs)
  struct Trans { float from = 0, to = 0; double start = 0; bool seen = false; };
  std::map<std::string, Trans> trans_;
  bool transBusy_ = false;
  friend struct SidebarPainter;
};

} }  // namespace deck::ui
