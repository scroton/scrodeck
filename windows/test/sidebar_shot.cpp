// windows/test/sidebar_shot.cpp — renders deck::ui::Sidebar off-screen from a meta JSON and saves a PNG.
//
//   sidebar_shot.exe <meta.json> <out.png> [width=340] [height=900] [scroll=0] [flags…]
//
// flags:  --sel=<tile id>      the selected tile            --hover=<id> --part=<Hit.kind>   what the mouse is over
//         --grouped            grouped view                 --collapse=<group>  (repeatable)
//         --nobars --nomsgs    the two toggles off          --search=<text>     the search pane open with this text
//         --searchopen         the pane open, empty         --closed            the collapsed rail
//         --window=<n>         this window's index          --scale=<n>         device scale (2 = a 192-dpi shot)
//         --skew=<ms>          State.clockSkew              --dump              also write <out.png>.txt: sizes, order,
//         --bench=<n>          time n draws (into the .txt)                     spans, and a hit-test sweep
//         --confirm=<kind>[:<account id>[:<0|1>]]   the selected tile asks (move | refresh | close; browser off/on)
//         --rename=<text>      the selected tile's title is the rename box, the text selected, focused
//         --hold=<kind>[:<text>]   the Special prompt form on the selected tile (next | timed | recurring | triggered)
//         --drag=<id>          that tile is being dragged     --drop=<id>:<before|after>   the reorder mark
//         --mouse=<x>,<y>      the pointer (the pin's corner zone)   --restartconfirm   "Stop n and restart"
//         --menu=<x>,<y>,<label|-|*label|~label>,…   a context menu at x,y (- separator, * checked, ~ dim)
//         --prompt=<x>,<y>,<placeholder>            the context menu's inline prompt
//         --settings=<settings.json>   the Settings panel over the sidebar, filled from a getSettings answer
//         --settingsscroll=<px>        … scrolled      --toolsw=<px>   the tool row's pinned width
// Built -mwindows: no console, no window; nothing appears on screen. Exit code 0 = the PNG was written.
#include "../src/ui.hpp"
#include <shellapi.h>
#include <fstream>
#include <sstream>

using namespace deck;

