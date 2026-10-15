// Mouse and keyboard in the game's own menus (GAME_NOTES "Menu dialogs"; the design of the ACFA
// port's menu_mouse.cpp, with AC4's addresses and layouts). The game's menus are dialogs of
// objects (buttons, list rows, sliders) laid out in the console's 1280x720 picture: the same
// dialog framework as ACFA's (CMenuDialog 0x150 bytes, CMenuDlgObj 0x30, CMenuSliderCtrl 0x58,
// CMenuListCtrl 0x60). Each frame, before the game turns pad presses into key messages
// (0x91af7c), a probe message with a code the game does not use goes through the game's own
// routing (the framework's vtable slot +20 -> the application: the system dialogs' manager, then
// the scene chain, whose leaf scene has a dialog manager or a menu object); the two places where a
// key reaches a dialog are hooked (the managers' dispatcher 0x5de750, the menu objects' 0x640e98):
// they stop the probe and note the dialog a key would reach. The cursor then focuses the item
// under it through the dialog's own SetFocus (the D-pad's path, with its highlight, help line and
// notifications); a click presses Cross after focusing what was clicked, a right click Circle,
// the wheel the D-pad. Nothing here changes the game's state other than through those calls and
// presses.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime/game/menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <set>
#include <string>

#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/game/frame_policy.h"
#include "runtime/game/system_menu.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/sysdialog.h"
#include "runtime/ui/overlay.h"

