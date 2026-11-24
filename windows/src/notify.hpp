// windows/src/notify.hpp — the deck's toasts and the taskbar flash on Windows (core/deck/seams.hpp Notifier), and
// the taskbar identity they need (main.js `APP_ID`, `ensureShortcut`, `maybeNotify`).
//
// A toast from a program that is not a packaged app needs an AppUserModelID and a Start-menu shortcut that carries
// the same id (the notification platform looks the sender up by it). main.js does that with
// app.setAppUserModelId + shell.writeShortcutLink; here it is SetCurrentProcessExplicitAppUserModelID + an
// IShellLink with PKEY_AppUserModel_ID. THE NATIVE DECK HAS ITS OWN ID AND ITS OWN SHORTCUT — never the Electron
// deck's ("demo.claude-deck", "Claude Deck.lnk"): the two run side by side and a toast must say which one sent it.
//
// The toast itself is WinRT (Windows.UI.Notifications) through its raw COM ABI: no C++/WinRT, no WRL, no import
// library — combase.dll's four entry points are looked up at run time, and the six interfaces used are declared
// below with the IIDs and method order of the SDK's windows.ui.notifications.h / windows.data.xml.dom.h. A machine
// where any of it is missing shows no toast and says why in lastError(); the flash still works.
//
// TESTS NEVER SHOW A TOAST AND NEVER WRITE THE SHORTCUT: with `record` set, toast() and flash() only append to
// `shown` / `flashed` (and the log), which is all a test reads. The real path below runs only in a real run.
#pragma once
#include "../../core/deck/seams.hpp"
#include "../../core/deck/edition.hpp"
#include <windows.h>
#include <inspectable.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propidl.h>
#include <functional>
#include <string>
#include <vector>

namespace deck {

// the native deck's own identity (main.js: 'demo.claude-deck' for the checkout, 'app.claude-deck' packaged); the
// names are the edition's (core/deck/edition.hpp): demo.claude-deck-native / "Claude Deck Native.lnk" in this copy
static const wchar_t* const kAppId = L"" DECK_EDITION_APP_ID;
static const wchar_t* const kShortcutName = L"" DECK_EDITION_SHORTCUT L".lnk";
// A REMOTE window (this exe driving a deck on another PC) is its own app on the taskbar: its own group, its own pin,
// its own icon (resource 2, windows/res/remote.ico: the same picture on blue) - 2026-11-19: "remote can get own
// task bar, and can you make the icons noticeably different". A remote shows no toasts, so it needs no Start-menu
// shortcut of its own and the exe never writes one for it.
static const wchar_t* const kRemoteAppId = L"" DECK_EDITION_REMOTE_APP_ID;
static const int kIconLocal = 1, kIconRemote = 2;

namespace toastabi {
struct HSTRING__; typedef HSTRING__* HSTR;
struct Token { __int64 value; };
static const IID IID_ManagerStatics = { 0x50ac103f, 0xd235, 0x4598, { 0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4 } };
static const IID IID_ToastFactory = { 0x04124b20, 0x82c6, 0x4229, { 0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53 } };
static const IID IID_Toast2 = { 0x9dfb9fd1, 0x143a, 0x490e, { 0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7 } };
static const IID IID_XmlDocumentIO = { 0x6cd0e74e, 0xee65, 0x4489, { 0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37 } };
static const IID IID_XmlDocument = { 0xf7f3a506, 0x1e87, 0x42d6, { 0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94 } };
// ITypedEventHandler<ToastNotification*, IInspectable*>
static const IID IID_ActivatedHandler = { 0xab54de2d, 0x97d9, 0x5528, { 0xb6, 0xad, 0x10, 0x5a, 0xfe, 0x15, 0x65, 0x30 } };
static const IID IID_Agile = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

struct IXmlDocumentIO : IInspectable { virtual HRESULT STDMETHODCALLTYPE LoadXml(HSTR xml) = 0; };
struct IActivatedHandler : IUnknown { virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) = 0; };
struct IToast : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Content(IInspectable** v) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_ExpirationTime(IInspectable* v) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ExpirationTime(IInspectable** v) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_Dismissed(IUnknown* h, Token* t) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_Dismissed(Token t) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_Activated(IActivatedHandler* h, Token* t) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_Activated(Token t) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_Failed(IUnknown* h, Token* t) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_Failed(Token t) = 0;
};
struct IToast2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Tag(HSTR v) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Tag(HSTR* v) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Group(HSTR v) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Group(HSTR* v) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_SuppressPopup(boolean v) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SuppressPopup(boolean* v) = 0;
};
struct IToastNotifier : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Show(IToast* t) = 0;
  virtual HRESULT STDMETHODCALLTYPE Hide(IToast* t) = 0;
};
struct IToastFactory : IInspectable { virtual HRESULT STDMETHODCALLTYPE CreateToastNotification(IInspectable* xmlDocument, IToast** out) = 0; };
struct IManagerStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotifier(IToastNotifier** out) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotifierWithId(HSTR appId, IToastNotifier** out) = 0;
};

