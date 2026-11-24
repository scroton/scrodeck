// windows/src/overlays.hpp — the things the Electron renderer lays OVER the sidebar or the window, drawn.
//
// Included by sidebar.cpp only, after SidebarPainter (these painters use its text and box rules, so a menu's
// 12px line is the sidebar's 12px line). Each painter draws a model from ui.hpp and writes back where it put
// things (box, rows, zones); the app reads those for the mouse. Sources: renderer/style.css `.palette`,
// `#pull-menu` + `.pull-*`, `#moveall-menu` + `.move-*` + `.pick-row`, `.hold-form` + `.sp-*`; index.html;
// js/88-palette.js, 45-elsewhere.js, 47-past.js, 60-move.js, 30-tiles.js renderHoldForm.

namespace {
inline void oz(std::vector<OZone>& v, const char* kind, const std::string& id, const RectF& r, const std::wstring& tip = L"", const std::string& arg = "") {
  OZone z; z.kind = kind; z.id = id; z.arg = arg; z.rect = r; z.tip = tip; v.push_back(z);
}
// a css button: background, 1px border, centred label
inline void obtn(SidebarPainter& p, const RectF& r, const std::wstring& label, const Font& f, bool hot, bool danger, const Rgba& borderC = kBtnBorder, const Rgba& hotBg = Rgba::hex(0x303030)) {
  p.fill(r, hot ? (danger ? Rgba::hex(0x3a1f1f) : hotBg) : kBtn); p.border(r, danger ? kDanger : borderC);
  p.tIn(label, r, f, danger ? Rgba::hex(0xf2b3b3) : kFg);
}
}  // namespace

// ================================================================================================ the palette
// css .palette: the whole window dimmed (#0008); .pal-box min(560px, 90vw) at 12vh, #161616, 1px #3a3a3a; the input
// 14px with 9px 12px padding and a 1px #333 line under it; .pal-list up to 50vh, 4px above and below; .pal-group
// 10px capitals; .pal-row 13px, 5px 12px, a 2px left edge that is white on the selected row.
void drawPalette(Gfx& g, const json& meta, const State& st, Palette& m, float viewW, float viewH) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, RectF(0, 0, viewW, viewH));
  g.fill(RectF(0, 0, viewW, viewH), Rgba(0, 0, 0, 0x88 / 255.f));
  const float w = std::floor(std::min(560.f, viewW * 0.9f)), x = std::floor((viewW - w) / 2), y = std::floor(viewH * 0.12f);
  const float inH = p.LN(14) + 18 + 1, groupH = p.LN(10) + 8, rowH = p.LN(13) + 10;
  // the list's content: a heading whenever the group changes
  struct Line { bool group; int item; float y, h; };
  std::vector<Line> lines; float cy = 4; std::string last = "\x01";
  for (size_t i = 0; i < m.items.size(); i++) {
    if (m.items[i].group != last) { lines.push_back({ true, (int)i, cy, groupH }); cy += groupH; last = m.items[i].group; }
    lines.push_back({ false, (int)i, cy, rowH }); cy += rowH;
  }
  const float content = cy + 4, listH = std::min(content, std::floor(viewH * 0.5f));
  const RectF box(x, y, w, 1 + inH + listH + 1);
  shadow(g, box, 12, 40, 0.8f);
  p.fill(box, Rgba::hex(0x161616)); p.border(box, Rgba::hex(0x3a3a3a));
  const RectF in(x + 1, y + 1, w - 2, inH);
  p.fill(in, Rgba::hex(0x0f0f0f)); p.fill(RectF(in.x, in.b() - 1, in.w, 1), Rgba::hex(0x333333));
  m.input.placeholder = L"type a command or a session name";
  m.input.draw(g, RectF(in.x + 12, in.y + 9, in.w - 24, p.LN(14)), p.F(14), kFg, Rgba::hex(0x757575), st.focusEdit == &m.input, st.blinkOn);
  // the selected row is kept in view (scrollIntoView({ block: 'nearest' }))
  const RectF list(x + 1, in.b(), w - 2, listH);
  for (auto& l : lines) if (!l.group && l.item == m.index) { if (l.y < m.scroll) m.scroll = l.y; else if (l.y + l.h > m.scroll + listH) m.scroll = l.y + l.h - listH; }
  m.scroll = std::max(0.f, std::min(std::max(0.f, content - listH), m.scroll));
  m.rows.assign(m.items.size(), RectF());
  g.pushClip(list);
  for (auto& l : lines) {
    const float ly = list.y + l.y - m.scroll;
    if (ly + l.h < list.y || ly > list.b()) continue;
    const PalItem& it = m.items[l.item];
    if (l.group) {
      std::wstring name = W(it.group); for (auto& c : name) c = (wchar_t)towupper(c);
      p.tracking = 0.6f; p.tb(name, list.x + 12, ly + 6 + p.baseIn(p.LN(10), 10), list.w - 24, p.F(10), kFaint, Align::Left, false); p.tracking = 0;
      continue;
    }
    const RectF row(list.x, ly, list.w, l.h);
    const bool sel = l.item == m.index;
    if (sel) { p.fill(row, Rgba::hex(0x262626)); p.fill(RectF(row.x, row.y, 2, row.h), Rgba::hex(0xffffff)); }
    const float hw = it.hint.empty() ? 0 : p.tw(it.hint, p.F(11.5f));
    p.tb(it.label, row.x + 2 + 12, row.y + 5 + p.baseIn(p.LN(13), 13), row.w - 2 - 24 - (hw ? hw + 10 : 0), p.F(13), kFg, Align::Left, true);
    if (hw) p.tb(it.hint, row.r() - 12 - hw, row.y + 5 + p.baseIn(p.LN(11.5f), 11.5f), hw + 2, p.F(11.5f), kFaint, Align::Left, false);
    // only the part of a row inside the list answers the mouse
    const float y0 = std::max(row.y, list.y), y1 = std::min(row.b(), list.b());
    if (y1 > y0) m.rows[l.item] = RectF(row.x, y0, row.w, y1 - y0);
  }
  g.popClip();
  m.box = box;
}

