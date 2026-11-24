// windows/src/cloud.hpp — the cloud panes (renderer/js/48-cloud.js), for the native shell.
//
// A cloud tile holds a Claude Code session that runs in Anthropic's cloud. Its pane is the session's own claude.ai
// page, laid over the tile's terminal below the pane's name (css `.term .cloud-pane`). In Electron that is a
// <webview> in the partition `persist:claude-<account>`; here it is one WebPane (WebView2) per cloud pane on show.
//
// WHO DRAWS WHAT
//   * Direct2D (the app's frame) draws the pane as ever: its background, its NAME with the cloud mark, its border.
//     Under the page it paints the css background of `.cloud-pane` (#262624), which is what shows until the page
//     has painted, and the whole body when there is no web view at all (below).
//   * The page is a CHILD WINDOW on the rectangle the css gives `.cloud-pane`: the pane, from
//     `calc(var(--pane-name-size) * 1.2 + 6px)` under its top edge to its bottom. In Electron the pane's 1px border
//     (`.term.grid.placed::after`, z 3) and the 2px attention frame (`.desk-attn::before`, z 5) are drawn OVER the
//     page's edge; a child window cannot be drawn over, so the page is inset by that border instead (1px in a
//     grid, 2px under the attention frame): the lines show exactly as they do there, the page is that much smaller.
//   * The sign-in hint (css `.cloud-hint`, z 4: over the page) is a small child window of its own above the page,
//     painted with Direct2D by the same code that draws it into the frame when the page is a still picture.
//   * Whatever the app draws OVER a pane (the palette, a menu, a panel, the drop preview, a note, the dialog) cannot
//     show over a child window either: while something is over a pane it is `covered` — photographed, hidden, and
//     the photograph drawn in the frame with the overlay over it (the ask pane's way, asks.hpp cover()).
//
// THE PROFILE (the Electron deck's partitions, ported by intent): ONE persistent WebView2 user-data folder for all
// cloud panes, `<state>\web-cloud\`, and inside it one named profile per account, `claude-<email or account id>`:
// sign in once per account, in the first of its panes; it survives restarts. The ask pane runs in the SAME folder
// (one browser process tree for everything, WEB-NOTES.md) but in an InPrivate profile of its own (`deck-asks`,
// in memory only, which holds the deck's own cookie): profiles share no cookies and no storage, so nothing of the
// deck's ever reaches a cloud pane, and nothing of claude.ai's reaches an ask page.
//
// HOW LONG A PAGE LIVES (a web view is a renderer process, and the first one a whole browser process tree): a pane
// that is not on screen (its tile left the layout, a zoom is in front of it, the ask pane is over the area, the
// window is minimised) is hidden and SUSPENDED at once, and CLOSED after CLOUD_OFFSCREEN_CLOSE_MS off screen; when
// the last web view of the process closes, the browser process tree goes with it. Back on screen, the pane is
// built again and loads the session's page afresh (what the Electron pane does on a redraw). A pane ON SCREEN is
// never closed, whatever it is doing and however long nobody has touched it: a half-typed prompt is not lost.
//
// WHAT MAY LOAD (main.js guardWebview / paneUrl / signInUrl, the same lists): a pane is only made for an
// https://claude.ai/ address. By itself the top document may go to claude.ai, claude.com, anthropic.com and the
// sign-in pages a login passes through; any other address is refused there and opens in the default browser. A new
// window is only ever a sign-in page (the runtime's own popup, same profile); any other opens in the default browser.
//
// TESTS never load claude.ai and never open a browser: a test run makes a web view only when it is given a local
// stand-in (`--cloud-base http://127.0.0.1:<port>`, loopback only), every address under https://claude.ai is
// mapped onto it, anything that is not that stand-in is refused and logged, and the rules above are evaluated on
// the address as it would have been (so the rules themselves are what is tested).
#pragma once
#include "gfx.hpp"
#include "ui.hpp"
#include "../web/webpane.hpp"
#include <d2d1.h>
#include <shlobj.h>
#include <shlwapi.h>
#include "../../core/third_party/nlohmann/json.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace deck { namespace cloud {

using json = nlohmann::json;