// combase.dll, looked up once (no import library: the exe links what it linked before)
struct Combase {
  HRESULT (WINAPI* createString)(const wchar_t*, UINT32, HSTR*) = nullptr;
  HRESULT (WINAPI* deleteString)(HSTR) = nullptr;
  HRESULT (WINAPI* getFactory)(HSTR, REFIID, void**) = nullptr;
  HRESULT (WINAPI* activate)(HSTR, IInspectable**) = nullptr;
  bool ok = false;
  Combase() {
    HMODULE m = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); if (!m) return;
    createString = (decltype(createString))(void*)GetProcAddress(m, "WindowsCreateString");
    deleteString = (decltype(deleteString))(void*)GetProcAddress(m, "WindowsDeleteString");
    getFactory = (decltype(getFactory))(void*)GetProcAddress(m, "RoGetActivationFactory");
    activate = (decltype(activate))(void*)GetProcAddress(m, "RoActivateInstance");
    ok = createString && deleteString && getFactory && activate;
  }
};
inline Combase& combase() { static Combase c; return c; }
struct Str {                                      // an HSTRING for the length of a call
  HSTR h = nullptr;
  explicit Str(const std::wstring& s) { combase().createString(s.c_str(), (UINT32)s.size(), &h); }
  ~Str() { if (h) combase().deleteString(h); }
};
template <class T> struct Ptr {                   // the smallest COM pointer
  T* p = nullptr;
  ~Ptr() { if (p) p->Release(); }
  T** out() { return &p; }
  T* operator->() const { return p; }
};
// The click. Windows calls Invoke on a thread of its own: nothing is touched here, a message is posted to the hub.
class Clicked : public IActivatedHandler {
 public:
  Clicked(HWND hub, UINT msg, std::string tile) : hub_(hub), msg_(msg), tile_(std::move(tile)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (!o) return E_POINTER;
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_ActivatedHandler) || IsEqualIID(r, IID_Agile)) { *o = static_cast<IActivatedHandler*>(this); AddRef(); return S_OK; }
    *o = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&n_); }
  ULONG STDMETHODCALLTYPE Release() override { const LONG n = InterlockedDecrement(&n_); if (!n) delete this; return (ULONG)n; }
  HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable*) override {
    std::string* s = new std::string(tile_);
    if (!PostMessageW(hub_, msg_, 0, (LPARAM)s)) delete s;
    return S_OK;
  }
 private:
  virtual ~Clicked() {}
  LONG n_ = 1; HWND hub_; UINT msg_; std::string tile_;
};
inline std::wstring xmlEsc(const std::wstring& s) {
  std::wstring o;
  for (wchar_t c : s) { if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else if (c == L'"') o += L"&quot;"; else if (c == L'\'') o += L"&apos;"; else if (c < 0x20 && c != L'\n' && c != L'\t') o += L' '; else o += c; }
  return o;
}
// The toast's document: two lines of text, as Electron's `new Notification({ title, body, silent })` makes it.
inline std::wstring toastXml(const std::wstring& title, const std::wstring& body, bool silent) {
  return L"<toast><visual><binding template=\"ToastGeneric\"><text>" + xmlEsc(title) + L"</text><text>" + xmlEsc(body) + L"</text></binding></visual>"
    + (silent ? L"<audio silent=\"true\"/>" : L"") + L"</toast>";
}
}  // namespace toastabi

