#include "runtime/gpu/window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <vector>

#include "runtime/gpu/input_hooks.h"
#include "runtime/gpu/present.h"
#include "runtime/input.h"
#include "runtime/log.h"

namespace rt::gpu {
namespace {

HWND g_hwnd = nullptr;
std::atomic<bool> g_close{false};
bool g_alive = false;
DWORD g_thread = 0;
bool g_mouse_captured = false;
bool g_fullscreen = false;
WINDOWPLACEMENT g_placement{};  // windowed placement while fullscreen
std::atomic<double> g_refresh_hz{0};

// Refresh rate of a monitor's current mode as the display path reports it (exact, e.g. 59.94 as
// 60000/1001); the GDI mode's whole hertz otherwise.
double monitor_refresh_hz(HMONITOR mon) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof mi;
  if (!mon || !GetMonitorInfoW(mon, &mi)) return 0;
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS) {
      for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || std::wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0)
          continue;
        const DISPLAYCONFIG_RATIONAL& r = paths[i].targetInfo.refreshRate;
        if (r.Numerator && r.Denominator) return double(r.Numerator) / double(r.Denominator);
      }
    }
  }
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return dm.dmDisplayFrequency;
  return 0;
}

void update_refresh() {
  double hz = monitor_refresh_hz(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST));
  if (g_refresh_hz.exchange(hz) != hz) LOG_I("gpu", "display refresh rate %.3f Hz", hz);
}

