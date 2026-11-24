// windows/src/askcards.hpp — the NATIVE ask pane: the scroller under the pane's bar, drawn by the shell (Direct2D /
// DirectWrite, the shell's own EditBox for the answer boxes), instead of a WebView2 browser process tree for it
// (2026-11-23: "native ask cards ... whichever is most performant"; config.json askPane "native", the default).
//
// WHAT IT STANDS IN FOR, piece by piece (asks.hpp keeps the model and the logic of 49-asks.js; it hands this view
// exactly what it hands the host document, so the two views cannot disagree):
//   * the host document (asks.hpp hostDocument): render() takes the same { sections, recent, empty } message and
//     lays out the same elements by the same css (the sticky session heads, Done / Copy all / Answer in the terminal
//     / Discard answers… and the confirm row, the round notes, the recently answered rows, the empty line), and
//     reports the same clicks (ui: done / copy / term / discard / keep / discard-yes / lhead);
//   * each page's iframe (the skill's page.html, embedded): its content is core/deck/askcards (the page parsed from
//     its own file, page.html's S and logic), drawn by page.html's dark-theme css (summary, the cards with their
//     context blocks, the option rows, the reply buttons, the question, the answer box, Copy, the FYI), and it speaks
//     the page's host bridge both ways (post: load / lock / get / clear; back: ready / change / answers), so
//     asks.hpp's Done, Copy all, Discard, the drafts (askDraft / askDraftGet) and the read-only log pages run
//     unchanged. A page with no askdata (an older skill's) is `legacy`, as the web pane calls it.
// Keys (the app routes them): Tab / Shift+Tab move between the answer boxes; Esc in a box leaves it (the pane keeps
// the keys), Esc on the pane closes it (asks.hpp key); Up / Down / PageUp / PageDown / Home / End / Space scroll.
// Links open in the default browser (or the deck's own rule for a local deck: asks.hpp linkClicked).
#pragma once
#include "gfx.hpp"
#include "edit.hpp"
#include "keys.hpp"
#include "../../core/deck/askcards.hpp"
#include "../../core/third_party/nlohmann/json.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace deck { namespace asks {

namespace ac = deck::core::askcards;

class CardsView {
 public:
  using json = nlohmann::json;
  struct Deps {
    std::function<void(const std::string& kind, const std::string& id, const json& msg)> page;   // a page's message to its host (ready / change / answers)
    std::function<void(const std::string& kind, const std::string& id)> legacy;                  // a page from before the host bridge
    std::function<void(const std::string& what, const std::string& sid, const std::string& id)> ui;   // a button of the host document
    std::function<void(const std::string& id, std::function<void(bool ok, const std::string& html, const std::string& err)>)> fetch;
    std::function<void(EditBox*)> focusEdit;                 // the app: this box has the keys (nullptr: none of the pane's)
    std::function<EditBox*()> focused;                       // the app's box with the keys
    std::function<void(const std::wstring&)> openLink;
    std::function<void(const std::string&)> copy;            // the clipboard
    std::function<void()> changed;                           // repaint
    std::function<UINT_PTR(unsigned ms, std::function<void()>)> after;
    std::function<void(UINT_PTR)> cancel;
    std::function<std::string()> now;                        // new Date().toISOString()
  };
  void init(const Deps& d) { d_ = d; }
  ~CardsView() { for (auto& kv : pages_) if (kv.second.changeT && d_.cancel) d_.cancel(kv.second.changeT); }

  // ---- the frames (asks.hpp makeFrame / dropFrame / askPost)
  void open(const std::string& kind, const std::string& id, const std::string& sid, const std::string& label) {
    const std::string key = kind + ":" + id;
    if (pages_.count(key)) return;
    NPage& p = pages_[key];
    p.kind = kind; p.id = id; p.sid = sid; p.label = label; p.gen = ++gen_;
    const unsigned g = p.gen;
    if (!d_.fetch) { p.st = Failed; p.err = "no way to load it"; return; }
    d_.fetch(id, [this, key, g](bool ok, const std::string& html, const std::string& err) {
      auto it = pages_.find(key); if (it == pages_.end() || it->second.gen != g) return;
      NPage& q = it->second;
      if (!ok) { q.st = Failed; q.err = err.empty() ? "it did not load" : err; dirty(); return; }
      q.page.reset(new ac::Page(ac::parsePage(html)));
      if (q.page->legacy) { q.st = Legacy; dirty(); if (d_.legacy) d_.legacy(q.kind, q.id); return; }
      if (!q.page->ok) { q.st = Failed; q.err = q.page->error; dirty(); return; }
      q.state.reset(new ac::State(q.page.get()));
      q.st = Ready;
      syncBoxes(q);
      dirty();
      json m = progress(q, false); m["askpage"] = "ready"; m["id"] = q.id; m["h"] = 1;
      m["session_id"] = jsonOf(q.page->data, "session_id"); m["round"] = jsonOf(q.page->data, "round");
      if (d_.page) d_.page(q.kind, q.id, m);
    });
  }
  void drop(const std::string& kind, const std::string& id) {
    auto it = pages_.find(kind + ":" + id); if (it == pages_.end()) return;
    unfocusIn(it->second);
    if (it->second.changeT && d_.cancel) d_.cancel(it->second.changeT);
    pages_.erase(it);
    dirty();
  }
  void dropAll() { std::vector<std::string> ks; for (auto& kv : pages_) ks.push_back(kv.first); for (auto& k : ks) { auto p = k.find(':'); drop(k.substr(0, p), k.substr(p + 1)); } }
  // host -> page (page.html "the host bridge"): load { state | null, readonly } / lock / get { req } / clear
  void post(const std::string& kind, const std::string& id, const json& m) {
    auto it = pages_.find(kind + ":" + id); if (it == pages_.end() || it->second.st != Ready) return;
    NPage& p = it->second;
    const std::string what = m.value("askpage", std::string());
    if (what == "load") {
      const json st = m.contains("state") ? m["state"] : json();
      const ac::OJ s = ac::OJ::parse(st.dump(-1, ' ', false, json::error_handler_t::replace), nullptr, false);
      p.state->load(!st.is_null() ? &s : nullptr, m.value("readonly", false));
      syncBoxes(p); dirty();
      if (!p.state->locked()) changeSoon(p);
    } else if (what == "lock") { p.state->lock(); syncBoxes(p); dirty(); }
    else if (what == "get") {
      const ac::OJ A = p.state->answers(d_.now ? d_.now() : std::string());
      json r = { { "askpage", "answers" }, { "id", p.id }, { "req", m.contains("req") ? m["req"] : json() },
                 { "answers", json::parse(ac::stringify(A), nullptr, false) }, { "text", ac::State::asText(A) }, { "locked", p.state->locked() } };
      if (d_.page) d_.page(p.kind, p.id, r);
    } else if (what == "clear") { if (p.state->clear()) { syncBoxes(p); dirty(); changeSoon(p); } }
  }
  // the host document's whole picture (asks.hpp render): sections, recent, empty
  void render(const json& m) { model_ = m; dirty(); }
  // op scroll: that session's section (or the recently answered, or the top) at the top, kept there for a moment
  // while the pages above it load, until the person scrolls, clicks or types
  void scrollTo(const std::string& to) { scrollTarget_ = to; scrollUntil_ = GetTickCount64() + 2500; dirty(); }
  void userActed() { scrollUntil_ = 0; }

  // ---- drawing
  void draw(Gfx& g, const RectF& area, bool blinkOn) {
    g_ = &g;
    if (area.w != area_.w || area.h != area_.h) { if (area.w != area_.w) layCache_.clear(); dirty_ = true; }
    area_ = area;
    if (dirty_ || builtW_ != area.w) build(g);
    clampScroll();
    g.pushClip(area);
    const float oy = area.y - scroll_;
    const EditBox* fe = d_.focused ? d_.focused() : nullptr;
    for (auto& p : prims_) if (p.sec < 0) drawPrim(g, p, area.x, oy, fe, blinkOn);
    for (size_t i = 0; i < secs_.size(); i++) {            // the sticky heads, over the content (css z-index 2)
      const float dy = stickDy((int)i);
      for (auto& p : prims_) if (p.sec == (int)i) drawPrim(g, p, area.x, oy + dy, fe, blinkOn);
    }
    g.popClip();
  }
  void dropDeviceResources() { brushes_.clear(); layCache_.clear(); dirty_ = true; }
  bool dirtyNow() const { return dirty_; }