// ---- main.js hostOf / under / signInUrl / paneUrl, and 48-cloud.js cloudSuffix: pure (unit-tested in web-test)
inline std::string hostOf(const std::string& url) {      // an https address's host, lower case; '' for anything else
  if (url.size() < 9) return "";
  std::string low = url.substr(0, 8); for (auto& c : low) c = (char)tolower((unsigned char)c);
  if (low != "https://") return "";
  size_t end = url.find_first_of("/?#", 8); if (end == std::string::npos) end = url.size();
  std::string h = url.substr(8, end - 8);
  const size_t at = h.rfind('@'); if (at != std::string::npos) h = h.substr(at + 1);
  if (!h.empty() && h[0] == '[') return "";               // an IPv6 literal is none of the names below
  const size_t colon = h.find(':'); if (colon != std::string::npos) h = h.substr(0, colon);
  for (auto& c : h) c = (char)tolower((unsigned char)c);
  while (!h.empty() && h.back() == '.') h.pop_back();
  for (char c : h) if (!(isalnum((unsigned char)c) || c == '-' || c == '.')) return "";
  return h;
}
inline bool under(const std::string& h, const std::string& d) { return h == d || (h.size() > d.size() && h.compare(h.size() - d.size(), d.size(), d) == 0 && h[h.size() - d.size() - 1] == '.'); }
inline bool signInUrl(const std::string& url) {
  const std::string h = hostOf(url); if (h.empty()) return false;
  for (const char* d : { "accounts.google.com", "appleid.apple.com", "login.microsoftonline.com", "login.live.com", "okta.com", "auth0.com" }) if (under(h, d)) return true;
  return false;
}
inline bool paneUrl(const std::string& url) {
  const std::string h = hostOf(url); if (h.empty()) return false;
  for (const char* d : { "claude.ai", "claude.com", "anthropic.com" }) if (under(h, d)) return true;
  return signInUrl(url);
}
// main.js guardWebview: only https://claude.ai/ may be a pane's page
inline bool pageUrlOk(const std::string& url) { return url.rfind("https://claude.ai/", 0) == 0; }
// 48-cloud.js onNav: the page is a sign-in page, or has left claude.ai: the hint shows
inline bool signInPage(const std::string& url) {
  for (const char* p : { "https://claude.ai/login", "https://claude.ai/signup", "https://claude.ai/magic-link" }) if (url.rfind(p, 0) == 0) return true;
  if (url.rfind("https://", 0) != 0) return true;
  size_t end = url.find('/', 8); if (end == std::string::npos) return true;     // the js wants the slash after the host
  const std::string h = url.substr(8, end - 8);
  for (char c : h) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return true;
  return !under(h, "claude.ai");
}
inline std::string cloudSuffix(const std::string& id) {
  for (const char* p : { "session_", "cse_" }) if (id.rfind(p, 0) == 0) return id.substr(strlen(p));
  return id;
}
// the profile of an account: `claude-` + its email (else its id), cut to what WebView2 takes as a profile name
inline std::wstring profileName(const std::string& emailOrId) {
  std::wstring n = L"claude-";
  for (unsigned char c : emailOrId) {
    if (n.size() >= 64) break;
    const bool ok = isalnum(c) || c == '#' || c == '@' || c == '$' || c == '(' || c == ')' || c == '+' || c == '-' || c == '_' || c == '~' || c == '.' || c == '=';
    n.push_back(ok ? (wchar_t)tolower(c) : L'_');
  }
  while (!n.empty() && (n.back() == L'.' || n.back() == L' ')) n.pop_back();
  return n;
}
// 48-cloud.js onNav: what the hint says
inline std::string hintText(const std::string& email, const std::string& label) {
  return "Sign in as " + email + (!label.empty() && label != email ? " (" + label + ")" : "") + ". Once is enough: every cloud pane of this account shares the login.";
}

constexpr unsigned CLOUD_OFFSCREEN_CLOSE_MS = 2 * 60000;  // a page off screen this long is closed (a web view is a process); built again, and its page loaded again, when shown

struct Deps {
  std::function<void(const std::string& tileId, bool byUser)> focused;   // the page took the keyboard (byUser: a click in it, not our own hand-off)
  std::function<void()> changed;                                          // repaint
  std::function<bool(unsigned vk, bool ctrl, bool shift, bool alt)> accelerator;   // a key the page does not type with: true = the app took it
  std::function<void(const std::string& url)> openOutside;                // the default browser (the app logs instead in a test)
  std::function<void(const std::string& line)> log;                       // tests
};

// where a pane's parts are (css px = DIPs), from the pane's rectangle
struct Geo {
  // css `.term .cloud-pane`: left 0, right 0, bottom 0, top calc(var(--pane-name-size) * 1.2 + 6px); inset by the
  // border the pane draws over it (see the head of this file)
  static RectF page(const Gfx& g, const RectF& pane, float nameSize, float inset) {
    const float top = g.snap(pane.y + nameSize * 1.2f + 6);
    const float x0 = g.snap(pane.x + inset), x1 = g.snap(pane.r() - inset), y1 = g.snap(pane.b() - inset);
    return RectF(x0, top, std::max(0.f, x1 - x0), std::max(0.f, y1 - top));
  }
};

// One view the app has on show this frame
struct View { std::string id; RectF pane; float inset = 0; bool hidden = false, covered = false; };

class CloudPanes {
 public:
  CloudPanes() : alive_(std::make_shared<bool>(true)) {}
  ~CloudPanes() { *alive_ = false; for (auto& kv : panes_) if (kv.second->hintWnd) DestroyWindow(kv.second->hintWnd); panes_.clear(); }

  // parent = nullptr: no window (a headless --shot run): no web view, the card instead.
  // dir: the persistent user-data folder (`<state>\web-cloud`). test + testBase: see the head of this file.
  void init(HWND parent, const Deps& deps, const std::wstring& dir, bool test, const std::string& testBase) {
    parent_ = parent; deps_ = deps; dir_ = dir; test_ = test; base_ = testBase;
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    if (test_ && !base_.empty() && base_.rfind("http://127.0.0.1:", 0) != 0) base_.clear();    // loopback, or nothing
  }
  web::WebPaneOptions webOptions;                         // tests: a fixed runtime folder (--web-runtime)
  unsigned offscreenMs = CLOUD_OFFSCREEN_CLOSE_MS;        // tests shorten it (--web-grace)
  float rasterScale = 0;                                  // tests (--dpi on a parked window): the page's own scale

