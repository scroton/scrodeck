// windows/web/webpane.cpp — see webpane.hpp. WebView2 through its COM interfaces, with small hand-written
// handler classes (no WRL, no ATL): it builds with MinGW-w64 g++ as well as MSVC.
#include "webpane.hpp"
#include <objbase.h>
#include <shlwapi.h>
#include <shellapi.h>
#include "../third_party/webview2/WebView2.h"
#include <atomic>
#include <cmath>
#include <map>
#include <vector>

namespace deck { namespace web {

namespace {

std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
std::string N(const wchar_t* w) {
  if (!w || !*w) return "";
  int len = (int)wcslen(w), n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr); return s;
}

template <class T> struct Ptr {
  T* p = nullptr;
  Ptr() {}
  Ptr(const Ptr& o) : p(o.p) { if (p) p->AddRef(); }
  Ptr& operator=(const Ptr& o) { if (o.p) o.p->AddRef(); if (p) p->Release(); p = o.p; return *this; }
  ~Ptr() { if (p) p->Release(); }
  T* operator->() const { return p; }
  T** put() { reset(); return &p; }
  explicit operator bool() const { return p != nullptr; }
  void reset() { if (p) { p->Release(); p = nullptr; } }
  template <class U> Ptr<U> as(const IID& iid) const { Ptr<U> o; if (p) p->QueryInterface(iid, (void**)o.put()); return o; }
};

// One COM callback object: IUnknown plus the one Invoke of a WebView2 handler interface.
template <class I, class... A>
class Cb final : public I {
 public:
  typedef std::function<HRESULT(A...)> Fn;
  Cb(const IID& iid, Fn fn) : iid_(iid), fn_(std::move(fn)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) { *out = static_cast<I*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++rc_; }
  ULONG STDMETHODCALLTYPE Release() override { ULONG n = --rc_; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return fn_ ? fn_(a...) : S_OK; }
 private:
  virtual ~Cb() {}
  const IID& iid_; Fn fn_; std::atomic<ULONG> rc_{1};
};
// make<I, args…>(iid, fn): a handler with one reference, held in a Ptr (the web view takes its own)
template <class I, class... A> Ptr<I> make(const IID& iid, typename Cb<I, A...>::Fn fn) { Ptr<I> p; p.p = new Cb<I, A...>(iid, std::move(fn)); return p; }

// ICoreWebView2EnvironmentOptions, the four properties of the base interface
class EnvOptions final : public ICoreWebView2EnvironmentOptions {
 public:
  std::wstring args, lang, target = L"100.0.1185.36";   // the oldest runtime this code is written against (InPrivate options)
  BOOL sso = FALSE;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions)) { *out = static_cast<ICoreWebView2EnvironmentOptions*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++rc_; }
  ULONG STDMETHODCALLTYPE Release() override { ULONG n = --rc_; if (!n) delete this; return n; }
  static HRESULT dup(const std::wstring& s, LPWSTR* out) {
    if (!out) return E_POINTER;
    if (s.empty()) { *out = nullptr; return S_OK; }
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    *out = (LPWSTR)CoTaskMemAlloc(bytes); if (!*out) return E_OUTOFMEMORY;
    memcpy(*out, s.c_str(), bytes); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* v) override { return dup(args, v); }
  HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR v) override { args = v ? v : L""; return S_OK; }
  HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* v) override { return dup(lang, v); }
  HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR v) override { lang = v ? v : L""; return S_OK; }
  HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* v) override { return dup(target, v); }
  HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR v) override { target = v ? v : L""; return S_OK; }
  HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* v) override { if (!v) return E_POINTER; *v = sso; return S_OK; }
  HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL v) override { sso = v; return S_OK; }
 private:
  virtual ~EnvOptions() {}
  std::atomic<ULONG> rc_{1};
};

const wchar_t* kMarker = L".claude-deck-webpane";   // only a folder holding this file is ever removed

// ---- the browser's command line, ONE for every web view of this process.
// Every WebView2 environment on one user-data folder must be made with the same options, or the second one fails
// (HRESULT_FROM_WIN32(ERROR_INVALID_STATE)): the ask pane and the cloud panes share one folder and so one browser
// process tree, which is the point (WEB-NOTES.md "One browser process tree"). So the arguments do not depend on
// what the view is for. WEB-NOTES.md has the table: each flag, what it does, the measured effect.
std::wstring& argsOverride() { static std::wstring s; return s; }
bool& argsOverridden() { static bool b = false; return b; }
const wchar_t* kDefaultArgs =
  // nothing that could put a window up, or ask anything, on a first run
  L"--no-first-run --no-default-browser-check"
  // msSmartScreenProtection: no reputation look-up of every address (a pane can only ever be at the deck's own
  // origin, or at claude.ai / Anthropic / a sign-in page: cloud.hpp paneUrl refuses the rest before it loads)
  // msEdgeTranslate: no translate bar.
  L" --disable-features=msSmartScreenProtection,msEdgeTranslate";

