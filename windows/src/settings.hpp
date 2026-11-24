// windows/src/settings.hpp — the Settings panel (renderer/index.html #settings, js/50-settings.js, css .panel …).
//
// Included by sidebar.cpp only, after overlays.hpp. The model (ui::Settings) holds what the panel's controls
// hold in the Electron page, under the SAME element ids (set-font, set-effort, …), so this file reads against
// index.html line by line: settingsFill() is openSettings(), settingsPayload() the object Save sends, and
// drawSettings() the panel. Controls the page leaves to the browser are drawn the way Chromium draws them on a
// page with no colour scheme: a <select> is a white box with a chevron (its list opens as a menu), a checkbox a
// 13px square, blue when ticked.

namespace {
typedef std::vector<std::pair<std::string, std::wstring>> Opts;
const Opts& optsOf(const Settings& m, const std::string& key) {
  static const std::map<std::string, Opts> table = {
    { "set-effort", { { "tag", L"tag" }, { "color", L"color (left border; whole border when selected)" }, { "none", L"nothing" } } },
    { "set-side", { { "left", L"left" }, { "right", L"right" } } },
    { "set-pinside", { { "edge", L"window edge" }, { "centre", L"terminal side" } } },
    { "set-driver", { { "auto", L"the phone while it is attached" }, { "phone", L"the phone (I am on it)" }, { "desk", L"this desk (the phone views)" } } },
    { "set-ask-picker", { { "alongside", L"shown, with a page beside it" }, { "pane", L"not shown: its questions go on a page" } } },
    { "set-lbl-mode", { { "reset", L"reset time and reset day" }, { "countdown", L"time until reset (hours; days for the week, hours under 24 h; one line when both reset together)" } } },
    { "set-resume", { { "ask", L"ask me each time" }, { "full", L"always resume the full session" }, { "never", L"full session, and tell Claude not to ask again for that session" }, { "summary", L"always resume from summary" } } },
    { "set-phone-bind", { { "tailscale", L"the Tailscale interface only" }, { "all", L"every interface (the house LAN too)" }, { "localhost", L"localhost only (behind Tailscale Serve)" } } },
  };
  // a deck that speaks the claim's words (branch native-remote-server's index.html #set-driver): the four, by name
  static const Opts driverClaim = { { "auto", L"auto: whoever is looking (a phone, else a remote desk, else the office deck)" }, { "office", L"the office deck (the deck's own windows)" },
    { "phone", L"the phone" }, { "remote", L"the remote desk (for the sessions it shows)" } };
  if (key == "set-driver" && m.newDriver) return driverClaim;
  static Opts fonts;
  if (key == "set-fontsel") { fonts.clear(); for (auto& n : m.fontNames) fonts.push_back({ n == "Cascadia Mono (default)" ? std::string() : n, W(n) }); return fonts; }
  static const Opts none;
  auto it = table.find(key); return it == table.end() ? none : it->second;
}
std::wstring optLabel(const Settings& m, const std::string& key) {
  auto v = m.selects.find(key); const std::string val = v == m.selects.end() ? "" : v->second;
  const Opts& o = optsOf(m, key);
  for (auto& p : o) if (p.first == val) return p.second;
  return o.empty() ? L"" : o[0].second;
}
bool boolOr(const json& o, const char* k, bool d) { const json& v = O(o, k); return v.is_boolean() ? v.get<bool>() : d; }
std::wstring numStr(const json& v, const wchar_t* d) { if (v.is_number_integer()) return std::to_wstring(v.get<long long>()); if (v.is_number()) { wchar_t b[32]; swprintf(b, 32, L"%g", v.get<double>()); return b; } if (v.is_string()) return W(v.get<std::string>()); return d; }
}  // namespace

const std::vector<std::pair<std::string, std::wstring>>& settingsOptions(const Settings& m, const std::string& key) { return optsOf(m, key); }

// 50-settings.js paintPhone: the link, and what the server is doing
static void fillPhone(Settings& m, const json& ph) {
  std::string url = S(ph, "url");
  if (url.empty() && !S(ph, "address").empty()) url = "http://" + S(ph, "address") + ":" + std::to_string((int)N(ph, "port")) + "/?t=" + S(ph, "token");
  m.edits["set-phone-url"].set(W(url)); m.edits["set-phone-url"].caret = m.edits["set-phone-url"].anchor = 0;
  const int clients = (int)N(ph, "clients"); const std::string port = std::to_string((int)N(ph, "port")), at = S(ph, "address") + ":" + port;
  const std::string n = std::to_string(clients) + " phone" + (clients == 1 ? "" : "s") + " connected";
  std::string st;
  if (truthy(ph, "error")) st = "Not serving: " + S(ph, "error");
  else if (!truthy(ph, "enabled")) st = truthy(ph, "tailscale") ? "Off. This machine is " + S(ph, "tailscale") + " on the tailnet." : "Off. Tailscale is not running, so there is no tailnet address to serve on.";
  else if (truthy(ph, "running") && S(ph, "bind") == "localhost" && truthy(ph, "serve")) st = "Behind Tailscale Serve: the link above is the HTTPS front door (" + S(ph, "serve") + "); the server itself listens on " + at + ". " + n + ". Open the link on the phone once; after that the app remembers it.";
  else if (truthy(ph, "running") && S(ph, "bind") == "localhost") st = "Serving on " + at + " \xC2\xB7 " + n + ". On no network of its own, and Tailscale Serve is not forwarding to this port, so the link above works only on this PC: run `tailscale serve --bg https / http://localhost:" + port + "` once, which gives the page HTTPS and lets Chrome install it as an app.";
  else if (truthy(ph, "running")) st = "Serving on " + at + " \xC2\xB7 " + n + ". Open the link on the phone once; after that it is installable from Chrome's menu (\"Install app\"), which needs HTTPS \xE2\x80\x94 see mobile.md.";
  else st = "Starting\xE2\x80\xA6";
  m.phoneState = W(st);
  // The native deck's own two fields (HOST-NOTES.md "Say so when the pages are not found"): `assets` 'ok' | 'missing'
  // and `assetRoot`, the folder that was tried ("" = none was found). An Electron deck sends neither: no line.
  m.phoneAssets.clear();
  if (S(ph, "assets") == "missing") {
    const std::string root = S(ph, "assetRoot");
    m.phoneAssets = W("The pages are not beside the exe (renderer\\phone\\index.html): "
      + (root.empty() ? std::string("no folder holding them was found") : "looked in " + root)
      + ". A phone or a remote desk that opens the link gets no page until the deck's renderer, lib and node_modules\\@xterm folders are next to " DECK_EDITION_EXE " (or --assets names them).");
  }
}

