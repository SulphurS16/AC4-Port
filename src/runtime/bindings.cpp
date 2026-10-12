#include "runtime/bindings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "common/file.h"
#include "runtime/input.h"
#include "runtime/log.h"

namespace rt::bindings {
namespace {

// cellPad digital words.
constexpr u16 SELECT = 0x1, L3 = 0x2, R3 = 0x4, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80;
constexpr u16 L2 = 0x1, R2 = 0x2, L1 = 0x4, R1 = 0x8, TRIANGLE = 0x10, CIRCLE = 0x20, CROSS = 0x40, SQUARE = 0x80;

enum class Kind { D1, D2, Axis };
struct Control {
  const char* name;
  Kind kind;
  u16 bit;
  int axis;  // 0 lx, 1 ly, 2 rx, 3 ry
  bool positive;
};
const Control kControls[] = {
    {"select", Kind::D1, SELECT, 0, false},     {"l3", Kind::D1, L3, 0, false},
    {"r3", Kind::D1, R3, 0, false},             {"start", Kind::D1, START, 0, false},
    {"up", Kind::D1, UP, 0, false},             {"right", Kind::D1, RIGHT, 0, false},
    {"down", Kind::D1, DOWN, 0, false},         {"left", Kind::D1, LEFT, 0, false},
    {"l2", Kind::D2, L2, 0, false},             {"r2", Kind::D2, R2, 0, false},
    {"l1", Kind::D2, L1, 0, false},             {"r1", Kind::D2, R1, 0, false},
    {"triangle", Kind::D2, TRIANGLE, 0, false}, {"circle", Kind::D2, CIRCLE, 0, false},
    {"cross", Kind::D2, CROSS, 0, false},       {"square", Kind::D2, SQUARE, 0, false},
    {"lstick_up", Kind::Axis, 0, 1, false},     {"lstick_down", Kind::Axis, 0, 1, true},
    {"lstick_left", Kind::Axis, 0, 0, false},   {"lstick_right", Kind::Axis, 0, 0, true},
    {"rstick_up", Kind::Axis, 0, 3, false},     {"rstick_down", Kind::Axis, 0, 3, true},
    {"rstick_left", Kind::Axis, 0, 2, false},   {"rstick_right", Kind::Axis, 0, 2, true},
};
constexpr size_t kControlCount = sizeof(kControls) / sizeof(kControls[0]);

// Input sources: virtual-key codes 1-255, mouse buttons 0x100 + i, wheel up/down 0x200/0x201.
constexpr u32 kMouse = 0x100, kWheelUp = 0x200, kWheelDown = 0x201;
struct KeyName {
  const char* name;
  u32 code;
};
const KeyName kKeys[] = {
    {"Space", VK_SPACE},     {"Enter", VK_RETURN},    {"Escape", VK_ESCAPE},   {"Tab", VK_TAB},
    {"Backspace", VK_BACK},  {"Shift", VK_SHIFT},     {"Ctrl", VK_CONTROL},    {"Alt", VK_MENU},
    {"Up", VK_UP},           {"Down", VK_DOWN},       {"Left", VK_LEFT},       {"Right", VK_RIGHT},
    {"Home", VK_HOME},       {"End", VK_END},         {"PageUp", VK_PRIOR},    {"PageDown", VK_NEXT},
    {"Insert", VK_INSERT},   {"Delete", VK_DELETE},   {"CapsLock", VK_CAPITAL}, {"Mouse1", kMouse + 0},
    {"Mouse2", kMouse + 1},  {"Mouse3", kMouse + 2},  {"Mouse4", kMouse + 3},  {"Mouse5", kMouse + 4},
    {"WheelUp", kWheelUp},   {"WheelDown", kWheelDown},
};

const char* const kDefault = R"(# ARMORED CORE for Answer (native port): keyboard and mouse bindings for pad port 0.
# Each line binds a pad control to one or more inputs: <control> = <input>[, <input>...]
# Controls: cross circle square triangle l1 l2 l3 r1 r2 r3 start select up down left right
#           lstick_up lstick_down lstick_left lstick_right rstick_up rstick_down rstick_left rstick_right
# Inputs: A-Z, 0-9, F1-F24, Num0-Num9, Space, Enter, Escape, Tab, Backspace, Shift, Ctrl, Alt, Up, Down,
#         Left, Right, Home, End, PageUp, PageDown, Insert, Delete, CapsLock, Mouse1 (left), Mouse2
#         (right), Mouse3 (middle), Mouse4, Mouse5, WheelUp, WheelDown
# Mouse buttons and the wheel count only while the window has captured the mouse (click into it;
# F1 or switching away releases it). The default follows the game's SIMPLE key layout.

# Movement (left stick) and view (right stick; the mouse is the main way to look)
lstick_up = W
lstick_down = S
lstick_left = A
lstick_right = D
rstick_up = Num8
rstick_down = Num5, Num2
rstick_left = Num4
rstick_right = Num6

# Weapons: right arm, left arm, shoulder; switch right / left arm weapon
cross = Mouse1, Enter
square = Mouse2
r1 = F, Mouse3
circle = R, Backspace
triangle = Q

# Boosts: boost (hold to rise), quick boost, overed boost
l2 = Space
r2 = Shift
l1 = E

# View reset, pause, menus
r3 = C
l3 = Z
start = Escape
select = Tab
up = Up
down = Down
left = Left
right = Right

[mouse]
# Rotation per raw mouse count, relative to the default (1.0). No acceleration or smoothing.
sensitivity = 1.0
invert_y = false
)";

std::vector<u32> g_bind[kControlCount];
MouseSettings g_mouse;
std::chrono::steady_clock::time_point g_wheel_up_until, g_wheel_down_until;

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(u8(s.back()))) s.pop_back();
  size_t b = 0;
  while (b < s.size() && std::isspace(u8(s[b]))) ++b;
  return s.substr(b);
}

