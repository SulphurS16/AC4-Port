// HLE: sys_io (cellPad, cellKb, cellMouse).
// Pad port 0 combines the first XInput controller with a keyboard fallback; ports 1-3 map to
// further XInput controllers when connected. Keyboard/mouse libraries report no device (the game
// is played with the pad; host keyboard input feeds the pad emulation instead).
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

#include <windows.h>
#include <xinput.h>

#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/lv2/lv2.h"
#include "runtime/bindings.h"
#include "runtime/datawatch.h"
#include "runtime/game/game_input.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/gpu/present.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/memory.h"
#include "runtime/memscan.h"
#include "runtime/rsx/rsx.h"
#include "runtime/sysdialog.h"

namespace rt::gpu {
extern std::atomic<int> g_capture_requests;
}  // namespace rt::gpu

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

// ---- scripted input (--input), for unattended runs ----
// "t:btn[+btn][:hold]" presses at t seconds; "@Thread Name+t:..." presses t seconds after the next
// start of that guest thread (games run each scene in a thread of its own) once all earlier
// presses have happened, so a script follows the game instead of a fixed timeline; "+t:..."
// presses t seconds after the previous press. The pseudo-button "shot" captures a screenshot when
// its press starts; "mx=N" and "my=N" move the mouse by N counts, evenly over the hold time.
//
// Live control (--control <file>): lines appended to the file while the game runs are added to the
// script (same syntax; "+t:..." items chain on the last press, so a new line runs right away), and
// a line "quit" ends the run. With the game paused in its own pause menu between lines, a mission
// can be played step by step from screenshots.
struct ScriptedPress {
  double t = 0, hold = 0.15;
  u16 d1 = 0, d2 = 0;
  int lx = -1, ly = -1, rx = -1, ry = -1;
  s64 mx = 0, my = 0;           // scripted mouse movement (counts) over the hold time
  s64 mx_done = 0, my_done = 0;  // ... already delivered
  bool shot = false;
  bool logged = false;
  double start = -1;  // first pad read at or after t: the press is held from there
  std::string anchor;  // thread name; t is resolved from its start
  double offset = 0;
};
std::vector<ScriptedPress> g_script;
std::mutex g_script_mu;  // live control appends while the game reads the pad
bool g_script_parsed = false;
std::atomic<bool> g_quit{false};
std::mutex g_starts_mu;
std::vector<std::pair<std::string, double>> g_starts;  // guest thread starts (name, seconds)

void parse_items(const std::string& s) {
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    size_t c1 = item.find(':');
    if (c1 == std::string::npos) continue;
    ScriptedPress p;
    p.t = std::atof(item.substr(0, c1).c_str());
    if (item[0] == '+') {  // relative to the previous press
      p.offset = std::atof(item.c_str() + 1);
      p.t = -2;
    } else if (item[0] == '@') {
      std::string when = item.substr(1, c1 - 1);
      size_t plus = when.rfind('+');
      p.anchor = when.substr(0, plus);
      p.offset = plus == std::string::npos ? 0 : std::atof(when.c_str() + plus + 1);
      p.t = -1;
    }
    std::string rest = item.substr(c1 + 1);
    size_t c2 = rest.find(':');
    if (c2 != std::string::npos) {
      p.hold = std::atof(rest.substr(c2 + 1).c_str());
      rest = rest.substr(0, c2);
    }
    size_t b = 0;
    while (b <= rest.size()) {
      size_t e = rest.find('+', b);
      std::string n = rest.substr(b, e == std::string::npos ? std::string::npos : e - b);
      b = e == std::string::npos ? rest.size() + 1 : e + 1;
      if (n == "select") p.d1 |= SELECT;
      else if (n == "l3") p.d1 |= L3;
      else if (n == "r3") p.d1 |= R3;
      else if (n == "start") p.d1 |= START;
      else if (n == "up") p.d1 |= UP;
      else if (n == "right") p.d1 |= RIGHT;
      else if (n == "down") p.d1 |= DOWN;
      else if (n == "left") p.d1 |= LEFT;
      else if (n == "l2") p.d2 |= L2;
      else if (n == "r2") p.d2 |= R2;
      else if (n == "l1") p.d2 |= L1;
      else if (n == "r1") p.d2 |= R1;
      else if (n == "triangle") p.d2 |= TRIANGLE;
      else if (n == "circle") p.d2 |= CIRCLE;
      else if (n == "cross") p.d2 |= CROSS;
      else if (n == "square") p.d2 |= SQUARE;
      else if (n == "lup") p.ly = 0;
      else if (n == "ldown") p.ly = 255;
      else if (n == "lleft") p.lx = 0;
      else if (n == "lright") p.lx = 255;
      else if (n == "rup") p.ry = 0;
      else if (n == "rdown") p.ry = 255;
      else if (n == "rleft") p.rx = 0;
      else if (n == "rright") p.rx = 255;
      else if (n.size() > 3 && n[2] == '=' && (n[0] == 'l' || n[0] == 'r') && (n[1] == 'x' || n[1] == 'y')) {
        int v = std::clamp(std::atoi(n.c_str() + 3), 0, 255);  // "rx=N": a stick axis at N (0-255)
        (n[0] == 'l' ? (n[1] == 'x' ? p.lx : p.ly) : (n[1] == 'x' ? p.rx : p.ry)) = v;
      }
      else if (n == "shot") p.shot = true;
      else if (n.rfind("mx=", 0) == 0) p.mx = std::atoll(n.c_str() + 3);
      else if (n.rfind("my=", 0) == 0) p.my = std::atoll(n.c_str() + 3);
      else if (!n.empty()) LOG_W("pad", "--input: unknown button '%s'", n.c_str());
    }
    g_script.push_back(p);
  }
}