namespace rt::game {
namespace {

using ppu::Ctx;

constexpr u32 kKeyMessage = 384;
constexpr u32 kProbeCode = 0x58ff;  // no key of the game (its codes: 0x5800-0x5833, table 0x1005fe68)

// Guest layout (GAME_NOTES "Menu dialogs"); ACFA's slots in brackets.
constexpr u32 kFrameworkDispatch = 20;  // framework vtable: dispatch(framework, message) [+24]
constexpr u32 kMgrStack = 4, kMgrDepth = 132, kMgrTable = 268, kMgrTableCount = 524;
constexpr u32 kMenuDlgA = 272, kMenuDlgB = 276, kMenuWhich = 284;  // a scene menu object: +284 == 1 ? +272 : +276
// The menu object's panel switch (0x6446c8, the hub's Left/Right: r4 = 1 for +272, 0 for +276).
constexpr u32 kMenuSetPanel = 0x00e78c88;
constexpr u32 kDlgBusy = 64;      // dialog vtable: nonzero while it takes no input [+72]
constexpr u32 kDlgSetFocus = 44;  // dialog vtable: SetFocus(dialog, object) [+48]
constexpr u32 kDlgChildren = 24, kDlgChildCount = 28, kDlgFocus = 128;
constexpr u32 kDlgRect = 136;  // the dialog on screen (floats l, t, r, b); its objects' rects are relative to it
constexpr u32 kObjId = 4;          // -1: an unnamed part
constexpr u32 kObjFocusable = 52;  // object vtable: focusable [+56]
constexpr u32 kObjRect = 56;       // object vtable: rect (left, top, right, bottom) by hidden pointer [+52]
constexpr u32 kObjBox = 48;        // object vtable: the whole box (a list's rect is its selected row)
constexpr u32 kObjClass = 8;       // object vtable: the runtime class (descriptor of its getter)
// Runtime class getters (their descriptors; derived lists share CMenuListCtrl's): CMenuSliderCtrl
// 0x1012ab38, CMenuListCtrl 0x1012ab28.
constexpr u32 kSliderClass = 0x00e75db0, kListClass = 0x00e75dd8;
// CMenuListCtrl (0x60 bytes): selected row +52, rows on a page +56, rows +60; the first row shown
// is the selection less half a page, within the list (its rect, 0x5f5a30).
constexpr u32 kListRow = 52, kListPage = 56, kListCount = 60;
// The list's key handler (0x5f9a7c) moves the selection with 0x5f97a0 (r3 list, r4 row; returns the
// row selected), then tells the parent dialog with 0x6a88b4 (r3 object, r4 code 5).
constexpr u32 kListSelect = 0x00e76048, kObjNotify = 0x00e7bd10;

// CMenuSliderCtrl (0x58 bytes, set up by 0x5f4f20): its style +48 (+24: 0 horizontal), min +52,
// max +56, value +60, step +64 (ints), the bar +68 (floats l, t, r, b: also its rect). Its key
// handler (0x5f3ecc) moves the value a step per Left/Right press (ten when held).
constexpr u32 kSliderStyle = 48, kStyleVertical = 24;
constexpr u32 kSliderMin = 52, kSliderMax = 56, kSliderValue = 60, kSliderStep = 64, kSliderBar = 68;

// Pad bits (cellPad digital words).
constexpr u16 kStart = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
constexpr u16 kCircle = 0x20, kCross = 0x40;

s64 now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<int> g_context{int(InputContext::Screen)};
std::atomic<s64> g_mouse_at{0}, g_pad_at{0};  // last use of each (ms)
std::atomic<s64> g_key_at{0};                 // last key pressed in a menu (hides the cursor, not mouse look)
std::atomic<bool> g_cursor{false};             // menu cursor shown (window thread reads)

// Mouse events from the window thread, handled on the game's main thread at its next frame.
struct Event {
  enum Kind { LeftDown, LeftUp, Right, Wheel } kind;
  int value;  // wheel notches
};
std::mutex g_mu;
std::deque<Event> g_events;
// Presses made for the mouse: active from `at` for 70 ms (two game frames at least); screen_only:
// dropped once a menu or gameplay has the input (cross_after_start).
struct Press {
  u16 d1, d2;
  s64 at;
  bool screen_only = false;
};
std::deque<Press> g_presses;
std::atomic<u16> g_hold{0};  // a direction held for a slider far from the cursor's value

// A slider being dragged (main thread only).
struct Drag {
  u32 slider = 0;  // 0: none
  bool button = false;  // the left button still down
  float target = 0;     // the value the cursor points at
  s64 next_tap = 0;
  s64 released = 0;  // when the button came up (the drive gives up 2 s later)
};
Drag g_drag;

// Main thread only.
bool g_probing = false;
u32 g_probe_dialog = 0;
u32 g_probe_object = 0;  // the scene menu object the probe reached (0: a dialog manager)
u32 g_last_dialog = 0;
int g_last_x = -1, g_last_y = -1;

bool valid(u32 a, u32 size = 4) { return a >= 0x10000 && !(a & 3) && vm::committed(a, size); }

// Calls the guest function behind descriptor `opd` with r3, r4 on a frame below the current stack
// pointer (leaving the 0x100 bytes under it to the caller); every register is restored after.
u64 call(Ctx& ctx, u32 opd, u64 r3, u64 r4 = 0, u64 r5 = 0, u64 r6 = 0) {
  if (!valid(opd, 8)) return 0;
  Ctx saved = ctx;
  u32 sp = (u32(ctx.r[1]) - 0x300) & ~15u;
  ppu::w64(sp, ctx.r[1]);  // back chain
  ctx.r[1] = sp;
  ctx.r[2] = ppu::r32(opd + 4);
  ctx.r[3] = r3;
  ctx.r[4] = r4;
  ctx.r[5] = r5;
  ctx.r[6] = r6;
  ppu::icall(ctx, ppu::r32(opd));
  u64 ret = ctx.r[3];
  ctx = saved;
  return ret;
}
u32 vfunc(u32 obj, u32 slot) {
  if (!valid(obj)) return 0;
  u32 vt = ppu::r32(obj);
  return valid(vt + slot) ? ppu::r32(vt + slot) : 0;
}

bool is_probe(u32 msg) { return g_probing && valid(msg, 8) && ppu::r32(msg) == kKeyMessage && ppu::r32(msg + 4) == kProbeCode; }

// The probe reached a manager: the dialog that a key would reach, as 0x5de750 decides it.
bool probe_manager(Ctx& ctx, u32 mgr) {
  if (!valid(mgr) || !valid(mgr + kMgrTableCount)) return false;
  u32 depth = ppu::r32(mgr + kMgrDepth);
  if (depth == 0 || depth > 32) return false;
  u32 top = ppu::r32(mgr + kMgrStack + 4 * (depth - 1));
  u32 n = ppu::r32(mgr + kMgrTableCount);
  bool enabled = false, listed = false;
  for (u32 i = 0; i < n && i < 32 && !listed; ++i) {
    u32 e = mgr + kMgrTable + 8 * i;
    if (ppu::r32(e + 4) == top) {
      listed = true;
      enabled = ppu::r32(e) == 1;
    }
  }
  if (!listed || !enabled) return false;
  if (call(ctx, vfunc(top, kDlgBusy), top) != 0) return false;
  g_probe_dialog = top;
  return true;
}

// A decide key (Cross sends 0x5801, then 0x5818) for the SYSTEM menu's PC SETTINGS row (the
// focus of the manager's top dialog; system_menu.cpp): the port's screen opens instead.
bool pc_settings_chosen(u32 mgr, u32 msg) {
  if (!valid(msg, 8) || ppu::r32(msg) != kKeyMessage) return false;
  u32 code = ppu::r32(msg + 4);
  if ((code != 0x5801 && code != 0x5818) || !valid(mgr + kMgrTableCount)) return false;
  u32 depth = ppu::r32(mgr + kMgrDepth);
  if (depth == 0 || depth > 32) return false;
  u32 top = ppu::r32(mgr + kMgrStack + 4 * (depth - 1));
  u32 focus = valid(top + kDlgFocus) ? ppu::r32(top + kDlgFocus) : 0;
  return valid(focus + kObjId) && ppu::r32(focus + kObjId) == kPcSettingsButton;
}

void dispatch_hook(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 msg = u32(ctx.r[4]);
  if (is_probe(msg)) {
    ctx.r[3] = probe_manager(ctx, u32(ctx.r[3])) ? 1 : 0;  // 1: the routing stops here
    return;
  }
  if (pc_settings_chosen(u32(ctx.r[3]), msg)) {
    if (ppu::r32(msg + 4) == 0x5801) {
      LOG_I("game", "SYSTEM menu: PC SETTINGS chosen");
      ui::open_from_menu();
    }
    ctx.r[3] = 1;  // taken: the game never sees a decide key on the added row
    return;
  }
  orig(ctx);
}

// The menu objects' way (the hub's menus): a key straight to one of the object's two dialogs.
void menu_object_hook(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 msg = u32(ctx.r[4]), obj = u32(ctx.r[3]);
  if (!is_probe(msg)) {
    orig(ctx);
    return;
  }
  u32 dialog = 0;
  if (valid(obj + kMenuWhich)) dialog = ppu::r32(obj + (ppu::r32(obj + kMenuWhich) == 1 ? kMenuDlgA : kMenuDlgB));
  if (valid(dialog) && call(ctx, vfunc(dialog, kDlgBusy), dialog) == 0) g_probe_dialog = dialog;
  g_probe_object = obj;
  ctx.r[3] = 1;  // the routing stops here either way (as the game's would: this object takes keys)
}

// The base dialog key handler (0x6a19a0: the key to the focus object's +36): the end of every
// other route (scenes that hold their dialogs themselves, e.g. ACSIS at scene +296 / +304).
void dialog_key_hook(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 msg = u32(ctx.r[4]), dialog = u32(ctx.r[3]);
  if (!is_probe(msg)) {
    orig(ctx);
    return;
  }
  if (!g_probe_dialog && valid(dialog + kDlgFocus) && valid(ppu::r32(dialog + kDlgFocus))) g_probe_dialog = dialog;
  ctx.r[3] = 1;
}

// Client pixels -> the game's 1280x720 picture (the frame is fitted to the window, centred).
bool to_game(int cx, int cy, float* gx, float* gy) {
  int w = 0, h = 0;
  gpu::window_client_size(&w, &h);
  if (w <= 0 || h <= 0) return false;
  float s = std::min(float(w) / 1280.0f, float(h) / 720.0f);
  float x0 = (float(w) - 1280.0f * s) / 2, y0 = (float(h) - 720.0f * s) / 2;
  *gx = (float(cx) - x0) / s;
  *gy = (float(cy) - y0) / s;
  return true;
}

struct Rect {
  float l = 0, t = 0, r = 0, b = 0;
  bool contains(float x, float y) const { return r > l && b > t && x >= l && x < r && y >= t && y < b; }
};
Rect rect_by(Ctx& ctx, u32 obj, u32 slot) {
  u32 out = (u32(ctx.r[1]) - 0x40) & ~15u;  // inside the 0x100 bytes call() leaves free
  for (int k = 0; k < 4; ++k) ppu::w32(out + 4 * k, 0);
  call(ctx, vfunc(obj, slot), out, obj);
  return {float(ppu::rf32(out)), float(ppu::rf32(out + 4)), float(ppu::rf32(out + 8)), float(ppu::rf32(out + 12))};
}
Rect rect_of(Ctx& ctx, u32 obj) { return rect_by(ctx, obj, kObjRect); }

// Picture coordinates -> the dialog's own (a pop-up's objects are placed relative to its corner).
void local(u32 dialog, float* x, float* y) {
  if (!valid(dialog + kDlgRect + 8)) return;
  *x -= float(ppu::rf32(dialog + kDlgRect));
  *y -= float(ppu::rf32(dialog + kDlgRect + 4));
}

bool is_class(u32 obj, u32 cls) {
  u32 vt = valid(obj) ? ppu::r32(obj) : 0;
  return valid(vt + kObjClass) && ppu::r32(vt + kObjClass) == cls;
}
bool is_list(u32 obj) { return is_class(obj, kListClass) && valid(obj + kListCount); }
// A horizontal slider (vertical ones are left to the keys).
bool is_slider(u32 obj) {
  if (!is_class(obj, kSliderClass) || !valid(obj + kSliderBar + 16)) return false;
  u32 style = ppu::r32(obj + kSliderStyle);
  return valid(style + kStyleVertical) && ppu::r32(style + kStyleVertical) == 0;
}
Rect slider_bar(u32 s) {
  return {float(ppu::rf32(s + kSliderBar)), float(ppu::rf32(s + kSliderBar + 4)), float(ppu::rf32(s + kSliderBar + 8)),
          float(ppu::rf32(s + kSliderBar + 12))};
}
// Where an object takes the cursor: a list's whole box (its rect is the selected row).
Rect hit_rect(Ctx& ctx, u32 obj) { return is_list(obj) ? rect_by(ctx, obj, kObjBox) : rect_of(ctx, obj); }

// The first row a list shows: the selection less half a page, within the list (0x5f5a30).
s32 list_top(s32 row, s32 page, s32 count) { return std::clamp(row - page / 2, 0, std::max(0, count - page)); }

// The row of `list` under (gx, gy), or -1; *safe: selecting it does not scroll the list.
s32 list_row_at(Ctx& ctx, u32 list, float gx, float gy, bool* safe) {
  Rect box = rect_by(ctx, list, kObjBox);
  s32 page = s32(ppu::r32(list + kListPage)), count = s32(ppu::r32(list + kListCount)), row = s32(ppu::r32(list + kListRow));
  if (page <= 0 || page > 64 || count <= 0 || !box.contains(gx, gy)) return -1;
  s32 top = list_top(row, page, count);
  s32 visible = s32((gy - box.t) / ((box.b - box.t) / float(page)));
  if (visible < 0 || visible >= page || top + visible >= count) return -1;
  s32 target = top + visible;
  *safe = list_top(target, page, count) == top;
  return target;
}

// Selects row `target` of `list` as its key handler does. Returns whether the list scrolled.
bool list_select(Ctx& ctx, u32 list, s32 target) {
  s32 row = s32(ppu::r32(list + kListRow)), page = s32(ppu::r32(list + kListPage)), count = s32(ppu::r32(list + kListCount));
  if (target == row) return false;
  s32 now = s32(call(ctx, kListSelect, list, u64(target)));
  if (now != row) call(ctx, kObjNotify, list, 5);
  return list_top(now, page, count) != list_top(row, page, count);
}

// The item of `dialog` under (gx, gy): named items (an ID: what the D-pad moves between) before
// unnamed parts, then the smallest. `slider`: a slider whose bar is under the point, if any.
u32 object_at(Ctx& ctx, u32 dialog, float gx, float gy, u32* slider = nullptr) {
  if (slider) *slider = 0;
  u32 arr = ppu::r32(dialog + kDlgChildren), n = ppu::r32(dialog + kDlgChildCount);
  if (n == 0 || n > 1024 || !valid(arr, 4 * n)) return 0;
  local(dialog, &gx, &gy);
  u32 best = 0;
  int best_rank = -1;
  float best_area = 0;
  for (u32 i = 0; i < n; ++i) {
    u32 obj = ppu::r32(arr + 4 * i);
    if (!valid(obj, 16)) continue;
    if (call(ctx, vfunc(obj, kObjFocusable), obj) == 0) continue;
    Rect r = hit_rect(ctx, obj);
    if (slider && is_slider(obj)) {  // the bar, with a margin for the cursor
      Rect bar = slider_bar(obj);
      if (bar.r > bar.l && gx >= bar.l - 8 && gx < bar.r + 8 && gy >= bar.t - 12 && gy < bar.b + 12) {
        *slider = obj;
        r = {bar.l - 8, bar.t - 12, bar.r + 8, bar.b + 12};
      }
    }
    if (!r.contains(gx, gy)) continue;
    int rank = ppu::r32(obj + kObjId) != 0xffffffffu ? 1 : 0;
    float area = (r.r - r.l) * (r.b - r.t);
    if (rank > best_rank || (rank == best_rank && area < best_area)) {
      best = obj;
      best_rank = rank;
      best_area = area;
    }
  }
  return best;
}

void press(u16 d1, u16 d2, s64 at, bool screen_only = false) {
  std::lock_guard lk(g_mu);
  g_presses.push_back({d1, d2, at, screen_only});
}
}  // namespace

void cross_after_start() { press(0, kCross, now_ms() + 200, true); }

namespace {

// The dragged slider follows the cursor: the D-pad held toward the target while it is ten steps
// away or more (the game speeds up a held key), single presses nearer; done within half a step.
void drive_slider(float gx) {
  u32 s = g_drag.slider;
  Rect bar = slider_bar(s);
  s32 lo = s32(ppu::r32(s + kSliderMin)), hi = s32(ppu::r32(s + kSliderMax)), v = s32(ppu::r32(s + kSliderValue));
  s32 step = std::max<s32>(1, s32(ppu::r32(s + kSliderStep)));
  s64 t = now_ms();
  if (!(bar.r > bar.l) || hi <= lo || (!g_drag.button && t - g_drag.released > 2000)) {
    g_drag = {};
    g_hold = 0;
    return;
  }
  if (g_drag.button) g_drag.target = float(lo) + std::clamp((gx - bar.l) / (bar.r - bar.l), 0.0f, 1.0f) * float(hi - lo);
  float delta = g_drag.target - float(v);
  u16 dir = delta > 0 ? kRight : kLeft;
  if (std::fabs(delta) <= float(step) / 2) {
    g_hold = 0;
    if (!g_drag.button) g_drag = {};  // released and there
    return;
  }
  if (std::fabs(delta) >= 10.0f * float(step)) {
    g_hold = dir;
    return;
  }
  g_hold = 0;
  if (t >= g_drag.next_tap) {
    press(dir, 0, t);
    g_drag.next_tap = t + 110;  // a press and a release frame or two
  }
}

// One frame of the menu mouse on the game's main thread (before the pads become key messages).
void frame(Ctx& ctx) {
  u32 framework = u32(ctx.r[3]);
  if (ui::is_open() || sysdlg::shown() || !valid(framework)) return;
  // Probe: which dialog takes keys now.
  u32 msg = (u32(ctx.r[1]) - 0x80) & ~15u;
  ppu::w32(msg, kKeyMessage);
  ppu::w32(msg + 4, kProbeCode);
  g_probe_dialog = 0;
  g_probe_object = 0;
  g_probing = true;
  call(ctx, vfunc(framework, kFrameworkDispatch), framework, msg);
  g_probing = false;
  u32 dialog = g_probe_dialog;
  bool menu = dialog && valid(dialog + kDlgFocus) && ppu::r32(dialog + kDlgFocus) != 0;
  InputContext c = menu ? InputContext::Menu : mission_running() ? InputContext::Gameplay : InputContext::Screen;
  InputContext was = InputContext(g_context.exchange(int(c)));
  bool mouse = g_mouse_at.load() > std::max(g_pad_at.load(), g_key_at.load());
  bool focus = input_has_focus();
  bool show = c == InputContext::Menu && mouse && focus;
  if (bool before = g_cursor.load(); c != was || show != before) {
    g_cursor = show;
    if (c != was) LOG_I("input", "input context %s", c == InputContext::Menu ? "menu" : c == InputContext::Gameplay ? "gameplay" : "screen");
    // Mouse look takes the mouse in gameplay; everything else gives it back (menus, the mission's
    // report, movies, loading).
    if (c == InputContext::Gameplay && was != InputContext::Gameplay) gpu::window_request_mouse_capture(true);
    else if (c != InputContext::Gameplay && was == InputContext::Gameplay) gpu::window_request_mouse_capture(false);
    gpu::window_refresh_cursor();
  }
  std::deque<Event> events;
  {
    std::lock_guard lk(g_mu);
    events.swap(g_events);
  }
  // Screens without a menu (title, movies, the mission report): a click is Start, then Cross as
  // the menu's confirm keys (cross_after_start).
  if (c == InputContext::Screen) {
    for (const Event& e : events)
      if (e.kind == Event::LeftDown) {
        press(kStart, 0, now_ms());
        if (!movie_playing()) cross_after_start();
      }
    g_last_dialog = dialog;
    return;
  }
  if (c != InputContext::Menu || dialog != g_last_dialog) {  // the dragged slider left with its dialog
    g_drag = {};
    g_hold = 0;
  }
  if (c != InputContext::Menu) {
    g_last_dialog = dialog;
    return;
  }
  // Hover: the item under the cursor takes the focus when the cursor moved or was clicked.
  POINT p{};
  float gx = 0, gy = 0;
  bool have = false;
  HWND hwnd = static_cast<HWND>(gpu::window_handle());
  if (!have && (!hwnd || !GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !to_game(p.x, p.y, &gx, &gy))) return;
  bool moved = p.x != g_last_x || p.y != g_last_y || dialog != g_last_dialog;
  g_last_x = p.x;
  g_last_y = p.y;
  g_last_dialog = dialog;
  bool down = std::any_of(events.begin(), events.end(), [](const Event& e) { return e.kind == Event::LeftDown; });
  u32 hit = 0, slider = 0;
  s32 row = -1;  // a list's row under the cursor
  bool row_safe = false;
  if (show && (moved || down) && !(g_drag.slider && g_drag.button)) {
    hit = object_at(ctx, dialog, gx, gy, &slider);
    // A menu object's other panel (the hub's mission list and its buttons): switched to as Left
    // and Right do when the cursor is on one of its items.
    u32 obj = g_probe_object;
    if (!hit && valid(obj + kMenuWhich)) {
      bool a = ppu::r32(obj + kMenuWhich) == 1;
      u32 other = ppu::r32(obj + (a ? kMenuDlgB : kMenuDlgA));
      if (valid(other + kDlgFocus) && other != dialog) {
        if (u32 h = object_at(ctx, other, gx, gy)) {
          call(ctx, kMenuSetPanel, obj, a ? 0 : 1);
          if (ppu::r32(obj + kMenuWhich) == (a ? 0u : 1u)) {
            dialog = other;
            g_last_dialog = other;
            hit = h;
          }
        }
      }
    }
    if (hit && hit != ppu::r32(dialog + kDlgFocus)) {
      call(ctx, vfunc(dialog, kDlgSetFocus), dialog, hit);
    }
    if (hit && is_list(hit)) {
      float lx = gx, ly = gy;
      local(dialog, &lx, &ly);
      row = list_row_at(ctx, hit, lx, ly, &row_safe);
      // Hover selects rows that do not scroll the list; after a scroll the cursor has to move a
      // row's height before hover selects again (no runaway scrolling under a resting cursor).
      static float scrolled_y = -1e9f;
      if (row >= 0 && row_safe && !down && std::fabs(ly - scrolled_y) > 40 && list_select(ctx, hit, row)) scrolled_y = ly;
    }
  }
  s64 t = now_ms();
  for (const Event& e : events) {
    if (e.kind == Event::LeftDown) {
      if (slider) {  // on a slider's bar: the slider follows the cursor while the button is down
        g_drag = {slider, true, 0, 0, 0};
      } else if (hit) {
        if (row >= 0) list_select(ctx, hit, row);  // a list's row: selected, then chosen
        press(0, kCross, t + (row >= 0 ? 50 : 0));  // a click on an item chooses it; elsewhere nothing
      }
    } else if (e.kind == Event::LeftUp) {
      if (g_drag.button) g_drag.released = t;
      g_drag.button = false;
    } else if (e.kind == Event::Right) {
      press(0, kCircle, t);
    } else if (e.kind == Event::Wheel) {
      for (int i = 0; i < std::abs(e.value); ++i, t += 90) press(e.value > 0 ? kUp : kDown, 0, t);
    }
  }
  if (g_drag.slider) {
    float lx = gx, ly = gy;
    local(dialog, &lx, &ly);
    drive_slider(lx);
  }
}

}  // namespace

InputContext input_context() { return InputContext(g_context.load(std::memory_order_relaxed)); }

void menu_mouse_moved() {
  g_mouse_at = now_ms();
  if (input_context() == InputContext::Menu && input_has_focus()) g_cursor = true;  // at once; frame() agrees
}

void menu_mouse_button(int button, bool down) {
  g_mouse_at = now_ms();
  if (button != 0 && button != 1) return;
  if (button == 1 && !down) return;
  std::lock_guard lk(g_mu);
  g_events.push_back({button == 1 ? Event::Right : down ? Event::LeftDown : Event::LeftUp, 0});
}

void menu_mouse_wheel(int notches) {
  g_mouse_at = now_ms();
  if (!notches) return;
  std::lock_guard lk(g_mu);
  g_events.push_back({Event::Wheel, notches});
}

bool menu_cursor_visible() { return g_cursor.load(std::memory_order_relaxed); }

bool controller_used_last() { return g_pad_at.load() > g_mouse_at.load(); }

void note_pad_input() {
  s64 t = now_ms();
  if (g_pad_at.exchange(t) < g_mouse_at.load() && g_cursor.load()) {
    g_cursor = false;  // the controller took over: the cursor hides at once
    gpu::window_refresh_cursor();
  }
}

void note_menu_key() {
  g_key_at = now_ms();
  if (g_cursor.exchange(false)) gpu::window_refresh_cursor();  // the keys took over until the mouse moves
}

void menu_pad_bits(u16* d1, u16* d2) {
  s64 t = now_ms();
  *d1 |= g_hold.load(std::memory_order_relaxed);
  std::lock_guard lk(g_mu);
  g_presses.erase(std::remove_if(g_presses.begin(), g_presses.end(), [&](const Press& p) { return p.at + 70 <= t; }), g_presses.end());
  const bool screen = g_context.load(std::memory_order_relaxed) == int(InputContext::Screen);
  for (const Press& p : g_presses) {
    if (p.at > t || (p.screen_only && !screen)) continue;
    *d1 |= p.d1;
    *d2 |= p.d2;
  }
}

}  // namespace rt::game

