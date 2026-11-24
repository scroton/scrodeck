// windows/web-test/smoke.cpp — step 1's proof: a WebView2 comes up in a PARKED window from a MinGW build with no
// WebView2Loader.dll, shows a page, and CapturePreview writes it as a PNG. Nothing appears on screen.
//   web-smoke.exe <out.png> <log.txt>
#include "../web/webpane.hpp"
#include <shellapi.h>
#include <cstdio>
#include <string>

using namespace deck::web;

static FILE* gLog = nullptr;
static void logf(const char* fmt, ...) { if (!gLog) return; va_list a; va_start(a, fmt); vfprintf(gLog, fmt, a); va_end(a); fputc('\n', gLog); fflush(gLog); }
static std::string nar(const std::wstring& w) { std::string s; for (wchar_t c : w) s += (c < 128 ? (char)c : '?'); return s; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc < 3) return 2;
  const std::wstring png = argv[1];
  gLog = _wfopen(argv[2], L"w");
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = inst; wc.lpszClassName = L"DeckWebSmoke";
  RegisterClassW(&wc);
  // parked off every screen, never activated, no taskbar button
  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, -32000, -32000, 800, 500, nullptr, nullptr, inst, nullptr);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);

  int code = 1; bool finished = false;
  WebPane pane;
  RuntimeInfo rt = findRuntime();
  logf("runtime found=%d version=%s dir=%s", rt.found, nar(rt.version).c_str(), nar(rt.dir).c_str());
  pane.onReady = [&](bool ok, const std::wstring& why) {
    logf("ready ok=%d %s pid=%lu", ok, nar(why).c_str(), pane.browserPid());
    if (!ok) { finished = true; return; }
    pane.navigateToString("<!doctype html><html><body style='margin:0;background:#0c0c0c;color:#ececec;font:20px Segoe UI'>"
                          "<h1 style='padding:24px'>WebView2 from MinGW, no loader DLL</h1><p style='padding:0 24px' id=v></p>"
                          "<script>document.getElementById('v').textContent = navigator.userAgent;</script></body></html>");
  };
  pane.onLoaded = [&](bool ok) {
    logf("loaded ok=%d", ok);
    SetTimer(hwnd, 1, 400, nullptr);          // a moment for the first paint
  };
  WebPaneOptions o;
  if (!pane.create(hwnd, Rect{ 0, 0, 800, 500 }, 1.0f, o)) { logf("create failed: state=%d %s", (int)pane.state(), nar(pane.error()).c_str()); finished = true; code = pane.state() == WebState::NoRuntime ? 3 : 1; }

  const DWORD t0 = GetTickCount();
  MSG m;
  while (!finished) {
    if (GetTickCount() - t0 > 30000) { logf("timeout"); break; }
    if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_TIMEOUT) continue;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      if (m.message == WM_TIMER && m.hwnd == hwnd && m.wParam == 1) {
        KillTimer(hwnd, 1);
        pane.capturePng(png, [&](bool ok) { logf("capture ok=%d", ok); code = ok ? 0 : 1; finished = true; });
        continue;
      }
      TranslateMessage(&m); DispatchMessageW(&m);
    }
  }
  const bool clean = pane.destroy(8000);
  logf("destroy clean=%d", clean);
  if (!clean && code == 0) code = 4;
  DestroyWindow(hwnd);
  CoUninitialize();
  logf("exit %d", code);
  if (gLog) fclose(gLog);
  return code;
}