void control_thread(std::string path) {
  // The file is truncated first: commands from an earlier run never replay.
  if (FILE* f = std::fopen(path.c_str(), "wb")) std::fclose(f);
  LOG_I("pad", "--control: reading commands from %s", path.c_str());
  long offset = 0;
  std::string partial;
  for (;;) {
    Sleep(100);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) continue;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size < offset) offset = 0;  // replaced by a shorter file
    if (size > offset) {
      std::string chunk(size_t(size - offset), '\0');
      std::fseek(f, offset, SEEK_SET);
      chunk.resize(std::fread(chunk.data(), 1, chunk.size(), f));
      offset += long(chunk.size());
      partial += chunk;
    }
    std::fclose(f);
    for (size_t nl; (nl = partial.find('\n')) != std::string::npos;) {
      std::string line = partial.substr(0, nl);
      partial.erase(0, nl + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty()) continue;
      LOG_I("pad", "control: %s", line.c_str());
      if (line == "quit") {
        g_quit = true;
        continue;
      }
      if (line.rfind("scan ", 0) == 0) {  // memory search (memscan.h)
        if (!memscan::command(line.substr(5))) LOG_W("pad", "control: unknown scan command '%s'", line.c_str());
        continue;
      }
      if (line.rfind("watch ", 0) == 0) {  // hardware watchpoint (datawatch.h)
        datawatch::command(line.substr(6));
        continue;
      }
      if (line.rfind("fps ", 0) == 0) {  // "fps N|console|display|unlimited": frame limit (config.h)
        if (!parse_fps_limit(line.substr(4), &cfg().fps_limit)) LOG_W("pad", "control: '%s': not a frame limit", line.c_str());
        continue;
      }
      if (line == "vsync on" || line == "vsync off") {
        cfg().vsync = line == "vsync on";
        continue;
      }
      if (line == "fullscreen on" || line == "fullscreen off") {  // tests: without taking the focus
        gpu::window_set_fullscreen(line == "fullscreen on", false);
        continue;
      }
      if (line == "shot output") {  // the window's output as presented (present.h)
        gpu::request_output_capture();
        continue;
      }
      if (line == "window minimize" || line == "window restore") {
        gpu::window_set_minimized(line == "window minimize");
        continue;
      }
      if (line.rfind("window ", 0) == 0) {  // "window WxH": client size (tests)
        int w = 0, h = 0;
        if (std::sscanf(line.c_str() + 7, "%dx%d", &w, &h) == 2 && w >= 160 && h >= 90) gpu::window_set_client_size(w, h);
        else LOG_W("pad", "control: '%s' is not 'window WxH'", line.c_str());
        continue;
      }
      if (line.rfind("dialog ", 0) == 0) {  // answers for system dialogs (sysdialog.h)
        sysdlg::queue_answers(line.substr(7));
        continue;
      }
      if (line.rfind("turntrace ", 0) == 0) {  // game_input.h
        game::turn_trace(std::atoi(line.c_str() + 10));
        continue;
      }
      if (line == "axes") {  // the game's input axes (game_input.h)
        game::axes_dump();
        continue;
      }
      if (line.rfind("axis ", 0) == 0) {
        if (!game::axis_command(line.substr(5))) LOG_W("pad", "control: '%s' is not 'axis INPUT VALUE|off'", line.c_str());
        continue;
      }
      if (line.rfind("peek ", 0) == 0) {  // "peek ADDR [N]": N guest floats (and words) at ADDR, live
        std::istringstream in(line.substr(5));
        std::string a;
        u32 n = 1;
        in >> a >> n;
        u32 addr = u32(std::strtoul(a.c_str(), nullptr, 0)) & ~3u;
        n = std::clamp<u32>(n, 1, 64);
        if (a.empty() || !vm::committed(addr, 4 * n)) {
          LOG_W("pad", "control: peek '%s': not mapped guest memory", a.c_str());
          continue;
        }
        std::string s;
        for (u32 i = 0; i < n; ++i) {
          u32 w = ppu::r32(addr + 4 * i);
          float f;
          std::memcpy(&f, &w, 4);
          s += ac4::strprintf(" %08x(%g)", w, double(f));
        }
        LOG_I("pad", "peek 0x%08x:%s", addr, s.c_str());
        continue;
      }
      if (line.rfind("gpu ", 0) == 0) {  // "gpu NAME VALUE": renderer switches for A/B measurements
        std::istringstream in(line.substr(4));
        std::string name, value;
        in >> name >> value;
        if (!rsx::gpu_option(name, value)) LOG_W("pad", "control: unknown renderer option '%s'", line.c_str());
        continue;
      }
      if (line.rfind("key ", 0) == 0) {  // "key NAME down|up": a host key or mouse button, for testing bindings
        std::string rest = line.substr(4);
        size_t sp = rest.rfind(' ');
        u32 code = sp == std::string::npos ? 0 : bindings::key_code(rest.substr(0, sp));
        bool down = sp != std::string::npos && rest.substr(sp + 1) == "down";
        if (code && code < 0x100) input_key_inject(code, down);
        else if (code >= 0x100 && code < 0x105) input_mouse_button_inject(int(code - 0x100), down);  // Mouse1-Mouse5
        else LOG_W("pad", "control: '%s' is not 'key <keyboard key or Mouse1-5> down|up'", line.c_str());
        continue;
      }
      std::lock_guard lk(g_script_mu);
      parse_items(line);
    }
  }
}