// ================================================================================================ elsewhere / past
// css #pull-menu: over the sidebar, its bottom 8px above #sb-bottom, growing upward to the top of the sidebar
// (max-height set on open), scrolling with no bar; #1c1c1c, 1px #333, padding 4, gap 2. .pull-row 12px, 4px 6px:
// the account (600, green when busy) · the title (wraps) · the status (10.5px, faint). A clicked row asks in
// place: the row stays as the title, the question and the buttons under it, one white 1px bound round both.
void drawPullMenu(Gfx& g, const json& meta, const State& st, PullMenu& m, const RectF& area, float bottomTop) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, area);
  m.zones.clear();
  const float x = area.x + 4, w = area.w - 8, maxH = std::max(120.f, bottomTop - area.y - 8);
  const float inX = x + 1 + 4, inW = w - 2 - 8;
  const Font rf = p.F(12), af = p.F(12, 600), sf = p.F(10.5f), qf = p.F(11.5f), bf = p.F(11);
  const float lh = p.LN(12);
  const bool past = m.which == "past";
  struct R { float y, h, titleW, abbrW, statW, qH; };
  std::vector<R> rs; float cy = 4;
  const float headH = past ? 2 + (lh + 4 + 2) + 4 : 0;
  if (past) cy += headH + 2;
  const float emptyH = 12 + p.LN(11.5f);
  for (size_t i = 0; i < m.rows.size(); i++) {
    const PullRow& r = m.rows[i];
    const bool asking = (int)i == m.confirm;
    const float rowW = asking ? inW - 2 : inW;
    R q; q.abbrW = p.tw(r.label, af) + (r.cloud ? 3 + 12 : 0); q.statW = p.tw(r.status, sf);
    q.titleW = std::max(20.f, rowW - 12 - q.abbrW - 6 - 6 - q.statW);
    float th = p.wrap(r.title, 0, 0, q.titleW, rf, kFg, lh, 0, true, false);
    float rowH = std::max(lh, th) + 8;
    q.qH = 0;
    if (asking) { q.qH = p.wrap(m.question, 0, 0, rowW - 12, qf, kDim, p.LN(11.5f), 0, false, false); rowH = 1 + rowH + 4 + q.qH + 4 + 17 + 4 + 1; }
    q.y = cy; q.h = rowH; rs.push_back(q);
    cy += rowH + ((!past && i + 1 < m.rows.size()) ? 2 : 0);
  }
  if (m.rows.empty() && !m.empty.empty()) cy += emptyH;
  const float content = cy + 4;
  const float h = std::min(maxH, content + 2);
  const RectF box = p.snap(RectF(x, bottomTop - 8 - h, w, h));
  m.contentH = content; m.scroll = std::max(0.f, std::min(std::max(0.f, content - (h - 2)), m.scroll));
  shadow(g, box, 8, 24, 0.667f);
  p.fill(box, Rgba::hex(0x1c1c1c)); p.border(box, Rgba::hex(0x333333));
  const RectF view(box.x + 1, box.y + 1, box.w - 2, box.h - 2);
  g.pushClip(view);
  const float oy = view.y - m.scroll;
  if (past) {
    // .past-head: the filter box and "n on disk"
    const float nw = p.tw(m.note, p.F(11.5f));
    const RectF in(inX + 2, oy + 4 + 2, std::max(20.f, inW - 4 - 6 - nw), lh + 4 + 2);
    const bool foc = st.focusEdit == &m.filter;
    if (foc) p.border(RectF(in.x - 1, in.y - 1, in.w + 2, in.h + 2), Rgba::hex(0x555555));
    p.fill(in, Rgba::hex(0x0f0f0f)); p.border(in, Rgba::hex(0x333333));
    m.filter.placeholder = L"filter past sessions";
    m.filter.draw(g, RectF(in.x + 7, in.y + 1, in.w - 14, in.h - 2), rf, kFg, Rgba::hex(0x757575), foc, st.blinkOn);
    oz(m.zones, "edit", "past-filter", in);
    p.tIn(m.note, RectF(in.r() + 6, in.y, nw + 2, in.h), p.F(11.5f), Rgba::hex(0x8c8c8c), Align::Left, false);
  }
  for (size_t i = 0; i < m.rows.size(); i++) {
    const PullRow& r = m.rows[i]; const R& q = rs[i];
    const bool asking = (int)i == m.confirm;
    float ry = oy + q.y, rx = inX, rw = inW;
    if (ry + q.h < view.y || ry > view.b()) continue;
    if (asking) { p.border(RectF(inX, ry, inW, q.h), Rgba::hex(0xffffff)); rx += 1; rw -= 2; ry += 1; }
    const float rowH = asking ? q.h - 2 - 4 - q.qH - 4 - 17 - 4 : q.h;
    const RectF row(rx, ry, rw, rowH);
    if ((int)i == m.hover) p.fill(row, asking ? Rgba::hex(0x161616) : Rgba::hex(0x2a2a2a));
    const float mid = row.y + (rowH - lh) / 2;                       // align-items: center
    float tx = row.x + 6;
    p.tb(r.label, tx, mid + p.baseIn(lh, 12), q.abbrW + 2, af, r.busy ? kGreen : kDim, Align::Left, false);
    if (r.cloud) { SidebarPainter::Xf xf(p, p.rnd(tx + p.tw(r.label, af) + 3), p.rnd(mid + (lh - 12) / 2), 12.f / 16); p.fillD(kCloudPath, r.busy ? kGreen : kCloud); }
    tx += q.abbrW + 6;
    p.wrap(r.title, tx, row.y + 4, q.titleW, rf, kFg, lh, 0, true, true);
    p.tb(r.status, row.r() - 6 - q.statW, mid + p.baseIn(lh, 10.5f), q.statW + 2, sf, kFaint, Align::Left, false);
    if (!asking) oz(m.zones, "pull-row", std::to_string(i), RectF(row.x, std::max(row.y, view.y), row.w, std::max(0.f, std::min(row.b(), view.b()) - std::max(row.y, view.y))));
    if (asking) {
      float qy = row.b() + 4;
      p.wrap(m.question, row.x + 6, qy, rw - 12, qf, kDim, p.LN(11.5f), 0, false, true);
      qy += q.qH + 4;
      float bx = row.x + 6;
      const float yw = p.tw(m.yesLabel, bf) + 12 + 2, nw2 = p.tw(L"Cancel", bf) + 12 + 2;
      const RectF yes(bx, qy, yw, 17), no(bx + yw + 4, qy, nw2, 17);
      obtn(p, yes, m.yesLabel, bf, st.hoverPart == "pull-yes", true);
      obtn(p, no, L"Cancel", bf, st.hoverPart == "pull-no", false);
      oz(m.zones, "pull-yes", std::to_string(i), yes); oz(m.zones, "pull-no", std::to_string(i), no);
    }
  }
  if (m.rows.empty() && !m.empty.empty()) p.tb(m.empty, inX + 6, oy + 4 + headH + (past ? 2 : 0) + 6 + p.baseIn(p.LN(11.5f), 11.5f), inW - 12, p.F(11.5f), kFaint, Align::Left, true);
  g.popClip();
  m.box = box;
}