  void setMeta(const json& meta) {
    tiles_ = meta.is_object() && meta.contains("tiles") && meta["tiles"].is_array() ? meta["tiles"] : json::array();
    accounts_ = meta.is_object() && meta.contains("accounts") && meta["accounts"].is_array() ? meta["accounts"] : json::array();
    // 48-cloud.js paintCloudPanes: a pane whose tile went, or became another session or another account's, is dropped
    for (auto it = panes_.begin(); it != panes_.end();) {
      const json* t = cloudTile(it->first);
      if (!t || str(*t, "cloudId") != it->second->cloudId || str(*t, "account") != it->second->account) { drop(*it->second); it = panes_.erase(it); } else ++it;
    }
  }
  // the tile is a cloud tile that has its session: its pane is the page, the terminal under it is not shown
  // (css `.term.cloud .xterm { visibility: hidden }`)
  bool isCloud(const std::string& id) const { const json* t = cloudTile(id); return t && !str(*t, "cloudId").empty(); }
  bool has(const std::string& id) const { return panes_.count(id) > 0; }
  size_t count() const { return panes_.size(); }
  // the pane shows the card (the browser desk's `.cloud-card`) instead of a page: no window, no runtime, a test
  // without a stand-in page, or an address that is not claude.ai's
  bool card(const std::string& id) const { auto it = panes_.find(id); return it == panes_.end() ? !canWeb() : it->second->noWeb; }

  // ---- every frame: where each cloud pane on show is. Builds a pane the first time it shows, keeps the page on
  // its rectangle, hides what left the screen, covers what the app draws over, drops what idled away.
  void sync(Gfx& g, const std::vector<View>& views, float nameSize) {
    g_ = &g; nameSize_ = nameSize; scale_ = g.scale;
    const ULONGLONG now = GetTickCount64();
    for (auto& kv : panes_) kv.second->seen = false;
    for (const View& v : views) {
      const json* t = cloudTile(v.id); if (!t || str(*t, "cloudId").empty()) continue;
      auto it = panes_.find(v.id);
      if (it == panes_.end()) { if (v.hidden) continue; it = panes_.emplace(v.id, build(v.id, *t)).first; }
      Pane& p = *it->second;
      p.seen = true; p.pane = v.pane; p.inset = v.inset;
      if (!v.hidden) p.shownAt = now;                      // on screen now (a still picture under an overlay is on screen too)
      const RectF page = Geo::page(g, v.pane, nameSize, v.inset);
      if (p.web && (page.x != p.page.x || page.y != p.page.y || page.w != p.page.w || page.h != p.page.h || scale_ != p.scale)) p.web->setBounds(web::Rect{ page.x, page.y, page.w, page.h }, scale_);
      p.page = page; p.scale = scale_;
      setHidden(p, v.hidden);
      if (!v.hidden) setCovered(p, v.covered);
      placeHint(p);
    }
    for (auto it = panes_.begin(); it != panes_.end();) {
      Pane& p = *it->second;
      if (!p.seen) { setHidden(p, true); placeHint(p); }
      // off screen (not in the layout, or hidden behind the ask pane / a minimised window) for the whole grace: closed
      if ((!p.seen || p.hidden) && now - p.shownAt > offscreenMs) {
        if (deps_.log) deps_.log("cloud pane closed (off screen " + std::to_string((now - p.shownAt) / 1000) + " s): " + p.id);
        drop(p); it = panes_.erase(it);
      } else ++it;
    }
  }
  // a pane is off screen and still has its page: sync() must be called again within tickMs() even if no frame is
  // drawn (a minimised window draws none), so the grace runs out on time (the app keeps a timer while this is true)
  bool anyOffscreen() const { for (auto& kv : panes_) if (!kv.second->seen || kv.second->hidden) return true; return false; }
  unsigned tickMs() const { return std::min(5000u, std::max(200u, offscreenMs / 4)); }
  // tests: an idle pane's clock, wound on
  void testAge(const std::string& id, unsigned ms) { auto it = panes_.find(id); if (it != panes_.end()) it->second->shownAt -= ms; }