inline std::wstring wideOf(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}

// main.js ensureShortcut, for this exe: `Start Menu\Programs\Claude Deck Native.lnk` carries the AppUserModelID.
//  - none there: made, pointing at this exe (its own icon), no arguments (the mode is remembered in config.json);
//  - there and pointing at this exe: its arguments are KEPT (a flag the user put on it), the id stamped;
//  - there and pointing somewhere else (the user's own launcher, another build): its target, arguments and icon
//    are the user's — only the id is stamped, so the window still groups under the pinned icon.
// Returns what it did ("created", "updated", "stamped", or why not), for the log.
// `dir`: the folder the shortcut lives in; empty = the user's Start menu `Programs` (the only place Windows looks
// a toast's sender up). `--register --shortcut-dir <folder>` and every test name a folder; a test NEVER names none.
// `args`: null = as above (none on a new shortcut, an existing one's kept); else the shortcut's arguments are set
// to exactly this (the installer's `--local`) - never on a shortcut that points somewhere else.
inline std::wstring shortcutPath(const std::wstring& dir) {
  if (!dir.empty()) return dir + (dir.back() == L'\\' || dir.back() == L'/' ? L"" : L"\\") + kShortcutName;
  wchar_t* programs = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)) || !programs) return L"";
  const std::wstring lnk = std::wstring(programs) + L"\\" + kShortcutName; CoTaskMemFree(programs);
  return lnk;
}
inline std::string ensureShortcut(const std::wstring& appId, const std::wstring& dir = L"", const std::wstring* args = nullptr) {
  static const PROPERTYKEY kAppUserModelId = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
  const std::wstring lnk = shortcutPath(dir);
  if (lnk.empty()) return "no Start Menu folder";
  wchar_t exe[32768]; const DWORD n = GetModuleFileNameW(nullptr, exe, 32768); if (!n) return "no exe path";
  const std::wstring self(exe, n);
  const bool exists = GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES;
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)) || !sl) return "no IShellLink";
  IPersistFile* pf = nullptr; IPropertyStore* ps = nullptr; std::string did = "failed";
  if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
    bool foreign = false;
    if (exists && SUCCEEDED(pf->Load(lnk.c_str(), STGM_READWRITE))) {
      wchar_t target[32768] = L""; sl->GetPath(target, 32768, nullptr, SLGP_RAWPATH);
      foreign = target[0] && CompareStringOrdinal(target, -1, self.c_str(), -1, TRUE) != CSTR_EQUAL;
    }
    if (!foreign) {
      wchar_t dir[32768]; wcsncpy(dir, self.c_str(), 32767); dir[32767] = 0; { wchar_t* s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
      sl->SetPath(self.c_str()); sl->SetWorkingDirectory(dir); sl->SetIconLocation(self.c_str(), 0);
      sl->SetDescription(L"" DECK_EDITION_DESCRIPTION L": all Claude Code sessions in one window");
      if (args) sl->SetArguments(args->c_str());
    }
    if (SUCCEEDED(sl->QueryInterface(IID_IPropertyStore, (void**)&ps)) && ps) {
      PROPVARIANT pv; PropVariantInit(&pv);
      pv.vt = VT_LPWSTR; pv.pwszVal = (LPWSTR)CoTaskMemAlloc((appId.size() + 1) * sizeof(wchar_t));
      if (pv.pwszVal) { wcscpy(pv.pwszVal, appId.c_str()); ps->SetValue(kAppUserModelId, pv); ps->Commit(); PropVariantClear(&pv); }
      if (SUCCEEDED(pf->Save(lnk.c_str(), TRUE))) did = foreign ? "stamped" : exists ? "updated" : "created";
    }
  }
  if (ps) ps->Release();
  if (pf) pf->Release();
  sl->Release();
  return did;
}

