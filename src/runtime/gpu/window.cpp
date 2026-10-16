#include "runtime/gpu/window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "runtime/config.h"
#include "runtime/game/menu_mouse.h"
#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/settings.h"
#include "runtime/sysdialog.h"
#include "runtime/ui/overlay.h"

namespace rt::gpu {
namespace {

HWND g_hwnd = nullptr;
std::atomic<bool> g_close{false};
bool g_alive = false;
DWORD g_thread = 0;
std::atomic<bool> g_mouse_captured{false};
std::atomic<bool> g_fullscreen{false};  // borderless fullscreen (set_fullscreen)
// F1 gave the mouse back in gameplay: mouse look waits for a click into the game, F1 again or the
// next stretch of gameplay (window thread).
bool g_user_released = false;
// The pointer's last screen position (window thread): Windows also sends WM_MOUSEMOVE when a window
// appears or moves under a resting pointer; only real moves count as using the mouse.
POINT g_pointer{LONG_MIN, LONG_MIN};

void clip_cursor() {
  if (input_isolated()) return;  // test runs leave the real cursor alone
  RECT r;
  GetClientRect(g_hwnd, &r);
  MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
  ClipCursor(&r);
}

// Mouse capture for mouse look: the cursor is hidden and confined to the client area (isolated
// test runs keep only the state: the real cursor stays as it is).
void set_mouse_capture(bool on) {
  if (on == g_mouse_captured) return;
  g_mouse_captured = on;
  if (input_isolated()) {
  } else if (on) {
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

// The menu cursor, drawn in the style of the game's interface (pale blue-white arrow, dark blue
// outline), sized for the window's DPI; made once per DPI.
HCURSOR make_menu_cursor(UINT dpi) {
  const int n = MulDiv(32, int(dpi), 96);
  // The arrow outline in a 32-unit box (tip at the top left), scaled to n pixels.
  static const float kShape[][2] = {{1, 1}, {1, 23}, {6.5f, 18}, {10.5f, 27}, {14.5f, 25.5f}, {10.5f, 16.5f}, {18, 16.5f}};
  constexpr int kPoints = int(sizeof kShape / sizeof kShape[0]);
  auto inside = [&](float x, float y) {  // even-odd rule
    bool in = false;
    for (int i = 0, j = kPoints - 1; i < kPoints; j = i++) {
      float xi = kShape[i][0], yi = kShape[i][1], xj = kShape[j][0], yj = kShape[j][1];
      if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
  };
  BITMAPV5HEADER bh{};
  bh.bV5Size = sizeof bh;
  bh.bV5Width = n;
  bh.bV5Height = -n;
  bh.bV5Planes = 1;
  bh.bV5BitCount = 32;
  bh.bV5Compression = BI_BITFIELDS;
  bh.bV5RedMask = 0x00FF0000;
  bh.bV5GreenMask = 0x0000FF00;
  bh.bV5BlueMask = 0x000000FF;
  bh.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color || !bits) return LoadCursorW(nullptr, IDC_ARROW);
  u32* px = static_cast<u32*>(bits);
  const float unit = 32.0f / float(n), edge = 1.6f;  // outline width in box units
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      int fill = 0, line = 0;  // 4x4 samples: fill (inside the arrow shrunk by the outline) and outline
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) {
          float fx = (float(x) + (float(sx) + 0.5f) / 4) * unit, fy = (float(y) + (float(sy) + 0.5f) / 4) * unit;
          if (!inside(fx, fy)) continue;
          bool core = inside(fx - edge, fy) && inside(fx + edge, fy) && inside(fx, fy - edge) && inside(fx, fy + edge);
          ++(core ? fill : line);
        }
      float a = float(fill + line) / 16.0f;
      if (a <= 0) {
        px[y * n + x] = 0;
        continue;
      }
      // fill: pale blue-white, a little darker toward the tail; outline: dark blue
      float t = std::min(1.0f, float(y) / float(n));
      float fr = 232 - 30 * t, fg = 240 - 24 * t, fb = 250 - 10 * t;
      float k = float(fill) / float(fill + line);
      float r = fr * k + 22 * (1 - k), g = fg * k + 34 * (1 - k), b = fb * k + 52 * (1 - k);
      u32 A = u32(a * 255 + 0.5f);
      auto pm = [&](float c) { return u32(c * a + 0.5f); };  // premultiplied
      px[y * n + x] = (A << 24) | (pm(r) << 16) | (pm(g) << 8) | pm(b);
    }
  HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
  ICONINFO ii{};
  ii.fIcon = FALSE;
  ii.xHotspot = DWORD(std::max(0, MulDiv(1, int(dpi), 96)));
  ii.yHotspot = ii.xHotspot;
  ii.hbmMask = mask;
  ii.hbmColor = color;
  HCURSOR c = HCURSOR(CreateIconIndirect(&ii));
  DeleteObject(mask);
  DeleteObject(color);
  return c ? c : LoadCursorW(nullptr, IDC_ARROW);
}

// The cursor over the client area: the menu cursor in the game's menus once the mouse moved there
// (until a key or a controller takes over) and in PC SETTINGS; the arrow in gameplay after F1 gave
// the mouse back; none otherwise, in every window mode: mouse look, the title, movies, cutscenes,
// loading screens, a menu driven by keys or a controller.
// The numpad's keys arrive as navigation keys while Num Lock is off (Num8 as Up, Num5 as Clear),
// without the extended-key flag the separate arrow and editing keys carry: for the game's bindings
// they are the numpad's keys whatever the Num Lock state (the camera keys Num8 / Num4 / Num5 / Num6).
u32 game_key(WPARAM vk, LPARAM lp) {
  if (lp & (1 << 24)) return u32(vk);
  switch (vk) {
    case VK_INSERT: return VK_NUMPAD0;
    case VK_END: return VK_NUMPAD1;
    case VK_DOWN: return VK_NUMPAD2;
    case VK_NEXT: return VK_NUMPAD3;
    case VK_LEFT: return VK_NUMPAD4;
    case VK_CLEAR: return VK_NUMPAD5;
    case VK_RIGHT: return VK_NUMPAD6;
    case VK_HOME: return VK_NUMPAD7;
    case VK_UP: return VK_NUMPAD8;
    case VK_PRIOR: return VK_NUMPAD9;
    case VK_DELETE: return VK_DECIMAL;
    default: return u32(vk);
  }
}

HCURSOR client_cursor() {
  if (g_mouse_captured && !input_isolated()) return nullptr;
  bool menu = sysdlg::shown() ? sysdlg::cursor_wanted() : game::menu_cursor_visible() || ui::is_open();
  if (menu) {
    static HCURSOR cur = nullptr;
    static UINT cur_dpi = 0;
    UINT dpi = GetDpiForWindow(g_hwnd);
    if (!cur || dpi != cur_dpi) {
      cur = make_menu_cursor(dpi ? dpi : 96);
      cur_dpi = dpi;
    }
    return cur;
  }
  if (g_user_released && game::input_context() == game::InputContext::Gameplay) return LoadCursorW(nullptr, IDC_ARROW);
  return nullptr;
}

void refresh_cursor() {
  POINT p{};
  RECT r{};
  if (!GetCursorPos(&p) || WindowFromPoint(p) != g_hwnd) return;
  ScreenToClient(g_hwnd, &p);
  GetClientRect(g_hwnd, &r);
  if (PtInRect(&r, p)) SetCursor(client_cursor());
}

// Borderless fullscreen: the window without frame covers its monitor (the swap chain follows the
// client size); toggled with F11 or Alt+Enter, restored to the previous window placement.
WINDOWPLACEMENT g_windowed_placement = [] {
  WINDOWPLACEMENT p{};
  p.length = sizeof p;
  return p;
}();
void set_fullscreen(bool on) {
  if (on == g_fullscreen || !g_hwnd) return;
  DWORD style = DWORD(GetWindowLongW(g_hwnd, GWL_STYLE));
  if (on) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetWindowPlacement(g_hwnd, &g_windowed_placement) || !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
      return;
    SetWindowLongW(g_hwnd, GWL_STYLE, LONG((style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
    const UINT test = input_isolated() || cfg().no_activate ? SWP_NOACTIVATE : 0;  // test runs never take the focus
    SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | test);
  } else {
    SetWindowLongW(g_hwnd, GWL_STYLE, LONG((style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW));
    SetWindowPlacement(g_hwnd, &g_windowed_placement);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  g_fullscreen = on;
  if (g_mouse_captured) clip_cursor();
  LOG_I("gpu", "window: %s", on ? "borderless fullscreen" : "windowed");
}

std::vector<Monitor> list_monitors() {
  std::vector<Monitor> out;
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(hm, &mi)) {
      Monitor m;
      std::wstring d = mi.szDevice;
      m.device.assign(d.begin(), d.end());
      m.x = mi.rcMonitor.left;
      m.y = mi.rcMonitor.top;
      m.w = mi.rcMonitor.right - mi.rcMonitor.left;
      m.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
      m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
      reinterpret_cast<std::vector<Monitor>*>(lp)->push_back(m);
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&out));
  return out;
}

void set_client_size(int w, int h);
// Moves the window to the monitor (by device name; "" or an unknown name: the primary) keeping
// its mode: covering it when borderless, centred at the same client size when windowed.
void move_to_monitor(const std::string& device) {
  if (!g_hwnd || exclusive_active()) return;
  std::vector<Monitor> ms = list_monitors();
  const Monitor* target = nullptr;
  for (const Monitor& m : ms)
    if (m.device == device || (!target && m.primary)) target = &m;
  for (const Monitor& m : ms)
    if (m.device == device) target = &m;
  if (!target) return;
  HMONITOR now = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(now, &mi);
  std::wstring d = mi.szDevice;
  if (std::string(d.begin(), d.end()) == target->device) return;
  if (g_fullscreen) {
    SetWindowPos(g_hwnd, HWND_TOP, target->x, target->y, target->w, target->h, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    // Somewhere on the target monitor first (MonitorFromWindow decides the work area), then centred.
    SetWindowPos(g_hwnd, nullptr, target->x + 16, target->y + 16, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    set_client_size(rc.right, rc.bottom);
  }
  LOG_I("gpu", "window moved to monitor %s (%dx%d)", target->device.c_str(), target->w, target->h);
}

void work_area(int* w, int* h) {
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  *w = mi.rcWork.right - mi.rcWork.left;
  *h = mi.rcWork.bottom - mi.rcWork.top;
}

// Windowed: the client area at w x h (or the largest size with that aspect ratio whose frame
// fits the work area), centred on the monitor.
void set_client_size(int w, int h) {
  if (!g_hwnd || exclusive_active()) return;  // applied when the window is a window again
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectEx(&frame, style, FALSE, 0);
  int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  int aw = mi.rcWork.right - mi.rcWork.left - fw, ah = mi.rcWork.bottom - mi.rcWork.top - fh;
  if (w > aw || h > ah) {
    double s = std::min(double(aw) / w, double(ah) / h);
    w = int(w * s);
    h = int(h * s);
  }
  int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - (w + fw)) / 2;
  int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - (h + fh)) / 2;
  if (g_fullscreen) {  // applies when leaving fullscreen
    g_windowed_placement.rcNormalPosition = RECT{x, y, x + w + fw, y + h + fh};
    g_windowed_placement.showCmd = SW_SHOWNORMAL;
    return;
  }
  // A maximized or minimized window is restored first. Not shown here: showing activates, and the
  // window's first show (window_create) decides that (test runs never take the focus).
  if (IsZoomed(g_hwnd) || IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
  SetWindowPos(g_hwnd, nullptr, x, y, w + fw, h + fh, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
  if (g_mouse_captured) clip_cursor();
}

// Mouse look takes the mouse in gameplay while the window is in front, unless F1 gave it back or
// a controller was used after the mouse (window thread).
void capture_for_gameplay() {
  if (g_mouse_captured || g_user_released || ui::is_open() || sysdlg::shown() || game::input_context() != game::InputContext::Gameplay ||
      game::controller_used_last())
    return;
  if (input_isolated() || GetForegroundWindow() == g_hwnd) set_mouse_capture(true);
}

// Focus changes (Alt+Tab, a click into another window): losing the focus gives the mouse back,
// getting it again in gameplay takes it for mouse look without a click.
void on_focus(bool focused) {
  LOG_I("input", "window focus %s", focused ? "back" : "lost");
  if (focused) {
    input_focus(true);
    capture_for_gameplay();
  } else {
    set_mouse_capture(false);
    input_focus(false);
  }
}

struct Call {
  const std::function<void()>* fn;
  bool done = false;
};
std::mutex g_call_mu;
std::condition_variable g_call_cv;
std::deque<Call*> g_calls;

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  const bool dialog = sysdlg::shown();  // a system dialog takes the input first, then PC SETTINGS
  const bool overlay = dialog || ui::is_open();
  switch (msg) {
    case WM_CLOSE:
      if (dialog) sysdlg::abort();  // answered as "back", so the game can go on to its exit
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
    case WM_MOUSEMOVE: {
      if (dialog) sysdlg::on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      else if (overlay) ui::on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ClientToScreen(h, &sp);
      bool moved = sp.x != g_pointer.x || sp.y != g_pointer.y;
      g_pointer = sp;
      if (!overlay && !g_mouse_captured && moved && !input_isolated()) {
        bool shown = game::menu_cursor_visible();
        game::menu_mouse_moved();  // in a menu the cursor appears at once
        if (!shown && game::input_context() == game::InputContext::Menu) SetCursor(client_cursor());
        capture_for_gameplay();  // the mouse moved in gameplay after a controller: mouse look takes it
      }
      break;
    }
    case WM_MOUSEACTIVATE:
      // The click that brings the window to the front only does that in menus and on the title or
      // a movie (it would choose an item or skip the movie); in gameplay it takes the mouse.
      if (LOWORD(lp) == HTCLIENT && GetForegroundWindow() != h && game::input_context() != game::InputContext::Gameplay)
        return MA_ACTIVATEANDEAT;
      break;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(client_cursor());
        return TRUE;
      }
      break;
    case WM_APP + 3:  // window_request_mouse_capture
      if (wp) {
        g_user_released = false;  // a new stretch of gameplay (a mission starts, the pause menu closes)
        capture_for_gameplay();
      } else {
        set_mouse_capture(false);
      }
      refresh_cursor();
      return 0;
    case WM_APP + 4:  // window_refresh_cursor
      refresh_cursor();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
      bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
      int button = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP   ? 0
                   : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? 1
                   : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ? 2
                   : GET_XBUTTON_WPARAM(wp) == XBUTTON1          ? 3
                                                                 : 4;
      if (dialog) {
        sysdlg::on_mouse_button(button, down, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (GetCapture() == h) ReleaseCapture();
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
      }
      if (overlay) {
        ui::on_mouse_button(button, down, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (down && ui::is_open()) SetCapture(h);  // drags in PC SETTINGS
        else ReleaseCapture();                     // also when the press closed it (right click)
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
      }
      if (!down && GetCapture() == h) ReleaseCapture();
      if (!g_mouse_captured && !input_isolated()) {
        // In the game's menus and on its other screens the mouse stays free (cursor, clicks);
        // in gameplay a click takes it for mouse look (isolated test runs never take the cursor).
        if (game::input_context() != game::InputContext::Gameplay) {
          if (button <= 1) game::menu_mouse_button(button, down);
          if (down) SetCursor(client_cursor());
          return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
        }
        if (msg == WM_LBUTTONDOWN) {
          g_user_released = false;
          set_mouse_capture(true);
          return 0;
        }
      }
      break;
    }
    case WM_MOUSEWHEEL:
      if (dialog) {
        sysdlg::on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
      }
      if (overlay) {
        ui::on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
      }
      if (!g_mouse_captured && game::input_context() == game::InputContext::Menu) {
        static int rest = 0;  // high-resolution wheels: notches of 120
        rest += GET_WHEEL_DELTA_WPARAM(wp);
        int notches = rest / WHEEL_DELTA;
        rest -= notches * WHEEL_DELTA;
        if (notches) game::menu_mouse_wheel(notches);
        return 0;
      }
      break;
    case WM_SIZE:
    case WM_MOVE:
      if (g_mouse_captured) clip_cursor();
      break;
    case WM_APP + 1:  // exclusive fullscreen was left: the window takes the configured mode
      settings::apply_window_mode();
      return 0;
    case WM_EXITSIZEMOVE:  // the user resized the window: kept for the next start
      if (!g_fullscreen && !exclusive_active() && !IsIconic(h) && !IsZoomed(h)) {
        RECT rc{};
        GetClientRect(h, &rc);
        if (rc.right >= 320 && rc.bottom >= 180) settings::note_window_size(rc.right, rc.bottom);
      }
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      bool repeat = lp & (1 << 30);
      if (!repeat && (wp == VK_F11 || (wp == VK_RETURN && msg == WM_SYSKEYDOWN && (lp & (1 << 29))))) {
        settings::toggle_fullscreen();
        return 0;  // not passed to the game's key bindings
      }
      if (wp == VK_F1 && !input_isolated()) {  // releases (or takes) the mouse
        if (!repeat && !overlay) {
          g_user_released = g_mouse_captured;  // given back until a click, F1 again or the next gameplay
          set_mouse_capture(!g_mouse_captured);
        }
        return 0;
      }
      if (dialog) {
        sysdlg::on_key(u32(wp), true, repeat);
        return 0;  // keys pressed in a system dialog never reach the game
      }
      if (overlay) {
        ui::on_key(u32(wp), true, repeat);
        return 0;  // keys pressed in PC SETTINGS never reach the game
      }
      input_key(game_key(wp, lp), true);
      if (msg == WM_SYSKEYDOWN && wp != VK_F10 && wp != VK_MENU) break;  // Alt+F4 and the like
      return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
      input_key(game_key(wp, lp), false);  // releases always reach the key state (no stuck keys)
      if (overlay && !dialog) ui::on_key(u32(wp), false, false);
      if (msg == WM_SYSKEYUP && wp != VK_F10 && wp != VK_MENU) break;
      return 0;
    case WM_CHAR:
      if (dialog) sysdlg::on_char(u32(wp));  // text entry
      return 0;
    case WM_SYSCHAR:
      return 0;  // no menu beep for Alt+key
    case WM_KILLFOCUS:
    case WM_SETFOCUS:
      if (!input_isolated()) on_focus(msg == WM_SETFOCUS);  // test runs: only the control file's "focus" lines
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void window_dpi_aware() {
  using SetCtx = BOOL(WINAPI*)(HANDLE);
  if (auto fn = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
    fn(reinterpret_cast<HANDLE>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

bool window_create(const std::string& title, int w, int h, bool visible) {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
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
  GetCursorPos(&g_pointer);  // where the pointer rests: no move yet
  set_client_size(w, h);  // fitted to the monitor, centred
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;  // generic desktop
  rid.usUsage = 0x02;      // mouse
  rid.hwndTarget = g_hwnd;
  if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
    LOG_W("input", "raw mouse input unavailable (RegisterRawInputDevices failed, error %lu)", GetLastError());
  if (visible && (input_isolated() || cfg().no_activate)) {
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);  // test runs never take the focus
  } else if (visible) {
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);  // granted when the launcher (in front) started the game
  }
  return true;
}

void window_set_fullscreen(bool on) {
  run_on_window_thread([on] { set_fullscreen(on); });
}
bool window_is_fullscreen() { return g_fullscreen.load(); }

void window_set_client_size(int w, int h) {
  run_on_window_thread([w, h] { set_client_size(w, h); });
}
void window_client_size(int* w, int* h) {
  if (exclusive_mode_size(w, h)) return;  // what is on screen: the display mode (the client area can be larger)
  RECT rc{};
  if (g_hwnd) GetClientRect(g_hwnd, &rc);
  *w = rc.right;
  *h = rc.bottom;
}
void window_work_area(int* w, int* h) {
  *w = *h = 0;
  if (g_hwnd) work_area(w, h);
}
void window_monitor_size(int* w, int* h) {
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  *w = *h = 0;
  if (g_hwnd && GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  }
}

std::vector<Monitor> window_monitors() { return list_monitors(); }
void window_move_to_monitor(const std::string& device) {
  run_on_window_thread([device] { move_to_monitor(device); });
}

void window_request_mouse_capture(bool on) {
  if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 3, on ? 1 : 0, 0);
}
void window_refresh_cursor() {
  if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 4, 0, 0);
}

void window_set_mouse_capture(bool on) {
  run_on_window_thread([on] {
    if (!on || !input_isolated()) set_mouse_capture(on);
  });
}
bool window_mouse_captured() { return g_mouse_captured.load(); }

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
  ui::tick();
  sysdlg::tick();
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