  // ---- the mouse (area coordinates are the app's DIPs)
  bool mouseMove(float x, float y) {
    const int t = targetAt(x, y);
    const int h = t >= 0 && !targets_[t].off ? t : -1;
    if (h == hover_) return false;
    hover_ = h; return true;
  }
  bool hand(float x, float y) const { const int t = targetAt(x, y); return t >= 0 && !targets_[t].off && targets_[t].kind != "box"; }
  std::wstring tooltipAt(float x, float y) const { const int t = targetAt(x, y); return t >= 0 ? targets_[t].tip : std::wstring(); }
  // the box under the pointer: its id for the app's editFor
  EditBox* boxAt(float x, float y, std::string* id) {
    const int t = targetAt(x, y);
    if (t < 0 || targets_[t].kind != "box") return nullptr;
    if (id) *id = "askbox:" + targets_[t].a + "|" + targets_[t].b;
    return boxOf(targets_[t].a, targets_[t].b);
  }
  EditBox* boxById(const std::string& id) {
    if (id.rfind("askbox:", 0) != 0) return nullptr;
    const std::string r = id.substr(7); const size_t bar = r.find('|'); if (bar == std::string::npos) return nullptr;
    return boxOf(r.substr(0, bar), r.substr(bar + 1));
  }
  bool owns(const EditBox* e) const { if (!e) return false; for (auto& kv : pages_) for (auto& b : kv.second.boxes) if (b.second.get() == e) return true; return false; }
  // a click that is not in a box: true = it was over the scroller
  bool click(float x, float y) {
    userActed();
    if (!area_.has(x, y)) return false;
    const int t = targetAt(x, y);
    if (t < 0 || targets_[t].off) return true;
    const Target tg = targets_[t];                         // (the action may rebuild the targets)
    if (tg.kind == "ui") { if (d_.ui) d_.ui(tg.a, tg.b, tg.c); }
    else if (tg.kind == "lhead") { if (d_.ui) d_.ui("lhead", "", tg.c); }
    else if (tg.kind == "link") { if (d_.openLink && !tg.c.empty()) d_.openLink(widen(tg.c)); }
    else if (tg.kind == "pick") pick(tg.a, tg.b, tg.c);
    else if (tg.kind == "copy") copyCard(tg.a, tg.b);
    return true;
  }
  bool wheel(int delta) { userActed(); const float was = scroll_; scroll_ -= delta / 120.f * 100.f; clampScroll(); return scroll_ != was; }
  // a key while the pane has the keys and no box does. true = taken
  bool key(UINT vk, const Mods& m) {
    if (m.ctrl || m.alt) return false;
    const float page = std::max(40.f, area_.h - 40), was = scroll_;
    switch (vk) {
      case VK_TAB: userActed(); focusStep(m.shift ? -1 : 1); return true;
      case VK_DOWN: scroll_ += 40; break;
      case VK_UP: scroll_ -= 40; break;
      case VK_NEXT: scroll_ += page; break;
      case VK_PRIOR: scroll_ -= page; break;
      case VK_SPACE: scroll_ += m.shift ? -page : page; break;
      case VK_HOME: scroll_ = 0; break;
      case VK_END: scroll_ = 1e9f; break;
      default: return false;
    }
    userActed(); clampScroll();
    if (scroll_ != was && d_.changed) d_.changed();
    return true;
  }
  // a key while one of the pane's boxes has the keys, before the box: Tab / Shift+Tab, Esc. true = taken
  bool boxKey(EditBox* e, UINT vk, const Mods& m) {
    userActed();
    if (vk == VK_TAB && !m.ctrl && !m.alt) { focusStep(m.shift ? -1 : 1, e); return true; }
    if (vk == VK_ESCAPE && !m.ctrl && !m.alt && !m.shift) { if (d_.focusEdit) d_.focusEdit(nullptr); return true; }
    return false;
  }
  // the box with the keys stays in sight (a Tab to it, typing past the bottom)
  void reveal(EditBox* e) {
    if (!e || !g_) return;
    if (dirty_) build(*g_);
    for (auto& t : targets_) if (t.kind == "box" && boxOf(t.a, t.b) == e) {
      float cx = 0, cy = 0, ch = 18; e->caretXY(cx, cy, ch);
      const float head = headAt(t.r.y);
      const float top = t.r.y - 4, bot = std::min(t.r.b(), t.r.y + 4 + cy + ch + 8) + 4;
      if (top - scroll_ < head) scroll_ = top - head;
      else if (bot - scroll_ > area_.h) scroll_ = bot - area_.h;
      clampScroll();
      return;
    }
  }

