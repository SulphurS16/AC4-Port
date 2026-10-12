// Host window (created and pumped on the process main thread).
#pragma once
#include <functional>
#include <string>

namespace rt::gpu {

bool window_create(const std::string& title, int client_w, int client_h, bool visible);
void* window_handle();   // HWND
bool window_pump();      // processes pending messages; false once the window was destroyed
bool window_close_requested();
void window_set_title(const std::string& title);
// Borderless fullscreen on the window's display (also F11 / Alt+Enter), or back to the window.
// activate = false (tests): the window does not take the keyboard focus.
void window_set_fullscreen(bool on, bool activate = true);
// Resizes the window's client area (tests; leaves fullscreen first).
void window_set_client_size(int w, int h);
// Minimizes or restores the window without changing which window is active (tests).
void window_set_minimized(bool on);
// Runs fn on the window's thread during its next window_pump() and waits for it to finish
// (directly when called on that thread). Returns false if there is no window.
bool run_on_window_thread(const std::function<void()>& fn);

}  // namespace rt::gpu
