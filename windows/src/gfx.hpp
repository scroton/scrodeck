// windows/src/gfx.hpp — the one drawing surface of the native shell: Direct2D + DirectWrite.
//
// Two targets behind one class: a window (ID2D1HwndRenderTarget) and an off-screen WIC bitmap that saves
// as a PNG. Every visual check in the tests uses the second, so nothing has to appear on the screen.
// Units are DIPs = CSS px, so numbers read out of renderer/style.css are used as they are.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>     // before windows.h (wsraw.hpp: the exe's own WebSocket client)
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <string>
#include <map>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdint>

namespace deck {

template <class T> struct Com {
  T* p = nullptr;
  Com() {}
  Com(const Com& o) : p(o.p) { if (p) p->AddRef(); }
  Com& operator=(const Com& o) { if (o.p) o.p->AddRef(); if (p) p->Release(); p = o.p; return *this; }
  ~Com() { if (p) p->Release(); }
  T* operator->() const { return p; }
  T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
  explicit operator bool() const { return p != nullptr; }
  void reset() { if (p) { p->Release(); p = nullptr; } }
};

struct Rgba {
  float r = 0, g = 0, b = 0, a = 1;
  Rgba() {}
  Rgba(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
  // 0xRRGGBB, the way the css writes them
  static Rgba hex(uint32_t rgb, float a = 1) { return Rgba(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
  Rgba alpha(float k) const { return Rgba(r, g, b, a * k); }
  static Rgba mix(const Rgba& x, const Rgba& y, float t) { return Rgba(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t); }
};

struct RectF {
  float x = 0, y = 0, w = 0, h = 0;
  RectF() {}
  RectF(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
  float r() const { return x + w; }
  float b() const { return y + h; }
  bool has(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  RectF inset(float d) const { return RectF(x + d, y + d, w - 2 * d, h - 2 * d); }
  RectF inset(float dx, float dy) const { return RectF(x + dx, y + dy, w - 2 * dx, h - 2 * dy); }
};

struct Font {
  std::wstring family = L"Segoe UI";
  float size = 13;
  int weight = 400;
  bool italic = false;
  bool operator<(const Font& o) const { return std::tie(family, size, weight, italic) < std::tie(o.family, o.size, o.weight, o.italic); }
};

enum class Align { Left, Center, Right };

inline std::wstring widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
inline std::string narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

class Gfx {
 public:
  Com<ID2D1Factory> d2d;
  Com<IDWriteFactory> dw;
  Com<IWICImagingFactory> wic;
  Com<ID2D1RenderTarget> rt;          // whichever target is current
  Com<ID2D1HwndRenderTarget> hwndRt;
  Com<IWICBitmap> wicBitmap;          // set when the target is off-screen
  Com<ID2D1SolidColorBrush> brush;
  std::map<Font, Com<IDWriteTextFormat>> formats;
  float width = 0, height = 0;        // DIPs
  float scale = 1;                    // device pixels per DIP of the current target (1.5 at 150 %)
  UINT forceDpi = 0;                  // > 0: a window target is drawn at this DPI whatever monitor it is on (tests)
  // a DIP coordinate moved to the nearest device pixel edge: what is laid out on these needs no anti-aliased edge,
  // and a part of the picture clipped to them can be repainted by itself, exactly (the scene, below)
  float snap(float v) const { return std::round(v * scale) / scale; }
  float snapDown(float v) const { return std::floor(v * scale + 0.001f) / scale; }
  float snapUp(float v) const { return std::ceil(v * scale - 0.001f) / scale; }

  bool init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)d2d.put()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw.put()))) return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
    return true;
  }

