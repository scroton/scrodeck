// windows/web/webpane.hpp — deck::web::WebPane: one embedded WebView2 (the Edge runtime) as a child of a window.
//
// What needs a browser engine in the native deck goes through this: the ask pane's pages (src/asks.hpp), later the
// cloud panes (48-cloud.js). The header names no WebView2 type, so including it costs nothing; webpane.cpp holds
// the COM. No WebView2Loader.dll: webview2_loader.cpp does its work, the exe stays one static file.
//
// Threading: everything here is called on the window (UI) thread, and every callback arrives on it, from the
// thread's message loop. Creation is asynchronous: create() returns at once, onReady follows. What is asked of the
// pane before it is ready (a cookie, a served document, a navigation, bounds, show) is kept and applied in order.
//
// What is always off: dev tools, the context menus, the status bar, autofill, password saving, the browser's own
// accelerator keys (F5, Ctrl+F, Ctrl+P …), zoom, swipe navigation, script dialogs, downloads, permission prompts
// (denied without asking). A new window is never opened: onNewWindow gets the address and the host decides.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "webview2_loader.hpp"

namespace deck { namespace web {

struct Rect { float x = 0, y = 0, w = 0, h = 0; };   // DIPs (css px) in the parent's client area

struct WebPaneOptions {
  // The profile folder. '' = <TEMP>\ClaudeDeckNative-web\<pid>, made here. NEVER next to the exe, never a real
  // browser profile. A folder made here carries a marker file, and only a folder with the marker is ever removed.
  std::wstring userDataDir;
  bool removeUserDataOnClose = true;   // destroy() removes the folder once the browser process has gone (only ever a folder made HERE, userDataDir empty)
  bool inPrivate = true;               // an InPrivate profile: cookies and storage live in memory only (with profileName: that profile's InPrivate side)
  std::wstring fixedRuntimeDir;        // a fixed-version runtime folder; '' = the installed Evergreen runtime
  uint32_t background = 0x0c0c0c;      // 0xRRGGBB shown before the first paint (css --bg): no white flash
  // A named profile inside userDataDir (cookies, storage and sign-ins of its own), for a view that is NOT private:
  // the cloud panes keep one per account, all in one folder and so in one browser process (src/cloud.hpp).
  // Letters, digits and #@$()+-_~.= only, 64 at most (WebView2's rule). '' = the folder's default profile.
  // Views of ONE userDataDir share one browser process tree whatever their profiles are (the ask pane and every
  // cloud pane: WEB-NOTES.md "One browser process tree").
  std::wstring profileName;
  // A real web site, not a page of ours (a cloud pane: claude.ai): the things a person needs there stay ON — the
  // page's context menu (copy / paste by mouse), password saving and autofill (they sign in), downloads, the
  // browser's own error page. Everything else stays as above (no dev tools, no browser accelerator keys, no zoom,
  // no status bar, no host objects; permissions denied except reading the clipboard).
  bool site = false;
};

enum class WebState { None, Creating, Ready, NoRuntime, Failed, Closed };

// What a served request is answered with (serve / onResource)
struct WebResponse {
  int status = 200;
  std::wstring headers;                // "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store"
  std::string body;
};

class WebPane {
 public:
  WebPane();
  ~WebPane();
  WebPane(const WebPane&) = delete;
  WebPane& operator=(const WebPane&) = delete;

  // ---- callbacks (set before create)
  std::function<void(bool ok, const std::wstring& why)> onReady;        // the web view exists (or why not)
  std::function<void(const std::string& json)> onMessage;               // window.chrome.webview.postMessage(obj)
  // A navigation of the top document or of a frame is about to start: true lets it go. Not set = nothing but
  // what the host itself asked for (navigate / navigateToString) and about:blank may load.
  std::function<bool(const std::wstring& url, bool mainFrame)> onNavigate;
  std::function<void(const std::wstring& url)> onNewWindow;             // a link wanted a new window (never opened here)
  // A page of ours (not `site`) started a download: the full path to save it at, without any download UI; '' = cancelled.
  // Not set = every download is cancelled. (The ask pane: an older ask page's own Done saves its answers file this way.)
  std::function<std::wstring(const std::wstring& fileName)> onDownload;
  // Set = asked FIRST for a new window: true lets the runtime open it as a window of its own in the same profile (a
  // sign-in popup that must talk to its opener); false = no window, and onNewWindow gets the address as before.
  std::function<bool(const std::wstring& url)> onPopup;
  std::function<void(const std::wstring& url)> onUrl;                   // the top document's address changed (a navigation, or one inside the page)
  std::function<void(bool ok)> onLoaded;                                // the top document finished loading
  // A key the page does not type with (F keys, Esc, Ctrl / Alt chords) while the web view has the focus.
  // true = the host took it (the page never sees it).
  std::function<bool(unsigned vk, bool ctrl, bool shift, bool alt)> onAccelerator;
  std::function<void(bool has)> onFocus;                                // the web view got / lost the keyboard
  // A request under a filter added with addResourceFilter: true = answer it with `out`, false = let it go to the network.
  std::function<bool(const std::wstring& url, WebResponse& out)> onResource;