// ================================================================================================ move every session
// css #moveall-menu: over the sidebar, its bottom 42px above the sidebar's; #1c1c1c, 1px #333, padding 6px 8px 8px,
// gap 6, 12px. The two accounts are <select>s (drawn as their closed box; the list is a menu), each with that
// account's usage bars under it.
void drawMoveAll(Gfx& g, const json& meta, const State& st, MoveAll& m, const RectF& area) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, area);
  m.zones.clear();
  const float x = area.x + 4, w = area.w - 8, cx = x + 1 + 8, cw = w - 2 - 16;
  const float lh = p.LN(12), selH = lh + 4 + 2;
  auto label = [&](const std::string& id) { return W(p.acctLabel(id)); };
  std::map<std::string, int> cnt;
  const json& tiles = O(meta, "tiles");
  if (tiles.is_array()) for (auto& t : tiles) if (S(t, "kind") == "claude") cnt[S(t, "account")]++;
  auto fake = [&](const std::string& id) { json t = json::object(); t["account"] = id; bool f = false; const json& sc = O(O(O(meta, "usage"), id), "scoped"); if (sc.is_array()) for (auto& s : sc) if (lower(S(s, "model")).find("fable") != std::string::npos) f = true; t["model"] = f ? "fable" : ""; t["kind"] = "claude"; return t; };
  const json fromT = fake(m.from), toT = fake(m.to);
  std::vector<const json*> list;
  if (m.advanced && tiles.is_array()) for (auto& t : tiles) if (S(t, "kind") == "claude" && S(t, "account") == m.from) list.push_back(&t);
  auto msgOf = [&](const json& t) { std::wstring src = W(S(t, "lastText")), line, out; auto flush = [&] { while (!line.empty() && iswspace(line.back())) line.pop_back(); bool blank = true; for (wchar_t c : line) if (!iswspace(c)) blank = false; if (!blank) { if (!out.empty()) out += L'\n'; out += line; } line.clear(); }; for (wchar_t c : src) { if (c == L'\r') continue; if (c == L'\n') flush(); else line += c; } flush(); return out; };
  const std::wstring title0 = L"Open sessions of one account as another";
  float tsize = 12; for (int guard = 0; guard < 20 && tsize > 8 && p.tw(title0, p.F(tsize)) > cw - 4; guard++) tsize -= 0.5f;
  const std::wstring optA = L"say ", optB = L"continue", optC = L" where the last message was a usage notice";
  const float pickMax = area.h * 0.4f;
  // one pass measures (draw = false), the second paints from the top it gives
  auto layout = [&](float top, bool draw) -> float {
    float y = top + 1 + 6;
    if (draw) p.tb(title0, cx + 2, y + 2 + p.baseIn(p.LN(tsize), tsize), cw - 4, p.F(tsize), kFg, Align::Left, false);
    y += 2 + p.LN(tsize) + 4 + 6;
    { // .move-mode: all | these
      const float bw = (cw - 4) / 2;
      for (int i = 0; i < 2; i++) {
        const RectF r(cx + i * (bw + 4), y, bw, 20); const bool on = (i == 1) == m.advanced;
        if (draw) { p.fill(r, on ? Rgba::hex(0x333333) : kBtn); p.border(r, on ? Rgba::hex(0x555555) : kBtnBorder); p.tIn(i ? L"these" : L"all", r, p.F(11.5f), on ? kFg : kDim); oz(m.zones, "move-mode", i ? "these" : "all", r); }
      }
      y += 20 + 6;
    }
    auto select = [&](const char* id, float sx, float sw, const std::wstring& text) {
      const RectF r(sx, y, sw, selH);
      if (draw) {
        p.fill(r, Rgba::hex(0x0f0f0f)); p.border(r, Rgba::hex(0x333333));
        p.tb(text, r.x + 5, r.y + 3 + p.baseIn(lh, 12), r.w - 10 - 14, p.F(12), kFg, Align::Left, true);
        { SidebarPainter::Xf xf(p, p.rnd(r.r() - 16), p.rnd(r.y + (selH - 10) / 2), 10.f / 24); p.strokeD("M6 9l6 6 6-6", 2.4f, kFg); }
        oz(m.zones, "move-select", id, r);
      }
    };
    select("from", cx, cw, label(m.from) + L" (" + std::to_wstring(cnt[m.from]) + L")"); y += selH + 4;
    y += p.bars(fromT, cx, y, cw, draw, false, "") + 4 + 4;
    if (draw) p.tb(L"as", cx, y + 3 + p.baseIn(lh, 12), 22, p.F(12), kFg, Align::Left, false);
    select("to", cx + 22 + 6, cw - 28, label(m.to)); y += selH + 4;
    y += p.bars(toT, cx, y, cw, draw, false, "") + 4 + 6;
    if (m.advanced) {
      // .move-pick: one row per session of the source account, ticked by default; white bound = picked
      float py = 0; std::vector<std::pair<float, float>> spans;
      const Font tf = p.F(12), mf = p.F(10);
      for (auto* t : list) {
        const std::wstring ti = S(*t, "title").empty() ? std::wstring(L"New session") : W(S(*t, "title")), msg = msgOf(*t);
        float h = 3 + p.wrap(ti, 0, 0, cw - 13, tf, kFg, lh, 0, true, false);
        if (!msg.empty()) h += p.wrap(msg, 0, 0, cw - 13, mf, kFg, 13, 2, true, false);
        static const std::regex usage("spend limit|usage limit|rate limit|limit resets|/usage-credits|hit your", std::regex::icase);
        if (!msg.empty() && std::regex_search(S(*t, "lastText"), usage)) h += p.LN(10);
        h += 3 + 2;
        spans.push_back({ py, h }); py += h + 3;
      }
      float content = list.empty() ? p.LN(11.5f) : py - 3;
      const float ph = std::min(content, pickMax);
      m.pickScroll = std::max(0.f, std::min(std::max(0.f, content - ph), m.pickScroll));
      if (draw) {
        const RectF view(cx, y, cw, ph);
        g.pushClip(view);
        if (list.empty()) p.tb(L"No sessions in this account.", cx, y + p.baseIn(p.LN(11.5f), 11.5f), cw, p.F(11.5f), Rgba::hex(0x8c8c8c), Align::Left, true);
        for (size_t i = 0; i < list.size(); i++) {
          const json& t = *list[i]; const std::string id = S(t, "id");
          const RectF r(cx, y + spans[i].first - m.pickScroll, cw, spans[i].second);
          if (r.b() < view.y || r.y > view.b()) continue;
          auto pk = m.picked.find(id); const bool sel = pk == m.picked.end() ? true : pk->second;
          p.fill(r, st.hoverPart == "move-pick" && st.hover == id ? Rgba::hex(0x1f1f1f) : Rgba::hex(0x161616));
          if (sel) { p.border(r, Rgba::hex(0xffffff)); p.fill(RectF(r.x, r.y, 2, r.h), Rgba::hex(0xffffff)); }
          const std::wstring ti = S(t, "title").empty() ? std::wstring(L"New session") : W(S(t, "title")), msg = msgOf(t);
          float ty = r.y + 1 + 3;
          ty += p.wrap(ti, r.x + 2 + 5, ty, cw - 13, tf, kFg, lh, 0, true, true);
          if (!msg.empty()) ty += p.wrap(msg, r.x + 2 + 5, ty, cw - 13, mf, Rgba::hex(0xb8b8b8), 13, 2, true, true);
          static const std::regex usage("spend limit|usage limit|rate limit|limit resets|/usage-credits|hit your", std::regex::icase);
          if (!msg.empty() && std::regex_search(S(t, "lastText"), usage)) p.tb(L"usage notice", r.x + 7, ty + p.baseIn(p.LN(10), 10), cw - 13, mf, kAmber, Align::Left, false);
          const float y0 = std::max(r.y, view.y), y1 = std::min(r.b(), view.b());
          if (y1 > y0) oz(m.zones, "move-pick", id, RectF(r.x, y0, r.w, y1 - y0));
        }
        g.popClip();
      }
      y += ph + 6;
    }
    { // .move-opt: [ ] say `continue` where the last message was a usage notice
      const Font of = p.F(11.5f), cf = p.F(11); const float olh = 11.5f * 1.35f;
      const float tx = cx + 13 + 6, tw0 = cw - 19;
      const std::wstring all = optA + optB + optC;
      const float th = p.wrap(all, 0, 0, tw0, of, kDim, olh, 0, false, false);
      if (draw) {
        const RectF cb(cx, y + 1, 13, 13);
        p.fill(cb, m.cont ? Rgba::hex(0x0075ff) : Rgba::hex(0x3b3b3b)); if (!m.cont) p.border(cb, Rgba::hex(0x858585));
        if (m.cont) { SidebarPainter::Xf xf(p, cb.x, cb.y, 13.f / 24); p.strokeD("M5 12.5l4.5 4.5L19 7.5", 3, Rgba::hex(0xffffff)); }
        // the code word wears its own box: drawn under the running text where the first line puts it
        const float aw = p.tw(optA, of), bw = p.tw(optB, cf);
        p.fill(RectF(tx + aw, y + 1, bw + 6, olh - 2), Rgba::hex(0x0f0f0f));
        p.wrap(all, tx, y, tw0, of, kDim, olh, 0, false, true);
        oz(m.zones, "move-continue", "", RectF(cx, y, cw, th));
      }
      y += th + 6;
    }
    { // .move-actions: the status (or the question with its two buttons) and Go
      const Font hf = p.F(11.5f), bf = p.F(11.5f);
      const float goW = p.tw(L"Go", bf) + 16 + 2;
      const float sw = cw - 8 - 4 - goW;
      float sh = 20;
      if (m.asking) {
        const float qh = p.wrap(m.question, 0, 0, sw, hf, Rgba::hex(0x8c8c8c), p.LN(11.5f), 0, false, false);
        const float yw = p.tw(L"Move them", bf) + 16 + 2, nw = p.tw(L"Cancel", bf) + 16 + 2;
        sh = std::max(20.f, qh + 2 + 20);
        if (draw) {
          p.wrap(m.question, cx, y, sw, hf, Rgba::hex(0x8c8c8c), p.LN(11.5f), 0, false, true);
          const RectF yes(cx + 4, y + qh + 2, yw, 20), no(cx + 4 + yw + 4, y + qh + 2, nw, 20);
          obtn(p, yes, L"Move them", bf, st.hoverPart == "move-yes", true); obtn(p, no, L"Cancel", bf, st.hoverPart == "move-no", false);
          oz(m.zones, "move-yes", "", yes); oz(m.zones, "move-no", "", no);
        }
      } else if (!m.status.empty()) {
        const float qh = p.wrap(m.status, 0, 0, sw, hf, Rgba::hex(0x8c8c8c), p.LN(11.5f), 0, false, false);
        sh = std::max(20.f, qh);
        if (draw) p.wrap(m.status, cx, y + (sh - qh) / 2, sw, hf, Rgba::hex(0x8c8c8c), p.LN(11.5f), 0, false, true);
      }
      if (draw) { const RectF go(cx + cw - goW, y + (sh - 20) / 2, goW, 20); obtn(p, go, L"Go", bf, st.hoverPart == "move-go", false); oz(m.zones, "move-go", "", go); }
      y += sh;
    }
    return y + 8 + 1 - top;
  };
  const float h = layout(0, false);
  const RectF box = p.snap(RectF(x, area.b() - 42 - h, w, h));
  shadow(g, box, 8, 24, 0.667f);
  p.fill(box, Rgba::hex(0x1c1c1c)); p.border(box, Rgba::hex(0x333333));
  layout(box.y, true);
  m.box = box;
}