  // ---- targets
  // `immediate`: present without waiting for the display refresh (the bench: a frame must not read as one refresh
  // interval). The window itself keeps the default: one present per refresh at most.
  bool targetWindow(HWND hwnd, bool immediate = false) {
    RECT rc; GetClientRect(hwnd, &rc);
    UINT dpi = GetDpiForWindow(hwnd); if (!dpi) dpi = 96;
    if (forceDpi > 0) dpi = forceDpi;                   // tests: the window's picture as on a monitor at this scale
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), (float)dpi, (float)dpi);
    hwndRt.reset(); brush.reset(); dropScene();
    HRESULT hr = d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom), immediate ? D2D1_PRESENT_OPTIONS_IMMEDIATELY : D2D1_PRESENT_OPTIONS_NONE), hwndRt.put());
    if (FAILED(hr)) return false;
    rt.reset(); rt.p = hwndRt.p; rt.p->AddRef();
    wicBitmap.reset();
    afterTarget();
    return true;
  }
  void resizeWindow(UINT pxW, UINT pxH) {
    if (hwndRt) { dropScene(); hwndRt->Resize(D2D1::SizeU(pxW, pxH)); D2D1_SIZE_F s = rt->GetSize(); width = s.width; height = s.height; }
  }
  // Another window of the same process: the factories — and the bundled fonts read from their files — are the
  // PROCESS's, so a path geometry, a stroke style or a text format made for one window's target is good on every
  // other's (resources of two D2D factories do not mix: a foreign geometry puts the target in error for the frame).
  // Each window still has its own target, scene and caches.
  bool initShared(const Gfx& o) {
    d2d = o.d2d; dw = o.dw; wic = o.wic;
    privateFonts = o.privateFonts; privateFamilies = o.privateFamilies; privateDir = o.privateDir;
    return d2d && dw;
  }
  // an off-screen surface of w×h DIPs at `scale` (1 = 96 dpi)
  bool targetBitmap(int w, int h, float scale = 1) {
    if (!wic) return false;
    wicBitmap.reset(); brush.reset(); hwndRt.reset(); dropScene();
    if (FAILED(wic->CreateBitmap((UINT)(w * scale), (UINT)(h * scale), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, wicBitmap.put()))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    rt.reset();
    if (FAILED(d2d->CreateWicBitmapRenderTarget(wicBitmap.p, props, rt.put()))) return false;
    afterTarget();
    return true;
  }
  bool savePng(const std::wstring& path) {
    if (!wicBitmap) return false;
    Com<IWICStream> stream; Com<IWICBitmapEncoder> enc; Com<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put()))) return false;
    enc->Initialize(stream.p, WICBitmapEncoderNoCache);
    if (FAILED(enc->CreateNewFrame(frame.put(), nullptr))) return false;
    frame->Initialize(nullptr);
    UINT w, h; wicBitmap->GetSize(&w, &h);
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WriteSource(wicBitmap.p, nullptr))) return false;
    frame->Commit(); enc->Commit();
    return true;
  }

  // ---- the scene: the window's picture, kept between frames (docs/NATIVE-PORT.md §7 "Renderer rules")
  // A bitmap the size of the target that holds everything that is not an overlay: the panes and the sidebar. A
  // frame repaints into it only what changed (a pane's damaged rows, the sidebar when its inputs changed), each
  // under a pixel-aligned clip and with the very draw calls a whole frame makes, so the picture is the one a full
  // redraw gives; then it is copied to the target in one blit and the overlays are drawn over it. An idle frame
  // is that blit. Same coordinates as the target (no transform): the drawing code does not know which it is on.
  Com<ID2D1BitmapRenderTarget> sceneRt;
  Com<ID2D1SolidColorBrush> sceneBrush, realBrush;
  Com<ID2D1RenderTarget> realRt;
  bool sceneFresh = true;             // the scene holds nothing yet: the caller paints all of it
  bool sceneOff = false;              // tests: draw every frame whole, straight to the target (the comparison)
  void dropScene() { sceneRt.reset(); sceneBrush.reset(); sceneFresh = true; gradKept.clear(); }
  // true = drawing now goes to the scene (until sceneEnd). false = there is none: draw to the target as before.
  bool sceneBegin() {
    if (!rt || sceneOff || realRt) return false;
    const D2D1_SIZE_U px = rt->GetPixelSize();
    if (sceneRt) { const D2D1_SIZE_U s = sceneRt->GetPixelSize(); if (s.width != px.width || s.height != px.height) dropScene(); }
    if (!sceneRt) {
      const D2D1_SIZE_F dip = rt->GetSize();
      // the target's own pixel format (opaque for the window, premultiplied for the off-screen bitmap): text is
      // anti-aliased by the same rule on both, so a glyph in the scene is the glyph the target would have drawn
      if (FAILED(rt->CreateCompatibleRenderTarget(&dip, &px, nullptr, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, sceneRt.put()))) { sceneRt.reset(); return false; }
      if (FAILED(sceneRt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), sceneBrush.put()))) { dropScene(); return false; }
      sceneRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
      sceneFresh = true;
    }
    realRt = rt; realBrush = brush;
    rt.reset(); rt.p = sceneRt.p; rt.p->AddRef(); brush = sceneBrush;
    rt->BeginDraw(); rt->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
  }
  void sceneEnd() {
    if (!realRt) return;
    const HRESULT hr = rt->EndDraw();
    rt = realRt; brush = realBrush; realRt.reset(); realBrush.reset();
    sceneFresh = false;
    if (FAILED(hr)) dropScene();      // lost with the device: the next frame builds it again
  }
  // the scene onto the target, pixel for pixel
  void sceneBlit() {
    if (!sceneRt || realRt) return;
    Com<ID2D1Bitmap> bmp; if (FAILED(sceneRt->GetBitmap(bmp.put())) || !bmp) return;
    const D2D1_SIZE_F s = rt->GetSize(); const D2D1_RECT_F r = D2D1::RectF(0, 0, s.width, s.height);
    rt->DrawBitmap(bmp.p, r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, r);
  }

  void begin() { rt->BeginDraw(); rt->SetTransform(D2D1::Matrix3x2F::Identity()); }
  // false = the device was lost: the caller recreates the target
  bool end() { HRESULT hr = rt->EndDraw(); return hr != (HRESULT)D2DERR_RECREATE_TARGET; }
  void clear(const Rgba& c) { rt->Clear(D2D1::ColorF(c.r, c.g, c.b, c.a)); }

  // ---- shapes
  void fill(const RectF& r, const Rgba& c) { rt->FillRectangle(d(r), b(c)); }
  void fillRound(const RectF& r, float rad, const Rgba& c) { rt->FillRoundedRectangle(D2D1::RoundedRect(d(r), rad, rad), b(c)); }
  void stroke(const RectF& r, const Rgba& c, float wdt = 1) { RectF q = r.inset(wdt / 2); rt->DrawRectangle(d(q), b(c), wdt); }
  void strokeRound(const RectF& r, float rad, const Rgba& c, float wdt = 1) { RectF q = r.inset(wdt / 2); rt->DrawRoundedRectangle(D2D1::RoundedRect(d(q), rad, rad), b(c), wdt); }
  void line(float x1, float y1, float x2, float y2, const Rgba& c, float wdt = 1) { rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b(c), wdt); }
  void ellipse(float cx, float cy, float rx, float ry, const Rgba& c) { rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b(c)); }
  void ring(float cx, float cy, float rx, float ry, const Rgba& c, float wdt = 1) { rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b(c), wdt); }
  void pushClip(const RectF& r) { rt->PushAxisAlignedClip(d(r), D2D1_ANTIALIAS_MODE_ALIASED); }
  void popClip() { rt->PopAxisAlignedClip(); }
  // A linear gradient brush, kept: it depends on nothing but its stops, its two end points and the target it was
  // made on (a usage bar asks for the same one every frame — making it again was most of what a tile cost to draw).
  // Kept per target, dropped with the scene and with every new target (dropScene), so none outlives its device.
  std::map<ID2D1RenderTarget*, std::unordered_map<std::string, Com<ID2D1LinearGradientBrush>>> gradKept;
  ID2D1LinearGradientBrush* gradient(const std::vector<std::pair<float, Rgba>>& stops, float x0, float y0, float x1, float y1) {
    std::string k; k.reserve(stops.size() * 20 + 16);
    for (const auto& s : stops) { const float v[5] = { s.first, s.second.r, s.second.g, s.second.b, s.second.a }; k.append((const char*)v, sizeof v); }
    { const float v[4] = { x0, y0, x1, y1 }; k.append((const char*)v, sizeof v); }
    auto& m = gradKept[rt.p];
    auto it = m.find(k); if (it != m.end()) return it->second.p;
    if (m.size() > 3000) m.clear();
    std::vector<D2D1_GRADIENT_STOP> gs;
    for (auto& s : stops) { D2D1_GRADIENT_STOP g; g.position = s.first; g.color = D2D1::ColorF(s.second.r, s.second.g, s.second.b, s.second.a); gs.push_back(g); }
    Com<ID2D1GradientStopCollection> coll; Com<ID2D1LinearGradientBrush> lg;
    if (FAILED(rt->CreateGradientStopCollection(gs.data(), (UINT32)gs.size(), coll.put()))) return nullptr;
    if (FAILED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)), coll.p, lg.put()))) return nullptr;
    ID2D1LinearGradientBrush* p = lg.p; m.emplace(std::move(k), lg);
    return p;
  }
  // a horizontal gradient bar (the usage bars: green → red)
  void fillGradientH(const RectF& r, const std::vector<std::pair<float, Rgba>>& stops, float x0, float x1) {
    ID2D1LinearGradientBrush* lg = gradient(stops, x0, 0, x1, 0);
    if (lg) rt->FillRectangle(d(r), lg);
  }

  // ---- fonts that are FILES, not installed fonts: the deck's bundled console fonts (assets/fonts, which the
  // Electron renderer loads by @font-face). A private DirectWrite collection made from every .ttf / .otf under a
  // folder (one level of sub-folders, as assets/fonts is laid out); a family it holds is drawn from it, and wins
  // over an installed font of the same name, as an @font-face family does. Nothing is installed, nothing is
  // copied. Needs Windows 10 1703 or later (IDWriteFactory5); without it, or without the folder, nothing is
  // loaded and only installed fonts are found. Call before anything is drawn.
  Com<IDWriteFontCollection> privateFonts; std::vector<std::wstring> privateFamilies; std::wstring privateDir;
  static std::wstring lowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
  bool hasPrivate(const std::wstring& family) const { const std::wstring l = lowerW(family); for (const auto& f : privateFamilies) if (lowerW(f) == l) return true; return false; }
  int loadFontFolder(const std::wstring& dir) {
    privateFonts.reset(); privateFamilies.clear(); privateDir.clear(); formats.clear(); runCacheDrop();
    if (dir.empty() || !dw) return 0;
    Com<IDWriteFactory5> f5; if (FAILED(dw->QueryInterface(__uuidof(IDWriteFactory5), (void**)f5.put())) || !f5) return 0;
    Com<IDWriteFontSetBuilder1> b; if (FAILED(f5->CreateFontSetBuilder(b.put())) || !b) return 0;
    int files = 0;
    auto addIn = [&](const std::wstring& d) {
      WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE) return;
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName; const size_t dot = n.rfind(L'.'); if (dot == std::wstring::npos) continue;
        const std::wstring ext = lowerW(n.substr(dot));
        if (ext != L".ttf" && ext != L".otf" && ext != L".ttc") continue;
        Com<IDWriteFontFile> ff;
        if (SUCCEEDED(dw->CreateFontFileReference((d + L"\\" + n).c_str(), nullptr, ff.put())) && ff && SUCCEEDED(b->AddFontFile(ff.p))) files++;
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    };
    addIn(dir);
    { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) { do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') addIn(dir + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd)); FindClose(h); } }
    if (!files) return 0;
    return finishPrivateFonts(f5.p, b.p, dir);
  }
  // The same fonts from INSIDE the exe (windows/res/fonts.rc: RCDATA 2001..2099), through DirectWrite's in-memory
  // font file loader (IDWriteFactory5, Windows 10 1703+): no folder beside the exe is needed. 0 = none there.
  Com<IDWriteInMemoryFontFileLoader> memLoader;
  int loadFontResources(HMODULE mod) {
    privateFonts.reset(); privateFamilies.clear(); privateDir.clear(); formats.clear(); runCacheDrop();
    if (!dw) return 0;
    Com<IDWriteFactory5> f5; if (FAILED(dw->QueryInterface(__uuidof(IDWriteFactory5), (void**)f5.put())) || !f5) return 0;
    if (!memLoader) { if (FAILED(f5->CreateInMemoryFontFileLoader(memLoader.put())) || !memLoader || FAILED(dw->RegisterFontFileLoader(memLoader.p))) { memLoader.reset(); return 0; } }
    Com<IDWriteFontSetBuilder1> b; if (FAILED(f5->CreateFontSetBuilder(b.put())) || !b) return 0;
    int files = 0;
    for (int id = 2001; id < 2100; id++) {
      HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));   // RT_RCDATA
      if (!r) break;
      HGLOBAL hg = LoadResource(mod, r); const void* data = hg ? LockResource(hg) : nullptr; const DWORD size = SizeofResource(mod, r);
      if (!data || !size) continue;
      // the resource stays mapped as long as the module does: no owner object, no copy needed for its lifetime
      Com<IDWriteFontFile> ff;
      if (SUCCEEDED(memLoader->CreateInMemoryFontFileReference(dw.p, data, size, nullptr, ff.put())) && ff && SUCCEEDED(b->AddFontFile(ff.p))) files++;
    }
    if (!files) return 0;
    return finishPrivateFonts(f5.p, b.p, L"(inside the exe)");
  }
  int finishPrivateFonts(IDWriteFactory5* f5, IDWriteFontSetBuilder1* b, const std::wstring& dir) {
    Com<IDWriteFontSet> set; if (FAILED(b->CreateFontSet(set.put())) || !set) return 0;
    Com<IDWriteFontCollection1> c1; if (FAILED(f5->CreateFontCollectionFromFontSet(set.p, c1.put())) || !c1) return 0;
    const UINT32 n = c1->GetFontFamilyCount();
    for (UINT32 i = 0; i < n; i++) {
      Com<IDWriteFontFamily> fam; Com<IDWriteLocalizedStrings> names;
      if (FAILED(static_cast<IDWriteFontCollection*>(c1.p)->GetFontFamily(i, fam.put())) || !fam || FAILED(fam->GetFamilyNames(names.put())) || !names) continue;
      UINT32 idx = 0; BOOL exists = FALSE; names->FindLocaleName(L"en-us", &idx, &exists); if (!exists) idx = 0;
      UINT32 len = 0; if (FAILED(names->GetStringLength(idx, &len))) continue;
      std::wstring nm(len + 1, L'\0'); names->GetString(idx, &nm[0], len + 1); nm.resize(len);
      if (!nm.empty()) privateFamilies.push_back(nm);
    }
    privateFonts.p = c1.p; privateFonts.p->AddRef(); privateDir = dir;
    return (int)privateFamilies.size();
  }

  // ---- text
  IDWriteTextFormat* format(const Font& f) {
    auto it = formats.find(f);
    if (it != formats.end()) return it->second.p;
    Com<IDWriteTextFormat> tf;
    dw->CreateTextFormat(f.family.c_str(), privateFonts && hasPrivate(f.family) ? privateFonts.p : nullptr, (DWRITE_FONT_WEIGHT)f.weight,
      f.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, f.size, L"en-us", tf.put());
    if (tf) tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formats[f] = tf;
    return formats[f].p;
  }
  // One line, clipped to r, vertically centred; `ellipsis` trims with … like text-overflow: ellipsis.
  void text(const std::wstring& s, const RectF& r, const Font& f, const Rgba& c, Align al = Align::Left, bool ellipsis = true) {
    if (s.empty() || r.w <= 0 || r.h <= 0) return;
    Com<IDWriteTextLayout> lay = layout(s, f, r.w, r.h, false, ellipsis, 1);
    if (!lay) return;
    lay->SetTextAlignment(al == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : al == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt->DrawTextLayout(D2D1::Point2F(r.x, r.y), lay.p, b(c), (D2D1_DRAW_TEXT_OPTIONS)(D2D1_DRAW_TEXT_OPTIONS_CLIP | 4 /*ENABLE_COLOR_FONT*/));
  }
  // Wrapped text clamped to maxLines (the tile's message preview: -webkit-line-clamp). Returns the height used.
  float textWrapped(const std::wstring& s, const RectF& r, const Font& f, const Rgba& c, int maxLines, float lineHeight = 0, bool draw = true) {
    if (s.empty() || r.w <= 0) return 0;
    Com<IDWriteTextLayout> lay = layout(s, f, r.w, 100000, true, true, maxLines, lineHeight);
    if (!lay) return 0;
    DWRITE_TEXT_METRICS m; lay->GetMetrics(&m);
    float lh = lineHeight > 0 ? lineHeight : (m.lineCount ? m.height / m.lineCount : f.size * 1.3f);
    UINT32 lines = m.lineCount; if (maxLines > 0 && (int)lines > maxLines) lines = maxLines;
    float h = lh * lines;
    if (draw) {
      pushClip(RectF(r.x, r.y, r.w, h + 0.5f));
      rt->DrawTextLayout(D2D1::Point2F(r.x, r.y), lay.p, b(c), (D2D1_DRAW_TEXT_OPTIONS)4);
      popClip();
    }
    return h;
  }
  float textWidth(const std::wstring& s, const Font& f) {
    if (s.empty()) return 0;
    Com<IDWriteTextLayout> lay = layout(s, f, 100000, 1000, false, false, 1);
    if (!lay) return 0;
    DWRITE_TEXT_METRICS m; lay->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
  }
  // A terminal run's layout, kept: one line of one style, never wrapped or trimmed, so it depends on nothing but
  // its text and its font. A pane that repaints finds nearly every run of its screen here (a scroll moves lines,
  // it does not change them), and DirectWrite is not asked to lay anything out again (§7: "never call DirectWrite
  // layout during a frame for unchanged content"). Layouts are device-independent: both targets share them.
  // BOUNDED by use, not by count alone (2026-11-24, the home remote's memory: 68 -> 305 MB private in 2.5 h with 12
  // busy tiles). Streaming output makes new runs all the time (every transcript line, every tick of a spinner's
  // elapsed time and token count) at ~1.4 KB a layout, and the old rule kept up to 30,000 of them (40-60 MB), then
  // dropped them ALL in one frame, so every pane on screen was laid out again at once. Now two generations: a run
  // is made in runNew; when runNew holds kRunGen, the older generation is let go and runNew becomes it; a run found
  // in the older one moves back into the new. What the panes show is always kept (drawn within the last generation);
  // what scrolled away and was not drawn again goes. At most 2 x kRunGen layouts (~11 MB), at least one generation,
  // so a screen of 12 panes (~1,500-2,500 runs) is never laid out again because of the cache.
  static constexpr size_t kRunGen = 4096;
  std::unordered_map<std::wstring, Com<IDWriteTextLayout>> runNew, runOld;
  size_t runTurns = 0;                                    // generations let go so far (the soak's log)
  IDWriteTextLayout* runLayout(const std::wstring& s, const Font& f) {
    std::wstring key; key.reserve(s.size() + f.family.size() + 8);
    key.push_back((wchar_t)(1 + (int)(f.size * 16))); key.push_back((wchar_t)(1 + f.weight + (f.italic ? 2000 : 0))); key += f.family; key.push_back(L'\x1'); key += s;
    auto it = runNew.find(key);
    if (it != runNew.end()) return it->second.p;
    Com<IDWriteTextLayout> lay;
    auto old = runOld.find(key);
    if (old != runOld.end()) { lay = old->second; runOld.erase(old); }
    else lay = layout(s, f, 100000, 1000, false, false, 1);
    if (!lay) return nullptr;
    if (runNew.size() >= kRunGen) { runOld.swap(runNew); runNew.clear(); runTurns++; }
    IDWriteTextLayout* p = lay.p;
    runNew.emplace(std::move(key), lay);
    return p;
  }
  size_t runCacheSize() const { return runNew.size() + runOld.size(); }
  std::string runCacheNote() const { return "new " + std::to_string(runNew.size()) + " old " + std::to_string(runOld.size()) + ", " + std::to_string(runTurns) + " generations let go"; }
  void runCacheDrop() { runNew.clear(); runOld.clear(); }
  // raw: a layout the caller positions itself (the terminal grid)
  Com<IDWriteTextLayout> layout(const std::wstring& s, const Font& f, float w, float h, bool wrap, bool ellipsis, int maxLines, float lineHeight = 0) {
    Com<IDWriteTextLayout> lay;
    IDWriteTextFormat* tf = format(f);
    if (!tf) return lay;
    dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, w, h, lay.put());
    if (!lay) return lay;
    lay->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (lineHeight > 0) lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight, lineHeight * 0.8f);
    if (ellipsis) {
      Com<IDWriteInlineObject> sign;
      dw->CreateEllipsisTrimmingSign(tf, sign.put());
      DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
      lay->SetTrimming(&trim, sign.p);
    }
    (void)maxLines;
    return lay;
  }

  ID2D1SolidColorBrush* b(const Rgba& c) { brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a)); return brush.p; }
  static D2D1_RECT_F d(const RectF& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

 private:
  void afterTarget() {
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.put());
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    D2D1_SIZE_F s = rt->GetSize(); width = s.width; height = s.height;
    float dx = 96, dy = 96; rt->GetDpi(&dx, &dy); scale = dx > 0 ? dx / 96.f : 1;
  }
};

}  // namespace deck
