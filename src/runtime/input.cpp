// Host input state (keyboard and mouse from the window; gamepads are polled by the pad module).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "runtime/config.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/input.h"
#include "runtime/log.h"

namespace rt {
namespace {
std::atomic<bool> g_keys[256];
std::atomic<bool> g_focus{true};
std::atomic<s64> g_mouse_dx{0}, g_mouse_dy{0};
std::atomic<s32> g_wheel{0};
std::atomic<u32> g_mouse_buttons{0};
std::atomic<bool> g_captured{false};
s32 g_abs_x = -1, g_abs_y = -1;  // last absolute position (remote desktop, tablets)
}  // namespace

namespace gpu {
void input_key(u32 vk, bool down) {
  if (vk < 256 && !input_isolated()) g_keys[vk] = down;
}
void input_focus(bool focused) {
  if (input_isolated()) return;
  g_focus = focused;
  if (!focused)
    for (auto& k : g_keys) k = false;
}
void input_mouse_raw(u16 flags, s32 x, s32 y, u16 bf, u16 bd) {
  if (!g_captured || input_isolated()) return;
  if (flags & MOUSE_MOVE_ABSOLUTE) {
    // Normalized 0..65535 over the (virtual) screen: the difference, in screen pixels.
    bool virt = flags & MOUSE_VIRTUAL_DESKTOP;
    s32 w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
    s32 px = s32(s64(x) * w / 65535), py = s32(s64(y) * h / 65535);
    if (g_abs_x >= 0) {
      g_mouse_dx += px - g_abs_x;
      g_mouse_dy += py - g_abs_y;
    }
    g_abs_x = px;
    g_abs_y = py;
  } else {
    g_mouse_dx += x;
    g_mouse_dy += y;
  }
  u32 b = g_mouse_buttons.load();
  for (int i = 0; i < 5; ++i) {
    if (bf & (1u << (2 * i))) b |= 1u << i;
    if (bf & (2u << (2 * i))) b &= ~(1u << i);
  }
  g_mouse_buttons = b;
  if (bf & RI_MOUSE_WHEEL) g_wheel += s16(bd);
}
void input_mouse_capture(bool captured) {
  g_captured = captured;
  g_mouse_buttons = 0;
  g_abs_x = g_abs_y = -1;
  LOG_I("input", "mouse %s", captured ? "captured (F1 releases it)" : "released");
}
}  // namespace gpu

bool input_isolated() { return cfg().isolate_input; }
void input_key_inject(u32 vk, bool down) {
  if (vk < 256) g_keys[vk] = down;
}
bool input_key_down(u32 vk) { return vk < 256 && g_keys[vk].load(); }
bool input_has_focus() { return g_focus.load(); }

void input_mouse_take(s64* dx, s64* dy) {
  *dx = g_mouse_dx.exchange(0);
  *dy = g_mouse_dy.exchange(0);
}
bool input_mouse_button(int i) { return i >= 0 && i < 5 && ((g_mouse_buttons.load() >> i) & 1); }
s32 input_mouse_wheel_take() { return g_wheel.exchange(0); }
bool input_mouse_captured() { return g_captured.load(); }
void input_mouse_inject(s64 dx, s64 dy) {
  g_mouse_dx += dx;
  g_mouse_dy += dy;
}
void input_mouse_button_inject(int i, bool down) {
  if (i < 0 || i >= 5) return;
  if (down) g_mouse_buttons |= 1u << i;
  else g_mouse_buttons &= ~(1u << i);
}

}  // namespace rt
