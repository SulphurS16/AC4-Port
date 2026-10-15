// HLE: sys_io (cellPad, cellKb, cellMouse).
// Pad port 0 combines the first XInput controller with a keyboard fallback; ports 1-3 map to
// further XInput controllers when connected. Keyboard/mouse libraries report no device (the game
// is played with the pad; host keyboard input feeds the pad emulation instead).
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

#include <windows.h>
#include <xinput.h>

#include "common/file.h"
#include "ppu/ppu_recomp.h"
#include "common/settings_file.h"
#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/lv2/lv2.h"
#include "runtime/bindings.h"
#include "runtime/game/menu_mouse.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/memory.h"
#include "runtime/rsx/rsx.h"
#include "runtime/sysdialog.h"
#include "runtime/ui/overlay.h"

namespace rt {
namespace {

constexpr s32 PAD_ERR_INVALID_PARAMETER = s32(0x80121102), PAD_ERR_ALREADY_INITIALIZED = s32(0x80121103),
              PAD_ERR_UNINITIALIZED = s32(0x80121104), PAD_ERR_NO_DEVICE = s32(0x80121107);
constexpr u32 kMaxPads = 127, kPorts = 4;

// Digital1
constexpr u16 SELECT = 0x1, L3 = 0x2, R3 = 0x4, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80;
// Digital2
constexpr u16 L2 = 0x1, R2 = 0x2, L1 = 0x4, R1 = 0x8, TRIANGLE = 0x10, CIRCLE = 0x20, CROSS = 0x40, SQUARE = 0x80;

struct PadState {
  u16 d1 = 0, d2 = 0;
  u8 rx = 128, ry = 128, lx = 128, ly = 128;
  u8 press[12] = {};  // right left up down tri circle cross square l1 r1 l2 r2
  bool connected = false;
};

// XInput is loaded from the OS at runtime (xinput1_4 ships with Windows 8+; 9_1_0 as fallback).
using XGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XSetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XGetState g_xget = nullptr;
XSetState g_xset = nullptr;
void load_xinput() {
  static bool done = false;
  if (done) return;
  done = true;
  HMODULE h = LoadLibraryW(L"xinput1_4.dll");
  if (!h) h = LoadLibraryW(L"xinput9_1_0.dll");
  if (!h) {
    LOG_W("pad", "XInput not available; keyboard input only");
    return;
  }
  g_xget = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(h, "XInputGetState")));
  g_xset = reinterpret_cast<XSetState>(reinterpret_cast<void*>(GetProcAddress(h, "XInputSetState")));
}

std::mutex g_mu;
bool g_init = false;
u32 g_max = 0;
u32 g_press_mode[kPorts] = {}, g_sensor_mode[kPorts] = {};
u16 g_last[kPorts][24];
bool g_have_last[kPorts] = {};

u8 axis(SHORT v) { return u8(std::clamp((int(v) + 32768) >> 8, 0, 255)); }

PadState read_port(u32 port) {
  PadState s;
  XINPUT_STATE xs{};
  bool xin = g_xget && !input_isolated() && g_xget(port, &xs) == ERROR_SUCCESS;
  if (xin) {
    s.connected = true;
    const auto& g = xs.Gamepad;
    WORD b = ui::pad_suppressed(port, g.wButtons);  // buttons still held from closing PC SETTINGS stay there
    if (b & XINPUT_GAMEPAD_BACK) s.d1 |= SELECT;
    if (b & XINPUT_GAMEPAD_LEFT_THUMB) s.d1 |= L3;
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB) s.d1 |= R3;
    if (b & XINPUT_GAMEPAD_START) s.d1 |= START;
    if (b & XINPUT_GAMEPAD_DPAD_UP) s.d1 |= UP;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) s.d1 |= RIGHT;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) s.d1 |= DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) s.d1 |= LEFT;
    if (g.bLeftTrigger > 30) s.d2 |= L2;
    if (g.bRightTrigger > 30) s.d2 |= R2;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) s.d2 |= L1;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) s.d2 |= R1;
    if (b & XINPUT_GAMEPAD_Y) s.d2 |= TRIANGLE;
    if (b & XINPUT_GAMEPAD_B) s.d2 |= CIRCLE;
    if (b & XINPUT_GAMEPAD_A) s.d2 |= CROSS;
    if (b & XINPUT_GAMEPAD_X) s.d2 |= SQUARE;
    s.lx = axis(g.sThumbLX);
    s.ly = u8(255 - axis(g.sThumbLY));  // PS3 Y axis points down
    s.rx = axis(g.sThumbRX);
    s.ry = u8(255 - axis(g.sThumbRY));
    s.press[10] = g.bLeftTrigger;
    s.press[11] = g.bRightTrigger;
    // A controller in use hides the menu cursor (a stick past a third of its travel, any button).
    auto away = [](SHORT v) { return std::abs(int(v)) > 11000; };
    if (b || g.bLeftTrigger > 30 || g.bRightTrigger > 30 || away(g.sThumbLX) || away(g.sThumbLY) || away(g.sThumbRX) ||
        away(g.sThumbRY))
      game::note_pad_input();
  }
  if (port == 0) {
    // Keyboard and mouse bindings (bindings.h; always available on port 0).
    s.connected = true;
    bindings::PadOut k = bindings::sample();
    s.d1 |= k.d1;
    s.d2 |= k.d2;
    game::menu_pad_bits(&s.d1, &s.d2);  // clicks, right clicks and the wheel in the game's menus
    if (k.lx >= 0) s.lx = u8(k.lx);
    if (k.ly >= 0) s.ly = u8(k.ly);
    if (k.rx >= 0) s.rx = u8(k.rx);
    if (k.ry >= 0) s.ry = u8(k.ry);
  }
  auto pr = [](bool on, u8 cur) { return on ? std::max<u8>(cur, 255) : cur; };
  s.press[0] = pr(s.d1 & RIGHT, s.press[0]);
  s.press[1] = pr(s.d1 & LEFT, s.press[1]);
  s.press[2] = pr(s.d1 & UP, s.press[2]);
  s.press[3] = pr(s.d1 & DOWN, s.press[3]);
  s.press[4] = pr(s.d2 & TRIANGLE, s.press[4]);
  s.press[5] = pr(s.d2 & CIRCLE, s.press[5]);
  s.press[6] = pr(s.d2 & CROSS, s.press[6]);
  s.press[7] = pr(s.d2 & SQUARE, s.press[7]);
  s.press[8] = pr(s.d2 & L1, s.press[8]);
  s.press[9] = pr(s.d2 & R1, s.press[9]);
  if (!(s.d2 & L2)) s.press[10] = std::min<u8>(s.press[10], 30);
  if ((s.d2 & L2) && !s.press[10]) s.press[10] = 255;
  if ((s.d2 & R2) && !s.press[11]) s.press[11] = 255;
  return s;
}