// One window's taskbar identity, set on the window itself (so an in-app switch local <-> remote regroups it):
// System.AppUserModel.ID, plus the relaunch command a pin of the RUNNING window starts - this exe with only the
// switches that say which deck it is (`--local`, `--state-dir <folder>`), never anything else from the command line
// (a test's `--link` would carry a token) - its display name and icon; and the window's own big and small icons.
// `clear` (at WM_DESTROY) takes the properties off again, as the shell asks of a window that set them.
inline void setWindowIdentity(HWND hwnd, HINSTANCE inst, bool remote, const std::wstring& stateDir, bool clear = false) {
  static const PROPERTYKEY kId = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
  static const PROPERTYKEY kRelaunchCommand = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 2 };
  static const PROPERTYKEY kRelaunchIcon = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 3 };
  static const PROPERTYKEY kRelaunchName = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 4 };
  if (!hwnd) return;
  if (!clear) {
    const int icon = remote ? kIconRemote : kIconLocal;
    HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(icon), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    HICON small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(icon), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
  }
  IPropertyStore* ps = nullptr;
  if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore, (void**)&ps)) || !ps) return;
  auto put = [&](const PROPERTYKEY& k, const std::wstring& v) {
    PROPVARIANT pv; PropVariantInit(&pv);
    if (!clear) {
      pv.vt = VT_LPWSTR; pv.pwszVal = (LPWSTR)CoTaskMemAlloc((v.size() + 1) * sizeof(wchar_t));
      if (!pv.pwszVal) return;
      wcscpy(pv.pwszVal, v.c_str());
    }
    ps->SetValue(k, pv);
    PropVariantClear(&pv);
  };
  wchar_t exe[32768]; const DWORD n = GetModuleFileNameW(nullptr, exe, 32768);
  const std::wstring self = n ? std::wstring(exe, n) : std::wstring();
  std::wstring cmd = L"\"" + self + L"\"";
  if (!remote) cmd += L" --local";
  if (!stateDir.empty()) cmd += L" --state-dir \"" + stateDir + L"\"";
  // the order the shell documents: the relaunch properties first, the id last (setting the id is what makes it look)
  if (clear) { put(kId, L""); put(kRelaunchCommand, L""); put(kRelaunchName, L""); put(kRelaunchIcon, L""); }
  else if (!self.empty()) {
    put(kRelaunchCommand, cmd);
    put(kRelaunchName, remote ? L"" DECK_EDITION_REMOTE_NAME : L"" DECK_EDITION_SHORTCUT);
    put(kRelaunchIcon, self + L"," + std::to_wstring(-(remote ? kIconRemote : kIconLocal)));
    put(kId, remote ? kRemoteAppId : kAppId);
  }
  ps->Release();
}

// (tests) what the shell now holds for a window: its AppUserModelID and relaunch command, read back from the window's
// own property store - the proof setWindowIdentity took, since a test cannot look at the taskbar.
inline std::wstring windowIdentity(HWND hwnd) {
  static const PROPERTYKEY kId = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
  static const PROPERTYKEY kRelaunchCommand = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 2 };
  IPropertyStore* ps = nullptr;
  if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore, (void**)&ps)) || !ps) return L"(no property store)";
  std::wstring out;
  for (const PROPERTYKEY* k : { &kId, &kRelaunchCommand }) {
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(ps->GetValue(*k, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out += std::wstring(out.empty() ? L"" : L" | ") + pv.pwszVal;
    else out += std::wstring(out.empty() ? L"" : L" | ") + L"(none)";
    PropVariantClear(&pv);
  }
  ps->Release();
  HICON big = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0);
  out += big ? L" | icon set" : L" | no icon";
  return out;
}