std::wstring tempBase() {
  wchar_t t[MAX_PATH + 2] = L""; GetTempPathW(MAX_PATH, t);
  std::wstring s = t; if (!s.empty() && s.back() != L'\\') s += L'\\';
  return s + L"ClaudeDeckNative-web";
}
bool isDir(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
bool isFile(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
// Everything under `dir`, then `dir`. Never follows a junction or a link (it removes the link itself).
bool removeTree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      const std::wstring p = dir + L"\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(p.c_str()); else removeTree(p);
      } else {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return RemoveDirectoryW(dir.c_str()) != 0 || !isDir(dir);
}
bool removeProfile(const std::wstring& dir) {
  if (dir.empty() || !isDir(dir)) return true;
  if (!isFile(dir + L"\\" + kMarker)) return false;     // not one of ours: left alone
  for (int i = 0; i < 20; i++) { if (removeTree(dir)) return true; Sleep(100); }   // the browser lets go of its files a moment after it exits
  return !isDir(dir);
}
bool pidAlive(DWORD pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
  const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h); return alive;
}

}  // namespace

struct WebPane::Impl : std::enable_shared_from_this<WebPane::Impl> {
  WebPane* owner = nullptr;
  HWND parent = nullptr;
  WebPaneOptions opts;
  RuntimeInfo rt;
  WebState state = WebState::None;
  std::wstring error;
  std::wstring userDir; bool ownDir = false;
  Rect dips; float scale = 1; bool wantVisible = true;
  Ptr<ICoreWebView2Environment> env;
  Ptr<ICoreWebView2Controller> controller;
  Ptr<ICoreWebView2> view;
  std::vector<std::function<void()>> pending;          // asked before the web view existed
  std::map<std::wstring, WebResponse> served;
  std::vector<std::wstring> allowedTop;                // what the host itself navigated to
  std::vector<Ptr<ICoreWebView2Frame>> frames;
  DWORD pid = 0; ULONGLONG closedAt = 0;               // the browser process, and when this object's last live view closed
  bool navRedirected = false, isSuspended = false;
  float rasterScale = 0;                               // > 0: asked for (setRasterScale)
  int memLevel = -1;                                   // the memory target the runtime says it has: 0 normal, 1 low, -1 not known / no ICoreWebView2_19

  // 2026-11-23: a view nobody can see asks its renderer for LESS memory (COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW:
  // the runtime trims what it can - caches, decoded images - and keeps the page as it is: its script, its scroll, its
  // sign-in), and for the normal amount again when it shows. A runtime older than ICoreWebView2_19 has no such thing:
  // skipped quietly.
  void applyMemoryTarget() {
    if (!view) return;
    auto v19 = view.as<ICoreWebView2_19>(IID_ICoreWebView2_19);
    if (!v19) { memLevel = -1; return; }
    v19->put_MemoryUsageTargetLevel(wantVisible ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL now = COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL;
    memLevel = SUCCEEDED(v19->get_MemoryUsageTargetLevel(&now)) ? (now == COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW ? 1 : 0) : -1;
  }

  void fail(WebState s, const std::wstring& why) {
    state = s; error = why;
    if (owner && owner->onReady) owner->onReady(false, why);
  }
  static std::wstring hrText(const wchar_t* what, HRESULT hr) { wchar_t b[160]; swprintf(b, 160, L"%ls (0x%08lX)", what, (unsigned long)hr); return b; }

  RECT px() const {
    RECT r;
    r.left = (LONG)std::lround(dips.x * scale); r.top = (LONG)std::lround(dips.y * scale);
    r.right = (LONG)std::lround((dips.x + dips.w) * scale); r.bottom = (LONG)std::lround((dips.y + dips.h) * scale);
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
  }
  // setRasterScale: bounds stay raw device pixels (px() already multiplies by `scale`), the page is drawn at rasterScale
  void applyRaster() {
    if (rasterScale <= 0 || !controller) return;
    if (auto c3 = controller.as<ICoreWebView2Controller3>(IID_ICoreWebView2Controller3)) {
      c3->put_ShouldDetectMonitorScaleChanges(FALSE);
      c3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
      c3->put_RasterizationScale(rasterScale);
    }
  }
  void run(std::function<void()> f) { if (state == WebState::Ready) f(); else if (state == WebState::Creating) pending.push_back(std::move(f)); }

  bool start() {
    rt = findRuntime(opts.fixedRuntimeDir);
    if (!rt.found) { state = WebState::NoRuntime; error = std::wstring(L"the WebView2 runtime is not installed: ") + runtimeDownloadName(); return false; }
    userDir = opts.userDataDir;
    const bool invented = userDir.empty();              // a folder of this class's own making: the only kind it marks, and so the only kind it ever removes
    if (userDir.empty()) {
      const std::wstring base = tempBase();
      CreateDirectoryW(base.c_str(), nullptr);
      userDir = base + L"\\" + std::to_wstring(GetCurrentProcessId());
      for (int i = 1; isDir(userDir) && i < 100; i++) userDir = base + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
    }
    if (!isDir(userDir)) {
      if (!CreateDirectoryW(userDir.c_str(), nullptr)) { state = WebState::Failed; error = L"cannot make the web view's profile folder: " + userDir; return false; }
      HANDLE h = !invented ? INVALID_HANDLE_VALUE : CreateFileW((userDir + L"\\" + kMarker).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) { char pidText[32]; int n = snprintf(pidText, sizeof pidText, "%lu", (unsigned long)GetCurrentProcessId()); DWORD wr; WriteFile(h, pidText, n, &wr, nullptr); CloseHandle(h); }
    }
    ownDir = isFile(userDir + L"\\" + kMarker);
    isSuspended = false; navRedirected = false;
    state = WebState::Creating;
    std::weak_ptr<Impl> weak = shared_from_this();
    EnvOptions* eo = new EnvOptions();
    eo->args = WebPane::browserArguments();            // the same for every view: see kDefaultArgs
    auto done = make<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
      IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, [weak](HRESULT hr, ICoreWebView2Environment* e) -> HRESULT {
        auto self = weak.lock(); if (!self || self->state != WebState::Creating) return S_OK;
        if (FAILED(hr) || !e) { self->fail(WebState::Failed, hrText(L"the WebView2 environment was not created", hr)); return S_OK; }
        self->env.p = e; e->AddRef();
        self->makeController();
        return S_OK;
      });
    const HRESULT hr = createEnvironment(rt, userDir, eo, done.p);
    eo->Release();
    if (FAILED(hr)) { state = WebState::Failed; error = hrText(L"the WebView2 runtime did not start", hr); return false; }
    return true;
  }

