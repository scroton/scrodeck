// windows/web/webview2_loader.hpp — what WebView2Loader.dll does, built in, so the exe stays ONE static file.
//
// Finds the installed Evergreen WebView2 runtime (or takes a fixed-version folder), loads its
// EBWebView\x64\EmbeddedBrowserWebView.dll and calls the exported CreateWebViewEnvironmentWithOptionsInternal.
// Ported from the MIT-licensed webview/webview project's built-in loader
// (core/include/webview/detail/platform/windows/webview2/loader.hh, (c) 2017 Serge Zaitsev, (c) 2022 Steffen
// André Langnes); its licence is windows/third_party/webview2/LICENSE-webview.txt.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <string>

struct ICoreWebView2EnvironmentOptions;
struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;

namespace deck { namespace web {

struct RuntimeInfo {
  bool found = false;
  bool fixed = false;        // a fixed-version folder was given (not the installed Evergreen runtime)
  std::wstring dir;          // ...\EdgeWebView\Application\<version>
  std::wstring dll;          // <dir>\EBWebView\x64\EmbeddedBrowserWebView.dll
  std::wstring version;      // 157.0.4341.60
};

// fixedDir: a folder holding a fixed-version runtime (the one with msedgewebview2.exe in it); '' = the installed one.
// Registry, HKLM then HKCU, the 32-bit view: EdgeUpdate\ClientState\{F3017226-…} `EBWebView` (what the loader DLL
// reads), then EdgeUpdate\Clients\{F3017226-…} `location` + `pv`.
RuntimeInfo findRuntime(const std::wstring& fixedDir = L"");

// The name to give a person whose machine has no runtime (the app shows it in a note; no dialog here).
inline const wchar_t* runtimeDownloadName() { return L"Microsoft Edge WebView2 Runtime (Evergreen)"; }

// S_OK = the handler will be invoked (on this thread's message loop) with the environment or an error.
// HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) = no runtime.
HRESULT createEnvironment(const RuntimeInfo& rt, const std::wstring& userDataDir, ICoreWebView2EnvironmentOptions* options,
                          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

} }  // namespace deck::web