// ================================================================================================ the Special prompt form
// 30-tiles.js renderHoldForm in the selected tile's .t-actions (css .hold-form, .sp-*): the kind tabs, what the
// kind does, the prompt, the kind's own fields, add / cancel.
float SidebarPainter::holdForm(const json& t, float cx, float y, float cw, bool draw) {
  HoldForm& f = *st.hold;
  const std::string id = S(t, "id");
  const Font bf = F(11), nf = F(11), inF = F(10.5f), labF = F(10);
  float yy = y;
  auto hot = [&](const std::string& arg) { return st.hover == id && st.hoverPart == "hold" && st.hoverArg == arg; };
  // kind 0: .hold-time / .sp-time (10.5px on #111, 1px #4e4e4e, padding 0 3); 1: a type=text / type=number input, where
  // the page's own `input[type=text]` rule outweighs .sp-num / .sp-trig (12.5px on #0f0f0f, 1px #333, padding 3px 6px).
  // icon: 1 = the calendar of a datetime-local box, 2 = the clock of a time box
  auto field = [&](EditBox& e, const char* name, float fx, float fy, float fw, const wchar_t* ph, int kind = 0, int icon = 0) {
    const RectF r(fx, fy, fw, 17);
    if (draw) {
      const bool foc = st.focusEdit == &e;
      if (foc) border(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), Rgba::hex(0x555555));
      fill(r, kind ? Rgba::hex(0x0f0f0f) : Rgba::hex(0x111111)); border(r, kind ? Rgba::hex(0x333333) : Rgba::hex(0x4e4e4e));
      e.placeholder = ph;
      const float pad = kind ? 7 : 4, iw = icon ? 16.f : 0.f;
      e.draw(g, RectF(r.x + pad, r.y + 1, r.w - 2 * pad - iw, 15), kind ? F(12.5f) : inF, kFg, Rgba::hex(0x757575), foc, st.blinkOn);
      if (icon == 1) { const RectF c = snap(RectF(r.r() - 15, r.y + 4, 9, 9)); border(c, kFg); fill(RectF(c.x, c.y, c.w, 3), kFg); }
      if (icon == 2) { const float ccx = r.r() - 10.5f, ccy = r.y + 8.5f; g.ring(ccx, ccy, 4.5f, 4.5f, kFg, 1); g.line(ccx, ccy - 2.5f, ccx, ccy, kFg, 1); g.line(ccx, ccy, ccx + 2, ccy + 1.5f, kFg, 1); }
      zone("edit", name, r);
      if (icon == 1) zone("hold", id, RectF(r.r() - 18, r.y, 18, r.h), L"Show the calendar", "cal");   // the calendar icon opens the picker
      if (&e == &f.time) f.timeBox = r;
      if (&e == &f.trig) f.trigBox = r;
    }
  };
  { // .sp-kinds
    static const struct { const char* k; const wchar_t* label; } kinds[4] = { { "next", L"Next turn" }, { "timed", L"At a time" }, { "recurring", L"Recurring" }, { "triggered", L"Triggered" } };
    float bx = cx, by = yy;
    for (auto& k : kinds) {
      const float bw = tw(k.label, bf) + 6 + 2;
      if (bx + bw > cx + cw + 0.5f && bx > cx) { bx = cx; by += 17 + 2; }
      const RectF r(bx, by, bw, 17); const bool on = f.kind == k.k;
      if (draw) { fill(r, hot(std::string("kind:") + k.k) ? Rgba::hex(0x303030) : kBtn); border(r, on ? Rgba::hex(0x6fa3d8) : Rgba::hex(0x4e4e4e)); tIn(k.label, r, bf, kFg); zone("hold", id, r, L"", std::string("kind:") + k.k); }
      bx += bw + 2;
    }
    yy = by + 17 + 3;
  }
  {
    const std::wstring note = f.kind == "next" ? L"Sent as soon as this session finishes its current turn and is idle at its prompt."
      : f.kind == "timed" ? L"Held on the desktop and typed in at the time you set; if the session is busy then, as soon as it is idle afterward."
      : f.kind == "recurring" ? L"While the deck is running and the time of day is in the window, sent every N minutes."
      : L"Sent when the named trigger goes true \x2014 a Claude session can raise it with the deck-triggers skill.";
    yy += wrap(note, cx, yy, cw, nf, kDim, 11 * 1.3f, 0, false, draw) + 3;
  }
  { // textarea.hold-text: three rows of 11px
    const float th = 3 * LN(11) + 4 + 2;
    const RectF r(cx, yy, cw, th);
    if (draw) {
      const bool foc = st.focusEdit == &f.text;
      fill(r, Rgba::hex(0x111111)); border(r, Rgba::hex(0x4e4e4e));
      if (foc) { border(RectF(r.x - 1, r.y - 1, r.w + 2, r.h + 2), Rgba::hex(0xffffff)); border(RectF(r.x - 2, r.y - 2, r.w + 4, r.h + 4), Rgba::hex(0x101010)); }   // the default focus ring
      f.text.multiline = true; f.text.placeholder = L"the prompt";
      f.text.draw(g, RectF(r.x + 4, r.y + 3, r.w - 8, r.h - 6), F(11), kFg, Rgba::hex(0x757575), foc, st.blinkOn);
      zone("edit", "hold-text", r);
    }
    yy += th + 3;
  }
  if (f.kind == "timed") {
    // .hold-when: the time, then the account's reset times as picks
    float bx = cx, by = yy;
    const float tw0 = 152;
    field(f.time, "hold-time", bx, by, tw0, L"mm/dd/yyyy, --:-- --", 0, 1); bx += tw0 + 3;
    for (size_t i = 0; i < f.picks.size(); i++) {
      const float bw = tw(f.picks[i].label, F(10)) + 6 + 2;
      if (bx + bw > cx + cw + 0.5f && bx > cx) { bx = cx; by += 17 + 3; }
      const RectF r(bx, by, bw, 17); const std::string arg = "pick:" + std::to_string(i);
      if (draw) { fill(r, hot(arg) ? Rgba::hex(0x303030) : kBtn); border(r, Rgba::hex(0x4e4e4e)); tIn(f.picks[i].label, r, F(10), kFg); zone("hold", id, r, L"", arg); }
      bx += bw + 3;
    }
    yy = by + 17 + 3;
  } else if (f.kind == "recurring") {
    // .sp-fields: every [n] min, between [from] and [to]
    float bx = cx, by = yy;
    struct L3 { const wchar_t* text; EditBox* e; const char* name; float w; };
    const L3 labs[3] = { { L"every", &f.every, "hold-every", 40 }, { L"min, between", &f.from, "hold-from", 82 }, { L"and", &f.to, "hold-to", 82 } };
    for (auto& l : labs) {
      const float lw = tw(l.text, labF), total = lw + 3 + l.w;
      if (bx + total > cx + cw + 0.5f && bx > cx) { bx = cx; by += 17 + 4; }
      if (draw) tb(l.text, bx, by + baseMid(17, 10), lw + 2, labF, kDim, Align::Left, false);
      field(*l.e, l.name, bx + lw + 3, by, l.w, L"", l.e == &f.every ? 1 : 0, l.e == &f.every ? 0 : 2);
      bx += total + 4;
    }
    yy = by + 17 + 3;
  } else if (f.kind == "triggered") {
    const float lw = tw(L"trigger", labF);
    if (draw) tb(L"trigger", cx, yy + baseMid(17, 10), lw + 2, labF, kDim, Align::Left, false);
    field(f.trig, "hold-trig", cx + lw + 3, yy, 130, L"trigger name", 1);
    yy += 17 + 3;
    // <datalist id="sp-trig-list">: the triggers the deck knows (state.triggers). As Chromium does, they drop from the
    // box as a list while it has the focus, narrowed by what is typed (a name typed in full is not offered again);
    // the list is drawn over everything (drawHoldPop), so here it is only worked out.
    if (draw) {
      std::string typed = narrow(f.trig.text); { const size_t a2 = typed.find_first_not_of(" \t"), b2 = typed.find_last_not_of(" \t"); typed = a2 == std::string::npos ? std::string() : typed.substr(a2, b2 - a2 + 1); }
      typed = lower(typed);
      f.trigList.clear();
      if (O(meta, "triggers").is_array()) for (auto& tr : O(meta, "triggers")) {
        const std::string n = S(tr, "name"); if (n.empty()) continue;
        const std::string ln = lower(n);
        if (!typed.empty() && (ln == typed || ln.find(typed) == std::string::npos)) continue;
        f.trigList.push_back(n);
      }
      if (f.trigIndex >= (int)f.trigList.size()) f.trigIndex = (int)f.trigList.size() - 1;
    }
    yy += wrap(L"Sent when this trigger turns on. Adding the prompt does not turn it on: a Claude session does, with the deck-triggers skill. Pick one the deck knows, or type a new name (the deck creates it, off).", cx, yy, cw, nf, kDim, 11 * 1.3f, 0, false, draw) + 3;   // .t-actions .confirm-text outweighs .sp-hint: 11px, dim
  }
  { // .btn-row: add | cancel | what went wrong
    const float aw = tw(L"add", bf) + 6 + 2, cwd = tw(L"cancel", bf) + 6 + 2;
    const RectF a(cx, yy, aw, 17), c(cx + aw + 4, yy, cwd, 17);
    if (draw) {
      fill(a, hot("add") ? Rgba::hex(0x303030) : kBtn); border(a, Rgba::hex(0x6fa3d8)); tIn(L"add", a, bf, kFg); zone("hold", id, a, L"", "add");
      fill(c, hot("cancel") ? Rgba::hex(0x303030) : kBtn); border(c, Rgba::hex(0x4e4e4e)); tIn(L"cancel", c, bf, kFg); zone("hold", id, c, L"", "cancel");
      if (!f.err.empty()) tb(f.err, c.r() + 4, yy + baseMid(17, 11), cx + cw - c.r() - 4, nf, kDanger, Align::Left, true);
    }
    yy += 17;
  }
  return yy - y;
}