// Borderless fullscreen on the window's display (F11 / Alt+Enter, --fullscreen); the windowed
// placement comes back when it ends.
void set_fullscreen(bool on, bool activate = true) {
  if (on == g_fullscreen || !g_hwnd) return;
  DWORD style = DWORD(GetWindowLongW(g_hwnd, GWL_STYLE));
  if (on) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    g_placement.length = sizeof g_placement;
    if (!GetWindowPlacement(g_hwnd, &g_placement) || !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
      return;
    SetWindowLongW(g_hwnd, GWL_STYLE, LONG((style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
    SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (activate ? 0 : SWP_NOACTIVATE));
  } else {
    SetWindowLongW(g_hwnd, GWL_STYLE, LONG((style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW));
    SetWindowPlacement(g_hwnd, &g_placement);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  g_fullscreen = on;
  update_refresh();
  LOG_I("gpu", "%s", on ? "borderless fullscreen" : "windowed");
}

void clip_cursor() {
  RECT r;
  GetClientRect(g_hwnd, &r);
  MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
  ClipCursor(&r);
}

// Mouse capture for mouse look: the cursor is hidden and confined to the client area.
void set_mouse_capture(bool on) {
  if (on == g_mouse_captured) return;
  g_mouse_captured = on;
  if (on) {
    clip_cursor();
    while (ShowCursor(FALSE) >= 0) {
    }
  } else {
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {
    }
  }
  input_mouse_capture(on);
}

struct Call {
  const std::function<void()>* fn;
  bool done = false;
};
std::mutex g_call_mu;
std::condition_variable g_call_cv;
std::deque<Call*> g_calls;

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      g_close = true;
      return 0;  // the runtime asks the game to exit first
    case WM_DESTROY:
      g_alive = false;
      return 0;
    case WM_INPUT: {
      RAWINPUT ri;
      UINT size = sizeof ri;
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
          ri.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = ri.data.mouse;
        input_mouse_raw(m.usFlags, m.lLastX, m.lLastY, m.usButtonFlags, m.usButtonData);
      }
      break;  // DefWindowProc cleans up the raw input buffer
    }
    case WM_LBUTTONDOWN:
      if (!g_mouse_captured && !input_isolated()) {  // isolated test runs never take the cursor
        set_mouse_capture(true);
        return 0;
      }
      break;
    case WM_SIZE:
    case WM_MOVE:
      if (g_mouse_captured) clip_cursor();
      break;
    case WM_EXITSIZEMOVE:  // possibly moved to another display
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      update_refresh();
      if (msg == WM_DPICHANGED && !g_fullscreen) {
        const RECT* r = reinterpret_cast<const RECT*>(lp);  // the size Windows suggests for the new scale
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      }
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // F11 / Alt+Enter: fullscreen (not passed to the game's bindings).
      if ((wp == VK_F11 || (wp == VK_RETURN && msg == WM_SYSKEYDOWN && (lp & (1 << 29)))) && !input_isolated()) {
        if (!(lp & (1 << 30))) set_fullscreen(!g_fullscreen);
        return 0;
      }
      if (wp == VK_F1 && !(lp & (1 << 30)) && !input_isolated()) set_mouse_capture(!g_mouse_captured);
      input_key(u32(wp), true);
      if (msg == WM_SYSKEYDOWN) break;
      return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      input_key(u32(wp), false);
      if (msg == WM_SYSKEYUP) break;
      return 0;
    case WM_KILLFOCUS:
      set_mouse_capture(false);
      input_focus(false);
      return 0;
    case WM_SETFOCUS:
      input_focus(true);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool window_create(const std::string& title, int w, int h, bool visible) {
  // Per-monitor DPI awareness: the swap chain matches the window's real pixels (no blurry
  // bitmap scaling by Windows on scaled displays). Windows 10 1703 and later.
  using SetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  if (auto set_ctx = reinterpret_cast<SetCtx>(
          reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
    set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  {  // the requested client size at the display's scale, within its work area (16:9 kept)
    HDC screen = GetDC(nullptr);
    int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    RECT work{0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    double scale = std::max(1.0, dpi / 96.0);
    scale = std::min({scale, 0.9 * (work.right - work.left) / w, 0.85 * (work.bottom - work.top) / h});
    if (scale > 0.25) {
      w = int(w * scale);
      h = int(h * scale);
    }
  }
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = L"ac4_window";
  RegisterClassExW(&wc);
  RECT r{0, 0, w, h};
  DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&r, style, FALSE);
  std::wstring wt(title.begin(), title.end());
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, wt.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) return false;
  g_alive = true;
  g_thread = GetCurrentThreadId();
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;  // generic desktop
  rid.usUsage = 0x02;      // mouse
  rid.hwndTarget = g_hwnd;
  if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
    LOG_W("input", "raw mouse input unavailable (RegisterRawInputDevices failed, error %lu)", GetLastError());
  if (visible) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  update_refresh();
  return true;
}

void window_set_fullscreen(bool on, bool activate) {
  run_on_window_thread([on, activate] { set_fullscreen(on, activate); });
}

void window_set_client_size(int w, int h) {
  run_on_window_thread([w, h] {
    if (g_fullscreen) set_fullscreen(false);
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, DWORD(GetWindowLongW(g_hwnd, GWL_STYLE)), FALSE);
    SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  });
}

void window_set_minimized(bool on) {  // the active window stays active either way
  run_on_window_thread([on] { ShowWindow(g_hwnd, on ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE); });
}

double display_refresh_hz() { return g_refresh_hz.load(std::memory_order_relaxed); }

void* window_handle() { return g_hwnd; }

bool window_pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  for (;;) {
    Call* c;
    {
      std::lock_guard lk(g_call_mu);
      if (g_calls.empty()) break;
      c = g_calls.front();
      g_calls.pop_front();
    }
    (*c->fn)();
    {
      std::lock_guard lk(g_call_mu);
      c->done = true;
    }
    g_call_cv.notify_all();
  }
  return g_alive;
}

bool run_on_window_thread(const std::function<void()>& fn) {
  if (!g_hwnd || !g_alive) return false;
  if (GetCurrentThreadId() == g_thread) {
    fn();
    return true;
  }
  Call c{&fn};
  std::unique_lock lk(g_call_mu);
  g_calls.push_back(&c);
  g_call_cv.wait(lk, [&] { return c.done; });
  return true;
}

bool window_close_requested() { return g_close.load(); }

void window_set_title(const std::string& title) {
  std::wstring wt(title.begin(), title.end());
  if (g_hwnd) SetWindowTextW(g_hwnd, wt.c_str());
}

}  // namespace rt::gpu