// `--unregister`: the shortcut goes when it is OURS - it points at this exe. One the user re-pointed at something
// else (their own launcher) is theirs and stays. Returns "removed", "absent", "foreign" (left alone) or why not.
inline std::string removeShortcut(const std::wstring& dir = L"") {
  const std::wstring lnk = shortcutPath(dir);
  if (lnk.empty()) return "no Start Menu folder";
  if (GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES) return "absent";
  wchar_t exe[32768]; const DWORD n = GetModuleFileNameW(nullptr, exe, 32768); if (!n) return "no exe path";
  const std::wstring self(exe, n);
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)) || !sl) return "no IShellLink";
  IPersistFile* pf = nullptr; std::string did = "failed"; bool ours = false;
  if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
    if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
      wchar_t target[32768] = L""; sl->GetPath(target, 32768, nullptr, SLGP_RAWPATH);
      ours = CompareStringOrdinal(target, -1, self.c_str(), -1, TRUE) == CSTR_EQUAL;
      did = ours ? "removed" : "foreign";
    }
    pf->Release();
  }
  sl->Release();                                          // (released before the file is deleted)
  if (ours && !DeleteFileW(lnk.c_str())) did = "failed";
  return did;
}

class ShellNotifier : public core::Notifier {
 public:
  // ---- set by the shell
  bool record = false;                            // a test: nothing is shown, nothing flashes; `shown` / `flashed` say what would have
  HWND hub = nullptr; UINT clickMsg = 0;          // a clicked toast posts clickMsg to hub (lParam = a heap std::string: clickKey(toast))
  // what a click carries back: the toast's kind, tile and session (core::Deck::toastClicked wants all three)
  static std::string clickKey(const core::Toast& t) { return t.kind + '\x1f' + t.tileId + '\x1f' + t.sessionId; }
  static core::Toast fromClickKey(const std::string& k) {
    core::Toast t; const size_t a = k.find('\x1f'), b = a == std::string::npos ? a : k.find('\x1f', a + 1);
    if (a == std::string::npos) { t.tileId = k; return t; }
    t.kind = k.substr(0, a); t.tileId = k.substr(a + 1, b == std::string::npos ? b : b - a - 1); if (b != std::string::npos) t.sessionId = k.substr(b + 1);
    return t;
  }
  std::function<void(int windowIndex)> onFlash;   // the shell flashes that window's taskbar button
  std::function<void(const std::string&)> log;    // (tests) a line per toast / flash
  std::vector<core::Toast> shown;
  std::vector<int> flashed;

  // The process's taskbar identity and its Start-menu shortcut. A real run calls this once, before its first window.
  // A REMOTE run takes the remote's own id and writes NO shortcut (it shows no toasts; before 2026-11-19 a remote run
  // wrote "Claude Deck Native.lnk" pointing at itself, which is the local deck's entry). Each window also carries its
  // id itself (setWindowIdentity), so a palette switch between the two regroups it.
  void identify(bool remote) {
    if (record) return;
    SetCurrentProcessExplicitAppUserModelID(remote ? kRemoteAppId : kAppId);
    if (!remote) shortcut_ = ensureShortcut(kAppId);
    else shortcut_ = "not written (remote)";
  }
  const std::string& shortcutState() const { return shortcut_; }
  const std::string& lastError() const { return error_; }