  void makeController() {
    std::weak_ptr<Impl> weak = shared_from_this();
    auto done = make<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
      IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, [weak](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
        auto self = weak.lock();
        if (!self || self->state != WebState::Creating) { if (c) c->Close(); return S_OK; }
        if (FAILED(hr) || !c) { self->fail(WebState::Failed, hrText(L"the web view was not created", hr)); return S_OK; }
        self->controller.p = c; c->AddRef();
        self->setup();
        return S_OK;
      });
    HRESULT hr = E_NOINTERFACE;
    if (opts.inPrivate) {
      auto env10 = env.as<ICoreWebView2Environment10>(IID_ICoreWebView2Environment10);
      Ptr<ICoreWebView2ControllerOptions> co;
      // InPrivate, in a profile of its own name when one is given (the ask pane inside the shared folder: its
      // in-memory profile hangs off `profileName`, never off a profile a cloud pane signs in to)
      if (env10 && SUCCEEDED(env10->CreateCoreWebView2ControllerOptions(co.put())) && co && SUCCEEDED(co->put_IsInPrivateModeEnabled(TRUE))
          && (opts.profileName.empty() || SUCCEEDED(co->put_ProfileName(opts.profileName.c_str())))) {
        hr = env10->CreateCoreWebView2ControllerWithOptions(parent, co.p, done.p);
      } else {
        // A runtime too old for an InPrivate profile (before 101): refuse rather than put the deck's cookie on disk.
        fail(WebState::Failed, L"this WebView2 runtime is too old for a private profile; update the " + std::wstring(runtimeDownloadName()));
        return;
      }
    } else if (!opts.profileName.empty()) {
      auto env10 = env.as<ICoreWebView2Environment10>(IID_ICoreWebView2Environment10);
      Ptr<ICoreWebView2ControllerOptions> co;
      if (env10 && SUCCEEDED(env10->CreateCoreWebView2ControllerOptions(co.put())) && co && SUCCEEDED(co->put_ProfileName(opts.profileName.c_str())))
        hr = env10->CreateCoreWebView2ControllerWithOptions(parent, co.p, done.p);
      else {
        // too old for named profiles (before 101): refuse rather than mix every account's sign-in in one profile
        fail(WebState::Failed, L"this WebView2 runtime is too old for a profile per account; update the " + std::wstring(runtimeDownloadName()));
        return;
      }
    } else hr = env->CreateCoreWebView2Controller(parent, done.p);
    if (FAILED(hr)) fail(WebState::Failed, hrText(L"the web view was not started", hr));
  }

  void setup() {
    std::weak_ptr<Impl> weak = shared_from_this();
    // the colour under the page, before the first paint
    if (auto c2 = controller.as<ICoreWebView2Controller2>(IID_ICoreWebView2Controller2)) {
      COREWEBVIEW2_COLOR col; col.A = 255; col.R = (BYTE)(opts.background >> 16); col.G = (BYTE)(opts.background >> 8); col.B = (BYTE)opts.background;
      c2->put_DefaultBackgroundColor(col);
    }
    controller->get_CoreWebView2(view.put());
    if (!view) { fail(WebState::Failed, L"the web view has no page object"); return; }
    { UINT32 p = 0; view->get_BrowserProcessId(&p); pid = p; }
    Ptr<ICoreWebView2Settings> s; view->get_Settings(s.put());
    if (s) {
      s->put_AreDevToolsEnabled(FALSE);
      s->put_AreDefaultContextMenusEnabled(opts.site ? TRUE : FALSE);
      s->put_IsStatusBarEnabled(FALSE);
      s->put_IsZoomControlEnabled(FALSE);
      s->put_AreDefaultScriptDialogsEnabled(FALSE);
      s->put_IsBuiltInErrorPageEnabled(opts.site ? TRUE : FALSE);
      s->put_AreHostObjectsAllowed(FALSE);
      s->put_IsWebMessageEnabled(TRUE);
      s->put_IsScriptEnabled(TRUE);
      if (auto s3 = s.as<ICoreWebView2Settings3>(IID_ICoreWebView2Settings3)) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
      if (auto s4 = s.as<ICoreWebView2Settings4>(IID_ICoreWebView2Settings4)) { s4->put_IsPasswordAutosaveEnabled(opts.site ? TRUE : FALSE); s4->put_IsGeneralAutofillEnabled(opts.site ? TRUE : FALSE); }
      if (auto s5 = s.as<ICoreWebView2Settings5>(IID_ICoreWebView2Settings5)) s5->put_IsPinchZoomEnabled(FALSE);
      if (auto s6 = s.as<ICoreWebView2Settings6>(IID_ICoreWebView2Settings6)) s6->put_IsSwipeNavigationEnabled(FALSE);
    }
    EventRegistrationToken tok;
    view->add_WebMessageReceived(make<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
      IID_ICoreWebView2WebMessageReceivedEventHandler, [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
        auto self = weak.lock(); if (!self || !self->owner || !self->owner->onMessage) return S_OK;
        LPWSTR json = nullptr;
        if (SUCCEEDED(a->get_WebMessageAsJson(&json)) && json) { std::string s = N(json); CoTaskMemFree(json); self->owner->onMessage(s); }
        return S_OK;
      }).p, &tok);
    auto nav = [weak](bool main) {
      return [weak, main](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
        auto self = weak.lock(); if (!self) return S_OK;
        LPWSTR uri = nullptr; a->get_Uri(&uri);
        const std::wstring url = uri ? uri : L""; if (uri) CoTaskMemFree(uri);
        BOOL redir = FALSE; a->get_IsRedirected(&redir); self->navRedirected = !!redir;
        bool ok = url == L"about:blank" || url == L"about:srcdoc";
        if (!ok && main) for (auto& u : self->allowedTop) if (u == url || (u == L"data:" && url.rfind(L"data:text/html", 0) == 0)) ok = true;
        if (!ok && self->owner && self->owner->onNavigate) ok = self->owner->onNavigate(url, main);
        self->navRedirected = false;
        if (!ok) a->put_Cancel(TRUE);
        return S_OK;
      };
    };
    view->add_NavigationStarting(make<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(IID_ICoreWebView2NavigationStartingEventHandler, nav(true)).p, &tok);
    view->add_FrameNavigationStarting(make<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(IID_ICoreWebView2NavigationStartingEventHandler, nav(false)).p, &tok);
    view->add_NavigationCompleted(make<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
      IID_ICoreWebView2NavigationCompletedEventHandler, [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
        auto self = weak.lock(); if (!self || !self->owner || !self->owner->onLoaded) return S_OK;
        BOOL ok = FALSE; a->get_IsSuccess(&ok); self->owner->onLoaded(!!ok); return S_OK;
      }).p, &tok);
    view->add_NewWindowRequested(make<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
      IID_ICoreWebView2NewWindowRequestedEventHandler, [weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
        auto self = weak.lock(); if (!self) { a->put_Handled(TRUE); return S_OK; }
        LPWSTR uri = nullptr; a->get_Uri(&uri);
        const std::wstring url = uri ? uri : L""; if (uri) CoTaskMemFree(uri);
        // a popup the host lets through (a sign-in window that talks to its opener) is the runtime's own window, in
        // this profile; anything else is never a window of the browser's
        if (self->owner && self->owner->onPopup && self->owner->onPopup(url)) return S_OK;
        a->put_Handled(TRUE);
        if (self->owner && self->owner->onNewWindow) self->owner->onNewWindow(url);
        return S_OK;
      }).p, &tok);
    view->add_WebResourceRequested(make<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(
      IID_ICoreWebView2WebResourceRequestedEventHandler, [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT {
        auto self = weak.lock(); if (!self) return S_OK;
        Ptr<ICoreWebView2WebResourceRequest> req; a->get_Request(req.put()); if (!req) return S_OK;
        LPWSTR uri = nullptr; req->get_Uri(&uri);
        const std::wstring url = uri ? uri : L""; if (uri) CoTaskMemFree(uri);
        WebResponse r; bool have = false;
        auto it = self->served.find(url);
        if (it != self->served.end()) { r = it->second; have = true; }
        else if (self->owner && self->owner->onResource) have = self->owner->onResource(url, r);
        if (!have) return S_OK;
        Ptr<IStream> body; body.p = SHCreateMemStream((const BYTE*)r.body.data(), (UINT)r.body.size());
        Ptr<ICoreWebView2WebResourceResponse> resp;
        const wchar_t* reason = r.status == 200 ? L"OK" : r.status == 404 ? L"Not Found" : r.status == 403 ? L"Forbidden" : L"";
        if (SUCCEEDED(self->env->CreateWebResourceResponse(body.p, r.status, reason, r.headers.c_str(), resp.put())) && resp) a->put_Response(resp.p);
        return S_OK;
      }).p, &tok);
    view->add_PermissionRequested(make<ICoreWebView2PermissionRequestedEventHandler, ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs*>(
      IID_ICoreWebView2PermissionRequestedEventHandler, [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* a) -> HRESULT {
        // the clipboard is the one thing an ask page asks for (its "Copy" buttons): writing needs no permission,
        // reading is allowed as the Electron frame allows it (allow="clipboard-read"); the rest is refused, unasked
        COREWEBVIEW2_PERMISSION_KIND k = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION; a->get_PermissionKind(&k);
        a->put_State(k == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY);
        return S_OK;
      }).p, &tok);
    view->add_SourceChanged(make<ICoreWebView2SourceChangedEventHandler, ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*>(
      IID_ICoreWebView2SourceChangedEventHandler, [weak](ICoreWebView2* v, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
        auto self = weak.lock(); if (!self || !self->owner || !self->owner->onUrl) return S_OK;
        LPWSTR uri = nullptr; v->get_Source(&uri);
        const std::wstring url = uri ? uri : L""; if (uri) CoTaskMemFree(uri);
        self->owner->onUrl(url);
        return S_OK;
      }).p, &tok);
    if (auto v4 = view.as<ICoreWebView2_4>(IID_ICoreWebView2_4)) {
      const bool site = opts.site;
      v4->add_DownloadStarting(make<ICoreWebView2DownloadStartingEventHandler, ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs*>(
        IID_ICoreWebView2DownloadStartingEventHandler, [site, weak](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* a) -> HRESULT {
          if (site) return S_OK;                         // a web site: the browser's own download handling
          // a page of ours: the owner says where (onDownload), with no download bar; nothing said = cancelled
          std::wstring to;
          auto self = weak.lock();
          if (self && self->owner && self->owner->onDownload) {
            LPWSTR suggested = nullptr; a->get_ResultFilePath(&suggested);
            std::wstring full = suggested ? suggested : L""; if (suggested) CoTaskMemFree(suggested);
            const size_t sl = full.find_last_of(L"\\/");
            to = self->owner->onDownload(sl == std::wstring::npos ? full : full.substr(sl + 1));
          }
          if (to.empty()) a->put_Cancel(TRUE);
          else a->put_ResultFilePath(to.c_str());
          a->put_Handled(TRUE);
          return S_OK;
        }).p, &tok);
      v4->add_FrameCreated(make<ICoreWebView2FrameCreatedEventHandler, ICoreWebView2*, ICoreWebView2FrameCreatedEventArgs*>(
        IID_ICoreWebView2FrameCreatedEventHandler, [weak](ICoreWebView2*, ICoreWebView2FrameCreatedEventArgs* a) -> HRESULT {
          auto self = weak.lock(); if (!self) return S_OK;
          Ptr<ICoreWebView2Frame> f; a->get_Frame(f.put()); if (!f) return S_OK;
          self->frames.push_back(f);
          EventRegistrationToken t2;
          f->add_Destroyed(make<ICoreWebView2FrameDestroyedEventHandler, ICoreWebView2Frame*, IUnknown*>(
            IID_ICoreWebView2FrameDestroyedEventHandler, [weak](ICoreWebView2Frame* gone, IUnknown*) -> HRESULT {
              auto s2 = weak.lock(); if (!s2) return S_OK;
              for (size_t i = 0; i < s2->frames.size(); i++) if (s2->frames[i].p == gone) { s2->frames.erase(s2->frames.begin() + i); break; }
              return S_OK;
            }).p, &t2);
          return S_OK;
        }).p, &tok);
    }
    controller->add_AcceleratorKeyPressed(make<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs*>(
      IID_ICoreWebView2AcceleratorKeyPressedEventHandler, [weak](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a) -> HRESULT {
        auto self = weak.lock(); if (!self || !self->owner || !self->owner->onAccelerator) return S_OK;
        COREWEBVIEW2_KEY_EVENT_KIND kind; a->get_KeyEventKind(&kind);
        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
        UINT vk = 0; a->get_VirtualKey(&vk);
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return S_OK;
        // the keys' real state: the keyboard is in the browser's process, this thread's own key state does not follow it
        const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0, shift = GetAsyncKeyState(VK_SHIFT) < 0, alt = GetAsyncKeyState(VK_MENU) < 0;
        if (self->owner->onAccelerator(vk, ctrl, shift, alt)) a->put_Handled(TRUE);
        return S_OK;
      }).p, &tok);
    auto foc = [weak](bool has) {
      return [weak, has](ICoreWebView2Controller*, IUnknown*) -> HRESULT { auto self = weak.lock(); if (self && self->owner && self->owner->onFocus) self->owner->onFocus(has); return S_OK; };
    };
    controller->add_GotFocus(make<ICoreWebView2FocusChangedEventHandler, ICoreWebView2Controller*, IUnknown*>(IID_ICoreWebView2FocusChangedEventHandler, foc(true)).p, &tok);
    controller->add_LostFocus(make<ICoreWebView2FocusChangedEventHandler, ICoreWebView2Controller*, IUnknown*>(IID_ICoreWebView2FocusChangedEventHandler, foc(false)).p, &tok);

    applyRaster();
    controller->put_Bounds(px());
    controller->put_IsVisible(wantVisible ? TRUE : FALSE);
    applyMemoryTarget();                                 // (made hidden, as a warmed view is: low from the start)
    state = WebState::Ready;
    std::vector<std::function<void()>> todo; todo.swap(pending);
    for (auto& f : todo) f();
    if (owner && owner->onReady) owner->onReady(true, L"");
  }
};