  // ---- life
  // false = nothing was started: state() says NoRuntime (no WebView2 runtime on this machine) or Failed (error()).
  bool create(HWND parent, const Rect& dips, float scale, const WebPaneOptions& opts = WebPaneOptions());
  WebState state() const;
  const std::wstring& error() const;
  const RuntimeInfo& runtime() const;
  // Close the web view. waitMs > 0: also wait that long for its browser process to go, then remove the profile
  // folder (when it is ours to remove). Returns true when nothing was left behind.
  bool destroy(unsigned waitMs = 0);
  // Profile folders under <TEMP>\ClaudeDeckNative-web whose process is gone (a crash left them): removed.
  static void sweepStale();

  // ---- place
  void setBounds(const Rect& dips, float scale);   // parent client DIPs -> device pixels, snapped
  void show(bool on);                              // (also the memory target: LOW while hidden, NORMAL shown - ICoreWebView2_19)
  bool visible() const;
  int memoryTarget() const;                        // what the runtime says: 0 normal, 1 low, -1 not known (an older runtime)
  void focus();                                    // the keyboard goes into the page
  void notifyParentMoved();                        // WM_MOVE / WM_MOVING of the top window (popups follow)
  // The page drawn at `scale` device pixels per css px whatever monitor the window is on (tests: a parked window is
  // on a 96 dpi screen; a real window follows its monitor by itself and needs nothing).
  void setRasterScale(float scale);
  // A view nobody can see (hidden behind a zoom, the window minimised): its renderer stops running script and
  // timers and gives memory back (TrySuspend; it must be hidden first). resume() before showing it again.
  void suspend();
  void resume();
  bool suspended() const;

  // ---- content
  void navigate(const std::wstring& url);
  void navigateToString(const std::string& html);
  void reload();
  std::wstring url() const;                        // the top document's address now ('' before the first navigation)
  bool navRedirected() const;                      // inside onNavigate: this navigation is a server redirect of one already let through
  // Answer requests for exactly `url` with this document instead of the network (a host page on the deck's own
  // origin, so the deck's SameSite cookie rides on what it frames). Replaces an earlier serve of the same url.
  void serve(const std::wstring& url, const WebResponse& doc);
  void addResourceFilter(const std::wstring& urlWildcard);              // onResource is asked for these
  // The deck's cookie, as lib/phone.js sets it (HttpOnly, SameSite=Lax, Path=/). Never written anywhere by this
  // class; with inPrivate it lives in memory only.
  void setCookie(const std::wstring& name, const std::wstring& value, const std::wstring& domain, const std::wstring& path, bool secure);
  void postJson(const std::string& json);          // -> chrome.webview 'message' event, e.data is the object

  // ---- tests and diagnostics
  void capturePng(const std::wstring& path, std::function<void(bool ok)> done);
  // the same picture as PNG bytes (the ask pane's still picture under an overlay the app draws)
  void capture(std::function<void(bool ok, const std::string& png)> done);
  void exec(const std::string& js, std::function<void(bool ok, const std::string& resultJson)> done);
  // run script in a frame of the page by its name="" (works across origins and sandboxes)
  void execInFrame(const std::wstring& frameName, const std::string& js, std::function<void(bool ok, const std::string& resultJson)> done);
  void devtoolsCall(const std::wstring& method, const std::string& paramsJson, std::function<void(bool ok, const std::string& resultJson)> done);
  unsigned long browserPid() const;
  const std::wstring& userDataDir() const;
  bool ownsUserData() const;                       // the folder was made here (it holds the marker): destroy / removeProfileFolder may remove it
  // The profile this view runs in: is it InPrivate (asked of the runtime, not of the options), and its folder on disk
  // (an InPrivate profile names the folder of the profile it hangs off; nothing of the private side is written there).
  bool privateMode() const;
  std::wstring profilePath() const;
  // Every cookie of this view's profile, gone (the ask pane's, before its view closes: the deck's cookie does not
  // outlive the pane even while the shared browser process lives on for a cloud pane).
  void clearCookies();
  // The browser's command line, the same for every view of the process (webpane.cpp kDefaultArgs says why each
  // argument is there). setBrowserArguments replaces it (tests: one flag at a time, measured) and must be called
  // before the first view is made: environments on one folder cannot differ.
  static std::wstring browserArguments();
  static void setBrowserArguments(const std::wstring& args);
  // Wait up to waitMs for a browser process to go (several panes of one folder share one: close them all with
  // destroy(0), then wait once). true = it is gone.
  static bool waitProcessGone(unsigned long pid, unsigned waitMs);
  // Remove a profile folder this class made (it holds the marker file; any other folder is left alone). true = gone.
  static bool removeProfileFolder(const std::wstring& dir);

  // http(s) only, in the default browser. false = refused or failed.
  static bool openExternal(const std::wstring& url);

  struct Impl;
 private:
  std::shared_ptr<Impl> d_;
};

} }  // namespace deck::web