// ================================================================================================ the cloud dialog
// 48-cloud.js openCloudDialog. css: .cloud-dlg-back fixed, rgba(0,0,0,.55), the box centred; .cloud-dlg
// min(640px, 100vw - 32px), border-box, #1c1c1c, 1px #3a3a3a, shadow 0 12px 40px #000c, padding 14px 16px 12px, a
// column with gap 10, 13px. .cd-title 14.5px 600, the cloud 16px in --cloud, gap 4. textarea / input: #0f0f0f, 1px
// #444 (#6fa3d8 with the focus), padding 6px 8px, 13px/1.4 system-ui; the textarea rows=7 (min-height 110px).
// .cd-folder a row: "Folder" (dim), gap 8, the box. .cd-note dim 12px/1.4. .btn-row gap 6: buttons --btn, 1px
// #4e4e4e (.primary #6fa3d8), padding 3px 14px, 12.5px, :hover #303030, :disabled 50 %; .cd-keys right, faint
// 11.5px. .cd-err #f2b3b3 12px, not there when empty.
void drawCloudDlg(Gfx& g, const json& meta, const State& st, CloudDlg& m, float viewW, float viewH) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, RectF(0, 0, viewW, viewH));
  g.fill(RectF(0, 0, viewW, viewH), Rgba(0, 0, 0, 0.55f));
  const float w = std::floor(std::max(120.f, std::min(640.f, viewW - 32))), inner = w - 2 - 32;
  Font sys; sys.family = L"Segoe UI"; sys.size = 13;      // font: 13px/1.4 system-ui, sans-serif
  const float lh = 18.2f, taH = 7 * lh + 12 + 2, inH = lh + 12 + 2;
  const float titleH = std::max(16.f, p.LN(14.5f));
  const std::wstring note = W("A git folder's repository goes to the cloud with it; any other folder starts the session empty. It runs on claude.ai under "
    + (m.email.empty() ? m.label : m.email) + ", not on this machine, and keeps going when the deck closes.");
  const float noteH = p.wrap(note, 0, 0, inner, p.F(12), kDim, 16.8f, 0, false, false);
  const float btnH = p.LN(12.5f) + 6 + 2;
  const float errH = m.err.empty() ? 0 : p.wrap(m.err, 0, 0, inner, p.F(12), kDim, p.LN(12), 0, false, false);
  const float total = 1 + 14 + titleH + 10 + taH + 10 + inH + 10 + noteH + 10 + btnH + (m.err.empty() ? 0 : 10 + errH) + 12 + 1;
  const RectF box(std::floor((viewW - w) / 2), std::floor(std::max(0.f, (viewH - total) / 2)), w, total);
  m.box = box; m.zones.clear();
  shadow(g, box, 12, 40, 0.8f);
  p.fill(box, Rgba::hex(0x1c1c1c)); p.border(box, Rgba::hex(0x3a3a3a));
  const float x = box.x + 1 + 16; float y = box.y + 1 + 14;
  // the title: the cloud, then "New cloud session · <label>"
  { SidebarPainter::Xf xf(p, p.rnd(x), p.rnd(y + (titleH - 16) / 2), 16.f / 16); p.fillD(kCloudPath, kCloud); }
  p.tb(W("New cloud session \xC2\xB7 " + m.label), x + 16 + 4, y + p.baseMid(titleH, 14.5f), inner - 20, p.F(14.5f, 600), kFg, Align::Left, true);
  y += titleH + 10;
  // the two boxes
  auto field = [&](EditBox& e, const RectF& r, const char* id) {
    const bool foc = st.focusEdit == &e;
    p.fill(r, Rgba::hex(0x0f0f0f)); p.border(r, Rgba::hex(foc ? 0x6fa3d8 : 0x444444));
    e.draw(g, RectF(r.x + 1 + 8, r.y + 1 + 6, r.w - 2 - 16, r.h - 2 - 12), sys, kFg, Rgba::hex(0x757575), foc, st.blinkOn);
    oz(m.zones, "edit", id, r);
  };
  m.task.multiline = true; m.task.lineHeight = lh; m.folder.lineHeight = lh;
  m.task.placeholder = L"What should it do? This is its first prompt.";
  field(m.task, RectF(x, y, inner, taH), "cdlg-task");
  y += taH + 10;
  const float flw = p.tw(L"Folder", p.F(13));
  p.tb(L"Folder", x, y + p.baseMid(inH, 13), flw + 2, p.F(13), kDim, Align::Left, false);
  field(m.folder, RectF(x + flw + 8, y, inner - flw - 8, inH), "cdlg-folder");
  y += inH + 10;
  p.wrap(note, x, y, inner, p.F(12), kDim, 16.8f, 0, false, true);
  y += noteH + 10;
  // Start / Cancel, the keys on the right
  const Font bf = p.F(12.5f);
  const float gw = p.tw(L"Start", bf) + 28 + 2, cw = p.tw(L"Cancel", bf) + 28 + 2;
  const RectF go(x, y, gw, btnH), cancel(x + gw + 6, y, cw, btnH);
  if (m.starting) p.pushOpacity(go, 0.5f);
  obtn(p, go, L"Start", bf, !m.starting && m.hover == "cdlg-go", false, Rgba::hex(0x6fa3d8));
  if (m.starting) p.popOpacity();
  obtn(p, cancel, L"Cancel", bf, m.hover == "cdlg-cancel", false, Rgba::hex(0x4e4e4e));
  oz(m.zones, "cdlg-go", "", go); oz(m.zones, "cdlg-cancel", "", cancel);
  const std::wstring keys = W("Ctrl+Enter starts \xC2\xB7 Esc cancels");
  const float kw = p.tw(keys, p.F(11.5f));
  p.tb(keys, x + inner - kw, y + p.baseMid(btnH, 11.5f), kw + 2, p.F(11.5f), kFaint, Align::Left, false);
  y += btnH;
  if (!m.err.empty()) { y += 10; p.wrap(m.err, x, y, inner, p.F(12), Rgba::hex(0xf2b3b3), p.LN(12), 0, false, true); }
}

