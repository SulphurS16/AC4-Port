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
// Runs fn on the window's thread during its next window_pump() and waits for it to finish
// (directly when called on that thread). Returns false if there is no window.
bool run_on_window_thread(const std::function<void()>& fn);

}  // namespace rt::gpu