static std::string utf8(const wchar_t* w) { return narrow(std::wstring(w)); }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc < 3) return 64;
  std::vector<std::wstring> pos; std::vector<std::string> flags;
  for (int i = 1; i < argc; i++) { std::wstring a = argv[i]; if (a.rfind(L"--", 0) == 0) flags.push_back(utf8(a.c_str())); else pos.push_back(a); }
  if (pos.size() < 2) return 64;
  int width = pos.size() > 2 ? _wtoi(pos[2].c_str()) : 340, height = pos.size() > 3 ? _wtoi(pos[3].c_str()) : 900;
  float scroll = pos.size() > 4 ? (float)_wtof(pos[4].c_str()) : 0;

  ui::json meta;
  {
    std::ifstream in(pos[0].c_str(), std::ios::binary);
    if (!in) return 66;
    std::stringstream ss; ss << in.rdbuf();
    meta = ui::json::parse(ss.str(), nullptr, false);
    if (meta.is_discarded()) return 65;
  }

  ui::State st; st.sidebarWidth = (float)width; st.scroll = scroll;
  float scale = 1; bool dump = false; int bench = 0;
  static EditBox renameEdit, searchEdit; static ui::HoldForm hold; static ui::CtxMenu menu; static ui::Settings settings;
  std::string confirmSpec, renameText, holdSpec; bool renaming = false;
  for (auto& f : flags) {
    auto val = [&](const char* k) -> const char* { size_t n = strlen(k); return f.compare(0, n, k) == 0 ? f.c_str() + n : nullptr; };
    if (const char* v = val("--sel=")) st.selected = v;
    else if (const char* v = val("--hover=")) st.hover = v;
    else if (const char* v = val("--part=")) st.hoverPart = v;
    else if (const char* v = val("--collapse=")) st.collapsedGroups.insert(v);
    else if (const char* v = val("--search=")) { st.search = v; st.searchOpen = true; }
    else if (const char* v = val("--window=")) st.windowIndex = atoi(v);
    else if (const char* v = val("--scale=")) scale = (float)atof(v);
    else if (const char* v = val("--skew=")) st.clockSkew = atoll(v);
    else if (f == "--grouped") st.grouped = true;
    else if (f == "--nobars") st.showBars = false;
    else if (f == "--nomsgs") st.showMsgs = false;
    else if (f == "--searchopen") st.searchOpen = true;
    else if (f == "--closed") st.sidebarOpen = false;
    else if (f == "--dump") dump = true;
    else if (const char* v = val("--bench=")) bench = atoi(v);
    else if (const char* v = val("--settings=")) {
      std::ifstream in(widen(v).c_str(), std::ios::binary); std::stringstream ss; ss << in.rdbuf();
      ui::json sj = ui::json::parse(ss.str(), nullptr, false);
      if (sj.is_object()) { settings.newDriver = ui::newDriverWords(meta); ui::settingsFill(settings, sj); settings.open = true; }
    }
    else if (const char* v = val("--settingsscroll=")) settings.scroll = (float)atof(v);
    else if (const char* v = val("--toolsw=")) st.toolsWidth = (float)atof(v);
    else if (const char* v = val("--confirm=")) confirmSpec = v;
    else if (const char* v = val("--rename=")) { renameText = v; renaming = true; }
    else if (const char* v = val("--hold=")) holdSpec = v;
    else if (const char* v = val("--drag=")) st.dragging = v;
    else if (const char* v = val("--drop=")) { std::string s = v; size_t c = s.rfind(':'); st.dropMark = s.substr(0, c); st.dropBefore = c != std::string::npos && s.substr(c + 1) == "before"; }
    else if (const char* v = val("--mouse=")) { st.mouseX = (float)atof(v); const char* c = strchr(v, ','); st.mouseY = c ? (float)atof(c + 1) : 0; }
    else if (f == "--restartconfirm") st.restartConfirming = true;
    else if (const char* v = val("--menu=")) {
      std::vector<std::string> parts; { std::string s = v; size_t p = 0; for (;;) { size_t q = s.find(',', p); parts.push_back(s.substr(p, q == std::string::npos ? q : q - p)); if (q == std::string::npos) break; p = q + 1; } }
      if (parts.size() >= 2) { menu.open = true; menu.x = (float)atof(parts[0].c_str()); menu.y = (float)atof(parts[1].c_str()); }
      for (size_t i = 2; i < parts.size(); i++) { ui::MenuItem it; std::string l = parts[i]; if (l == "-") it.sep = true; else { if (l[0] == '*') { it.checked = true; l = l.substr(1); } else if (l[0] == '~') { it.dim = true; l = l.substr(1); } it.label = widen(l); } menu.items.push_back(it); }
    }
    else if (const char* v = val("--prompt=")) {
      std::string s = v; size_t a = s.find(','), b = a == std::string::npos ? a : s.find(',', a + 1);
      if (b != std::string::npos) { menu.open = menu.prompt = true; menu.x = (float)atof(s.c_str()); menu.y = (float)atof(s.c_str() + a + 1); menu.input.placeholder = widen(s.substr(b + 1)); st.focusEdit = &menu.input; }
    }
  }
  if (!confirmSpec.empty() && !st.selected.empty()) {
    ui::State::Confirm c; size_t a = confirmSpec.find(':'); c.kind = confirmSpec.substr(0, a);
    if (a != std::string::npos) { size_t b = confirmSpec.find(':', a + 1); c.account = confirmSpec.substr(a + 1, b == std::string::npos ? b : b - a - 1); if (b != std::string::npos) c.browser = atoi(confirmSpec.c_str() + b + 1); }
    st.confirm[st.selected] = c;
  }
  if (renaming && !st.selected.empty()) { st.renaming = st.selected; st.renameEdit = &renameEdit; renameEdit.set(widen(renameText)); renameEdit.selectAll(); st.focusEdit = &renameEdit; }
  if (st.searchOpen) { st.searchEdit = &searchEdit; searchEdit.set(widen(st.search)); }
  if (!holdSpec.empty() && !st.selected.empty()) {
    size_t a = holdSpec.find(':'); hold.kind = holdSpec.substr(0, a); hold.tile = st.selected;
    if (a != std::string::npos) hold.text.set(widen(holdSpec.substr(a + 1)));
    hold.every.set(L"30"); hold.from.set(L"09:00 AM"); hold.to.set(L"05:00 PM"); hold.time.set(L"11/18/2026, 03:15 PM");
    hold.picks.push_back({ L"session reset +5m 15:15", 0 }); hold.picks.push_back({ L"week reset +5m sat 11:27", 0 }); hold.picks.push_back({ L"in 1 h 11:10", 0 });
    st.hold = &hold; st.confirm[st.selected] = { "hold", "", -1 }; st.focusEdit = &hold.text;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Gfx g; if (!g.init()) return 70;
  if (!g.targetBitmap(width, height, scale)) return 71;
  ui::Sidebar sb;
  g.begin(); g.clear(Rgba::hex(0x0c0c0c));
  sb.draw(g, RectF(0, 0, (float)width, (float)height), meta, st);
  if (settings.open) ui::drawSettings(g, meta, st, settings, RectF(0, 0, (float)width, (float)height));
  if (menu.open) ui::drawCtxMenu(g, meta, st, menu, (float)width, (float)height);
  if (!g.end()) return 72;
  if (!g.savePng(pos[1])) return 73;

  double benchMs = 0;
  if (bench > 0) {   // the cost of one Sidebar::draw on this target, averaged
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < bench; i++) { g.begin(); sb.draw(g, RectF(0, 0, (float)width, (float)height), meta, st); g.end(); }
    QueryPerformanceCounter(&b);
    benchMs = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart / bench;
    dump = true;
  }

  if (dump) {
    std::ofstream out((pos[1] + L".txt").c_str(), std::ios::binary);
    if (bench > 0) out << "draw: " << benchMs << " ms/frame over " << bench << " frames\n";
    out << "contentHeight " << sb.contentHeight() << "  viewHeight " << sb.viewHeight() << "\n";
    out << "order:";
    for (auto& id : sb.order()) { float a = 0, b = 0; sb.tileSpan(id, a, b); out << " " << id << "[" << a << ".." << b << "]"; }
    out << "\n";
    for (auto& h : sb.all()) if (h.kind == "tool") out << "tool " << h.id << "  tip: " << narrow(sb.tooltipAt(h.rect.x + h.rect.w / 2, h.rect.y + h.rect.h / 2)) << "\n";
    const float xs[3] = { 3.f, width / 2.f, width - 12.f };
    for (float x : xs) {
      out << "x=" << x << "\n";
      std::string last;
      for (float y = 0.5f; y < height; y += 1) {
        ui::Hit h = sb.hitTest(x, y);
        std::string tip = narrow(sb.tooltipAt(x, y));
        std::string key = h.kind + " " + h.id + (h.arg.empty() ? "" : " (" + h.arg + ")") + (tip.empty() ? "" : "  tip: " + tip);
        if (key != last) { out << "  y=" << (int)y << "  " << key << "\n"; last = key; }
      }
    }
  }
  return 0;
}