  // ---- tests (app.cpp `ask` steps): what is shown, and where a thing is to click on it
  json dump() const {
    json o = { { "scroll", (int)scroll_ }, { "contentH", (int)contentH_ }, { "viewH", (int)area_.h }, { "pages", json::array() }, { "order", order_ } };
    const EditBox* fe = d_.focused ? d_.focused() : nullptr;
    o["focus"] = nullptr;
    for (auto& kv : pages_) {
      const NPage& p = kv.second;
      json pj = { { "key", kv.first }, { "st", p.st == Ready ? "ready" : p.st == Loading ? "loading" : p.st == Legacy ? "legacy" : "failed" }, { "err", p.err } };
      if (p.st == Ready) {
        const ac::State::Progress g = p.state->progress();
        pj["locked"] = p.state->locked(); pj["count"] = p.state->countText(); pj["drafted"] = g.drafted;
        json cards = json::array();
        for (auto& c : p.page->cards) {
          json chk = json::array();
          for (auto& op : c.options) if (p.state->checked(c.qid, op.key)) chk.push_back(op.key);
          for (auto& r : c.replies) if (p.state->checked(c.qid, r.pick)) chk.push_back(r.pick);
          cards.push_back({ { "qid", c.qid }, { "state", p.state->stateLabel(c.qid) }, { "checked", chk }, { "text", p.state->text(c.qid) } });
        }
        pj["cards"] = cards;
        json notes = json::object(); for (auto& n : p.notes) notes[n.first] = n.second;
        pj["notes"] = notes;
        for (auto& b : p.boxes) if (b.second.get() == fe) o["focus"] = kv.first + "|" + b.first;
      }
      o["pages"].push_back(pj);
    }
    json secs = json::array();
    if (model_.is_object() && model_.contains("sections")) for (auto& s : model_["sections"]) if (!s.value("hidden", false))
      secs.push_back({ { "name", s.value("name", std::string()) }, { "count", s.value("count", std::string()) }, { "flash", s.value("flash", std::string()) },
                       { "done", s.contains("done") ? s["done"] : json() }, { "confirm", s.contains("confirm") ? s["confirm"] : json() } });
    o["sections"] = secs;
    o["targets"] = targets_.size();
    return o;
  }
  // `what` = ui <what> [<n-th section>] | pick <n-th page> <qid> <k> | box <n> <qid> | copy <n> <qid> |
  // link <n-th link> | lhead <n-th row>: the target scrolled into sight, its centre in the app's DIPs
  bool targetCenter(const std::vector<std::string>& w, float& x, float& y) {
    if (!g_ || w.empty()) return false;
    if (dirty_) build(*g_);
    auto num = [&](size_t i) { return i < w.size() ? atoi(w[i].c_str()) : 0; };
    auto arg = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };
    int pick = -1, nth = 0;
    for (size_t i = 0; i < targets_.size() && pick < 0; i++) {
      const Target& t = targets_[i];
      if (w[0] == "ui" && t.kind == "ui" && t.a == arg(1)) { if (nth++ == num(2)) pick = (int)i; }
      else if (w[0] == "lhead" && t.kind == "lhead") { if (nth++ == num(1)) pick = (int)i; }
      else if (w[0] == "link" && t.kind == "link") { if (nth++ == num(1)) pick = (int)i; }
      else if ((w[0] == "pick" || w[0] == "box" || w[0] == "copy") && t.kind == w[0]) {
        const int pi = num(1);
        if (pi < 0 || pi >= (int)order_.size() || t.a != order_[pi].get<std::string>() || t.b != arg(2)) continue;
        if (w[0] == "pick" && t.c != arg(3)) continue;
        pick = (int)i;
      }
    }
    if (pick < 0) return false;
    const Target t = targets_[pick];
    if (t.sec >= 0 && t.sec < (int)secs_.size()) {         // a sticky head's: its section scrolled to where the head shows
      const float vy = t.r.y + stickDy(t.sec) - scroll_;
      if (vy < 0 || vy + t.r.h > area_.h) { scroll_ = secs_[t.sec].top; clampScroll(); }
    } else {                                               // in sight, under the sticky head
      const float head = headAt(t.r.y);
      if (t.r.y - scroll_ < head) scroll_ = t.r.y - head - 4;
      else if (t.r.b() - scroll_ > area_.h) scroll_ = t.r.b() - area_.h + 4;
      clampScroll();
    }
    const float dy = t.sec >= 0 ? stickDy(t.sec) : 0;
    x = area_.x + t.r.x + std::min(t.r.w / 2, 12.f); y = area_.y + t.r.y + dy - scroll_ + t.r.h / 2;
    return true;
  }

 private:
  enum PSt { Loading, Ready, Failed, Legacy };
  struct NPage {
    std::string kind, id, sid, label; PSt st = Loading; std::string err; unsigned gen = 0;
    std::unique_ptr<ac::Page> page; std::unique_ptr<ac::State> state;
    std::map<std::string, std::unique_ptr<EditBox>> boxes;  // qid -> its answer box
    std::map<std::string, std::string> notes;               // qid -> the Copy note ("Copied: ...")
    UINT_PTR changeT = 0;
  };
  struct Span { UINT32 at = 0, len = 0; bool bold = false, code = false; std::string href; uint32_t color = 0; bool hasColor = false; float size = 0; int weight = 0; };
  struct TextBlock {
    Com<IDWriteTextLayout> lay; float w = 0, h = 0, base = 0;
    std::vector<RectF> codeRects;                          // relative to the origin
    std::vector<std::pair<RectF, std::string>> links;
  };
  enum PK { Fill, Stroke, Dashed, Text, Box };
  struct Prim {
    PK k = Fill; RectF r; uint32_t c = 0; float a = 1; float wdt = 1;
    int hover = -1; uint32_t hc = 0;                       // the colour while target `hover` is under the pointer
    std::shared_ptr<TextBlock> t; uint32_t tc = 0;
    EditBox* box = nullptr; int sec = -1;
  };
  struct Target { RectF r; int sec = -1; std::string kind, a, b, c; std::wstring tip; bool off = false; };
  struct Sec { float top = 0, bottom = 0, headH = 0; };

  Deps d_;
  std::map<std::string, NPage> pages_;
  json model_ = json::object(), order_ = json::array();
  std::vector<Prim> prims_; std::vector<Target> targets_; std::vector<Sec> secs_;
  std::map<uint32_t, Com<ID2D1SolidColorBrush>> brushes_;
  std::map<std::wstring, std::shared_ptr<TextBlock>> layCache_;
  Gfx* g_ = nullptr; RectF area_; float builtW_ = -1, scroll_ = 0, contentH_ = 0;
  bool dirty_ = true; int hover_ = -1; unsigned gen_ = 0;
  std::string scrollTarget_; ULONGLONG scrollUntil_ = 0;
  std::wstring fam_;
  int curSec_ = -1;                                        // build: the sticky head the prims go to

  // ---- the page's palette (page.html :root[data-theme="dark"]) and the host document's (#ask-pane)
  static constexpr uint32_t K_BG = 0x0c0c0c, K_TILE = 0x171717, K_LINE = 0x2c2c2c, K_BOX = 0x1f1f1f, K_FG = 0xececec, K_DIM = 0x9a9a9a, K_FAINT = 0x6a6a6a,
    K_ACCENT = 0x6aa9ff, K_ACCENT_FG = 0x0b1220, K_GREEN = 0x3ecf5a, K_REC_EDGE = 0x2f6e3d, K_SEL_BG = 0x16243a, K_BTN = 0x262626, K_BTN_LINE = 0x4e4e4e,
    K_IN = 0x111111, K_ANS = 0x24211a, K_LATER = 0x121212, K_HEAD = 0x141414, K_HBTN_LINE = 0x383838, K_DANGER = 0xc94a4a, K_LROW = 0x121212, K_LROW_HOVER = 0x1b1b1b;
  static uint32_t bright(uint32_t c, float k) { auto ch = [&](int s) { return (uint32_t)std::min(255.f, ((c >> s) & 255) * k) << s; }; return ch(16) | ch(8) | ch(0); }
  static uint32_t mixc(uint32_t a, uint32_t b, float t) { auto ch = [&](int s) { return (uint32_t)(((a >> s) & 255) + (((int)((b >> s) & 255) - (int)((a >> s) & 255)) * t)) << s; }; return ch(16) | ch(8) | ch(0); }
  static json jsonOf(const ac::OJ& o, const char* k) { if (!o.is_object() || !o.contains(k)) return json(); return json::parse(o[k].dump(-1, ' ', false, ac::OJ::error_handler_t::replace), nullptr, false); }
  static std::string s(const json& j, const char* k) { if (!j.is_object()) return ""; auto it = j.find(k); return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(); }
  static bool b(const json& j, const char* k) { if (!j.is_object()) return false; auto it = j.find(k); return it != j.end() && it->is_boolean() && it->get<bool>(); }

  void dirty() { dirty_ = true; if (d_.changed) d_.changed(); }
  ID2D1SolidColorBrush* brush(Gfx& g, uint32_t c) {
    auto it = brushes_.find(c); if (it != brushes_.end()) return it->second.p;
    Com<ID2D1SolidColorBrush> br; const Rgba x = Rgba::hex(c);
    if (g.rt) g.rt->CreateSolidColorBrush(D2D1::ColorF(x.r, x.g, x.b, 1), br.put());
    brushes_[c] = br; return br.p;
  }
  const std::wstring& fam(Gfx& g) {
    if (!fam_.empty()) return fam_;
    fam_ = L"Segoe UI";
    Com<IDWriteFontCollection> fc;
    if (g.dw && SUCCEEDED(g.dw->GetSystemFontCollection(fc.put())) && fc) { UINT32 i = 0; BOOL has = FALSE; if (SUCCEEDED(fc->FindFamilyName(L"Segoe UI Variable Text", &i, &has)) && has) fam_ = L"Segoe UI Variable Text"; }
    return fam_;
  }
  Font font(Gfx& g, float size, int weight = 400) { Font f; f.family = fam(g); f.size = size; f.weight = weight; return f; }

  // ---- text: one DirectWrite layout, ranges styled (bold, code, links, colours), kept while the width holds
  std::shared_ptr<TextBlock> text(Gfx& g, const std::wstring& str, const std::vector<Span>& spans, const Font& f, float lh, float maxW, bool wrap, bool ellipsis = false, float spacing = 0) {
    std::wstring key = str; key += L'\x1'; key += f.family; key += L'\x1';
    wchar_t num[96]; swprintf(num, 96, L"%g|%d|%g|%g|%d%d|%g", f.size, f.weight, lh, maxW, wrap ? 1 : 0, ellipsis ? 1 : 0, spacing); key += num;
    for (auto& sp : spans) { swprintf(num, 96, L"|%u,%u,%d%d,%x,%d,%g,%d", sp.at, sp.len, sp.bold, sp.code, sp.color, sp.hasColor, sp.size, sp.weight); key += num; key += widen(sp.href); }
    auto it = layCache_.find(key); if (it != layCache_.end()) return it->second;
    auto t = std::make_shared<TextBlock>();
    t->lay = g.layout(str, f, wrap || ellipsis ? std::max(1.f, maxW) : 100000.f, 100000.f, wrap, ellipsis, 0, lh);
    if (!t->lay) { layCache_[key] = t; return t; }
    if (wrap) t->lay->SetWordWrapping((DWRITE_WORD_WRAPPING)2 /*EMERGENCY_BREAK: overflow-wrap anywhere*/);
    if (spacing > 0) { Com<IDWriteTextLayout1> l1; if (SUCCEEDED(t->lay->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)l1.put())) && l1) l1->SetCharacterSpacing(0, spacing, 0, DWRITE_TEXT_RANGE{ 0, (UINT32)str.size() }); }
    for (auto& sp : spans) {
      if (!sp.len) continue;
      const DWRITE_TEXT_RANGE r{ sp.at, sp.len };
      if (sp.bold) t->lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
      if (sp.weight) t->lay->SetFontWeight((DWRITE_FONT_WEIGHT)sp.weight, r);
      if (sp.size > 0) t->lay->SetFontSize(sp.size, r);
      if (sp.code) { t->lay->SetFontFamilyName(L"Consolas", r); t->lay->SetFontSize(f.size >= 13 ? 12.f : f.size - 0.5f, r); }
      if (!sp.href.empty()) { t->lay->SetUnderline(TRUE, r); t->lay->SetDrawingEffect(brush(g, K_ACCENT), r); }
      else if (sp.hasColor) t->lay->SetDrawingEffect(brush(g, sp.color), r);
    }
    DWRITE_TEXT_METRICS m{}; t->lay->GetMetrics(&m);
    t->w = m.widthIncludingTrailingWhitespace; t->h = lh > 0 ? std::max(lh, lh * (float)m.lineCount) : m.height;
    if (lh <= 0 && m.lineCount == 0) t->h = f.size * 1.3f;
    UINT32 nl = 0; t->lay->GetLineMetrics(nullptr, 0, &nl);
    if (nl) { std::vector<DWRITE_LINE_METRICS> lm(nl); t->lay->GetLineMetrics(lm.data(), nl, &nl); t->base = lm[0].baseline; } else t->base = f.size;
    auto rects = [&](UINT32 at, UINT32 len, std::vector<RectF>& out) {
      UINT32 n = 0; t->lay->HitTestTextRange(at, len, 0, 0, nullptr, 0, &n);
      if (!n) return;
      std::vector<DWRITE_HIT_TEST_METRICS> hm(n);
      if (SUCCEEDED(t->lay->HitTestTextRange(at, len, 0, 0, hm.data(), n, &n))) for (UINT32 i = 0; i < n; i++) out.push_back(RectF(hm[i].left, hm[i].top, hm[i].width, hm[i].height));
    };
    for (auto& sp : spans) {
      if (sp.code) { std::vector<RectF> rs; rects(sp.at, sp.len, rs); for (auto& q : rs) t->codeRects.push_back(RectF(q.x - 2, q.y + 1, q.w + 4, q.h - 2)); }
      if (!sp.href.empty()) { std::vector<RectF> rs; rects(sp.at, sp.len, rs); for (auto& q : rs) t->links.push_back({ q, sp.href }); }
    }
    if (layCache_.size() > 4000) layCache_.clear();
    layCache_[key] = t;
    return t;
  }
  // a line of the model -> the text and its spans
  static void runsOf(const ac::Line& l, std::wstring& out, std::vector<Span>& spans, bool hasColor = false, uint32_t color = 0) {
    for (auto& r : l) {
      const std::wstring w = widen(r.text);
      Span sp; sp.at = (UINT32)out.size(); sp.len = (UINT32)w.size(); sp.bold = r.bold; sp.code = r.code; sp.href = r.href; sp.hasColor = hasColor; sp.color = color;
      out += w; spans.push_back(sp);
    }
  }
  std::shared_ptr<TextBlock> lineText(Gfx& g, const ac::Line& l, const Font& f, float lh, float w, bool wrap = true) { std::wstring s; std::vector<Span> sp; runsOf(l, s, sp); return text(g, s, sp, f, lh, w, wrap); }
  std::shared_ptr<TextBlock> plain(Gfx& g, const std::string& str, const Font& f, float lh, float w, bool wrap, bool ellipsis = false, float spacing = 0) { return text(g, widen(str), {}, f, lh, w, wrap, ellipsis, spacing); }

  // ---- the display list
  void fill(const RectF& r, uint32_t c, float a = 1, int hover = -1, uint32_t hc = 0) { Prim p; p.k = Fill; p.r = r; p.c = c; p.a = a; p.hover = hover; p.hc = hc; p.sec = curSec_; prims_.push_back(p); }
  void stroke(const RectF& r, uint32_t c, float a = 1, float wdt = 1, bool dashed = false, int hover = -1, uint32_t hc = 0) { Prim p; p.k = dashed ? Dashed : Stroke; p.r = r; p.c = c; p.a = a; p.wdt = wdt; p.hover = hover; p.hc = hc; p.sec = curSec_; prims_.push_back(p); }
  void put(const std::shared_ptr<TextBlock>& t, float x, float y, uint32_t c, float a = 1) {
    if (!t || !t->lay) return;
    Prim p; p.k = Text; p.r = RectF(x, y, t->w, t->h); p.t = t; p.tc = c; p.a = a; p.sec = curSec_; prims_.push_back(p);
    for (auto& l : t->links) { Target tg; tg.r = RectF(x + l.first.x, y + l.first.y, l.first.w, l.first.h); tg.sec = curSec_; tg.kind = "link"; tg.c = l.second; tg.tip = widen(l.second); targets_.push_back(tg); }
  }
  int target(const RectF& r, const std::string& kind, const std::string& a, const std::string& b2, const std::string& c, const std::wstring& tip = L"", bool off = false) {
    Target t; t.r = r; t.sec = curSec_; t.kind = kind; t.a = a; t.b = b2; t.c = c; t.tip = tip; t.off = off; targets_.push_back(t); return (int)targets_.size() - 1;
  }
  void drawPrim(Gfx& g, const Prim& p, float ox, float oy, const EditBox* focused, bool blinkOn) {
    const RectF r(ox + p.r.x, oy + p.r.y, p.r.w, p.r.h);
    if (r.b() < area_.y - 2 || r.y > area_.b() + 2) return;
    const uint32_t c = p.hover >= 0 && p.hover == hover_ ? p.hc : p.c;
    switch (p.k) {
      case Fill: g.fill(r, Rgba::hex(c, p.a)); break;
      case Stroke: g.stroke(r, Rgba::hex(c, p.a), p.wdt); break;
      case Dashed: {
        const Rgba col = Rgba::hex(c, p.a);
        for (float x = r.x; x < r.r(); x += 6) { g.fill(RectF(x, r.y, std::min(3.f, r.r() - x), 1), col); g.fill(RectF(x, r.b() - 1, std::min(3.f, r.r() - x), 1), col); }
        for (float y = r.y; y < r.b(); y += 6) { g.fill(RectF(r.x, y, 1, std::min(3.f, r.b() - y)), col); g.fill(RectF(r.r() - 1, y, 1, std::min(3.f, r.b() - y)), col); }
        break;
      }
      case Text: {
        for (auto& q : p.t->codeRects) g.fill(RectF(r.x + q.x, r.y + q.y, q.w, q.h), Rgba::hex(K_BOX, p.a));
        ID2D1SolidColorBrush* br = g.b(Rgba::hex(p.tc, p.a));
        if (p.a < 1) for (auto& kv : brushes_) if (kv.second) kv.second->SetOpacity(p.a);
        g.rt->DrawTextLayout(D2D1::Point2F(r.x, r.y), p.t->lay.p, br, (D2D1_DRAW_TEXT_OPTIONS)4);
        if (p.a < 1) for (auto& kv : brushes_) if (kv.second) kv.second->SetOpacity(1);
        break;
      }
      case Box: if (p.box) p.box->draw(g, r, font(g, 13), Rgba::hex(K_FG, p.a), Rgba::hex(K_FAINT), p.box == focused, blinkOn); break;
    }
  }

  // ---- layout: the whole scroller, top to bottom (contents in scroller coordinates, x from the area's left)
  void build(Gfx& g) {
    prims_.clear(); targets_.clear(); secs_.clear(); order_ = json::array(); curSec_ = -1;
    const float W = std::max(80.f, area_.w);
    builtW_ = area_.w;
    float y = 0;
    const json& M = model_;
    std::vector<const json*> sections;
    if (M.is_object() && M.contains("sections") && M["sections"].is_array()) for (auto& s0 : M["sections"]) if (!b(s0, "hidden")) sections.push_back(&s0);
    std::stable_sort(sections.begin(), sections.end(), [](const json* a, const json* b2) { return a->value("order", 0) < b2->value("order", 0); });
    std::map<std::string, float> secTop;
    for (const json* sp : sections) {
      const json& S = *sp;
      Sec sec; sec.top = y; secTop[s(S, "sid")] = y;
      secs_.push_back(sec); curSec_ = (int)secs_.size() - 1;
      const float headH = sessionHead(g, S, y, W);
      curSec_ = -1;
      secs_.back().headH = headH;
      y += headH;
      std::vector<const json*> rounds;
      if (S.contains("rounds") && S["rounds"].is_array()) for (auto& r : S["rounds"]) rounds.push_back(&r);
      std::stable_sort(rounds.begin(), rounds.end(), [](const json* a, const json* b2) { return a->value("order", 0) < b2->value("order", 0); });
      for (const json* rp : rounds) {
        const json& R = *rp;
        if (!b(R, "frame")) continue;
        const std::string note = s(R, "note");
        if (!note.empty()) { auto t = plain(g, note, font(g, 11.5f), 16.1f, W - 16, true); y += 6; put(t, 8, y, K_DIM); y += t->h; }
        y = pageBlock(g, "ask:" + s(R, "id"), 8, y, W - 16);
      }
      y += 6;
      secs_.back().bottom = y;
      fill(RectF(0, y, W, 1), K_LINE); y += 1;
    }
    // the recently answered rows
    const json R = M.is_object() && M.contains("recent") ? M["recent"] : json();
    if (b(R, "show")) {
      Sec sec; sec.top = y; secTop[std::string("__recent")] = y; secs_.push_back(sec);
      float headH = 0;
      if (!b(R, "headHidden")) {
        curSec_ = (int)secs_.size() - 1;
        fill(RectF(0, y, W, 0), K_HEAD);
        const size_t at = prims_.size() - 1;
        auto nm = plain(g, "Recently answered", font(g, 13.5f, 600), 18.9f, W, false);
        const float mx = 2 + 8 + nm->w + 6;
        auto meta = plain(g, s(R, "meta"), font(g, 11.5f), 18.9f, std::max(10.f, W - mx - 8), false, true);
        put(nm, 10, y + 4, K_FG); put(meta, mx, y + 4 + (nm->base - meta->base), K_FAINT);
        headH = 4 + 19 + 5 + 1;
        prims_[at].r.h = headH;
        fill(RectF(0, y, 2, headH), K_FAINT);
        fill(RectF(0, y + headH - 1, W, 1), K_LINE);
        curSec_ = -1;
      }
      secs_.back().headH = headH;
      y += headH + 4;
      std::vector<const json*> rows;
      if (R.contains("rows") && R["rows"].is_array()) for (auto& r : R["rows"]) rows.push_back(&r);
      std::stable_sort(rows.begin(), rows.end(), [](const json* a, const json* b2) { return a->value("order", 0) < b2->value("order", 0); });
      for (size_t i = 0; i < rows.size(); i++) {
        const json& E = *rows[i];
        if (i) y += 2;
        const float top = y, x = 8, w = W - 16;
        const size_t bgAt = prims_.size(); fill(RectF(x, top, w, 0), K_LROW);
        // the head: chevron, abbr, name, meta (a click opens / closes it)
        const bool open = b(E, "open");
        auto ch = plain(g, open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", font(g, 10), 18.9f, 20, false);
        auto ab = plain(g, s(E, "abbr"), font(g, 13, 600), 18.9f, w, false);
        auto nm = plain(g, s(E, "name"), font(g, 13.5f, 600), 18.9f, std::max(20.f, (w - 12) * 0.5f), false, true);
        float cx = x + 1 + 6;
        const int ht = target(RectF(x + 1, top + 1, w - 2, 3 + 19 + 3), "lhead", "", "", s(E, "id"));
        fill(RectF(x + 1, top + 1, w - 2, 3 + 19 + 3), K_LROW, 1, ht, K_LROW_HOVER);
        const float base = top + 1 + 3 + nm->base;
        put(ch, cx, base - ch->base, K_FAINT); cx += ch->w + 6;
        if (!s(E, "abbr").empty()) { put(ab, cx, base - ab->base, K_DIM); cx += ab->w + 6; }
        put(nm, cx, base - nm->base, K_FG); cx += nm->w + 6;
        auto meta = plain(g, s(E, "meta"), font(g, 11.5f), 18.9f, std::max(10.f, x + w - 7 - cx), false, true);
        put(meta, cx, base - meta->base, K_FAINT);
        y = top + 1 + 3 + 19 + 3;
        if (open) {
          if (b(E, "frame")) y = pageBlock(g, "log:" + s(E, "id"), x + 1 + 6, y, w - 2 - 12);
          if (E.contains("lines") && E["lines"].is_string()) {
            y += 4;
            auto t = plain(g, E["lines"].get<std::string>(), Font{ L"Consolas", 12, 400, false }, 16.8f, w - 2 - 12 - 14, true);
            fill(RectF(x + 7, y, w - 14, t->h + 10), K_BG); stroke(RectF(x + 7, y, w - 14, t->h + 10), K_LINE);
            put(t, x + 7 + 7, y + 5, K_FG); y += t->h + 10;
          }
          y += 6;
        }
        y += 1;
        prims_[bgAt].r.h = y - top;
        stroke(RectF(x, top, w, y - top), K_LINE);
      }
      y += 6;
      secs_.back().bottom = y;
      fill(RectF(0, y, W, 1), K_LINE); y += 1;
    }
    if (M.is_object() && M.contains("empty") && M["empty"].is_string()) {
      auto t = plain(g, M["empty"].get<std::string>(), font(g, 13), 18.2f, std::max(40.f, W - 32), true);
      put(t, 16, y + 16, K_DIM); y += 16 + t->h + 16;
    }
    contentH_ = y;
    dirty_ = false;
    // the place asked for (op scroll), kept while the pages above it come in
    if (scrollUntil_ && GetTickCount64() < scrollUntil_) { auto it = secTop.find(scrollTarget_); scroll_ = it != secTop.end() ? it->second : 0; }
    clampScroll();
    if (hover_ >= (int)targets_.size()) hover_ = -1;
  }

  // a button as the host document's head draws it (css .ap-head button): 22 high, 12px, 10 each side
  float headButton(Gfx& g, float x, float y, const std::string& label, const std::string& what, const std::string& sid, const std::wstring& tip, bool off, int style /*0 plain, 1 primary, 2 danger, 3 the quiet Discard*/) {
    auto t = plain(g, label, font(g, 12, style == 1 ? 600 : 400), 20, 1000, false);
    const float w = t->w + 20 + 2, h = 22;
    const uint32_t bg = style == 1 ? K_ACCENT : style == 2 ? K_DANGER : style == 3 ? K_HEAD : K_BTN, line = style == 1 ? K_ACCENT : style == 2 ? K_DANGER : style == 3 ? K_HEAD : K_HBTN_LINE;
    const uint32_t ink = style == 1 ? K_ACCENT_FG : style == 2 ? 0xffffff : style == 3 ? K_FAINT : K_FG;
    const float a = off ? 0.4f : 1;
    const int ti = target(RectF(x, y, w, h), "ui", what, sid, "", tip, off);
    fill(RectF(x, y, w, h), mixc(K_HEAD, bg, a), 1, off ? -1 : ti, style == 3 ? K_HEAD : bright(bg, 1.25f));
    stroke(RectF(x, y, w, h), mixc(K_HEAD, line, a), 1, 1, false, off ? -1 : ti, style == 3 ? 0x3a2424 : bright(line, 1.25f));
    put(t, x + 11, y + (h - t->h) / 2, mixc(K_HEAD, ink, a));
    return w;
  }
  // .ap-head: the session's name line and its buttons (or the discard confirm). Returns its height.
  float sessionHead(Gfx& g, const json& S, float y, float W) {
    const size_t bgAt = prims_.size();
    fill(RectF(0, y, W, 0), K_HEAD);
    float x = 2 + 8;
    const float rowTop = y + 4;
    auto nm0 = plain(g, s(S, "name"), font(g, 13.5f, 600), 18.9f, 10000, false);
    const float base = rowTop + nm0->base;
    if (S.contains("mark") && S["mark"].is_object()) {                       // .t-ask: "?3", filled when drafted, dim when sent
      const json& mk = S["mark"]; const std::string kind = s(mk, "kind");
      const std::string txt = (kind == "sent" ? "" : "?") + s(mk, "text");
      auto t = plain(g, txt, font(g, 10, 700), 11, 200, false);
      const float w = std::max(15.f, t->w + 6 + 2), h = 13, my = rowTop + (19 - h) / 2;
      const uint32_t col = kind == "sent" ? 0x4a6c99 : K_ACCENT;
      if (kind == "drafted") fill(RectF(x, my, w, h), K_ACCENT);
      stroke(RectF(x, my, w, h), col);
      put(t, x + (w - t->w) / 2, my + (h - t->h) / 2, kind == "drafted" ? K_ACCENT_FG : col);
      target(RectF(x, my, w, h), "mark", "", "", "", widen(s(mk, "title")));
      x += w + 6;
    }
    if (!s(S, "abbr").empty()) { auto ab = plain(g, s(S, "abbr"), font(g, 13, 600), 18.9f, 400, false); put(ab, x, base - ab->base, K_DIM); x += ab->w + 6; }
    const float avail = std::max(20.f, W - 8 - x);
    auto nm = plain(g, s(S, "name"), font(g, 13.5f, 600), 18.9f, std::min(nm0->w + 1, std::max(40.f, avail * 0.6f)), false, true);
    put(nm, x, base - nm->base, K_FG); x += nm->w + 6;
    auto meta = plain(g, s(S, "meta"), font(g, 11.5f), 18.9f, std::max(10.f, W - 8 - x), false, true);
    put(meta, x, base - meta->base, K_FAINT);
    float ry = rowTop + 19 + 4, rowH = 22;
    x = 2 + 8;
    if (S.contains("confirm") && S["confirm"].is_object()) {                 // .ap-confirm: the question, Keep them, Discard
      const json& c = S["confirm"];
      auto t = plain(g, s(c, "text"), font(g, 12), 16.8f, std::max(60.f, W - 10 - 8 - 200), true);
      put(t, x, ry + std::max(0.f, (22 - t->h) / 2), K_FG); x += t->w + 8 + 4;
      rowH = std::max(22.f, t->h);
      x += headButton(g, x, ry, "Keep them", "keep", s(S, "sid"), L"", false, 1) + 4;
      headButton(g, x, ry, "Discard", "discard-yes", s(S, "sid"), L"", !b(c, "armed"), 2);
    } else {
      const json done = S.contains("done") ? S["done"] : json::object(), term = S.contains("term") ? S["term"] : json::object();
      x += headButton(g, x, ry, s(done, "text").empty() ? "Done" : s(done, "text"), "done", s(S, "sid"), widen(s(done, "title")), b(done, "disabled"), 1) + 4;
      x += headButton(g, x, ry, "Copy all", "copy", s(S, "sid"), L"", false, 0) + 4;
      x += headButton(g, x, ry, s(term, "text"), "term", s(S, "sid"), widen(s(term, "title")), false, 0) + 4;
      auto cnt = plain(g, s(S, "count"), font(g, 12), 16.8f, 1000, false);
      put(cnt, x + 6, ry + (22 - cnt->h) / 2, K_DIM); x += 6 + cnt->w + 4;
      auto dt = plain(g, "Discard answers\xE2\x80\xA6", font(g, 12), 20, 1000, false);
      const float dw = dt->w + 22, dx = W - 8 - dw;
      const std::string fl = s(S, "flash");
      if (!fl.empty()) {
        const float fw = dx - 4 - x;
        if (fw >= 120) { auto f = plain(g, fl, font(g, 12), 16.8f, fw, true); put(f, x, ry + std::max(0.f, (22 - f->h) / 2), K_GREEN); rowH = std::max(22.f, f->h); }
        else { auto f = plain(g, fl, font(g, 12), 16.8f, W - 20, true); put(f, 10, ry + 22 + 4, K_GREEN); rowH = 22 + 4 + f->h; }   // (flex-wrap: on a line of its own)
      }
      headButton(g, std::max(x, dx), ry, "Discard answers\xE2\x80\xA6", "discard", s(S, "sid"), L"Discard the answers in progress", b(S, "discardDisabled"), 3);
    }
    const float h = 4 + 19 + 4 + rowH + 5 + 1;
    prims_[bgAt].r.h = h;
    fill(RectF(0, y, 2, h), K_ACCENT);
    fill(RectF(0, y + h - 1, W, 1), K_LINE);
    return h;
  }

  // ---- a page, as page.html draws itself embedded (edge to edge, its full height)
  float h2(Gfx& g, const std::string& t, float x, float y, float w, float top) {
    std::string up; for (char c : t) up += (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    auto tb = plain(g, up, font(g, 10.5f, 600), 14.7f, w, true, false, 0.63f);
    y += top; put(tb, x, y, K_DIM); return y + tb->h + 4;
  }
  float bullets(Gfx& g, const std::vector<ac::Line>& ls, float x, float y, float w, uint32_t col = K_FG, float pad = 16) {
    for (auto& l : ls) {
      y += 1;
      auto dot = plain(g, "\xE2\x80\xA2", font(g, 13), 18.2f, 20, false);
      auto t = lineText(g, l, font(g, 13), 18.2f, w - pad);
      put(dot, x + pad - 10, y, col); put(t, x + pad, y, col);
      y += t->h + 1;
    }
    return y;
  }
  float pageBlock(Gfx& g, const std::string& key, float x, float y, float w) {
    order_.push_back(key);
    auto it = pages_.find(key);
    if (it == pages_.end() || it->second.st == Loading) { auto t = plain(g, "Loading the page\xE2\x80\xA6", font(g, 12), 16.8f, w, true); put(t, x, y + 6, K_FAINT); return y + 6 + t->h + 6; }
    NPage& p = it->second;
    if (p.st == Failed) { auto t = plain(g, "The page did not load: " + p.err + ". Answer in the session's terminal.", font(g, 12), 16.8f, w, true); put(t, x, y + 6, K_DIM); return y + 6 + t->h + 6; }
    if (p.st == Legacy) {
      auto t = plain(g, "An older ask-page skill made this page, and the native pane shows only pages made since the skill's host bridge. Answer in the session's terminal, or set askPane to \"web\" in config.json for its own page.", font(g, 12), 16.8f, w, true);
      put(t, x, y + 6, K_DIM); return y + 6 + t->h + 6;
    }
    const ac::Page& P = *p.page; const ac::State& St = *p.state;
    const bool narrow = w < 760, locked = St.locked();
    if (!P.replaced.empty()) {                                                 // .replaced
      y += 8;
      auto t = lineText(g, P.replaced, font(g, 13), 18.2f, w - 20 - 2);
      fill(RectF(x, y, w, t->h + 12 + 2), K_BOX); stroke(RectF(x, y, w, t->h + 14), K_ACCENT);
      put(t, x + 11, y + 7, K_FG); y += t->h + 14 + 4;
    }
    if (!P.summary.empty()) { y = h2(g, "Summary", x, y, w, 4); y = bullets(g, P.summary, x, y, w); }
    y = h2(g, "Questions and decisions", x, y, w, 4);
    bool sub = false;
    for (size_t i = 0; i < P.cards.size(); i++) {
      const ac::Card& c = P.cards[i];
      if (c.carried && !sub) { sub = true; y = h2(g, "Still open from earlier rounds", x, y, w, 12 - 4); }
      const float cx = c.groupNext ? x + 18 : x, cw = c.groupNext ? w - 18 : w;
      y = card(g, p, c, cx, y, cw, narrow, locked);
      y += c.grouped && !c.groupLast ? 2 : 4;
    }
    if (!P.fyi.empty()) { y = h2(g, "FYI, nothing needed", x, y, w, 4); y = bullets(g, P.fyi, x, y, w); }
    return y + 2;
  }
  float blocks(Gfx& g, const std::vector<ac::Block>& bs, float x, float y, float w) {
    float pending = 3; bool first = true;                  // (.context's own margin-top 3 collapses with the first block's)
    auto gap = [&](float top) { y += std::max(pending, top); first = false; };
    const Font body = font(g, 13);
    for (auto& bl : bs) {
      switch (bl.kind) {
        case ac::Block::Para: {
          gap(3);
          std::wstring str; std::vector<Span> sp;
          for (size_t i = 0; i < bl.lines.size(); i++) { if (i) str += L" "; runsOf(bl.lines[i], str, sp); }
          auto t = text(g, str, sp, body, 18.2f, w, true); put(t, x, y, K_FG); y += t->h; pending = 3; break;
        }
        case ac::Block::Bullets: gap(3); y = bullets(g, bl.lines, x, y - 1, w) - 1; pending = 3; break;
        case ac::Block::Quote: {
          gap(3);
          std::wstring str; std::vector<Span> sp;
          for (size_t i = 0; i < bl.lines.size(); i++) { if (i) str += L" "; runsOf(bl.lines[i], str, sp); }
          auto t = text(g, str, sp, body, 18.2f, w - 2 - 16, true);
          fill(RectF(x, y, 2, t->h + 2), K_LINE); put(t, x + 2 + 8, y + 1, K_DIM); y += t->h + 2; pending = 3; break;
        }
        case ac::Block::Code: {
          gap(4);
          auto t = plain(g, bl.code, Font{ L"Consolas", 12, 400, false }, 16.8f, w - 2 - 12, true);
          fill(RectF(x, y, w, t->h + 10), K_IN); stroke(RectF(x, y, w, t->h + 10), K_LINE);
          put(t, x + 7, y + 5, K_FG); y += t->h + 10; pending = 4; break;
        }
        case ac::Block::Table: gap(4); y = table(g, bl, x, y, w); pending = 4; break;
      }
    }
    (void)first;
    return y;                                              // (the last block's bottom margin collapses into the next one's top)
  }
  // .context table: natural column widths; too wide for the card, every column gets its share and wraps
  float table(Gfx& g, const ac::Block& bl, float x, float y, float w) {
    const Font f = font(g, 12.5f), fh = font(g, 12.5f, 600);
    size_t cols = bl.head.size(); for (auto& r : bl.rows) cols = std::max(cols, r.size());
    if (!cols) return y;
    std::vector<float> nat(cols, 0);
    auto natW = [&](const ac::Line& l, const Font& ff) { std::wstring s0; std::vector<Span> sp; runsOf(l, s0, sp); return text(g, s0, sp, ff, 17.5f, 100000, false)->w; };
    for (size_t c = 0; c < bl.head.size(); c++) nat[c] = std::max(nat[c], natW(bl.head[c], fh) + 8);
    for (auto& r : bl.rows) for (size_t c = 0; c < r.size(); c++) nat[c] = std::max(nat[c], natW(r[c], f) + 8);
    float sum = 0; for (float v : nat) sum += v;
    std::vector<float> cw = nat;
    if (sum > w) { for (size_t c = 0; c < cols; c++) cw[c] = std::max(40.f, w * nat[c] / sum); }
    float tw = 0; for (float v : cw) tw += v;
    auto row = [&](const std::vector<ac::Line>& cells, const std::vector<bool>* num, bool head) {
      float cx = x, rh = 0; std::vector<std::shared_ptr<TextBlock>> ts;
      for (size_t c = 0; c < cols; c++) {
        const bool isNum = num && c < num->size() && (*num)[c];
        std::shared_ptr<TextBlock> t;
        if (c < cells.size()) { std::wstring s0; std::vector<Span> sp; runsOf(cells[c], s0, sp); t = text(g, s0, sp, head ? fh : f, 17.5f, std::max(10.f, cw[c] - 8), !isNum); }
        ts.push_back(t); if (t) rh = std::max(rh, t->h);
      }
      rh = std::max(rh, 17.5f);
      for (size_t c = 0; c < cols; c++) {
        const bool isNum = num && c < num->size() && (*num)[c];
        if (ts[c]) put(ts[c], isNum ? cx + cw[c] - 8 - ts[c]->w : cx, y + 1, head ? K_DIM : K_FG);
        cx += cw[c];
      }
      y += 1 + rh + 1;
      fill(RectF(x, y - 1, tw, 1), K_LINE);
    };
    if (!bl.head.empty()) row(bl.head, nullptr, true);
    for (size_t r = 0; r < bl.rows.size(); r++) row(bl.rows[r], r < bl.num.size() ? &bl.num[r] : nullptr, false);
    return y;
  }
  float card(Gfx& g, NPage& p, const ac::Card& c, float x, float y, float w, bool narrow, bool locked) {
    const ac::State& St = *p.state;
    const std::string status = St.status(c.qid);
    const bool answered = status == "answered" || status == "dropped", later = status == "later";
    const float top = y;
    const size_t bgAt = prims_.size();
    fill(RectF(x, top, w, 0), later ? K_LATER : K_TILE);
    const float ix = x + 2 + 8, iw = std::max(40.f, w - 2 - 1 - 16);
    y += 1 + 6;
    // .cardhead: qid, title (wraps), chips, the state chip; one baseline
    {
      auto q = plain(g, c.qid, font(g, 13, 700), 18.2f, 200, false);
      std::vector<std::shared_ptr<TextBlock>> chips;
      for (auto& ch : c.chips) chips.push_back(plain(g, ch, font(g, 10.5f), 14.7f, 400, false));
      auto stc = plain(g, St.stateLabel(c.qid), font(g, 10.5f), 14.7f, 200, false);
      float right = 0; for (auto& t : chips) right += t->w + 8; right += stc->w;
      const float tw = std::max(60.f, iw - q->w - 8 - right - 8);
      auto ti = plain(g, c.title, font(g, 13.5f, 600), 18.9f, tw, true);
      const float base = std::max(q->base, ti->base);
      put(q, ix, y + base - q->base, K_ACCENT);
      put(ti, ix + q->w + 8, y + base - ti->base, K_FG);
      float cx = ix + q->w + 8 + ti->w + 8;
      cx = std::max(cx, ix + iw - right);
      for (auto& t : chips) { put(t, cx, y + base - t->base, K_DIM); cx += t->w + 8; }
      put(stc, cx, y + base - stc->base, answered ? K_GREEN : K_DIM);
      y += std::max(ti->h, q->h);
    }
    if (!c.context.empty()) y = blocks(g, c.context, ix, y, iw);
    // .options
    if (!c.options.empty()) {
      y += 5;
      for (size_t i = 0; i < c.options.size(); i++) {
        const ac::Option& o = c.options[i];
        if (i) y += 2;
        const bool on = St.checked(c.qid, o.key);
        std::wstring str; std::vector<Span> sp;
        runsOf(o.label, str, sp); for (auto& x0 : sp) if (!x0.bold) x0.weight = 600;     // .olabel 600
        if (!o.cons.empty()) { const UINT32 at = (UINT32)str.size(); str += L" - "; Span d; d.at = at; d.len = 3; d.hasColor = true; d.color = K_DIM; sp.push_back(d); runsOf(o.cons, str, sp, true, K_DIM); }
        auto k = plain(g, o.key, font(g, 13, 700), 17.55f, 100, false);
        const float left = o.rec ? 5 + 2 : 1 + 6;
        auto t = text(g, str, sp, font(g, 13), 17.55f, std::max(20.f, iw - left - 6 - 1 - k->w - 8), true);
        const float h = std::max(narrow ? 30.f : 22.f, t->h + 4 + 2);
        const float a = locked && !on ? 0.45f : 1;
        const int ti = target(RectF(ix, y, iw, h), "pick", key(p), c.qid, o.key, o.rec ? L"Recommended" : L"", locked);
        const uint32_t bg = on && !o.rec ? K_SEL_BG : K_BOX;
        fill(RectF(ix, y, iw, h), mixc(K_TILE, bg, a), 1, locked ? -1 : ti, bright(bg, 1.18f));
        if (on) { const uint32_t bc = o.rec ? K_GREEN : K_ACCENT; stroke(RectF(ix, y, iw, h), bc, 1, 2); }
        if (o.rec) fill(RectF(ix, y + (on ? 2 : 0), 5, h - (on ? 4 : 0)), on ? K_GREEN : mixc(K_TILE, K_REC_EDGE, a));
        const float ty = y + std::max(3.f, (h - t->h) / 2);
        put(k, ix + left, ty + (t->base - k->base), on ? K_FG : K_DIM, a);
        put(t, ix + left + k->w + 8, ty, K_FG, a);
        y += h;
      }
    }
    // .replies: one row of equal buttons (5 a row on a narrow page), the page's order
    if (!c.replies.empty()) {
      y += 4;
      const int per = narrow ? 5 : 10; const float gapx = 2, bh = narrow ? 26.f : 20.f;
      const float bw = (iw - gapx * (per - 1)) / per;
      for (size_t i = 0; i < c.replies.size(); i++) {
        const ac::Reply& r = c.replies[i];
        if (i && i % per == 0) y += bh + 2;
        const float bx = ix + (i % per) * (bw + gapx);
        const bool on = St.checked(c.qid, r.pick), off = r.off || locked;
        const float a = locked ? (on ? 1.f : 0.45f) : (r.off ? 0.35f : 1.f);
        const int ti = target(RectF(bx, y, bw, bh), "pick", key(p), c.qid, r.pick, widen(r.tip), off);
        const uint32_t bg = on ? K_SEL_BG : K_BTN, line = on ? K_ACCENT : K_BTN_LINE;
        fill(RectF(bx, y, bw, bh), mixc(K_TILE, bg, a), 1, off ? -1 : ti, bright(bg, 1.18f));
        if (r.none && !on) stroke(RectF(bx, y, bw, bh), mixc(K_TILE, line, a), 1, 1, true);
        else stroke(RectF(bx, y, bw, bh), mixc(K_TILE, line, a), 1, on ? 2.f : 1.f);
        auto t = plain(g, r.label, font(g, 11.5f), 16, std::max(4.f, bw - 4), false);
        put(t, bx + std::max(2.f, (bw - t->w) / 2), y + (bh - t->h) / 2, on ? K_FG : K_DIM, a);
      }
      y += bh;
    }
    // .question (+ "pick any")
    {
      y += 6;
      std::wstring str; std::vector<Span> sp;
      runsOf(c.question, str, sp);
      if (c.pickAny) { const UINT32 at = (UINT32)str.size(); str += L" pick any"; Span ch; ch.at = at; ch.len = 9; ch.size = 10.5f; ch.weight = 400; ch.hasColor = true; ch.color = K_DIM; sp.push_back(ch); }
      auto t = text(g, str, sp, font(g, 13, 600), 18.2f, iw, true);
      put(t, ix, y, K_FG); y += t->h + 3;
    }
    // the answer box: as tall as its text (page.html fit), 40 at least
    {
      EditBox* e = ensureBox(p, c.qid);
      const std::wstring txt = e->text;
      float th = 18.2f;
      if (!txt.empty()) { auto t = text(g, txt + (txt.back() == L'\n' ? L" " : L""), {}, font(g, 13), 18.2f, std::max(10.f, iw - 2 - 12), true); th = t->h; }
      const float h = std::max(40.f, th + 6 + 2);
      const uint32_t bg = locked ? K_BOX : answered ? K_ANS : K_IN;
      fill(RectF(ix, y, iw, h), bg); stroke(RectF(ix, y, iw, h), K_BTN_LINE);
      Prim pb; pb.k = Box; pb.r = RectF(ix + 1 + 6, y + 1 + 3, iw - 2 - 12, h - 2 - 6); pb.box = e; pb.sec = curSec_; pb.c = 0; prims_.push_back(pb);
      target(RectF(ix, y, iw, h), "box", key(p), c.qid, "");
      y += h;
    }
    // .tools: Copy and its note
    {
      y += 3;
      auto t = plain(g, "Copy", font(g, 11.5f), 18, 200, false);
      const float bh = narrow ? 26.f : 20.f, bw = t->w + 16 + 2;
      const int ti = target(RectF(ix, y, bw, bh), "copy", key(p), c.qid, "");
      fill(RectF(ix, y, bw, bh), K_BTN, 1, ti, bright(K_BTN, 1.18f)); stroke(RectF(ix, y, bw, bh), K_BTN_LINE);
      put(t, ix + 9, y + (bh - t->h) / 2, K_FG);
      auto nt = p.notes.find(c.qid);
      if (nt != p.notes.end() && !nt->second.empty()) { auto n = plain(g, nt->second, font(g, 11), 15.4f, std::max(20.f, iw - bw - 8), false, true); put(n, ix + bw + 8, y + (bh - n->h) / 2, K_DIM); }
      y += bh;
    }
    y += 7 + 1;
    prims_[bgAt].r.h = y - top;
    stroke(RectF(x, top, w, y - top), K_LINE);
    fill(RectF(x, top, 2, y - top), answered ? K_GREEN : K_LINE);
    return y;
  }
  static std::string key(const NPage& p) { return p.kind + ":" + p.id; }

  // ---- the state and the boxes
  EditBox* ensureBox(NPage& p, const std::string& qid) {
    auto it = p.boxes.find(qid);
    if (it != p.boxes.end()) return it->second.get();
    std::unique_ptr<EditBox> e(new EditBox());
    e->multiline = true; e->lineHeight = 18.2f; e->owner = "askbox";
    e->placeholder = L"Your answer to " + widen(qid);
    if (p.page) { const int ci = p.page->cardOf(qid); if (ci >= 0 && !p.page->cards[ci].placeholder.empty()) e->placeholder = widen(p.page->cards[ci].placeholder); }
    if (p.state) { e->set(widen(p.state->text(qid))); e->readOnly = p.state->locked(); }
    const std::string k = key(p);
    EditBox* raw = e.get();
    e->onChange = [this, k, qid, raw] { typed(k, qid, raw); };
    p.boxes[qid] = std::move(e);
    return raw;
  }
  void syncBoxes(NPage& p) {
    if (!p.page || !p.state) return;
    for (auto& c : p.page->cards) {
      EditBox* e = ensureBox(p, c.qid);
      const std::wstring want = widen(p.state->text(c.qid));
      if (e->text != want) e->set(want);
      e->readOnly = p.state->locked();
    }
  }
  void unfocusIn(NPage& p) {
    EditBox* f = d_.focused ? d_.focused() : nullptr;
    for (auto& b2 : p.boxes) if (b2.second.get() == f && d_.focusEdit) d_.focusEdit(nullptr);
  }
  EditBox* boxOf(const std::string& k, const std::string& qid) { auto it = pages_.find(k); if (it == pages_.end()) return nullptr; auto b2 = it->second.boxes.find(qid); return b2 == it->second.boxes.end() ? nullptr : b2->second.get(); }
  void typed(const std::string& k, const std::string& qid, EditBox* e) {
    auto it = pages_.find(k); if (it == pages_.end() || it->second.st != Ready) return;
    NPage& p = it->second;
    if (!p.state->type(qid, narrow(e->text))) { e->set(widen(p.state->text(qid))); return; }
    userActed();
    dirty(); changeSoon(p);
    if (g_) reveal(e);
  }
  void pick(const std::string& k, const std::string& qid, const std::string& what) {
    auto it = pages_.find(k); if (it == pages_.end() || it->second.st != Ready) return;
    NPage& p = it->second;
    bool focusBox = false;
    if (!p.state->pick(qid, what, &focusBox)) return;
    syncBoxes(p); dirty(); changeSoon(p);
    if (focusBox && d_.focusEdit) { EditBox* e = boxOf(k, qid); if (e) { d_.focusEdit(e); reveal(e); } }   // See below: the note is the answer
  }
  void copyCard(const std::string& k, const std::string& qid) {
    auto it = pages_.find(k); if (it == pages_.end() || it->second.st != Ready) return;
    NPage& p = it->second;
    const std::string line = p.state->line(qid);
    if (d_.copy) d_.copy(line);
    p.notes[qid] = "Copied: " + line; dirty();
    if (d_.after) { const unsigned g = p.gen; d_.after(5000, [this, k, qid, g] { auto j = pages_.find(k); if (j == pages_.end() || j->second.gen != g) return; j->second.notes.erase(qid); dirty(); }); }
  }
  json progress(const NPage& p, bool withState) const {
    const ac::OJ o = p.state->progressJson(withState);
    return json::parse(ac::stringify(o), nullptr, false);
  }
  // persist() -> onChange: the page's `change` to its host, 120 ms after the last edit
  void changeSoon(NPage& p) {
    if (!d_.after) return;
    if (p.changeT && d_.cancel) d_.cancel(p.changeT);
    const std::string k = key(p); const unsigned g = p.gen;
    p.changeT = d_.after(120, [this, k, g] {
      auto it = pages_.find(k); if (it == pages_.end() || it->second.gen != g) return;
      NPage& q = it->second; q.changeT = 0;
      if (q.st != Ready) return;
      json m = progress(q, true); m["askpage"] = "change"; m["id"] = q.id;
      if (d_.page) d_.page(q.kind, q.id, m);
    });
  }
  // Tab / Shift+Tab: the next answer box that takes typing, in the order they are drawn
  void focusStep(int dir, EditBox* from = nullptr) {
    if (!g_) return;
    if (dirty_) build(*g_);
    std::vector<EditBox*> ring;
    for (auto& t : targets_) if (t.kind == "box") { EditBox* e = boxOf(t.a, t.b); if (e && !e->readOnly) ring.push_back(e); }
    if (ring.empty()) return;
    if (!from && d_.focused) { EditBox* f = d_.focused(); if (owns(f)) from = f; }
    int at = -1; for (size_t i = 0; i < ring.size(); i++) if (ring[i] == from) at = (int)i;
    int next = at < 0 ? (dir > 0 ? 0 : (int)ring.size() - 1) : (at + dir + (int)ring.size()) % (int)ring.size();
    if (d_.focusEdit) d_.focusEdit(ring[next]);
    ring[next]->caret = ring[next]->anchor = (int)ring[next]->text.size();
    reveal(ring[next]);
    if (d_.changed) d_.changed();
  }

  // ---- scrolling and the sticky heads
  void clampScroll() { scroll_ = std::max(0.f, std::min(scroll_, std::max(0.f, contentH_ - area_.h))); }
  float stickDy(int i) const {
    if (i < 0 || i >= (int)secs_.size()) return 0;
    const Sec& s0 = secs_[i];
    return std::max(0.f, std::min(scroll_ - s0.top, s0.bottom - s0.headH - s0.top));
  }
  // the height of the sticky head over content y (what a thing scrolled into sight must clear)
  float headAt(float y) const { for (auto& s0 : secs_) if (y >= s0.top && y < s0.bottom + 1) return s0.headH; return 0; }
  int targetAt(float x, float y) const {
    if (!area_.has(x, y)) return -1;
    const float lx = x - area_.x, ly = y - area_.y;
    for (int i = (int)targets_.size() - 1; i >= 0; i--) {                    // the sticky heads first: they are on top
      const Target& t = targets_[i]; if (t.sec < 0) continue;
      const float dy = stickDy(t.sec);
      if (t.r.has(lx, ly + scroll_ - dy)) return i;
    }
    for (size_t i = 0; i < secs_.size(); i++) {                               // under a head: nothing below it takes the click
      const float dy = stickDy((int)i), top = secs_[i].top + dy - scroll_;
      if (secs_[i].headH > 0 && ly >= top && ly < top + secs_[i].headH) return -1;
    }
    for (int i = (int)targets_.size() - 1; i >= 0; i--) { const Target& t = targets_[i]; if (t.sec < 0 && t.r.has(lx, ly + scroll_)) return i; }
    return -1;
  }
};

}}  // namespace deck::asks