WebPane::WebPane() : d_(std::make_shared<Impl>()) { d_->owner = this; }
WebPane::~WebPane() { destroy(0); d_->owner = nullptr; }

bool WebPane::create(HWND parent, const Rect& dips, float scale, const WebPaneOptions& opts) {
  if (d_->state == WebState::Creating || d_->state == WebState::Ready) return true;
  d_->parent = parent; d_->dips = dips; d_->scale = scale > 0 ? scale : 1; d_->opts = opts;
  d_->error.clear();
  return d_->start();
}
WebState WebPane::state() const { return d_->state; }
const std::wstring& WebPane::error() const { return d_->error; }
const RuntimeInfo& WebPane::runtime() const { return d_->rt; }
unsigned long WebPane::browserPid() const { return d_->pid; }

bool WebPane::destroy(unsigned waitMs) {
  Impl& d = *d_;
  const bool had = d.state == WebState::Ready || d.state == WebState::Creating;
  d.pending.clear(); d.frames.clear();
  // what belonged to the view that is going: a view made again on this object starts clean (its filters are per
  // view: a document served to the old one must be registered again for the new one)
  d.served.clear(); d.allowedTop.clear(); d.isSuspended = false; d.navRedirected = false;
  if (d.controller) d.controller->Close();
  d.view.reset(); d.controller.reset(); d.env.reset();
  if (had) { d.state = WebState::Closed; d.closedAt = GetTickCount64(); }
  bool clean = true;
  // (only a process a view of this object had until a moment ago: one closed long ago has gone by itself, and its
  // process id may be another program's by now)
  if (waitMs && d.pid && GetTickCount64() - d.closedAt < 10000) {
    // the browser process goes a moment after its last web view closes; its messages need this thread's loop
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, d.pid);
    const DWORD until = GetTickCount() + waitMs;
    while (h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT && (int)(until - GetTickCount()) > 0) {
      MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
      MsgWaitForMultipleObjects(1, &h, FALSE, 50, QS_ALLINPUT);
    }
    if (h) { clean = WaitForSingleObject(h, 0) != WAIT_TIMEOUT; CloseHandle(h); }
  }
  if (waitMs && d.opts.removeUserDataOnClose && d.ownDir && !d.userDir.empty()) {
    if (!removeProfile(d.userDir)) clean = false;
    else d.userDir.clear();
  }
  if (waitMs) d.pid = 0;
  return clean;
}