  void toast(const core::Toast& t) override {
    shown.push_back(t);
    if (shown.size() > 64) shown.erase(shown.begin());
    if (log) log("toast [" + t.kind + "] tile " + t.tileId + " silent " + std::to_string(t.silent) + " '" + t.title + "' / '" + t.body + "'");
    if (record) return;
    show(wideOf(t.title), wideOf(t.body), t.silent, clickKey(t), false);
  }
  void flash(int windowIndex) override {
    flashed.push_back(windowIndex);
    if (flashed.size() > 64) flashed.erase(flashed.begin());
    if (log) log("flash window " + std::to_string(windowIndex));
    if (record) return;
    if (onFlash) onFlash(windowIndex);
  }
  // One toast. `quiet`: it goes straight to the notification centre without popping up (IToastNotification2
  // SuppressPopup) and is taken down again at once — the self-check a real run can make without anything appearing.
  bool show(const std::wstring& title, const std::wstring& body, bool silent, const std::string& clickKeyOrTile, bool quiet) {
    const std::string& tileId = clickKeyOrTile;
    using namespace toastabi;
    error_.clear();
    Combase& cb = combase();
    if (!cb.ok) { error_ = "no WinRT on this machine (combase.dll)"; return false; }
    auto fail = [&](const char* what, HRESULT hr) { char b[96]; snprintf(b, sizeof b, "%s: 0x%08lx", what, (unsigned long)hr); error_ = b; return false; };
    HRESULT hr;
    Ptr<IInspectable> doc;
    { Str cls(L"Windows.Data.Xml.Dom.XmlDocument"); hr = cb.activate(cls.h, doc.out()); if (FAILED(hr) || !doc.p) return fail("XmlDocument", hr); }
    { Ptr<IXmlDocumentIO> io; hr = doc.p->QueryInterface(IID_XmlDocumentIO, (void**)io.out()); if (FAILED(hr) || !io.p) return fail("IXmlDocumentIO", hr);
      Str xml(toastXml(title, body, silent || quiet)); hr = io->LoadXml(xml.h); if (FAILED(hr)) return fail("LoadXml", hr); }
    Ptr<IInspectable> xdoc; hr = doc.p->QueryInterface(IID_XmlDocument, (void**)xdoc.out()); if (FAILED(hr) || !xdoc.p) return fail("IXmlDocument", hr);
    Ptr<IToastFactory> factory;
    { Str cls(L"Windows.UI.Notifications.ToastNotification"); hr = cb.getFactory(cls.h, IID_ToastFactory, (void**)factory.out()); if (FAILED(hr) || !factory.p) return fail("ToastNotification factory", hr); }
    IToast* toast = nullptr; hr = factory->CreateToastNotification(xdoc.p, &toast); if (FAILED(hr) || !toast) return fail("CreateToastNotification", hr);
    Ptr<IManagerStatics> mgr;
    { Str cls(L"Windows.UI.Notifications.ToastNotificationManager"); hr = cb.getFactory(cls.h, IID_ManagerStatics, (void**)mgr.out()); if (FAILED(hr) || !mgr.p) { toast->Release(); return fail("ToastNotificationManager", hr); } }
    Ptr<IToastNotifier> notifier;
    { Str id(kAppId); hr = mgr->CreateToastNotifierWithId(id.h, notifier.out()); if (FAILED(hr) || !notifier.p) { toast->Release(); return fail("CreateToastNotifierWithId", hr); } }
    if (quiet) { Ptr<IToast2> t2; if (SUCCEEDED(toast->QueryInterface(IID_Toast2, (void**)t2.out())) && t2.p) t2->put_SuppressPopup(TRUE); }
    if (hub && clickMsg && !quiet) { Clicked* h = new Clicked(hub, clickMsg, tileId); Token tok = {}; toast->add_Activated(h, &tok); h->Release(); }
    hr = notifier->Show(toast);
    if (FAILED(hr)) { toast->Release(); return fail("Show", hr); }
    if (quiet) { notifier->Hide(toast); toast->Release(); return true; }
    // the toast object is kept while it may still be clicked (its handler lives with it); the oldest go first
    live_.push_back(toast);
    while (live_.size() > 16) { live_.front()->Release(); live_.erase(live_.begin()); }
    return true;
  }
  // (the toasts still held when the process ends are left to it: COM is gone by the time a static is destroyed)

 private:
  std::vector<toastabi::IToast*> live_;
  std::string shortcut_, error_;
};

}  // namespace deck