// openSettings(): every control takes the deck's value (and its default where the deck has none)
void settingsFill(Settings& m, const json& s) {
  m.accts.clear(); m.checks.clear(); m.selects.clear(); m.edits.clear(); m.colors.clear(); m.status.clear(); m.scroll = 0;
  const json& aliases = O(s, "aliases");
  if (O(s, "accountRows").is_array()) for (auto& r : O(s, "accountRows")) {
    std::unique_ptr<SettingsAcct> a(new SettingsAcct());
    std::string kw; if (O(r, "keywords").is_array()) for (auto& k : O(r, "keywords")) if (k.is_string()) { if (!kw.empty()) kw += ", "; kw += k.get<std::string>(); }
    a->id = S(r, "id"); a->email = S(r, "email"); a->key = a->email.empty() ? a->id : a->email;
    a->kw.set(W(kw)); a->dir.set(W(S(r, "dir"))); a->label.set(W(S(aliases, a->key.c_str())));
    m.accts.push_back(std::move(a));
  }
  auto E = [&](const char* k, const std::wstring& v) { m.edits[k].set(v); };
  E("set-font", numStr(O(s, "fontSize"), L"14")); E("set-zoomfont", numStr(O(s, "zoomFontSize"), L"0"));
  // 20-terminals.js setupFontControls: the bundled fonts; a font that is not one of them sits in the free-text box
  m.fontNames = { "Cascadia Mono (default)", "Reddit Mono", "JetBrains Mono", "Fira Code", "Source Code Pro", "IBM Plex Mono", "Inconsolata", "Ubuntu Mono" };
  // … and the ones used before on this machine (fontHistory), after them
  for (auto& h : m.fontHistory) { bool have = false; for (auto& n : m.fontNames) if (lower(n) == lower(h)) have = true; if (!have && !h.empty()) m.fontNames.push_back(h); }
  std::string cur = S(s, "fontFamily"); { size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t"); cur = a == std::string::npos ? std::string() : cur.substr(a, b - a + 1); }
  bool inList = cur.empty();
  for (auto& n : m.fontNames) if (lower(n) == lower(cur)) inList = true;
  m.selects["set-fontsel"] = inList ? cur : ""; E("set-fontfamily", W(cur)); m.fontSource = inList ? "select" : "box";
  auto Sel = [&](const char* k, const std::string& v, const char* d) { m.selects[k] = v.empty() ? d : v; };
  Sel("set-effort", S(s, "effortStyle"), "tag"); Sel("set-side", S(s, "sidebarSide"), "left"); Sel("set-pinside", S(s, "pinSide"), "edge"); Sel("set-driver", m.newDriver ? driverMode(S(s, "driver")) : S(s, "driver"), "auto");
  E("set-msg-normal", numStr(O(O(s, "msgLines"), "normal"), L"3")); E("set-msg-wide", numStr(O(O(s, "msgLines"), "wide"), L"10"));
  E("set-grid-rows", numStr(O(O(s, "grid"), "rows"), L"2")); E("set-grid-cols", numStr(O(O(s, "grid"), "cols"), L"2"));
  m.checks["set-one-window"] = boolOr(s, "oneWindow", true); m.checks["set-step-esc"] = boolOr(s, "stepEscape", true);
  const json& ak = O(s, "asks");
  m.checks["set-ask-pages"] = boolOr(ak, "usePages", true);
  E("set-ask-threshold", O(ak, "deckThreshold").is_number_integer() ? numStr(O(ak, "deckThreshold"), L"") : L"");
  m.checks["set-ask-background"] = boolOr(ak, "background", true);
  m.selects["set-ask-picker"] = S(ak, "picker") == "pane" ? "pane" : "alongside";
  m.checks["set-ask-tile"] = boolOr(ak, "showTile", true); m.checks["set-ask-marks"] = boolOr(ak, "showMarks", true); m.checks["set-ask-close"] = boolOr(ak, "closeOnDone", true);
  const json& hb = O(s, "hibernate");
  m.checks["set-hib-enabled"] = boolOr(hb, "enabled", true); E("set-hib-idle", numStr(O(hb, "idleMinutes"), L"30")); m.checks["set-hib-pinned"] = boolOr(hb, "keepPinned", true);
  const json& nf = O(s, "notify");
  m.checks["set-notify-waiting"] = boolOr(nf, "waiting", true); m.checks["set-notify-done"] = boolOr(nf, "done", true); m.checks["set-notify-asks"] = boolOr(nf, "asks", true);
  E("set-notify-min", numStr(O(nf, "minTurnSec"), L"20")); m.checks["set-notify-sound"] = boolOr(nf, "sound", true);
  Sel("set-lbl-mode", S(s, "barLabelMode"), "reset");
  m.checks["set-lbl-time"] = boolOr(O(s, "barLabels"), "time", true); m.checks["set-lbl-day"] = boolOr(O(s, "barLabels"), "day", true);
  static const struct { const char* k; const char* name; const char* from; const char* to; } bars[4] = {
    { "session", "Session (5 h)", "#22c55e", "#ef4444" }, { "week", "Week", "#22c55e", "#ef4444" }, { "fable", "Fable (if applicable)", "#22d3ee", "#d946ef" }, { "context", "Context", "#fbbf24", "#f97316" } };
  for (auto& b : bars) {
    const json& c = O(O(s, "bars"), b.k); const std::string k = b.k;
    m.checks["b-show:" + k] = boolOr(c, "show", true);
    m.edits["b-name:" + k].set(W(O(c, "name").is_string() ? S(c, "name") : std::string(b.name)));
    m.colors[k + ":from"] = S(c, "from").empty() ? b.from : S(c, "from"); m.colors[k + ":to"] = S(c, "to").empty() ? b.to : S(c, "to");
  }
  E("set-cwd", W(S(s, "cwd"))); m.checks["set-autotrust"] = boolOr(s, "autoTrust", true); Sel("set-resume", S(s, "resumeChoice"), "ask"); m.checks["set-anim"] = boolOr(s, "animations", true);
  m.checks["set-auditor-autosend"] = boolOr(s, "auditorAutoSend", !deck::edition::kPublic);   // auditors (core/deck/auditors.hpp auditorAutoSendOn): off = Send on the child's tile
  if (deck::edition::kPublic) m.checks["set-usage-live"] = boolOr(s, "usageLive", false);   // the public build's opt-in (core/deck/edition.hpp)
  if (deck::edition::kPublic) m.checks["set-cloud-live"] = boolOr(s, "cloudLive", false);   // ... and the cloud list's (the same sign-in)
  m.checks["set-resume-after-restart"] = boolOr(s, "resumeAfterRestart", true);   // pause.hpp: off = only "Pause until restart" resumes by itself   // auditors (core/deck/auditors.hpp): off = Send on the child's tile
  const json& ph = O(s, "phoneStatus");
  m.checks["set-phone-on"] = truthy(ph, "enabled"); E("set-phone-port", numStr(O(ph, "port"), L"8788")); Sel("set-phone-bind", S(ph, "bind"), "tailscale");
  std::wstring pages; if (O(s, "pages").is_array()) for (auto& p : O(s, "pages")) { if (!pages.empty()) pages += L"\n"; pages += W(S(p, "label") + " = " + S(p, "file")); }
  E("set-phone-pages", pages); m.edits["set-phone-pages"].multiline = true; m.edits["set-phone-pages"].caret = m.edits["set-phone-pages"].anchor = 0;
  m.edits["set-phone-url"].multiline = true; m.edits["set-phone-url"].readOnly = true;
  fillPhone(m, ph);
}
void settingsPhone(Settings& m, const json& phoneStatus) { fillPhone(m, phoneStatus); }

// The object Save sends (50-settings.js), less `aliases`, which the app adds once saveAccounts has answered.
// A remote's phone block is ignored by the deck (main.js: saveSettings from a socket never changes it).
json settingsPayload(const Settings& m) {
  auto C = [&](const char* k) { auto it = m.checks.find(k); return it != m.checks.end() && it->second; };
  auto V = [&](const std::string& k) { auto it = m.edits.find(k); return it == m.edits.end() ? std::string() : narrow(it->second.text); };
  auto L = [&](const char* k) { auto it = m.selects.find(k); return it == m.selects.end() ? std::string() : it->second; };
  json bars = json::object();
  for (const char* k : { "session", "week", "fable", "context" }) {
    const std::string kk = k; auto cf = m.colors.find(kk + ":from"), ct = m.colors.find(kk + ":to");
    bars[k] = { { "show", C(("b-show:" + kk).c_str()) }, { "name", V("b-name:" + kk) }, { "from", cf == m.colors.end() ? "" : cf->second }, { "to", ct == m.colors.end() ? "" : ct->second } };
  }
  std::string th = V("set-ask-threshold"); { size_t a = th.find_first_not_of(" \t"), b = th.find_last_not_of(" \t"); th = a == std::string::npos ? "" : th.substr(a, b - a + 1); }
  json pages = json::array();
  { std::istringstream is(V("set-phone-pages")); std::string l;
    while (std::getline(is, l)) { size_t a = l.find_first_not_of(" \t\r"), b = l.find_last_not_of(" \t\r"); if (a == std::string::npos) continue; l = l.substr(a, b - a + 1);
      size_t i = l.find('='); auto tr = [](std::string x) { size_t a2 = x.find_first_not_of(" \t"), b2 = x.find_last_not_of(" \t"); return a2 == std::string::npos ? std::string() : x.substr(a2, b2 - a2 + 1); };
      if (i != std::string::npos && i > 0) pages.push_back({ { "label", tr(l.substr(0, i)) }, { "file", tr(l.substr(i + 1)) } }); else pages.push_back({ { "label", "" }, { "file", l } }); } }
  json out = {
    { "aliasesExplicit", true }, { "fontSize", V("set-font") }, { "zoomFontSize", V("set-zoomfont") }, { "fontFamily", m.fontSource == "select" ? L("set-fontsel") : V("set-fontfamily") },
    { "cwd", V("set-cwd") }, { "autoTrust", C("set-autotrust") }, { "resumeChoice", L("set-resume") }, { "animations", C("set-anim") }, { "auditorAutoSend", C("set-auditor-autosend") }, { "resumeAfterRestart", C("set-resume-after-restart") },
    { "effortStyle", L("set-effort") }, { "sidebarSide", L("set-side") }, { "pinSide", L("set-pinside") }, { "driver", L("set-driver") }, { "bars", bars },
    { "barLabels", { { "time", C("set-lbl-time") }, { "day", C("set-lbl-day") } } }, { "barLabelMode", L("set-lbl-mode") },
    { "grid", { { "rows", V("set-grid-rows") }, { "cols", V("set-grid-cols") } } }, { "oneWindow", C("set-one-window") }, { "stepEscape", C("set-step-esc") },
    { "asks", { { "deckThreshold", th.empty() ? json() : json(th) }, { "picker", L("set-ask-picker") }, { "usePages", C("set-ask-pages") }, { "background", C("set-ask-background") },
                { "showTile", C("set-ask-tile") }, { "showMarks", C("set-ask-marks") }, { "closeOnDone", C("set-ask-close") } } },
    { "hibernate", { { "enabled", C("set-hib-enabled") }, { "idleMinutes", V("set-hib-idle") }, { "keepPinned", C("set-hib-pinned") } } },
    { "notify", { { "waiting", C("set-notify-waiting") }, { "done", C("set-notify-done") }, { "asks", C("set-notify-asks") }, { "minTurnSec", V("set-notify-min") }, { "sound", C("set-notify-sound") } } },
    { "msgLines", { { "normal", V("set-msg-normal") }, { "wide", V("set-msg-wide") } } },
    { "phone", { { "enabled", C("set-phone-on") }, { "port", V("set-phone-port") }, { "bind", L("set-phone-bind") } } },
    { "pages", pages },
  };
  if (deck::edition::kPublic) { out["usageLive"] = C("set-usage-live"); out["cloudLive"] = C("set-cloud-live"); }
  return out;
}

// css .panel: over the sidebar, from just above the tool row up; #141414 with a 1px #2a2a2a line on top. The
// head (Settings · status · Cancel · Save) stays; the body scrolls with no bar.
void drawSettings(Gfx& g, const json& meta, const State& st, Settings& m, const RectF& area) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, area);
  m.zones.clear(); m.tips.clear(); m.boxes.clear();
  const float X = area.x, Wd = area.w, bx = X + 14, bw = Wd - 28;
  const float bottom = area.b() - 34, maxH = area.h - 34, headH = 1 + 8 + 24 + 6;
  const Font ff = p.F(12.5f), hf = p.F(11.5f);
  const Rgba hintC = Rgba::hex(0x8c8c8c);
  const float lhF = p.LN(12.5f), inH = lhF + 6 + 2;
  const bool askOn = m.checks["set-ask-pages"];
  const RectF none;
  RectF view;                                            // the body's visible box (set before the painting pass)
  float y = 0; bool draw = false; bool first = true;
  auto vis = [&](const RectF& r) { return draw && r.b() > view.y && r.y < view.b(); };
  // a drawn block, kept when it is whole in view (Settings::boxes)
  auto keepBox = [&](const char* kind, const std::string& id, const RectF& r) { if (draw && r.y >= view.y && r.b() <= view.b()) oz(m.boxes, kind, id, r); };
  auto zoneIn = [&](const char* kind, const std::string& id, const RectF& r, const std::wstring& tip = L"") {
    const float y0 = std::max(r.y, view.y), y1 = std::min(r.b(), view.b());
    if (y1 > y0) oz(m.zones, kind, id, RectF(r.x, y0, r.w, y1 - y0), tip);
  };
  auto gap = [&] { if (!first) y += 6; first = false; };
  // index.html: a row's own title="" (the Asks settings explain themselves on hover). The row from y0 down to here.
  auto title = [&](const wchar_t* text, float y0) {
    if (!draw) return;
    const float a = std::max(y0, view.y), b = std::min(y, view.b());
    if (b > a) oz(m.tips, "tip", "", RectF(bx, a, bw, b - a), text);
  };
  // ---- the browser's own controls
  auto checkbox = [&](const std::string& key, float cx, float cy, bool disabled = false) {
    const RectF r = p.snap(RectF(cx, cy, 13, 13));
    if (!vis(r)) return;
    const bool on = m.checks[key];
    if (on) { g.fillRound(r, 2, Rgba::hex(0x0075ff)); SidebarPainter::Xf xf(p, r.x, r.y, 13.f / 24); p.strokeD("M5 12.5l4.5 4.5L19 7.5", 3, Rgba::hex(0xffffff)); }
    else { g.fillRound(r, 2, Rgba::hex(0xffffff)); g.strokeRound(r, 2, Rgba::hex(0x767676), 1); }
    if (!disabled) zoneIn("set-check", key, RectF(r.x - 3, r.y - 3, 19, 19));
    keepBox("check", key, r);
  };
  // a default <select>: white, 1px #767676, 13.33px text, a chevron; `dark` = the styled font select
  auto selW = [&](const std::string& key) { float w = 0; for (auto& o : optsOf(m, key)) w = std::max(w, p.tw(o.second, p.F(13.333f))); return std::ceil(w) + 4 + 14 + 2; };
  auto select = [&](const std::string& key, const RectF& r, bool dark = false, bool disabled = false) {
    if (!vis(r)) return;
    const std::wstring label = optLabel(m, key);
    if (dark) { p.fill(r, Rgba::hex(0x0f0f0f)); p.border(r, Rgba::hex(0x333333)); p.tIn(label, RectF(r.x + 9, r.y, r.w - 30, r.h), ff, kFg, Align::Left, true); }
    else { p.fill(r, Rgba::hex(0xffffff)); p.border(r, Rgba::hex(0x767676)); g.pushClip(RectF(r.x + 1, r.y + 1, r.w - 18, r.h - 2)); p.tIn(label, RectF(r.x + 5, r.y, 2000, r.h), p.F(13.333f), Rgba::hex(0x000000), Align::Left, false); g.popClip(); }
    { SidebarPainter::Xf xf(p, p.rnd(r.r() - 17), p.rnd(r.y + (r.h - 12) / 2), 12.f / 24); p.strokeD("M6 9l6 6 6-6", 2.6f, dark ? kFg : Rgba::hex(0x000000)); }
    if (!disabled) zoneIn("set-select", key, r, key == "set-fontsel" ? L"Bundled fonts and ones you have used before" : L"");
  };
  auto input = [&](const std::string& key, EditBox& e, const RectF& r, const wchar_t* ph = L"", bool dim = false) {
    if (!vis(r)) return;
    const bool foc = st.focusEdit == &e;
    if (dim) p.pushOpacity(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 0.45f);
    if (foc) p.border(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), Rgba::hex(0x555555));
    p.fill(r, Rgba::hex(0x0f0f0f)); p.border(r, Rgba::hex(0x333333));
    e.placeholder = ph;
    e.draw(g, RectF(r.x + 7, r.y + 1, r.w - 14, r.h - 2), ff, kFg, Rgba::hex(0x757575), foc, st.blinkOn);
    if (dim) p.popOpacity();
    // index.html: the two "Preview lines" boxes carry a title each (title="normal", title="sidebar wider than 400px")
    zoneIn("edit", "set:" + key, r, key == "set-msg-normal" ? L"normal" : key == "set-msg-wide" ? L"sidebar wider than 400px" : L"");
  };
  auto smallBtn = [&](const char* id, const std::wstring& label, float rx, float cy, bool danger, bool alignRight, float bh = 19) {
    const float w = p.tw(label, p.F(11)) + (danger ? 10 : 14) + 2; const RectF r(alignRight ? rx - w : rx, cy, w, bh);
    if (vis(r)) { obtn(p, r, label, p.F(11), st.hoverPart == "set-btn" && st.hover == id, danger); zoneIn("set-btn", id, r); }
    return w;
  };
  // ---- the page's blocks
  auto h3 = [&](const wchar_t* text) {
    gap(); y += 10;
    std::wstring t = text; for (auto& c : t) c = (wchar_t)towupper(c);
    if (vis(RectF(bx, y, bw, 15))) { p.tracking = 0.6f; p.tb(t, bx, y + p.baseIn(p.LN(11), 11), bw, p.F(11, 600), Rgba::hex(0xb4b4b4), Align::Left, false); p.tracking = 0; }
    y += p.LN(11) + 2;
  };
  auto subhead = [&](const wchar_t* text) {
    gap(); y += 10;
    if (vis(RectF(bx, y, bw, 15))) p.tb(text, bx, y + p.baseIn(p.LN(11), 11), bw, p.F(11, 600), kDim, Align::Left, false);
    y += p.LN(11) + 2;
  };
  auto hint = [&](const std::wstring& text, const Rgba& c = Rgba::hex(0x8c8c8c)) {
    gap();
    const float h = p.wrap(text, bx, y, bw, hf, c, p.LN(11.5f), 0, false, false);
    if (vis(RectF(bx, y, bw, h))) p.wrap(text, bx, y, bw, hf, c, p.LN(11.5f), 0, false, true);
    keepBox("hint", "", RectF(bx, y, bw, h));
    y += h;
  };
  // .field: the label on the left, the control(s) on the right; `rowH` is the control's height
  auto fieldLabel = [&](const std::wstring& label, float rowH, float maxW) {
    const float lw = p.tw(label, ff);
    if (lw <= maxW) { if (vis(RectF(bx, y, bw, rowH))) p.tb(label, bx, y + (rowH - lhF) / 2 + p.baseIn(lhF, 12.5f), lw + 2, ff, kFg, Align::Left, false); return rowH; }
    const float h = p.wrap(label, bx, y, std::max(30.f, maxW), ff, kFg, lhF, 0, false, vis(RectF(bx, y, bw, 60)));
    return std::max(rowH, h);
  };
  // The row is as tall as its label: a label that wraps (the auditor's, at any width) takes two lines or more, and the
  // hint under it starts below them (it was laid at a fixed 19px and the hint ran over the second line, 2026-11-24).
  auto checkField = [&](const wchar_t* label, const char* key, bool dep = false) {
    gap();
    const bool off = dep && !askOn;
    const float maxW = bw - 30, lw = p.tw(label, ff);
    const float h = lw <= maxW ? 19.f : std::max(19.f, p.wrap(label, bx, y, std::max(30.f, maxW), ff, kFg, lhF, 0, false, false));
    if (off && draw) p.pushOpacity(RectF(bx, y, bw, h), 0.45f);
    fieldLabel(label, 19, maxW);
    checkbox(key, bx + bw - 3 - 13, y + 3, off);
    if (!off && draw) zoneIn("set-check", key, RectF(bx, y, bw - 20, h));     // the label is the control's too
    if (off && draw) p.popOpacity();
    keepBox("label", key, RectF(bx, lw <= maxW ? y + (19 - lhF) / 2 : y, std::min(lw, maxW), lw <= maxW ? lhF : h));
    y += h;
  };
  auto selectField = [&](const wchar_t* label, const char* key, bool dep = false) {
    gap();
    const bool off = dep && !askOn;
    const float sw = selW(key), lw = p.tw(label, ff);
    float lblMax = bw - 8 - sw; float sx = bx + bw - sw;
    if (lw > lblMax) { lblMax = 0; for (size_t i = 0, j; i < wcslen(label); i = j + 1) { j = std::wstring(label).find(L' ', i); if (j == std::wstring::npos) j = wcslen(label); lblMax = std::max(lblMax, p.tw(std::wstring(label).substr(i, j - i), ff)); } lblMax = std::ceil(lblMax) + 1; sx = bx + lblMax + 8; }
    if (off && draw) p.pushOpacity(RectF(bx, y, bw, 34), 0.45f);
    const float h = fieldLabel(label, 19, lblMax);
    select(key, RectF(sx, y + (h - 19) / 2, std::min(sw, X + Wd - sx), 19), false, off);   // a list wider than the panel runs off its edge, as the page's does
    if (off && draw) p.popOpacity();
    y += h;
  };
  // label ... [number] hint [number] hint
  struct Num { const char* key; const wchar_t* after; const wchar_t* ph; };
  auto numField = [&](const wchar_t* label, std::vector<Num> nums, const wchar_t* between = nullptr, bool dep = false) {
    gap();
    const bool off = dep && !askOn;
    if (off && draw) p.pushOpacity(RectF(bx, y, bw, inH), 0.45f);
    fieldLabel(label, inH, bw);
    float rx = bx + bw;
    for (int i = (int)nums.size() - 1; i >= 0; i--) {
      if (nums[i].after && *nums[i].after) { const float w = p.tw(nums[i].after, hf); if (draw) p.tb(nums[i].after, rx - w, y + (inH - p.LN(11.5f)) / 2 + p.baseIn(p.LN(11.5f), 11.5f), w + 2, hf, hintC, Align::Left, false); rx -= w + 4; }
      input(nums[i].key, m.edits[nums[i].key], RectF(rx - 64, y, 64, inH), nums[i].ph ? nums[i].ph : L""); rx -= 64 + 4;
      if (i > 0 && between) { const float w = p.tw(between, hf); if (draw) p.tb(between, rx - w, y + (inH - p.LN(11.5f)) / 2 + p.baseIn(p.LN(11.5f), 11.5f), w + 2, hf, hintC, Align::Left, false); rx -= w + 4; }
    }
    if (off && draw) p.popOpacity();
    y += inH;
  };
  auto textField = [&](const wchar_t* label, const char* key) {
    gap();
    const float lw = p.tw(label, ff);
    fieldLabel(label, inH, bw);
    input(key, m.edits[key], RectF(bx + lw + 8, y, bw - lw - 8, inH));
    y += inH;
  };
  // .labels-field: the label, then ticks spread across the row
  auto checksField = [&](const wchar_t* label, std::vector<std::pair<const char*, const wchar_t*>> cs) {
    gap();
    const float lw = p.tw(label, ff); std::vector<float> ws; float sum = lw;
    for (auto& c : cs) { ws.push_back(8 + 20 + 4 + p.tw(c.second, p.F(12))); sum += ws.back(); }
    const float space = std::max(8.f, 8 + (bw - sum - 8 * cs.size()) / cs.size());
    const float lh = fieldLabel(label, 19, bw);
    float cx = bx + lw + space;
    for (size_t i = 0; i < cs.size(); i++) {
      checkbox(cs[i].first, cx + 8 + 4, y + 3);
      if (draw) p.tb(cs[i].second, cx + 8 + 20 + 4, y + 1 + p.baseIn(p.LN(12), 12), ws[i], p.F(12), kFg, Align::Left, false);
      if (draw) zoneIn("set-check", cs[i].first, RectF(cx + 8, y, ws[i] - 8, 19));
      cx += ws[i] + space;
    }
    y += std::max(19.f, lh);
  };
  auto textarea = [&](const char* key, int rows, const wchar_t* ph) {
    gap();
    EditBox& e = m.edits[key];
    const Font mf = [&] { Font f; f.family = L"Consolas"; f.size = 12; return f; }();
    float th = rows * 12 * 1.4f;
    if (rows == 0) {                                      // the link: the box grows to its text (sizePhoneUrl)
      Com<IDWriteTextLayout> lay = g.layout(e.text.empty() ? std::wstring(L" ") : e.text, mf, bw - 14, 10000, true, false, 0);
      DWRITE_TEXT_METRICS tm{}; if (lay) { lay->SetWordWrapping((DWRITE_WORD_WRAPPING)2); lay->GetMetrics(&tm); }
      th = std::max(12 * 1.4f, std::ceil(tm.height));
    }
    const RectF r(bx, y, bw, th + 8 + 2);
    if (vis(r)) {
      const bool foc = st.focusEdit == &e, hot = st.hoverPart == "edit" && st.hover == std::string("set:") + key;
      p.fill(r, Rgba::hex(0x0f0f0f)); p.border(r, rows == 0 && hot ? Rgba::hex(0x777777) : Rgba::hex(0x333333));
      if (foc) { p.border(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), Rgba::hex(0xffffff)); }
      e.placeholder = ph;
      e.draw(g, RectF(r.x + 7, r.y + 5, r.w - 14, r.h - 10), mf, kFg, Rgba::hex(0x757575), foc, st.blinkOn);
      zoneIn("edit", std::string("set:") + key, r, rows == 0 ? L"Click to copy the link" : L"");
    }
    y += r.h;
  };

  auto body = [&] {
    first = true;
    h3(L"Accounts");
    hint(L"One row per Claude account: the keyword(s) you type to run it, its config folder, and the short label shown in the sidebar. Saving writes the alias file that userswitch reads too. Sign in from the row's button once; the email appears after.");
    { // #alias-rows: one grid for every row, so the columns line up
      gap();
      float whoW = 60;
      for (auto& a : m.accts) whoW = std::max(whoW, a->email.empty() ? (a->id.empty() ? p.tw(L"save, then log in", p.F(11)) : p.tw(L"log in", p.F(11)) + 16) : p.tw(W(a->email), p.F(11)));
      whoW = std::ceil(whoW);
      const float fr = std::max(0.f, bw - 44 - whoW - 20 - 24), c1 = std::max(90.f, fr * 1.1f / 2.6f), c2 = std::max(120.f, fr - c1);
      for (size_t i = 0; i < m.accts.size(); i++) {
        SettingsAcct& a = *m.accts[i]; const std::string n = std::to_string(i);
        float cx = bx;
        input("acct:" + n + ":kw", a.kw, RectF(cx, y, c1, inH), L"keyword(s)"); cx += c1 + 6;
        input("acct:" + n + ":dir", a.dir, RectF(cx, y, c2, inH), L"~\\.claude-name"); cx += c2 + 6;
        input("acct:" + n + ":label", a.label, RectF(cx, y, 44, inH), L"label"); cx += 44 + 6;
        if (!a.email.empty()) { if (vis(RectF(cx, y, whoW, inH))) p.tb(W(a.email), cx, y + (inH - p.LN(11)) / 2 + p.baseIn(p.LN(11), 11), whoW, p.F(11), kDim, Align::Right, true); }
        else if (!a.id.empty()) smallBtn(("acct-login:" + n).c_str(), L"log in", cx + whoW, y + (inH - 19) / 2, false, true);
        else if (vis(RectF(cx, y, whoW, inH))) p.tb(L"save, then log in", cx, y + (inH - p.LN(11)) / 2 + p.baseIn(p.LN(11), 11), whoW, p.F(11), kDim, Align::Right, true);
        cx += whoW + 6;
        smallBtn(("acct-rm:" + n).c_str(), L"\x00D7", cx, y, true, false);
        y += inH + (i + 1 < m.accts.size() ? 5 : 0);
      }
    }
    // a direct child of the scrolling column: with more settings than room it gives up its 19px for its text's 17
    gap(); y += 2; smallBtn("acct-add", L"+ add account", bx, y, false, false, 17); y += 17;
    h3(L"Tiles");
    selectField(L"Effort level shown as", "set-effort");
    selectField(L"Sidebar on the", "set-side");
    selectField(L"Pin sits on the", "set-pinside");
    selectField(L"Size is driven by", "set-driver");
    numField(L"Preview lines", { { "set-msg-normal", L"normal", nullptr }, { "set-msg-wide", L"wide", nullptr } });
    h3(L"Layout");
    hint(L"The terminal area is a grid of up to 4\x00D7" L"4. Place a session with the number keys (hold the cells you want, in reading order \x2014 1\x00A0" L"2\x00A0" L"3 top row, 4\x00A0" L"5\x00A0" L"6 next \x2014 and release) or by dragging a tile onto it. One shape for the whole deck; changing it clears the current arrangement.");
    numField(L"Grid", { { "set-grid-rows", L"rows", nullptr }, { "set-grid-cols", L"cols", nullptr } }, L"\x00D7");
    checkField(L"A session shows in one window at a time", "set-one-window");
    hint(L"On: moving a session to another window removes it from the one it was in (growing into a neighbouring region never removes). Off: it can be on screen in several windows; the focused window drives its size.");
    checkField(L"Esc during an F11 / F12 walk returns to where you started", "set-step-esc");
    hint(L"Stepping with F11 / F12 (or Up / Down) is a preview until you click or type; the first Esc undoes the walk and reaches no session.");
    h3(L"Asks");
    hint(L"Claude sessions can put their questions for you on an ask page (the ask-page skill) instead of in their terminal: one card per question, answered with a click or a note. The asks tile at the top of the sidebar lists every session's, sessions outside the deck included: click it to answer them (F6 or F7 closes); F7 on a session shows just its own. Done types your answers into the session.");
    subhead(L"How sessions in the deck ask you");
    { const float t0 = y; checkField(L"Use ask pages", "set-ask-pages"); title(L"On: sessions use ask pages, by the rules below.\r\nOff: sessions in the deck never make a page; every question is asked in their terminal, Claude Code's own picker included. Nothing new shows in the asks tile (the recently answered stay).", t0 + (first ? 0 : 0)); }
    { const float t0 = y; numField(L"A page when a session has more than", { { "set-ask-threshold", L"questions", L"3" } }, nullptr, true); title(L"A session puts its questions on a page when it has MORE than this many at once; at or under it, it asks in its terminal.\r\nBlank = the skill's own number (3). 0 = always the page, never the terminal. A big number (99) = practically never a page by itself.", t0 + (first ? 0 : 0)); }
    { const float t0 = y; checkField(L"Also list terminal questions in the asks tile", "set-ask-background", true); title(L"On: a question asked in the terminal (at or under the number above, or in Claude Code's own picker) also gets a page in the asks tile. Answer it in either place; answered from the tile, Done types your answers into the session and it carries on.\r\nOff: terminal questions stay in the terminal only.", t0 + (first ? 0 : 0)); }
    { const float t0 = y; selectField(L"Claude Code's own question picker", "set-ask-picker", true); title(L"Claude Code's own question picker (the numbered options in the terminal).\r\nShown: it appears as usual, with the same questions on a page beside it (F7); answer either one.\r\nNot shown: the session puts its questions on a page instead, and waits for your Done.", t0 + (first ? 0 : 0)); }
    { const float t0 = y; hint(L"\x24D8 Questions in the terminal, answers from the asks tile too: hover here for the settings that do it.", kAsk); title(L"Every question in the terminal, every one answerable from the asks tile:\r\n1. Use ask pages: on\r\n2. A page when more than: 99 (no page on its own)\r\n3. Also list terminal questions in the asks tile: on\r\n4. Claude Code's own question picker: shown\r\nEach question then appears in its session's terminal AND in the asks tile. Answer in the terminal as usual, or in the tile: Done saves your answers to a file and tells the session where it is in one line; it reads them and carries on.", t0 + (first ? 0 : 0)); }
    subhead(L"What the deck shows");
    { const float t0 = y; checkField(L"The asks tile at the top of the sidebar", "set-ask-tile"); title(L"The asks tile at the top of the sidebar: every session's waiting questions and the recently answered ones. Off hides it; F7 on a session and the palette still open the asks.", t0 + (first ? 0 : 0)); }
    { const float t0 = y; checkField(L"Ask marks on session tiles and panes", "set-ask-marks"); title(L"A small ? mark with the number of questions on a session's tile and on its pane name: hollow = waiting, filled = answers in progress, a tick = sent and not yet read. Off hides the marks (the asks tile keeps its own).", t0 + (first ? 0 : 0)); }
    subhead(L"After Done");
    { const float t0 = y; checkField(L"Close the ask (F7 shows your answers again, read-only)", "set-ask-close"); title(L"On: Done closes a session's ask. F7 on that session shows what you sent, read-only, until it asks again. In the every-session view (F6 on the asks tile) it closes only when no other session is waiting.\r\nOff: it stays open, showing your answers read-only.", t0 + (first ? 0 : 0)); }
    hint(L"Sessions in the deck read the first four at their next question (the deck writes them to deck.json in the skill's state folder). Sessions outside the deck keep the skill's own settings.");
    h3(L"Sleep");
    hint(L"A session nobody is looking at has its process killed and comes back with --resume the moment it is wanted \x2014 on screen, typed into, or holding a prompt that is due. The conversation is untouched and resuming costs no tokens; a Claude process is about 244\x00A0MB, so this is where the deck\x2019s memory goes. A session that is busy, waiting on you, or holding a prompt never sleeps.");
    checkField(L"Sleep idle sessions", "set-hib-enabled");
    numField(L"After", { { "set-hib-idle", L"minutes idle", nullptr } });
    checkField(L"Keep pinned sessions awake", "set-hib-pinned");
    h3(L"Notifications");
    hint(L"Windows toasts while the deck is not the focused window. Clicking a toast brings the session up.");
    checksField(L"Toast when", { { "set-notify-waiting", L"a session waits for you" }, { "set-notify-done", L"a turn finishes" }, { "set-notify-asks", L"a session asks you on a page" } });
    numField(L"Only turns longer than", { { "set-notify-min", L"seconds", nullptr } });
    checkField(L"Sound", "set-notify-sound");
    h3(L"Graphs");
    checksField(L"Labels", { { "set-lbl-time", L"session" }, { "set-lbl-day", L"week" } });
    selectField(L"Labels show", "set-lbl-mode");
    { // #bar-rows: show | name | from -> to
      gap();
      int i = 0;
      for (const char* k : { "session", "week", "fable", "context" }) {
        const std::string kk = k;
        checkbox("b-show:" + kk, bx + 4, y + (inH - 13) / 2);
        input("b-name:" + kk, m.edits["b-name:" + kk], RectF(bx + 24, y, bw - 24 - 80, inH));
        for (int c = 0; c < 2; c++) {
          const std::string ck = kk + (c ? ":to" : ":from");
          const RectF r = p.snap(RectF(bx + bw - (c ? 34 : 74), y + (inH - 20) / 2, 34, 20));
          if (vis(r)) {
            p.fill(r, Rgba::hex(0x0f0f0f)); p.border(r, Rgba::hex(0x333333));
            unsigned rgb = 0; const std::string hx = m.colors[ck]; if (hx.size() == 7) rgb = (unsigned)strtoul(hx.c_str() + 1, nullptr, 16);
            p.fill(RectF(r.x + 3, r.y + 4, r.w - 6, r.h - 8), Rgba::hex(rgb)); p.border(RectF(r.x + 3, r.y + 4, r.w - 6, r.h - 8), Rgba::hex(0x777777));
            zoneIn("set-color", ck, r, c ? L"color at 100 %" : L"color at 0 %");
          }
        }
        y += inH + (++i < 4 ? 4 : 0);
      }
    }
    h3(L"Terminal");
    numField(L"Font size", { { "set-font", L"", nullptr } });
    { const float t0 = y; numField(L"Zoomed pane font size", { { "set-zoomfont", L"", nullptr } }); title(L"Font size while a pane is zoomed to fill the terminal area (double-click its name / F6). 0 = same as Font size.", t0 + (first ? 0 : 0)); }
    { // Font: the dropdown and the free-text box; the one touched last counts, the other is greyed
      gap();
      const float lw = p.tw(L"Font", ff), avail = bw - lw - 16, sw = std::max(60.f, (avail - 140) / 2);
      fieldLabel(L"Font", inH, bw);
      const RectF sr(bx + lw + 8, y, sw, inH);
      if (m.fontSource != "select" && draw) p.pushOpacity(RectF(sr.x - 1, sr.y - 1, sr.w + 2, sr.h + 2), 0.45f);
      select("set-fontsel", sr, true);
      if (m.fontSource != "select" && draw) p.popOpacity();
      input("set-fontfamily", m.edits["set-fontfamily"], RectF(sr.r() + 8, y, avail - sw, inH), L"or type a font name", m.fontSource != "box");
      y += inH;
    }
    // (this exe is one file: the bundled fonts are the deck's files, read from an assets\fonts folder beside it. When
    // that folder is not there the hint says what to do, instead of promising fonts that will not show)
    if (m.bundledFonts) hint(L"Bundled with the deck (no install needed): Reddit Mono, JetBrains Mono, Fira Code, Source Code Pro, IBM Plex Mono, Inconsolata, Ubuntu Mono. Any font installed on this machine works too. Blank = Cascadia Mono. Licenses: assets/fonts/LICENSES.md.");
    else hint(L"The deck's bundled fonts (Reddit Mono, JetBrains Mono, Fira Code, Source Code Pro, IBM Plex Mono, Inconsolata, Ubuntu Mono) are files: copy the deck's assets\\fonts folder next to " DECK_EDITION_EXE L" (as assets\\fonts) and they need no install. It is not there now, so only fonts installed on this machine work. Blank = Cascadia Mono.");
    textField(L"New session folder", "set-cwd");
    checkField(L"Answer the home-folder trust prompt automatically", "set-autotrust");
    selectField(L"Large-session resume prompt", "set-resume");
    hint(L"Claude Code asks this whenever a big session is resumed, which the deck does on every restart, refresh and account move; the prompt itself costs nothing. Full keeps every detail (the first message afterwards re-reads the history once); summary shrinks the context so later turns cost less. Only \"don't ask again\" is remembered by Claude Code, per session.");
    checkField(L"Animations (rainbow drift for max, purple pulse for ultracode)", "set-anim");
    hint(L"Claude Code never remembers trust for a session rooted at your home folder, so it asks on every launch. With this on, the deck picks \"Yes, I trust this folder\" for you.");
    checkField(L"Type each file an auditor writes into its child without asking (off: each waits on the child's tile for Send)", "set-auditor-autosend");
    if (!deck::edition::kPublic) hint(L"An auditor session (tile menu: Prompts, Make auditor) writes to-<n>.md in its inbox for its child n; the deck types that file into the child as your own message once the child is idle, and moves it to sent. A to-<n>.now.md file is typed at once, interrupting the child's turn (Esc). The inbox's log.md has one line per delivery or refusal.");
    else hint(L"An auditor session (tile menu: Prompts, Make auditor) writes to-<n>.md in its inbox for its child n. You delegate that writing to it: each file waits on the child's tile until you press Send (or, with this box ticked, is typed in for you once the child is idle), then moves to sent. A to-<n>.now.md file interrupts the child's turn (Esc). The inbox's log.md has one line per delivery or refusal.");
    checkField(L"Send RESUME after a restart to paused sessions", "set-resume-after-restart");
    hint(L"At the deck's next start, each tile still marked paused (the deck's Pause, or a PAUSE pasted into the session) gets the RESUME prompt by itself once its session is up and idle. Off: only \"Pause until restart\" tiles do; the others keep their mark until their next prompt.");
    if (deck::edition::kPublic) {
      h3(L"Usage");
      checkField(L"Live usage bars", "set-usage-live");
      hint(L"Reads your Claude Code sign-in to fetch your usage from Anthropic; not an official API. Off (the default): the deck reads none of it and the usage bars stay empty.");
      checkField(L"Cloud session list", "set-cloud-live");
      hint(L"Reads your Claude Code sign-in to list your cloud sessions; not an official API. Off (the default): the deck reads none of it, lists no cloud session under Elsewhere and shows no cloud tile's status.");
    }
    h3(L"Phone");
    hint(L"Serves this deck as a phone-sized page: the same tiles, usage bars and account moves, plus the terminals themselves. It listens on the Tailscale interface only \x2014 not the house LAN, never the internet \x2014 and the token in the link guards it anyway. The sessions stay on this PC; the phone is a second view of them.");
    checkField(L"Serve the phone view", "set-phone-on");
    numField(L"Port", { { "set-phone-port", L"", nullptr } });
    selectField(L"Listen on", "set-phone-bind");
    { gap(); fieldLabel(L"Link", 19, bw); float rx = bx + bw; rx -= smallBtn("phone-newtoken", L"new token", rx, y, true, true) + 4; smallBtn("phone-copy", L"copy", rx, y, false, true); y += 19; }
    if (!m.edits["set-phone-url"].text.empty()) textarea("set-phone-url", 0, L"");
    hint(m.phoneState);
    if (!m.phoneAssets.empty()) hint(m.phoneAssets, Rgba::hex(0xf87171));
    { gap(); fieldLabel(L"Pages on the phone", lhF, bw); y += lhF; }
    textarea("set-phone-pages", 3, L"Report = C:\\Users\\you\\report.html");
    hint(L"One per line, Label = full path of an .html file. Each becomes a button in the phone's top bar that opens the file as it is on this PC now (and any .html file beside it that the page links to). For self-contained pages, like a report a scheduled job rewrites.");
    y += 14;
  };
  // measure, then paint from where the panel's own height puts it
  y = 0; draw = false; body();
  const float content = y, h = std::min(maxH, headH + content), top = bottom - h;
  const RectF box = p.snap(RectF(X, top, Wd, h));
  view = RectF(X, top + headH, Wd, h - headH);
  m.contentH = content; m.scroll = std::max(0.f, std::min(std::max(0.f, content - view.h), m.scroll));
  shadow(g, RectF(box.x, box.y, box.w, 8), -8, 30, 0.667f);
  p.fill(box, Rgba::hex(0x141414)); p.fill(RectF(box.x, box.y, box.w, 1), Rgba::hex(0x2a2a2a));
  // .panel-head
  {
    const float hy = top + 1 + 8;
    p.tb(L"Settings", X + 14, hy + (24 - p.LN(14)) / 2 + p.baseIn(p.LN(14), 14), 120, p.F(14), kFg, Align::Left, false);
    const float sw = p.tw(L"Save", p.F(12.5f, 600)) + 24 + 2, cw = p.tw(L"Cancel", ff) + 24 + 2;
    const RectF save(X + Wd - 12 - sw, hy, sw, 24), cancel(save.x - 8 - cw, hy, cw, 24);
    const bool sh = st.hoverPart == "set-btn" && st.hover == "save", ch = st.hoverPart == "set-btn" && st.hover == "cancel";
    p.fill(save, sh ? Rgba::hex(0xffffff) : Rgba::hex(0xf0f0f0)); p.tIn(L"Save", save, p.F(12.5f, 600), Rgba::hex(0x111111));
    p.fill(cancel, ch ? Rgba::hex(0x303030) : kBtn); p.border(cancel, kBtnBorder); p.tIn(L"Cancel", cancel, ff, kFg);
    oz(m.zones, "set-btn", "save", save); oz(m.zones, "set-btn", "cancel", cancel);
    if (!m.status.empty()) { const float stw = std::min(260.f, cancel.x - 8 - (X + 14 + 70)); p.tb(m.status, cancel.x - 8 - stw, hy + (24 - p.LN(11.5f)) / 2 + p.baseIn(p.LN(11.5f), 11.5f), stw, hf, hintC, Align::Right, true); }
  }
  g.pushClip(view);
  y = view.y - m.scroll; draw = true; body();
  g.popClip();
  m.box = box;
}
