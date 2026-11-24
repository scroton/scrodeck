// windows/web-test/asks_test.cpp — the ask pane (windows/src/asks.hpp) against a deck, headless.
//
// A PARKED window (off every screen, never activated, no taskbar button) stands in for the app: it owns an
// AskPane, connects to the fake deck (fake-deck.js: the real lib/phone.js with fake deps) the way the app does
// (ws.hpp), and walks the pane through what a person does: open every session's asks, answer cards, Done, one
// session's view, the read-only "answered" view, the recently answered list, close. It asserts on the pane's
// state, on what the pages say, and on the exact calls and requests the deck received; it saves a picture of each
// stage (the Gfx-drawn bar composed with the web view's CapturePreview). Exit code 0 = every check passed.
//   deck-web-asks.exe <port> <out dir> <the fake deck's calls file>
#include "../src/asks.hpp"
#include "../src/cloud.hpp"     // the cloud panes' rules (pure: what may load, the hint, the profile names)
#include <shellapi.h>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace deck;
using json = nlohmann::json;

static const char* P1 = "ask_native-test_11111111_r1_1118-1935_c8f2";
static const char* S1 = "11111111-2222-4333-8444-555555555555";
static const char* P2 = "ask_bug-queue_aaaaaaaa_r1_1118-1935_c233";
static const char* S2 = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
static const UINT WM_WS = WM_APP + 1;
static const float WIN_W = 1100, WIN_H = 720, SIDEBAR_W = 300;