namespace ppu {

// 0x91af7c (r3 the framework), every frame on the main thread: the menu mouse first, then the
// pads become key messages {384, code} (table 0x1005fe68) dispatched through the framework's
// vtable slot +20.
void hook_menu_frame(Ctx& ctx, void (*orig)(Ctx&)) {
  rt::game::frame(ctx);
  orig(ctx);
}
// 0x5de750 (r3 a dialog manager, r4 the message): a key to the manager's top dialog when it takes
// input (the system dialogs' manager and scenes' managers alike).
void hook_menu_dispatch(Ctx& ctx, void (*orig)(Ctx&)) { rt::game::dispatch_hook(ctx, orig); }
// 0x5de614: the same dispatcher taking the message by value (r4 still points at it).
void hook_menu_dispatch_v(Ctx& ctx, void (*orig)(Ctx&)) { rt::game::dispatch_hook(ctx, orig); }
// 0x640e98 (r3 a scene's menu object, r4 the message): the hub's menus (two dialogs, no manager).
void hook_menu_object(Ctx& ctx, void (*orig)(Ctx&)) { rt::game::menu_object_hook(ctx, orig); }
// 0x6a19a0 (r3 a dialog, r4 the message): a dialog's base key handler.
void hook_menu_dialog_key(Ctx& ctx, void (*orig)(Ctx&)) { rt::game::dialog_key_hook(ctx, orig); }

}  // namespace ppu