// Settings' colour box picker (ui.hpp ColorPick): Chromium's popup for `<input type=color>` on Windows, in the deck's
// colours. 12px padding; the square 216 x 130 (the hue at full saturation, white from the left, black from the
// bottom, a ring where the colour is); a row with the colour in a circle and the hue strip; "HEX" and the value.
void drawColorPick(Gfx& g, const json& meta, const State& st, ColorPick& m, float viewW, float viewH) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, RectF(0, 0, viewW, viewH));
  const float pad = 12, inner = 216, svH = 130, rowH = 24, inH = 26;
  const float w = 1 + pad + inner + pad + 1, hgt = 1 + pad + svH + pad + rowH + pad + inH + pad + 1;
  // under the box it opened from, inside the window; above it when there is no room below
  float x = std::floor(m.anchor.x), y = std::floor(m.anchor.b() + 4);
  if (x + w > viewW - 4) x = std::max(4.f, std::floor(viewW - 4 - w));
  if (y + hgt > viewH - 4) y = std::max(4.f, std::floor(m.anchor.y - 4 - hgt));
  m.box = RectF(x, y, w, hgt); m.zones.clear();
  shadow(g, m.box, 8, 24, 0.6f);
  p.fill(m.box, Rgba::hex(0x1c1c1c)); p.border(m.box, Rgba::hex(0x3a3a3a));
  const float ix = x + 1 + pad; float iy = y + 1 + pad;
  // the square
  m.sv = RectF(ix, iy, inner, svH);
  g.fill(m.sv, Rgba::hex(hsvToRgb(m.h, 1, 1)));
  if (ID2D1LinearGradientBrush* lg = g.gradient({ { 0.f, Rgba(1, 1, 1, 1) }, { 1.f, Rgba(1, 1, 1, 0) } }, m.sv.x, 0, m.sv.r(), 0)) g.rt->FillRectangle(g.d(m.sv), lg);
  if (ID2D1LinearGradientBrush* lg = g.gradient({ { 0.f, Rgba(0, 0, 0, 0) }, { 1.f, Rgba(0, 0, 0, 1) } }, 0, m.sv.y, 0, m.sv.b())) g.rt->FillRectangle(g.d(m.sv), lg);
  p.border(m.sv, Rgba::hex(0x3a3a3a));
  {
    const float cx = m.sv.x + m.s * inner, cy = m.sv.y + (1 - m.v) * svH;
    g.ring(cx, cy, 6, 6, Rgba(1, 1, 1, 1), 2); g.ring(cx, cy, 7.5f, 7.5f, Rgba(0, 0, 0, 0.45f), 1);
  }
  oz(m.zones, "cp-sv", "", m.sv);
  iy += svH + pad;
  // the colour, then the hue strip with its thumb
  const unsigned rgb = hsvToRgb(m.h, m.s, m.v);
  g.ellipse(ix + 12, iy + rowH / 2, 11, 11, Rgba::hex(rgb)); g.ring(ix + 12, iy + rowH / 2, 11, 11, Rgba::hex(0x555555), 1);
  m.hue = RectF(ix + 24 + 12, iy + (rowH - 12) / 2, inner - 36, 12);
  if (ID2D1LinearGradientBrush* lg = g.gradient({ { 0.f, Rgba::hex(0xff0000) }, { 1 / 6.f, Rgba::hex(0xffff00) }, { 2 / 6.f, Rgba::hex(0x00ff00) }, { 3 / 6.f, Rgba::hex(0x00ffff) },
                                                  { 4 / 6.f, Rgba::hex(0x0000ff) }, { 5 / 6.f, Rgba::hex(0xff00ff) }, { 1.f, Rgba::hex(0xff0000) } }, m.hue.x, 0, m.hue.r(), 0))
    g.rt->FillRectangle(g.d(m.hue), lg);
  {
    const float hx = m.hue.x + m.h / 360 * m.hue.w, hy = m.hue.y + m.hue.h / 2;
    g.ellipse(hx, hy, 7, 7, Rgba::hex(hsvToRgb(m.h, 1, 1))); g.ring(hx, hy, 7, 7, Rgba(1, 1, 1, 1), 2);
  }
  oz(m.zones, "cp-hue", "", RectF(m.hue.x - 4, iy, m.hue.w + 8, rowH));
  iy += rowH + pad;
  // the value
  const Font lf = p.F(11);
  p.tb(L"HEX", ix, iy + p.baseMid(inH, 11), 36, lf, kFaint, Align::Left, false);
  const RectF fr(ix + 40, iy, inner - 40, inH);
  const bool foc = st.focusEdit == &m.hex;
  p.fill(fr, Rgba::hex(0x0f0f0f)); p.border(fr, Rgba::hex(foc ? 0x6fa3d8 : 0x444444));
  Font sys; sys.family = L"Segoe UI"; sys.size = 13;
  m.hex.draw(g, RectF(fr.x + 1 + 8, fr.y + 1 + 4, fr.w - 2 - 16, fr.h - 2 - 8), sys, kFg, Rgba::hex(0x757575), foc, st.blinkOn);
  oz(m.zones, "edit", "cp-hex", fr);
}