void parse_script() {
  g_script_parsed = true;
  {
    std::lock_guard lk(g_script_mu);
    parse_items(cfg().input_script);
    if (!g_script.empty()) LOG_I("pad", "--input: %zu scripted press(es) on port 0", g_script.size());
  }
  if (!cfg().control_file.empty()) std::thread(control_thread, cfg().control_file).detach();
}

void apply_script(PadState& s) {
  if (!g_script_parsed) parse_script();
  std::lock_guard script_lock(g_script_mu);
  if (g_script.empty()) return;
  s.connected = true;
  double now = double(lv2::system_time_us()) / 1e6;
  double prev_start = 0;
  bool prev_done = true;
  for (auto& p : g_script) {
    if (p.t < 0 && prev_done) {  // anchored: the first start of the thread after the previous press
      std::lock_guard lk(g_starts_mu);
      for (const auto& [name, at] : g_starts)
        if (name == p.anchor && at >= prev_start) {
          p.t = at + p.offset;
          LOG_I("pad", "--input: '%s' started at %.2f s; press due at %.2f s", p.anchor.c_str(), at, p.t);
          break;
        }
    }
    if (p.t == -2 && prev_done) p.t = prev_start + p.offset;
    prev_done = p.start >= 0;
    if (p.start >= 0) prev_start = p.start;
    // A press starts at the first read after its time, so a game that is not polling at that
    // moment still sees it.
    if (p.t < 0 || now < p.t) continue;
    if (p.start < 0) {
      p.start = now;
      if (p.shot) ++gpu::g_capture_requests;
    }
    if (p.mx || p.my) {  // the share of the movement due by now
      double frac = p.hold > 0 ? std::min(1.0, (now - p.start) / p.hold) : 1.0;
      s64 x = std::llround(double(p.mx) * frac), y = std::llround(double(p.my) * frac);
      input_mouse_inject(x - p.mx_done, y - p.my_done);
      p.mx_done = x;
      p.my_done = y;
    }
    if (now >= p.start + p.hold) continue;
    if (!p.logged) {
      p.logged = true;
      LOG_I("pad", "scripted press at %.2f s, read at %.2f s (d1 0x%x d2 0x%x)", p.t, now, p.d1, p.d2);
    }
    s.d1 |= p.d1;
    s.d2 |= p.d2;
    if (p.lx >= 0) s.lx = u8(p.lx);
    if (p.ly >= 0) s.ly = u8(p.ly);
    if (p.rx >= 0) s.rx = u8(p.rx);
    if (p.ry >= 0) s.ry = u8(p.ry);
  }
}

PadState read_port(u32 port) {
  PadState s;
  XINPUT_STATE xs{};
  bool xin = g_xget && !input_isolated() && g_xget(port, &xs) == ERROR_SUCCESS;
  if (xin) {
    s.connected = true;
    const auto& g = xs.Gamepad;
    WORD b = g.wButtons;
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
  }
  if (port == 0) {
    // Keyboard and mouse bindings (bindings.h; always available on port 0).
    s.connected = true;
    bindings::PadOut k = bindings::sample();
    s.d1 |= k.d1;
    s.d2 |= k.d2;
    if (k.lx >= 0) s.lx = u8(k.lx);
    if (k.ly >= 0) s.ly = u8(k.ly);
    if (k.rx >= 0) s.rx = u8(k.rx);
    if (k.ry >= 0) s.ry = u8(k.ry);
  }
  if (port == 0) apply_script(s);
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
  if (sysdlg::active()) {  // a system dialog holds the controller: the game sees it at rest
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

bool input_quit_requested() { return g_quit; }

void input_thread_started(const std::string& name) {
  std::lock_guard lk(g_starts_mu);
  g_starts.push_back({name, double(lv2::system_time_us()) / 1e6});
}

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
