// Host window (created and pumped on the process main thread).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace rt::gpu {

// Per-monitor DPI awareness for the process (window sizes are physical pixels); call before
// creating windows.
void window_dpi_aware();
bool window_create(const std::string& title, int client_w, int client_h, bool visible);
void* window_handle();   // HWND
bool window_pump();      // processes pending messages; false once the window was destroyed
bool window_close_requested();
void window_set_title(const std::string& title);
// Borderless fullscreen on the window's monitor (also toggled with F11 or Alt+Enter).
void window_set_fullscreen(bool on);
bool window_is_fullscreen();
// Client size in windowed mode (fitted to the monitor's work area, centred on it).
void window_set_client_size(int w, int h);
void window_client_size(int* w, int* h);
// Work area of the monitor the window is on (the largest window that fits), and its full size.
void window_work_area(int* w, int* h);
void window_monitor_size(int* w, int* h);
// The monitors (in Windows' order) and moving the window to one of them by device name (windowed:
// centred there; borderless: covering it; exclusive fullscreen then uses its output).
struct Monitor {
  std::string device;  // e.g. \\.\DISPLAY2
  int x = 0, y = 0, w = 0, h = 0;
  bool primary = false;
};
std::vector<Monitor> window_monitors();
void window_move_to_monitor(const std::string& device);
// Mouse capture for mouse look (hidden cursor confined to the window; click into the window
// to capture, focus loss releases it).
void window_set_mouse_capture(bool on);
bool window_mouse_captured();
// Any thread, asynchronous: mouse capture for mouse look on or off (the game's menus give the
// mouse back, gameplay takes it), and the cursor's look brought up to date (the menu cursor).
void window_request_mouse_capture(bool on);
void window_refresh_cursor();
// Runs fn on the window's thread during its next window_pump() and waits for it to finish
// (directly when called on that thread). Returns false if there is no window.
bool run_on_window_thread(const std::function<void()>& fn);

}  // namespace rt::gpu