s32 cellPadInit(u32 max) {
  std::lock_guard lk(g_mu);
  if (g_init) return PAD_ERR_ALREADY_INITIALIZED;
  if (max == 0 || max > kMaxPads) return PAD_ERR_INVALID_PARAMETER;
  load_xinput();
  g_init = true;
  g_max = std::min(max, kPorts);
  std::fill(std::begin(g_have_last), std::end(g_have_last), false);
  LOG_I("pad", "cellPadInit(%u)", max);
  return CELL_OK;
}
s32 cellPadEnd() {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  g_init = false;
  return CELL_OK;
}
s32 cellPadClearBuf(u32 port) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= g_max) return PAD_ERR_INVALID_PARAMETER;
  g_have_last[port] = false;
  return CELL_OK;
}
struct PadData {
  bes32 len;
  be16 button[64];
};
s32 cellPadGetData(u32 port, Ptr<PadData> data) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= g_max || !data) return PAD_ERR_INVALID_PARAMETER;
  PadState s = read_port(port);
  if (!s.connected) return PAD_ERR_NO_DEVICE;
  ui::PadMode ui_mode = ui::pad_mode();
  if (sysdlg::active() || ui_mode != ui::PadMode::Normal) {  // a system dialog or PC SETTINGS holds the controller: the game sees it at rest
    PadState idle;
    idle.connected = true;
    s = idle;
  }
  u16 b[24] = {};
  b[2] = s.d1;
  b[3] = s.d2;
  b[4] = s.rx;
  b[5] = s.ry;
  b[6] = s.lx;
  b[7] = s.ly;
  for (int i = 0; i < 12; ++i) b[8 + i] = s.press[i];
  b[20] = b[21] = b[22] = 512;  // sensors at rest (X/Y/Z centred, 1 g)
  b[23] = 512;
  s32 len = g_sensor_mode[port] ? 24 : (g_press_mode[port] ? 20 : 8);
  if (g_have_last[port] && std::memcmp(g_last[port], b, sizeof b) == 0) {
    data->len = 0;  // CELL_PAD_LEN_NO_CHANGE
    return CELL_OK;
  }
  std::memcpy(g_last[port], b, sizeof b);
  g_have_last[port] = true;
  data->len = len;
  b[0] = 0;
  b[1] = u16((0x7 << 4) | std::min(len / 2, 15));
  for (int i = 0; i < 64; ++i) data->button[i] = i < len ? b[i] : 0;
  return CELL_OK;
}
struct PadInfo {
  be32 max_connect, now_connect, system_info;
  be16 vendor_id[kMaxPads], product_id[kMaxPads];
  u8 status[kMaxPads];
};
s32 cellPadGetInfo(Ptr<PadInfo> info) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  std::memset(info.get(), 0, sizeof(PadInfo));
  info->max_connect = g_max;
  u32 now = 0;
  for (u32 p = 0; p < g_max; ++p) {
    bool c = read_port(p).connected;
    if (c) ++now;
    info->vendor_id[p] = 0x054c;   // Sony
    info->product_id[p] = 0x0268;  // DUALSHOCK 3 (reported to the game)
    info->status[p] = c ? 1 : 0;
  }
  info->now_connect = now;
  info->system_info = sysdlg::active() ? 1 : 0;  // CELL_PAD_INFO_INTERCEPTED
  return CELL_OK;
}
s32 cellPadGetCapabilityInfo(u32 port, Ptr<be32> info) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= g_max) return PAD_ERR_INVALID_PARAMETER;
  if (!read_port(port).connected) return PAD_ERR_NO_DEVICE;
  info[0] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;  // PS3 conformity, press, sensor, analog stick, actuator
  for (int i = 1; i < 32; ++i) info[i] = 0;
  return CELL_OK;
}
s32 cellPadSetPressMode(u32 port, u32 mode) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= g_max || mode > 1) return PAD_ERR_INVALID_PARAMETER;
  g_press_mode[port] = mode;
  return CELL_OK;
}
s32 cellPadSetSensorMode(u32 port, u32 mode) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= g_max || mode > 1) return PAD_ERR_INVALID_PARAMETER;
  g_sensor_mode[port] = mode;
  return CELL_OK;
}
// Whether the pad on a port supports pressure-sensitive buttons / motion sensors: 1 or 0, or an
// error (every connected pad here reports both, as a DUALSHOCK 3 does).
s32 cellPadInfoPressMode(u32 port) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= kMaxPads) return PAD_ERR_INVALID_PARAMETER;
  if (port >= g_max || !read_port(port).connected) return PAD_ERR_NO_DEVICE;
  return 1;
}
s32 cellPadInfoSensorMode(u32 port) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= kMaxPads) return PAD_ERR_INVALID_PARAMETER;
  if (port >= g_max || !read_port(port).connected) return PAD_ERR_NO_DEVICE;
  return 1;
}
struct ActParam {
  u8 motor[2], reserved[6];
};
s32 cellPadSetActDirect(u32 port, Ptr<ActParam> p) {
  std::lock_guard lk(g_mu);
  if (!g_init) return PAD_ERR_UNINITIALIZED;
  if (port >= g_max || !p) return PAD_ERR_INVALID_PARAMETER;
  XINPUT_VIBRATION v{};
  v.wRightMotorSpeed = p->motor[0] ? 0xC000 : 0;  // small motor: on/off
  v.wLeftMotorSpeed = WORD(p->motor[1]) * 257;     // large motor: 0-255
  if (g_xset) g_xset(port, &v);
  return CELL_OK;
}