// The special-prompt form's two popups (ui.hpp HoldForm::pop), over everything, hanging from their box.
// 1: Chromium's calendar for a datetime-local box - the month and year with the previous / next arrows, the days of
//    the week, six weeks of days (the other months' days dimmed), today ringed, the box's day filled; a click on a day
//    puts that date in the box and keeps its time. 2: the trigger box's <datalist> - one row per known trigger,
//    narrowed by what is typed, the highlighted row (arrow keys) lit; a click fills the box.
void drawHoldPop(Gfx& g, const json& meta, const State& st, HoldForm& f, float viewW, float viewH) {
  Sidebar scratch; SidebarPainter p(g, scratch, meta, st, RectF(0, 0, viewW, viewH));
  f.popZones.clear();
  auto place = [&](const RectF& anchor, float w, float h) {
    float x = std::floor(anchor.x), y = std::floor(anchor.b() + 1);
    if (x + w > viewW - 2) x = std::max(2.f, std::floor(viewW - 2 - w));
    if (y + h > viewH - 2) y = std::max(2.f, std::floor(anchor.y - 1 - h));
    return RectF(x, y, w, h);
  };
  if (f.pop == 1) {
    const float cell = 26, pad = 8, headH = 26, dowH = 18;
    const float w = 1 + pad + 7 * cell + pad + 1, h = 1 + pad + headH + dowH + 6 * cell + pad + 1;
    const RectF box = place(f.timeBox, w, h); f.popBox = box;
    shadow(g, box, 4, 12, 0.6f);
    p.fill(box, Rgba::hex(0x1c1c1c)); p.border(box, Rgba::hex(0x3a3a3a));
    const float x0 = box.x + 1 + pad; float y0 = box.y + 1 + pad;
    static const wchar_t* months[12] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August", L"September", L"October", L"November", L"December" };
    wchar_t title[40]; swprintf(title, 40, L"%ls %d", months[f.calM], f.calY);
    p.tb(title, x0 + 4, y0 + p.baseMid(headH, 13), 5 * cell, p.F(13, 600), kFg, Align::Left, true);
    const RectF prev(x0 + 5 * cell, y0, cell, headH), next(x0 + 6 * cell, y0, cell, headH);
    for (int k = 0; k < 2; k++) {
      const RectF r = k ? next : prev; const bool hot = st.hoverPart == "holdpop" && st.hover == (k ? "next" : "prev");
      if (hot) p.fill(r.inset(2), Rgba::hex(0x303030));
      const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, d = k ? 3.f : -3.f;
      g.line(cx - d, cy - 5, cx + d, cy, kFg, 1.5f); g.line(cx + d, cy, cx - d, cy + 5, kFg, 1.5f);
      oz(f.popZones, "holdpop", k ? "next" : "prev", r, k ? L"Next month" : L"Previous month");
    }
    y0 += headH;
    static const wchar_t* dows[7] = { L"Su", L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa" };
    for (int i = 0; i < 7; i++) p.tb(dows[i], x0 + i * cell, y0 + p.baseMid(dowH, 11), cell, p.F(11), kDim, Align::Center, false);
    y0 += dowH;
    // the first cell: the Sunday on or before the 1st
    struct tm first{}; first.tm_year = f.calY - 1900; first.tm_mon = f.calM; first.tm_mday = 1; first.tm_hour = 12; first.tm_isdst = -1; mktime(&first);
    const time_t now = time(nullptr); struct tm today{}; localtime_s(&today, &now);
    int selY = -1, selM = -1, selD = -1;
    { int mo = 0, d = 0, y = 0; if (swscanf(f.time.text.c_str(), L"%2d/%2d/%4d", &mo, &d, &y) == 3) { selY = y; selM = mo - 1; selD = d; }
      else if (swscanf(f.time.text.c_str(), L"%4d-%2d-%2d", &y, &mo, &d) == 3) { selY = y; selM = mo - 1; selD = d; } }
    for (int i = 0; i < 42; i++) {
      struct tm dd{}; dd.tm_year = f.calY - 1900; dd.tm_mon = f.calM; dd.tm_mday = 1 - first.tm_wday + i; dd.tm_hour = 12; dd.tm_isdst = -1; mktime(&dd);
      const RectF r(x0 + (i % 7) * cell, y0 + (i / 7) * cell, cell, cell);
      const bool inMonth = dd.tm_mon == f.calM, isToday = dd.tm_year == today.tm_year && dd.tm_mon == today.tm_mon && dd.tm_mday == today.tm_mday;
      const bool isSel = dd.tm_year + 1900 == selY && dd.tm_mon == selM && dd.tm_mday == selD;
      char arg[24]; snprintf(arg, sizeof arg, "%04d-%02d-%02d", dd.tm_year + 1900, dd.tm_mon + 1, dd.tm_mday);
      const bool hot = st.hoverPart == "holdpop" && st.hover == arg;
      if (isSel) p.fill(r.inset(2), Rgba::hex(0x6fa3d8)); else if (hot) p.fill(r.inset(2), Rgba::hex(0x303030));
      if (isToday && !isSel) p.border(r.inset(2), Rgba::hex(0x6fa3d8));
      p.tb(std::to_wstring(dd.tm_mday), r.x, r.y + p.baseMid(cell, 12), r.w, p.F(12), isSel ? Rgba::hex(0x0f0f0f) : inMonth ? kFg : kFaint, Align::Center, false);
      oz(f.popZones, "holdpop", arg, r);
    }
  } else if (f.pop == 2 && !f.trigList.empty()) {
    const float rowH = 20;
    const float w = std::max(f.trigBox.w, 160.f), h = 2 + rowH * (float)std::min<size_t>(f.trigList.size(), 8);
    const RectF box = place(f.trigBox, w, h); f.popBox = box;
    shadow(g, box, 4, 12, 0.6f);
    p.fill(box, Rgba::hex(0x1c1c1c)); p.border(box, Rgba::hex(0x3a3a3a));
    for (size_t i = 0; i < f.trigList.size() && i < 8; i++) {
      const RectF r(box.x + 1, box.y + 1 + i * rowH, box.w - 2, rowH);
      const bool lit = (int)i == f.trigIndex || (st.hoverPart == "holdtrig" && st.hover == f.trigList[i]);
      if (lit) p.fill(r, Rgba::hex(0x2b4a6b));
      p.tb(W(f.trigList[i]), r.x + 8, r.y + p.baseMid(rowH, 12), r.w - 16, p.F(12), kFg, Align::Left, true);
      oz(f.popZones, "holdtrig", f.trigList[i], r);
    }
  }
}