void WebPane::sweepStale() {
  const std::wstring base = tempBase();
  WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
    const DWORD pid = wcstoul(fd.cFileName, nullptr, 10);
    if (!pid || pid == GetCurrentProcessId() || pidAlive(pid)) continue;
    removeProfile(base + L"\\" + fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

void WebPane::setBounds(const Rect& dips, float scale) {
  d_->dips = dips; if (scale > 0) d_->scale = scale;
  if (d_->controller) d_->controller->put_Bounds(d_->px());
}
void WebPane::show(bool on) {
  d_->wantVisible = on;
  if (on && d_->isSuspended) resume();                 // (showing a suspended view resumes it anyway; said outright)
  if (d_->controller) d_->controller->put_IsVisible(on ? TRUE : FALSE);
  d_->applyMemoryTarget();                             // hidden: the low memory target; shown: the normal one
}
bool WebPane::visible() const { return d_->wantVisible; }
int WebPane::memoryTarget() const { return d_->memLevel; }
void WebPane::focus() { auto d = d_; d->run([d] { if (d->controller) d->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }); }
void WebPane::setRasterScale(float scale) { d_->rasterScale = scale; d_->applyRaster(); }
void WebPane::suspend() {
  auto d = d_;
  d->run([d] {
    if (d->isSuspended || !d->view) return;
    auto v3 = d->view.as<ICoreWebView2_3>(IID_ICoreWebView2_3); if (!v3) return;
    BOOL vis = TRUE; d->controller->get_IsVisible(&vis); if (vis) return;        // only a hidden view may be suspended
    d->isSuspended = true;
    std::weak_ptr<Impl> weak = d;
    const HRESULT hr = v3->TrySuspend(make<ICoreWebView2TrySuspendCompletedHandler, HRESULT, BOOL>(IID_ICoreWebView2TrySuspendCompletedHandler, [weak](HRESULT r, BOOL ok) -> HRESULT {
      auto self = weak.lock(); if (self && (FAILED(r) || !ok)) self->isSuspended = false; return S_OK; }).p);
    if (FAILED(hr)) d->isSuspended = false;
  });
}
void WebPane::resume() {
  auto d = d_;
  d->run([d] { if (!d->isSuspended || !d->view) return; d->isSuspended = false; if (auto v3 = d->view.as<ICoreWebView2_3>(IID_ICoreWebView2_3)) v3->Resume(); });
}
bool WebPane::suspended() const { return d_->isSuspended; }
void WebPane::notifyParentMoved() { if (d_->controller) d_->controller->NotifyParentWindowPositionChanged(); }

void WebPane::navigate(const std::wstring& url) {
  auto d = d_;
  d->run([d, url] { d->allowedTop.push_back(url); if (d->view) d->view->Navigate(url.c_str()); });
}
void WebPane::reload() { auto d = d_; d->run([d] { if (d->view) d->view->Reload(); }); }
std::wstring WebPane::url() const {
  if (!d_->view) return L"";
  LPWSTR uri = nullptr; d_->view->get_Source(&uri);
  std::wstring u = uri ? uri : L""; if (uri) CoTaskMemFree(uri);
  return u == L"about:blank" ? L"" : u;
}
bool WebPane::navRedirected() const { return d_->navRedirected; }
const std::wstring& WebPane::userDataDir() const { return d_->userDir; }
bool WebPane::ownsUserData() const { return d_->ownDir; }
std::wstring WebPane::browserArguments() { return argsOverridden() ? argsOverride() : std::wstring(kDefaultArgs); }
void WebPane::setBrowserArguments(const std::wstring& a) { argsOverride() = a; argsOverridden() = true; }
bool WebPane::privateMode() const {
  if (!d_->view) return false;
  auto v13 = d_->view.as<ICoreWebView2_13>(IID_ICoreWebView2_13); if (!v13) return false;
  Ptr<ICoreWebView2Profile> p; if (FAILED(v13->get_Profile(p.put())) || !p) return false;
  BOOL priv = FALSE; p->get_IsInPrivateModeEnabled(&priv); return !!priv;
}
std::wstring WebPane::profilePath() const {
  if (!d_->view) return L"";
  auto v13 = d_->view.as<ICoreWebView2_13>(IID_ICoreWebView2_13); if (!v13) return L"";
  Ptr<ICoreWebView2Profile> p; if (FAILED(v13->get_Profile(p.put())) || !p) return L"";
  LPWSTR s = nullptr; p->get_ProfilePath(&s);
  std::wstring o = s ? s : L""; if (s) CoTaskMemFree(s); return o;
}
void WebPane::clearCookies() {
  if (!d_->view) return;
  auto v2 = d_->view.as<ICoreWebView2_2>(IID_ICoreWebView2_2); if (!v2) return;
  Ptr<ICoreWebView2CookieManager> cm; v2->get_CookieManager(cm.put());
  if (cm) cm->DeleteAllCookies();
}
bool WebPane::waitProcessGone(unsigned long pid, unsigned waitMs) {
  if (!pid) return true;
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid); if (!h) return GetLastError() != ERROR_ACCESS_DENIED;
  const DWORD until = GetTickCount() + waitMs;
  while (WaitForSingleObject(h, 0) == WAIT_TIMEOUT && (int)(until - GetTickCount()) > 0) {
    MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    MsgWaitForMultipleObjects(1, &h, FALSE, 50, QS_ALLINPUT);
  }
  const bool gone = WaitForSingleObject(h, 0) != WAIT_TIMEOUT; CloseHandle(h); return gone;
}
bool WebPane::removeProfileFolder(const std::wstring& dir) { return removeProfile(dir); }
void WebPane::navigateToString(const std::string& html) {
  auto d = d_;
  d->run([d, html] { d->allowedTop.push_back(L"data:"); if (d->view) d->view->NavigateToString(W(html).c_str()); });
}
void WebPane::serve(const std::wstring& url, const WebResponse& doc) {
  auto d = d_;
  d->run([d, url, doc] {
    const bool had = d->served.count(url) > 0;
    d->served[url] = doc;
    if (!had && d->view) d->view->AddWebResourceRequestedFilter(url.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
  });
}
void WebPane::addResourceFilter(const std::wstring& wild) {
  auto d = d_;
  d->run([d, wild] { if (d->view) d->view->AddWebResourceRequestedFilter(wild.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL); });
}
void WebPane::setCookie(const std::wstring& name, const std::wstring& value, const std::wstring& domain, const std::wstring& path, bool secure) {
  auto d = d_;
  d->run([d, name, value, domain, path, secure] {
    auto v2 = d->view.as<ICoreWebView2_2>(IID_ICoreWebView2_2); if (!v2) return;
    Ptr<ICoreWebView2CookieManager> cm; v2->get_CookieManager(cm.put()); if (!cm) return;
    Ptr<ICoreWebView2Cookie> c;
    if (FAILED(cm->CreateCookie(name.c_str(), value.c_str(), domain.c_str(), path.c_str(), c.put())) || !c) return;
    c->put_IsHttpOnly(TRUE);
    c->put_IsSecure(secure ? TRUE : FALSE);
    c->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);
    cm->AddOrUpdateCookie(c.p);
  });
}
void WebPane::postJson(const std::string& json) {
  auto d = d_;
  d->run([d, json] { if (d->view) d->view->PostWebMessageAsJson(W(json).c_str()); });
}

void WebPane::capture(std::function<void(bool, const std::string&)> done) {
  Impl& d = *d_;
  if (!d.view) { if (done) done(false, ""); return; }
  Ptr<IStream> st; st.p = SHCreateMemStream(nullptr, 0);
  if (!st) { if (done) done(false, ""); return; }
  const HRESULT hr = d.view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, st.p,
    make<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>(IID_ICoreWebView2CapturePreviewCompletedHandler, [st, done](HRESULT r) -> HRESULT {
      bool ok = SUCCEEDED(r); std::string bytes;
      if (ok) {
        STATSTG sg = {}; LARGE_INTEGER zero = {};
        ok = SUCCEEDED(st->Stat(&sg, STATFLAG_NONAME)) && SUCCEEDED(st->Seek(zero, STREAM_SEEK_SET, nullptr)) && sg.cbSize.QuadPart > 0 && sg.cbSize.QuadPart < (256u << 20);
        if (ok) { bytes.resize((size_t)sg.cbSize.QuadPart); ULONG got = 0; ok = SUCCEEDED(st->Read(&bytes[0], (ULONG)bytes.size(), &got)) && got == bytes.size(); }
      }
      if (done) done(ok, ok ? bytes : std::string());
      return S_OK;
    }).p);
  if (FAILED(hr) && done) done(false, "");
}
// into memory, then one write: the file is complete and closed before `done` runs (it may open it at once)
void WebPane::capturePng(const std::wstring& path, std::function<void(bool)> done) {
  capture([path, done](bool ok, const std::string& bytes) {
    if (ok) {
      HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) ok = false;
      else { DWORD wr = 0; ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wr, nullptr) && wr == bytes.size(); CloseHandle(h); }
    }
    if (done) done(ok);
  });
}
void WebPane::exec(const std::string& js, std::function<void(bool, const std::string&)> done) {
  Impl& d = *d_;
  if (!d.view) { if (done) done(false, ""); return; }
  const HRESULT hr = d.view->ExecuteScript(W(js).c_str(), make<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
    IID_ICoreWebView2ExecuteScriptCompletedHandler, [done](HRESULT r, LPCWSTR json) -> HRESULT { if (done) done(SUCCEEDED(r), N(json)); return S_OK; }).p);
  if (FAILED(hr) && done) done(false, "");
}
void WebPane::execInFrame(const std::wstring& frameName, const std::string& js, std::function<void(bool, const std::string&)> done) {
  Impl& d = *d_;
  for (auto& f : d.frames) {
    LPWSTR nm = nullptr; f->get_Name(&nm);
    const bool hit = nm && frameName == nm; if (nm) CoTaskMemFree(nm);
    if (!hit) continue;
    auto f2 = f.as<ICoreWebView2Frame2>(IID_ICoreWebView2Frame2); if (!f2) break;
    const HRESULT hr = f2->ExecuteScript(W(js).c_str(), make<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
      IID_ICoreWebView2ExecuteScriptCompletedHandler, [done](HRESULT r, LPCWSTR json) -> HRESULT { if (done) done(SUCCEEDED(r), N(json)); return S_OK; }).p);
    if (FAILED(hr) && done) done(false, "");
    return;
  }
  if (done) done(false, "");
}
void WebPane::devtoolsCall(const std::wstring& method, const std::string& params, std::function<void(bool, const std::string&)> done) {
  Impl& d = *d_;
  if (!d.view) { if (done) done(false, ""); return; }
  const HRESULT hr = d.view->CallDevToolsProtocolMethod(method.c_str(), W(params).c_str(), make<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, HRESULT, LPCWSTR>(
    IID_ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, [done](HRESULT r, LPCWSTR json) -> HRESULT { if (done) done(SUCCEEDED(r), N(json)); return S_OK; }).p);
  if (FAILED(hr) && done) done(false, "");
}

bool WebPane::openExternal(const std::wstring& url) {
  if (url.size() > 2048) return false;
  std::wstring low = url.substr(0, 8); for (auto& c : low) c = (wchar_t)towlower(c);
  if (low.rfind(L"http://", 0) != 0 && low.rfind(L"https://", 0) != 0) return false;
  for (wchar_t c : url) if (c < 0x21 || c == L'"') return false;       // no spaces, quotes or control characters: an address, nothing else
  return (INT_PTR)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

} }  // namespace deck::web