bool equal_ci(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(u8(a[i])) != std::tolower(u8(b[i]))) return false;
  return true;
}

}  // namespace

u32 key_code(const std::string& n) {
  for (const KeyName& k : kKeys)
    if (equal_ci(n, k.name)) return k.code;
  if (n.size() == 1 && std::isalnum(u8(n[0]))) return u32(std::toupper(u8(n[0])));  // 'A'-'Z', '0'-'9'
  if (n.size() >= 2 && (n[0] == 'F' || n[0] == 'f') && std::isdigit(u8(n[1]))) {
    int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 24) return u32(VK_F1 + f - 1);
  }
  if (n.size() == 4 && equal_ci(n.substr(0, 3), "Num") && std::isdigit(u8(n[3]))) return u32(VK_NUMPAD0 + (n[3] - '0'));
  return 0;
}

namespace {

void parse(const std::string& text, const std::string& source) {
  for (auto& b : g_bind) b.clear();
  std::istringstream in(text);
  std::string line, section;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      LOG_W("input", "%s:%d: expected 'name = value'", source.c_str(), line_no);
      continue;
    }
    std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (section == "mouse") {
      if (equal_ci(name, "sensitivity")) g_mouse.sensitivity = std::atof(value.c_str());
      else if (equal_ci(name, "invert_y")) g_mouse.invert_y = equal_ci(value, "true") || value == "1";
      else LOG_W("input", "%s:%d: unknown mouse setting '%s'", source.c_str(), line_no, name.c_str());
      continue;
    }
    size_t c = 0;
    while (c < kControlCount && !equal_ci(name, kControls[c].name)) ++c;
    if (c == kControlCount) {
      LOG_W("input", "%s:%d: unknown pad control '%s'", source.c_str(), line_no, name.c_str());
      continue;
    }
    std::istringstream items(value);
    for (std::string item; std::getline(items, item, ',');) {
      item = trim(item);
      if (item.empty()) continue;
      if (u32 code = key_code(item)) g_bind[c].push_back(code);
      else LOG_W("input", "%s:%d: unknown input '%s'", source.c_str(), line_no, item.c_str());
    }
  }
}

}  // namespace

void load(const std::string& path) {
  std::string text;
  if (std::ifstream f{std::filesystem::path(path), std::ios::binary}) {
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    LOG_I("input", "keyboard/mouse bindings from %s", path.c_str());
  } else {
    text = kDefault;
    try {
      std::filesystem::create_directories(std::filesystem::path(path).parent_path());
      ac4::guard_write(path);
      std::ofstream(std::filesystem::path(path), std::ios::binary) << kDefault;
      LOG_I("input", "wrote the default keyboard/mouse bindings to %s", path.c_str());
    } catch (const std::exception& e) {
      LOG_W("input", "default bindings not written (%s); using them in memory", e.what());
    }
  }
  parse(text, path);
  LOG_I("input", "mouse sensitivity %.3f%s", g_mouse.sensitivity, g_mouse.invert_y ? ", Y inverted" : "");
}

PadOut sample() {
  PadOut o;
  if (!input_has_focus()) return o;
  auto now = std::chrono::steady_clock::now();
  if (s32 w = input_mouse_wheel_take()) {  // one notch: a press of 0.1 s
    (w > 0 ? g_wheel_up_until : g_wheel_down_until) = now + std::chrono::milliseconds(100);
  }
  auto down = [&](u32 code) {
    if (code < kMouse) return input_key_down(code);
    if (code < kWheelUp) return input_mouse_button(int(code - kMouse));
    return now < (code == kWheelUp ? g_wheel_up_until : g_wheel_down_until);
  };
  bool neg[4] = {}, pos[4] = {};
  for (size_t c = 0; c < kControlCount; ++c) {
    if (!std::any_of(g_bind[c].begin(), g_bind[c].end(), down)) continue;
    const Control& k = kControls[c];
    if (k.kind == Kind::D1) o.d1 |= k.bit;
    else if (k.kind == Kind::D2) o.d2 |= k.bit;
    else (k.positive ? pos : neg)[k.axis] = true;
  }
  int* axes[4] = {&o.lx, &o.ly, &o.rx, &o.ry};
  for (int a = 0; a < 4; ++a)
    if (neg[a] || pos[a]) *axes[a] = neg[a] && pos[a] ? 128 : neg[a] ? 0 : 255;
  return o;
}

const MouseSettings& mouse() { return g_mouse; }

}  // namespace rt::bindings