static FILE* gLog = nullptr;
static int gFails = 0;
static void logf(const char* fmt, ...) { if (!gLog) return; va_list a; va_start(a, fmt); vfprintf(gLog, fmt, a); va_end(a); fputc('\n', gLog); fflush(gLog); }
static void check(const std::string& name, bool ok, const std::string& detail = "") { if (!ok) gFails++; logf("%s  %s%s%s", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  - ", detail.c_str()); }

struct App {
  HWND hwnd = nullptr;
  float scale = 1;
  Gfx g;
  WsClient ws;
  asks::AskPane pane;
  std::wstring outDir, callsFile;
  int rid = 0;
  std::map<int, std::function<void(bool, const json&, const std::string&)>> waiting;
  bool gotMeta = false;
  int windowFrames = 0;                 // > 0: WM_PAINT draws frames into the window target (the composed-window check)
  std::string activated, noteText, clipboard; int focusTerminal = 0, changed = 0;
  std::vector<std::wstring> links;
  std::vector<std::pair<unsigned, int>> accels;

  RectF mainRect() const { return RectF(SIDEBAR_W, 0, WIN_W - SIDEBAR_W, WIN_H); }

  void onWs(WPARAM ev, std::string* s) {
    std::unique_ptr<std::string> hold(s);
    if (ev != WsMessage) return;
    json m = json::parse(*s, nullptr, false);
    if (!m.is_object()) return;
    const std::string t = m.value("t", "");
    if (t == "init" || t == "meta") { if (m.contains("meta")) { pane.setMeta(m["meta"]); gotMeta = true; } }
    else if (t == "ok" || t == "err") {
      auto it = waiting.find(m.value("rid", -1)); if (it == waiting.end()) return;
      auto f = std::move(it->second); waiting.erase(it);
      if (t == "ok") f(true, m.contains("result") ? m["result"] : json(), ""); else f(false, json(), m.value("error", "error"));
    }
  }
  void call(const std::string& fn, const json& args, std::function<void(bool, const json&, const std::string&)> done) {
    const int id = ++rid; if (done) waiting[id] = std::move(done);
    ws.send(json{ { "t", "call" }, { "rid", id }, { "fn", fn }, { "args", args } }.dump());
  }
  json record() const {
    // shared for delete: the fake replaces the file by rename while this reads it
    std::string text;
    HANDLE h = CreateFileW(callsFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { char buf[65536]; DWORD got = 0; while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got); CloseHandle(h); }
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j : json::object();
  }
  // the last recorded call of this name whose first argument is `first` ('' = any)
  json lastCall(const std::string& fn, const std::string& first = "") const {
    json r = record(), hit;
    if (r.contains("calls")) for (auto& c : r["calls"]) if (c.value("fn", "") == fn && (first.empty() || (c["args"].is_array() && !c["args"].empty() && c["args"][0] == first))) hit = c;
    return hit;
  }

  // the picture a person would see: the Gfx-drawn pane (bar) with the web view's capture where its window is
  void capture(const std::wstring& name, std::function<void()> then) {
    const std::wstring webPng = outDir + L"\\" + name + L".web.png", outPng = outDir + L"\\" + name + L".png";
    pane.web().capturePng(webPng, [this, webPng, outPng, name, then](bool ok) {
      bool saved = false;
      if (ok && g.targetBitmap((int)WIN_W, (int)WIN_H, scale)) {
        g.begin(); g.clear(Rgba::hex(0x0c0c0c));
        g.fill(RectF(0, 0, SIDEBAR_W, WIN_H), Rgba::hex(0x0f0f0f));     // where the sidebar would be
        Font f; f.size = 12; g.text(L"(the sidebar: not part of this test)", RectF(12, 8, SIDEBAR_W - 24, 20), f, Rgba::hex(0x6a6a6a));
        pane.draw(g);
        Com<IWICBitmapDecoder> dec; Com<IWICBitmapFrameDecode> frame; Com<IWICFormatConverter> conv; Com<ID2D1Bitmap> bmp;
        if (pane.isOpen() && SUCCEEDED(g.wic->CreateDecoderFromFilename(webPng.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, dec.put())) && dec
            && SUCCEEDED(dec->GetFrame(0, frame.put())) && SUCCEEDED(g.wic->CreateFormatConverter(conv.put()))
            && SUCCEEDED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))
            && SUCCEEDED(g.rt->CreateBitmapFromWicBitmap(conv.p, nullptr, bmp.put())) && bmp) {
          const RectF w = pane.webRect();
          g.rt->DrawBitmap(bmp.p, D2D1::RectF(w.x, w.y, w.r(), w.b()), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        g.end();
        saved = g.savePng(outPng);
      }
      if (saved) DeleteFileW(webPng.c_str());
      check("picture " + narrow(name) + ".png", saved);
      then();
    });
  }
};
static App* gApp = nullptr;

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_WS && gApp) { gApp->onWs(w, (std::string*)l); return 0; }
  if (m == WM_PAINT && gApp && gApp->windowFrames) {      // a frame as the app paints one: in WM_PAINT, into the window's Direct2D target
    PAINTSTRUCT ps; BeginPaint(h, &ps);
    App& a = *gApp;
    a.g.begin(); a.g.clear(Rgba::hex(0x0c0c0c)); a.g.fill(RectF(0, 0, SIDEBAR_W, WIN_H), Rgba::hex(0x0f0f0f));
    a.g.fill(a.mainRect(), Rgba::hex(0x7a1020));          // what a frame would paint under the pane: must never show through the page
    a.pane.draw(a.g);
    const bool okEnd = a.g.end(); a.windowFrames++;
    if (a.windowFrames == 2) logf("INFO  window frame: EndDraw %s, window state %u, target %ux%u px", okEnd ? "ok" : "RECREATE", a.g.hwndRt ? (unsigned)a.g.hwndRt->CheckWindowState() : 99u, a.g.hwndRt ? a.g.hwndRt->GetPixelSize().width : 0, a.g.hwndRt ? a.g.hwndRt->GetPixelSize().height : 0);
    EndPaint(h, &ps);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

// ---- the script: steps run one after the other; each starts something, then is polled until it says done
struct Step { std::string name; std::function<void()> start; std::function<bool()> done; int timeoutMs = 15000; };

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc < 4) return 2;
  const int port = _wtoi(argv[1]);
  App app; gApp = &app;
  app.outDir = argv[2]; app.callsFile = argv[3];
  gLog = _wfopen((app.outDir + L"\\asks.log").c_str(), L"w");
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  web::WebPane::sweepStale();

  WNDCLASSW wc = {}; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"DeckWebAsksTest";
  RegisterClassW(&wc);
  app.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP | WS_CLIPCHILDREN, -32000, -32000, 100, 100, nullptr, nullptr, inst, nullptr);
  UINT dpi = GetDpiForWindow(app.hwnd); if (!dpi) dpi = 96;
  app.scale = dpi / 96.f;
  SetWindowPos(app.hwnd, nullptr, -32000, -32000, (int)(WIN_W * app.scale), (int)(WIN_H * app.scale), SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);
  logf("window dpi %u scale %.2f", dpi, app.scale);
  if (!app.g.init()) { logf("FAIL  Gfx init"); return 1; }

  // links: only an address, only http(s) — and never opened by this test
  check("openExternal refuses file:", !web::WebPane::openExternal(L"file:///C:/Windows/notepad.exe"));
  check("openExternal refuses javascript:", !web::WebPane::openExternal(L"javascript:alert(1)"));
  check("openExternal refuses a quoted address", !web::WebPane::openExternal(L"https://example.invalid/\" & calc"));

  // ---- whose key is it (keys.hpp: pure). The window's own keys, by either road: its message loop (windowKeyOf)
  // and, while the page has the keyboard, the web view's accelerator event (webKeyOf).
  {
    auto M = [](bool c, bool s, bool a) { Mods m; m.ctrl = c; m.shift = s; m.alt = a; return m; };
    check("window key: F5 redraw, F6 zoom, F9 sidebar, F10 fullscreen, F11 / F12 step", windowKeyOf(VK_F5, M(0, 0, 0)) == WinKey::Redraw && windowKeyOf(VK_F6, M(0, 0, 0)) == WinKey::Zoom && windowKeyOf(VK_F9, M(0, 0, 0)) == WinKey::Sidebar
      && windowKeyOf(VK_F10, M(0, 0, 0)) == WinKey::Fullscreen && windowKeyOf(VK_F11, M(0, 0, 0)) == WinKey::Prev && windowKeyOf(VK_F12, M(0, 0, 0)) == WinKey::Next);
    check("window key: F7 asks, also with Shift; not with Ctrl or Alt", windowKeyOf(VK_F7, M(0, 0, 0)) == WinKey::Asks && windowKeyOf(VK_F7, M(0, 1, 0)) == WinKey::Asks && windowKeyOf(VK_F7, M(1, 0, 0)) == WinKey::None && windowKeyOf(VK_F7, M(0, 0, 1)) == WinKey::None);
    check("window key: Ctrl+Shift+P palette; not Ctrl+P, Shift+P, Ctrl+Alt+Shift+P", windowKeyOf('P', M(1, 1, 0)) == WinKey::Palette && windowKeyOf('P', M(1, 0, 0)) == WinKey::None && windowKeyOf('P', M(0, 1, 0)) == WinKey::None && windowKeyOf('P', M(1, 1, 1)) == WinKey::None);
    check("window key: Ctrl+` sidebar; not ` alone, not Ctrl+Shift+`", windowKeyOf(VK_OEM_3, M(1, 0, 0)) == WinKey::Sidebar && windowKeyOf(VK_OEM_3, M(0, 0, 0)) == WinKey::None && windowKeyOf(VK_OEM_3, M(1, 1, 0)) == WinKey::None);
    bool none = true;
    for (UINT vk : { (UINT)VK_RETURN, (UINT)VK_TAB, (UINT)VK_BACK, (UINT)VK_DELETE, (UINT)VK_LEFT, (UINT)VK_RIGHT, (UINT)VK_UP, (UINT)VK_DOWN, (UINT)VK_HOME, (UINT)VK_END, (UINT)VK_PRIOR, (UINT)VK_NEXT, (UINT)VK_SPACE, (UINT)VK_F1, (UINT)VK_F2, (UINT)VK_F3, (UINT)VK_F4, (UINT)VK_F8, (UINT)'A', (UINT)'1' })
      if (windowKeyOf(vk, M(0, 0, 0)) != WinKey::None) none = false;
    check("window key: nothing a page types or moves with (Enter, Tab, arrows, Backspace, F1-F4, F8, letters, digits)", none);
    // the web view's event: App = handled by the app (the page never sees it), ClosePane, Page = let through
    bool app = true;
    for (UINT vk : { (UINT)VK_F5, (UINT)VK_F6, (UINT)VK_F7, (UINT)VK_F9, (UINT)VK_F10, (UINT)VK_F11, (UINT)VK_F12 }) for (bool inFrame : { false, true }) if (webKeyOf(vk, false, false, false, false, inFrame) != WebKey::App) app = false;
    check("page has the keys: F5 F6 F7 F9 F10 F11 F12 are the app's, wherever the caret is", app);
    check("page has the keys: Ctrl+Shift+P and Ctrl+` are the app's", webKeyOf('P', true, true, false, false, true) == WebKey::App && webKeyOf(VK_OEM_3, true, false, false, false, true) == WebKey::App);
    check("page has the keys: Esc closes the pane, unless a page's own box has the caret", webKeyOf(VK_ESCAPE, false, false, false, false, false) == WebKey::ClosePane && webKeyOf(VK_ESCAPE, false, false, false, false, true) == WebKey::Page);
    check("page has the keys: Shift+Esc, Ctrl+Esc stay the page's", webKeyOf(VK_ESCAPE, false, true, false, false, false) == WebKey::Page && webKeyOf(VK_ESCAPE, true, false, false, false, false) == WebKey::Page);
    bool page = true;
    for (UINT vk : { (UINT)'C', (UINT)'V', (UINT)'X', (UINT)'A', (UINT)'Z', (UINT)'P' }) if (webKeyOf(vk, true, false, false, false, true) != WebKey::Page) page = false;
    for (UINT vk : { (UINT)VK_RETURN, (UINT)VK_TAB, (UINT)VK_BACK, (UINT)VK_LEFT, (UINT)VK_UP, (UINT)VK_HOME, (UINT)VK_PRIOR, (UINT)VK_F1, (UINT)VK_F8 }) if (webKeyOf(vk, false, false, false, false, true) != WebKey::Page || webKeyOf(vk, false, true, false, false, false) != WebKey::Page) page = false;
    check("page has the keys: Ctrl+C/V/X/A/Z/P, Enter, Tab, Backspace, arrows, Home, PgUp, F1, F8 go to the page", page);
    check("page has the keys: Ctrl+F7 and Alt+F7 are not the asks key", webKeyOf(VK_F7, true, false, false, false, false) == WebKey::Page && webKeyOf(VK_F7, false, false, true, false, false) == WebKey::Page);
    check("AltGr (Ctrl+Alt with the right Alt) is no modifier: AltGr+` is not Ctrl+`, AltGr+F7 is F7", webKeyOf(VK_OEM_3, true, false, true, true, true) == WebKey::Page && webKeyOf(VK_F7, true, false, true, true, true) == WebKey::App && modsFrom(true, true, true, true).shift && !modsFrom(true, false, true, true).ctrl);
    {   // src/cloud.hpp: main.js guardWebview / paneUrl / signInUrl, 48-cloud.js onNav / cloudSuffix
      using namespace deck::cloud;
      check("cloud: a pane is only made for https://claude.ai/", pageUrlOk("https://claude.ai/code/session_x") && !pageUrlOk("https://claude.ai.evil.com/x") && !pageUrlOk("http://claude.ai/x") && !pageUrlOk("https://evil.com/claude.ai/"));
      check("cloud: paneUrl: claude.ai, claude.com, anthropic.com and what is under them", paneUrl("https://claude.ai/x") && paneUrl("https://api.claude.com/") && paneUrl("https://console.anthropic.com/a?b") && paneUrl("https://CLAUDE.AI/") && paneUrl("https://claude.ai:443/x"));
      check("cloud: paneUrl refuses look-alikes, http, other schemes, a host hidden behind an @", !paneUrl("https://notclaude.ai/") && !paneUrl("https://claude.ai.evil.com/") && !paneUrl("http://claude.ai/") && !paneUrl("file:///c:/x") && !paneUrl("https://evil.com/?https://claude.ai/") && !paneUrl("https://claude.ai@evil.com/") && !paneUrl("javascript:alert(1)") && !paneUrl(""));
      check("cloud: signInUrl: the six hosts a login passes through, nothing that only looks like one", signInUrl("https://accounts.google.com/o/oauth2") && signInUrl("https://appleid.apple.com/") && signInUrl("https://login.microsoftonline.com/x") && signInUrl("https://login.live.com/") && signInUrl("https://acme.okta.com/") && signInUrl("https://x.auth0.com/") && paneUrl("https://accounts.google.com/x") && !signInUrl("https://google.com/") && !signInUrl("https://accounts.google.com.evil.com/") && !signInUrl("https://claude.ai/login"));
      check("cloud: the sign-in hint shows on claude.ai's login pages and anywhere off claude.ai", signInPage("https://claude.ai/login?x") && signInPage("https://claude.ai/signup") && signInPage("https://claude.ai/magic-link#a") && signInPage("https://accounts.google.com/x") && !signInPage("https://claude.ai/code/session_1") && !signInPage("https://www.claude.ai/chat"));
      check("cloud: cloudSuffix, the profile names, the hint's words", cloudSuffix("session_01abc") == "01abc" && cloudSuffix("cse_9") == "9" && cloudSuffix("zzz") == "zzz" && profileName("the user@Example.com") == L"claude-demo@example.com" && profileName("a b/c") == L"claude-a_b_c" && profileName(std::string(100, 'x')).size() == 64
        && hintText("a@b.c", "Work") == "Sign in as a@b.c (Work). Once is enough: every cloud pane of this account shares the login." && hintText("a@b.c", "a@b.c") == "Sign in as a@b.c. Once is enough: every cloud pane of this account shares the login.");
    }
  }

  DeckLink link; link.secure = false; link.host = L"127.0.0.1"; link.port = port; link.token = "faketoken00000001";
  asks::Deps deps;
  deps.call = [&](const std::string& fn, const json& args, std::function<void(bool, const json&, const std::string&)> done) { app.call(fn, args, done); };
  deps.fire = [&](const std::string& fn, const json& args) { app.ws.send(json{ { "t", "call" }, { "rid", 0 }, { "fn", fn }, { "args", args } }.dump()); };
  deps.activate = [&](const std::string& id) { app.activated = id; };
  deps.focusTerminal = [&] { app.focusTerminal++; };
  deps.note = [&](const std::string& t, int) { app.noteText = t; logf("note: %s", t.c_str()); };
  deps.writeClipboard = [&](const std::string& t) { app.clipboard = t; };       // kept here: the test never touches the real clipboard
  deps.changed = [&] { app.changed++; };
  deps.accelerator = [&](unsigned vk, bool c, bool s, bool a) { app.accels.push_back({ vk, (c ? 1 : 0) | (s ? 2 : 0) | (a ? 4 : 0) }); return vk == VK_F6 || vk == VK_F7; };
  deps.openLink = [&](const std::wstring& url) { app.links.push_back(url); };  // recorded, never opened
  app.pane.init(app.hwnd, deps);
  app.pane.setLink(link);
  app.pane.layout(app.mainRect(), app.scale);
  app.ws.start(link, app.hwnd, WM_WS);

  asks::AskPane& pane = app.pane;
  std::string r1, r2, r3; bool flag = false; DWORD mark = 0;
  auto host = [&](const std::string& js, std::string* out) { out->clear(); pane.web().exec(js, [out](bool ok, const std::string& s) { *out = ok ? (s.empty() ? "null" : s) : "!"; }); };
  auto inFrame = [&](const std::string& name, const std::string& js, std::string* out) { out->clear(); pane.web().execInFrame(widen(name), js, [out](bool ok, const std::string& s) { *out = ok ? (s.empty() ? "null" : s) : "!"; }); };
  // a script's result arrives as JSON: the string it names (non-ASCII may come escaped)
  auto S = [](const std::string& r) { json j = json::parse(r, nullptr, false); return j.is_string() ? j.get<std::string>() : r; };
  // a real (trusted) mouse click in the host document, at the middle of the element `sel` names
  auto clickHost = [&](const std::string& sel) {
    pane.web().exec("(function(){var e=document.querySelector(" + json(sel).dump() + ");if(!e)return null;var r=e.getBoundingClientRect();return [r.left+r.width/2,r.top+r.height/2];})()",
      [&](bool ok, const std::string& s) {
        json p = json::parse(s, nullptr, false);
        if (!ok || !p.is_array()) { check("found the element to click", false, s); return; }
        for (const char* type : { "mousePressed", "mouseReleased" })
          pane.web().devtoolsCall(L"Input.dispatchMouseEvent", json{ { "type", type }, { "x", p[0] }, { "y", p[1] }, { "button", "left" }, { "clickCount", 1 } }.dump(), nullptr);
      });
  };
  const std::string secSel1 = std::string("section[data-sid=\"") + S1 + "\"]", secSel2 = std::string("section[data-sid=\"") + S2 + "\"]";
  auto pollHost = [&](const std::string& js, std::string* out) { static DWORD last = 0; if (GetTickCount() - last > 200) { last = GetTickCount(); host(js, out); } };

  // the window's own picture, as composed (the app's --window-shot does the same): pixels 0xRRGGBB, and a PNG
  auto grabWindow = [&](std::vector<unsigned>& px, int& W, int& H, const wchar_t* name) {
    RECT rc; GetClientRect(app.hwnd, &rc); W = rc.right; H = rc.bottom;
    HDC wdc = GetDC(app.hwnd), mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HBITMAP dib = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    InvalidateRect(app.hwnd, nullptr, FALSE); UpdateWindow(app.hwnd);       // as the app's own --window-shot does
    const BOOL ok = PrintWindow(app.hwnd, mem, 1 /* PW_CLIENTONLY */ | 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();
    px.resize((size_t)W * H);
    for (size_t i = 0; i < px.size(); i++) { const BYTE* p = (const BYTE*)bits + i * 4; px[i] = (unsigned)(p[2] << 16 | p[1] << 8 | p[0]); }
    SelectObject(mem, old);
    bool saved = false;
    { Com<IWICBitmap> wb; Com<IWICStream> st; Com<IWICBitmapEncoder> enc; Com<IWICBitmapFrameEncode> fr;
      const std::wstring out = app.outDir + L"\\" + name;
      if (SUCCEEDED(app.g.wic->CreateBitmapFromHBITMAP(dib, nullptr, WICBitmapIgnoreAlpha, wb.put())) && SUCCEEDED(app.g.wic->CreateStream(st.put())) && SUCCEEDED(st->InitializeFromFilename(out.c_str(), GENERIC_WRITE))
          && SUCCEEDED(app.g.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) && SUCCEEDED(enc->Initialize(st.p, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(fr.put(), nullptr))) {
        fr->Initialize(nullptr); fr->SetSize(W, H); WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR; fr->SetPixelFormat(&fmt);
        saved = SUCCEEDED(fr->WriteSource(wb.p, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
      } }
    DeleteObject(dib); DeleteDC(mem); ReleaseDC(app.hwnd, wdc);
    return ok && saved;
  };

  std::vector<Step> steps;
  steps.push_back({ "the deck's hello carries the asks", [] {}, [&] { return app.gotMeta; } });
  // the base line for the composed-window check further down: this window's Direct2D frames, no web view yet
  steps.push_back({ "the window's Direct2D frames alone (before any web view)", [&] {
      check("a window render target", app.g.targetWindow(app.hwnd));
      app.windowFrames = 1; mark = GetTickCount();
    }, [&] {
      if (app.windowFrames < 4) { if (GetTickCount() - mark >= (DWORD)(app.windowFrames * 60)) { InvalidateRect(app.hwnd, nullptr, FALSE); UpdateWindow(app.hwnd); } return false; }
      if (GetTickCount() - mark < 500) return false;
      std::vector<unsigned> px; int W = 0, H = 0;
      const bool printed = grabWindow(px, W, H, L"asks-0-window-before.png");
      app.windowFrames = 0;
      char b[96]; snprintf(b, sizeof b, "sidebar #%06x main #%06x", printed ? px[(size_t)(300 * app.scale) * W + (int)(20 * app.scale)] : 0, printed ? px[(size_t)(300 * app.scale) * W + (int)(600 * app.scale)] : 0);
      check("PrintWindow sees this window's Direct2D frame", printed && px[(size_t)(300 * app.scale) * W + (int)(20 * app.scale)] == 0x0f0f0f && px[(size_t)(300 * app.scale) * W + (int)(600 * app.scale)] == 0x7a1020, b);
      return true;
    } });
  steps.push_back({ "open every session's asks; both pages load and take their drafts", [&] {
      check("mark before anything: 5 waiting", pane.markInfo(S1).kind == "waiting" && pane.markInfo(S1).text == "5", pane.markInfo(S1).title);
      pane.asksSelect(""); pane.tileClick("");
      check("the pane is open on every session", pane.isOpen() && pane.mode() == "all");
      check("web view state", pane.web().state() == web::WebState::Creating || pane.web().state() == web::WebState::Ready, narrow(pane.web().error()));
    }, [&] { return pane.hostReady() && !app.lastCall("askDraftGet", P1).is_null() && !app.lastCall("askDraftGet", P2).is_null(); } });
  steps.push_back({ "settle, then the first picture", [&] { mark = GetTickCount(); flag = false; }, [&] {
      if (GetTickCount() - mark < 900) return false;
      if (mark != 1) {
        mark = 1;
        check("bar title", pane.barTitle() == "Asks \xC2\xB7 2 sessions waiting", pane.barTitle());
        check("bar hint", pane.barHint() == "F6 or F7 closes \xC2\xB7 Done sends that session its answers", pane.barHint());
        json rec = app.record(); bool cookie = true, noToken = true, embed = true; int n = 0;
        for (auto& h : rec["http"]) { n++; if (h.value("auth", "") != "cookie") cookie = false; if (h.value("url", "").find("t=") != std::string::npos) noToken = false; if (h.value("url", "").find("?embed=deck&theme=dark") == std::string::npos) embed = false; }
        check("the deck served both pages", n >= 2, std::to_string(n) + " requests");
        check("every page request was let in by the deck_phone cookie", cookie);
        check("no page URL carries the token", noToken);
        check("pages are asked for with ?embed=deck&theme=dark", embed);
        app.capture(L"asks-1-all", [&] { flag = true; });
      }
      return flag;
    } });
  steps.push_back({ "the host document shows both sessions as the js lays them out", [&] {
      host("(function(){var s=[...document.querySelectorAll('section.ap-sess:not(.ap-recent)')].map(function(e){return {sid:e.dataset.sid,hidden:e.hidden,name:e.querySelector('.ap-name').textContent,meta:e.querySelector('.ap-meta').textContent,"
           "count:e.querySelector('.ap-count').textContent,done:e.querySelector('.ap-done').textContent,term:e.querySelector('.ap-term').textContent,mark:e.querySelector('.ap-mark').textContent,abbr:e.querySelector('.ap-abbr').textContent,"
           "frames:[...e.querySelectorAll('iframe')].map(function(f){return {src:f.getAttribute('src'),sandbox:f.getAttribute('sandbox'),h:f.getBoundingClientRect().height,title:f.title}}),sticky:getComputedStyle(e.querySelector('.ap-head')).position}});"
           "return {secs:s,empty:document.querySelector('.ap-empty').hidden,recent:document.querySelector('.ap-recent').hidden,origin:location.origin,cookie:document.cookie}})()", &r1);
    }, [&] {
      if (r1.empty()) return false;
      json j = json::parse(r1, nullptr, false);
      const bool ok = j.is_object() && j["secs"].is_array() && j["secs"].size() == 2;
      check("two sections", ok, r1.substr(0, 300));
      if (ok) {
        json a = j["secs"][0], b = j["secs"][1];
        check("section 1: the session outside the deck", a.value("sid", "") == S1 && a.value("name", "") == "native-test" && a.value("term", "") == "Answer in its terminal" && a.value("abbr", "") == "", a.dump().substr(0, 300));
        check("section 1 meta", a.value("meta", "").rfind("native-test \xC2\xB7 claude \xC2\xB7 round 1 \xC2\xB7 asked ", 0) == 0 && a.value("meta", "").find("outside the deck") != std::string::npos, a.value("meta", ""));
        check("section 1 count and mark", a.value("count", "") == "0 of 5 answered" && a.value("mark", "") == "5" && a.value("done", "") == "Done", a.value("count", "") + " / " + a.value("mark", ""));
        check("section 2: a tile of this deck (its title, its account)", b.value("sid", "") == S2 && b.value("name", "") == "Bug queue" && b.value("abbr", "") == "vv" && b.value("term", "") == "Answer in the terminal", b.dump().substr(0, 300));
        check("headers are sticky", a.value("sticky", "") == "sticky");
        json f = a["frames"];
        check("one sandboxed frame per page, sized to the page", f.size() == 1 && f[0].value("sandbox", "") == "allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads" && f[0].value("h", 0.0) > 400, f.dump());
        check("the frame's address is the deck's /ask/ route, no token", f.size() == 1 && f[0].value("src", "") == "http://127.0.0.1:" + std::to_string(port) + "/ask/" + P1 + ".html?embed=deck&theme=dark", f.size() ? f[0].value("src", "") : "");
        check("the host document cannot read the cookie (HttpOnly)", j.value("cookie", "x").empty(), j.value("cookie", ""));
        check("nothing waiting line hidden, no recent list yet", j.value("empty", false) && j.value("recent", false));
      }
      return true;
    } });
  steps.push_back({ "a link on a page asks the host, and opens nothing by itself", [&] {
      inFrame(P1, "(function(){var a=document.querySelector('a[href^=\"http\"]');if(!a)return 'none';a.click();return a.href+' '+a.target})()", &r1); mark = GetTickCount();
    }, [&] {
      if (r1.empty()) return false;
      if (app.links.empty() && GetTickCount() - mark < 2500) return false;
      if (S(r1) == "none") logf("INFO  the sample page has no link");
      else check("the link came to the host as an address", app.links.size() == 1 && app.links[0].rfind(L"http://example.invalid/", 0) == 0, r1 + " -> " + (app.links.empty() ? "(nothing)" : narrow(app.links[0])));
      return true;
    } });
  steps.push_back({ "answer two cards on the page: a click on an option, words in a box", [&] {
      inFrame(P1, "(function(){var c=document.querySelectorAll('.card');c[0].querySelector('[data-pick]').click();var t=c[1].querySelector('textarea');t.value='add 150 MB instead';t.dispatchEvent(new Event('input',{bubbles:true}));return c.length})()", &r1);
    }, [&] { json c = app.lastCall("askDraft", P1); return !c.is_null() && c["args"][1].value("drafted", 0) == 2; } });
  steps.push_back({ "the draft went to the deck exactly as the page gave it; the header follows", [&] {
      json c = app.lastCall("askDraft", P1); json p = c["args"][1];
      check("askDraft is fire-and-forget with [page id, payload]", c["args"].size() == 2 && c["args"][0] == P1);
      check("askDraft counts", p.value("drafted", -1) == 2 && p.value("answered", -1) == 2 && p.value("later", -1) == 0 && p.value("total", -1) == 5, p.dump().substr(0, 200));
      check("askDraft state: Q1's pick, Q2's words", p["state"]["Q1"].value("pick", "") == "a" && p["state"]["Q1"].value("text", "") == "ship tonight" && p["state"]["Q2"].value("text", "") == "add 150 MB instead", p["state"].dump().substr(0, 300));
      check("mark: drafted", pane.markInfo(S1).kind == "drafted" && pane.markInfo(S1).title == "2 of 5 answered on the page, not sent yet (F7)", pane.markInfo(S1).title);
      mark = GetTickCount(); flag = false; r1.clear();
    }, [&] {
      if (GetTickCount() - mark < 600) return false;
      if (r1.empty()) { pollHost("document.querySelector('" + secSel1 + " .ap-count').textContent+'|'+document.querySelector('" + secSel1 + " .ap-mark').className+'|'+document.querySelector('" + secSel1 + " .ap-discard').disabled", &r1); return false; }
      if (mark != 1) { mark = 1; check("header: 2 of 5 answered, the mark filled, Discard armed", S(r1) == "2 of 5 answered|ap-mark t-ask drafted|false", r1); app.capture(L"asks-2-answered", [&] { flag = true; }); }
      return flag;
    } });
  steps.push_back({ "an overlay of the app's over the pane: the page becomes a still picture, then comes back", [&] { pane.cover(true); mark = GetTickCount(); flag = false; }, [&] {
      if (pane.web().visible() && GetTickCount() - mark < 3000) return false;
      if (mark != 1) {
        mark = 1;
        check("covered: the web view is hidden once its picture is taken", !pane.web().visible() && pane.covered());
        // what the app's frame would show: the bar, the still picture, and something drawn over both
        bool saved = false;
        if (app.g.targetBitmap((int)WIN_W, (int)WIN_H, app.scale)) {
          app.g.begin(); app.g.clear(Rgba::hex(0x0c0c0c)); app.g.fill(RectF(0, 0, SIDEBAR_W, WIN_H), Rgba::hex(0x0f0f0f));
          pane.draw(app.g);
          const RectF box(SIDEBAR_W + 120, 90, 560, 220);
          app.g.fill(box, Rgba::hex(0x161616)); app.g.stroke(box, Rgba::hex(0x3a3a3a));
          Font f; f.size = 14; app.g.text(L"(an overlay drawn by the app: the palette, a menu, a panel)", RectF(box.x + 12, box.y + 9, box.w - 24, 20), f, Rgba::hex(0xececec));
          app.g.end(); saved = app.g.savePng(app.outDir + L"\\asks-2b-covered.png");
          pane.dropDeviceResources();
        }
        check("picture asks-2b-covered.png", saved);
        pane.cover(false);
        check("uncovered: the live page is back", pane.web().visible() && !pane.covered());
        flag = true;
      }
      return flag;
    } });
  // The real thing: Direct2D presenting into the window while the web view is its child. The window's own picture
  // (PrintWindow with full content, which sees a window parked off screen) must show BOTH: the bar Direct2D drew
  // and the page, the page not painted over by the frames presented after it came up.
  steps.push_back({ "the window itself: Direct2D frames and the web view child together", [&] {
      check("the window render target again (the pictures above drew into a bitmap)", app.g.targetWindow(app.hwnd));
      mark = GetTickCount(); app.windowFrames = 1;
    }, [&] {
      if (app.windowFrames < 9) {
        if (GetTickCount() - mark < (DWORD)(app.windowFrames * 60)) return false;
        InvalidateRect(app.hwnd, nullptr, FALSE); UpdateWindow(app.hwnd);
        return false;
      }
      if (GetTickCount() - mark < 1000) return false;
      std::vector<unsigned> px; int W = 0, H = 0;
      const bool printed = grabWindow(px, W, H, L"asks-2c-window.png");
      app.windowFrames = 0;
      check("PrintWindow gave the window's picture", printed);
      if (printed) {
        auto at = [&](float x, float y) { return px[(size_t)((int)(y * app.scale)) * W + (int)(x * app.scale)]; };
        char b[96];
        snprintf(b, sizeof b, "#%06x", at(SIDEBAR_W + 2, 2)); check("the bar Direct2D drew is in the window", at(SIDEBAR_W + 2, 2) == 0x0f0f0f, b);
        snprintf(b, sizeof b, "#%06x", at(20, 300)); check("so is the rest of the frame (the sidebar's side)", at(20, 300) == 0x0f0f0f, b);
        int red = 0; for (unsigned c : px) if (c == 0x7a1020) red++;
        check("the page is over the frame's paint (the red a frame paints under the pane does not show)", red == 0, std::to_string(red) + " red pixels");
        const RectF w = pane.webRect();
        snprintf(b, sizeof b, "#%06x", at(w.x + 0.5f, w.y + 2)); check("the web view starts right under the bar (the session header's blue left edge)", at(w.x + 0.5f, w.y + 2) == 0x6aa9ff, b);
      }
      pane.dropDeviceResources();
      return true;
    } });
  steps.push_back({ "Copy all: the page's lines, headed by its id", [&] { clickHost(secSel1 + " .ap-copy"); }, [&] { return !app.clipboard.empty(); } });
  steps.push_back({ "Done (a real click): the page is asked for its answers, the deck gets askDone", [&] {
      check("copied text", app.clipboard == std::string(P1) + "\nQ1 (fix timing): ship tonight\nQ2 (Apollo release): add 150 MB instead", app.clipboard);
      clickHost(secSel1 + " .ap-done");
    }, [&] { return !app.lastCall("askDone").is_null(); } });
  steps.push_back({ "askDone's arguments; the page locks; the pane stays (another session still waits)", [&] {
      json c = app.lastCall("askDone"); json a = c["args"];
      check("askDone(sessionId, pages)", a.size() == 2 && a[0] == S1 && a[1].is_array() && a[1].size() == 1 && a[1][0].value("id", "") == P1, a.dump().substr(0, 200));
      if (a.size() == 2 && a[1].is_array() && a[1].size() == 1) {
        json A = a[1][0]["answers"];
        check("the page's own answers JSON", A.value("kind", "") == "ask-page-answers" && A.value("id", "") == P1 && A.value("answered", -1) == 2 && A.value("total", -1) == 5 && A.value("session_id", "") == S1, A.dump().substr(0, 300));
        check("its Copy-all lines", A["lines"].is_array() && A["lines"].size() == 2 && A["lines"][0] == "Q1 (fix timing): ship tonight" && A["lines"][1] == "Q2 (Apollo release): add 150 MB instead", A["lines"].dump());
        check("each card's answer", A["answers"].is_array() && A["answers"].size() == 5 && A["answers"][0].value("choice", "") == "a" && A["answers"][0].value("status", "") == "answered" && A["answers"][2].value("status", "") == "skipped");
      }
      mark = GetTickCount(); flag = false; r1.clear(); r2.clear();
    }, [&] {
      if (GetTickCount() - mark < 900) return false;
      if (r1.empty()) { pollHost("document.querySelector('" + secSel1 + " .ap-done').textContent+'|'+document.querySelector('" + secSel1 + " .ap-done').disabled+'|'+document.querySelector('" + secSel1 + " .ap-flash').textContent+'|'+document.querySelector('" + secSel1 + " .ap-count').textContent+'|'+document.querySelector('" + secSel1 + " .ap-rnote').textContent", &r1); return false; }
      if (r2.empty()) { if (mark != 2) { mark = 2; inFrame(P1, "document.documentElement.classList.contains('locked')+'|'+document.querySelector('.card textarea').readOnly", &r2); } return false; }
      if (mark != 1) {
        mark = 1;
        check("Done reads Sent, disabled; the flash says where the answers went", S(r1).rfind("Sent|true|Saved the answers file for native-test (it runs outside the deck); it picks it up if it is watching.|2 of 5 answered \xC2\xB7 sent|Sent ", 0) == 0 && S(r1).find(" as its answers file: read-only, waiting for the session to read it") != std::string::npos, r1);
        check("the page is locked (read-only)", S(r2) == "true|true", r2);
        check("the pane is still open: the other session waits", pane.isOpen() && pane.mode() == "all");
        check("mark: sent", pane.markInfo(S1).kind == "sent" && pane.markInfo(S1).text == "\xE2\x9C\x93");
        app.capture(L"asks-3-done", [&] { flag = true; });
      }
      return flag;
    } });
  steps.push_back({ "F7 on a session: just its own; Done with nothing answered says so", [&] {
      pane.asksBlur(); pane.closePane();
      check("closed; the terminal has the keys back", !pane.isOpen() && app.focusTerminal == 1, std::to_string(app.focusTerminal));
      pane.asksKey("t1");
      check("F7 on the tile opens its session's asks", pane.isOpen() && pane.mode() == S2 && pane.barTitle() == "Bug queue asks" && pane.barHint() == "F7 closes \xC2\xB7 Done sends that session its answers", pane.barTitle() + " / " + pane.barHint());
      mark = GetTickCount(); r1.clear(); flag = false;
    }, [&] {
      if (GetTickCount() - mark < 500) return false;
      if (mark != 1) { mark = 1; clickHost(secSel2 + " .ap-done"); return false; }
      if (r1.empty() || S(r1) == "|false|true") { pollHost("document.querySelector('" + secSel2 + " .ap-flash').textContent+'|'+document.querySelector('" + secSel2 + "').hidden+'|'+document.querySelector('" + secSel1 + "').hidden", &r1); return false; }
      if (!flag && r1 != "!") { check("only that session shows; the flash", S(r1) == "Nothing answered yet: pick an option or a reply, or type in a box.|false|true", r1); r1 = "!"; app.capture(L"asks-4-session", [&] { flag = true; }); }
      return flag;
    } });
  steps.push_back({ "answer it and press Done: the deck's tile gets it, the pane closes onto the tile", [&] {
      inFrame(P2, "(function(){document.querySelector('.card [data-pick=\"b\"]').click();return 1})()", &r1);
      mark = 0;
    }, [&] {
      { json c = app.lastCall("askDraft", P2); if (c.is_null() || c["args"][1].value("drafted", 0) != 1) return false; }   // the page's change is debounced 120 ms
      if (!mark) { mark = 1; clickHost(secSel2 + " .ap-done"); return false; }
      return !pane.isOpen();
    } });
  steps.push_back({ "after Done on a deck session; then the session reads its answers", [&] {
      json c = app.lastCall("askDone");
      check("askDone for the second session", c["args"][0] == S2 && c["args"][1][0].value("id", "") == P2 && c["args"][1][0]["answers"]["lines"][0] == "Q1 (Apollo release): add 200 MB", c["args"].dump().substr(0, 120));
      check("closeOnDone: closed onto its tile, with the note", app.activated == "t1" && app.noteText == "Done: saved the answers file, and one line naming it goes in as your next message.", app.activated + " / " + app.noteText);
      pane.asksKey("t1");
      check("F7 again: its sent ask, read-only", pane.isOpen() && pane.mode() == S2);
      app.call("__read", json::array({ P2 }), nullptr);            // ask_page.py read: the round stops waiting
    }, [&] { return pane.mode() == std::string("log:") + P2 && !app.lastCall("askLogState", P2).is_null(); } });
  steps.push_back({ "the session's last answers, read-only, until it asks again", [&] { mark = GetTickCount(); flag = false; r1.clear(); r2.clear(); }, [&] {
      if (GetTickCount() - mark < 1200) return false;
      if (r1.empty()) { if (mark != 2) { mark = 2; inFrame(std::string("log:") + P2, "document.documentElement.classList.contains('locked')+'|'+document.querySelector('.card textarea').value", &r1); } return false; }
      if (r2.empty()) { pollHost("(function(){var r=document.querySelector('.ap-recent');return r.hidden+'|'+r.querySelector('.ap-rhead').hidden+'|'+r.querySelectorAll('.ap-lrow.open iframe').length+'|'+r.querySelector('.ap-lrow .ap-meta').textContent+'|'+document.querySelectorAll('section.ap-sess:not(.ap-recent):not([hidden])').length})()", &r2); return false; }
      if (mark != 1) {
        mark = 1;
        check("bar: answered, read-only", pane.barTitle().rfind("Bug queue \xC2\xB7 answered ", 0) == 0 && pane.barTitle().find(" \xC2\xB7 read-only") != std::string::npos && pane.barHint() == "F7 closes \xC2\xB7 it shows here until the session asks again", pane.barTitle() + " / " + pane.barHint());
        check("the answered page shows locked, with what was sent", S(r1) == "true|add 200 MB", r1);
        check("the log view: one open row with its frame, no list header, no waiting section", S(r2).rfind("false|true|1|bug-queue \xC2\xB7 round 1 \xC2\xB7 1 answer \xC2\xB7 saved to a file, named in one line ", 0) == 0 && S(r2).size() > 4 && S(r2).substr(S(r2).size() - 2) == "|0", r2);
        app.capture(L"asks-5-log", [&] { flag = true; });
      }
      return flag;
    } });
  steps.push_back({ "Esc closes; the tile toggles the every-session view; a row scrolls to the recent list", [&] {
      check("Esc (the app has the keys) closes the pane", pane.key(VK_ESCAPE) && !pane.isOpen());
      pane.asksSelect(asks::AskPane::RECENT); pane.tileClick(asks::AskPane::RECENT);
      check("the tile's Recently answered row opens every session's view", pane.isOpen() && pane.mode() == "all" && pane.barTitle() == "Asks \xC2\xB7 1 session waiting", pane.barTitle());
      mark = GetTickCount(); flag = false; r1.clear();
    }, [&] {
      if (GetTickCount() - mark < 900) return false;
      if (r1.empty()) { pollHost("(function(){var r=document.querySelector('.ap-recent');return r.hidden+'|'+r.querySelectorAll('.ap-lrow').length+'|'+r.querySelector('.ap-rhead .ap-meta').textContent+'|'+r.querySelectorAll('iframe').length})()", &r1); return false; }
      if (mark != 1) {
        mark = 1;
        check("the recent list: both answered pages, collapsed", S(r1) == "false|2|the last 2 you answered, every session \xC2\xB7 read-only|0", r1);
        app.capture(L"asks-6-recent", [&] { flag = true; });
      }
      return flag;
    } });
  steps.push_back({ "the tile again closes it; F6 with the asks tile selected opens it", [&] {
      pane.tileClick("");
      check("a click on the tile while every session shows closes it", !pane.isOpen());
      check("F6 opens (the asks tile is the selection)", pane.asksZoom() && pane.isOpen() && pane.mode() == "all");
      check("F6 closes", pane.asksZoom() && !pane.isOpen());
      pane.asksBlur();
      check("F6 is not the asks' when the tile is not selected", !pane.asksZoom());
      asks::MenuItem mi = pane.menuItem(json{ { "id", "t1" }, { "kind", "claude" }, { "sessionId", S2 } });
      check("tile menu: its last answers", mi.has && mi.label == "Its last answers (read-only)  F7", mi.label);
      asks::MenuItem m2 = pane.menuItem(json{ { "id", "tx" }, { "kind", "claude" }, { "sessionId", S1 } });
      check("tile menu: answer its questions", m2.has && m2.label == "Answer its questions (5)  F7", m2.label);
    }, [] { return true; } });

  // A key the page does not type with, while the web view has the keyboard, must reach the app (F6 / F7 / F9 …).
  // Nothing here may take the real keyboard focus (and SendInput would type into whatever the user has in front
  // of them), so the keyboard is moved into the web view (openPane -> MoveFocus) and the key is POSTED to the
  // browser's own top window under this one (Chrome_WidgetWin_1, which then has this thread's focus). Found
  // 2026-11-18: that raises AcceleratorKeyPressed for real; a key posted to Chrome_RenderWidgetHostHWND, or a
  // DevTools key event, does not. What stays unproven is only a key from the physical keyboard.
  steps.push_back({ "a function key in the web view reaches the app (AcceleratorKeyPressed)", [&] {
      check("no accelerator reached the app by accident", app.accels.empty(), std::to_string(app.accels.size()));
      pane.openPane("all");
      mark = GetTickCount(); flag = false;
    }, [&] {
      if (GetTickCount() - mark < 700) return false;
      if (!flag) {
        flag = true; int n = 0;
        EnumChildWindows(app.hwnd, [](HWND c, LPARAM lp) -> BOOL {
          wchar_t cls[64] = L""; GetClassNameW(c, cls, 64);
          if (wcscmp(cls, L"Chrome_WidgetWin_1") == 0) { (*(int*)lp)++; PostMessageW(c, WM_KEYDOWN, VK_F7, 0x00410001); PostMessageW(c, WM_KEYUP, VK_F7, 0xC0410001); }
          return TRUE;
        }, (LPARAM)&n);
        logf("INFO  posted F7 to %d browser window(s) under the parked window", n);
        mark = GetTickCount() - 700;
        return false;
      }
      if (app.accels.empty() && GetTickCount() - mark < 2200) return false;
      check("AcceleratorKeyPressed fired for the posted F7 and handed it to the app's handler", !app.accels.empty() && app.accels[0].first == VK_F7, app.accels.empty() ? "nothing arrived" : "vk " + std::to_string(app.accels[0].first));
      check("the pane counted it", pane.acceleratorsSeen() >= 1 && pane.lastAccelerator() == VK_F7);
      pane.closePane();
      return true;
    } });

  auto run = [&](std::vector<Step>& list, asks::AskPane& pn) {
    size_t at = 0; bool started = false; DWORD stepT0 = GetTickCount(); const DWORD t0 = GetTickCount();
    MSG m;
    while (at < list.size()) {
      if (GetTickCount() - t0 > 100000) { check("the whole run in 100 s", false); break; }
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
      while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
      Step& s = list[at];
      if (!started) { logf("---- %s", s.name.c_str()); s.start(); started = true; stepT0 = GetTickCount(); }
      if (s.done()) { at++; started = false; continue; }
      if ((int)(GetTickCount() - stepT0) > s.timeoutMs) {
        check("step finished in time: " + s.name, false, "web state " + std::to_string((int)pn.web().state()) + " " + narrow(pn.web().error()) + " mode '" + pn.mode() + "' r1 " + r1.substr(0, 200) + " r2 " + r2.substr(0, 200));
        break;
      }
    }
  };
  run(steps, pane);
  const unsigned long pid = pane.web().browserPid();
  const bool clean = pane.shutdown(8000);
  check("shutdown: the browser process gone, the profile folder removed", clean, "pid " + std::to_string(pid));
  app.ws.stop();

  // ---- the in-process deck (no link): no /ask/ route to ask, so the page FILE is served from memory at a made-up
  // origin under the header lib/phone.js gives it. A second pane, a second web view, its own profile.
  if (argc >= 5) {
    asks::AskPane local; int draftGets = 0; std::vector<std::string> fired;
    asks::Deps d2;
    d2.call = [&](const std::string& fn, const json&, std::function<void(bool, const json&, const std::string&)> done) { if (fn == "askDraftGet") draftGets++; done(true, json(), ""); };
    d2.fire = [&](const std::string& fn, const json& a) { fired.push_back(fn + " " + a.dump()); };
    d2.note = [&](const std::string& t, int) { logf("note: %s", t.c_str()); };
    local.init(app.hwnd, d2);
    local.layout(app.mainRect(), app.scale);
    const std::string file = narrow(std::wstring(argv[4])) + "\\" + P1 + ".html";
    std::string url = "file:///"; for (char c : file) { if (c == '\\') url += '/'; else if (c == ' ') url += "%20"; else url += c; }
    json meta = { { "tiles", json::array() }, { "accounts", json::array() }, { "settings", json::object() }, { "askLog", json::array() },
      { "asks", json::array({ json{ { "sessionId", S1 }, { "name", "native-test" }, { "account", "claude" }, { "ended", false }, { "latest", nullptr },
        { "rounds", json::array({ json{ { "id", P1 }, { "round", 1 }, { "created", nullptr }, { "title", "" }, { "source", "picker" }, { "background", false }, { "status", "open" }, { "total", 5 }, { "answered", 0 }, { "later", 0 }, { "drafted", 0 }, { "sent", nullptr }, { "url", url }, { "replaces", json::array() } } }) } } }) } };
    local.setMeta(meta);
    std::vector<Step> more;
    more.push_back({ "local deck: the page file shows through the same pane", [&] { local.openPane(S1); }, [&] { return local.hostReady() && draftGets > 0; } });
    more.push_back({ "local deck: picture", [&] { mark = GetTickCount(); flag = false; r1.clear(); }, [&] {
        if (GetTickCount() - mark < 900) return false;
        if (r1.empty()) { static DWORD last = 0; if (GetTickCount() - last > 200) { last = GetTickCount(); local.web().exec("document.querySelector('.ap-rnote').textContent+'|'+location.origin+'|'+document.querySelector('iframe').getBoundingClientRect().height", [&](bool ok, const std::string& s) { r1 = ok ? s : "!"; }); } return false; }
        if (mark != 1) {
          mark = 1;
          check("local deck: the picker note, the made-up origin, the page sized", S(r1).rfind("From Claude Code's picker: it is also open in the session's terminal, and Done presses Esc there first|https://asks.claude-deck.invalid|", 0) == 0 && S(r1).substr(S(r1).rfind('|') + 1) != "320", r1);
          check("local deck: the page reported its state through the bridge", !fired.empty() && fired[0].rfind("askDraft ", 0) == 0, fired.empty() ? "" : fired[0].substr(0, 120));
          local.web().capturePng(app.outDir + L"\\asks-7-local.png", [&](bool ok) { check("picture asks-7-local.png (the web view alone)", ok); flag = true; });
        }
        return flag;
      } });
    run(more, local);
    check("local deck: shutdown clean", local.shutdown(8000));
  }
  DestroyWindow(app.hwnd);
  CoUninitialize();
  logf("%s  (%d failed)", gFails ? "FAILED" : "ALL PASSED", gFails);
  if (gLog) fclose(gLog);
  return gFails ? 1 : 0;
}