// ---- keyboard / mouse: no devices ----
constexpr s32 KB_ERR_NO_DEVICE = s32(0x80121007), MOUSE_ERR_NO_DEVICE = s32(0x80121207);
s32 cellKbInit(u32 max) { return CELL_OK; }
s32 cellKbEnd() { return CELL_OK; }
s32 cellKbClearBuf(u32 port) { return KB_ERR_NO_DEVICE; }
s32 cellKbGetInfo(u32 info) {
  std::memset(ppu::mem() + info, 0, 12 + 127);
  ppu::w32(info, 1);
  return CELL_OK;
}
s32 cellKbRead(u32 port, u32 data) { return KB_ERR_NO_DEVICE; }
s32 cellKbSetCodeType(u32 port, u32 type) { return CELL_OK; }
s32 cellKbSetReadMode(u32 port, u32 mode) { return CELL_OK; }
s32 cellMouseInit(u32 max) { return CELL_OK; }
s32 cellMouseEnd() { return CELL_OK; }
s32 cellMouseGetInfo(u32 info) {
  std::memset(ppu::mem() + info, 0, 12 + 127 * 5);
  ppu::w32(info, 1);
  return CELL_OK;
}
s32 cellMouseGetData(u32 port, u32 data) { return MOUSE_ERR_NO_DEVICE; }

}  // namespace

bool input_quit_requested() { return false; }
void input_thread_started(const std::string&) {}

static hle::Module s_module("sys_io", {
    HLE_FN(cellKbClearBuf),
    HLE_FN(cellKbEnd),
    HLE_FN(cellKbGetInfo),
    HLE_FN(cellKbInit),
    HLE_FN(cellKbRead),
    HLE_FN(cellKbSetCodeType),
    HLE_FN(cellKbSetReadMode),
    HLE_FN(cellMouseEnd),
    HLE_FN(cellMouseGetData),
    HLE_FN(cellMouseGetInfo),
    HLE_FN(cellMouseInit),
    HLE_FN(cellPadClearBuf),
    HLE_FN(cellPadEnd),
    HLE_FN(cellPadGetCapabilityInfo),
    HLE_FN(cellPadGetData),
    HLE_FN(cellPadGetInfo),
    HLE_FN(cellPadInfoPressMode),
    HLE_FN(cellPadInfoSensorMode),
    HLE_FN(cellPadInit),
    HLE_FN(cellPadSetActDirect),
    HLE_FN(cellPadSetPressMode),
    HLE_FN(cellPadSetSensorMode),
});

}  // namespace rt