  // ---- drawing (the app calls these from its frame)
  // In the pane's own paint (the scene), after its background and before its name: what is under the page.
  void drawBody(Gfx& g, const std::string& id, const RectF& pane, float nameSize, float inset, bool linkHover) {
    const RectF page = Geo::page(g, pane, nameSize, inset);
    g.fill(page, Rgba::hex(0x262624));                    // css .term .cloud-pane { background: #262624 }
    if (!card(id)) return;
    // 48-cloud.js buildCloudPane, the card of a view with no <webview>; css .cloud-card
    g.pushClip(page);
    const std::wstring fam = family(g);
    const float x = page.x + 28; float y = page.y + 24;
    Font fh; fh.family = fam; fh.size = 15; fh.weight = 600;
    const Rgba cloudCol = Rgba::hex(0x8fb8e8);
    ui::drawCloudMark(g, x, y + 1, 18, cloudCol);
    g.text(L"Cloud session", RectF(x + 18 + 6, y, std::max(0.f, page.r() - 28 - (x + 24)), 20), fh, cloudCol, Align::Left, true);
    y += 20 + 10;
    Font ft; ft.family = fam; ft.size = 13.5f;
    const float ch = g.textWidth(L"0", ft);
    const float tw = std::max(40.f, std::min(46 * ch, page.w - 56));
    std::wstring what = L"It runs on claude.ai, not on the deck's machine. Open it there to read it and answer it.";
    y += g.textWrapped(what, RectF(x, y, tw, 400), ft, Rgba::hex(0x9a9a9a), 0, 18.9f) + 10;
    const std::wstring link = L"Open on claude.ai";
    const float lw = std::ceil(g.textWidth(link, ft)) + 22, lh = 18 + 10;
    const RectF lr(x, y, lw, lh);
    if (linkHover) g.fill(lr, Rgba::hex(0x1c2a3a));
    g.stroke(lr, Rgba::hex(0x3e6a9a), 1);
    g.text(link, RectF(lr.x + 11, lr.y + 1, lw - 22 + 2, lh - 2), ft, Rgba::hex(0x9cc7f5), Align::Left, false);
    links_[id] = lr;
    y += lh + 10;
    auto it = panes_.find(id);
    if (it != panes_.end() && !it->second->why.empty()) {  // (an addition: WHY there is no page here)
      Font fw; fw.family = fam; fw.size = 12;
      g.textWrapped(it->second->why, RectF(x, y, tw, 400), fw, Rgba::hex(0x6a6a6a), 0, 16.8f);
    }
    g.popClip();
  }
  // Over the scene: the still pictures of the covered panes, and a hint that has no window of its own just now.
  void drawOver(Gfx& g) {
    for (auto& kv : panes_) {
      Pane& p = *kv.second;
      if (!p.seen || p.hidden) continue;
      if (p.covered && !p.snap.empty() && g.wic && g.rt) {
        if (!p.snapBmp || p.snapRt != g.rt.p) {
          p.snapBmp.reset(); p.snapRt = g.rt.p;
          Com<IStream> st; st.p = SHCreateMemStream((const BYTE*)p.snap.data(), (UINT)p.snap.size());
          Com<IWICBitmapDecoder> dec; Com<IWICBitmapFrameDecode> frame; Com<IWICFormatConverter> conv;
          if (st && SUCCEEDED(g.wic->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnLoad, dec.put())) && dec && SUCCEEDED(dec->GetFrame(0, frame.put()))
              && SUCCEEDED(g.wic->CreateFormatConverter(conv.put())) && SUCCEEDED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
            g.rt->CreateBitmapFromWicBitmap(conv.p, nullptr, p.snapBmp.put());
        }
        if (p.snapBmp) g.rt->DrawBitmap(p.snapBmp.p, D2D1::RectF(p.page.x, p.page.y, p.page.r(), p.page.b()), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
      }
      if (p.hint && !hintWindowUp(p)) { const RectF b = hintBox(g, p); paintHint(g.rt.p, g, b, p.hintText); }
    }
  }
  // the render target is being replaced: the photographs' bitmaps go with it
  void dropDeviceResources() { for (auto& kv : panes_) { kv.second->snapBmp.reset(); kv.second->snapRt = nullptr; } }

  // what the pane's own paint (drawBody) is drawn from, for the scene's key
  std::string sig(const std::string& id) const { if (!isCloud(id)) return "t"; auto it = panes_.find(id); return std::string(card(id) ? "card" : "page") + (it != panes_.end() ? narrow(it->second->why) : std::string()); }
  // the wanted state of every pane is the state it has (the app repaints until it is: a note or a menu came up)
  bool settled(const std::vector<View>& views) const { for (const View& v : views) { auto it = panes_.find(v.id); if (it != panes_.end() && it->second->web && !v.hidden && it->second->covered != v.covered) return false; } return true; }

  // ---- the card's link (a pane with no page)
  bool linkAt(float x, float y, std::string& id) const { for (auto& kv : links_) if (card(kv.first) && kv.second.has(x, y)) { id = kv.first; return true; } return false; }
  void forgetLinks() { links_.clear(); }

  // ---- focus (48-cloud.js focusCloud): the pane's page has the keys instead of the terminal under it
  // asUser (tests): as a click into the page would move it there (a real click cannot be made without the mouse)
  bool focus(const std::string& id, bool asUser = false) {
    auto it = panes_.find(id); if (it == panes_.end() || !it->second->web) return false;
    Pane& p = *it->second;
    if (p.hidden || p.covered || p.focused) return true;
    p.selfFocusAt = asUser ? 0 : GetTickCount64();
    p.web->focus();
    return true;
  }
  // the pane's page can take the keyboard now (its web view is up), or never will (no page here)
  bool ready(const std::string& id) const { auto it = panes_.find(id); return it != panes_.end() && (!it->second->web || it->second->web->state() == web::WebState::Ready); }
  std::string focusedId() const { for (auto& kv : panes_) if (kv.second->focused) return kv.first; return ""; }
  bool webFocused() const { return !focusedId().empty(); }

  // ---- F5, a tap on the pane's name, the tile's `reload` (48-cloud.js cloudRedraw): the page again — back to the
  // session if it wandered. false = no page here (the caller redraws the terminal, as the js does)
  bool redraw(const std::string& id) {
    auto it = panes_.find(id); const json* t = cloudTile(id);
    if (it == panes_.end() || !t || !it->second->web) return false;
    Pane& p = *it->second;
    const std::string at = canon(narrow(p.web->url()));
    const bool same = !at.empty() && at.find(cloudSuffix(str(*t, "cloudId"))) != std::string::npos;
    if (deps_.log) deps_.log("cloud redraw " + id + ": " + (same ? "reload" : "load " + p.url) + " (was at " + at + ")");
    if (same) p.web->reload(); else p.web->navigate(widen(real(p.url)));
    return true;
  }

  // the top window moved: the web views' own popups follow
  void parentMoved() { for (auto& kv : panes_) if (kv.second->web) kv.second->web->notifyParentMoved(); }
  // before the app exits. begin: every web view closes (nothing waits); shutdown: their one browser process is
  // waited for. The profile folder STAYS: it holds the sign-ins.
  void beginShutdown() { for (auto& kv : panes_) { Pane& p = *kv.second; if (p.web) { if (p.web->browserPid()) { pid_ = p.web->browserPid(); pidAt_ = GetTickCount64(); } p.web->destroy(0); } if (p.hintWnd) { DestroyWindow(p.hintWnd); p.hintWnd = nullptr; } } }
  // (the wait is for a browser process a view of ours had until a moment ago: one whose last view closed long ago
  // has gone by itself, and its process id may be another program's by now)
  bool shutdown(unsigned waitMs = 3000) { beginShutdown(); const bool gone = pid_ && GetTickCount64() - pidAt_ < 10000 ? web::WebPane::waitProcessGone(pid_, waitMs) : true; panes_.clear(); return gone; }
  bool anyMade() const { return pid_ != 0 || std::any_of(panes_.begin(), panes_.end(), [](const auto& kv) { return kv.second->web != nullptr; }); }
  size_t webCount() const { size_t n = 0; for (auto& kv : panes_) if (kv.second->web) n++; return n; }

  // ---- tests
  web::WebPane* webOf(const std::string& id) { auto it = panes_.find(id); return it == panes_.end() ? nullptr : it->second->web.get(); }
  std::string dumpState() const {
    std::string o = "cloud panes " + std::to_string(panes_.size()) + " (web views " + std::to_string(webCount()) + "), folder " + narrow(dir_) + ", stand-in '" + base_ + "'\n";
    for (auto& kv : panes_) {
      const Pane& p = *kv.second; char b[240];
      snprintf(b, sizeof b, " page [%.0f,%.0f %.0fx%.0f] hidden %d covered %d still %zu bytes focused %d suspended %d memory target %d", p.page.x, p.page.y, p.page.w, p.page.h, p.hidden, p.covered, p.snap.size(), p.focused, p.web ? (int)p.web->suspended() : 0, p.web ? p.web->memoryTarget() : -1);
      o += "  " + kv.first + " " + p.cloudId + " profile " + narrow(p.profile) + (p.web ? " web state " + std::to_string((int)p.web->state()) + " (2 = ready)" : " NO WEB VIEW (" + narrow(p.why) + ")") + b
        + " url '" + (p.web ? narrow(p.web->url()) : std::string()) + "' hint " + (p.hint ? "'" + narrow(p.hintText) + "'" : std::string("-")) + (p.hintWnd && IsWindowVisible(p.hintWnd) ? " (its own window)" : "") + "\n";
    }
    return o;
  }

 private:
  struct Pane {
    std::string id, cloudId, account, url;               // url: the session's page as the deck names it (https://claude.ai/…)
    std::wstring profile, why;                            // why: there is no page here (the card says)
    std::unique_ptr<web::WebPane> web; bool noWeb = false;
    ULONGLONG shownAt = 0, selfFocusAt = 0;
    RectF pane, page; float inset = 0, scale = 0;
    bool seen = false, hidden = false, covered = false, focused = false, focusBeforeCover = false;
    bool hint = false; std::wstring hintText; HWND hintWnd = nullptr; RECT hintPx = {};
    std::string snap; Com<ID2D1Bitmap> snapBmp; ID2D1RenderTarget* snapRt = nullptr; unsigned snapGen = 0;
  };
  HWND parent_ = nullptr; Deps deps_; std::wstring dir_; bool test_ = false; std::string base_;
  json tiles_ = json::array(), accounts_ = json::array();
  std::map<std::string, std::unique_ptr<Pane>> panes_;
  std::map<std::string, RectF> links_;
  std::shared_ptr<bool> alive_;
  Gfx* g_ = nullptr; float nameSize_ = 13, scale_ = 1;
  unsigned long pid_ = 0; ULONGLONG pidAt_ = 0;             // the browser process of the last view closed, and when
  std::wstring family_; bool hintClassMade_ = false;
  Com<ID2D1DCRenderTarget> hintRt_;

  static std::string str(const json& j, const char* k) { if (!j.is_object()) return ""; auto it = j.find(k); return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(); }
  const json* cloudTile(const std::string& id) const { for (auto& t : tiles_) if (str(t, "id") == id) return str(t, "kind") == "cloud" ? &t : nullptr; return nullptr; }
  const json* acct(const std::string& id) const { for (auto& a : accounts_) if (str(a, "id") == id) return &a; return nullptr; }
  std::string acctEmail(const std::string& id) const { const json* a = acct(id); const std::string e = a ? str(*a, "email") : ""; return e.empty() ? id : e; }   // cloudAcctEmail
  std::string acctLabel(const std::string& id) const { const json* a = acct(id); return a ? str(*a, "label") : id; }
  // a test's stand-in for claude.ai: an address as it really is <-> as it would have been
  std::string real(const std::string& url) const { return !base_.empty() && url.rfind("https://claude.ai", 0) == 0 ? base_ + url.substr(17) : url; }
  std::string canon(const std::string& url) const { return !base_.empty() && url.rfind(base_, 0) == 0 && (url.size() == base_.size() || url[base_.size()] == '/' || url[base_.size()] == '?') ? "https://claude.ai" + url.substr(base_.size()) : url; }
  bool standIn(const std::string& url) const { return !base_.empty() && url.rfind(base_ + "/", 0) == 0; }
  bool canWeb() const { return parent_ && !dir_.empty() && (!test_ || !base_.empty()); }
  const std::wstring& family(Gfx& g) {
    if (!family_.empty()) return family_;
    family_ = L"Segoe UI";
    Com<IDWriteFontCollection> fc;
    if (g.dw && SUCCEEDED(g.dw->GetSystemFontCollection(fc.put())) && fc) { UINT32 i = 0; BOOL hasIt = FALSE; if (SUCCEEDED(fc->FindFamilyName(L"Segoe UI Variable Text", &i, &hasIt)) && hasIt) family_ = L"Segoe UI Variable Text"; }
    return family_;
  }

  void outside(const std::string& url) {                 // main.js openOutside: http(s) only
    std::string low = url.substr(0, 8); for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
    if (low.rfind("http://", 0) != 0 && low.rfind("https://", 0) != 0) return;
    if (deps_.openOutside) deps_.openOutside(url);
  }

  std::unique_ptr<Pane> build(const std::string& id, const json& t) {
    auto up = std::make_unique<Pane>(); Pane& p = *up;
    p.id = id; p.cloudId = str(t, "cloudId"); p.account = str(t, "account"); p.url = str(t, "cloudUrl");
    p.profile = profileName(acctEmail(p.account)); p.shownAt = GetTickCount64();
    // main.js guardWebview: only https://claude.ai/ in an account's own partition may be a pane
    if (!pageUrlOk(p.url)) { p.noWeb = true; p.why = L"This session's address is not a claude.ai page."; if (deps_.log) deps_.log("cloud: refused a pane for " + p.url); return up; }
    if (!canWeb()) {
      p.noWeb = true;
      if (test_ && parent_) { p.why = L"(a test run: claude.ai is never loaded)"; if (deps_.log) deps_.log("cloud: NOT LOADED in a test (no stand-in page): " + p.url); }
      return up;
    }
    SHCreateDirectoryExW(nullptr, dir_.c_str(), nullptr);
    p.web = std::make_unique<web::WebPane>();
    web::WebPane& w = *p.web; Pane* pp = &p; std::shared_ptr<bool> alive = alive_;
    // each of these may arrive after the pane was dropped: `live` says whether pp still is this id's pane
    auto live = [this, alive, id, pp]() { if (!*alive) return false; auto it = panes_.find(id); return it != panes_.end() && it->second.get() == pp; };
    w.onReady = [this, live, pp](bool ok, const std::wstring& why) {
      if (!live()) return;
      if (!ok) { pp->noWeb = true; pp->why = L"The page's web view did not start: " + why; }
      if (deps_.log) deps_.log("cloud web view " + pp->id + (ok ? " ready, profile " + narrow(pp->profile) : " FAILED: " + narrow(why)));
      changed();
    };
    w.onLoaded = [this, live, pp](bool ok) { if (live() && deps_.log) deps_.log("cloud page " + pp->id + (ok ? " loaded: " : " did not load: ") + narrow(pp->web->url())); };
    // the top document going somewhere by itself (main.js will-navigate; a frame is the page's own business, and a
    // server redirect of a navigation already let through is not asked about there either)
    w.onNavigate = [this, live, pp](const std::wstring& wurl, bool main) {
      if (!live()) return false;
      const std::string url = narrow(wurl), as = canon(url);
      const bool redirect = pp->web->navRedirected();
      const bool ok = !main || redirect || paneUrl(as);
      if (test_) {                                        // a test loads its stand-in page and nothing else, whatever the rule says
        if (deps_.log) deps_.log(std::string("cloud nav ") + (main ? "top " : "frame ") + as + (ok ? ": allowed by the rule" : ": refused by the rule") + (ok && !standIn(url) ? " (not loaded: a test loads only its stand-in)" : ""));
        if (!ok) outside(as);
        return ok && standIn(url);
      }
      if (!ok) outside(url);
      return ok;
    };
    // a new window: only a sign-in page opens as one (the runtime's own popup, same profile); the rest goes outside
    w.onPopup = [this, live](const std::wstring& wurl) {
      if (!live()) return false;
      const bool ok = signInUrl(canon(narrow(wurl)));
      if (test_) { if (deps_.log) deps_.log("cloud popup " + canon(narrow(wurl)) + (ok ? ": a sign-in page, allowed as a popup (not opened: a test opens no window)" : ": not a sign-in page")); return false; }
      return ok;
    };
    w.onNewWindow = [this, live](const std::wstring& wurl) { if (!live()) return; const std::string as = canon(narrow(wurl)); if (signInUrl(as)) return; outside(test_ ? as : narrow(wurl)); };
    w.onUrl = [this, live, pp](const std::wstring& wurl) {
      if (!live()) return;
      const std::string as = canon(narrow(wurl));
      if (as.empty()) return;
      const bool si = signInPage(as);
      pp->hint = si;
      if (si) pp->hintText = widen(hintText(acctEmail(pp->account), acctLabel(pp->account)));
      if (deps_.log) deps_.log("cloud at " + pp->id + " " + as + (si ? "  -> the sign-in hint" : ""));
      placeHint(*pp); changed();
    };
    w.onAccelerator = [this, live](unsigned vk, bool c, bool s, bool a) { return live() && deps_.accelerator ? deps_.accelerator(vk, c, s, a) : false; };
    w.onFocus = [this, live, pp](bool has) {
      if (!live()) return;
      pp->focused = has;
      // a click in the page is a click in the pane: it selects the tile (48-cloud.js: wv 'focus' -> activate)
      if (has) { const bool self = pp->selfFocusAt && GetTickCount64() - pp->selfFocusAt < 800; pp->selfFocusAt = 0; if (deps_.focused) deps_.focused(pp->id, !self); }
      changed();
    };
    web::WebPaneOptions o = webOptions;
    o.userDataDir = dir_; o.inPrivate = false; o.removeUserDataOnClose = false; o.profileName = p.profile; o.site = true; o.background = 0x262624;
    if (rasterScale > 0) w.setRasterScale(rasterScale);
    const RectF page = g_ ? Geo::page(*g_, p.pane, nameSize_, p.inset) : RectF();
    if (!w.create(parent_, web::Rect{ page.x, page.y, page.w, page.h }, scale_, o)) {
      p.noWeb = true;
      p.why = w.state() == web::WebState::NoRuntime ? std::wstring(L"This machine has no ") + web::runtimeDownloadName() + L": install it from Microsoft to see the page here."
                                                    : L"The page's web view did not start: " + w.error();
      if (deps_.log) deps_.log("cloud: no web view: " + narrow(p.why));
      p.web.reset(); return up;
    }
    w.show(false);                                        // sync() shows it once it knows where it is
    p.hidden = true;
    if (deps_.log) deps_.log("cloud pane built " + id + " -> " + real(p.url) + " (as " + p.url + "), profile " + narrow(p.profile));
    w.navigate(widen(real(p.url)));
    return up;
  }
  void drop(Pane& p) {
    if (p.web) { if (p.web->browserPid()) { pid_ = p.web->browserPid(); pidAt_ = GetTickCount64(); } p.web->destroy(0); p.web.reset(); }
    if (p.hintWnd) { DestroyWindow(p.hintWnd); p.hintWnd = nullptr; }
    links_.erase(p.id);
  }
  void changed() { if (deps_.changed) deps_.changed(); }

  // hidden: nothing of the pane is on screen (it left the layout, a zoom is in front of it, the ask pane is over the
  // whole area, the window is minimised). The web view is not shown and its page is suspended.
  void setHidden(Pane& p, bool on) {
    if (on == p.hidden) return;
    p.hidden = on;
    if (!p.web) return;
    if (on) {
      if (p.focused && parent_) SetFocus(parent_);
      p.covered = false; p.snap.clear(); p.snapBmp.reset(); p.snapGen++;
      p.web->show(false); p.web->suspend();
    } else p.web->show(true);
  }
  // covered: something of the app's is drawn over the pane (asks.hpp cover(), per pane)
  void setCovered(Pane& p, bool on) {
    if (on == p.covered) return;
    p.covered = on; p.snapGen++;
    if (!p.web) return;
    if (!on) {
      p.snap.clear(); p.snapBmp.reset();
      p.web->show(true);
      if (p.focusBeforeCover && parent_ && GetFocus() == parent_) { p.selfFocusAt = GetTickCount64(); p.web->focus(); }
      p.focusBeforeCover = false; changed(); return;
    }
    p.focusBeforeCover = p.focused;
    if (p.focused && parent_) SetFocus(parent_);         // the overlay is the app's: so are the keys
    if (p.web->state() != web::WebState::Ready) { p.web->show(false); return; }
    const unsigned gen = p.snapGen; std::shared_ptr<bool> alive = alive_; Pane* pp = &p; const std::string id = p.id;
    p.web->capture([this, alive, gen, pp, id](bool ok, const std::string& png) {
      if (!*alive) return;
      auto it = panes_.find(id); if (it == panes_.end() || it->second.get() != pp || gen != pp->snapGen || !pp->covered) return;
      pp->snap = ok ? png : std::string(); pp->snapBmp.reset();
      pp->web->show(false);
      placeHint(*pp); changed();
    });
  }

  // ---- the hint (css `.term .cloud-hint`): centred, `calc(var(--pane-name-size) * 1.2 + 14px)` under the pane's
  // top, at most 80 % wide; #1c2a3a, 1px #3e6a9a, #dbe8f6, 12.5px, padding 6px 10px
  RectF hintBox(Gfx& g, const Pane& p) {
    Font f; f.family = family(g); f.size = 12.5f;
    const float maxW = std::max(60.f, p.pane.w * 0.8f);
    const float w = std::min(maxW, std::ceil(g.textWidth(p.hintText, f)) + 22);
    Com<IDWriteTextLayout> lay = g.layout(p.hintText, f, std::max(1.f, w - 22), 2000, true, false, 0);
    DWRITE_TEXT_METRICS m{}; if (lay) lay->GetMetrics(&m);
    const float h = std::ceil(m.height > 0 ? m.height : 17) + 14;
    return RectF(g.snap(p.pane.x + (p.pane.w - w) / 2), g.snap(p.pane.y + nameSize_ * 1.2f + 14), w, h);
  }
  // `box` in the target's own DIPs
  void paintHint(ID2D1RenderTarget* rt, Gfx& g, const RectF& box, const std::wstring& text) {
    if (!rt) return;
    Com<ID2D1SolidColorBrush> br; if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0x1c2a3a), br.put())) || !br) return;
    rt->FillRectangle(D2D1::RectF(box.x, box.y, box.r(), box.b()), br.p);
    br->SetColor(D2D1::ColorF(0x3e6a9a));
    rt->DrawRectangle(D2D1::RectF(box.x + 0.5f, box.y + 0.5f, box.r() - 0.5f, box.b() - 0.5f), br.p, 1);
    Font f; f.family = family(g); f.size = 12.5f;
    Com<IDWriteTextLayout> lay = g.layout(text, f, std::max(1.f, box.w - 22), 2000, true, false, 0);
    br->SetColor(D2D1::ColorF(0xdbe8f6));
    if (lay) rt->DrawTextLayout(D2D1::Point2F(box.x + 11, box.y + 7), lay.p, br.p, (D2D1_DRAW_TEXT_OPTIONS)4);
  }
  bool hintWindowUp(const Pane& p) const { return p.hintWnd && IsWindowVisible(p.hintWnd); }
  static LRESULT CALLBACK hintProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;          // css pointer-events: none
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
      CloudPanes* self = (CloudPanes*)GetWindowLongPtrW(h, GWLP_USERDATA);
      if (self) self->paintHintWindow(h, dc);
      EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, m, w, l);
  }
  void paintHintWindow(HWND h, HDC dc) {
    if (!g_ || !g_->d2d) return;
    Pane* p = nullptr; for (auto& kv : panes_) if (kv.second->hintWnd == h) p = kv.second.get();
    if (!p) return;
    if (!hintRt_) {
      const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 0, 0);
      if (FAILED(g_->d2d->CreateDCRenderTarget(&props, hintRt_.put()))) { hintRt_.reset(); return; }
      hintRt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    RECT rc; GetClientRect(h, &rc);
    if (FAILED(hintRt_->BindDC(dc, &rc))) return;
    hintRt_->SetDpi(96 * scale_, 96 * scale_);
    hintRt_->BeginDraw(); hintRt_->SetTransform(D2D1::Matrix3x2F::Identity());
    paintHint(hintRt_.p, *g_, RectF(0, 0, rc.right / scale_, rc.bottom / scale_), p->hintText);
    if (hintRt_->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) hintRt_.reset();
  }
  // the hint's own window: up while the hint shows over a LIVE page; otherwise the frame draws the hint itself
  void placeHint(Pane& p) {
    const bool want = p.hint && p.web && parent_ && g_ && p.seen && !p.hidden && !p.covered;
    if (!want) { if (p.hintWnd && IsWindowVisible(p.hintWnd)) { ShowWindow(p.hintWnd, SW_HIDE); changed(); } return; }
    if (!hintClassMade_) {
      WNDCLASSW wc = {}; wc.lpfnWndProc = hintProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ClaudeDeckNativeCloudHint"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
      RegisterClassW(&wc); hintClassMade_ = true;
    }
    const RectF b = hintBox(*g_, p);
    RECT px = { (LONG)std::lround(b.x * scale_), (LONG)std::lround(b.y * scale_), (LONG)std::lround(b.r() * scale_), (LONG)std::lround(b.b() * scale_) };
    if (!p.hintWnd) {
      p.hintWnd = CreateWindowExW(WS_EX_NOACTIVATE, L"ClaudeDeckNativeCloudHint", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, px.left, px.top, px.right - px.left, px.bottom - px.top, parent_, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!p.hintWnd) return;
      SetWindowLongPtrW(p.hintWnd, GWLP_USERDATA, (LONG_PTR)this);
    }
    const bool moved = memcmp(&px, &p.hintPx, sizeof px) != 0, up = IsWindowVisible(p.hintWnd) != 0;
    if (moved || !up) {
      p.hintPx = px;
      SetWindowPos(p.hintWnd, HWND_TOP, px.left, px.top, px.right - px.left, px.bottom - px.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);   // above the page's own window
      InvalidateRect(p.hintWnd, nullptr, FALSE);
      if (!up) changed();
    }
  }
};

} }  // namespace deck::cloud
